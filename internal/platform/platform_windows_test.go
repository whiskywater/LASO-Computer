//go:build windows

package platform

import (
	"testing"
	"unsafe"
)

func TestInputMatchesWindowsAMD64Size(t *testing.T) {
	if size := unsafe.Sizeof(input{}); size != 40 {
		t.Fatalf("INPUT size = %d, want 40", size)
	}
}
