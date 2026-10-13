package vault

import (
	"context"
	"crypto/subtle"
	"errors"
	"os"
	"path/filepath"
	"runtime"
	"sort"
	"strconv"
	"sync"
	"time"

	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/identity"
	"forkmesh.com/desktop-core/internal/ipc"
	"forkmesh.com/desktop-core/internal/qsettings"
	"forkmesh.com/desktop-core/internal/rpc"
)

const owner = "vault"

type Module struct {
	Identities    func() []Signer
	WriteSettings func(ctx context.Context, writes []daemon.SettingWrite) error
	DesktopPeer   func(peer rpc.Peer) bool
	Interval      time.Duration
	Finalize      *bool

	core      *daemon.Core
	store     *Store
	migrateMu sync.Mutex

	mu           sync.Mutex
	start        *identity.Identity
	disk         *identity.Identity
	diskStamp    string
	lastIdentity string
	migration    string
	migrationErr string
	copied       map[string]bool
}

func New() *Module { return &Module{} }

func (m *Module) Name() string { return owner }

func StorePath(data string) string { return filepath.Join(data, "vault", "secrets.json") }

func (m *Module) Store() *Store { return m.store }

func (m *Module) Start(ctx context.Context, core *daemon.Core) error {
	m.core = core
	m.store = &Store{Path: StorePath(core.Layout.Data), Signers: m.signers}
	if core.Identity != nil {
		m.start = core.Identity
	}
	m.migration = "unknown"
	local := func(method string, handler rpc.Handler) {
		core.Registry.Handle(method, owner, func(ctx context.Context, req *rpc.Request) (any, error) {
			if req.Peer == nil || rpc.Remote(req.Peer) {
				return nil, rpc.Fail("forbidden")
			}
			return handler(ctx, req)
		})
	}
	desktop := func(method string, handler rpc.Handler) {
		local(method, func(ctx context.Context, req *rpc.Request) (any, error) {
			if !m.desktopPeer(req.Peer) {
				return nil, rpc.Fail("forbidden")
			}
			return handler(ctx, req)
		})
	}
	local("core.vault.status", m.handleStatus)
	local("core.vault.list", m.handleList)
	local("core.vault.migrate", m.handleMigrate)
	local("core.vault.rotate", m.handleRotate)
	desktop("core.vault.get", m.handleGet)
	desktop("core.vault.set", m.handleSet)
	desktop("core.vault.delete", m.handleDelete)
	active.Store(m)
	go m.loop(ctx)
	return nil
}

func (m *Module) Stop() {
	active.CompareAndSwap(m, nil)
}

func (m *Module) desktopPeer(peer rpc.Peer) bool {
	if m.DesktopPeer != nil {
		return m.DesktopPeer(peer)
	}
	if peer == nil || rpc.Remote(peer) {
		return false
	}
	if peer.Transport() == rpc.UI {
		return true
	}
	if peer.Transport() != rpc.CLI || m.core == nil {
		return false
	}
	ui := m.core.UI()
	cli, ok := peer.(*ipc.Conn)
	if ui == nil || !ok {
		return false
	}
	uiPID, uiVerified := ui.PeerPID()
	cliPID, cliVerified := cli.PeerPID()
	if uiVerified && cliVerified {
		return uiPID == cliPID
	}
	if runtime.GOOS == "windows" && !uiVerified && !cliVerified {
		return ui.PID() > 0 && ui.PID() == cli.PID()
	}
	return false
}

func identityStamp(path string) string {
	info, err := os.Stat(path)
	if err != nil {
		return ""
	}
	return info.ModTime().UTC().Format(time.RFC3339Nano) + "/" + strconv.FormatInt(info.Size(), 10)
}

func (m *Module) diskIdentity() *identity.Identity {
	path := m.core.Layout.IdentityKey()
	stamp := identityStamp(path)
	m.mu.Lock()
	if stamp != "" && stamp == m.diskStamp && m.disk != nil {
		disk := m.disk
		m.mu.Unlock()
		return disk
	}
	m.mu.Unlock()
	if stamp == "" {
		return nil
	}
	loaded, err := identity.Load(path)
	if err != nil {
		return nil
	}
	m.mu.Lock()
	m.disk = loaded
	m.diskStamp = stamp
	if m.start == nil {
		m.start = loaded
	}
	m.mu.Unlock()
	return loaded
}

