package client

import (
	"bytes"
	"context"
	"crypto/rand"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"regexp"
	"sync"
	"time"

	"github.com/Registered-Agent-Attorney/LASO-Computer/internal/capabilities"
)

const ProtocolVersion = 1

const (
	maxReplayRequests = 4096
	maxTrackedJobs    = 4096
	maxActiveJobs     = 64
	maxRetainedJobs   = 64
	keepCompletedJobs = 32
)

type Request struct {
	ProtocolVersion int             `json:"protocol_version"`
	RequestID       string          `json:"request_id"`
	Operation       string          `json:"operation"`
	JobID           string          `json:"job_id,omitempty"`
	ExternalJobID   string          `json:"external_job_id,omitempty"`
	Payload         json.RawMessage `json:"payload,omitempty"`
}

type Response struct {
	ProtocolVersion int             `json:"protocol_version"`
	RequestID       string          `json:"request_id"`
	OK              bool            `json:"ok"`
	State           string          `json:"state"`
	ExternalJobID   string          `json:"external_job_id,omitempty"`
	Payload         json.RawMessage `json:"payload,omitempty"`
	Metadata        map[string]any  `json:"metadata,omitempty"`
	Error           string          `json:"error,omitempty"`
}

type Hello struct {
	ClientID     string             `json:"client_id"`
	OS           string             `json:"os"`
	Architecture string             `json:"architecture"`
	Capabilities []CapabilityStatus `json:"capabilities"`
}

type CapabilityStatus struct {
	Name        string `json:"name"`
	Description string `json:"description"`
	Risk        string `json:"risk"`
	Available   bool   `json:"available"`
	Decision    string `json:"decision"`
}

type Replay struct {
	hash               [32]byte
	response           Response
	resultFromJobState bool
}
type Job struct {
	mu        sync.RWMutex
	ID        string
	LASOJobID string
	RequestID string
	Cancel    context.CancelFunc
	Done      chan struct{}
	State     string
	Payload   json.RawMessage
	Error     string
}

type Engine struct {
	handleMu    sync.Mutex
	mu          sync.Mutex
	root        context.Context
	executor    *capabilities.Executor
	hello       Hello
	jobs        map[string]*Job
	jobOrder    []string
	jobByLASOID map[string]string
	replay      map[string]Replay
	closed      bool
}

func NewEngine(root context.Context, executor *capabilities.Executor, hello Hello) (*Engine, error) {
	if root == nil || executor == nil {
		return nil, capabilities.ErrInvalid
	}
	return &Engine{root: root, executor: executor, hello: hello, jobs: map[string]*Job{}, jobByLASOID: map[string]string{}, replay: map[string]Replay{}}, nil
}

func DecodeRequest(data []byte) (Request, error) {
	var req Request
	dec := json.NewDecoder(bytes.NewReader(data))
	dec.DisallowUnknownFields()
	if err := dec.Decode(&req); err != nil {
		return Request{}, fmt.Errorf("malformed request")
	}
	if err := dec.Decode(new(any)); err != io.EOF {
		return Request{}, fmt.Errorf("malformed request")
	}
	if req.ProtocolVersion != ProtocolVersion || !wireID.MatchString(req.RequestID) || len(req.Operation) < 1 || len(req.Operation) > 32 {
		return Request{}, fmt.Errorf("invalid request envelope")
	}
	if len(req.Payload) > 1<<20 {
		return Request{}, fmt.Errorf("request exceeds size limit")
	}
	return req, nil
}

var wireID = regexp.MustCompile(`^[A-Za-z0-9._:/-]{1,128}$`)

func (e *Engine) Handle(data []byte) Response {
	e.handleMu.Lock()
	defer e.handleMu.Unlock()
	req, err := DecodeRequest(data)
	if err != nil {
		return Response{ProtocolVersion: ProtocolVersion, OK: false, State: "Failed", Error: "malformed or unsupported request"}
	}
	hash := sha256.Sum256(data)
	e.mu.Lock()
	if previous, ok := e.replay[req.RequestID]; ok {
		e.mu.Unlock()
		if previous.hash == hash {
			if previous.resultFromJobState {
				return e.inspect(req, true)
			}
			return previous.response
		}
		return fail(req.RequestID, "request id reused with different content")
	}
	if e.closed && req.Operation != "shutdown" {
		e.mu.Unlock()
		return fail(req.RequestID, "client is shutting down")
	}
	if len(e.replay) >= maxReplayRequests {
		e.mu.Unlock()
		return fail(req.RequestID, "request replay cache full")
	}
	e.mu.Unlock()
	response := e.handleUnique(req)
	entry := Replay{hash: hash, response: response}
	if req.Operation == "result" && len(response.Payload) > 0 {
		// The job owns the result bytes. Cache only its identity so repeated
		// result polls cannot pin an additional large screenshot in replay state.
		entry.response.Payload = nil
		entry.resultFromJobState = true
	}
	e.mu.Lock()
	e.replay[req.RequestID] = entry
	e.mu.Unlock()
	return response
}

