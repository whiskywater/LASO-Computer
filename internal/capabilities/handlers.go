package capabilities

import (
	"bytes"
	"context"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"image"
	"image/jpeg"
	"io"
	"net/url"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"time"

	"github.com/Registered-Agent-Attorney/LASO-Computer/internal/config"
	"github.com/Registered-Agent-Attorney/LASO-Computer/internal/platform"
)

type handlers struct {
	desktop    platform.Desktop
	cfg        config.Config
	focusState *focusState
}
type focusState struct {
	focusMu     sync.Mutex
	focusLeases map[string]focusLease
	nextToken   uint64
}
type focusLease struct {
	windowID     string
	sessionScope string
	agentScope   string
	jobID        string // the worker job that established this short session continuation
	token        uint64
	expiresAt    time.Time
}

const focusLeaseTTL = 2 * time.Minute

type principalContextKey struct{}

func principalScope(ctx context.Context) string {
	p, _ := ctx.Value(principalContextKey{}).(Principal)
	if p.SessionID == "" || p.JobID == "" {
		return ""
	}
	return p.AgentID + "\x00" + p.SessionID + "\x00" + p.JobID
}
func principalSessionScope(ctx context.Context) string {
	p, _ := ctx.Value(principalContextKey{}).(Principal)
	if p.AgentID == "" || p.SessionID == "" || p.JobID == "" {
		return ""
	}
	return p.AgentID + "\x00" + p.SessionID
}
func principalAgentScope(ctx context.Context) string {
	p, _ := ctx.Value(principalContextKey{}).(Principal)
	if p.AgentID == "" {
		return ""
	}
	return p.AgentID
}
func (h handlers) authorizeWindow(ctx context.Context, id string) error {
	scope := principalSessionScope(ctx)
	if scope == "" {
		return ErrFailed
	}
	if h.focusState == nil {
		return ErrFailed
	}
	h.focusState.focusMu.Lock()
	if err := ctx.Err(); err != nil {
		h.focusState.focusMu.Unlock()
		return err
	}
	if h.focusState.focusLeases == nil {
		h.focusState.focusLeases = make(map[string]focusLease)
	}
	h.focusState.nextToken++
	token := h.focusState.nextToken
	p, _ := ctx.Value(principalContextKey{}).(Principal)
	h.focusState.focusLeases[scope] = focusLease{windowID: id, sessionScope: scope, agentScope: principalAgentScope(ctx), jobID: p.JobID, token: token, expiresAt: time.Now().Add(focusLeaseTTL)}
	h.focusState.focusMu.Unlock()
	context.AfterFunc(ctx, func() { h.clearLease(scope, token) })
	return nil
}
func (h handlers) authorizedWindow(ctx context.Context) (string, bool) {
	scope := principalSessionScope(ctx)
	if scope == "" || ctx.Err() != nil {
		h.clearLeaseForContext(ctx)
		return "", false
	}
	if h.focusState == nil {
		return "", false
	}
	h.focusState.focusMu.Lock()
	defer h.focusState.focusMu.Unlock()
	lease, ok := h.focusState.focusLeases[scope]
	if ok && (time.Now().After(lease.expiresAt) || lease.windowID == "") {
		delete(h.focusState.focusLeases, scope)
		ok = false
	}
	for key := range h.focusState.focusLeases {
		if key != scope {
			delete(h.focusState.focusLeases, key)
		}
	}
	return lease.windowID, ok && lease.windowID != ""
}

func (h handlers) clearLease(scope string, token uint64) {
	if h.focusState == nil {
		return
	}
	h.focusState.focusMu.Lock()
	defer h.focusState.focusMu.Unlock()
	if lease, ok := h.focusState.focusLeases[scope]; ok && lease.token == token {
		delete(h.focusState.focusLeases, scope)
	}
}
func (h handlers) clearLeaseForContext(ctx context.Context) {
	if h.focusState == nil {
		return
	}
	if scope := principalSessionScope(ctx); scope != "" {
		h.focusState.focusMu.Lock()
		delete(h.focusState.focusLeases, scope)
		h.focusState.focusMu.Unlock()
	}
}
func (h handlers) invalidateLease(ctx context.Context, id string) {
	if h.focusState == nil {
		return
	}
	scope := principalSessionScope(ctx)
	h.focusState.focusMu.Lock()
	defer h.focusState.focusMu.Unlock()
	if lease, ok := h.focusState.focusLeases[scope]; ok && (id == "" || lease.windowID == id) {
		delete(h.focusState.focusLeases, scope)
	}
}

