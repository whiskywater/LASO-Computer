package main

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"net"
	"net/http"
	"os"
	"os/signal"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
	"syscall"
	"time"

	"github.com/Registered-Agent-Attorney/LASO-Computer/internal/audit"
	"github.com/Registered-Agent-Attorney/LASO-Computer/internal/capabilities"
	"github.com/Registered-Agent-Attorney/LASO-Computer/internal/client"
	"github.com/Registered-Agent-Attorney/LASO-Computer/internal/config"
	"github.com/Registered-Agent-Attorney/LASO-Computer/internal/identity"
	"github.com/Registered-Agent-Attorney/LASO-Computer/internal/platform"
	"github.com/Registered-Agent-Attorney/LASO-Computer/internal/supervisor"
	"github.com/Registered-Agent-Attorney/LASO-Computer/internal/transport"
)

const version = "0.1.0-dev"

func main() {
	if err := run(os.Args[1:]); err != nil {
		fmt.Fprintln(os.Stderr, "laso-computer:", err)
		os.Exit(1)
	}
}

func run(args []string) error {
	options, err := parseArgs(args)
	if err != nil {
		return err
	}
	if options.command == "shutdown" {
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		if err := supervisor.RequestShutdownControl(ctx); err != nil {
			return err
		}
		fmt.Fprintln(os.Stdout, "shutdown request accepted")
		return nil
	}
	stateDir, err := defaultStateDir()
	if err != nil {
		return errors.New("user state directory unavailable")
	}
	if options.configPath == "" {
		options.configPath = filepath.Join(stateDir, "config.json")
	}
	if options.command == "version" {
		fmt.Fprintln(os.Stdout, version)
		return nil
	}
	if options.command == "init-config" {
		if _, err := os.Stat(options.configPath); err == nil {
			return errors.New("configuration already exists; refusing to overwrite")
		}
		if err := config.WriteExample(options.configPath); err != nil {
			return errors.New("could not create configuration")
		}
		fmt.Fprintln(os.Stdout, "created deny-by-default configuration")
		return nil
	}
	cfg, err := config.Load(options.configPath)
	if err != nil {
		return errors.New("configuration invalid or unreadable")
	}
	if options.command == "check-config" {
		fmt.Fprintln(os.Stdout, "configuration valid; default policy denies all capabilities")
		return nil
	}
	desktop := platform.NewDesktop()
	_, available, err := capabilities.NewDesktopRegistry(desktop, cfg)
	if err != nil {
		return errors.New("capability registry initialization failed")
	}
	if options.command == "capabilities" || options.command == "status" {
		clientID, err := identity.LoadOrCreate(stateDir)
		if err != nil {
			return errors.New("local identity unavailable")
		}
		items := make([]client.CapabilityStatus, 0, len(capabilities.Catalog()))
		for _, item := range capabilities.Catalog() {
			items = append(items, client.CapabilityStatus{Name: item.Name, Description: item.Description, Risk: item.Risk, Profile: item.Profile, InputSchema: item.InputSchema, Available: available[item.Name], Decision: string(cfg.Policy().Decide(item.Name))})
		}
		if options.command == "capabilities" {
			return json.NewEncoder(os.Stdout).Encode(items)
		}
		return json.NewEncoder(os.Stdout).Encode(client.Hello{ClientID: clientID, OS: runtime.GOOS, Architecture: runtime.GOARCH, Capabilities: items})
	}
	if options.command != "run" && options.command != "listen" && options.command != "supervise" {
		return errors.New("unknown command")
	}
	auditWriter, err := audit.Open(filepath.Join(stateDir, "audit.jsonl"))
	if err != nil {
		return errors.New("audit log unavailable")
	}
	defer auditWriter.Close()
	if options.command == "listen" {
		return runLoopbackListener(cfg, stateDir, auditWriter, options.listenAddress)
	}
	if options.command == "supervise" {
		return runSupervised(cfg, stateDir, auditWriter, options)
	}
	return runProtocol(cfg, stateDir, auditWriter, os.Stdin, os.Stdout)
}

func runProtocol(cfg config.Config, stateDir string, auditWriter *audit.Writer, input io.Reader, output io.Writer) error {
	engine, server, err := newWorkerEngine(cfg, stateDir, auditWriter, input, output)
	if err != nil {
		return err
	}
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	fmt.Fprintln(os.Stderr, "laso-computer: LASO worker protocol v1 ready; local policy is enforced")
	return server.Serve(ctx, engine)
}

