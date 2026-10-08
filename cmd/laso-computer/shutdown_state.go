package main

import (
	"sync"

	"github.com/Registered-Agent-Attorney/LASO-Computer/internal/client"
)

// supervisedWorkerState serializes connection attachment against a local
// shutdown request. The listener serves one connection at a time, but a
// shutdown can arrive while that connection is being initialized or handled.
type supervisedWorkerState struct {
	mu       sync.Mutex
	engine   *client.Engine
	stopping bool
}

func (s *supervisedWorkerState) attach(engine *client.Engine) bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.stopping || s.engine != nil {
		return false
	}
	s.engine = engine
	return true
}

func (s *supervisedWorkerState) detach(engine *client.Engine) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.engine == engine {
		s.engine = nil
	}
}

// allowShutdown atomically closes an idle engine to new requests before
// permitting the supervisor to stop. Active jobs cause a rejection without
// canceling them or changing worker state.
func (s *supervisedWorkerState) allowShutdown() bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.stopping {
		return false
	}
	if s.engine != nil && !s.engine.CloseIfIdle() {
		return false
	}
	s.stopping = true
	return true
}
