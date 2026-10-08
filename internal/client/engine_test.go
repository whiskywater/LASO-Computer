package client

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/Registered-Agent-Attorney/LASO-Computer/internal/audit"
	"github.com/Registered-Agent-Attorney/LASO-Computer/internal/capabilities"
	"github.com/Registered-Agent-Attorney/LASO-Computer/internal/policy"
)

type eventCapture struct {
	mu     sync.Mutex
	events []audit.Event
}

func (c *eventCapture) Record(event audit.Event) {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.events = append(c.events, event)
}

func (c *eventCapture) hasSession(sessionID string) bool {
	c.mu.Lock()
	defer c.mu.Unlock()
	for _, event := range c.events {
		if event.SessionID == sessionID {
			return true
		}
	}
	return false
}

func testEngine(t *testing.T, handler capabilities.Handler) (*Engine, *atomic.Int32) {
	t.Helper()
	r := capabilities.NewRegistry()
	if err := r.Register("screen.capture", handler); err != nil {
		t.Fatal(err)
	}
	if err := r.Register("pointer.move", handler); err != nil {
		t.Fatal(err)
	}
	ex, err := capabilities.NewExecutor(r, policy.Policy{Default: policy.Allow}, map[string]bool{"screen.capture": true, "pointer.move": true}, nil, nil)
	if err != nil {
		t.Fatal(err)
	}
	engine, err := NewEngine(context.Background(), ex, Hello{ClientID: "local-test"})
	if err != nil {
		t.Fatal(err)
	}
	return engine, &atomic.Int32{}
}

func submitBytes(id, job, cap string) []byte {
	data, _ := json.Marshal(Request{ProtocolVersion: 1, RequestID: id, Operation: "submit", JobID: job, Payload: json.RawMessage(`{"capability":"` + cap + `","arguments":{}}`)})
	return data
}

func TestDecodeRejectsMalformedUnknownAndOversizedRequests(t *testing.T) {
	for _, data := range [][]byte{[]byte(`{`), []byte(`{"protocol_version":1,"request_id":"r","operation":"hello","extra":1}`), []byte(`{"protocol_version":1,"request_id":"r","operation":"hello"} {}`)} {
		if _, err := DecodeRequest(data); err == nil {
			t.Fatalf("malformed request accepted: %s", data)
		}
	}
	data := make([]byte, (1<<20)+1)
	if _, err := DecodeRequest(data); err == nil {
		t.Fatal("oversized request accepted")
	}
}

func TestDecodeRejectsDuplicateJSONKeysAtEveryLevel(t *testing.T) {
	for _, data := range [][]byte{
		[]byte(`{"protocol_version":1,"protocol_version":1,"request_id":"r","operation":"hello"}`),
		[]byte(`{"protocol_version":1,"request_id":"r","operation":"hello","payload":{"x":1,"x":2}}`),
	} {
		if _, err := DecodeRequest(data); err == nil {
			t.Fatalf("duplicate JSON key accepted: %s", data)
		}
	}
}

func TestSubmitReplaysSameRequestWithoutExecutingTwice(t *testing.T) {
	called := make(chan struct{}, 2)
	var count atomic.Int32
	handler := func(context.Context, json.RawMessage) (json.RawMessage, error) {
		count.Add(1)
		called <- struct{}{}
		return json.RawMessage(`{"captured":true}`), nil
	}
	engine, _ := testEngine(t, handler)
	request := submitBytes("req-duplicate", "job-1", "screen.capture")
	first := engine.Handle(request)
	second := engine.Handle(request)
	if !first.OK || !second.OK || first.ExternalJobID != second.ExternalJobID {
		t.Fatalf("replay response mismatch: %#v %#v", first, second)
	}
	<-called
	job := engine.jobs[first.ExternalJobID]
	select {
	case <-job.Done:
	case <-time.After(time.Second):
		t.Fatal("submitted job did not complete")
	}
	if count.Load() != 1 {
		t.Fatalf("duplicate request executed %d times", count.Load())
	}
}

