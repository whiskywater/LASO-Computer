package capabilities

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"regexp"
	"strings"
	"time"

	"github.com/Registered-Agent-Attorney/LASO-Computer/internal/audit"
	"github.com/Registered-Agent-Attorney/LASO-Computer/internal/policy"
)

var (
	ErrDenied      = errors.New("capability denied")
	ErrUnknown     = errors.New("unknown capability")
	ErrApproval    = errors.New("approval unavailable or denied")
	ErrUnavailable = errors.New("capability unavailable on this endpoint")
	ErrInvalid     = errors.New("invalid capability request")
	ErrFailed      = errors.New("capability failed")
)

type Principal struct {
	AgentID       string `json:"agent_id,omitempty"`
	SessionID     string `json:"session_id,omitempty"`
	JobID         string `json:"job_id,omitempty"`
	ExternalJobID string `json:"-"`
}

type Invocation struct {
	RequestID  string          `json:"request_id"`
	Capability string          `json:"capability"`
	Arguments  json.RawMessage `json:"arguments"`
	Principal  Principal       `json:"principal"`
}

type Approval func(context.Context, string, Principal) (bool, error)
type Handler func(context.Context, json.RawMessage) (json.RawMessage, error)

type Registry struct{ handlers map[string]Handler }

func NewRegistry() *Registry { return &Registry{handlers: make(map[string]Handler)} }

func (r *Registry) Register(name string, handler Handler) error {
	if !Known(name) || handler == nil {
		return ErrInvalid
	}
	if _, exists := r.handlers[name]; exists {
		return fmt.Errorf("capability already registered")
	}
	r.handlers[name] = handler
	return nil
}

func (r *Registry) Names() []string {
	result := make([]string, 0, len(r.handlers))
	for name := range r.handlers {
		result = append(result, name)
	}
	sortStrings(result)
	return result
}

type Authorizer interface{ Decide(string) policy.Decision }
type Auditor interface{ Record(audit.Event) }

type Executor struct {
	registry  *Registry
	policy    Authorizer
	available map[string]bool
	audit     Auditor
	approve   Approval
}

func NewExecutor(registry *Registry, p Authorizer, available map[string]bool, writer Auditor, approval Approval) (*Executor, error) {
	if registry == nil || p == nil {
		return nil, ErrInvalid
	}
	return &Executor{registry: registry, policy: p, available: available, audit: writer, approve: approval}, nil
}

var principalPattern = regexp.MustCompile(`^[A-Za-z0-9._:@/-]{1,128}$`)

func (e *Executor) Invoke(ctx context.Context, call Invocation) (json.RawMessage, error) {
	if len(call.RequestID) < 1 || len(call.RequestID) > 128 || strings.ContainsAny(call.RequestID, "\r\n\x00") || len(call.Capability) < 1 || len(call.Capability) > 128 {
		return nil, ErrInvalid
	}
	for _, id := range []string{call.Principal.AgentID, call.Principal.SessionID, call.Principal.JobID, call.Principal.ExternalJobID} {
		if id != "" && !principalPattern.MatchString(id) {
			return nil, ErrInvalid
		}
	}
	handler, ok := e.registry.handlers[call.Capability]
	if !ok {
		e.record(audit.Event{RequestID: call.RequestID, Capability: "unknown", Decision: "deny", Outcome: "unknown_capability", JobID: call.Principal.JobID, SessionID: call.Principal.SessionID, AgentID: call.Principal.AgentID})
		return nil, ErrUnknown
	}
	decision := e.policy.Decide(call.Capability)
	base := audit.Event{RequestID: call.RequestID, JobID: call.Principal.JobID, SessionID: call.Principal.SessionID, AgentID: call.Principal.AgentID, Capability: call.Capability, Decision: string(decision), Outcome: "denied"}
	if decision == policy.Deny {
		e.record(base)
		return nil, ErrDenied
	}
	if decision == policy.RequireApproval {
		if e.approve == nil {
			e.record(base)
			return nil, ErrApproval
		}
		approved, err := e.approve(ctx, call.Capability, call.Principal)
		if err != nil || !approved {
			base.Outcome = "approval_denied"
			e.record(base)
			return nil, ErrApproval
		}
		base.Outcome = "approved"
		e.record(base)
	}
	if !e.available[call.Capability] {
		base.Outcome = "unavailable"
		e.record(base)
		return nil, ErrUnavailable
	}
	if err := ctx.Err(); err != nil {
		base.Outcome = "cancelled"
		e.record(base)
		return nil, err
	}
	base.Outcome = "started"
	started := time.Now()
	e.record(base)
	result, err := handler(ctx, call.Arguments)
	base.DurationMS = time.Since(started).Milliseconds()
	switch {
	case errors.Is(err, context.Canceled), errors.Is(err, context.DeadlineExceeded):
		base.Outcome = "cancelled"
	case err != nil:
		base.Outcome = "failed"
	default:
		base.Outcome = "completed"
	}
	e.record(base)
	if err != nil {
		return nil, err
	}
	return result, nil
}

func (e *Executor) record(event audit.Event) {
	if e.audit != nil {
		e.audit.Record(event)
	}
}

func sortStrings(values []string) {
	for i := 1; i < len(values); i++ {
		for j := i; j > 0 && values[j] < values[j-1]; j-- {
			values[j], values[j-1] = values[j-1], values[j]
		}
	}
}