func NewDesktopRegistry(desktop platform.Desktop, cfg config.Config) (*Registry, map[string]bool, error) {
	if desktop == nil {
		return nil, nil, ErrInvalid
	}
	available := desktop.Available()
	available["shell.execute"] = len(cfg.Shell.AllowedExecutables) > 0
	h := handlers{desktop: desktop, cfg: cfg, focusState: &focusState{focusLeases: make(map[string]focusLease)}}
	if _, ok := desktop.(platform.UIAutomation); ok {
		ready := available["ui.inspect"]
		available["ui.inspect"], available["ui.focus"], available["ui.invoke"] = ready, ready, ready
		available["window.focus"], available["keyboard.type"], available["keyboard.key"] = ready, ready, ready
	} else {
		available["ui.inspect"], available["ui.focus"], available["ui.invoke"] = false, false, false
		available["window.focus"], available["keyboard.type"], available["keyboard.key"] = false, false, false
	}
	// Managed browser support is opt-in through the dedicated provider interface.
	// The legacy default-browser opener is not part of either managed profile.
	for _, name := range []string{"browser.navigate", "browser.snapshot", "browser.query", "browser.click", "browser.fill", "browser.select", "browser.tabs", "browser.back", "browser.screenshot"} {
		available[name] = false
	}
	r := NewRegistry()
	entries := map[string]Handler{
		"screen.capture":     h.capture,
		"pointer.move":       h.move,
		"pointer.click":      h.click,
		"keyboard.type":      h.typeText,
		"keyboard.key":       h.key,
		"clipboard.read":     h.clipboardRead,
		"clipboard.write":    h.clipboardWrite,
		"window.list":        h.windowList,
		"browser.status":     h.browserStatus,
		"window.focus":       h.windowFocus,
		"ui.inspect":         h.uiInspect,
		"ui.focus":           h.uiFocus,
		"ui.invoke":          h.uiInvoke,
		"browser.navigate":   h.browserNavigate,
		"browser.snapshot":   h.browserUnavailable,
		"browser.query":      h.browserUnavailable,
		"browser.click":      h.browserUnavailable,
		"browser.fill":       h.browserUnavailable,
		"browser.select":     h.browserUnavailable,
		"browser.tabs":       h.browserUnavailable,
		"browser.back":       h.browserUnavailable,
		"browser.screenshot": h.browserUnavailable,
		"shell.execute":      h.shellExecute,
	}
	for _, item := range Catalog() {
		if err := r.Register(item.Name, entries[item.Name]); err != nil {
			return nil, nil, err
		}
	}
	return r, available, nil
}

func decodeArgs(raw json.RawMessage, target any) error {
	if len(raw) == 0 {
		raw = json.RawMessage(`{}`)
	}
	dec := json.NewDecoder(bytes.NewReader(raw))
	dec.DisallowUnknownFields()
	if err := dec.Decode(target); err != nil {
		return ErrInvalid
	}
	if err := dec.Decode(new(any)); err != io.EOF {
		return ErrInvalid
	}
	return nil
}

func checkContext(ctx context.Context) error {
	if err := ctx.Err(); err != nil {
		return err
	}
	return nil
}
func encode(value any) (json.RawMessage, error) {
	data, err := json.Marshal(value)
	if err != nil {
		return nil, ErrFailed
	}
	return data, nil
}

