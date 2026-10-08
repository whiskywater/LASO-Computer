//go:build windows

package supervisor

import (
	"context"
	"errors"
	"io"
	"net"
	"os/user"
	"time"

	"github.com/Microsoft/go-winio"
)

func ListenShutdownControl() (net.Listener, error) {
	current, err := user.Current()
	if err != nil {
		return nil, errors.New("current Windows user could not be identified")
	}
	pipeName, securityDescriptor, err := shutdownControlIdentity(current.Uid)
	if err != nil {
		return nil, err
	}
	return listenShutdownControlAt(pipeName, securityDescriptor)
}

func listenShutdownControlAt(pipeName, securityDescriptor string) (net.Listener, error) {
	listener, err := winio.ListenPipe(pipeName, &winio.PipeConfig{SecurityDescriptor: securityDescriptor})
	if err != nil {
		return nil, errors.New("current-user shutdown pipe could not be created")
	}
	return listener, nil
}

func RequestShutdownControl(ctx context.Context) error {
	current, err := user.Current()
	if err != nil {
		return errors.New("current Windows user could not be identified")
	}
	pipeName, _, err := shutdownControlIdentity(current.Uid)
	if err != nil {
		return err
	}
	return requestShutdownControlAt(ctx, pipeName)
}

func requestShutdownControlAt(ctx context.Context, pipeName string) error {
	connection, err := winio.DialPipeContext(ctx, pipeName)
	if err != nil {
		return errors.New("current user's LASO Computer supervisor was not reachable")
	}
	defer connection.Close()
	deadline := time.Now().Add(shutdownControlReadLimit)
	if ctxDeadline, ok := ctx.Deadline(); ok && ctxDeadline.Before(deadline) {
		deadline = ctxDeadline
	}
	_ = connection.SetDeadline(deadline)
	if _, err := connection.Write([]byte(shutdownControlRequest)); err != nil {
		return errors.New("shutdown request could not be sent")
	}
	response := make([]byte, len(shutdownControlAccepted))
	if _, err := io.ReadFull(connection, response); err != nil || string(response) != shutdownControlAccepted {
		return errors.New("shutdown request was not accepted")
	}
	return nil
}
