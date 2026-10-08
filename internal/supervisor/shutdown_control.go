package supervisor

import (
	"bytes"
	"context"
	"errors"
	"io"
	"net"
	"regexp"
	"time"
)

const (
	shutdownControlRequest   = "shutdown\n"
	shutdownControlAccepted  = "stopping\n"
	shutdownControlRejected  = "rejected\n"
	shutdownControlReadLimit = 3 * time.Second
)

var windowsSIDPattern = regexp.MustCompile(`^S-\d(?:-\d+)+$`)

func shutdownControlIdentity(sid string) (string, string, error) {
	if !windowsSIDPattern.MatchString(sid) {
		return "", "", errors.New("current Windows user SID is invalid")
	}
	return `\\.\pipe\laso-computer-control-` + sid, "D:P(A;;GA;;;" + sid + ")", nil
}

// ServeShutdownControl accepts exact shutdown frames from the platform's
// locally secured listener. The callback must reject while capability work is
// active; accepted requests cancel the supervisor after acknowledgement.
func ServeShutdownControl(ctx context.Context, listener net.Listener, allowShutdown func() bool, requestShutdown context.CancelFunc) error {
	if ctx == nil || listener == nil || allowShutdown == nil || requestShutdown == nil {
		return errors.New("shutdown control dependencies are required")
	}
	stopClose := context.AfterFunc(ctx, func() { _ = listener.Close() })
	defer stopClose()
	defer listener.Close()
	for {
		connection, err := listener.Accept()
		if err != nil {
			if ctx.Err() != nil {
				return nil
			}
			return errors.New("shutdown control listener failed")
		}
		accepted := handleShutdownControlConnection(connection, allowShutdown)
		if accepted {
			requestShutdown()
			return nil
		}
	}
}

func handleShutdownControlConnection(connection net.Conn, allowShutdown func() bool) bool {
	defer connection.Close()
	_ = connection.SetDeadline(time.Now().Add(shutdownControlReadLimit))
	request := make([]byte, len(shutdownControlRequest))
	if _, err := io.ReadFull(connection, request); err != nil {
		return false
	}
	if !bytes.Equal(request, []byte(shutdownControlRequest)) {
		_, _ = connection.Write([]byte(shutdownControlRejected))
		return false
	}
	if !allowShutdown() {
		_, _ = connection.Write([]byte(shutdownControlRejected))
		return false
	}
	_, _ = connection.Write([]byte(shutdownControlAccepted))
	return true
}
