package agentmodel

import (
	"sync"
	"time"

	"forkmesh.com/desktop-core/internal/agents"
)

const sessionCacheMaxAge = 2 * time.Second

type sessionCache struct {
	mu       sync.Mutex
	list     []agents.Session
	byID     map[int]int
	loadedAt time.Time
	valid    bool
	epoch    uint64
}

func (c *sessionCache) invalidate() {
	c.mu.Lock()
	c.valid = false
	c.epoch++
	c.mu.Unlock()
}

func (c *sessionCache) get(store *agents.Store, now time.Time) ([]agents.Session, map[int]int) {
	c.mu.Lock()
	if c.valid && now.Sub(c.loadedAt) < sessionCacheMaxAge {
		list, byID := c.list, c.byID
		c.mu.Unlock()
		return list, byID
	}
	epoch := c.epoch
	c.mu.Unlock()
	list := store.LoadAllSessions()
	byID := make(map[int]int, len(list))
	for i := len(list) - 1; i >= 0; i-- {
		byID[list[i].ID] = i
	}
	c.mu.Lock()
	if c.epoch == epoch {
		c.list, c.byID, c.loadedAt, c.valid = list, byID, now, true
	}
	c.mu.Unlock()
	return list, byID
}

func (m *Module) sessionList() ([]agents.Session, map[int]int, *agents.Store) {
	writer := m.writer()
	if writer == nil {
		return nil, nil, nil
	}
	list, byID := m.sessions.get(writer.Store(), time.Now())
	return list, byID, writer.Store()
}

func (m *Module) findSession(id int) (agents.Session, *agents.Store, bool) {
	list, byID, store := m.sessionList()
	if store == nil || id <= 0 {
		return agents.Session{}, store, false
	}
	index, ok := byID[id]
	if !ok {
		return agents.Session{}, store, false
	}
	return list[index], store, true
}