func (e *Engine) handleUnique(req Request) Response {
	switch req.Operation {
	case "hello":
		payload, err := json.Marshal(e.hello)
		if err != nil {
			return fail(req.RequestID, "capability discovery failed")
		}
		return ok(req.RequestID, "Completed", payload, "")
	case "submit":
		return e.submit(req)
	case "status":
		return e.inspect(req, false)
	case "result":
		return e.inspect(req, true)
	case "cancel":
		return e.cancel(req)
	case "shutdown":
		return e.shutdown(req)
	default:
		return fail(req.RequestID, "unknown operation")
	}
}

type toolPayload struct {
	Capability string                 `json:"capability"`
	Arguments  json.RawMessage        `json:"arguments"`
	Principal  capabilities.Principal `json:"principal"`
}

func (e *Engine) submit(req Request) Response {
	if !wireID.MatchString(req.JobID) {
		return fail(req.RequestID, "invalid job id")
	}
	var payload toolPayload
	dec := json.NewDecoder(bytes.NewReader(req.Payload))
	dec.DisallowUnknownFields()
	if len(req.Payload) == 0 || dec.Decode(&payload) != nil || payload.Capability == "" {
		return fail(req.RequestID, "invalid capability request")
	}
	if dec.Decode(new(any)) != io.EOF {
		return fail(req.RequestID, "invalid capability request")
	}
	if payload.Principal.JobID != "" && payload.Principal.JobID != req.JobID {
		return fail(req.RequestID, "job identity mismatch")
	}
	payload.Principal.JobID = req.JobID
	if len(payload.Arguments) > 1<<20 {
		return fail(req.RequestID, "request exceeds size limit")
	}

	e.mu.Lock()
	if e.closed {
		e.mu.Unlock()
		return fail(req.RequestID, "client is shutting down")
	}
	if _, exists := e.jobByLASOID[req.JobID]; exists {
		e.mu.Unlock()
		return fail(req.RequestID, "job id already submitted")
	}
	e.pruneJobsLocked()
	active := 0
	for _, existing := range e.jobs {
		existing.mu.RLock()
		running := existing.State == "Running"
		existing.mu.RUnlock()
		if running {
			active++
		}
	}
	if active >= maxActiveJobs {
		e.mu.Unlock()
		return fail(req.RequestID, "active job limit reached")
	}
	if len(e.jobByLASOID) >= maxTrackedJobs {
		e.mu.Unlock()
		return fail(req.RequestID, "job replay cache full")
	}
	if len(e.jobs) >= maxRetainedJobs {
		e.mu.Unlock()
		return fail(req.RequestID, "completed job retention limit reached")
	}
	ctx, cancel := context.WithCancel(e.root)
	externalID, err := randomID()
	if err != nil {
		cancel()
		e.mu.Unlock()
		return fail(req.RequestID, "client could not allocate job")
	}
	job := &Job{ID: externalID, LASOJobID: req.JobID, RequestID: req.RequestID, Cancel: cancel, Done: make(chan struct{}), State: "Running"}
	e.jobs[externalID] = job
	e.jobOrder = append(e.jobOrder, externalID)
	e.jobByLASOID[req.JobID] = externalID
	e.mu.Unlock()
	payload.Principal.ExternalJobID = externalID
	go e.run(ctx, job, capabilities.Invocation{RequestID: req.RequestID, Capability: payload.Capability, Arguments: payload.Arguments, Principal: payload.Principal})
	return Response{ProtocolVersion: ProtocolVersion, RequestID: req.RequestID, OK: true, State: "Running", ExternalJobID: externalID, Metadata: map[string]any{"job_id": req.JobID}}
}

// pruneJobsLocked retains recent results for status/result polling while
// keeping completed output memory bounded. LASO job IDs remain as tombstones
// for this process lifetime so an evicted result cannot be submitted again.
func (e *Engine) pruneJobsLocked() {
	if len(e.jobs) < maxRetainedJobs {
		return
	}
	kept := e.jobOrder[:0]
	for _, id := range e.jobOrder {
		job := e.jobs[id]
		if job == nil {
			continue
		}
		job.mu.RLock()
		completed := job.State != "Running"
		job.mu.RUnlock()
		if completed && len(e.jobs) >= keepCompletedJobs {
			delete(e.jobs, id)
			continue
		}
		kept = append(kept, id)
	}
	e.jobOrder = kept
}

