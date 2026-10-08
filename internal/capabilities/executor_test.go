package capabilities

import (
	"context"
	"encoding/json"
	"errors"
	"image"
	"os"
	"path/filepath"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	"github.com/Registered-Agent-Attorney/LASO-Computer/internal/audit"
	"github.com/Registered-Agent-Attorney/LASO-Computer/internal/config"
	"github.com/Registered-Agent-Attorney/LASO-Computer/internal/platform"
	"github.com/Registered-Agent-Attorney/LASO-Computer/internal/policy"
)

type testDesktop struct{ calls atomic.Int32 }

func (d *testDesktop) Available() map[string]bool {
	return map[string]bool{"screen.capture": true, "browser.navigate": true}
}
func (d *testDesktop) Capture(context.Context) (image.Image, platform.Display, error) {
	d.calls.Add(1)
	return nil, platform.Display{}, nil
}
func (d *testDesktop) Move(context.Context, int, int) error               { d.calls.Add(1); return nil }
func (d *testDesktop) Click(context.Context, int, int, string, int) error { d.calls.Add(1); return nil }
func (d *testDesktop) Type(context.Context, string) error                 { d.calls.Add(1); return nil }
func (d *testDesktop) Key(context.Context, string, []string) error        { d.calls.Add(1); return nil }
func (d *testDesktop) Windows(context.Context) ([]platform.Window, error) {
	d.calls.Add(1)
	return nil, nil
}
func (d *testDesktop) Focus(context.Context, string) error           { d.calls.Add(1); return nil }
func (d *testDesktop) ClipboardRead(context.Context) (string, error) { d.calls.Add(1); return "", nil }
func (d *testDesktop) ClipboardWrite(context.Context, string) error  { d.calls.Add(1); return nil }
func (d *testDesktop) OpenURL(context.Context, string) error         { d.calls.Add(1); return nil }

type events []audit.Event

func (e *events) Record(v audit.Event) { *e = append(*e, v) }

func testExecutor(t *testing.T, decision policy.Decision, writer Auditor) (*Executor, *testDesktop) {
	t.Helper()
	d := &testDesktop{}
	cfg := config.Default()
	r, available, err := NewDesktopRegistry(d, cfg)
	if err != nil {
		t.Fatal(err)
	}
	ex, err := NewExecutor(r, policy.Policy{Default: policy.Deny, Capabilities: map[string]policy.Decision{"screen.capture": decision}}, available, writer, nil)
	if err != nil {
		t.Fatal(err)
	}
	return ex, d
}

func TestRegistryRejectsDuplicateAndUnknownRegistration(t *testing.T) {
	r := NewRegistry()
	h := func(context.Context, json.RawMessage) (json.RawMessage, error) { return nil, nil }
	if err := r.Register("screen.capture", h); err != nil {
		t.Fatal(err)
	}
	if err := r.Register("screen.capture", h); err == nil {
		t.Fatal("duplicate registration accepted")
	}
	if err := r.Register("not.a.capability", h); err == nil {
		t.Fatal("unknown registration accepted")
	}
	if got := r.Names(); len(got) != 1 || got[0] != "screen.capture" {
		t.Fatalf("unexpected registry names: %v", got)
	}
}

func TestDenyStopsBeforePlatformHandler(t *testing.T) {
	ex, d := testExecutor(t, policy.Deny, nil)
	_, err := ex.Invoke(context.Background(), Invocation{RequestID: "req-1", Capability: "screen.capture", Arguments: json.RawMessage(`{}`)})
	if !errors.Is(err, ErrDenied) {
		t.Fatalf("expected denied, got %v", err)
	}
	if d.calls.Load() != 0 {
		t.Fatal("denied action reached platform executor")
	}
}

func TestRequireApprovalFailsClosedUnlessApproved(t *testing.T) {
	for _, approved := range []bool{false, true} {
		d := &testDesktop{}
		cfg := config.Default()
		r, available, err := NewDesktopRegistry(d, cfg)
		if err != nil {
			t.Fatal(err)
		}
		approvalCalls := atomic.Int32{}
		approve := func(context.Context, string, Principal) (bool, error) { approvalCalls.Add(1); return approved, nil }
		ex, err := NewExecutor(r, policy.Policy{Default: policy.RequireApproval}, available, nil, approve)
		if err != nil {
			t.Fatal(err)
		}
		_, _ = ex.Invoke(context.Background(), Invocation{RequestID: "req-approval", Capability: "screen.capture", Arguments: json.RawMessage(`{}`)})
		if approvalCalls.Load() != 1 {
			t.Fatalf("approval was not requested exactly once")
		}
		wantCalls := int32(0)
		if approved {
			wantCalls = 1
		}
		if d.calls.Load() != wantCalls {
			t.Fatalf("approved=%v reached platform %d times", approved, d.calls.Load())
		}
	}
}

