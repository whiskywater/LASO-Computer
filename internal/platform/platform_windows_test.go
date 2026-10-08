//go:build windows

package platform

import (
	"context"
	"errors"
	"testing"
	"time"
	"unsafe"
)

func TestInputMatchesWindowsAMD64Size(t *testing.T) {
	if size := unsafe.Sizeof(input{}); size != 40 {
		t.Fatalf("INPUT size = %d, want 40", size)
	}
}

func TestResolveWindowThreadIDUsesReturnValueNotProcessID(t *testing.T) {
	threadID, err := resolveWindowThreadID(0x123, func(hwnd uintptr, processID *uint32) uintptr {
		if hwnd != 0x123 {
			t.Fatalf("hwnd=%#x", hwnd)
		}
		*processID = 4242
		return 77
	})
	if err != nil {
		t.Fatal(err)
	}
	if threadID != 77 {
		t.Fatalf("threadID=%d want=77; process ID must not be used as GUI thread ID", threadID)
	}
	if _, err := resolveWindowThreadID(0x123, func(uintptr, *uint32) uintptr { return 0 }); err == nil {
		t.Fatal("zero thread ID accepted")
	}
}

func TestForegroundFocusBelongsToAuthorizedTarget(t *testing.T) {
	child := func(parent, candidate uintptr) bool { return parent == 1 && (candidate == 2 || candidate == 3) }
	tests := []struct {
		name               string
		top, active, focus uintptr
		want               bool
	}{
		{"focused child", 1, 1, 2, true},
		{"stolen foreground", 1, 4, 4, false},
		{"active outside target", 1, 4, 2, false},
		{"focus outside target", 1, 1, 4, false},
		{"missing focus", 1, 1, 0, false},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			if got := focusBelongsTo(tt.top, tt.active, tt.focus, child); got != tt.want {
				t.Fatalf("got %v want %v", got, tt.want)
			}
		})
	}
}

func TestWindowActivationPostconditionAndFailureModes(t *testing.T) {
	validateOK := func() bool { return true }
	noPause := func(time.Duration) {}
	t.Run("foreground denial", func(t *testing.T) {
		err := waitForWindowActivation(context.Background(), "0x1", 1, validateOK, func(uintptr) bool { return false }, func() uintptr { return 0 }, noPause, 0)
		if err == nil {
			t.Fatal("expected foreground denial")
		}
	})
	t.Run("activation timeout", func(t *testing.T) {
		err := waitForWindowActivation(context.Background(), "0x1", 1, validateOK, func(uintptr) bool { return false }, func() uintptr { return 0 }, noPause, 5*time.Millisecond)
		if err == nil {
			t.Fatal("expected bounded timeout")
		}
	})
	t.Run("stale hwnd", func(t *testing.T) {
		called := false
		err := waitForWindowActivation(context.Background(), "0x1", 1, func() bool { return false }, func(uintptr) bool { called = true; return true }, func() uintptr { return 1 }, noPause, time.Second)
		if err == nil || called {
			t.Fatalf("err=%v activate called=%v", err, called)
		}
	})
	t.Run("retry until verified", func(t *testing.T) {
		attempts := 0
		active := uintptr(0)
		err := waitForWindowActivation(context.Background(), "0x1", 1, validateOK, func(uintptr) bool {
			attempts++
			if attempts == 2 {
				active = 1
			}
			return true
		}, func() uintptr { return active }, noPause, time.Second)
		if err != nil || attempts != 2 {
			t.Fatalf("err=%v attempts=%d", err, attempts)
		}
	})
	t.Run("cancelled", func(t *testing.T) {
		ctx, cancel := context.WithCancel(context.Background())
		cancel()
		err := waitForWindowActivation(ctx, "0x1", 1, validateOK, func(uintptr) bool { return true }, func() uintptr { return 1 }, noPause, time.Second)
		if !errors.Is(err, context.Canceled) {
			t.Fatalf("got %v", err)
		}
	})
}

func TestFocusedChildMustRemainContinuousBeforeInput(t *testing.T) {
	if !focusContinuous(1, 2, 1, 2) {
		t.Fatal("expected authorized focus")
	}
	if focusContinuous(1, 2, 3, 4) {
		t.Fatal("accepted stolen foreground")
	}
	if focusContinuous(1, 2, 1, 3) {
		t.Fatal("accepted changed child focus")
	}
}