func (h handlers) capture(ctx context.Context, _ json.RawMessage) (json.RawMessage, error) {
	if err := checkContext(ctx); err != nil {
		return nil, err
	}
	img, display, err := h.desktop.Capture(ctx)
	if err != nil {
		return nil, ErrFailed
	}
	if img == nil {
		return nil, ErrFailed
	}
	work := img
	var encoded []byte
	for scale := 1; scale <= 8; scale *= 2 {
		if err := checkContext(ctx); err != nil {
			return nil, err
		}
		if scale > 1 {
			work = resizeNearest(img, scale)
		}
		var b bytes.Buffer
		if err := jpeg.Encode(&b, work, &jpeg.Options{Quality: 60}); err != nil {
			return nil, ErrFailed
		}
		encoded = b.Bytes()
		if len(encoded) <= 600*1024 {
			break
		}
	}
	if len(encoded) > 600*1024 {
		return nil, fmt.Errorf("image too large after bounded scaling")
	}
	return encode(map[string]any{"width": work.Bounds().Dx(), "height": work.Bounds().Dy(), "display": display.Name, "captured_at": time.Now().UTC().Format(time.RFC3339Nano), "format": "jpeg", "data_base64": base64.StdEncoding.EncodeToString(encoded), "persisted": false})
}

func resizeNearest(src image.Image, scale int) image.Image {
	bounds := src.Bounds()
	w, h := bounds.Dx()/scale, bounds.Dy()/scale
	if w < 1 || h < 1 {
		return src
	}
	dst := image.NewRGBA(image.Rect(0, 0, w, h))
	for y := 0; y < h; y++ {
		for x := 0; x < w; x++ {
			dst.Set(x, y, src.At(bounds.Min.X+x*scale, bounds.Min.Y+y*scale))
		}
	}
	return dst
}

type moveArgs struct {
	X               int    `json:"x"`
	Y               int    `json:"y"`
	CoordinateSpace string `json:"coordinate_space"`
}

func (h handlers) move(ctx context.Context, raw json.RawMessage) (json.RawMessage, error) {
	var a moveArgs
	if decodeArgs(raw, &a) != nil || a.X < 0 || a.Y < 0 || a.X > 100000 || a.Y > 100000 || a.CoordinateSpace != "primary_display_pixels" {
		return nil, ErrInvalid
	}
	if err := h.desktop.Move(ctx, a.X, a.Y); err != nil {
		return nil, ErrFailed
	}
	return encode(map[string]bool{"moved": true})
}

type clickArgs struct {
	X               int    `json:"x"`
	Y               int    `json:"y"`
	Button          string `json:"button"`
	Count           int    `json:"count"`
	CoordinateSpace string `json:"coordinate_space"`
}

func (h handlers) click(ctx context.Context, raw json.RawMessage) (json.RawMessage, error) {
	var a clickArgs
	if decodeArgs(raw, &a) != nil || a.X < 0 || a.Y < 0 || a.X > 100000 || a.Y > 100000 || a.CoordinateSpace != "primary_display_pixels" || a.Count < 1 || a.Count > 2 || a.Button != "left" && a.Button != "right" && a.Button != "middle" {
		return nil, ErrInvalid
	}
	if err := h.desktop.Click(ctx, a.X, a.Y, a.Button, a.Count); err != nil {
		return nil, ErrFailed
	}
	return encode(map[string]any{"clicked": true, "count": a.Count})
}

type typeArgs struct {
	Text string `json:"text"`
}

func (h handlers) typeText(ctx context.Context, raw json.RawMessage) (json.RawMessage, error) {
	var a typeArgs
	if decodeArgs(raw, &a) != nil || !validUIString(a.Text, 4096, false) {
		return nil, ErrInvalid
	}
	id, ok := h.authorizedWindow(ctx)
	if !ok {
		return nil, ErrFailed
	}
	if d, ok := h.desktop.(interface {
		TypeInWindow(context.Context, string, string) error
	}); ok {
		if err := d.TypeInWindow(ctx, id, a.Text); err != nil {
			h.invalidateLease(ctx, id)
			return nil, ErrFailed
		}
	} else {
		return nil, ErrFailed
	}
	return encode(map[string]any{"typed_chars": len([]rune(a.Text)), "sensitive_payload_redacted": true})
}