func TestAllowInvokesRegisteredHandler(t *testing.T) {
	ex, d := testExecutor(t, policy.Allow, nil)
	_, err := ex.Invoke(context.Background(), Invocation{RequestID: "req-2", Capability: "screen.capture", Arguments: json.RawMessage(`{}`)})
	if !errors.Is(err, ErrFailed) {
		t.Fatalf("fake capture should report failure, got %v", err)
	}
	if d.calls.Load() != 1 {
		t.Fatalf("expected one platform call, got %d", d.calls.Load())
	}
}

func TestExecutorHonorsCancellation(t *testing.T) {
	r := NewRegistry()
	started := make(chan struct{})
	if err := r.Register("pointer.move", func(ctx context.Context, _ json.RawMessage) (json.RawMessage, error) {
		close(started)
		<-ctx.Done()
		return nil, ctx.Err()
	}); err != nil {
		t.Fatal(err)
	}
	ex, err := NewExecutor(r, policy.Policy{Default: policy.Allow}, map[string]bool{"pointer.move": true}, nil, nil)
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan error, 1)
	go func() {
		_, err := ex.Invoke(ctx, Invocation{RequestID: "req-cancel", Capability: "pointer.move", Arguments: json.RawMessage(`{}`)})
		done <- err
	}()
	<-started
	cancel()
	select {
	case err := <-done:
		if !errors.Is(err, context.Canceled) {
			t.Fatalf("expected cancellation, got %v", err)
		}
	case <-time.After(time.Second):
		t.Fatal("executor did not stop after cancellation")
	}
}

func TestAuditDoesNotRecordSensitiveArguments(t *testing.T) {
	path := filepath.Join(t.TempDir(), "audit.jsonl")
	writer, err := audit.Open(path)
	if err != nil {
		t.Fatal(err)
	}
	defer writer.Close()
	secret := "Example-Passphrase-8zQ"
	r := NewRegistry()
	if err := r.Register("keyboard.type", func(_ context.Context, raw json.RawMessage) (json.RawMessage, error) {
		var a map[string]string
		_ = json.Unmarshal(raw, &a)
		return json.Marshal(map[string]int{"typed_chars": len(a["text"])})
	}); err != nil {
		t.Fatal(err)
	}
	ex, err := NewExecutor(r, policy.Policy{Default: policy.Allow}, map[string]bool{"keyboard.type": true}, writer, nil)
	if err != nil {
		t.Fatal(err)
	}
	args, _ := json.Marshal(map[string]string{"text": secret})
	if _, err := ex.Invoke(context.Background(), Invocation{RequestID: "req-audit", Capability: "keyboard.type", Arguments: args, Principal: Principal{AgentID: "agent-test", SessionID: "session-test", JobID: "job-test"}}); err != nil {
		t.Fatal(err)
	}
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	if strings.Contains(string(data), secret) {
		t.Fatal("audit log contains typed text")
	}
	if !strings.Contains(string(data), `"payload_redacted":true`) {
		t.Fatalf("audit redaction marker missing: %s", data)
	}
}

func TestAuditFailureStopsAllowedCapability(t *testing.T) {
	path := filepath.Join(t.TempDir(), "audit.jsonl")
	writer, err := audit.Open(path)
	if err != nil {
		t.Fatal(err)
	}
	if err := writer.Close(); err != nil {
		t.Fatal(err)
	}
	ex, d := testExecutor(t, policy.Allow, writer)
	_, err = ex.Invoke(context.Background(), Invocation{RequestID: "req-audit-failure", Capability: "screen.capture", Arguments: json.RawMessage(`{}`)})
	if !errors.Is(err, ErrAudit) {
		t.Fatalf("expected audit failure, got %v", err)
	}
	if d.calls.Load() != 0 {
		t.Fatal("capability ran without a durable started audit event")
	}
	if writer.Err() == nil {
		t.Fatal("writer did not retain its append failure")
	}
}

func TestBrowserNavigationRejectsNonWebSchemes(t *testing.T) {
	d := &testDesktop{}
	cfg := config.Default()
	r, available, err := NewDesktopRegistry(d, cfg)
	if err != nil {
		t.Fatal(err)
	}
	ex, err := NewExecutor(r, policy.Policy{Default: policy.Allow}, available, nil, nil)
	if err != nil {
		t.Fatal(err)
	}
	_, err = ex.Invoke(context.Background(), Invocation{RequestID: "req-url", Capability: "browser.navigate", Arguments: json.RawMessage(`{"url":"file:///private/data"}`)})
	if !errors.Is(err, ErrUnavailable) {
		t.Fatalf("expected managed browser to fail closed while unavailable, got %v", err)
	}
	if d.calls.Load() != 0 {
		t.Fatal("invalid URL reached platform opener")
	}
}
