package client

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	"github.com/Registered-Agent-Attorney/LASO-Computer/internal/capabilities"
	"github.com/Registered-Agent-Attorney/LASO-Computer/internal/policy"
)

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
	if result.OK || result.Error != "unknown capability" {
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