func TestHelloIncludesCoreWorkerMetadataAndLegacyDiscovery(t *testing.T) {
	engine, _ := testEngine(t, func(context.Context, json.RawMessage) (json.RawMessage, error) {
		return nil, nil
	})
	engine.hello = Hello{
		ClientID: "local-test", OS: "windows", Architecture: "amd64",
		Capabilities: []CapabilityStatus{{Name: "screen.capture", Available: true, Decision: "deny"}},
	}
	response := engine.Handle(mustRequest(t, Request{ProtocolVersion: 1, RequestID: "hello-1", Operation: "hello"}))
	if !response.OK || response.Metadata["name"] != "LASO Computer" || response.Metadata["supports_status"] != true || response.Metadata["supports_recovery"] != false || response.Metadata["supports_cancellation"] != true {
		t.Fatalf("Core worker metadata missing: %#v", response)
	}
	coreCapabilities, ok := response.Metadata["capabilities"].([]string)
	if !ok || len(coreCapabilities) != 1 || coreCapabilities[0] != "screen.capture" {
		t.Fatalf("Core capability list mismatch: %#v", response.Metadata["capabilities"])
	}
	var legacy Hello
	if err := json.Unmarshal(response.Payload, &legacy); err != nil || legacy.ClientID != "local-test" {
		t.Fatalf("legacy capability discovery missing: %#v, %v", legacy, err)
	}
}

func TestCoreWorkerRequestEnvelopeUsesInputAndDurableSessionIdentity(t *testing.T) {
	called := make(chan capabilities.Invocation, 1)
	engine, _ := testEngine(t, func(_ context.Context, _ json.RawMessage) (json.RawMessage, error) {
		return json.RawMessage(`{"ok":true}`), nil
	})
	auditor := &eventCapture{}
	// Replace the engine's executor with a handler that captures the translated
	// invocation while retaining the same policy guarded path.
	r := capabilities.NewRegistry()
	if err := r.Register("screen.capture", func(_ context.Context, args json.RawMessage) (json.RawMessage, error) {
		called <- capabilities.Invocation{Capability: "screen.capture", Arguments: append(json.RawMessage(nil), args...)}
		return json.RawMessage(`{"captured":true}`), nil
	}); err != nil {
		t.Fatal(err)
	}
	executor, err := capabilities.NewExecutor(r, policy.Policy{Default: policy.Allow}, map[string]bool{"screen.capture": true}, auditor, nil)
	if err != nil {
		t.Fatal(err)
	}
	engine.executor = executor
	payload := map[string]any{
		"job_id": "core-job-1", "worker_id": "computer", "capability": "screen.capture",
		"task_type": "computer-use", "instructions": "capture only", "deadline": time.Now().UTC().Format(time.RFC3339Nano), "timeout_ms": 60000,
		"idempotency_key": "stable-key", "run_id": "run-1", "node_id": "capture", "attempt": 2,
		"input": map[string]any{}, "output_schema": map[string]any{}, "metadata": map[string]any{},
		"artifact_ids": []string{}, "durable_session": true, "durable_session_id": "session-1",
		"continuation":    map[string]any{"provider_id": "provider", "provider_version": "1", "state": "opaque"},
		"session_context": map[string]any{"payload": map[string]any{"history": []any{}}},
	}
	encoded, err := json.Marshal(payload)
	if err != nil {
		t.Fatal(err)
	}
	ack := engine.Handle(mustRequest(t, Request{ProtocolVersion: 1, RequestID: "core-submit-1", Operation: "submit", JobID: "core-job-1", Payload: encoded}))
	if !ack.OK {
		t.Fatalf("Core payload rejected: %#v", ack)
	}
	job := engine.jobs[ack.ExternalJobID]
	select {
	case invocation := <-called:
		if invocation.Capability != "screen.capture" || string(invocation.Arguments) != `{}` {
			t.Fatalf("translated invocation mismatch: %#v", invocation)
		}
	case <-time.After(time.Second):
		t.Fatal("Core payload did not reach the guarded capability handler")
	}
	select {
	case <-job.Done:
	case <-time.After(time.Second):
		t.Fatal("Core payload job did not finish")
	}
	if job.LASOJobID != "core-job-1" {
		t.Fatalf("unexpected LASO job identity: %q", job.LASOJobID)
	}
	if !auditor.hasSession("session-1") {
		t.Fatal("Core durable session identity was not carried into local audit")
	}
}