func runLoopbackListener(cfg config.Config, stateDir string, auditWriter *audit.Writer, address string) error {
	if err := validateListenAddress(address); err != nil {
		return err
	}
	listener, err := net.Listen("tcp", address)
	if err != nil {
		return errors.New("loopback worker listener could not start")
	}
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	return serveLoopbackListener(ctx, listener, cfg, stateDir, auditWriter, nil)
}

func runSupervised(cfg config.Config, stateDir string, auditWriter *audit.Writer, options cliOptions) error {
	workerListener, err := net.Listen("tcp", options.listenAddress)
	if err != nil {
		return errors.New("loopback worker listener could not start")
	}
	healthListener, err := net.Listen("tcp", options.healthAddress)
	if err != nil {
		_ = workerListener.Close()
		return errors.New("loopback health listener could not start")
	}
	logFile, logger, err := openServiceLog(stateDir)
	if err != nil {
		_ = workerListener.Close()
		_ = healthListener.Close()
		return errors.New("service log unavailable")
	}
	defer logFile.Close()
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	controlListener, err := supervisor.ListenShutdownControl()
	if err != nil {
		_ = workerListener.Close()
		_ = healthListener.Close()
		return errors.New("local shutdown control could not start")
	}
	var controlDone chan error
	var workerState *supervisedWorkerState
	if controlListener != nil {
		controlDone = make(chan error, 1)
		workerState = &supervisedWorkerState{}
		go func() {
			controlDone <- supervisor.ServeShutdownControl(ctx, controlListener, workerState.allowShutdown, stop)
			close(controlDone)
		}()
	}
	tunnelState := supervisor.NewStateStore()
	workerDone := make(chan error, 1)
	tunnelDone := make(chan error, 1)
	go func() {
		workerDone <- serveLoopbackListener(ctx, workerListener, cfg, stateDir, auditWriter, workerState)
		close(workerDone)
	}()
	go func() {
		tunnelDone <- supervisor.RunTunnel(ctx, options.tunnel, tunnelState)
		close(tunnelDone)
	}()
	healthServer := &http.Server{Handler: newHealthHandler(tunnelState), ReadHeaderTimeout: 3 * time.Second}
	healthDone := make(chan error, 1)
	go func() {
		healthDone <- healthServer.Serve(healthListener)
		close(healthDone)
	}()
	monitorDone := make(chan struct{})
	go func() {
		defer close(monitorDone)
		monitorTunnelState(ctx, tunnelState, logger)
	}()
	logger.Printf("service_start worker_listener=listening health_listener=loopback shutdown_control=%t", controlListener != nil)

	var result error
	select {
	case <-ctx.Done():
	case err := <-workerDone:
		if err != nil {
			result = errors.New("worker listener stopped")
		}
		stop()
	case err := <-tunnelDone:
		if err != nil {
			result = errors.New("tunnel supervisor stopped")
		}
		stop()
	case err := <-healthDone:
		if err != nil && !errors.Is(err, http.ErrServerClosed) {
			result = errors.New("health listener stopped")
		}
		stop()
	case err := <-controlDone:
		if err != nil {
			result = errors.New("local shutdown control stopped")
		}
		stop()
	}
	stop()
	_ = workerListener.Close()
	shutdownCtx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	_ = healthServer.Shutdown(shutdownCtx)
	_ = healthListener.Close()
	waitChannels := []<-chan error{workerDone, tunnelDone, healthDone}
	if controlDone != nil {
		waitChannels = append(waitChannels, controlDone)
	}
	waitForShutdown(shutdownCtx, waitChannels...)
	select {
	case <-monitorDone:
	case <-shutdownCtx.Done():
	}
	logger.Printf("service_stop")
	return result
}

func waitForShutdown(ctx context.Context, channels ...<-chan error) {
	for _, done := range channels {
		select {
		case <-done:
		case <-ctx.Done():
			return
		}
	}
}

