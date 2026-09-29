package identity

import (
	"crypto/rand"
	"encoding/hex"
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

func LoadOrCreate(directory string) (string, error) {
	if err := os.MkdirAll(directory, 0700); err != nil {
		return "", fmt.Errorf("prepare identity directory")
	}
	path := filepath.Join(directory, "client-id")
	data, err := os.ReadFile(path)
	if err == nil {
		id := strings.TrimSpace(string(data))
		if valid(id) {
			return id, nil
		}
		return "", fmt.Errorf("invalid local identity file")
	}
	if !os.IsNotExist(err) {
		return "", fmt.Errorf("read local identity")
	}
	var raw [16]byte
	if _, err := rand.Read(raw[:]); err != nil {
		return "", fmt.Errorf("generate local identity")
	}
	id := hex.EncodeToString(raw[:])
	f, err := os.OpenFile(path, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0600)
	if err != nil {
		if os.IsExist(err) {
			return LoadOrCreate(directory)
		}
		return "", fmt.Errorf("write local identity")
	}
	defer f.Close()
	if _, err := f.WriteString(id + "\n"); err != nil {
		return "", fmt.Errorf("write local identity")
	}
	return id, nil
}

func valid(id string) bool {
	if len(id) != 32 {
		return false
	}
	_, err := hex.DecodeString(id)
	return err == nil
}