func TestCoreTaskPromptIsNotBrowserStatusArguments(t *testing.T) {
	called := make(chan json.RawMessage, 1)
	r := capabilities.NewRegistry()
	if err := r.Register("browser.status", func(_ context.Context, args json.RawMessage) (json.RawMessage, error) {
		called <- append(json.RawMessage(nil), args...)
		return json.RawMessage(`{"browser_visible":true}`), nil
	}); err != nil {
		t.Fatal(err)
	}
	executor, err := capabilities.NewExecutor(r, policy.Policy{Default: policy.Allow}, map[string]bool{"browser.status": true}, nil, nil)
	if err != nil {
		t.Fatal(err)
	}
	engine, err := NewEngine(context.Background(), executor, Hello{ClientID: "local-test"})
	if err != nil {
		t.Fatal(err)
	}
	payload, err := json.Marshal(map[string]any{
		"capability": "browser.status",
		"input":      map[string]string{"prompt": "Return browser status only"},
	})
	if err != nil {
		t.Fatal(err)
	}
	request, err := json.Marshal(Request{ProtocolVersion: 1, RequestID: "req-browser-status", Operation: "submit", JobID: "job-browser-status", Payload: payload})
	if err != nil {
		t.Fatal(err)
	}
	ack := engine.Handle(request)
	if !ack.OK || ack.ExternalJobID == "" {
		t.Fatalf("Core browser.status request rejected: %#v", ack)
	}
	select {
	case args := <-called:
		if string(args) != `{}` {
			t.Fatalf("task prompt leaked into browser.status arguments: %s", args)
		}
	case <-time.After(time.Second):
		t.Fatal("browser.status handler did not receive the task")
	}
	job := engine.jobs[ack.ExternalJobID]
	select {
	case <-job.Done:
	case <-time.After(time.Second):
		t.Fatal("browser.status job did not finish")
	}
	if job.State != "Completed" {
		t.Fatalf("browser.status job failed: %s", job.Error)
	}
}

func TestCancelledHandlerCannotPublishLateResult(t *testing.T) {
	started := make(chan struct{})
	release := make(chan struct{})
	engine, _ := testEngine(t, func(context.Context, json.RawMessage) (json.RawMessage, error) {
		close(started)
		<-release // Deliberately ignore the cancelled context.
		return json.RawMessage(`{"stale":true}`), nil
	})
	ack := engine.Handle(submitBytes("req-stale-start", "job-stale", "screen.capture"))
	<-started
	job := engine.jobs[ack.ExternalJobID]
	job.Cancel()
	close(release)
	select {
	case <-job.Done:
	case <-time.After(time.Second):
		t.Fatal("uncooperative handler did not return")
	}
	result := engine.Handle(mustRequest(t, Request{ProtocolVersion: 1, RequestID: "req-stale-result", Operation: "result", ExternalJobID: ack.ExternalJobID}))
	if !result.OK || result.State != "Cancelled" || len(result.Payload) != 0 {
		t.Fatalf("late result escaped cancellation: %#v", result)
	}
}

