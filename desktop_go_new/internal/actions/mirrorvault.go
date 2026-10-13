package actions

import (
	"encoding/base64"
	"os"
	"sort"
	"strings"

	"forkmesh.com/desktop-core/internal/qsettings"
	"forkmesh.com/desktop-core/internal/vault"
)

const (
	mirrorRollbackNamespace = "mirror-rollback"
	mirrorTxnNamespace      = "mirror-txn"
	mirrorTxnValuePrefix    = "g:"
)

type mirrorTxn struct {
	requestID          string
	previousGeneration string
}

func (env MirrorConfigEnv) vaultStore() *vault.Store {
	if env.Vault == nil {
		return nil
	}
	return env.Vault()
}

func vaultUsable(store *vault.Store) bool {
	if store == nil {
		return false
	}
	state := store.Status().State
	return state == "empty" || state == "ready"
}

func (env MirrorConfigEnv) changed(names []string) {
	if env.VaultChanged != nil && len(names) > 0 {
		sort.Strings(names)
		env.VaultChanged(names)
	}
}

func settingsFileTime(path string) int64 {
	info, err := os.Stat(path)
	if err != nil {
		return 0
	}
	return info.ModTime().UnixMilli()
}

func effectiveVariables(tx *vault.Tx, settings *qsettings.Settings, confAt int64) (map[string]string, error) {
	out := map[string]string{}
	for _, entry := range tx.Names(vault.ActionsNamespace + "/") {
		name, ok := vault.NameOf(vault.ActionsNamespace, entry)
		if !ok {
			continue
		}
		value, _, err := tx.Get(entry)
		if err != nil {
			return nil, err
		}
		out[name] = string(value)
	}
	for name, value := range vault.PendingPlaintext(settings) {
		entry := vault.EntryName(vault.ActionsNamespace, name)
		if !tx.Has(entry) || tx.At(entry) < confAt {
			out[name] = value
		}
	}
	return out, nil
}

func markerMap(names map[string]string) map[string]any {
	out := map[string]any{}
	for name := range names {
		out[name] = vault.Marker
	}
	return out
}

func markerSnapshotValue(names map[string]string) map[string]any {
	return map[string]any{"type": "bytes", "value": base64.RawURLEncoding.EncodeToString(qtJSONCompact(markerMap(names)))}
}

func clearNamespace(tx *vault.Tx, namespace string) []string {
	var dropped []string
	for _, entry := range tx.Names(namespace + "/") {
		if tx.Delete(entry) {
			if name, ok := vault.NameOf(namespace, entry); ok {
				dropped = append(dropped, name)
			}
		}
	}
	return dropped
}

func readMirrorTxn(tx *vault.Tx) (mirrorTxn, bool, error) {
	for _, entry := range tx.Names(mirrorTxnNamespace + "/") {
		requestID, ok := vault.NameOf(mirrorTxnNamespace, entry)
		if !ok {
			continue
		}
		value, _, err := tx.Get(entry)
		if err != nil {
			return mirrorTxn{}, false, err
		}
		return mirrorTxn{requestID: requestID, previousGeneration: strings.TrimPrefix(string(value), mirrorTxnValuePrefix)}, true, nil
	}
	return mirrorTxn{}, false, nil
}

func (env MirrorConfigEnv) sealMirrorVariables(settings *qsettings.Settings, requestID string, next map[string]string) (map[string]string, error) {
	store := env.vaultStore()
	previousGeneration := qtTrim(settings.String(mirrorGenerationSetting, ""))
	confAt := settingsFileTime(env.SettingsPath)
	var previous map[string]string
	var changed []string
	err := store.Update(func(tx *vault.Tx) error {
		clearNamespace(tx, mirrorRollbackNamespace)
		clearNamespace(tx, mirrorTxnNamespace)
		current, err := effectiveVariables(tx, settings, confAt)
		if err != nil {
			return err
		}
		previous = current
		for _, name := range vault.SortedKeys(current) {
			if err := tx.Set(vault.EntryName(mirrorRollbackNamespace, name), []byte(current[name])); err != nil {
				return err
			}
		}
		for _, entry := range tx.Names(vault.ActionsNamespace + "/") {
			name, ok := vault.NameOf(vault.ActionsNamespace, entry)
			if _, keep := next[name]; ok && !keep && tx.Delete(entry) {
				changed = append(changed, name)
			}
		}
		for _, name := range vault.SortedKeys(next) {
			if err := tx.Set(vault.EntryName(vault.ActionsNamespace, name), []byte(next[name])); err != nil {
				return err
			}
			changed = append(changed, name)
		}
		return tx.Set(vault.EntryName(mirrorTxnNamespace, requestID), []byte(mirrorTxnValuePrefix+previousGeneration))
	})
	if err != nil {
		return nil, err
	}
	env.changed(changed)
	return previous, nil
}

func (env MirrorConfigEnv) commitMirrorTxn() error {
	store := env.vaultStore()
	if store == nil || !store.Exists() {
		return nil
	}
	return store.Update(func(tx *vault.Tx) error {
		clearNamespace(tx, mirrorRollbackNamespace)
		clearNamespace(tx, mirrorTxnNamespace)
		return nil
	})
}

func (env MirrorConfigEnv) rollbackMirrorTxn() error {
	store := env.vaultStore()
	var changed []string
	err := store.Update(func(tx *vault.Tx) error {
		changed = clearNamespace(tx, vault.ActionsNamespace)
		for _, entry := range tx.Names(mirrorRollbackNamespace + "/") {
			name, ok := vault.NameOf(mirrorRollbackNamespace, entry)
			if !ok {
				continue
			}
			value, _, err := tx.Get(entry)
			if err != nil {
				return err
			}
			if err := tx.Set(vault.EntryName(vault.ActionsNamespace, name), value); err != nil {
				return err
			}
			changed = append(changed, name)
		}
		clearNamespace(tx, mirrorRollbackNamespace)
		clearNamespace(tx, mirrorTxnNamespace)
		return nil
	})
	if err == nil {
		env.changed(changed)
	}
	return err
}

func (env MirrorConfigEnv) pendingMirrorTxn() (mirrorTxn, bool, error) {
	store := env.vaultStore()
	if store == nil || !store.Exists() {
		return mirrorTxn{}, false, nil
	}
	var txn mirrorTxn
	var found bool
	err := store.View(func(tx *vault.Tx) error {
		var err error
		txn, found, err = readMirrorTxn(tx)
		return err
	})
	return txn, found, err
}

func (env MirrorConfigEnv) reconcileMirrorVault(journalRequestID string, journalPresent bool) string {
	txn, found, err := env.pendingMirrorTxn()
	if err != nil || !found {
		return ""
	}
	rollback := false
	if journalPresent && journalRequestID == txn.requestID {
		rollback = true
	} else {
		generation := qtTrim(env.load().String(mirrorGenerationSetting, ""))
		rollback = generation != txn.requestID && generation == txn.previousGeneration
	}
	if rollback {
		if env.rollbackMirrorTxn() != nil {
			return "configuration_vault_rollback_failed"
		}
		return ""
	}
	if env.commitMirrorTxn() != nil {
		return "configuration_vault_rollback_failed"
	}
	return ""
}
