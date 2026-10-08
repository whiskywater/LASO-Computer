package capabilities

import (
	"context"
	"encoding/json"
	"errors"
	"github.com/Registered-Agent-Attorney/LASO-Computer/internal/config"
	"github.com/Registered-Agent-Attorney/LASO-Computer/internal/platform"
	"image"
	"testing"
	"time"
)

type uiaDesktop struct {
	windows                 []platform.Window
	items                   []platform.UIElement
	inspectedID             string
	depth, nodes            int
	focusID, target, action string
	valid                   bool
	keyErr                  error
	keyCalls                int
	invokeErr               error
	invokeCancel            context.CancelFunc
}

func workerContext() context.Context {
	return workerContextFor("agent", "session", "job")
}
func workerContextFor(agent, session, job string) context.Context {
	return context.WithValue(context.Background(), principalContextKey{}, Principal{AgentID: agent, SessionID: session, JobID: job})
}

func (d *uiaDesktop) Available() map[string]bool {
	return map[string]bool{"ui.inspect": true, "ui.focus": true, "ui.invoke": true, "window.focus": true, "keyboard.type": true, "keyboard.key": true}
}
func (d *uiaDesktop) Capture(context.Context) (image.Image, platform.Display, error) {
	return nil, platform.Display{}, nil
}
func (d *uiaDesktop) Move(context.Context, int, int) error               { return nil }
func (d *uiaDesktop) Click(context.Context, int, int, string, int) error { return nil }
func (d *uiaDesktop) Type(context.Context, string) error                 { return nil }
func (d *uiaDesktop) TypeInWindow(context.Context, string, string) error { return nil }
func (d *uiaDesktop) Key(context.Context, string, []string) error        { return nil }
func (d *uiaDesktop) KeyInWindow(context.Context, string, string, []string) error {
	d.keyCalls++
	return d.keyErr
}
func (d *uiaDesktop) Windows(context.Context) ([]platform.Window, error) { return d.windows, nil }
func (d *uiaDesktop) Focus(context.Context, string) error                { return nil }
func (d *uiaDesktop) ClipboardRead(context.Context) (string, error)      { return "", nil }
func (d *uiaDesktop) ClipboardWrite(context.Context, string) error       { return nil }
func (d *uiaDesktop) OpenURL(context.Context, string) error              { return nil }
func (d *uiaDesktop) Inspect(_ context.Context, id string, depth, nodes int) ([]platform.UIElement, bool, error) {
	d.inspectedID = id
	d.depth = depth
	d.nodes = nodes
	return d.items, false, nil
}
func (d *uiaDesktop) FocusElement(_ context.Context, id, target string) error {
	d.focusID = id
	d.target = target
	return nil
}
func (d *uiaDesktop) InvokeElement(_ context.Context, id, target, action string) error {
	d.focusID = id
	d.target = target
	d.action = action
	if d.invokeCancel != nil {
		d.invokeCancel()
	}
	return d.invokeErr
}
func (d *uiaDesktop) ValidateWindow(_ context.Context, id string) bool {
	if !d.valid {
		return false
	}
	for _, w := range d.windows {
		if w.ID == id {
			return true
		}
	}
	return false
}