func TestRestartReportsLostJobAsUnknownWithoutReplayingResult(t *testing.T) {
	first, _ := testEngine(t, func(context.Context, json.RawMessage) (json.RawMessage, error) {
		return json.RawMessage(`{"result":"from prior process"}`), nil
	})
	ack := first.Handle(submitBytes("req-before-restart", "job-before-restart", "screen.capture"))
	if !ack.OK {
		t.Fatalf("initial job was not accepted: %#v", ack)
	}
	prior := first.jobs[ack.ExternalJobID]
	<-prior.Done

	var calls atomic.Int32
	restarted, _ := testEngine(t, func(context.Context, json.RawMessage) (json.RawMessage, error) {
		calls.Add(1)
		return json.RawMessage(`{"result":"must not be replayed"}`), nil
	})
	status := restarted.Handle(mustRequest(t, Request{ProtocolVersion: 1, RequestID: "req-after-restart", Operation: "result", ExternalJobID: ack.ExternalJobID}))
	if !status.OK || status.State != "Unknown" || len(status.Payload) != 0 {
		t.Fatalf("restart did not preserve an ambiguous outcome: %#v", status)
	}
	cancel := restarted.Handle(mustRequest(t, Request{ProtocolVersion: 1, RequestID: "req-cancel-after-restart", Operation: "cancel", ExternalJobID: ack.ExternalJobID}))
	var cancellation map[string]bool
	if !cancel.OK || cancel.State != "Unknown" || json.Unmarshal(cancel.Payload, &cancellation) != nil || cancellation["acknowledged"] {
		t.Fatalf("unknown prior job was incorrectly reported cancelled: %#v", cancel)
	}
	if calls.Load() != 0 {
		t.Fatal("status or cancellation replayed the prior action")
	}
}

func TestCoreWorkerFailuresUseSuccessfulProtocolResponses(t *testing.T) {
	engine, _ := testEngine(t, func(context.Context, json.RawMessage) (json.RawMessage, error) {
		return nil, capabilities.ErrDenied
	})
	ack := engine.Handle(submitBytes("req-failed-submit", "job-failed", "screen.capture"))
	job := engine.jobs[ack.ExternalJobID]
	select {
	case <-job.Done:
	case <-time.After(time.Second):
		t.Fatal("failed job did not finish")
	}
	result := engine.Handle(mustRequest(t, Request{ProtocolVersion: 1, RequestID: "req-failed-result", Operation: "result", ExternalJobID: ack.ExternalJobID}))
	if !result.OK || result.State != "Failed" || result.Error != "capability denied" {
		t.Fatalf("worker-declared failure was represented as a protocol error: %#v", result)
	}
}

