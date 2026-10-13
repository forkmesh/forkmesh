package hosts

import (
	"bytes"
	"encoding/json"
	"sort"
	"strings"

	"forkmesh.com/desktop-core/internal/daemon"
)

const (
	decommissionedNodesSetting = "hosts/decommissionedNodes"
	deletedMeshNodesSetting    = "nodes/deletedNodes"
	decommissionedNodeTTLMs    = 30 * 24 * 3600 * 1000
	decommissionedNodeGraceMs  = 10 * 60 * 1000
	decommissionedNodeLimit    = 200
	savedHostInstallingStaleMs = 2 * 60 * 60 * 1000
)

var preservedHostFields = []string{"provider", "planType", "displayName", "dnsHostname", "publicOrigin", "region", "monthlyCost", "ramMb", "diskGb", "instanceId"}

func compactJSON(value any) string {
	var buffer bytes.Buffer
	encoder := json.NewEncoder(&buffer)
	encoder.SetEscapeHTML(false)
	_ = encoder.Encode(value)
	return strings.TrimRight(buffer.String(), "\n")
}

func (m *Module) rawSavedHosts() []any {
	var stored []any
	if err := json.Unmarshal([]byte(m.settingString(hostsSetting)), &stored); err != nil {
		return []any{}
	}
	for index, value := range stored {
		host, ok := value.(map[string]any)
		if !ok {
			continue
		}
		password := ""
		for _, field := range secretHostFields {
			if secret, present := host[field]; present {
				if password == "" {
					password = jsonString(secret)
				}
				delete(host, field)
			}
		}
		if password != "" {
			m.mu.Lock()
			m.passwords[credentialKey(jsonString(host["name"]), jsonString(host["ip"]), jsonString(host["user"]))] = password
			m.mu.Unlock()
		}
		stored[index] = host
	}
	return stored
}

func objectAt(hosts []any, index int) map[string]any {
	if index < 0 || index >= len(hosts) {
		return map[string]any{}
	}
	if host, ok := hosts[index].(map[string]any); ok {
		return host
	}
	return map[string]any{}
}

func scrubbedForWrite(hosts []any) []any {
	out := make([]any, 0, len(hosts))
	for _, value := range hosts {
		host, ok := value.(map[string]any)
		if !ok {
			out = append(out, value)
			continue
		}
		clean := map[string]any{}
		for key, field := range host {
			secret := false
			for _, name := range secretHostFields {
				if key == name {
					secret = true
					break
				}
			}
			if !secret {
				clean[key] = field
			}
		}
		out = append(out, clean)
	}
	return out
}

func (m *Module) saveSavedHosts(hosts []any) {
	m.writeSettings(daemon.SettingWrite{Key: hostsSetting, Type: "string", Value: compactJSON(scrubbedForWrite(hosts))})
}

func mergeSavedHostRecord(previous, entry, metadata map[string]any) map[string]any {
	if _, has := entry["identityFile"]; !has {
		if value, ok := previous["identityFile"]; ok {
			entry["identityFile"] = value
		}
	}
	for _, field := range preservedHostFields {
		if value, ok := previous[field]; ok {
			entry[field] = value
		}
	}
	for key, value := range metadata {
		entry[key] = value
	}
	return entry
}

func isFleetInstanceHost(name, address string, fleetAddresses []string) bool {
	if strings.HasPrefix(strings.ToLower(strings.TrimSpace(name)), "fmnext-") {
		return true
	}
	trimmed := strings.TrimSpace(address)
	if trimmed == "" {
		return false
	}
	for _, candidate := range fleetAddresses {
		if candidate == trimmed {
			return true
		}
	}
	return false
}

func (m *Module) rememberHost(name, ip, user, pass, status, identityFile string, metadata map[string]any) bool {
	if isFleetInstanceHost(name, ip, m.cachedEnv().FleetAddresses) {
		return false
	}
	m.savedMu.Lock()
	defer m.savedMu.Unlock()
	hosts := m.rawSavedHosts()
	entry := map[string]any{
		"name":            name,
		"ip":              ip,
		"user":            user,
		"status":          status,
		"statusChangedAt": float64(m.nowMs()),
	}
	if strings.TrimSpace(identityFile) != "" {
		entry["identityFile"] = strings.TrimSpace(identityFile)
	}
	replaced := false
	for i := range hosts {
		old := objectAt(hosts, i)
		if jsonString(old["name"]) != name {
			continue
		}
		if changedAt, has := old["statusChangedAt"]; has && jsonString(old["status"]) == status {
			entry["statusChangedAt"] = changedAt
		}
		entry = mergeSavedHostRecord(old, entry, metadata)
		oldKey := credentialKey(jsonString(old["name"]), jsonString(old["ip"]), jsonString(old["user"]))
		if oldKey != credentialKey(name, ip, user) {
			m.mu.Lock()
			delete(m.passwords, oldKey)
			m.mu.Unlock()
		}
		hosts[i] = entry
		replaced = true
		break
	}
	if !replaced {
		hosts = append(hosts, mergeSavedHostRecord(map[string]any{}, entry, metadata))
	}
	key := credentialKey(name, ip, user)
	m.mu.Lock()
	delete(m.passwords, key)
	if pass != "" {
		m.passwords[key] = pass
	}
	m.mu.Unlock()
	m.saveSavedHosts(hosts)
	m.forgetLedgerEntry(decommissionedNodesSetting, name)
	m.forgetLedgerEntry(deletedMeshNodesSetting, name)
	return true
}