type keyArgs struct {
	Key string `json:"key"`
}

func (h handlers) key(ctx context.Context, raw json.RawMessage) (json.RawMessage, error) {
	var a keyArgs
	if decodeArgs(raw, &a) != nil || !validCoreKey(a.Key) {
		return nil, ErrInvalid
	}
	id, ok := h.authorizedWindow(ctx)
	if !ok {
		return nil, ErrFailed
	}
	if d, ok := h.desktop.(interface {
		KeyInWindow(context.Context, string, string, []string) error
	}); ok {
		if err := d.KeyInWindow(ctx, id, a.Key, nil); err != nil {
			h.invalidateLease(ctx, id)
			return nil, ErrFailed
		}
	} else {
		return nil, ErrFailed
	}
	return encode(map[string]bool{"pressed": true})
}

func (h handlers) clipboardRead(ctx context.Context, _ json.RawMessage) (json.RawMessage, error) {
	value, err := h.desktop.ClipboardRead(ctx)
	if err != nil {
		return nil, ErrFailed
	}
	return encode(map[string]any{"text": value, "chars": len([]rune(value))})
}

type clipboardArgs struct {
	Text string `json:"text"`
}

func (h handlers) clipboardWrite(ctx context.Context, raw json.RawMessage) (json.RawMessage, error) {
	var a clipboardArgs
	if decodeArgs(raw, &a) != nil || len(a.Text) > 1<<20 {
		return nil, ErrInvalid
	}
	if err := h.desktop.ClipboardWrite(ctx, a.Text); err != nil {
		return nil, ErrFailed
	}
	return encode(map[string]any{"written_chars": len([]rune(a.Text)), "sensitive_payload_redacted": true})
}

func (h handlers) windowList(ctx context.Context, _ json.RawMessage) (json.RawMessage, error) {
	items, err := h.desktop.Windows(ctx)
	if err != nil {
		return nil, ErrFailed
	}
	if len(items) > 512 {
		items = items[:512]
	}
	return encode(map[string]any{"windows": items})
}

func (h handlers) browserStatus(ctx context.Context, raw json.RawMessage) (json.RawMessage, error) {
	var args struct{}
	if decodeArgs(raw, &args) != nil {
		return nil, ErrInvalid
	}
	items, err := h.desktop.Windows(ctx)
	if err != nil {
		return nil, ErrFailed
	}
	if len(items) > 512 {
		items = items[:512]
	}
	status := map[string]bool{
		"browser_visible":        false,
		"active_browser_visible": false,
	}
	for _, item := range items {
		title := strings.ToLower(item.Title)
		edge := strings.Contains(title, "microsoft edge") || strings.Contains(title, "edge browser")
		browser := edge || strings.Contains(title, "google chrome") ||
			strings.Contains(title, "chrome browser") || strings.Contains(title, "firefox") ||
			strings.Contains(title, "brave")
		status["browser_visible"] = status["browser_visible"] || browser
		status["active_browser_visible"] = status["active_browser_visible"] || (browser && item.Active)
	}
	return encode(map[string]any{"window_count": len(items), "browser_status": status,
		"managed_provider": map[string]bool{"available": false, "healthy": false}})
}

type focusArgs struct {
	WindowID string `json:"window_id"`
}

func (h handlers) windowFocus(ctx context.Context, raw json.RawMessage) (json.RawMessage, error) {
	var a focusArgs
	if decodeArgs(raw, &a) != nil || !validUIString(a.WindowID, 256, false) {
		return nil, ErrInvalid
	}
	if driver, ok := h.desktop.(platform.UIAutomation); !ok || !driver.ValidateWindow(ctx, a.WindowID) {
		h.invalidateLease(ctx, "")
		return nil, ErrFailed
	}
	if err := h.desktop.Focus(ctx, a.WindowID); err != nil {
		h.invalidateLease(ctx, "")
		return nil, ErrFailed
	}
	if err := h.authorizeWindow(ctx, a.WindowID); err != nil {
		return nil, err
	}
	return encode(map[string]bool{"focused": true})
}

