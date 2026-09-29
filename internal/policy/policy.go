package policy

import "fmt"

type Decision string

const (
	Allow           Decision = "allow"
	RequireApproval Decision = "require_approval"
	Deny            Decision = "deny"
)

type Policy struct {
	Default      Decision            `json:"default_decision"`
	Capabilities map[string]Decision `json:"capabilities"`
}

func (p Policy) Decide(capability string) Decision {
	if d, ok := p.Capabilities[capability]; ok {
		return d
	}
	if p.Default == "" {
		return Deny
	}
	return p.Default
}

func ParseDecision(raw string) (Decision, error) {
	d := Decision(raw)
	switch d {
	case Allow, RequireApproval, Deny:
		return d, nil
	default:
		return "", fmt.Errorf("unsupported policy decision")
	}
}
