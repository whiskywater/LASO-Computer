package audit

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"sync"
	"time"
)

type Event struct {
	Time            time.Time `json:"time"`
	RequestID       string    `json:"request_id,omitempty"`
	JobID           string    `json:"job_id,omitempty"`
	SessionID       string    `json:"session_id,omitempty"`
	AgentID         string    `json:"agent_id,omitempty"`
	Capability      string    `json:"capability"`
	Decision        string    `json:"decision"`
	Outcome         string    `json:"outcome"`
	DurationMS      int64     `json:"duration_ms,omitempty"`
	PayloadRedacted bool      `json:"payload_redacted"`
}

type Writer struct {
	mu sync.Mutex
	f  *os.File
}

func Open(path string) (*Writer, error) {
	if err := os.MkdirAll(filepathDir(path), 0700); err != nil {
		return nil, fmt.Errorf("prepare audit directory")
	}
	f, err := os.OpenFile(path, os.O_APPEND|os.O_CREATE|os.O_WRONLY, 0600)
	if err != nil {
		return nil, fmt.Errorf("open audit log")
	}
	if err := f.Chmod(0600); err != nil {
		_ = f.Close()
		return nil, fmt.Errorf("secure audit log")
	}
	return &Writer{f: f}, nil
}

func filepathDir(path string) string {
	// filepath.Dir is kept behind this helper so all audit writes stay centralized.
	return filepath.Dir(path)
}

func (w *Writer) Record(event Event) {
	if w == nil || w.f == nil {
		return
	}
	event.Time = time.Now().UTC()
	event.PayloadRedacted = true
	w.mu.Lock()
	defer w.mu.Unlock()
	_ = json.NewEncoder(w.f).Encode(event)
}

func (w *Writer) Close() error {
	if w == nil || w.f == nil {
		return nil
	}
	return w.f.Close()
}