func newHealthHandler(tunnelState *supervisor.StateStore) http.Handler {
	healthMux := http.NewServeMux()
	healthMux.HandleFunc("/livez", func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodGet {
			w.WriteHeader(http.StatusMethodNotAllowed)
			return
		}
		w.Header().Set("Cache-Control", "no-store")
		w.WriteHeader(http.StatusOK)
		_, _ = io.WriteString(w, "ok\n")
	})
	healthMux.HandleFunc("/healthz", func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodGet {
			w.WriteHeader(http.StatusMethodNotAllowed)
			return
		}
		w.Header().Set("Content-Type", "application/json")
		w.Header().Set("Cache-Control", "no-store")
		state := tunnelState.Get()
		status := http.StatusOK
		condition := "ready"
		if state.Phase != "connected" {
			status = http.StatusServiceUnavailable
			condition = "degraded"
		}
		w.WriteHeader(status)
		_ = json.NewEncoder(w).Encode(struct {
			Condition      string           `json:"condition"`
			WorkerListener string           `json:"worker_listener"`
			Tunnel         supervisor.State `json:"tunnel"`
		}{Condition: condition, WorkerListener: "listening", Tunnel: state})
	})
	return healthMux
}

func monitorTunnelState(ctx context.Context, state *supervisor.StateStore, logger *log.Logger) {
	ticker := time.NewTicker(250 * time.Millisecond)
	defer ticker.Stop()
	last := supervisor.State{}
	for {
		current := state.Get()
		if current.Phase != last.Phase || current.LastFailure != last.LastFailure || current.RestartCount != last.RestartCount {
			logger.Printf("tunnel_state phase=%s failure=%s restart_count=%d", current.Phase, current.LastFailure, current.RestartCount)
			last = current
		}
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
		}
	}
}

const maximumServiceLogBytes = 8 << 20

func openServiceLog(stateDir string) (*os.File, *log.Logger, error) {
	if err := os.MkdirAll(stateDir, 0700); err != nil {
		return nil, nil, err
	}
	path := filepath.Join(stateDir, "service.log")
	if info, err := os.Stat(path); err == nil && info.Size() >= maximumServiceLogBytes {
		backup := path + ".1"
		_ = os.Remove(backup)
		if err := os.Rename(path, backup); err != nil {
			return nil, nil, err
		}
	}
	f, err := os.OpenFile(path, os.O_WRONLY|os.O_APPEND|os.O_CREATE, 0600)
	if err != nil {
		return nil, nil, err
	}
	return f, log.New(f, "laso-computer: ", log.LstdFlags|log.LUTC), nil
}

func serveLoopbackListener(ctx context.Context, listener net.Listener, cfg config.Config, stateDir string, auditWriter *audit.Writer, workerState *supervisedWorkerState) error {
	stopListener := context.AfterFunc(ctx, func() { _ = listener.Close() })
	defer stopListener()
	defer listener.Close()
	fmt.Fprintln(os.Stderr, "laso-computer: loopback worker protocol listener ready; local policy is enforced")
	for {
		connection, err := listener.Accept()
		if err != nil {
			if ctx.Err() != nil {
				return nil
			}
			return errors.New("loopback worker listener failed")
		}
		if !isLoopbackAddr(connection.RemoteAddr()) {
			_ = connection.Close()
			continue
		}
		if err := serveConnection(ctx, connection, cfg, stateDir, auditWriter, workerState); err != nil && ctx.Err() == nil {
			fmt.Fprintln(os.Stderr, "laso-computer: remote worker connection ended")
		}
	}
}

func serveConnection(parent context.Context, connection net.Conn, cfg config.Config, stateDir string, auditWriter *audit.Writer, workerState *supervisedWorkerState) error {
	defer connection.Close()
	ctx, cancel := context.WithCancel(parent)
	defer cancel()
	stopClose := context.AfterFunc(ctx, func() { _ = connection.Close() })
	defer stopClose()
	engine, server, err := newWorkerEngine(cfg, stateDir, auditWriter, connection, connection)
	if err != nil {
		return errors.New("local worker runtime unavailable")
	}
	if workerState != nil {
		if !workerState.attach(engine) {
			engine.Close()
			return errors.New("supervisor is stopping")
		}
		defer workerState.detach(engine)
	}
	return server.Serve(ctx, engine)
}