func (m *Module) forgetHostNamed(node string) bool {
	m.savedMu.Lock()
	hosts := m.rawSavedHosts()
	changed := false
	for i := len(hosts) - 1; i >= 0; i-- {
		host := objectAt(hosts, i)
		if !strings.EqualFold(strings.TrimSpace(jsonString(host["name"])), node) {
			continue
		}
		m.mu.Lock()
		delete(m.passwords, credentialKey(jsonString(host["name"]), jsonString(host["ip"]), jsonString(host["user"])))
		m.mu.Unlock()
		hosts = append(hosts[:i], hosts[i+1:]...)
		changed = true
	}
	if changed {
		m.saveSavedHosts(hosts)
	}
	m.savedMu.Unlock()
	if !changed {
		return false
	}
	m.recordLedgerEntry(decommissionedNodesSetting, node)
	return true
}

func (m *Module) forgetHostAt(index int) (map[string]any, bool) {
	m.savedMu.Lock()
	hosts := m.rawSavedHosts()
	if index < 0 || index >= len(hosts) {
		m.savedMu.Unlock()
		return nil, false
	}
	removed := objectAt(hosts, index)
	hosts = append(hosts[:index], hosts[index+1:]...)
	m.mu.Lock()
	delete(m.passwords, credentialKey(jsonString(removed["name"]), jsonString(removed["ip"]), jsonString(removed["user"])))
	m.mu.Unlock()
	m.saveSavedHosts(hosts)
	m.savedMu.Unlock()
	m.recordLedgerEntry(decommissionedNodesSetting, jsonString(removed["name"]))
	m.setStatus("install", "Removed \""+jsonString(removed["name"])+"\" from the saved hosts list.")
	return removed, true
}

func (m *Module) ledger(key string) map[string]any {
	ledger := map[string]any{}
	_ = json.Unmarshal([]byte(m.settingString(key)), &ledger)
	if ledger == nil {
		ledger = map[string]any{}
	}
	return ledger
}

func pruneLedger(ledger map[string]any, nowMs int64) map[string]any {
	type kept struct {
		stamp int64
		key   string
	}
	list := []kept{}
	for key, value := range ledger {
		stamp := int64(jsonDouble(value))
		if stamp <= 0 || nowMs-stamp > decommissionedNodeTTLMs {
			continue
		}
		list = append(list, kept{stamp, key})
	}
	sort.Slice(list, func(i, j int) bool {
		if list[i].stamp != list[j].stamp {
			return list[i].stamp > list[j].stamp
		}
		return list[i].key < list[j].key
	})
	out := map[string]any{}
	for i := 0; i < len(list) && i < decommissionedNodeLimit; i++ {
		out[list[i].key] = float64(list[i].stamp)
	}
	return out
}

func recordLedger(ledger map[string]any, node string, nowMs int64) map[string]any {
	key := strings.ToLower(strings.TrimSpace(node))
	if key == "" || nowMs <= 0 {
		return ledger
	}
	updated := map[string]any{}
	for k, v := range ledger {
		updated[k] = v
	}
	updated[key] = float64(nowMs)
	return pruneLedger(updated, nowMs)
}

func ledgersEqual(a, b map[string]any) bool { return compactJSON(a) == compactJSON(b) }

func (m *Module) recordLedgerEntry(setting, node string) {
	current := m.ledger(setting)
	updated := recordLedger(current, node, m.nowMs())
	if ledgersEqual(current, updated) {
		return
	}
	m.writeSettings(daemon.SettingWrite{Key: setting, Type: "string", Value: compactJSON(updated)})
}

func (m *Module) forgetLedgerEntry(setting, node string) {
	key := strings.ToLower(strings.TrimSpace(node))
	current := m.ledger(setting)
	if _, present := current[key]; key == "" || !present {
		return
	}
	delete(current, key)
	m.writeSettings(daemon.SettingWrite{Key: setting, Type: "string", Value: compactJSON(current)})
}

func savedHostVultrInstanceID(host map[string]any) string {
	if !strings.EqualFold(strings.TrimSpace(jsonString(host["provider"])), "Vultr") {
		return ""
	}
	return strings.TrimSpace(jsonString(host["instanceId"]))
}

func savedHostMayBeActive(host map[string]any) bool {
	status := "installed"
	if value, ok := host["status"].(string); ok {
		status = value
	}
	switch strings.ToLower(strings.TrimSpace(status)) {
	case "added", "installing", "install failed", "uninstalling", "uninstalled":
		return false
	}
	return true
}

func (m *Module) savedHostByName(name string) (map[string]any, int) {
	hosts := m.rawSavedHosts()
	for i := range hosts {
		host := objectAt(hosts, i)
		if lowerHost(jsonString(host["name"])) == lowerHost(name) {
			return host, i
		}
	}
	return nil, -1
}

func (m *Module) savedHostExact(name string) (map[string]any, int) {
	hosts := m.rawSavedHosts()
	for i := range hosts {
		host := objectAt(hosts, i)
		if jsonString(host["name"]) == name {
			return host, i
		}
	}
	return nil, -1
}