func TestCoreWorkerTimeoutAndOversizedResultFailClosed(t *testing.T) {
	var calls atomic.Int32
	engine, _ := testEngine(t, func(context.Context, json.RawMessage) (json.RawMessage, error) {
		calls.Add(1)
		return json.RawMessage(`{"ok":true}`), nil
	})
	// Core currently sends deadline as a submission timestamp. It must not be
	// interpreted as an expiry; only timeout_ms has duration semantics.
	priorTimestamp := map[string]any{"capability": "screen.capture", "input": map[string]any{}, "deadline": time.Now().Add(-time.Minute).UTC().Format(time.RFC3339Nano)}
	priorTimestampWire, _ := json.Marshal(priorTimestamp)
	timestampAck := engine.Handle(mustRequest(t, Request{ProtocolVersion: 1, RequestID: "req-timestamp-submit", Operation: "submit", JobID: "job-timestamp", Payload: priorTimestampWire}))
	if !timestampAck.OK {
		t.Fatalf("Core submission timestamp was rejected: %#v", timestampAck)
	}
	timestampJob := engine.jobs[timestampAck.ExternalJobID]
	<-timestampJob.Done
	timestampResult := engine.Handle(mustRequest(t, Request{ProtocolVersion: 1, RequestID: "req-timestamp-result", Operation: "result", ExternalJobID: timestampAck.ExternalJobID}))
	if timestampResult.State != "Completed" {
		t.Fatalf("Core submission timestamp was treated as an expiry: %#v", timestampResult)
	}

	timeoutEngine, _ := testEngine(t, func(_ context.Context, _ json.RawMessage) (json.RawMessage, error) {
		calls.Add(1)
		time.Sleep(20 * time.Millisecond) // Deliberately ignore the request deadline.
		return json.RawMessage(`{"late":true}`), nil
	})
	payload := map[string]any{"capability": "screen.capture", "input": map[string]any{}, "timeout_ms": 1}
	encoded, _ := json.Marshal(payload)
	ack := timeoutEngine.Handle(mustRequest(t, Request{ProtocolVersion: 1, RequestID: "req-expired-submit", Operation: "submit", JobID: "job-expired", Payload: encoded}))
	if !ack.OK {
		t.Fatalf("timed job was not accepted as a worker job: %#v", ack)
	}
	job := timeoutEngine.jobs[ack.ExternalJobID]
	select {
	case <-job.Done:
	case <-time.After(time.Second):
		t.Fatal("timed job did not finish")
	}
	result := timeoutEngine.Handle(mustRequest(t, Request{ProtocolVersion: 1, RequestID: "req-expired-result", Operation: "result", ExternalJobID: ack.ExternalJobID}))
	if !result.OK || result.State != "TimedOut" || len(result.Payload) != 0 {
		t.Fatalf("late timed action result escaped: %#v calls=%d", result, calls.Load())
	}

	largeEngine, _ := testEngine(t, func(context.Context, json.RawMessage) (json.RawMessage, error) {
		return json.RawMessage(`"` + strings.Repeat("x", maxResultBytes+1) + `"`), nil
	})
	largeAck := largeEngine.Handle(submitBytes("req-large-output-submit", "job-large-output", "screen.capture"))
	largeJob := largeEngine.jobs[largeAck.ExternalJobID]
	<-largeJob.Done
	largeResult := largeEngine.Handle(mustRequest(t, Request{ProtocolVersion: 1, RequestID: "req-large-output-result", Operation: "result", ExternalJobID: largeAck.ExternalJobID}))
	if !largeResult.OK || largeResult.State != "Failed" || len(largeResult.Payload) != 0 {
		t.Fatalf("oversized result was not bounded: %#v", largeResult)
	}
}

func TestLargeResultReplayKeepsPayloadInJobStateOnly(t *testing.T) {
	largePayload, err := json.Marshal(map[string]string{"data": strings.Repeat("x", 256*1024)})
	if err != nil {
		t.Fatal(err)
	}
	engine, _ := testEngine(t, func(context.Context, json.RawMessage) (json.RawMessage, error) { return largePayload, nil })
	ack := engine.Handle(submitBytes("req-large-submit", "job-large", "screen.capture"))
	if !ack.OK {
		t.Fatal("large result job was not submitted")
	}
	job := engine.jobs[ack.ExternalJobID]
	select {
	case <-job.Done:
	case <-time.After(time.Second):
		t.Fatal("large result job did not complete")
	}
	request := mustRequest(t, Request{ProtocolVersion: 1, RequestID: "req-large-result", Operation: "result", ExternalJobID: ack.ExternalJobID})
	first := engine.Handle(request)
	if !first.OK || !bytes.Equal(first.Payload, largePayload) {
		t.Fatal("first large result was not returned")
	}
	if cached := engine.replay["req-large-result"]; len(cached.response.Payload) != 0 || !cached.resultFromJobState {
		t.Fatal("large result bytes were retained in the request replay cache")
	}
	second := engine.Handle(request)
	if !second.OK || !bytes.Equal(second.Payload, largePayload) {
		t.Fatal("duplicate result request could not recover the job result")
	}
}

func TestRequestIDReuseWithDifferentPayloadIsRejected(t *testing.T) {
	var count atomic.Int32
	engine, _ := testEngine(t, func(context.Context, json.RawMessage) (json.RawMessage, error) {
		count.Add(1)
		return json.RawMessage(`{}`), nil
	})
	first := engine.Handle(submitBytes("req-reuse", "job-a", "screen.capture"))
	if !first.OK {
		t.Fatal(first.Error)
	}
	different := engine.Handle(submitBytes("req-reuse", "job-b", "pointer.move"))
	if different.OK || different.Error != "request id reused with different content" {
		t.Fatalf("request ID collision accepted: %#v", different)
	}
	job := engine.jobs[first.ExternalJobID]
	<-job.Done
	if count.Load() != 1 {
		t.Fatalf("unexpected handler count: %d", count.Load())
	}
}