func newWorkerEngine(cfg config.Config, stateDir string, auditWriter *audit.Writer, input io.Reader, output io.Writer) (*client.Engine, *transport.Server, error) {
	clientID, err := identity.LoadOrCreate(stateDir)
	if err != nil {
		return nil, nil, errors.New("local identity unavailable")
	}
	var server *transport.Server
	desktop := platform.NewDesktop()
	registry, available, err := capabilities.NewDesktopRegistry(desktop, cfg)
	if err != nil {
		return nil, nil, errors.New("capability registry initialization failed")
	}
	approval := func(ctx context.Context, name string, principal capabilities.Principal) (bool, error) {
		if server == nil {
			return false, errors.New("approval transport unavailable")
		}
		return server.RequestApproval(ctx, name, principal)
	}
	executor, err := capabilities.NewExecutor(registry, cfg.Policy(), available, auditWriter, approval)
	if err != nil {
		return nil, nil, errors.New("local authorization initialization failed")
	}
	statuses := make([]client.CapabilityStatus, 0, len(capabilities.Catalog()))
	for _, item := range capabilities.Catalog() {
		statuses = append(statuses, client.CapabilityStatus{Name: item.Name, Description: item.Description, Risk: item.Risk, Profile: item.Profile, InputSchema: item.InputSchema, Available: available[item.Name], Decision: string(cfg.Policy().Decide(item.Name))})
	}
	engine, err := client.NewEngine(context.Background(), executor, client.Hello{ClientID: clientID, OS: runtime.GOOS, Architecture: runtime.GOARCH, Capabilities: statuses})
	if err != nil {
		return nil, nil, errors.New("client runtime initialization failed")
	}
	server = transport.NewServer(input, output, time.Duration(cfg.ApprovalTimeoutMS)*time.Millisecond)
	return engine, server, nil
}

type cliOptions struct {
	command       string
	configPath    string
	listenAddress string
	healthAddress string
	tunnel        supervisor.TunnelConfig
}

