//go:build linux

package platform

import (
	"encoding/binary"
	"testing"

	"github.com/jezek/xgb/xproto"
)

func TestClientMessageData32SerializesFiveWords(t *testing.T) {
	data := xproto.ClientMessageDataUnionData32New([]uint32{2, 0, 0, 0, 0})
	serialized := data.Bytes()
	if len(serialized) != 20 || binary.LittleEndian.Uint32(serialized[:4]) != 2 {
		t.Fatal("X11 client-message data union was not initialized")
	}
}