func TestUnknownCapabilityFailsClosed(t *testing.T) {
	var calls atomic.Int32
	engine, _ := testEngine(t, func(context.Context, json.RawMessage) (json.RawMessage, error) {
		calls.Add(1)
		return nil, nil
	})
	request := submitBytes("req-unknown", "job-unknown", "shell.execute")
	ack := engine.Handle(request)
	job := engine.jobs[ack.ExternalJobID]
	<-job.Done
	result := engine.Handle(mustRequest(t, Request{ProtocolVersion: 1, RequestID: "req-result", Operation: "result", ExternalJobID: ack.ExternalJobID}))
	if !result.OK || result.State != "Failed" || result.Error != "unknown capability" {
		t.Fatalf("unknown capability result: %#v", result)
	}
	if calls.Load() != 0 {
		t.Fatal("unknown capability reached a registered handler")
	}
}

func TestCancellationStopsRunningAction(t *testing.T) {
	started := make(chan struct{})
	engine, _ := testEngine(t, func(ctx context.Context, _ json.RawMessage) (json.RawMessage, error) {
		close(started)
		<-ctx.Done()
		return nil, ctx.Err()
	})
	ack := engine.Handle(submitBytes("req-start", "job-cancel", "pointer.move"))
	<-started
	cancel := engine.Handle(mustRequest(t, Request{ProtocolVersion: 1, RequestID: "req-stop", Operation: "cancel", ExternalJobID: ack.ExternalJobID}))
	if !cancel.OK || cancel.State != "Cancelled" {
		t.Fatalf("cancellation not acknowledged: %#v", cancel)
	}
	var cancelPayload map[string]bool
	if err := json.Unmarshal(cancel.Payload, &cancelPayload); err != nil || !cancelPayload["acknowledged"] {
		t.Fatalf("cancellation acknowledgement missing: %s", cancel.Payload)
	}
	result := engine.Handle(mustRequest(t, Request{ProtocolVersion: 1, RequestID: "req-cancel-result", Operation: "result", ExternalJobID: ack.ExternalJobID}))
	if result.State != "Cancelled" {
		t.Fatalf("unexpected final state: %#v", result)
	}
}

func TestDuplicateLASOJobIDRejected(t *testing.T) {
	engine, _ := testEngine(t, func(ctx context.Context, _ json.RawMessage) (json.RawMessage, error) {
		<-ctx.Done()
		return nil, ctx.Err()
	})
	first := engine.Handle(submitBytes("req-job-a", "same-job", "screen.capture"))
	second := engine.Handle(submitBytes("req-job-b", "same-job", "screen.capture"))
	if !first.OK || second.OK || second.Error != "job id already submitted" {
		t.Fatalf("duplicate job behavior: %#v %#v", first, second)
	}
	firstJob := engine.jobs[first.ExternalJobID]
	firstJob.Cancel()
	<-firstJob.Done
}

func mustRequest(t *testing.T, request Request) []byte {
	t.Helper()
	data, err := json.Marshal(request)
	if err != nil {
		t.Fatal(err)
	}
	return data
}

func TestExecutorUnknownCallRemainsDenied(t *testing.T) {
	r := capabilities.NewRegistry()
	if err := r.Register("screen.capture", func(context.Context, json.RawMessage) (json.RawMessage, error) { return nil, nil }); err != nil {
		t.Fatal(err)
	}
	ex, err := capabilities.NewExecutor(r, policy.Policy{Default: policy.Allow}, map[string]bool{"screen.capture": true}, nil, nil)
	if err != nil {
		t.Fatal(err)
	}
	_, err = ex.Invoke(context.Background(), capabilities.Invocation{RequestID: "req-not-registered", Capability: "shell.execute"})
	if !errors.Is(err, capabilities.ErrUnknown) {
		t.Fatalf("expected unknown capability, got %v", err)
	}
}