func parseArgs(args []string) (cliOptions, error) {
	options := cliOptions{
		command:       "run",
		listenAddress: "127.0.0.1:49192",
		healthAddress: "127.0.0.1:49193",
		tunnel: supervisor.TunnelConfig{
			SSHExecutable: "ssh",
			RemotePort:    49191,
		},
	}
	commandSelected := false
	var err error
	selectCommand := func(next string) error {
		if commandSelected {
			return errors.New("multiple commands are not allowed")
		}
		commandSelected = true
		options.command = next
		return nil
	}
	for i := 0; i < len(args); i++ {
		arg := args[i]
		value := func(flag string) (string, error) {
			if i+1 >= len(args) {
				return "", fmt.Errorf("%s requires a value", flag)
			}
			i++
			return args[i], nil
		}
		switch {
		case arg == "--config":
			options.configPath, err = value(arg)
			if err != nil {
				return cliOptions{}, err
			}
		case strings.HasPrefix(arg, "--config="):
			options.configPath = strings.TrimPrefix(arg, "--config=")
			if options.configPath == "" {
				return cliOptions{}, errors.New("--config requires a path")
			}
		case arg == "--status":
			if err := selectCommand("status"); err != nil {
				return cliOptions{}, err
			}
		case arg == "--shutdown":
			if err := selectCommand("shutdown"); err != nil {
				return cliOptions{}, err
			}
		case arg == "--capabilities":
			if err := selectCommand("capabilities"); err != nil {
				return cliOptions{}, err
			}
		case arg == "--check-config":
			if err := selectCommand("check-config"); err != nil {
				return cliOptions{}, err
			}
		case arg == "--init-config":
			if err := selectCommand("init-config"); err != nil {
				return cliOptions{}, err
			}
		case arg == "--version" || arg == "version":
			if err := selectCommand("version"); err != nil {
				return cliOptions{}, err
			}
		case arg == "--supervise":
			if err := selectCommand("supervise"); err != nil {
				return cliOptions{}, err
			}
		case arg == "--listen":
			address, err := value(arg)
			if err != nil {
				return cliOptions{}, err
			}
			options.listenAddress = address
			if err := validateListenAddress(options.listenAddress); err != nil {
				return cliOptions{}, err
			}
			if err := selectCommand("listen"); err != nil {
				return cliOptions{}, err
			}
		case strings.HasPrefix(arg, "--listen="):
			options.listenAddress = strings.TrimPrefix(arg, "--listen=")
			if err := validateListenAddress(options.listenAddress); err != nil {
				return cliOptions{}, err
			}
			if err := selectCommand("listen"); err != nil {
				return cliOptions{}, err
			}
		case arg == "--worker-address":
			options.listenAddress, err = value(arg)
			if err != nil {
				return cliOptions{}, err
			}
			if err := validateListenAddress(options.listenAddress); err != nil {
				return cliOptions{}, err
			}
		case strings.HasPrefix(arg, "--worker-address="):
			options.listenAddress = strings.TrimPrefix(arg, "--worker-address=")
			if err := validateListenAddress(options.listenAddress); err != nil {
				return cliOptions{}, err
			}
		case arg == "--health-address":
			options.healthAddress, err = value(arg)
			if err != nil {
				return cliOptions{}, err
			}
			if err := validateListenAddress(options.healthAddress); err != nil {
				return cliOptions{}, err
			}
		case strings.HasPrefix(arg, "--health-address="):
			options.healthAddress = strings.TrimPrefix(arg, "--health-address=")
			if err := validateListenAddress(options.healthAddress); err != nil {
				return cliOptions{}, err
			}
		case arg == "--ssh-executable":
			options.tunnel.SSHExecutable, err = value(arg)
			if err != nil {
				return cliOptions{}, err
			}
		case strings.HasPrefix(arg, "--ssh-executable="):
			options.tunnel.SSHExecutable = strings.TrimPrefix(arg, "--ssh-executable=")
		case arg == "--ssh-config":
			options.tunnel.SSHConfigPath, err = value(arg)
			if err != nil {
				return cliOptions{}, err
			}
		case strings.HasPrefix(arg, "--ssh-config="):
			options.tunnel.SSHConfigPath = strings.TrimPrefix(arg, "--ssh-config=")
		case arg == "--ssh-host":
			options.tunnel.HostAlias, err = value(arg)
			if err != nil {
				return cliOptions{}, err
			}
		case strings.HasPrefix(arg, "--ssh-host="):
			options.tunnel.HostAlias = strings.TrimPrefix(arg, "--ssh-host=")
		case arg == "--remote-port":
			portText, err := value(arg)
			if err != nil {
				return cliOptions{}, err
			}
			port, parseErr := strconv.Atoi(portText)
			if parseErr != nil || port < 1 || port > 65535 {
				return cliOptions{}, errors.New("--remote-port must be between 1 and 65535")
			}
			options.tunnel.RemotePort = uint16(port)
		case strings.HasPrefix(arg, "--remote-port="):
			portText := strings.TrimPrefix(arg, "--remote-port=")
			port, parseErr := strconv.Atoi(portText)
			if parseErr != nil || port < 1 || port > 65535 {
				return cliOptions{}, errors.New("--remote-port must be between 1 and 65535")
			}
			options.tunnel.RemotePort = uint16(port)
		case arg == "run":
			if err := selectCommand("run"); err != nil {
				return cliOptions{}, err
			}
		default:
			return cliOptions{}, errors.New("unknown option or command")
		}
	}
	if options.command == "supervise" {
		if options.listenAddress == options.healthAddress {
			return cliOptions{}, errors.New("worker and health listeners require distinct addresses")
		}
		_, localPort, _ := net.SplitHostPort(options.listenAddress)
		port, _ := strconv.Atoi(localPort)
		options.tunnel.LocalPort = uint16(port)
		if options.tunnel.HostAlias == "" {
			return cliOptions{}, errors.New("--ssh-host is required with --supervise")
		}
		if err := supervisor.ValidateTunnelConfig(options.tunnel); err != nil {
			return cliOptions{}, err
		}
	}
	return options, nil
}

func validateListenAddress(address string) error {
	host, portText, err := net.SplitHostPort(address)
	if err != nil {
		return errors.New("listener address must be IPv4 loopback and include a port")
	}
	ip := net.ParseIP(host)
	port, portErr := strconv.Atoi(portText)
	if ip == nil || ip.To4() == nil || !ip.IsLoopback() || portErr != nil || port < 1 || port > 65535 {
		return errors.New("listener address must be IPv4 loopback and include a valid port")
	}
	return nil
}

func isLoopbackAddr(address net.Addr) bool {
	tcpAddress, ok := address.(*net.TCPAddr)
	return ok && tcpAddress.IP != nil && tcpAddress.IP.IsLoopback()
}

func defaultStateDir() (string, error) {
	base, err := os.UserConfigDir()
	if err != nil {
		return "", err
	}
	return filepath.Join(base, "LASO", "Computer"), nil
}
