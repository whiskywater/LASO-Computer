//go:build !windows

package supervisor

import (
	"context"
	"errors"
	"net"
)

func ListenShutdownControl() (net.Listener, error) { return nil, nil }

func RequestShutdownControl(context.Context) error {
	return errors.New("supervisor shutdown control is available only on Windows")
}
