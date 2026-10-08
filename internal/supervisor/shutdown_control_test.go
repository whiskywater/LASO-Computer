package supervisor

import (
	"context"
	"io"
	"net"
	"strings"
	"testing"
	"time"
)

func TestShutdownControlIdentityUsesOnlyValidatedSID(t *testing.T) {
	sid := "S-1-5-21-1000-2000-3000-1001"
	name, descriptor, err := shutdownControlIdentity(sid)
	if err != nil {
		t.Fatal(err)
	}
	if name != `\\.\pipe\laso-computer-control-`+sid {
		t.Fatalf("unexpected control pipe name %q", name)
	}
	if descriptor != "D:P(A;;GA;;;"+sid+")" {
		t.Fatalf("unexpected security descriptor %q", descriptor)
	}
	for _, forbidden := range []string{"S-1-1-0", "S-1-5-11", "BA", "WD"} {
		if strings.Contains(descriptor, forbidden) {
			t.Fatalf("control descriptor includes broad principal %q", forbidden)
		}
	}
}

func TestShutdownControlIdentityRejectsSDDLInjection(t *testing.T) {
	for _, sid := range []string{"", "S-1-1-0;D:(A;;GA;;;WD)", "S-1-5-21-", "S-1-a-5"} {
		if _, _, err := shutdownControlIdentity(sid); err == nil {
			t.Errorf("invalid SID %q was accepted", sid)
		}
	}
}

func TestShutdownControlRejectsMalformedFrame(t *testing.T) {
	client, server := net.Pipe()
	done := make(chan bool, 1)
	go func() { done <- handleShutdownControlConnection(server, func() bool { return true }) }()
	_ = client.SetDeadline(time.Now().Add(time.Second))
	if _, err := client.Write([]byte("not-stop\n")); err != nil {
		t.Fatal(err)
	}
	response := make([]byte, len(shutdownControlRejected))
	if _, err := io.ReadFull(client, response); err != nil {
		t.Fatal(err)
	}
	if string(response) != shutdownControlRejected {
		t.Fatalf("unexpected rejection response %q", response)
	}
	if <-done {
		t.Fatal("malformed request was accepted")
	}
}

func TestServeShutdownControlCancelsOnlyAfterAcceptedFrame(t *testing.T) {
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	done := make(chan error, 1)
	go func() { done <- ServeShutdownControl(ctx, listener, func() bool { return true }, cancel) }()

	connection, err := net.DialTimeout("tcp", listener.Addr().String(), time.Second)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := connection.Write([]byte(shutdownControlRequest)); err != nil {
		t.Fatal(err)
	}
	response := make([]byte, len(shutdownControlAccepted))
	_ = connection.SetDeadline(time.Now().Add(time.Second))
	if _, err := io.ReadFull(connection, response); err != nil {
		t.Fatal(err)
	}
	_ = connection.Close()
	if string(response) != shutdownControlAccepted {
		t.Fatalf("unexpected acknowledgement %q", response)
	}
	select {
	case err := <-done:
		if err != nil {
			t.Fatal(err)
		}
	case <-time.After(time.Second):
		t.Fatal("shutdown control did not return after cancellation")
	}
	if ctx.Err() == nil {
		t.Fatal("accepted shutdown frame did not cancel the supervisor")
	}
}
