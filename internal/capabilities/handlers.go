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
	"time"

	"github.com/Registered-Agent-Attorney/LASO-Computer/internal/config"
	"github.com/Registered-Agent-Attorney/LASO-Computer/internal/platform"
)

type handlers struct {
	desktop platform.Desktop
	cfg     config.Config
}

func NewDesktopRegistry(desktop platform.Desktop, cfg config.Config) (*Registry, map[string]bool, error) {
	if desktop == nil {
		return nil, nil, ErrInvalid
	}
	available := desktop.Available()
	available["shell.execute"] = len(cfg.Shell.AllowedExecutables) > 0
	h := handlers{desktop: desktop, cfg: cfg}
	r := NewRegistry()
	entries := map[string]Handler{
		"screen.capture":   h.capture,
		"pointer.move":     h.move,
		"pointer.click":    h.click,
		"keyboard.type":    h.typeText,
		"keyboard.key":     h.key,
		"clipboard.read":   h.clipboardRead,
		"clipboard.write":  h.clipboardWrite,
		"window.list":      h.windowList,
		"window.focus":     h.windowFocus,
		"browser.navigate": h.browserNavigate,
		"shell.execute":    h.shellExecute,
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
	Text      string `json:"text"`
	Sensitive bool   `json:"sensitive"`
}

func (h handlers) typeText(ctx context.Context, raw json.RawMessage) (json.RawMessage, error) {
	var a typeArgs
	if decodeArgs(raw, &a) != nil || len(a.Text) > 8192 {
		return nil, ErrInvalid
	}
	if err := h.desktop.Type(ctx, a.Text); err != nil {
		return nil, ErrFailed
	}
	return encode(map[string]any{"typed_chars": len([]rune(a.Text)), "sensitive_payload_redacted": true})
}

type keyArgs struct {
	Key       string   `json:"key"`
	Modifiers []string `json:"modifiers"`
}

func (h handlers) key(ctx context.Context, raw json.RawMessage) (json.RawMessage, error) {
	var a keyArgs
	if decodeArgs(raw, &a) != nil || len(a.Key) < 1 || len(a.Key) > 32 || len(a.Modifiers) > 3 {
		return nil, ErrInvalid
	}
	if err := h.desktop.Key(ctx, a.Key, a.Modifiers); err != nil {
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

type focusArgs struct {
	WindowID string `json:"window_id"`
}

func (h handlers) windowFocus(ctx context.Context, raw json.RawMessage) (json.RawMessage, error) {
	var a focusArgs
	if decodeArgs(raw, &a) != nil || len(a.WindowID) > 128 {
		return nil, ErrInvalid
	}
	if err := h.desktop.Focus(ctx, a.WindowID); err != nil {
		return nil, ErrFailed
	}
	return encode(map[string]bool{"focused": true})
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
	if err := h.desktop.OpenURL(ctx, a.URL); err != nil {
		return nil, ErrFailed
	}
	return encode(map[string]any{"opened": true, "host": u.Hostname()})
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
