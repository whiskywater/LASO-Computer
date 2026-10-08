package transport

import (
	"bytes"
	"context"
	"encoding/json"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/Registered-Agent-Attorney/LASO-Computer/internal/capabilities"
	"github.com/Registered-Agent-Attorney/LASO-Computer/internal/client"
	"github.com/Registered-Agent-Attorney/LASO-Computer/internal/policy"
)

type lockedBuffer struct {
	mu sync.Mutex
	b  bytes.Buffer
}

func (b *lockedBuffer) Write(p []byte) (int, error) {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.b.Write(p)
}
func (b *lockedBuffer) String() string { b.mu.Lock(); defer b.mu.Unlock(); return b.b.String() }

func TestApprovalIsCorrelatedAndContainsNoActionArguments(t *testing.T) {
	out := &lockedBuffer{}
	server := NewServer(strings.NewReader(""), out, time.Second)
	type answer struct {
		ok  bool
		err error
	}
	done := make(chan answer, 1)
	go func() {
		ok, err := server.RequestApproval(context.Background(), "keyboard.type", capabilities.Principal{JobID: "job-1", SessionID: "session-1", ExternalJobID: "local-1"})
		done <- answer{ok, err}
	}()
	var request workerRequest
	deadline := time.Now().Add(time.Second)
	for time.Now().Before(deadline) {
		data := strings.TrimSpace(out.String())
		if data != "" {
			if err := json.Unmarshal([]byte(data), &request); err != nil {
				t.Fatal(err)
			}
			break
		}
		time.Sleep(time.Millisecond)
	}
	if request.RequestID == "" || request.RequestType != "permission" || request.Payload["resource"] != "keyboard.type" {
		t.Fatalf("unexpected worker request: %#v", request)
	}
	if strings.Contains(out.String(), "secret") {
		t.Fatal("approval request included action payload")
	}
	response, _ := json.Marshal(workerResponse{ProtocolVersion: 1, MessageType: "worker_response", RequestID: request.RequestID, Decision: "approved"})
	if !server.receiveApproval(response) {
		t.Fatal("worker response was not routed")
	}
	select {
	case result := <-done:
		if result.err != nil || !result.ok {
			t.Fatalf("approval was not accepted: %#v", result)
		}
	case <-time.After(time.Second):
		t.Fatal("approval wait did not finish")
	}
}

func TestDuplicateWorkerResponseKeysFailClosed(t *testing.T) {
	server := NewServer(strings.NewReader(""), &lockedBuffer{}, time.Second)
	response := make(chan workerResponse, 1)
	server.pending["request-1"] = response
	frame := []byte(`{"protocol_version":1,"message_type" : "worker_response","request_id":"request-1","request_id":"request-2","decision":"approved"}`)
	if !server.receiveApproval(frame) {
		t.Fatal("duplicate worker response was not recognized")
	}
	select {
	case result := <-response:
		if !result.protocolError {
			t.Fatal("duplicate worker response was not rejected")
		}
	case <-time.After(time.Second):
		t.Fatal("malformed worker response left interaction pending")
	}
}

func TestQuestionExchangeReturnsStrictlyCorrelatedPayload(t *testing.T) {
	out := &lockedBuffer{}
	server := NewServer(strings.NewReader(""), out, time.Second)
	type answer struct {
		response InteractionResponse
		err      error
	}
	done := make(chan answer, 1)
	go func() {
		response, err := server.RequestInteraction(context.Background(), "question", "Choose an option", "Select one item", map[string]string{"question": "Continue?"},
			capabilities.Principal{JobID: "job-q", SessionID: "session-q", ExternalJobID: "local-q"}, "medium", "provider")
		done <- answer{response, err}
	}()
	request := waitWorkerRequest(t, out)
	if request.RequestType != "question" || request.Payload["question"] != "Continue?" || request.WorkerJobID != "job-q" {
		t.Fatalf("unexpected question request: %#v", request)
	}
	reply, _ := json.Marshal(workerResponse{ProtocolVersion: 1, MessageType: "worker_response", RequestID: request.RequestID, Decision: "answered", Payload: json.RawMessage(`{"answer":"yes"}`)})
	if !server.receiveApproval(reply) {
		t.Fatal("question response was not routed")
	}
	select {
	case result := <-done:
		if result.err != nil || result.response.Decision != "answered" || string(result.response.Payload) != `{"answer":"yes"}` {
			t.Fatalf("unexpected answer: %#v", result)
		}
	case <-time.After(time.Second):
		t.Fatal("question exchange did not finish")
	}
}

