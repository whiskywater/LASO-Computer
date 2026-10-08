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
	protocolError   bool
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
	data, err := json.Marshal(value)
	if err != nil {
		return fmt.Errorf("protocol output could not be encoded")
	}
	if len(data) > maxFrameBytes {
		return fmt.Errorf("protocol output exceeds frame limit")
	}
	s.writeMu.Lock()
	defer s.writeMu.Unlock()
	data = append(data, '\n')
	written, err := s.output.Write(data)
	if err != nil {
		return fmt.Errorf("protocol output failed")
	}
	if written != len(data) {
		return io.ErrShortWrite
	}
	return nil
}

var safeID = regexp.MustCompile(`^[A-Za-z0-9._:/-]{1,128}$`)

func (s *Server) RequestApproval(ctx context.Context, capability string, principal capabilities.Principal) (bool, error) {
	answer, err := s.RequestInteraction(ctx, "permission", "Local computer capability request", "Request local permission to use "+capability,
		map[string]string{"resource": capability}, principal, riskFor(capability), "tool")
	if err != nil {
		return false, err
	}
	return answer.Decision == "approved", nil
}

type InteractionResponse struct {
	Decision string          `json:"decision"`
	Payload  json.RawMessage `json:"payload,omitempty"`
	Reason   string          `json:"reason,omitempty"`
}

func (s *Server) RequestInteraction(ctx context.Context, requestType, title, summary string, payload map[string]string,
	principal capabilities.Principal, risk, category string) (InteractionResponse, error) {
	if requestType != "approval" && requestType != "permission" && requestType != "question" ||
		len(title) == 0 || len(title) > 256 || len(summary) == 0 || len(summary) > 2048 ||
		len(payload) > 16 || (risk != "low" && risk != "medium" && risk != "high") || len(category) == 0 || len(category) > 64 {
		return InteractionResponse{}, errors.New("invalid worker interaction")
	}
	bytes := 0
	for key, value := range payload {
		bytes += len(key) + len(value)
		if len(key) == 0 || len(key) > 64 || len(value) > 4096 || strings.ContainsAny(key+value, "\x00\r\n") || bytes > 16384 {
			return InteractionResponse{}, errors.New("invalid worker interaction")
		}
	}
	for _, id := range []string{principal.JobID, principal.SessionID, principal.ExternalJobID} {
		if id != "" && !safeID.MatchString(id) {
			return InteractionResponse{}, errors.New("invalid worker interaction identity")
		}
	}
	var idBytes [16]byte
	if _, err := rand.Read(idBytes[:]); err != nil {
		return InteractionResponse{}, errors.New("interaction unavailable")
	}
	id := "interaction-" + hex.EncodeToString(idBytes[:])
	timeout := s.approvalTimeout
	if timeout <= 0 || timeout > 10*time.Minute {
		timeout = 60 * time.Second
	}
	deadline := time.Now().Add(timeout)
	response := make(chan workerResponse, 1)
	s.pendingMu.Lock()
	if s.closed {
		s.pendingMu.Unlock()
		return InteractionResponse{}, errors.New("approval connection closed")
	}
	if len(s.pending) >= 64 {
		s.pendingMu.Unlock()
		return InteractionResponse{}, errors.New("too many pending worker interactions")
	}
	s.pending[id] = response
	s.pendingMu.Unlock()
	defer func() { s.pendingMu.Lock(); delete(s.pending, id); s.pendingMu.Unlock() }()
	request := workerRequest{ProtocolVersion: client.ProtocolVersion, MessageType: "worker_request", RequestID: id, WorkerJobID: principal.JobID, WorkerID: "laso-computer", ExternalJobID: principal.ExternalJobID, SessionID: principal.SessionID, RequestType: requestType, Title: title, Summary: summary, Payload: payload, CreatedAt: time.Now().UTC().Format(time.RFC3339Nano), Deadline: deadline.UTC().Format(time.RFC3339Nano), Risk: risk, Category: category}
	if err := s.write(request); err != nil {
		return InteractionResponse{}, errors.New("interaction connection failed")
	}
	timer := time.NewTimer(time.Until(deadline))
	defer timer.Stop()
	select {
	case answer := <-response:
		if answer.protocolError {
			return InteractionResponse{}, errors.New("malformed interaction response")
		}
		if requestType == "question" {
			if answer.Decision != "answered" || len(answer.Payload) == 0 || len(answer.Payload) > 16*1024 || !json.Valid(answer.Payload) {
				return InteractionResponse{}, errors.New("question response missing or invalid")
			}
		} else if answer.Decision != "approved" && answer.Decision != "denied" && answer.Decision != "cancelled" && answer.Decision != "expired" {
			return InteractionResponse{}, errors.New("permission response missing or invalid")
		}
		return InteractionResponse{Decision: answer.Decision, Payload: append(json.RawMessage(nil), answer.Payload...), Reason: answer.Reason}, nil
	case <-ctx.Done():
		return InteractionResponse{}, ctx.Err()
	case <-timer.C:
		return InteractionResponse{}, errors.New("interaction timed out")
	}
}