func TestFrozenChatProfileUIInspectSchemaAndBounds(t *testing.T) {
	d := &uiaDesktop{valid: true, windows: []platform.Window{{ID: "0x123", Title: "Outlook", Active: true}}, items: []platform.UIElement{{Name: "Send", AutomationID: "send", ControlType: 50000, Enabled: true}}}
	h := handlers{desktop: d, focusState: &focusState{focusLeases: make(map[string]focusLease)}}
	if _, e := h.uiInspect(context.Background(), json.RawMessage(`{"target":"Send"}`)); !errors.Is(e, ErrInvalid) {
		t.Fatalf("unknown inspect field accepted: %v", e)
	}
	if _, e := h.uiInspect(context.Background(), json.RawMessage(`{"window_id":""}`)); !errors.Is(e, ErrInvalid) {
		t.Fatalf("empty window id accepted: %v", e)
	}
	r, e := h.uiInspect(context.Background(), json.RawMessage(`{"window_id":"0x123"}`))
	if e != nil {
		t.Fatal(e)
	}
	var out struct {
		Elements  []platform.UIElement `json:"elements"`
		Truncated bool                 `json:"truncated"`
	}
	if e = json.Unmarshal(r, &out); e != nil {
		t.Fatal(e)
	}
	if len(out.Elements) != 1 || d.inspectedID != "0x123" || d.depth != 16 || d.nodes != 256 {
		t.Fatalf("unexpected inspect result=%s driver=%+v", r, d)
	}
}
func TestFrozenChatProfileTargetActionsAndExactInputs(t *testing.T) {
	d := &uiaDesktop{valid: true, windows: []platform.Window{{ID: "0x123", Title: "Outlook", Active: true}}}
	h := handlers{desktop: d, focusState: &focusState{focusLeases: make(map[string]focusLease)}}
	if _, e := h.uiFocus(workerContext(), json.RawMessage(`{"window_id":"0x123","target":"Send"}`)); e != nil || d.target != "Send" {
		t.Fatalf("focus failed: %v", e)
	}
	if _, e := h.uiInvoke(context.Background(), json.RawMessage(`{"window_id":"0x123","target":"Send","action":"double_click"}`)); e != nil || d.action != "double_click" {
		t.Fatalf("invoke failed: %v", e)
	}
	if _, e := h.uiInvoke(context.Background(), json.RawMessage(`{"window_id":"0x123","target":"Send","action":"toggle"}`)); !errors.Is(e, ErrInvalid) {
		t.Fatalf("unsupported action accepted: %v", e)
	}
	if _, e := h.typeText(context.Background(), json.RawMessage(`{"text":"hi","sensitive":true}`)); !errors.Is(e, ErrInvalid) {
		t.Fatalf("unfrozen keyboard field accepted: %v", e)
	}
	if _, e := h.key(context.Background(), json.RawMessage(`{"key":"F1"}`)); !errors.Is(e, ErrInvalid) {
		t.Fatalf("unsupported core key accepted: %v", e)
	}
	if _, e := h.key(workerContext(), json.RawMessage(`{"key":"ARROW_UP"}`)); e != nil {
		t.Fatalf("allowed core key rejected: %v", e)
	}
	otherJob := workerContextFor("agent", "session", "next-tool-job")
	if _, e := h.key(otherJob, json.RawMessage(`{"key":"ARROW_UP"}`)); e != nil {
		t.Fatalf("same-session next-job focus continuation failed: %v", e)
	}
	otherSession := workerContextFor("agent", "new-session", "job")
	if _, e := h.key(otherSession, json.RawMessage(`{"key":"ARROW_UP"}`)); !errors.Is(e, ErrFailed) {
		t.Fatalf("focus lease crossed session boundary: %v", e)
	}
	if len(h.focusState.focusLeases) != 0 {
		t.Fatalf("changed session left stale leases: %+v", h.focusState.focusLeases)
	}
}

type manualCancelContext struct {
	context.Context
	canceled bool
}

func (c *manualCancelContext) Done() <-chan struct{} { return nil }
func (c *manualCancelContext) Err() error {
	if c.canceled {
		return context.Canceled
	}
	return nil
}

func TestUIInvokeFailureAndCancellationClearFocusLease(t *testing.T) {
	d := &uiaDesktop{valid: true, windows: []platform.Window{{ID: "0x123", Title: "Fixture", Active: true}}}
	h := handlers{desktop: d, focusState: &focusState{focusLeases: make(map[string]focusLease)}}
	ctx := workerContext()
	if err := h.authorizeWindow(ctx, "0x123"); err != nil {
		t.Fatal(err)
	}
	d.invokeErr = errors.New("invoke failed")
	if _, err := h.uiInvoke(ctx, json.RawMessage(`{"window_id":"0x123","target":"Button","action":"click"}`)); !errors.Is(err, ErrFailed) {
		t.Fatalf("invoke failure=%v", err)
	}
	if _, ok := h.authorizedWindow(ctx); ok {
		t.Fatal("failed invoke retained focus lease")
	}

	controlled := &manualCancelContext{Context: workerContext()}
	if err := h.authorizeWindow(controlled, "0x123"); err != nil {
		t.Fatal(err)
	}
	d.invokeErr = nil
	controlled.canceled = true
	if _, err := h.uiInvoke(controlled, json.RawMessage(`{"window_id":"0x123","target":"Button","action":"click"}`)); !errors.Is(err, context.Canceled) {
		t.Fatalf("cancelled invoke=%v", err)
	}
	if len(h.focusState.focusLeases) != 0 {
		t.Fatalf("cancelled invoke retained leases: %+v", h.focusState.focusLeases)
	}
}

