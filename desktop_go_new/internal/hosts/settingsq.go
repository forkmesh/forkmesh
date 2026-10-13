package hosts

import (
	"context"
	"encoding/json"
	"sort"
	"sync"
	"time"

	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/phonefamily"
	"forkmesh.com/desktop-core/internal/rpc"
)

const (
	settingsWriteTimeout = 10 * time.Second
	settingsRetryEvery   = 2 * time.Second
)

type pendingSetting struct {
	write   daemon.SettingWrite
	version int64
}

type settingsQueue struct {
	mu      sync.Mutex
	pending map[string]pendingSetting
	version int64
	wake    chan struct{}
	writing sync.Mutex
}

func (m *Module) queue() *settingsQueue {
	m.mu.Lock()
	defer m.mu.Unlock()
	if m.writes == nil {
		m.writes = &settingsQueue{pending: map[string]pendingSetting{}, wake: make(chan struct{}, 1)}
	}
	return m.writes
}

func (m *Module) callUI(ctx context.Context, method string, params any) ([]byte, error) {
	if m.CallUI != nil {
		return m.CallUI(ctx, method, params)
	}
	if m.core == nil {
		return nil, rpc.Fail("desktop_unavailable")
	}
	return m.core.CallUI(ctx, method, params)
}

func (m *Module) uiAttached() bool {
	if m.Attached != nil {
		return m.Attached()
	}
	return m.desktopAttached()
}

func (m *Module) writeToDesktop(ctx context.Context, writes []daemon.SettingWrite) error {
	if m.WriteSettings != nil {
		return m.WriteSettings(ctx, writes)
	}
	if m.core == nil {
		return rpc.Fail("desktop_unavailable")
	}
	return m.core.WriteSettings(ctx, writes)
}

func (m *Module) writeSettings(writes ...daemon.SettingWrite) {
	if len(writes) == 0 {
		return
	}
	q := m.queue()
	q.mu.Lock()
	for _, write := range writes {
		q.version++
		q.pending[write.Key] = pendingSetting{write: write, version: q.version}
	}
	q.mu.Unlock()
	m.flushSettings()
	select {
	case q.wake <- struct{}{}:
	default:
	}
}

func (m *Module) flushSettings() bool {
	q := m.queue()
	q.writing.Lock()
	defer q.writing.Unlock()
	q.mu.Lock()
	if len(q.pending) == 0 {
		q.mu.Unlock()
		return true
	}
	keys := make([]string, 0, len(q.pending))
	for key := range q.pending {
		keys = append(keys, key)
	}
	sort.Strings(keys)
	batch := make([]daemon.SettingWrite, 0, len(keys))
	versions := map[string]int64{}
	for _, key := range keys {
		batch = append(batch, q.pending[key].write)
		versions[key] = q.pending[key].version
	}
	q.mu.Unlock()
	if !m.uiAttached() {
		return false
	}
	base := m.ctx
	if base == nil {
		base = context.Background()
	}
	ctx, cancel := context.WithTimeout(base, settingsWriteTimeout)
	err := m.writeToDesktop(ctx, batch)
	cancel()
	if err != nil {
		if !phonefamily.DesktopUnavailable(err) {
			m.logCore("hosts: settings write failed: %v", err)
		}
		return false
	}
	q.mu.Lock()
	for key, version := range versions {
		if current, ok := q.pending[key]; ok && current.version == version {
			delete(q.pending, key)
		}
	}
	q.mu.Unlock()
	for _, write := range batch {
		if write.Key == hostsSetting || write.Key == decommissionedNodesSetting || write.Key == deletedMeshNodesSetting {
			m.broadcastUI("core.hosts.saved", map[string]any{"keys": keys})
			break
		}
	}
	return true
}

func (m *Module) settingsLoop(ctx context.Context) {
	q := m.queue()
	ticker := time.NewTicker(settingsRetryEvery)
	defer ticker.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-q.wake:
		case <-ticker.C:
		}
		q.mu.Lock()
		empty := len(q.pending) == 0
		q.mu.Unlock()
		if !empty {
			m.flushSettings()
		}
	}
}

func (m *Module) pendingSettingValue(key string) (daemon.SettingWrite, bool) {
	q := m.queue()
	q.mu.Lock()
	defer q.mu.Unlock()
	entry, ok := q.pending[key]
	return entry.write, ok
}

func (m *Module) settingString(key string) string {
	if write, ok := m.pendingSettingValue(key); ok {
		if write.Remove {
			return ""
		}
		switch value := write.Value.(type) {
		case string:
			return value
		case json.RawMessage:
			return string(value)
		}
		encoded, _ := json.Marshal(write.Value)
		return string(encoded)
	}
	return m.settings().String(key, "")
}

func (m *Module) pendingSettingsCount() int {
	q := m.queue()
	q.mu.Lock()
	defer q.mu.Unlock()
	return len(q.pending)
}

func (m *Module) logCore(format string, args ...any) {
	if m.core != nil && m.core.Logger != nil {
		m.core.Logger.Printf(format, args...)
	}
}