func riskFor(capability string) string {
	if capability == "shell.execute" {
		return "high"
	}
	if strings.HasPrefix(capability, "clipboard.") || capability == "screen.capture" || capability == "keyboard.type" || capability == "ui.invoke" {
		return "high"
	}
	return "medium"
}

func (s *Server) receiveApproval(data []byte) bool {
	var header struct {
		MessageType string `json:"message_type"`
	}
	headerErr := json.Unmarshal(data, &header)
	if header.MessageType == "worker_response" {
		if err := client.ValidateUniqueJSONKeys(data); err != nil {
			s.failAllPendingInteractions()
			return true
		}
	} else if bytes.Contains(data, []byte(`"message_type":"worker_response"`)) {
		s.failPendingInteraction(data)
		return true
	}
	if headerErr != nil {
		if bytes.Contains(data, []byte(`"message_type":"worker_response"`)) {
			s.failPendingInteraction(data)
			return true
		}
		return false
	}
	if header.MessageType != "worker_response" {
		return false
	}
	var reply workerResponse
	dec := json.NewDecoder(bytes.NewReader(data))
	dec.DisallowUnknownFields()
	if dec.Decode(&reply) != nil || dec.Decode(new(any)) != io.EOF || reply.ProtocolVersion != client.ProtocolVersion || reply.MessageType != "worker_response" || !safeID.MatchString(reply.RequestID) || len(reply.Reason) > 4096 {
		s.failPendingInteraction(data)
		return true
	}
	if reply.Decision != "approved" && reply.Decision != "denied" && reply.Decision != "cancelled" && reply.Decision != "expired" && reply.Decision != "answered" {
		s.failPendingInteraction(data)
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

func (s *Server) failAllPendingInteractions() {
	s.pendingMu.Lock()
	defer s.pendingMu.Unlock()
	for id, ch := range s.pending {
		select {
		case ch <- workerResponse{RequestID: id, protocolError: true}:
		default:
		}
	}
}

func (s *Server) failPendingInteraction(data []byte) {
	var correlation struct {
		RequestID string `json:"request_id"`
	}
	_ = json.Unmarshal(data, &correlation)
	s.pendingMu.Lock()
	defer s.pendingMu.Unlock()
	if safeID.MatchString(correlation.RequestID) {
		if ch := s.pending[correlation.RequestID]; ch != nil {
			select {
			case ch <- workerResponse{RequestID: correlation.RequestID, protocolError: true}:
			default:
			}
		}
		return
	}
	// Without a correlation id it is unsafe to associate the malformed frame
	// with one interaction. Fail all outstanding interactions closed.
	for id, ch := range s.pending {
		select {
		case ch <- workerResponse{RequestID: id, protocolError: true}:
		default:
		}
	}
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
			select {
			case ch <- workerResponse{RequestID: id, protocolError: true}:
			default:
			}
			delete(s.pending, id)
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
