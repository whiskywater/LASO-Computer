package main

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/Registered-Agent-Attorney/LASO-Computer/internal/capabilities"
	"github.com/Registered-Agent-Attorney/LASO-Computer/internal/client"
	"github.com/Registered-Agent-Attorney/LASO-Computer/internal/policy"
	"github.com/Registered-Agent-Attorney/LASO-Computer/internal/supervisor"
)

func TestParseArgsLoopbackListener(t *testing.T) {
	options, err := parseArgs([]string{"--config", "worker.json", "--listen=127.0.0.1:49192"})
	if err != nil {
		t.Fatal(err)
	}
	if options.command != "listen" || options.configPath != "worker.json" || options.listenAddress != "127.0.0.1:49192" {
		t.Fatalf("unexpected parsed listener options: %+v", options)
	}
}

func TestParseArgsRejectsMultipleCommands(t *testing.T) {
	if _, err := parseArgs([]string{"--status", "--listen=127.0.0.1:49192"}); err == nil {
		t.Fatal("multiple commands were accepted")
	}
}

func TestParseArgsShutdown(t *testing.T) {
	options, err := parseArgs([]string{"--shutdown"})
	if err != nil {
		t.Fatal(err)
	}
	if options.command != "shutdown" {
		t.Fatalf("unexpected shutdown command options: %+v", options)
	}
	if _, err := parseArgs([]string{"--shutdown", "--supervise"}); err == nil {
		t.Fatal("shutdown combined with supervise was accepted")
	}
}

func TestParseArgsSuperviseRequiresLoopbackAndHost(t *testing.T) {
	options, err := parseArgs([]string{"--supervise", "--ssh-host", "server1300", "--worker-address", "127.0.0.1:49192", "--health-address", "127.0.0.1:49193", "--remote-port", "49191"})
	if err != nil {
		t.Fatal(err)
	}
	if options.command != "supervise" || options.tunnel.HostAlias != "server1300" || options.tunnel.LocalPort != 49192 || options.tunnel.RemotePort != 49191 {
		t.Fatalf("unexpected supervised options: %+v", options)
	}
	if _, err := parseArgs([]string{"--supervise", "--ssh-host", "server1300", "--worker-address", "0.0.0.0:49192"}); err == nil {
		t.Fatal("non-loopback worker bind was accepted")
	}
	if _, err := parseArgs([]string{"--supervise", "--ssh-host", "server1300", "--health-address", "0.0.0.0:49193"}); err == nil {
		t.Fatal("non-loopback health bind was accepted")
	}
	if _, err := parseArgs([]string{"--supervise", "--ssh-host", "server1300", "--health-address", "127.0.0.1:49192"}); err == nil {
		t.Fatal("overlapping worker and health listeners were accepted")
	}
	if _, err := parseArgs([]string{"--supervise"}); err == nil {
		t.Fatal("supervisor without an SSH target was accepted")
	}
}

func TestHealthHandlerReportsDegradedTunnelWithoutSensitiveFields(t *testing.T) {
	store := supervisor.NewStateStore()
	req := httptest.NewRequest("GET", "/healthz", nil)
	res := httptest.NewRecorder()
	newHealthHandler(store).ServeHTTP(res, req)
	if res.Code != 503 {
		t.Fatalf("expected disconnected tunnel to be degraded, got %d", res.Code)
	}
	body := res.Body.String()
	for _, field := range []string{`"condition":"degraded"`, `"worker_listener":"listening"`, `"phase":"starting"`} {
		if !strings.Contains(body, field) {
			t.Errorf("health output missing %s: %s", field, body)
		}
	}
	for _, forbidden := range []string{"token", "secret", "password", "client_id"} {
		if strings.Contains(strings.ToLower(body), forbidden) {
			t.Errorf("health output included sensitive field %q", forbidden)
		}
	}
}

