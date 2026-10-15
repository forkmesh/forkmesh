package vault

import (
	"context"
	"crypto/subtle"
	"io"
	"os"
	"path/filepath"
	"strings"

	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/qsettings"
	"forkmesh.com/desktop-core/internal/rpc"
)

const (
	DesktopNamespace  = "desktop"
	ReleaseNamespace  = "release"
	SealKeySetting    = "chat/e2ee/x25519PrivateV2"
	GenieTokenSetting = "genie/token"
	maxReleaseBytes   = 64 << 10
)

var ErrBusy = rpc.Fail("vault_busy")

var SettingSecrets = []string{SealKeySetting, GenieTokenSetting}

var ReleaseFiles = map[string]string{
	"apk-keystore-password": ".apk-keystore-password",
	"windows-cert-password": ".windows-cert-password",
}

func settingText(settings *qsettings.Settings, key string) string {
	if settings == nil {
		return ""
	}
	value, ok := settings.Value(key)
	if !ok {
		return ""
	}
	return value.String()
}

func PendingSettingSecrets(settings *qsettings.Settings) map[string]string {
	out := map[string]string{}
	for _, key := range SettingSecrets {
		text := settingText(settings, key)
		if text != "" && !IsMarker(text) {
			out[key] = text
		}
	}
	return out
}

func ReleaseDir(data string) string { return filepath.Join(data, "release") }

type releaseFile struct {
	path  string
	value string
	at    int64
	mode  os.FileMode
}

func readReleaseFile(path string) (releaseFile, bool) {
	info, err := os.Lstat(path)
	if err != nil || !info.Mode().IsRegular() || info.Size() > maxReleaseBytes {
		return releaseFile{}, false
	}
	handle, err := os.Open(path)
	if err != nil {
		return releaseFile{}, false
	}
	defer handle.Close()
	raw, err := io.ReadAll(io.LimitReader(handle, maxReleaseBytes+1))
	if err != nil || len(raw) > maxReleaseBytes {
		return releaseFile{}, false
	}
	text := string(raw)
	zero(raw)
	if strings.TrimSpace(text) == "" || IsMarker(strings.TrimSpace(text)) {
		return releaseFile{}, false
	}
	return releaseFile{path: path, value: text, at: info.ModTime().UnixMilli(), mode: info.Mode().Perm()}, true
}

func (m *Module) releaseDir() string { return ReleaseDir(m.core.Layout.Data) }

func (m *Module) pendingReleaseFiles() map[string]releaseFile {
	out := map[string]releaseFile{}
	if m.core == nil {
		return out
	}
	for name, file := range ReleaseFiles {
		if found, ok := readReleaseFile(filepath.Join(m.releaseDir(), file)); ok {
			out[name] = found
		}
	}
	return out
}

func (m *Module) overlay(namespace string, plain map[string]string, at func(name string) int64) (map[string]string, error) {
	out := map[string]string{}
	err := m.store.View(func(tx *Tx) error {
		for _, entry := range tx.Names(namespace + "/") {
			name, ok := NameOf(namespace, entry)
			if !ok {
				continue
			}
			value, _, err := tx.Get(entry)
			if err != nil {
				return err
			}
			out[name] = string(value)
			zero(value)
		}
		for name, value := range plain {
			entry := EntryName(namespace, name)
			if !tx.Has(entry) || tx.At(entry) < at(name) {
				out[name] = value
			}
		}
		return nil
	})
	if err != nil {
		return plain, err
	}
	return out, nil
}

func (m *Module) DesktopSecrets() (map[string]string, error) {
	plain := PendingSettingSecrets(m.settings())
	confAt := settingsTime(m.settingsPath())
	return m.overlay(DesktopNamespace, plain, func(string) int64 { return confAt })
}

func (m *Module) ReleaseSecrets() (map[string]string, error) {
	files := m.pendingReleaseFiles()
	plain := map[string]string{}
	for name, file := range files {
		plain[name] = file.value
	}
	return m.overlay(ReleaseNamespace, plain, func(name string) int64 { return files[name].at })
}

func Secret(core *daemon.Core, namespace, name string) string {
	if core == nil {
		return ""
	}
	module, ok := core.Module(owner).(*Module)
	if ok && module != nil && module.store != nil {
		var values map[string]string
		switch namespace {
		case DesktopNamespace:
			values, _ = module.DesktopSecrets()
		case ReleaseNamespace:
			values, _ = module.ReleaseSecrets()
		default:
			values, _ = module.namespaceValues(namespace)
		}
		return values[name]
	}
	if namespace == DesktopNamespace && core.Settings != nil {
		return PendingSettingSecrets(core.Settings.Get())[name]
	}
	return ""
}

func (m *Module) seal(namespace string, pending map[string]string, at func(name string) int64) error {
	return m.store.Update(func(tx *Tx) error {
		for _, name := range SortedKeys(pending) {
			entry := EntryName(namespace, name)
			if tx.Has(entry) && tx.At(entry) >= at(name) {
				continue
			}
			next := []byte(pending[name])
			current, ok, err := tx.Get(entry)
			same := err == nil && ok && subtle.ConstantTimeCompare(current, next) == 1
			zero(current)
			if !same {
				if err := tx.Set(entry, next); err != nil {
					zero(next)
					return err
				}
			}
			zero(next)
		}
		return nil
	})
}