func (m *Module) signers() []Signer {
	if m.Identities != nil {
		return m.Identities()
	}
	out := []Signer{}
	disk := m.diskIdentity()
	if disk != nil {
		out = append(out, disk)
	}
	m.mu.Lock()
	start := m.start
	m.mu.Unlock()
	if start != nil && (disk == nil || start.PublicKey() != disk.PublicKey()) {
		out = append(out, start)
	}
	return out
}

func (m *Module) settings() *qsettings.Settings {
	if m.core == nil || m.core.Settings == nil {
		return nil
	}
	return m.core.Settings.Get()
}

func (m *Module) settingsPath() string { return m.core.Layout.SettingsFile() }

func (m *Module) writeSettings(ctx context.Context, writes []daemon.SettingWrite) error {
	if m.WriteSettings != nil {
		return m.WriteSettings(ctx, writes)
	}
	return m.core.WriteSettings(ctx, writes)
}

func failure(err error) error {
	switch {
	case err == nil:
		return nil
	case errors.Is(err, ErrIdentityMismatch):
		return rpc.Fail("vault_locked")
	case errors.Is(err, ErrTampered):
		return rpc.Fail("vault_tampered")
	case errors.Is(err, ErrNoIdentity):
		return rpc.Fail("no_identity")
	case errors.Is(err, ErrInvalidName):
		return rpc.Fail("invalid_name")
	case errors.Is(err, ErrTooLarge):
		return rpc.Fail("value_too_large")
	case errors.Is(err, ErrFormat):
		return rpc.Fail("vault_invalid")
	}
	var coded *rpc.Error
	if errors.As(err, &coded) {
		return err
	}
	return rpc.Fail("vault_io")
}

