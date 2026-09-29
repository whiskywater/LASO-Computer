package capabilities

import (
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/Registered-Agent-Attorney/LASO-Computer/internal/config"
	"github.com/Registered-Agent-Attorney/LASO-Computer/internal/policy"
)

const shellTestHelperEnv = "LASO_COMPUTER_SHELL_TEST_HELPER"

func TestShellChildUsesTemporaryWorkingDirectory(t *testing.T) {
	previous, wasSet := os.LookupEnv(shellTestHelperEnv)
	if err := os.Setenv(shellTestHelperEnv, "1"); err != nil {
		t.Fatal("could not prepare shell helper")
	}
	t.Cleanup(func() {
		if wasSet {
			_ = os.Setenv(shellTestHelperEnv, previous)
		} else {
			_ = os.Unsetenv(shellTestHelperEnv)
		}
	})
	executable, err := os.Executable()
	if err != nil {
		t.Fatal("test executable unavailable")
	}
	cfg := config.Default()
	cfg.Shell.AllowedExecutables = []string{executable}
	cfg.Shell.EnvironmentAllowlist = []string{shellTestHelperEnv}
	registry, available, err := NewDesktopRegistry(&testDesktop{}, cfg)
	if err != nil {
		t.Fatal("shell registry unavailable")
	}
	executor, err := NewExecutor(registry, policy.Policy{Default: policy.Allow}, available, nil, nil)
	if err != nil {
		t.Fatal("shell executor unavailable")
	}
	args, err := json.Marshal(map[string]any{
		"executable": executable,
		"arguments":  []string{"-test.run=TestShellWorkingDirectoryProbe"},
		"timeout_ms": 10000,
	})
	if err != nil {
		t.Fatal("shell arguments unavailable")
	}
	raw, err := executor.Invoke(context.Background(), Invocation{RequestID: "shell-cwd-test", Capability: "shell.execute", Arguments: args})
	if err != nil {
		t.Fatal("shell helper failed")
	}
	var result shellResult
	if err := json.Unmarshal(raw, &result); err != nil || result.ExitCode != 0 {
		t.Fatal("shell helper returned an invalid result")
	}
	want := strings.ToLower(filepath.Clean(os.TempDir()))
	if !strings.Contains(strings.ToLower(result.Stdout), want) {
		t.Fatal("shell child did not start in the OS temporary directory")
	}
}

func TestShellWorkingDirectoryProbe(t *testing.T) {
	if os.Getenv(shellTestHelperEnv) != "1" {
		return
	}
	workingDir, err := os.Getwd()
	if err != nil {
		os.Exit(2)
	}
	_, _ = os.Stdout.WriteString(workingDir)
}
