package config

import (
	"os"
	"path/filepath"
	"testing"

	"github.com/Registered-Agent-Attorney/LASO-Computer/internal/policy"
)

func TestDefaultPolicyDeniesCapabilities(t *testing.T) {
	cfg := Default()
	if err := cfg.Validate(); err != nil {
		t.Fatal(err)
	}
	if cfg.Policy().Decide("screen.capture") != policy.Deny || cfg.Policy().Decide("shell.execute") != policy.Deny {
		t.Fatal("default configuration must deny all capabilities")
	}
}

func TestValidateRejectsUnknownCapabilityAndDecision(t *testing.T) {
	cfg := Default()
	cfg.Capabilities["files.read"] = policy.Allow
	if err := cfg.Validate(); err == nil {
		t.Fatal("unknown capability accepted")
	}
	cfg = Default()
	cfg.Capabilities["screen.capture"] = "always"
	if err := cfg.Validate(); err == nil {
		t.Fatal("unknown decision accepted")
	}
}

func TestValidateRequiresCleanAbsoluteShellAllowlist(t *testing.T) {
	base := t.TempDir()
	cfg := Default()
	cfg.Shell.AllowedExecutables = []string{base + string(os.PathSeparator) + "tools" + string(os.PathSeparator) + ".." + string(os.PathSeparator) + "tool.exe"}
	if err := cfg.Validate(); err == nil {
		t.Fatal("non-normalized path accepted")
	}
	cfg.Shell.AllowedExecutables = []string{filepath.Join(base, "tool.exe")}
	if err := cfg.Validate(); err != nil {
		t.Fatalf("clean absolute path rejected: %v", err)
	}
}

func TestLoadRejectsUnknownFieldsAndOversizedConfig(t *testing.T) {
	path := filepath.Join(t.TempDir(), "config.json")
	if err := os.WriteFile(path, []byte(`{"version":1,"default_decision":"deny","capabilities":{},"approval_timeout_ms":60000,"unexpected":true}`), 0600); err != nil {
		t.Fatal(err)
	}
	if _, err := Load(path); err == nil {
		t.Fatal("unknown configuration field accepted")
	}
	if err := os.WriteFile(path, make([]byte, MaxConfigBytes+1), 0600); err != nil {
		t.Fatal(err)
	}
	if _, err := Load(path); err == nil {
		t.Fatal("oversized configuration accepted")
	}
}