func (e *Engine) run(ctx context.Context, job *Job, call capabilities.Invocation) {
	result, err := e.executor.Invoke(ctx, call)
	job.mu.Lock()
	if err == nil {
		job.State = "Completed"
		job.Payload = result
	} else if errors.Is(err, context.Canceled) {
		job.State = "Cancelled"
		job.Error = "cancelled"
	} else if errors.Is(err, context.DeadlineExceeded) {
		job.State = "TimedOut"
		job.Error = "timed out"
	} else {
		job.State = "Failed"
		job.Error = publicError(err)
	}
	job.mu.Unlock()
	close(job.Done)
}

func publicError(err error) string {
	switch {
	case errors.Is(err, capabilities.ErrDenied):
		return "capability denied"
	case errors.Is(err, capabilities.ErrUnknown):
		return "unknown capability"
	case errors.Is(err, capabilities.ErrApproval):
		return "approval denied or unavailable"
	case errors.Is(err, capabilities.ErrUnavailable):
		return "capability unavailable"
	case errors.Is(err, capabilities.ErrInvalid):
		return "invalid capability request"
	default:
		return "capability failed"
	}
}

func (e *Engine) lookup(req Request) (*Job, Response, bool) {
	if req.ExternalJobID == "" || len(req.ExternalJobID) > 128 {
		return nil, fail(req.RequestID, "invalid external job id"), false
	}
	e.mu.Lock()
	job := e.jobs[req.ExternalJobID]
	e.mu.Unlock()
	if job == nil {
		return nil, fail(req.RequestID, "job not found"), false
	}
	return job, Response{}, true
}

func (e *Engine) inspect(req Request, result bool) Response {
	job, response, ok := e.lookup(req)
	if !ok {
		return response
	}
	job.mu.RLock()
	state, payload, errText := job.State, job.Payload, job.Error
	job.mu.RUnlock()
	if !result {
		return Response{ProtocolVersion: ProtocolVersion, RequestID: req.RequestID, OK: true, State: state, ExternalJobID: job.ID}
	}
	if state == "Running" {
		return Response{ProtocolVersion: ProtocolVersion, RequestID: req.RequestID, OK: true, State: state, ExternalJobID: job.ID}
	}
	out := Response{ProtocolVersion: ProtocolVersion, RequestID: req.RequestID, OK: state == "Completed", State: state, ExternalJobID: job.ID, Payload: payload, Error: errText}
	return out
}

func (e *Engine) cancel(req Request) Response {
	job, response, ok := e.lookup(req)
	if !ok {
		return response
	}
	job.Cancel()
	select {
	case <-job.Done:
	case <-time.After(2 * time.Second):
	}
	job.mu.RLock()
	state := job.State
	job.mu.RUnlock()
	return Response{ProtocolVersion: ProtocolVersion, RequestID: req.RequestID, OK: true, State: state, ExternalJobID: job.ID, Payload: json.RawMessage(`{"cancellation_requested":true}`)}
}

func (e *Engine) shutdown(req Request) Response {
	e.mu.Lock()
	e.closed = true
	jobs := make([]*Job, 0, len(e.jobs))
	for _, job := range e.jobs {
		jobs = append(jobs, job)
		job.Cancel()
	}
	e.mu.Unlock()
	deadline := time.NewTimer(3 * time.Second)
	defer deadline.Stop()
	for _, job := range jobs {
		select {
		case <-job.Done:
		case <-deadline.C:
			return fail(req.RequestID, "shutdown timed out while actions were stopping")
		}
	}
	return ok(req.RequestID, "Completed", json.RawMessage(`{"stopped":true}`), "")
}

func (e *Engine) Close() {
	e.mu.Lock()
	e.closed = true
	for _, job := range e.jobs {
		job.Cancel()
	}
	e.mu.Unlock()
}

func ok(id, state string, payload json.RawMessage, external string) Response {
	return Response{ProtocolVersion: ProtocolVersion, RequestID: id, OK: true, State: state, Payload: payload, ExternalJobID: external}
}
func fail(id, message string) Response {
	return Response{ProtocolVersion: ProtocolVersion, RequestID: id, OK: false, State: "Failed", Error: message}
}

func randomID() (string, error) {
	var raw [16]byte
	if _, err := rand.Read(raw[:]); err != nil {
		return "", err
	}
	return hex.EncodeToString(raw[:]), nil
}