func TestSupervisedShutdownRejectsActiveJobsThenStopsWhenIdle(t *testing.T) {
	started := make(chan struct{}, 2)
	finished := make(chan struct{}, 2)
	release := make(chan struct{})
	var releaseOnce sync.Once
	defer releaseOnce.Do(func() { close(release) })

	registry := capabilities.NewRegistry()
	if err := registry.Register("pointer.move", func(context.Context, json.RawMessage) (json.RawMessage, error) {
		started <- struct{}{}
		<-release
		finished <- struct{}{}
		return json.RawMessage(`{}`), nil
	}); err != nil {
		t.Fatal(err)
	}
	executor, err := capabilities.NewExecutor(registry, policy.Policy{Default: policy.Allow}, map[string]bool{"pointer.move": true}, nil, nil)
	if err != nil {
		t.Fatal(err)
	}
	engine, err := client.NewEngine(context.Background(), executor, client.Hello{ClientID: "shutdown-test"})
	if err != nil {
		t.Fatal(err)
	}
	workerState := &supervisedWorkerState{}
	if !workerState.attach(engine) {
		t.Fatal("worker engine did not attach")
	}
	for i := 1; i <= 2; i++ {
		payload := json.RawMessage(`{"capability":"pointer.move","arguments":{}}`)
		request, marshalErr := json.Marshal(client.Request{ProtocolVersion: client.ProtocolVersion, RequestID: fmt.Sprintf("request-%d", i), Operation: "submit", JobID: fmt.Sprintf("job-%d", i), Payload: payload})
		if marshalErr != nil {
			t.Fatal(marshalErr)
		}
		response := engine.Handle(request)
		if !response.OK || response.State != "Running" {
			t.Fatalf("active job %d did not start: %#v", i, response)
		}
		select {
		case <-started:
		case <-time.After(time.Second):
			t.Fatalf("active job %d handler did not start", i)
		}
	}

	controlListener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	controlDone := make(chan error, 1)
	go func() {
		controlDone <- supervisor.ServeShutdownControl(ctx, controlListener, workerState.allowShutdown, cancel)
	}()

	if response := sendShutdownControlForTest(t, controlListener.Addr().String()); response != "rejected\n" {
		t.Fatalf("active-job shutdown was not rejected: %q", response)
	}
	if ctx.Err() != nil || workerState.stopping {
		t.Fatal("rejected shutdown changed the running supervisor state")
	}
	postRejectPayload := json.RawMessage(`{"capability":"pointer.move","arguments":{}}`)
	postRejectRequest, err := json.Marshal(client.Request{ProtocolVersion: client.ProtocolVersion, RequestID: "request-after-rejection", Operation: "submit", JobID: "job-after-rejection", Payload: postRejectPayload})
	if err != nil {
		t.Fatal(err)
	}
	if response := engine.Handle(postRejectRequest); !response.OK || response.State != "Running" {
		t.Fatalf("worker stopped accepting work after rejected shutdown: %#v", response)
	}
	select {
	case <-started:
	case <-time.After(time.Second):
		t.Fatal("job after rejected shutdown did not start")
	}
	healthListener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	healthServer := &http.Server{Handler: newHealthHandler(supervisor.NewStateStore())}
	healthDone := make(chan error, 1)
	go func() { healthDone <- healthServer.Serve(healthListener) }()
	stopHealth := context.AfterFunc(ctx, func() { _ = healthServer.Close() })
	defer func() {
		stopHealth()
		_ = healthServer.Close()
		<-healthDone
	}()
	healthResponse, err := http.Get("http://" + healthListener.Addr().String() + "/livez")
	if err != nil {
		t.Fatalf("health listener stopped after rejected shutdown: %v", err)
	}
	_ = healthResponse.Body.Close()
	if healthResponse.StatusCode != http.StatusOK {
		t.Fatalf("health listener returned %d after rejected shutdown", healthResponse.StatusCode)
	}

	releaseOnce.Do(func() { close(release) })
	for i := 0; i < 3; i++ {
		select {
		case <-finished:
		case <-time.After(time.Second):
			t.Fatal("active capability did not finish after release")
		}
	}
	deadline := time.Now().Add(time.Second)
	for {
		response := sendShutdownControlForTest(t, controlListener.Addr().String())
		if response == "stopping\n" {
			break
		}
		if time.Now().After(deadline) {
			t.Fatalf("idle shutdown was not accepted after jobs finished: %q", response)
		}
		time.Sleep(10 * time.Millisecond)
	}
	select {
	case err := <-controlDone:
		if err != nil {
			t.Fatal(err)
		}
	case <-time.After(time.Second):
		t.Fatal("accepted idle shutdown did not stop the supervisor control")
	}
	if ctx.Err() == nil {
		t.Fatal("accepted idle shutdown did not cancel the supervisor")
	}
	postStopRequest, err := json.Marshal(client.Request{ProtocolVersion: client.ProtocolVersion, RequestID: "request-after-accept", Operation: "hello"})
	if err != nil {
		t.Fatal(err)
	}
	if response := engine.Handle(postStopRequest); response.OK {
		t.Fatalf("idle shutdown left the worker engine accepting requests: %#v", response)
	}
}

func sendShutdownControlForTest(t *testing.T, address string) string {
	t.Helper()
	connection, err := net.DialTimeout("tcp", address, time.Second)
	if err != nil {
		t.Fatal(err)
	}
	defer connection.Close()
	_ = connection.SetDeadline(time.Now().Add(time.Second))
	if _, err := connection.Write([]byte("shutdown\n")); err != nil {
		t.Fatal(err)
	}
	response := make([]byte, len("rejected\n"))
	if _, err := io.ReadFull(connection, response); err != nil {
		t.Fatal(err)
	}
	return string(response)
}

func TestValidateListenAddressOnlyAllowsIPv4Loopback(t *testing.T) {
	for _, address := range []string{"127.0.0.1:49192", "127.12.0.1:60000"} {
		if err := validateListenAddress(address); err != nil {
			t.Errorf("loopback address %q rejected: %v", address, err)
		}
	}
	for _, address := range []string{"0.0.0.0:49192", "192.168.1.5:49192", "[::1]:49192", "localhost:49192", "127.0.0.1:0", "127.0.0.1:65536"} {
		if err := validateListenAddress(address); err == nil {
			t.Errorf("unsafe or invalid address %q was accepted", address)
		}
	}
}
