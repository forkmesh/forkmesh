package actions

import (
	"context"
	"reflect"
	"strings"
	"sync"
	"time"

	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/qsettings"
)

type SettingsWriter func(ctx context.Context, writes []daemon.SettingWrite) error

type settingsOverlay struct {
	mu        sync.Mutex
	writeMu   sync.Mutex
	approvals map[string]map[string]map[string]any
	values    map[string]string
}

func newSettingsOverlay() *settingsOverlay {
	return &settingsOverlay{approvals: map[string]map[string]map[string]any{}, values: map[string]string{}}
}

func (m *Module) settingsWriter() SettingsWriter {
	if m.WriteSettings != nil {
		return m.WriteSettings
	}
	if m.core == nil {
		return func(context.Context, []daemon.SettingWrite) error { return errDesktopUnavailable }
	}
	return m.core.WriteSettings
}

func (m *Module) writeSettings(writes []daemon.SettingWrite) error {
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	return m.settingsWriter()(ctx, writes)
}

func (o *settingsOverlay) queuedApprovals(repoKey string) map[string]map[string]any {
	o.mu.Lock()
	defer o.mu.Unlock()
	out := map[string]map[string]any{}
	for path, approval := range o.approvals[repoKey] {
		out[path] = approval
	}
	return out
}

func (m *Module) storedApprovalsMerged(repoKey string) map[string]any {
	approvals := storedApprovals(m.settings(), repoKey)
	for path, approval := range m.overlay.queuedApprovals(repoKey) {
		approvals[path] = approval
	}
	return approvals
}

func (m *Module) isApproved(repoKey, workflowPath, content, repositoryTree, executionDigest string) bool {
	if repositoryTree == "" || executionDigest == "" {
		return false
	}
	approval, ok := m.storedApprovalsMerged(repoKey)[workflowPath].(map[string]any)
	if !ok {
		return false
	}
	return jsonInt(approval, "schema", 0) == 2 &&
		jsonString(approval, "workflowContent") == content &&
		jsonString(approval, "repositoryTree") == repositoryTree &&
		jsonString(approval, "executionDigest") == executionDigest
}

func (m *Module) approve(repoKey, workflowPath, content, repositoryTree, executionDigest string) {
	if repositoryTree == "" || executionDigest == "" {
		return
	}
	m.overlay.mu.Lock()
	if m.overlay.approvals[repoKey] == nil {
		m.overlay.approvals[repoKey] = map[string]map[string]any{}
	}
	m.overlay.approvals[repoKey][workflowPath] = map[string]any{
		"schema":          float64(2),
		"workflowContent": content,
		"repositoryTree":  repositoryTree,
		"executionDigest": executionDigest,
	}
	m.overlay.mu.Unlock()
	go m.writeQueuedApprovals(repoKey)
}

func (m *Module) writeQueuedApprovals(repoKey string) bool {
	m.overlay.writeMu.Lock()
	defer m.overlay.writeMu.Unlock()
	queued := m.overlay.queuedApprovals(repoKey)
	if len(queued) == 0 {
		return true
	}
	approvals := storedApprovals(m.settings(), repoKey)
	for path, approval := range queued {
		approvals[path] = approval
	}
	if err := m.writeSettings([]daemon.SettingWrite{{Key: approvalKey(repoKey), Type: "json", Value: approvals}}); err != nil {
		m.logf("approval for %s not written yet: %v", repoKey, err)
		return false
	}
	stored := storedApprovals(m.settings(), repoKey)
	m.overlay.mu.Lock()
	defer m.overlay.mu.Unlock()
	entry := m.overlay.approvals[repoKey]
	for path, approval := range queued {
		if reflect.DeepEqual(entry[path], approval) && reflect.DeepEqual(stored[path], approval) {
			delete(entry, path)
		}
	}
	if len(entry) == 0 {
		delete(m.overlay.approvals, repoKey)
	}
	return true
}

func (m *Module) flushQueuedApprovals() {
	m.overlay.mu.Lock()
	keys := make([]string, 0, len(m.overlay.approvals))
	for key := range m.overlay.approvals {
		keys = append(keys, key)
	}
	m.overlay.mu.Unlock()
	for _, key := range keys {
		m.writeQueuedApprovals(key)
	}
}

func (m *Module) pendingApprovalCount() int {
	m.overlay.mu.Lock()
	defer m.overlay.mu.Unlock()
	count := 0
	for _, entry := range m.overlay.approvals {
		count += len(entry)
	}
	return count
}

func (m *Module) settingValue(settings *qsettings.Settings, key string) string {
	m.overlay.mu.Lock()
	value, ok := m.overlay.values[key]
	m.overlay.mu.Unlock()
	if ok {
		return value
	}
	return strings.ToLower(qtTrim(settings.String(key, "")))
}

func (m *Module) persistValues(values map[string]string) bool {
	if len(values) == 0 {
		return true
	}
	writes := make([]daemon.SettingWrite, 0, len(values))
	for _, key := range sortedKeys(values) {
		writes = append(writes, daemon.SettingWrite{Key: key, Type: "string", Value: values[key]})
	}
	if err := m.writeSettings(writes); err != nil {
		m.logf("could not record %d Actions head(s): %v", len(values), err)
		return false
	}
	m.overlay.mu.Lock()
	for key, value := range values {
		m.overlay.values[key] = value
	}
	m.overlay.mu.Unlock()
	return true
}

func (m *Module) forgetValueOverlay() {
	m.overlay.mu.Lock()
	m.overlay.values = map[string]string{}
	m.overlay.mu.Unlock()
}
