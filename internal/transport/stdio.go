package transport

import (
	"bufio"
	"bytes"
	"context"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"regexp"
	"strings"
	"sync"
	"time"

	"github.com/Registered-Agent-Attorney/LASO-Computer/internal/capabilities"
	"github.com/Registered-Agent-Attorney/LASO-Computer/internal/client"
)

const maxFrameBytes = 1 << 20

type workerResponse struct {
	ProtocolVersion int             `json:"protocol_version"`
	MessageType     string          `json:"message_type"`
	RequestID       string          `json:"request_id"`
	Decision        string          `json:"decision"`
	Payload         json.RawMessage `json:"payload,omitempty"`
	Reason          string          `json:"reason,omitempty"`
}

type workerRequest struct {
	ProtocolVersion int               `json:"protocol_version"`
	MessageType     string            `json:"message_type"`
	RequestID       string            `json:"request_id"`
	WorkerJobID     string            `json:"worker_job_id"`
	WorkerID        string            `json:"worker_id"`
	ExternalJobID   string            `json:"external_job_id"`
	SessionID       string            `json:"session_id,omitempty"`
	RequestType     string            `json:"request_type"`
	Title           string            `json:"title"`
	Summary         string            `json:"summary"`
	Payload         map[string]string `json:"payload"`
	CreatedAt       string            `json:"created_at"`
	Deadline        string            `json:"deadline"`
	Risk            string            `json:"risk"`
	Category        string            `json:"category"`
}

type Server struct {
	input           io.Reader
	output          io.Writer
	approvalTimeout time.Duration
	writeMu         sync.Mutex
	pendingMu       sync.Mutex
	pending         map[string]chan workerResponse
	closed          bool
}

func NewServer(input io.Reader, output io.Writer, timeout time.Duration) *Server {
	return &Server{input: input, output: output, approvalTimeout: timeout, pending: map[string]chan workerResponse{}}
}

func (s *Server) write(value any) error {
	s.writeMu.Lock()
	defer s.writeMu.Unlock()
	enc := json.NewEncoder(s.output)
	return enc.Encode(value)
}

var safeID = regexp.MustCompile(`^[A-Za-z0-9._:/-]{1,128}$`)

func (s *Server) RequestApproval(ctx context.Context, capability string, principal capabilities.Principal) (bool, error) {
	var idBytes [16]byte
	if _, err := rand.Read(idBytes[:]); err != nil {
		return false, errors.New("approval unavailable")
	}
	id := "perm-" + hex.EncodeToString(idBytes[:])
	timeout := s.approvalTimeout
	if timeout <= 0 || timeout > 10*time.Minute {
		timeout = 60 * time.Second
	}
	deadline := time.Now().Add(timeout)
	response := make(chan workerResponse, 1)
	s.pendingMu.Lock()
	if s.closed {
		s.pendingMu.Unlock()
		return false, errors.New("approval connection closed")
	}
	s.pending[id] = response
	s.pendingMu.Unlock()
	defer func() { s.pendingMu.Lock(); delete(s.pending, id); s.pendingMu.Unlock() }()
	request := workerRequest{ProtocolVersion: client.ProtocolVersion, MessageType: "worker_request", RequestID: id, WorkerJobID: principal.JobID, WorkerID: "laso-computer", ExternalJobID: principal.ExternalJobID, SessionID: principal.SessionID, RequestType: "permission", Title: "Local computer capability request", Summary: "Request local permission to use " + capability, Payload: map[string]string{"resource": capability}, CreatedAt: time.Now().UTC().Format(time.RFC3339Nano), Deadline: deadline.UTC().Format(time.RFC3339Nano), Risk: riskFor(capability), Category: "tool"}
	if err := s.write(request); err != nil {
		return false, errors.New("approval connection failed")
	}
	timer := time.NewTimer(time.Until(deadline))
	defer timer.Stop()
	select {
	case answer := <-response:
		return answer.Decision == "approved", nil
	case <-ctx.Done():
		return false, ctx.Err()
	case <-timer.C:
		return false, errors.New("approval timed out")
	}
}

func riskFor(capability string) string {
	if capability == "shell.execute" {
		return "high"
	}
	if strings.HasPrefix(capability, "clipboard.") || capability == "screen.capture" || capability == "keyboard.type" {
		return "high"
	}
	return "medium"
}

func (s *Server) receiveApproval(data []byte) bool {
	var header struct {
		MessageType string `json:"message_type"`
	}
	if json.Unmarshal(data, &header) != nil || header.MessageType != "worker_response" {
		return false
	}
	var reply workerResponse
	dec := json.NewDecoder(bytes.NewReader(data))
	dec.DisallowUnknownFields()
	if dec.Decode(&reply) != nil || reply.ProtocolVersion != client.ProtocolVersion || reply.MessageType != "worker_response" || !safeID.MatchString(reply.RequestID) || len(reply.Reason) > 4096 {
		return true
	}
	if reply.Decision != "approved" && reply.Decision != "denied" && reply.Decision != "cancelled" && reply.Decision != "expired" {
		return true
	}
	s.pendingMu.Lock()
	ch := s.pending[reply.RequestID]
	s.pendingMu.Unlock()
	if ch != nil {
		select {
		case ch <- reply:
		default:
		}
	}
	return true
}

func (s *Server) Serve(ctx context.Context, engine *client.Engine) error {
	if engine == nil {
		return fmt.Errorf("client engine unavailable")
	}
	lines := make(chan []byte, 1)
	scanErr := make(chan error, 1)
	go func() {
		scanner := bufio.NewScanner(s.input)
		scanner.Buffer(make([]byte, 32*1024), maxFrameBytes)
		for scanner.Scan() {
			line := append([]byte(nil), scanner.Bytes()...)
			select {
			case lines <- line:
			case <-ctx.Done():
				scanErr <- nil
				return
			}
		}
		scanErr <- scanner.Err()
		close(lines)
	}()
	defer func() {
		s.pendingMu.Lock()
		s.closed = true
		for id, ch := range s.pending {
			delete(s.pending, id)
			close(ch)
		}
		s.pendingMu.Unlock()
		engine.Close()
	}()
	for {
		select {
		case <-ctx.Done():
			return nil
		case data, open := <-lines:
			if !open {
				if err := <-scanErr; err != nil {
					return fmt.Errorf("protocol input failed or exceeded frame limit")
				}
				return nil
			}
			if s.receiveApproval(data) {
				continue
			}
			var request client.Request
			if json.Unmarshal(data, &request) == nil && request.Operation == "shutdown" && request.ProtocolVersion == client.ProtocolVersion {
				response := engine.Handle(data)
				if err := s.write(response); err != nil {
					return fmt.Errorf("protocol output failed")
				}
				if response.OK {
					return nil
				}
				continue
			}
			response := engine.Handle(data)
			if err := s.write(response); err != nil {
				return fmt.Errorf("protocol output failed")
			}
		}
	}
}