func (m *Module) verify(namespace string, pending map[string]string, at func(name string) int64) error {
	verified := true
	err := m.store.View(func(tx *Tx) error {
		for _, name := range SortedKeys(pending) {
			entry := EntryName(namespace, name)
			stored, ok, err := tx.Get(entry)
			if err != nil {
				return err
			}
			newer := ok && tx.At(entry) >= at(name)
			if !ok || (!newer && subtle.ConstantTimeCompare(stored, []byte(pending[name])) != 1) {
				verified = false
			}
			zero(stored)
		}
		return nil
	})
	if err == nil && !verified {
		err = ErrTampered
	}
	return err
}

func (m *Module) MigrateDesktopSecrets(ctx context.Context) (int, error) {
	m.migrateMu.Lock()
	defer m.migrateMu.Unlock()
	for attempt := 0; attempt < 3; attempt++ {
		pending := PendingSettingSecrets(m.settings())
		if len(pending) == 0 {
			return 0, nil
		}
		confAt := settingsTime(m.settingsPath())
		at := func(string) int64 { return confAt }
		if err := m.seal(DesktopNamespace, pending, at); err != nil {
			m.core.Logger.Printf("vault: moving desktop secrets stopped: %v", err)
			return 0, err
		}
		if err := m.verify(DesktopNamespace, pending, at); err != nil {
			m.core.Logger.Printf("vault: desktop secrets verification failed: %v", err)
			return 0, err
		}
		if !sameMap(PendingSettingSecrets(m.settings()), pending) {
			continue
		}
		if !m.finalized() {
			m.noteCopied(DesktopNamespace, SortedKeys(pending))
			return len(pending), nil
		}
		writes := make([]daemon.SettingWrite, 0, len(pending))
		for _, key := range SortedKeys(pending) {
			writes = append(writes, daemon.SettingWrite{Key: key, Type: "string", Value: Marker})
		}
		if err := m.writeSettings(ctx, writes); err != nil {
			m.core.Logger.Printf("vault: %d desktop secrets are in the vault; removing them from ForkMesh.conf waits for the desktop (%v)", len(pending), err)
			return 0, err
		}
		m.core.LogUI("Moved %d desktop secrets (%s) into the encrypted vault (%s); ForkMesh.conf now keeps only a marker", len(pending), strings.Join(SortedKeys(pending), ", "), m.store.Path)
		m.broadcast(DesktopNamespace, SortedKeys(pending))
		return len(pending), nil
	}
	return 0, ErrBusy
}

func replaceWithMarker(file releaseFile) error {
	dir := filepath.Dir(file.path)
	temp, err := os.CreateTemp(dir, ".vault-marker-*.tmp")
	if err != nil {
		return err
	}
	name := temp.Name()
	committed := false
	defer func() {
		if !committed {
			_ = os.Remove(name)
		}
	}()
	mode := file.mode
	if mode == 0 {
		mode = 0o600
	}
	if err := temp.Chmod(mode); err != nil && !isWindows() {
		_ = temp.Close()
		return err
	}
	if _, err := temp.WriteString(Marker + "\n"); err != nil {
		_ = temp.Close()
		return err
	}
	if err := temp.Sync(); err != nil {
		_ = temp.Close()
		return err
	}
	if err := temp.Close(); err != nil {
		return err
	}
	if info, err := os.Lstat(file.path); err != nil || !info.Mode().IsRegular() || info.ModTime().UnixMilli() != file.at {
		return ErrBusy
	}
	if err := os.Rename(name, file.path); err != nil {
		return err
	}
	committed = true
	syncDir(dir)
	return nil
}

func (m *Module) MigrateReleaseFiles() (int, error) {
	m.migrateMu.Lock()
	defer m.migrateMu.Unlock()
	files := m.pendingReleaseFiles()
	if len(files) == 0 {
		return 0, nil
	}
	pending := map[string]string{}
	for name, file := range files {
		pending[name] = file.value
	}
	at := func(name string) int64 { return files[name].at }
	if err := m.seal(ReleaseNamespace, pending, at); err != nil {
		m.core.Logger.Printf("vault: moving release passwords stopped: %v", err)
		return 0, err
	}
	if err := m.verify(ReleaseNamespace, pending, at); err != nil {
		m.core.Logger.Printf("vault: release passwords verification failed: %v", err)
		return 0, err
	}
	if !m.finalized() {
		m.noteCopied(ReleaseNamespace, SortedKeys(files))
		return len(files), nil
	}
	moved := []string{}
	for _, name := range SortedKeys(files) {
		if err := replaceWithMarker(files[name]); err != nil {
			m.core.Logger.Printf("vault: %s is sealed in the vault but its file was left as it is (%v)", files[name].path, err)
			continue
		}
		moved = append(moved, name)
	}
	if len(moved) == 0 {
		return 0, nil
	}
	m.core.LogUI("Moved %d release signing passwords (%s) into the encrypted vault (%s); %s keeps only markers", len(moved), strings.Join(moved, ", "), m.store.Path, m.releaseDir())
	m.broadcast(ReleaseNamespace, moved)
	return len(moved), nil
}