func (m *Module) ActionVariables() (map[string]string, error) {
	plain := PendingPlaintext(m.settings())
	confAt := settingsTime(m.settingsPath())
	out := map[string]string{}
	err := m.store.View(func(tx *Tx) error {
		for _, entry := range tx.Names(ActionsNamespace + "/") {
			name, ok := NameOf(ActionsNamespace, entry)
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
			entry := EntryName(ActionsNamespace, name)
			if !tx.Has(entry) || tx.At(entry) < confAt {
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

func (m *Module) namespaceValues(namespace string) (map[string]string, error) {
	switch namespace {
	case ActionsNamespace:
		return m.ActionVariables()
	case DesktopNamespace:
		return m.DesktopSecrets()
	case ReleaseNamespace:
		return m.ReleaseSecrets()
	}
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
		return nil
	})
	return out, err
}

type namespaceParams struct {
	Namespace string            `json:"namespace"`
	Names     []string          `json:"names"`
	Values    map[string]string `json:"values"`
	Delete    []string          `json:"delete"`
	Replace   bool              `json:"replace"`
}

func (m *Module) bind(req *rpc.Request) (namespaceParams, error) {
	var params namespaceParams
	if err := req.Bind(&params); err != nil {
		return params, rpc.Fail("bad_request")
	}
	if params.Namespace == "" {
		params.Namespace = ActionsNamespace
	}
	if !ValidNamespace(params.Namespace) {
		return params, rpc.Fail("bad_request")
	}
	return params, nil
}

func (m *Module) handleStatus(ctx context.Context, req *rpc.Request) (any, error) {
	status := m.store.Status()
	m.mu.Lock()
	migration := map[string]any{"state": m.migration}
	if m.migrationErr != "" {
		migration["error"] = m.migrationErr
	}
	m.mu.Unlock()
	migration["pending"] = len(PendingPlaintext(m.settings()))
	migration["pendingDesktop"] = len(PendingSettingSecrets(m.settings()))
	migration["pendingRelease"] = len(m.pendingReleaseFiles())
	return map[string]any{"vault": status, "migration": migration, "finalized": m.finalized()}, nil
}

func (m *Module) handleList(ctx context.Context, req *rpc.Request) (any, error) {
	params, err := m.bind(req)
	if err != nil {
		return nil, err
	}
	names := []string{}
	if params.Namespace == ActionsNamespace {
		values, err := m.ActionVariables()
		names = SortedKeys(values)
		if err != nil {
			return map[string]any{"names": names, "state": m.store.Status().State}, nil
		}
		return map[string]any{"names": names, "state": "ready"}, nil
	}
	err = m.store.View(func(tx *Tx) error {
		for _, entry := range tx.Names(params.Namespace + "/") {
			if name, ok := NameOf(params.Namespace, entry); ok {
				names = append(names, name)
			}
		}
		return nil
	})
	if err != nil {
		return nil, failure(err)
	}
	return map[string]any{"names": names, "state": "ready"}, nil
}

func (m *Module) handleGet(ctx context.Context, req *rpc.Request) (any, error) {
	params, err := m.bind(req)
	if err != nil {
		return nil, err
	}
	values, err := m.namespaceValues(params.Namespace)
	if err != nil {
		return nil, failure(err)
	}
	if params.Names != nil {
		picked := map[string]string{}
		for _, name := range params.Names {
			if value, ok := values[name]; ok {
				picked[name] = value
			}
		}
		values = picked
	}
	return map[string]any{"values": values}, nil
}

func (m *Module) handleSet(ctx context.Context, req *rpc.Request) (any, error) {
	params, err := m.bind(req)
	if err != nil {
		return nil, err
	}
	changed, err := m.Apply(params.Namespace, params.Values, params.Delete, params.Replace)
	if err != nil {
		return nil, failure(err)
	}
	return map[string]any{"ok": true, "changed": changed, "finalized": m.finalized()}, nil
}

func (m *Module) handleDelete(ctx context.Context, req *rpc.Request) (any, error) {
	params, err := m.bind(req)
	if err != nil {
		return nil, err
	}
	changed, err := m.Apply(params.Namespace, nil, params.Names, false)
	if err != nil {
		return nil, failure(err)
	}
	return map[string]any{"ok": true, "changed": changed, "finalized": m.finalized()}, nil
}

func (m *Module) handleMigrate(ctx context.Context, req *rpc.Request) (any, error) {
	released, releaseErr := m.MigrateReleaseFiles()
	desktop, desktopErr := m.MigrateDesktopSecrets(ctx)
	result, err := m.Migrate(ctx)
	if err == nil {
		err = desktopErr
	}
	if err == nil {
		err = releaseErr
	}
	if err != nil {
		return nil, failure(err)
	}
	result["desktop"] = desktop
	result["release"] = released
	return result, nil
}

func (m *Module) handleRotate(ctx context.Context, req *rpc.Request) (any, error) {
	keyID, err := m.store.Rotate()
	if err != nil {
		return nil, failure(err)
	}
	if keyID != "" {
		m.core.LogUI("Rotated the vault data key (now %s)", keyID)
	}
	return map[string]any{"keyId": keyID}, nil
}

func (m *Module) Apply(namespace string, values map[string]string, remove []string, replace bool) ([]string, error) {
	if !ValidNamespace(namespace) {
		return nil, ErrInvalidName
	}
	for name, value := range values {
		if name == "" || !ValidName(EntryName(namespace, name)) {
			return nil, ErrInvalidName
		}
		if len(value) > MaxValueBytes {
			return nil, ErrTooLarge
		}
	}
	changed := []string{}
	err := m.store.Update(func(tx *Tx) error {
		for _, name := range SortedKeys(values) {
			entry := EntryName(namespace, name)
			next := []byte(values[name])
			current, ok, err := tx.Get(entry)
			same := err == nil && ok && subtle.ConstantTimeCompare(current, next) == 1
			zero(current)
			if same {
				zero(next)
				continue
			}
			if err := tx.Set(entry, next); err != nil {
				zero(next)
				return err
			}
			zero(next)
			changed = append(changed, name)
		}
		drop := append([]string{}, remove...)
		if replace {
			for _, entry := range tx.Names(namespace + "/") {
				name, ok := NameOf(namespace, entry)
				if !ok {
					continue
				}
				if _, keep := values[name]; !keep {
					drop = append(drop, name)
				}
			}
		}
		for _, name := range drop {
			if tx.Delete(EntryName(namespace, name)) {
				changed = append(changed, name)
			}
		}
		return nil
	})
	if err != nil {
		return nil, err
	}
	if len(changed) > 0 {
		sort.Strings(changed)
		m.broadcast(namespace, changed)
	}
	return changed, nil
}

func (m *Module) broadcast(namespace string, names []string) {
	if m.core == nil {
		return
	}
	m.core.BroadcastUI("vault.changed", map[string]any{"namespace": namespace, "names": names})
}

func (m *Module) setMigration(state, code string) {
	m.mu.Lock()
	m.migration = state
	m.migrationErr = code
	m.mu.Unlock()
}

func sameMap(a, b map[string]string) bool {
	if len(a) != len(b) {
		return false
	}
	for key, value := range a {
		other, ok := b[key]
		if !ok || subtle.ConstantTimeCompare([]byte(value), []byte(other)) != 1 {
			return false
		}
	}
	return true
}

func (m *Module) Migrate(ctx context.Context) (map[string]any, error) {
	m.migrateMu.Lock()
	defer m.migrateMu.Unlock()
	for attempt := 0; attempt < 3; attempt++ {
		pending := PendingPlaintext(m.settings())
		if len(pending) == 0 {
			m.setMigration("done", "")
			return map[string]any{"state": "done", "moved": 0}, nil
		}
		confAt := settingsTime(m.settingsPath())
		err := m.store.Update(func(tx *Tx) error {
			for _, name := range SortedKeys(pending) {
				entry := EntryName(ActionsNamespace, name)
				if tx.Has(entry) && tx.At(entry) >= confAt {
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
		if err != nil {
			m.setMigration("failed", failure(err).Error())
			m.core.Logger.Printf("vault: migration stopped: %v", err)
			return nil, err
		}
		markers := map[string]any{}
		verified := true
		err = m.store.View(func(tx *Tx) error {
			for _, name := range SortedKeys(pending) {
				entry := EntryName(ActionsNamespace, name)
				stored, ok, err := tx.Get(entry)
				if err != nil {
					return err
				}
				newer := ok && tx.At(entry) >= confAt
				if !ok || (!newer && subtle.ConstantTimeCompare(stored, []byte(pending[name])) != 1) {
					verified = false
				}
				zero(stored)
			}
			for _, entry := range tx.Names(ActionsNamespace + "/") {
				if name, ok := NameOf(ActionsNamespace, entry); ok {
					markers[name] = Marker
				}
			}
			return nil
		})
		if err != nil || !verified {
			if err == nil {
				err = ErrTampered
			}
			m.setMigration("failed", failure(err).Error())
			m.core.Logger.Printf("vault: migration verification failed: %v", err)
			return nil, err
		}
		if !sameMap(PendingPlaintext(m.settings()), pending) {
			continue
		}
		if !m.finalized() {
			m.setMigration("copied", "")
			m.noteCopied(ActionsNamespace, SortedKeys(pending))
			return map[string]any{"state": "copied", "moved": len(pending)}, nil
		}
		if err := m.writeSettings(ctx, []daemon.SettingWrite{{Key: VariablesSetting, Type: "json", Value: markers}}); err != nil {
			m.setMigration("pending", failure(err).Error())
			m.core.Logger.Printf("vault: %d Actions variables are in the vault; removing them from ForkMesh.conf waits for the desktop (%v)", len(pending), err)
			return nil, err
		}
		m.setMigration("done", "")
		m.core.LogUI("Moved %d Actions variables into the encrypted vault (%s); ForkMesh.conf now keeps only their names", len(pending), m.store.Path)
		m.broadcast(ActionsNamespace, SortedKeys(pending))
		return map[string]any{"state": "done", "moved": len(pending)}, nil
	}
	m.setMigration("pending", "vault_busy")
	return nil, rpc.Fail("vault_busy")
}

func (m *Module) checkIdentity() {
	disk := m.diskIdentity()
	if disk == nil {
		return
	}
	m.mu.Lock()
	previous := m.lastIdentity
	m.lastIdentity = disk.PublicKey()
	m.mu.Unlock()
	if previous == "" || previous == disk.PublicKey() || !m.store.Exists() {
		return
	}
	if err := m.store.Update(func(tx *Tx) error { return nil }); err != nil {
		m.core.LogUI("The node identity changed and the vault could not be opened for it (%s); restore the previous identity key to read the stored secrets", failure(err).Error())
		return
	}
	m.core.LogUI("The node identity changed; the vault key is now wrapped for the new identity too")
}

func (m *Module) loop(ctx context.Context) {
	interval := m.Interval
	if interval <= 0 {
		interval = 5 * time.Second
	}
	ticker := time.NewTicker(interval)
	defer ticker.Stop()
	var lastUI *ipc.Conn
	var lastTry, lastRelease time.Time
	m.checkIdentity()
	for {
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
		}
		m.checkIdentity()
		if len(m.pendingReleaseFiles()) > 0 && time.Since(lastRelease) >= time.Minute {
			lastRelease = time.Now()
			_, _ = m.MigrateReleaseFiles()
		}
		ui := m.core.UI()
		if ui == nil || (len(PendingPlaintext(m.settings())) == 0 && len(PendingSettingSecrets(m.settings())) == 0) {
			continue
		}
		if ui == lastUI && time.Since(lastTry) < time.Minute {
			continue
		}
		lastUI, lastTry = ui, time.Now()
		attempt, cancel := context.WithTimeout(ctx, 30*time.Second)
		if len(PendingPlaintext(m.settings())) > 0 {
			_, _ = m.Migrate(attempt)
		}
		_, _ = m.MigrateDesktopSecrets(attempt)
		cancel()
	}
}
