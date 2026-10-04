package main

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"os/signal"
	"path/filepath"
	"runtime"
	"strings"
	"syscall"
	"time"

	"github.com/Registered-Agent-Attorney/LASO-Computer/internal/audit"
	"github.com/Registered-Agent-Attorney/LASO-Computer/internal/capabilities"
	"github.com/Registered-Agent-Attorney/LASO-Computer/internal/client"
	"github.com/Registered-Agent-Attorney/LASO-Computer/internal/config"
	"github.com/Registered-Agent-Attorney/LASO-Computer/internal/identity"
	"github.com/Registered-Agent-Attorney/LASO-Computer/internal/platform"
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
	command, configPath, err := parseArgs(args)
	if err != nil {
		return err
	}
	stateDir, err := defaultStateDir()
	if err != nil {
		return errors.New("user state directory unavailable")
	}
	if configPath == "" {
		configPath = filepath.Join(stateDir, "config.json")
	}
	if command == "version" {
		fmt.Fprintln(os.Stdout, version)
		return nil
	}
	if command == "init-config" {
		if _, err := os.Stat(configPath); err == nil {
			return errors.New("configuration already exists; refusing to overwrite")
		}
		if err := config.WriteExample(configPath); err != nil {
			return errors.New("could not create configuration")
		}
		fmt.Fprintln(os.Stdout, "created deny-by-default configuration")
		return nil
	}
	cfg, err := config.Load(configPath)
	if err != nil {
		return errors.New("configuration invalid or unreadable")
	}
	if command == "check-config" {
		fmt.Fprintln(os.Stdout, "configuration valid; default policy denies all capabilities")
		return nil
	}
	desktop := platform.NewDesktop()
	registry, available, err := capabilities.NewDesktopRegistry(desktop, cfg)
	if err != nil {
		return errors.New("capability registry initialization failed")
	}
	if command == "capabilities" || command == "status" {
		clientID, err := identity.LoadOrCreate(stateDir)
		if err != nil {
			return errors.New("local identity unavailable")
		}
		items := make([]client.CapabilityStatus, 0, len(capabilities.Catalog()))
		for _, item := range capabilities.Catalog() {
			items = append(items, client.CapabilityStatus{Name: item.Name, Description: item.Description, Risk: item.Risk, Available: available[item.Name], Decision: string(cfg.Policy().Decide(item.Name))})
		}
		if command == "capabilities" {
			return json.NewEncoder(os.Stdout).Encode(items)
		}
		return json.NewEncoder(os.Stdout).Encode(client.Hello{ClientID: clientID, OS: runtime.GOOS, Architecture: runtime.GOARCH, Capabilities: items})
	}
	if command != "run" {
		return errors.New("unknown command")
	}
	clientID, err := identity.LoadOrCreate(stateDir)
	if err != nil {
		return errors.New("local identity unavailable")
	}
	auditWriter, err := audit.Open(filepath.Join(stateDir, "audit.jsonl"))
	if err != nil {
		return errors.New("audit log unavailable")
	}
	defer auditWriter.Close()
	var server *transport.Server
	approval := func(ctx context.Context, name string, principal capabilities.Principal) (bool, error) {
		if server == nil {
			return false, errors.New("approval transport unavailable")
		}
		return server.RequestApproval(ctx, name, principal)
	}
	executor, err := capabilities.NewExecutor(registry, cfg.Policy(), available, auditWriter, approval)
	if err != nil {
		return errors.New("local authorization initialization failed")
	}
	statuses := make([]client.CapabilityStatus, 0, len(capabilities.Catalog()))
	for _, item := range capabilities.Catalog() {
		statuses = append(statuses, client.CapabilityStatus{Name: item.Name, Description: item.Description, Risk: item.Risk, Available: available[item.Name], Decision: string(cfg.Policy().Decide(item.Name))})
	}
	engine, err := client.NewEngine(context.Background(), executor, client.Hello{ClientID: clientID, OS: runtime.GOOS, Architecture: runtime.GOARCH, Capabilities: statuses})
	if err != nil {
		return errors.New("client runtime initialization failed")
	}
	server = transport.NewServer(os.Stdin, os.Stdout, time.Duration(cfg.ApprovalTimeoutMS)*time.Millisecond)
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	fmt.Fprintln(os.Stderr, "laso-computer: LASO worker protocol v1 ready; local policy is enforced")
	return server.Serve(ctx, engine)
}

func parseArgs(args []string) (command, configPath string, err error) {
	command = "run"
	for i := 0; i < len(args); i++ {
		arg := args[i]
		switch {
		case arg == "--config":
			if i+1 >= len(args) {
				return "", "", errors.New("--config requires a path")
			}
			i++
			configPath = args[i]
		case strings.HasPrefix(arg, "--config="):
			configPath = strings.TrimPrefix(arg, "--config=")
			if configPath == "" {
				return "", "", errors.New("--config requires a path")
			}
		case arg == "--status":
			command = "status"
		case arg == "--capabilities":
			command = "capabilities"
		case arg == "--check-config":
			command = "check-config"
		case arg == "--init-config":
			command = "init-config"
		case arg == "--version" || arg == "version":
			command = "version"
		case arg == "run":
			command = "run"
		default:
			return "", "", fmt.Errorf("unknown option or command")
		}
	}
	return command, configPath, nil
}

func defaultStateDir() (string, error) {
	base, err := os.UserConfigDir()
	if err != nil {
		return "", err
	}
	return filepath.Join(base, "LASO", "Computer"), nil
}
