package supervisor

import (
	"bufio"
	"context"
	"errors"
	"fmt"
	"io"
	"os/exec"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"sync"
	"time"
)

var hostAliasPattern = regexp.MustCompile(`^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$`)

const (
	initialRetryDelay = 5 * time.Second
	maximumRetryDelay = time.Minute
)

// TunnelConfig contains non-secret OpenSSH connection settings. Authentication
// remains owned by the user's SSH agent and SSH configuration.
type TunnelConfig struct {
	SSHExecutable string
	SSHConfigPath string
	HostAlias     string
	LocalPort     uint16
	RemotePort    uint16
}

// State is safe to expose from the loopback health endpoint.
type State struct {
	Phase        string    `json:"phase"`
	RestartCount uint64    `json:"restart_count"`
	LastFailure  string    `json:"last_failure,omitempty"`
	ChangedAt    time.Time `json:"changed_at"`
}

// StateStore holds the most recent tunnel state for health reporting.
type StateStore struct {
	mu    sync.RWMutex
	state State
}

func NewStateStore() *StateStore {
	return &StateStore{state: State{Phase: "starting", ChangedAt: time.Now().UTC()}}
}

func (s *StateStore) Get() State {
	s.mu.RLock()
	defer s.mu.RUnlock()
	return s.state
}

func (s *StateStore) set(phase, failure string, restartCount uint64) {
	s.mu.Lock()
	s.state = State{Phase: phase, LastFailure: failure, RestartCount: restartCount, ChangedAt: time.Now().UTC()}
	s.mu.Unlock()
}

func ValidateTunnelConfig(cfg TunnelConfig) error {
	if strings.TrimSpace(cfg.SSHExecutable) == "" || strings.ContainsRune(cfg.SSHExecutable, 0) {
		return errors.New("SSH executable is required")
	}
	if cfg.SSHConfigPath != "" && (!filepath.IsAbs(cfg.SSHConfigPath) || strings.ContainsRune(cfg.SSHConfigPath, 0)) {
		return errors.New("SSH config path must be absolute")
	}
	if !hostAliasPattern.MatchString(cfg.HostAlias) {
		return errors.New("SSH host alias is invalid")
	}
	if cfg.LocalPort == 0 || cfg.RemotePort == 0 {
		return errors.New("local and remote tunnel ports must be valid")
	}
	return nil
}

// RunTunnel maintains one authenticated loopback-only reverse forward. A dead
// SSH process is treated as a disconnected endpoint and retried with bounded
// exponential backoff. It never prompts for or handles credentials.
func RunTunnel(ctx context.Context, cfg TunnelConfig, state *StateStore) error {
	if cfg.SSHExecutable == "" {
		cfg.SSHExecutable = "ssh"
	}
	if err := ValidateTunnelConfig(cfg); err != nil {
		return err
	}
	if state == nil {
		return errors.New("tunnel state store is required")
	}
	return runWithReconnect(ctx, cfg, state, runSSH, initialRetryDelay)
}

type sessionFunc func(context.Context, TunnelConfig, func()) error

func runWithReconnect(ctx context.Context, cfg TunnelConfig, state *StateStore, session sessionFunc, retryDelay time.Duration) error {
	delay := retryDelay
	var restarts uint64
	for ctx.Err() == nil {
		state.set("connecting", "", restarts)
		err := session(ctx, cfg, func() { state.set("connected", "", restarts) })
		if ctx.Err() != nil {
			state.set("stopped", "", restarts)
			return nil
		}
		restarts++
		failure := "tunnel_exited"
		if errors.Is(err, errTunnelStart) {
			failure = "tunnel_start_failed"
		} else if errors.Is(err, errTunnelNotReady) {
			failure = "tunnel_not_ready"
		}
		state.set("backoff", failure, restarts)
		if !wait(ctx, delay) {
			break
		}
		delay = nextDelay(delay)
	}
	state.set("stopped", "", restarts)
	return nil
}

var errTunnelStart = errors.New("tunnel process could not start")
var errTunnelNotReady = errors.New("tunnel did not establish the reverse forward")

func runSSH(ctx context.Context, cfg TunnelConfig, connected func()) error {
	cmd := exec.Command(cfg.SSHExecutable, sshArgs(cfg)...)
	cmd.Stdin = nil
	cmd.Stdout = io.Discard
	stderr, err := cmd.StderrPipe()
	if err != nil {
		return errTunnelStart
	}
	if err := cmd.Start(); err != nil {
		return errTunnelStart
	}
	ready := make(chan struct{}, 1)
	scanDone := make(chan struct{})
	go func() {
		defer close(scanDone)
		scanner := bufio.NewScanner(stderr)
		for scanner.Scan() {
			if forwardReadyLine(scanner.Text(), cfg.RemotePort) {
				select {
				case ready <- struct{}{}:
				default:
				}
			}
		}
	}()
	exited := make(chan error, 1)
	go func() { exited <- cmd.Wait() }()
	connectTimer := time.NewTimer(30 * time.Second)
	defer connectTimer.Stop()
	select {
	case <-ctx.Done():
		_ = cmd.Process.Kill()
		<-exited
		<-scanDone
		return nil
	case <-exited:
		<-scanDone
		return errors.New("tunnel process exited before connection became stable")
	case <-connectTimer.C:
		_ = cmd.Process.Kill()
		<-exited
		<-scanDone
		return errTunnelNotReady
	case <-ready:
		connected()
	}
	select {
	case <-ctx.Done():
		_ = cmd.Process.Kill()
		<-exited
		<-scanDone
		return nil
	case <-exited:
		<-scanDone
		return errors.New("tunnel process exited")
	}
}

func forwardReadyLine(line string, remotePort uint16) bool {
	fields := strings.Fields(line)
	for i, field := range fields {
		if field != "listen" || i+1 >= len(fields) {
			continue
		}
		endpoint := strings.TrimSuffix(fields[i+1], ",")
		if endpoint == fmt.Sprintf("127.0.0.1:%d", remotePort) {
			return true
		}
		if endpoint == "127.0.0.1" && i+3 < len(fields) && fields[i+2] == "port" && strings.TrimSuffix(fields[i+3], ",") == strconv.Itoa(int(remotePort)) {
			return true
		}
	}
	return false
}

func sshArgs(cfg TunnelConfig) []string {
	args := []string{
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
	}
	if cfg.SSHConfigPath != "" {
		args = append(args, "-F", cfg.SSHConfigPath)
	}
	forward := fmt.Sprintf("127.0.0.1:%s:127.0.0.1:%s", strconv.Itoa(int(cfg.RemotePort)), strconv.Itoa(int(cfg.LocalPort)))
	return append(args, "-R", forward, cfg.HostAlias)
}

func nextDelay(delay time.Duration) time.Duration {
	delay *= 2
	if delay > maximumRetryDelay {
		return maximumRetryDelay
	}
	return delay
}

func wait(ctx context.Context, duration time.Duration) bool {
	timer := time.NewTimer(duration)
	defer timer.Stop()
	select {
	case <-ctx.Done():
		return false
	case <-timer.C:
		return true
	}
}
