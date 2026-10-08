//go:build windows

package supervisor

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"os/user"
	"testing"
	"time"
)

func TestCurrentUserNamedPipeShutdownLifecycle(t *testing.T) {
	current, err := user.Current()
	if err != nil {
		t.Fatal(err)
	}
	pipeName, securityDescriptor, err := shutdownControlIdentity(current.Uid)
	if err != nil {
		t.Fatal(err)
	}
	var suffix [8]byte
	if _, err := rand.Read(suffix[:]); err != nil {
		t.Fatal(err)
	}
	pipeName += "-test-" + hex.EncodeToString(suffix[:])
	listener, err := listenShutdownControlAt(pipeName, securityDescriptor)
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	done := make(chan error, 1)
	go func() { done <- ServeShutdownControl(ctx, listener, func() bool { return true }, cancel) }()

	requestCtx, requestCancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer requestCancel()
	if err := requestShutdownControlAt(requestCtx, pipeName); err != nil {
		t.Fatal(err)
	}
	select {
	case err := <-done:
		if err != nil {
			t.Fatal(err)
		}
	case <-time.After(time.Second):
		t.Fatal("named pipe shutdown did not stop its supervisor context")
	}
	if ctx.Err() == nil {
		t.Fatal("named pipe request did not cancel the supervisor context")
	}
}