func TestUIInvokeInFlightCancellationPreservesCancelledStatusAndClearsLease(t *testing.T) {
	d := &uiaDesktop{valid: true, windows: []platform.Window{{ID: "0x123", Title: "Fixture", Active: true}}}
	h := handlers{desktop: d, focusState: &focusState{focusLeases: make(map[string]focusLease)}}
	ctx, cancel := context.WithCancel(workerContext())
	if err := h.authorizeWindow(ctx, "0x123"); err != nil {
		t.Fatal(err)
	}
	d.invokeCancel = cancel
	d.invokeErr = context.Canceled
	if _, err := h.uiInvoke(ctx, json.RawMessage(`{"window_id":"0x123","target":"Button","action":"click"}`)); !errors.Is(err, context.Canceled) {
		t.Fatalf("in-flight cancellation=%v", err)
	}
	if len(h.focusState.focusLeases) != 0 {
		t.Fatalf("in-flight cancellation retained leases: %+v", h.focusState.focusLeases)
	}
}

func TestFocusLeaseExpiresCancelsAndClearsOnTargetInvalidation(t *testing.T) {
	d := &uiaDesktop{valid: true, windows: []platform.Window{{ID: "0x123", Title: "Notepad", Active: true}}}
	h := handlers{desktop: d, focusState: &focusState{focusLeases: make(map[string]focusLease)}}
	ctx := workerContext()
	if err := h.authorizeWindow(ctx, "0x123"); err != nil {
		t.Fatal(err)
	}
	h.focusState.focusMu.Lock()
	lease := h.focusState.focusLeases[principalSessionScope(ctx)]
	lease.expiresAt = time.Now().Add(-time.Second)
	h.focusState.focusLeases[principalSessionScope(ctx)] = lease
	h.focusState.focusMu.Unlock()
	if _, ok := h.authorizedWindow(ctx); ok {
		t.Fatal("expired focus lease accepted")
	}
	if len(h.focusState.focusLeases) != 0 {
		t.Fatal("expired lease not removed")
	}
	cancelCtx, cancel := context.WithCancel(workerContext())
	if err := h.authorizeWindow(cancelCtx, "0x123"); err != nil {
		t.Fatal(err)
	}
	cancel()
	if _, ok := h.authorizedWindow(cancelCtx); ok {
		t.Fatal("cancelled focus lease accepted")
	}
	if len(h.focusState.focusLeases) != 0 {
		t.Fatal("cancelled lease not removed")
	}
	if err := h.authorizeWindow(ctx, "0x123"); err != nil {
		t.Fatal(err)
	}
	d.keyErr = errors.New("stale target")
	args := json.RawMessage(`{"key":"ARROW_UP"}`)
	if _, err := h.key(ctx, args); !errors.Is(err, ErrFailed) {
		t.Fatalf("target failure=%v", err)
	}
	d.keyErr = nil
	if _, ok := h.authorizedWindow(ctx); ok {
		t.Fatal("target failure retained focus lease")
	}
}
func TestWindowFocusRequiresCurrentWindow(t *testing.T) {
	d := &uiaDesktop{valid: false, windows: []platform.Window{{ID: "0x123", Title: "Outlook", Active: true}}}
	h := handlers{desktop: d, focusState: &focusState{focusLeases: make(map[string]focusLease)}}
	if _, e := h.windowFocus(workerContext(), json.RawMessage(`{"window_id":"0x123"}`)); !errors.Is(e, ErrFailed) {
		t.Fatalf("stale window focused: %v", e)
	}
	d.valid = true
	if _, e := h.windowFocus(workerContext(), json.RawMessage(`{"window_id":"0x123"}`)); e != nil {
		t.Fatalf("current window not focused: %v", e)
	}
}
func TestUIACapabilitiesUnavailableWithoutUIADriver(t *testing.T) {
	d := &testDesktop{}
	_, available, e := NewDesktopRegistry(d, config.Default())
	if e != nil {
		t.Fatal(e)
	}
	for _, name := range []string{"ui.inspect", "ui.focus", "ui.invoke", "window.focus", "keyboard.type", "keyboard.key"} {
		if available[name] {
			t.Fatalf("%s advertised without UIA", name)
		}
	}
}
