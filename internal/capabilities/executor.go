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
	"github.com/Registered-Agent-Attorney/LASO-Computer/internal/catalog"
	"github.com/Registered-Agent-Attorney/LASO-Computer/internal/policy"
)

var (
	ErrDenied      = errors.New("capability denied")
	ErrUnknown     = errors.New("unknown capability")
	ErrApproval    = errors.New("approval unavailable or denied")
	ErrUnavailable = errors.New("capability unavailable on this endpoint")
	ErrInvalid     = errors.New("invalid capability request")
	ErrFailed      = errors.New("capability failed")
	ErrAudit       = errors.New("capability audit unavailable")
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
		if err := e.record(audit.Event{RequestID: call.RequestID, Capability: "unknown", Decision: "deny", Outcome: "unknown_capability", JobID: call.Principal.JobID, SessionID: call.Principal.SessionID, AgentID: call.Principal.AgentID}); err != nil {
			return nil, err
		}
		return nil, ErrUnknown
	}
	decision := e.policy.Decide(call.Capability)
	base := audit.Event{RequestID: call.RequestID, JobID: call.Principal.JobID, SessionID: call.Principal.SessionID, AgentID: call.Principal.AgentID, Capability: call.Capability, Decision: string(decision), Outcome: "denied"}
	if decision == policy.Deny {
		if err := e.record(base); err != nil {
			return nil, err
		}
		return nil, ErrDenied
	}
	if err := validateArguments(call.Capability, call.Arguments); err != nil {
		base.Outcome = "invalid_arguments"
		if recordErr := e.record(base); recordErr != nil {
			return nil, recordErr
		}
		return nil, ErrInvalid
	}
	if decision == policy.RequireApproval {
		if e.approve == nil {
			if err := e.record(base); err != nil {
				return nil, err
			}
			return nil, ErrApproval
		}
		approved, err := e.approve(ctx, call.Capability, call.Principal)
		if err != nil || !approved {
			base.Outcome = "approval_denied"
			if recordErr := e.record(base); recordErr != nil {
				return nil, recordErr
			}
			return nil, ErrApproval
		}
		base.Outcome = "approved"
		if err := e.record(base); err != nil {
			return nil, err
		}
	}
	if !e.available[call.Capability] {
		base.Outcome = "unavailable"
		if err := e.record(base); err != nil {
			return nil, err
		}
		return nil, ErrUnavailable
	}
	if err := ctx.Err(); err != nil {
		base.Outcome = "cancelled"
		if recordErr := e.record(base); recordErr != nil {
			return nil, recordErr
		}
		return nil, err
	}
	base.Outcome = "started"
	started := time.Now()
	if err := e.record(base); err != nil {
		return nil, err
	}
	ctx = context.WithValue(ctx, principalContextKey{}, call.Principal)
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
	if recordErr := e.record(base); recordErr != nil {
		return nil, recordErr
	}
	if err != nil {
		return nil, err
	}
	return result, nil
}

func (e *Executor) record(event audit.Event) error {
	if e.audit != nil {
		e.audit.Record(event)
		if reporter, ok := e.audit.(interface{ Err() error }); ok && reporter.Err() != nil {
			return ErrAudit
		}
	}
	return nil
}

func sortStrings(values []string) {
	for i := 1; i < len(values); i++ {
		for j := i; j > 0 && values[j] < values[j-1]; j-- {
			values[j], values[j-1] = values[j-1], values[j]
		}
	}
}

// validateArguments translates the frozen JSON Schema subset in the shared catalog
// at the capability boundary. It rejects unknown keys, wrong types, missing fields,
// and values outside the published enum/length/range bounds before a handler runs.
func validateArguments(name string, raw json.RawMessage) error {
	var descriptor *catalog.Descriptor
	for _, item := range catalog.List() {
		if item.Name == name {
			copy := item
			descriptor = &copy
			break
		}
	}
	if descriptor == nil || len(descriptor.InputSchema) == 0 {
		return nil
	}
	var schema struct {
		AdditionalProperties bool     `json:"additionalProperties"`
		Required             []string `json:"required"`
		Properties           map[string]struct {
			Type      string   `json:"type"`
			MinLength *int     `json:"minLength"`
			MaxLength *int     `json:"maxLength"`
			Enum      []string `json:"enum"`
			Minimum   *int     `json:"minimum"`
			Maximum   *int     `json:"maximum"`
		} `json:"properties"`
	}
	if json.Unmarshal(descriptor.InputSchema, &schema) != nil {
		return ErrInvalid
	}
	var fields map[string]json.RawMessage
	if len(raw) == 0 {
		raw = []byte("{}")
	}
	if json.Unmarshal(raw, &fields) != nil || fields == nil {
		return ErrInvalid
	}
	for _, key := range schema.Required {
		if _, ok := fields[key]; !ok {
			return ErrInvalid
		}
	}
	for key, value := range fields {
		prop, ok := schema.Properties[key]
		if !ok {
			if !schema.AdditionalProperties {
				return ErrInvalid
			}
			continue
		}
		var kind any
		if json.Unmarshal(value, &kind) != nil {
			return ErrInvalid
		}
		switch prop.Type {
		case "string":
			text, ok := kind.(string)
			if !ok {
				return ErrInvalid
			}
			n := len([]byte(text))
			if prop.MinLength != nil && n < *prop.MinLength {
				return ErrInvalid
			}
			if prop.MaxLength != nil && n > *prop.MaxLength {
				return ErrInvalid
			}
			if len(prop.Enum) > 0 {
				found := false
				for _, v := range prop.Enum {
					if text == v {
						found = true
						break
					}
				}
				if !found {
					return ErrInvalid
				}
			}
		case "integer":
			number, ok := kind.(float64)
			if !ok || number != float64(int64(number)) {
				return ErrInvalid
			}
			if prop.Minimum != nil && number < float64(*prop.Minimum) {
				return ErrInvalid
			}
			if prop.Maximum != nil && number > float64(*prop.Maximum) {
				return ErrInvalid
			}
		default:
			return ErrInvalid
		}
	}
	return nil
}
