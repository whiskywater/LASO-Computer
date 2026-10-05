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
