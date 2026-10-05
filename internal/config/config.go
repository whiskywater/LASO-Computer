package config

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"

	"github.com/Registered-Agent-Attorney/LASO-Computer/internal/catalog"
	"github.com/Registered-Agent-Attorney/LASO-Computer/internal/policy"
)

const MaxConfigBytes = 1 << 20

type ShellConfig struct {
	AllowedExecutables   []string `json:"allowed_executables"`
	EnvironmentAllowlist []string `json:"environment_allowlist"`
}

type Config struct {
	Version           int                        `json:"version"`
	DefaultDecision   policy.Decision            `json:"default_decision"`
	Capabilities      map[string]policy.Decision `json:"capabilities"`
	Shell             ShellConfig                `json:"shell"`
	ApprovalTimeoutMS int                        `json:"approval_timeout_ms"`
}

func Default() Config {
	return Config{Version: 1, DefaultDecision: policy.Deny, Capabilities: map[string]policy.Decision{}, ApprovalTimeoutMS: 60000}
}

func (c Config) Policy() policy.Policy {
	return policy.Policy{Default: c.DefaultDecision, Capabilities: c.Capabilities}
}

func Load(path string) (Config, error) {
	f, err := os.Open(path)
	if errors.Is(err, os.ErrNotExist) {
		return Default(), nil
	}
	if err != nil {
		return Config{}, fmt.Errorf("open config: %w", err)
	}
	defer f.Close()
	data, err := io.ReadAll(io.LimitReader(f, MaxConfigBytes+1))
	if err != nil {
		return Config{}, fmt.Errorf("read config")
	}
	if len(data) > MaxConfigBytes {
		return Config{}, fmt.Errorf("config exceeds size limit")
	}
	dec := json.NewDecoder(bytes.NewReader(data))
	dec.DisallowUnknownFields()
	var cfg Config
	if err := dec.Decode(&cfg); err != nil {
		return Config{}, fmt.Errorf("decode config")
	}
	if err := dec.Decode(new(any)); !errors.Is(err, io.EOF) {
		return Config{}, fmt.Errorf("config must contain one JSON value")
	}
	if err := cfg.Validate(); err != nil {
		return Config{}, err
	}
	return cfg, nil
}

func (c Config) Validate() error {
	if c.Version != 1 {
		return fmt.Errorf("unsupported config version")
	}
	if _, err := policy.ParseDecision(string(c.DefaultDecision)); err != nil {
		return fmt.Errorf("invalid default decision")
	}
	if c.Capabilities == nil {
		return fmt.Errorf("capabilities must be an object")
	}
	for name, value := range c.Capabilities {
		if !catalog.Known(name) {
			return fmt.Errorf("unknown capability in config: %s", name)
		}
		if _, err := policy.ParseDecision(string(value)); err != nil {
			return fmt.Errorf("invalid decision for capability %s", name)
		}
	}
	if c.ApprovalTimeoutMS < 1000 || c.ApprovalTimeoutMS > 10*60*1000 {
		return fmt.Errorf("approval timeout must be between 1000 and 600000 milliseconds")
	}
	seen := map[string]bool{}
	for _, executable := range c.Shell.AllowedExecutables {
		if !filepath.IsAbs(executable) || strings.ContainsRune(executable, 0) || filepath.Clean(executable) != executable || seen[executable] {
			return fmt.Errorf("shell executable entries must be unique clean absolute paths")
		}
		seen[executable] = true
	}
	seen = map[string]bool{}
	for _, name := range c.Shell.EnvironmentAllowlist {
		if !validEnvName(name) || seen[name] {
			return fmt.Errorf("shell environment names must be unique valid identifiers")
		}
		seen[name] = true
	}
	return nil
}

func validEnvName(value string) bool {
	if value == "" || len(value) > 128 {
		return false
	}
	for i, r := range value {
		if !(r == '_' || r >= 'A' && r <= 'Z' || r >= 'a' && r <= 'z' || i > 0 && r >= '0' && r <= '9') {
			return false
		}
	}
	return true
}

func WriteExample(path string) error {
	cfg := Default()
	data, err := json.MarshalIndent(cfg, "", "  ")
	if err != nil {
		return err
	}
	if err := os.MkdirAll(filepath.Dir(path), 0700); err != nil {
		return err
	}
	f, err := os.OpenFile(path, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0600)
	if err != nil {
		return err
	}
	if _, err := f.Write(append(data, '\n')); err != nil {
		_ = f.Close()
		_ = os.Remove(path)
		return err
	}
	return f.Close()
}
