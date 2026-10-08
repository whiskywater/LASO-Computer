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
	"github.com/Registered-Agent-Attorney/LASO-Computer/internal/catalog"
)

const ProtocolVersion = 1

const (
	maxReplayRequests = 4096
	maxTrackedJobs    = 4096
	maxActiveJobs     = 64
	maxRetainedJobs   = 64
	keepCompletedJobs = 32
	maxResultBytes    = (1 << 20) - 4096
	maxJobDuration    = 24 * time.Hour
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
	Name        string          `json:"name"`
	Description string          `json:"description"`
	Risk        string          `json:"risk"`
	Profile     string          `json:"profile,omitempty"`
	InputSchema json.RawMessage `json:"input_schema,omitempty"`
	Available   bool            `json:"available"`
	Decision    string          `json:"decision"`
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
	if err := ValidateUniqueJSONKeys(data); err != nil {
		return Request{}, fmt.Errorf("malformed request")
	}
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

// ValidateUniqueJSONKeys rejects duplicate object keys at every nesting level.
// encoding/json otherwise accepts duplicates and silently keeps the last value,
// which can make policy and protocol validation disagree across implementations.
func ValidateUniqueJSONKeys(data []byte) error {
	dec := json.NewDecoder(bytes.NewReader(data))
	dec.UseNumber()
	var value func() error
	value = func() error {
		tok, err := dec.Token()
		if err != nil {
			return err
		}
		delim, ok := tok.(json.Delim)
		if !ok {
			return nil
		}
		switch delim {
		case '{':
			keys := make(map[string]struct{})
			for dec.More() {
				keyToken, err := dec.Token()
				if err != nil {
					return err
				}
				key, ok := keyToken.(string)
				if !ok {
					return fmt.Errorf("invalid JSON object key")
				}
				if _, exists := keys[key]; exists {
					return fmt.Errorf("duplicate JSON object key")
				}
				keys[key] = struct{}{}
				if err := value(); err != nil {
					return err
				}
			}
			end, err := dec.Token()
			if err != nil || end != json.Delim('}') {
				return fmt.Errorf("malformed JSON object")
			}
		case '[':
			for dec.More() {
				if err := value(); err != nil {
					return err
				}
			}
			end, err := dec.Token()
			if err != nil || end != json.Delim(']') {
				return fmt.Errorf("malformed JSON array")
			}
		default:
			return fmt.Errorf("unexpected JSON delimiter")
		}
		return nil
	}
	if err := value(); err != nil {
		return err
	}
	if _, err := dec.Token(); err != io.EOF {
		if err == nil {
			return fmt.Errorf("trailing JSON value")
		}
		return err
	}
	return nil
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
		capabilities := make([]string, 0, len(e.hello.Capabilities))
		for _, capability := range e.hello.Capabilities {
			if capability.Available {
				capabilities = append(capabilities, capability.Name)
			}
		}
		metadata := map[string]any{
			"id":                    "laso-computer",
			"name":                  "LASO Computer",
			"version":               "1",
			"description":           "Local computer-use worker with user-controlled capability policy",
			"capabilities":          capabilities,
			"capability_profiles":   catalog.ProfileDescriptors(),
			"supports_status":       true,
			"supports_recovery":     false,
			"supports_cancellation": true,
			"computer": map[string]any{
				"client_id":         e.hello.ClientID,
				"os":                e.hello.OS,
				"architecture":      e.hello.Architecture,
				"capability_status": e.hello.Capabilities,
			},
		}
		return Response{ProtocolVersion: ProtocolVersion, RequestID: req.RequestID, OK: true,
			State: "Completed", Payload: payload, Metadata: metadata}
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

// toolPayload accepts both the original Computer v1 payload and the current
// LASO WorkerRequest envelope. Keeping both shapes lets already-deployed local
// process integrations continue to work while the Core envelope is validated
// and translated into the same guarded local capability invocation.
type toolPayload struct {
	JobID            string                 `json:"job_id,omitempty"`
	WorkerID         string                 `json:"worker_id,omitempty"`
	Capability       string                 `json:"capability"`
	TaskType         string                 `json:"task_type,omitempty"`
	Instructions     string                 `json:"instructions,omitempty"`
	Deadline         string                 `json:"deadline,omitempty"`
	IdempotencyKey   string                 `json:"idempotency_key,omitempty"`
	RunID            string                 `json:"run_id,omitempty"`
	NodeID           string                 `json:"node_id,omitempty"`
	Attempt          uint                   `json:"attempt,omitempty"`
	TimeoutMS        uint64                 `json:"timeout_ms,omitempty"`
	Input            json.RawMessage        `json:"input,omitempty"`
	OutputSchema     json.RawMessage        `json:"output_schema,omitempty"`
	Metadata         json.RawMessage        `json:"metadata,omitempty"`
	ArtifactIDs      []string               `json:"artifact_ids,omitempty"`
	DurableSession   bool                   `json:"durable_session,omitempty"`
	DurableSessionID string                 `json:"durable_session_id,omitempty"`
	Continuation     json.RawMessage        `json:"continuation,omitempty"`
	SessionContext   json.RawMessage        `json:"session_context,omitempty"`
	Arguments        json.RawMessage        `json:"arguments,omitempty"`
	Principal        capabilities.Principal `json:"principal,omitempty"`
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
	if payload.JobID != "" && payload.JobID != req.JobID {
		return fail(req.RequestID, "job identity mismatch")
	}
	if payload.Principal.JobID != "" && payload.Principal.JobID != req.JobID {
		return fail(req.RequestID, "job identity mismatch")
	}
	if len(payload.Arguments) > 0 && len(payload.Input) > 0 {
		return fail(req.RequestID, "capability arguments are ambiguous")
	}
	if len(payload.Instructions) > 1<<20 || len(payload.Deadline) > 128 ||
		len(payload.IdempotencyKey) > 128 || len(payload.RunID) > 128 || len(payload.NodeID) > 128 ||
		len(payload.WorkerID) > 128 || len(payload.TaskType) > 128 || len(payload.DurableSessionID) > 128 ||
		len(payload.OutputSchema) > 64*1024 || len(payload.Metadata) > 64*1024 ||
		len(payload.Continuation) > 64*1024 || len(payload.SessionContext) > 1<<20 ||
		len(payload.ArtifactIDs) > 256 {
		return fail(req.RequestID, "capability request exceeds size limit")
	}
	for _, artifactID := range payload.ArtifactIDs {
		if len(artifactID) == 0 || len(artifactID) > 128 || !wireID.MatchString(artifactID) {
			return fail(req.RequestID, "invalid capability request")
		}
	}
	if payload.TimeoutMS > uint64(maxJobDuration/time.Millisecond) {
		return fail(req.RequestID, "capability timeout exceeds limit")
	}
	payload.Principal.JobID = req.JobID
	if payload.Principal.SessionID == "" {
		payload.Principal.SessionID = payload.DurableSessionID
	}
	arguments := payload.Arguments
	if len(arguments) == 0 && len(payload.Input) > 0 {
		// Core's input is the task context, which can contain a prompt unrelated
		// to the capability's typed arguments. These read-only status probes take
		// no arguments, so do not pass task text into their strict empty schemas.
		switch payload.Capability {
		case "browser.status", "window.list":
			arguments = json.RawMessage(`{}`)
		default:
			arguments = payload.Input
		}
	}
	if len(payload.Arguments) > 1<<20 {
		return fail(req.RequestID, "request exceeds size limit")
	}
	if len(arguments) > 1<<20 {
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
	var ctx context.Context
	var cancel context.CancelFunc
	if payload.TimeoutMS == 0 {
		ctx, cancel = context.WithCancel(e.root)
	} else {
		ctx, cancel = context.WithTimeout(e.root, time.Duration(payload.TimeoutMS)*time.Millisecond)
	}
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
	go e.run(ctx, job, capabilities.Invocation{RequestID: req.RequestID, Capability: payload.Capability, Arguments: arguments, Principal: payload.Principal})
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
	// A handler can return a value after ignoring cooperative cancellation. Such
	// a value no longer belongs to a live action and must not be exposed as a
	// successful completion.
	if ctx.Err() != nil {
		result = nil
		err = ctx.Err()
	}
	if err == nil && len(result) > maxResultBytes {
		result = nil
		err = errors.New("result exceeds size limit")
	}
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
		unknown := Response{ProtocolVersion: ProtocolVersion, RequestID: req.RequestID, OK: true,
			State: "Unknown", ExternalJobID: req.ExternalJobID, Error: "job not found"}
		if req.Operation == "cancel" {
			unknown.Payload = json.RawMessage(`{"acknowledged":false}`)
		}
		return nil, unknown, false
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
	out := Response{ProtocolVersion: ProtocolVersion, RequestID: req.RequestID, OK: true, State: state, ExternalJobID: job.ID, Payload: payload, Error: errText}
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
	return Response{ProtocolVersion: ProtocolVersion, RequestID: req.RequestID, OK: true, State: state,
		ExternalJobID: job.ID, Payload: json.RawMessage(fmt.Sprintf(`{"acknowledged":%t}`, state == "Cancelled"))}
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

// CloseIfIdle prevents new requests only after all submitted capability jobs
// have finished. It deliberately does not cancel active work, allowing a
// supervisor to reject a restart request without interrupting a capability.
func (e *Engine) CloseIfIdle() bool {
	e.handleMu.Lock()
	defer e.handleMu.Unlock()
	e.mu.Lock()
	defer e.mu.Unlock()
	if e.closed {
		return false
	}
	for _, job := range e.jobs {
		job.mu.RLock()
		running := job.State == "Running"
		job.mu.RUnlock()
		if running {
			return false
		}
	}
	e.closed = true
	return true
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
