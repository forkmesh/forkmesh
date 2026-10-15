package vault

import (
	"os"
	"strings"
)

const FinalizeSetting = "core/vaultFinalize"

func (m *Module) finalized() bool {
	if value := strings.TrimSpace(os.Getenv("FORKMESH_VAULT_FINALIZE")); value != "" {
		return value == "1" || strings.EqualFold(value, "true") || strings.EqualFold(value, "yes")
	}
	if m.Finalize != nil {
		return *m.Finalize
	}
	settings := m.settings()
	return settings != nil && settings.Bool(FinalizeSetting, false)
}

func (m *Module) noteCopied(namespace string, names []string) {
	key := namespace + ":" + strings.Join(names, ",")
	m.mu.Lock()
	if m.copied == nil {
		m.copied = map[string]bool{}
	}
	seen := m.copied[key]
	m.copied[key] = true
	m.mu.Unlock()
	if seen || m.core == nil {
		return
	}
	m.core.LogUI("Copied %d %s secrets into the encrypted vault (%s); the plaintext copies stay in place until the vault is finalized (%s=true or FORKMESH_VAULT_FINALIZE=1), because the unchanged desktop and tools still read them", len(names), namespace, m.store.Path, FinalizeSetting)
}