func validUIString(value string, max int, optional bool) bool {
	if value == "" {
		return optional
	}
	return len(value) <= max && !strings.ContainsAny(value, "\x00\r\n")
}

func validCoreKey(key string) bool {
	switch key {
	case "ENTER", "ESC", "TAB", "SPACE", "BACKSPACE", "DELETE", "ARROW_UP", "ARROW_DOWN", "ARROW_LEFT", "ARROW_RIGHT", "HOME", "END":
		return true
	default:
		return false
	}
}

func (h handlers) uiInspect(ctx context.Context, raw json.RawMessage) (json.RawMessage, error) {
	var a struct {
		WindowID *string `json:"window_id,omitempty"`
	}
	if decodeArgs(raw, &a) != nil || (a.WindowID != nil && !validUIString(*a.WindowID, 256, false)) {
		return nil, ErrInvalid
	}
	driver, ok := h.desktop.(platform.UIAutomation)
	if !ok {
		return nil, ErrUnavailable
	}
	if err := checkContext(ctx); err != nil {
		return nil, err
	}
	elements, truncated, err := driver.Inspect(ctx, func() string {
		if a.WindowID == nil {
			return ""
		}
		return *a.WindowID
	}(), 16, 256)
	if err != nil {
		return nil, ErrFailed
	}
	if len(elements) > 256 {
		elements = elements[:256]
		truncated = true
	}
	return encode(map[string]any{"elements": elements, "truncated": truncated})
}

func (h handlers) uiFocus(ctx context.Context, raw json.RawMessage) (json.RawMessage, error) {
	var a struct {
		WindowID string `json:"window_id"`
		Target   string `json:"target"`
	}
	if decodeArgs(raw, &a) != nil || !validUIString(a.WindowID, 256, false) || !validUIString(a.Target, 512, false) {
		return nil, ErrInvalid
	}
	driver, ok := h.desktop.(platform.UIAutomation)
	if !ok {
		return nil, ErrUnavailable
	}
	if err := checkContext(ctx); err != nil {
		h.invalidateLease(ctx, "")
		return nil, err
	}
	if err := driver.FocusElement(ctx, a.WindowID, a.Target); err != nil {
		h.invalidateLease(ctx, "")
		return nil, ErrFailed
	}
	if err := h.authorizeWindow(ctx, a.WindowID); err != nil {
		return nil, err
	}
	return encode(map[string]bool{"focused": true})
}

func (h handlers) uiInvoke(ctx context.Context, raw json.RawMessage) (json.RawMessage, error) {
	var a struct {
		WindowID string `json:"window_id"`
		Target   string `json:"target"`
		Action   string `json:"action"`
	}
	if decodeArgs(raw, &a) != nil || !validUIString(a.WindowID, 256, false) || !validUIString(a.Target, 512, false) ||
		(a.Action != "click" && a.Action != "double_click" && a.Action != "submit") {
		return nil, ErrInvalid
	}
	driver, ok := h.desktop.(platform.UIAutomation)
	if !ok {
		return nil, ErrUnavailable
	}
	if err := checkContext(ctx); err != nil {
		h.invalidateLease(ctx, a.WindowID)
		return nil, err
	}
	if err := driver.InvokeElement(ctx, a.WindowID, a.Target, a.Action); err != nil {
		h.invalidateLease(ctx, a.WindowID)
		if ctxErr := ctx.Err(); ctxErr != nil {
			return nil, ctxErr
		}
		return nil, ErrFailed
	}
	if err := checkContext(ctx); err != nil {
		h.invalidateLease(ctx, a.WindowID)
		return nil, err
	}
	return encode(map[string]any{"invoked": true, "action": a.Action})
}

func (h handlers) browserUnavailable(context.Context, json.RawMessage) (json.RawMessage, error) {
	return nil, ErrUnavailable
}

type browserArgs struct {
	URL string `json:"url"`
}