func TestMalformedCorrelatedInteractionFailsClosedImmediately(t *testing.T) {
	out := &lockedBuffer{}
	server := NewServer(strings.NewReader(""), out, time.Second)
	done := make(chan error, 1)
	go func() {
		_, err := server.RequestInteraction(context.Background(), "permission", "Permission", "Allow action", map[string]string{"resource": "ui.invoke"}, capabilities.Principal{JobID: "job-m"}, "high", "tool")
		done <- err
	}()
	request := waitWorkerRequest(t, out)
	bad, _ := json.Marshal(map[string]any{"protocol_version": 1, "message_type": "worker_response", "request_id": request.RequestID, "decision": "approved", "unexpected": true})
	if !server.receiveApproval(bad) {
		t.Fatal("malformed worker response was not consumed")
	}
	select {
	case err := <-done:
		if err == nil || !strings.Contains(err.Error(), "malformed") {
			t.Fatalf("malformed reply did not fail closed: %v", err)
		}
	case <-time.After(time.Second):
		t.Fatal("malformed correlated reply waited for timeout")
	}
}

func TestInteractionHonorsCancellationAndTimeout(t *testing.T) {
	for _, cancelNow := range []bool{true, false} {
		out := &lockedBuffer{}
		timeout := 20 * time.Millisecond
		server := NewServer(strings.NewReader(""), out, timeout)
		ctx, cancel := context.WithCancel(context.Background())
		if cancelNow {
			cancel()
		}
		_, err := server.RequestInteraction(ctx, "permission", "Permission", "Approve", map[string]string{"resource": "ui.focus"}, capabilities.Principal{}, "medium", "tool")
		cancel()
		if err == nil {
			t.Fatal("interaction without a response succeeded")
		}
		if cancelNow && !strings.Contains(err.Error(), "canceled") {
			t.Fatalf("cancellation was not returned: %v", err)
		}
		if !cancelNow && !strings.Contains(err.Error(), "timed out") {
			t.Fatalf("missing response did not time out: %v", err)
		}
		server.pendingMu.Lock()
		pending := len(server.pending)
		server.pendingMu.Unlock()
		if pending != 0 {
			t.Fatal("completed interaction remained correlated")
		}
	}
}

func waitWorkerRequest(t *testing.T, out *lockedBuffer) workerRequest {
	t.Helper()
	deadline := time.Now().Add(time.Second)
	for time.Now().Before(deadline) {
		data := strings.TrimSpace(out.String())
		if data != "" {
			var request workerRequest
			if err := json.Unmarshal([]byte(data), &request); err != nil {
				t.Fatal(err)
			}
			return request
		}
		time.Sleep(time.Millisecond)
	}
	t.Fatal("worker interaction was not written")
	return workerRequest{}
}

func TestMalformedFrameReturnsBoundedFailure(t *testing.T) {
	registry := capabilities.NewRegistry()
	if err := registry.Register("screen.capture", func(context.Context, json.RawMessage) (json.RawMessage, error) { return nil, nil }); err != nil {
		t.Fatal(err)
	}
	executor, err := capabilities.NewExecutor(registry, policy.Policy{Default: policy.Deny}, map[string]bool{"screen.capture": true}, nil, nil)
	if err != nil {
		t.Fatal(err)
	}
	engine, err := client.NewEngine(context.Background(), executor, client.Hello{ClientID: "test"})
	if err != nil {
		t.Fatal(err)
	}
	out := &bytes.Buffer{}
	server := NewServer(strings.NewReader("{malformed}\n"), out, time.Second)
	if err := server.Serve(context.Background(), engine); err != nil {
		t.Fatal(err)
	}
	var response client.Response
	if err := json.Unmarshal(bytes.TrimSpace(out.Bytes()), &response); err != nil {
		t.Fatal(err)
	}
	if response.OK || response.Error != "malformed or unsupported request" {
		t.Fatalf("unexpected malformed response: %#v", response)
	}
}

func TestProtocolOutputRejectsOversizedFrame(t *testing.T) {
	output := &bytes.Buffer{}
	server := NewServer(strings.NewReader(""), output, time.Second)
	if err := server.write(strings.Repeat("x", maxFrameBytes)); err == nil {
		t.Fatal("oversized protocol output was accepted")
	}
	if output.Len() != 0 {
		t.Fatalf("partial oversized frame was written: %d bytes", output.Len())
	}
}
