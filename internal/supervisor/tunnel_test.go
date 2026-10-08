package supervisor

import (
	"context"
	"errors"
	"path/filepath"
	"reflect"
	"testing"
	"time"
)

func TestValidateTunnelConfig(t *testing.T) {
	valid := TunnelConfig{SSHExecutable: "ssh.exe", HostAlias: "server1300", LocalPort: 49192, RemotePort: 49191}
	if err := ValidateTunnelConfig(valid); err != nil {
		t.Fatalf("valid tunnel configuration rejected: %v", err)
	}

	cases := []struct {
		name string
		edit func(*TunnelConfig)
	}{
		{name: "empty host alias", edit: func(cfg *TunnelConfig) { cfg.HostAlias = "" }},
		{name: "option injection host alias", edit: func(cfg *TunnelConfig) { cfg.HostAlias = "-oProxyCommand=bad" }},
		{name: "missing local port", edit: func(cfg *TunnelConfig) { cfg.LocalPort = 0 }},
		{name: "missing remote port", edit: func(cfg *TunnelConfig) { cfg.RemotePort = 0 }},
		{name: "relative ssh config", edit: func(cfg *TunnelConfig) { cfg.SSHConfigPath = "ssh_config" }},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			cfg := valid
			tc.edit(&cfg)
			if err := ValidateTunnelConfig(cfg); err == nil {
				t.Fatal("unsafe tunnel configuration was accepted")
			}
		})
	}
}

func TestSSHArgsUseLoopbackReverseForwardAndKeepalives(t *testing.T) {
	args := sshArgs(TunnelConfig{
		SSHConfigPath: `C:\Users\test\.ssh\config`, HostAlias: "server1300", LocalPort: 49192, RemotePort: 49191,
	})
	want := []string{
		"-N", "-T",
		"-o", "BatchMode=yes",
		"-o", "ExitOnForwardFailure=yes",
		"-o", "ServerAliveInterval=20",
		"-o", "ServerAliveCountMax=3",
		"-o", "StrictHostKeyChecking=yes",
		"-o", "ForwardAgent=no",
		"-o", "PermitLocalCommand=no",
		"-o", "ConnectTimeout=10",
		"-v",
		"-F", `C:\Users\test\.ssh\config`,
		"-R", "127.0.0.1:49191:127.0.0.1:49192", "server1300",
	}
	if !reflect.DeepEqual(args, want) {
		t.Fatalf("unexpected SSH arguments:\n got: %#v\nwant: %#v", args, want)
	}
}

func TestForwardReadyLineRequiresTheExpectedLoopbackPort(t *testing.T) {
	if !forwardReadyLine("debug1: remote forward success for: listen 127.0.0.1 port 49191, connect 127.0.0.1 port 49192", 49191) {
		t.Fatal("expected loopback remote forward success to be recognized")
	}
	if !forwardReadyLine("debug1: remote forward success for: listen 127.0.0.1:49191, connect 127.0.0.1:49192", 49191) {
		t.Fatal("expected current OpenSSH loopback remote forward success to be recognized")
	}
	for _, line := range []string{
		"debug1: remote forward success for: listen 0.0.0.0 port 49191, connect 127.0.0.1 port 49192",
		"debug1: remote forward success for: listen 0.0.0.0:49191, connect 127.0.0.1:49192",
		"debug1: remote forward success for: listen 127.0.0.1 port 49190, connect 127.0.0.1 port 49192",
		"debug1: remote forward success for: listen 127.0.0.1:491910, connect 127.0.0.1:49192",
	} {
		if forwardReadyLine(line, 49191) {
			t.Errorf("unexpected forward success marker accepted: %s", line)
		}
	}
}

func TestRunSSHFailsWhenExecutableIsMissing(t *testing.T) {
	cfg := TunnelConfig{SSHExecutable: filepath.Join(t.TempDir(), "missing-ssh"), HostAlias: "server1300", LocalPort: 49192, RemotePort: 49191}
	err := runSSH(context.Background(), cfg, func() { t.Error("unavailable SSH unexpectedly connected") })
	if !errors.Is(err, errTunnelStart) {
		t.Fatalf("expected a closed start failure, got %v", err)
	}
}

func TestRunWithReconnectRestartsAfterTunnelLoss(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	store := NewStateStore()
	var calls int
	attempts := make(chan int, 4)
	release := make(chan struct{})
	done := make(chan error, 1)
	go func() {
		done <- runWithReconnect(ctx, TunnelConfig{}, store, func(ctx context.Context, _ TunnelConfig, connected func()) error {
			calls++
			connected()
			attempts <- calls
			if calls == 3 {
				cancel()
				return nil
			}
			<-release
			return errors.New("connection dropped")
		}, time.Millisecond)
	}()
	for expected := 1; expected <= 3; expected++ {
		if attempt := <-attempts; attempt != expected {
			t.Fatalf("expected attempt %d, got %d", expected, attempt)
		}
		if expected < 3 && store.Get().Phase != "connected" {
			got := store.Get().Phase
			t.Fatalf("expected connected state for attempt %d, got %q", expected, got)
		}
		if expected < 3 {
			release <- struct{}{}
		}
	}
	state := <-done
	if state != nil {
		t.Fatalf("expected graceful shutdown, got %v", state)
	}
	if calls != 3 {
		t.Fatalf("expected three connection attempts, got %d", calls)
	}
	if got := store.Get(); got.Phase != "stopped" || got.RestartCount != 2 {
		t.Fatalf("unexpected final tunnel state: %+v", got)
	}
}

func TestNextDelayCapsExponentialBackoff(t *testing.T) {
	if got := nextDelay(maximumRetryDelay); got != maximumRetryDelay {
		t.Fatalf("backoff exceeded cap: %v", got)
	}
	if got := nextDelay(initialRetryDelay); got != 2*initialRetryDelay {
		t.Fatalf("unexpected exponential delay: %v", got)
	}
}