func (h handlers) browserNavigate(ctx context.Context, raw json.RawMessage) (json.RawMessage, error) {
	var a browserArgs
	if decodeArgs(raw, &a) != nil || len(a.URL) > 4096 {
		return nil, ErrInvalid
	}
	u, err := url.ParseRequestURI(a.URL)
	if err != nil || u.Host == "" || u.User != nil || (u.Scheme != "http" && u.Scheme != "https") {
		return nil, ErrInvalid
	}
	if err := checkContext(ctx); err != nil {
		return nil, err
	}
	return nil, ErrUnavailable
}

type shellArgs struct {
	Executable string   `json:"executable"`
	Arguments  []string `json:"arguments"`
	TimeoutMS  int      `json:"timeout_ms"`
}
type shellResult struct {
	ExitCode        int    `json:"exit_code"`
	Stdout          string `json:"stdout"`
	Stderr          string `json:"stderr"`
	OutputTruncated bool   `json:"output_truncated"`
}
type boundedBuffer struct {
	data      []byte
	limit     int
	truncated bool
}

func (b *boundedBuffer) Write(p []byte) (int, error) {
	n := len(p)
	room := b.limit - len(b.data)
	if room > len(p) {
		room = len(p)
	}
	if room > 0 {
		b.data = append(b.data, p[:room]...)
	}
	if room < n {
		b.truncated = true
	}
	return n, nil
}

func (h handlers) shellExecute(ctx context.Context, raw json.RawMessage) (json.RawMessage, error) {
	var a shellArgs
	if decodeArgs(raw, &a) != nil || len(a.Executable) > 4096 || len(a.Arguments) > 128 {
		return nil, ErrInvalid
	}
	clean := filepath.Clean(a.Executable)
	if !filepath.IsAbs(a.Executable) || clean != a.Executable || strings.ContainsRune(a.Executable, 0) {
		return nil, ErrInvalid
	}
	allowed := false
	for _, path := range h.cfg.Shell.AllowedExecutables {
		if path == a.Executable {
			allowed = true
			break
		}
	}
	if !allowed {
		return nil, ErrDenied
	}
	total := 0
	for _, arg := range a.Arguments {
		if strings.ContainsRune(arg, 0) || len(arg) > 8192 {
			return nil, ErrInvalid
		}
		total += len(arg)
		if total > 32768 {
			return nil, ErrInvalid
		}
	}
	timeout := a.TimeoutMS
	if timeout == 0 {
		timeout = 30000
	}
	if timeout < 100 || timeout > 300000 {
		return nil, ErrInvalid
	}
	runCtx, cancel := context.WithTimeout(ctx, time.Duration(timeout)*time.Millisecond)
	defer cancel()
	cmd := exec.CommandContext(runCtx, a.Executable, a.Arguments...)
	cmd.Dir = os.TempDir()
	cmd.Env = []string{}
	for _, name := range h.cfg.Shell.EnvironmentAllowlist {
		if value, ok := os.LookupEnv(name); ok {
			if len(value) > 8192 {
				return nil, ErrInvalid
			}
			cmd.Env = append(cmd.Env, name+"="+value)
		}
	}
	var stdout, stderr boundedBuffer
	stdout.limit = 64 * 1024
	stderr.limit = 64 * 1024
	cmd.Stdout = &stdout
	cmd.Stderr = &stderr
	cmd.WaitDelay = 2 * time.Second
	err := cmd.Run()
	if errors.Is(runCtx.Err(), context.Canceled) {
		return nil, context.Canceled
	}
	if errors.Is(runCtx.Err(), context.DeadlineExceeded) {
		return nil, context.DeadlineExceeded
	}
	result := shellResult{ExitCode: 0, Stdout: string(stdout.data), Stderr: string(stderr.data), OutputTruncated: stdout.truncated || stderr.truncated}
	if err != nil {
		var exit *exec.ExitError
		if errors.As(err, &exit) {
			result.ExitCode = exit.ExitCode()
		} else {
			return nil, ErrFailed
		}
	}
	return encode(result)
}

var _ io.Writer = (*boundedBuffer)(nil)
