package plugins

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"sync"
	"time"

	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/identity"
	"forkmesh.com/desktop-core/internal/phonefamily"
	"forkmesh.com/desktop-core/internal/rpc"
)

const (
	StateInvalid      = "invalid"
	StateUntrusted    = "untrusted"
	StateNeedsConsent = "needs_consent"
	StateDisabled     = "disabled"
	StateStopped      = "stopped"
	StateRunning      = "running"
	StateFailed       = "failed"

	crashWindow = 10 * time.Minute
	crashLimit  = 5
)

type Options struct {
	PackagesDir  string
	SignersFile  string
	ConsentFile  string
	DataDir      string
	LogDir       string
	ShimBinary   string
	TrustedKeys  []string
	ScanInterval time.Duration
	IdleTick     time.Duration
	StartTimeout time.Duration
}

type consent struct {
	Enabled bool     `json:"enabled"`
	Grants  []string `json:"grants"`
	Signer  string   `json:"signer"`
	Version string   `json:"version,omitempty"`
}

type consentFile struct {
	SchemaVersion int                 `json:"schemaVersion"`
	Plugins       map[string]*consent `json:"plugins"`
}

type plugin struct {
	id        string
	dir       string
	manifest  Manifest
	signature Signature
	digest    string
	problem   string
	trusted   bool
	proc      *process
	crashes   []time.Time
	lastError string
	lastUsed  time.Time
	inflight  int
	failed    bool
	restartAt time.Time
	starting  chan struct{}
}

type Host struct {
	options Options
	core    *daemon.Core
	ctx     context.Context
	gate    *phonefamily.Gate

	mu          sync.Mutex
	plugins     map[string]*plugin
	consents    consentFile
	fingerprint string
	stopping    bool
}

func New() *Host { return &Host{plugins: map[string]*plugin{}} }

func NewWithOptions(options Options) *Host {
	return &Host{options: options, plugins: map[string]*plugin{}}
}

func (h *Host) Name() string { return "plugins" }

func (h *Host) Start(ctx context.Context, core *daemon.Core) error {
	h.core = core
	h.ctx = ctx
	layout := core.Layout
	if h.options.PackagesDir == "" {
		h.options.PackagesDir = filepath.Join(layout.Data, "marketplace", "packages")
	}
	if h.options.SignersFile == "" {
		h.options.SignersFile = filepath.Join(layout.Config, "marketplace", "trusted-signers.json")
	}
	if h.options.ConsentFile == "" {
		h.options.ConsentFile = filepath.Join(layout.Data, "core", "plugins.json")
	}
	if h.options.DataDir == "" {
		h.options.DataDir = filepath.Join(layout.Data, "core", "plugins")
	}
	if h.options.LogDir == "" {
		h.options.LogDir = filepath.Join(layout.Logs, "core", "plugins")
	}
	if h.options.ShimBinary == "" && UsesShim() {
		if path, err := os.Executable(); err == nil {
			h.options.ShimBinary = path
		}
	}
	if h.options.ScanInterval <= 0 {
		h.options.ScanInterval = 15 * time.Second
	}
	if h.options.IdleTick <= 0 {
		h.options.IdleTick = 5 * time.Second
	}
	if h.options.StartTimeout <= 0 {
		h.options.StartTimeout = 10 * time.Second
	}
	h.loadConsents()
	h.register()
	h.Rescan()
	go h.loop(ctx)
	return nil
}

func (h *Host) Busy() bool {
	h.mu.Lock()
	defer h.mu.Unlock()
	for _, p := range h.plugins {
		if p.inflight > 0 {
			return true
		}
	}
	return false
}

func (h *Host) Stop() {
	h.mu.Lock()
	h.stopping = true
	procs := []*process{}
	for _, p := range h.plugins {
		if p.proc != nil {
			procs = append(procs, p.proc)
			p.proc = nil
		}
	}
	h.mu.Unlock()
	var wg sync.WaitGroup
	for _, proc := range procs {
		wg.Add(1)
		go func(proc *process) {
			defer wg.Done()
			proc.stop()
		}(proc)
	}
	wg.Wait()
}

func (h *Host) logf(format string, args ...any) {
	if h.core != nil {
		h.core.Logger.Printf("plugins: "+format, args...)
	}
}

func (h *Host) trustedKeys() map[string]string {
	keys := map[string]string{}
	for _, key := range h.options.TrustedKeys {
		keys[key] = "configured"
	}
	if h.core != nil {
		if id, err := h.core.RequireIdentity(); err == nil {
			keys[id.PublicKey()] = "this node"
		}
	}
	raw, err := os.ReadFile(h.options.SignersFile)
	if err != nil {
		return keys
	}
	var file struct {
		Signers []struct {
			Key  string `json:"key"`
			Name string `json:"name"`
		} `json:"signers"`
	}
	if json.Unmarshal(raw, &file) == nil {
		for _, signer := range file.Signers {
			if _, err := identity.Decode(signer.Key, 32); err == nil {
				keys[signer.Key] = signer.Name
			}
		}
	}
	return keys
}

func (h *Host) loadConsents() {
	h.consents = consentFile{SchemaVersion: 1, Plugins: map[string]*consent{}}
	raw, err := os.ReadFile(h.options.ConsentFile)
	if err != nil {
		return
	}
	var file consentFile
	if json.Unmarshal(raw, &file) == nil && file.Plugins != nil {
		h.consents = file
	}
}

func (h *Host) saveConsentsLocked() error {
	raw, _ := json.MarshalIndent(h.consents, "", "  ")
	if err := os.MkdirAll(filepath.Dir(h.options.ConsentFile), 0o700); err != nil {
		return err
	}
	temp := h.options.ConsentFile + ".tmp"
	if err := os.WriteFile(temp, append(raw, '\n'), 0o600); err != nil {
		return err
	}
	return os.Rename(temp, h.options.ConsentFile)
}

func (h *Host) packagesFingerprint() string {
	entries, err := os.ReadDir(h.options.PackagesDir)
	if err != nil {
		return ""
	}
	var builder strings.Builder
	for _, entry := range entries {
		if !entry.IsDir() || strings.HasPrefix(entry.Name(), ".") {
			continue
		}
		for _, name := range []string{"manifest.json", SignatureFile} {
			if info, err := os.Stat(filepath.Join(h.options.PackagesDir, entry.Name(), name)); err == nil {
				fmt.Fprintf(&builder, "%s/%s:%d:%d;", entry.Name(), name, info.Size(), info.ModTime().UnixNano())
			}
		}
	}
	if info, err := os.Stat(h.options.SignersFile); err == nil {
		fmt.Fprintf(&builder, "signers:%d;", info.ModTime().UnixNano())
	}
	return builder.String()
}

func (h *Host) Rescan() {
	fingerprint := h.packagesFingerprint()
	keys := h.trustedKeys()
	found := map[string]*plugin{}
	entries, _ := os.ReadDir(h.options.PackagesDir)
	for _, entry := range entries {
		if !entry.IsDir() || strings.HasPrefix(entry.Name(), ".") {
			continue
		}
		dir := filepath.Join(h.options.PackagesDir, entry.Name())
		manifest, err := LoadManifest(dir)
		if errors.Is(err, ErrNoService) || errors.Is(err, os.ErrNotExist) {
			continue
		}
		p := &plugin{id: entry.Name(), dir: dir, manifest: manifest}
		switch {
		case err != nil:
			p.problem = err.Error()
		case manifest.ID != entry.Name():
			p.problem = "package folder does not match its id"
		default:
			if _, err := ExecPath(dir, *manifest.Service); err != nil {
				p.problem = err.Error()
				break
			}
			signature, err := Verify(dir, manifest)
			p.signature = signature
			if err != nil {
				p.problem = err.Error()
				break
			}
			p.digest = signature.Digest()
			_, p.trusted = keys[signature.Signer]
		}
		found[p.id] = p
	}

	var stop []*process
	var boot []string
	h.mu.Lock()
	for id, old := range h.plugins {
		next, ok := found[id]
		if !ok || next.digest != old.digest || !h.allowedLocked(next) {
			if old.proc != nil {
				stop = append(stop, old.proc)
				old.proc = nil
			}
			if ok && next.digest != old.digest && old.digest != "" {
				h.logf("%s changed on disk (%s); restarting it", id, next.manifest.Version)
			}
			continue
		}
		next.proc, next.crashes, next.lastError, next.lastUsed, next.failed, next.restartAt = old.proc, old.crashes, old.lastError, old.lastUsed, old.failed, old.restartAt
	}
	h.plugins = found
	h.fingerprint = fingerprint
	for id, p := range h.plugins {
		if p.proc == nil && h.allowedLocked(p) && p.manifest.Service.Start == StartBoot && !p.failed {
			boot = append(boot, id)
		}
	}
	h.mu.Unlock()
	for _, proc := range stop {
		proc.stop()
	}
	sort.Strings(boot)
	for _, id := range boot {
		go func(id string) {
			if _, err := h.ensure(id); err != nil {
				h.logf("%s: %v", id, err)
			}
		}(id)
	}
	h.broadcastChanged()
}

func subset(want, have []string) bool {
	set := map[string]bool{}
	for _, id := range have {
		set[id] = true
	}
	for _, id := range want {
		if !set[id] {
			return false
		}
	}
	return true
}

func (h *Host) stateLocked(p *plugin) string {
	switch {
	case p.problem != "":
		return StateInvalid
	case !p.trusted:
		return StateUntrusted
	}
	granted := h.consents.Plugins[p.id]
	switch {
	case granted == nil || !granted.Enabled:
		return StateDisabled
	case granted.Signer != p.signature.Signer || !subset(p.manifest.PermissionIDs(), granted.Grants):
		return StateNeedsConsent
	case p.failed:
		return StateFailed
	case p.proc != nil:
		return StateRunning
	}
	return StateStopped
}

func (h *Host) allowedLocked(p *plugin) bool {
	state := h.stateLocked(p)
	return state == StateStopped || state == StateRunning || state == StateFailed
}

func (h *Host) rowLocked(p *plugin) map[string]any {
	row := map[string]any{
		"id": p.id, "name": p.manifest.Name, "version": p.manifest.Version, "summary": p.manifest.Summary,
		"kind": "service", "state": h.stateLocked(p), "permissions": p.manifest.Permissions,
		"signer": p.signature.Signer, "trusted": p.trusted, "problem": p.problem, "lastError": p.lastError,
		"restarts": len(p.crashes), "networkIsolation": false,
	}
	if p.manifest.Service != nil {
		row["start"] = p.manifest.Service.Start
		row["methods"] = p.manifest.Service.Methods
		row["events"] = p.manifest.Service.Events
	}
	if granted := h.consents.Plugins[p.id]; granted != nil {
		row["enabled"] = granted.Enabled
		row["grants"] = granted.Grants
	} else {
		row["enabled"] = false
		row["grants"] = []string{}
	}
	if p.proc != nil {
		row["pid"] = p.proc.pid()
		row["networkIsolation"] = p.proc.isolated
		row["inflight"] = p.inflight
	}
	return row
}

func (h *Host) List() []map[string]any {
	h.mu.Lock()
	defer h.mu.Unlock()
	ids := make([]string, 0, len(h.plugins))
	for id := range h.plugins {
		ids = append(ids, id)
	}
	sort.Strings(ids)
	out := make([]map[string]any, 0, len(ids))
	for _, id := range ids {
		out = append(out, h.rowLocked(h.plugins[id]))
	}
	return out
}

func (h *Host) broadcastChanged() {
	if h.core != nil {
		h.core.BroadcastUI("core.plugins.changed", map[string]any{"plugins": h.List()})
	}
}

func (h *Host) loop(ctx context.Context) {
	scan := time.NewTicker(h.options.ScanInterval)
	idle := time.NewTicker(h.options.IdleTick)
	defer scan.Stop()
	defer idle.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-scan.C:
			h.mu.Lock()
			changed := h.packagesFingerprint() != h.fingerprint
			h.mu.Unlock()
			if changed {
				h.Rescan()
			}
		case <-idle.C:
			h.reap()
		}
	}
}

func (h *Host) reap() {
	now := time.Now()
	var stop []*process
	var restart []string
	h.mu.Lock()
	for id, p := range h.plugins {
		service := p.manifest.Service
		if service == nil {
			continue
		}
		if p.proc != nil && service.Start == StartLazy && p.inflight == 0 &&
			now.Sub(p.lastUsed) > time.Duration(service.IdleExitSeconds)*time.Second {
			stop = append(stop, p.proc)
			p.proc = nil
			h.logf("%s idle for %ds; stopped", id, service.IdleExitSeconds)
		}
		if p.proc == nil && service.Start == StartBoot && !p.failed && !p.restartAt.IsZero() &&
			now.After(p.restartAt) && h.allowedLocked(p) {
			p.restartAt = time.Time{}
			restart = append(restart, id)
		}
	}
	h.mu.Unlock()
	for _, proc := range stop {
		proc.stop()
	}
	for _, id := range restart {
		if _, err := h.ensure(id); err != nil {
			h.logf("%s: restart: %v", id, err)
		}
	}
	if len(stop) > 0 || len(restart) > 0 {
		h.broadcastChanged()
	}
}

func (h *Host) onExit(p *plugin, proc *process, err error) {
	h.mu.Lock()
	if p.proc != proc || h.stopping {
		h.mu.Unlock()
		return
	}
	p.proc = nil
	now := time.Now()
	kept := p.crashes[:0]
	for _, at := range p.crashes {
		if now.Sub(at) < crashWindow {
			kept = append(kept, at)
		}
	}
	p.crashes = append(kept, now)
	p.lastError = fmt.Sprintf("exited: %v", err)
	if len(p.crashes) >= crashLimit {
		p.failed = true
		h.logf("%s crashed %d times in %s; not restarting it until it is restarted by hand", p.id, len(p.crashes), crashWindow)
	} else if p.manifest.Service.Start == StartBoot {
		backoff := time.Second << (len(p.crashes) - 1)
		if backoff > time.Minute {
			backoff = time.Minute
		}
		p.restartAt = now.Add(backoff)
		h.logf("%s exited (%v); restarting in %s", p.id, err, backoff)
	}
	h.mu.Unlock()
	h.broadcastChanged()
}

func (h *Host) ensure(id string) (*process, error) {
	for {
		h.mu.Lock()
		p := h.plugins[id]
		if p == nil {
			h.mu.Unlock()
			return nil, rpc.Fail("unknown_plugin")
		}
		if !h.allowedLocked(p) {
			state := h.stateLocked(p)
			h.mu.Unlock()
			return nil, rpc.Fail("plugin_" + state)
		}
		if p.failed {
			h.mu.Unlock()
			return nil, rpc.Fail("plugin_failed")
		}
		if p.proc != nil {
			proc := p.proc
			h.mu.Unlock()
			return proc, nil
		}
		if p.starting != nil {
			wait := p.starting
			h.mu.Unlock()
			<-wait
			continue
		}
		p.starting = make(chan struct{})
		grants := append([]string(nil), h.consents.Plugins[id].Grants...)
		h.mu.Unlock()

		proc, err := h.launch(p, grants)

		h.mu.Lock()
		close(p.starting)
		p.starting = nil
		if err != nil {
			p.lastError = err.Error()
			h.mu.Unlock()
			h.broadcastChanged()
			return nil, err
		}
		if h.plugins[id] != p || h.stopping {
			h.mu.Unlock()
			proc.stop()
			return nil, rpc.Fail("plugin_changed")
		}
		p.proc = proc
		p.lastUsed = time.Now()
		p.lastError = ""
		h.mu.Unlock()
		go func() {
			err := proc.wait()
			h.onExit(p, proc, err)
		}()
		h.logf("%s %s started (pid %d, network isolated: %v)", id, p.manifest.Version, proc.pid(), proc.isolated)
		h.broadcastChanged()
		return proc, nil
	}
}

func (h *Host) emit(p *plugin, event string, data json.RawMessage) {
	if !p.manifest.Service.HasEvent(event) || h.core == nil {
		return
	}
	name := "plugin." + p.id + "." + event
	h.core.BroadcastUI(name, data)
	if p.manifest.Service.RemoteEvent(event) {
		h.core.Hub.Broadcast(name, data, func(peer rpc.Peer) bool {
			return rpc.Remote(peer) && h.gate.IsOwner(peer.Device())
		})
	}
}

func (h *Host) RemoteList() []map[string]any {
	h.mu.Lock()
	defer h.mu.Unlock()
	out := []map[string]any{}
	for _, p := range h.plugins {
		service := p.manifest.Service
		if service == nil || len(service.RemoteMethods) == 0 || !h.allowedLocked(p) {
			continue
		}
		out = append(out, map[string]any{"id": p.id, "name": p.manifest.Name, "version": p.manifest.Version,
			"summary": p.manifest.Summary, "methods": service.RemoteMethods, "events": service.RemoteEvents,
			"running": p.proc != nil})
	}
	sort.Slice(out, func(i, j int) bool { return out[i]["id"].(string) < out[j]["id"].(string) })
	return out
}

func (h *Host) Call(ctx context.Context, req *rpc.Request, id, method string) (any, error) {
	h.mu.Lock()
	p := h.plugins[id]
	if p == nil || p.manifest.Service == nil {
		h.mu.Unlock()
		return nil, rpc.Fail("unknown_plugin")
	}
	if !p.manifest.Service.HasMethod(method) {
		h.mu.Unlock()
		return nil, rpc.Fail("unknown_method")
	}
	p.inflight++
	h.mu.Unlock()
	defer func() {
		h.mu.Lock()
		p.inflight--
		p.lastUsed = time.Now()
		h.mu.Unlock()
	}()
	proc, err := h.ensure(id)
	if err != nil {
		return nil, err
	}
	result, err := proc.call(ctx, method, req.Params, func(data json.RawMessage) { req.Progress(data) })
	if err != nil {
		return nil, err
	}
	return result, nil
}

func (h *Host) setConsent(id string, enabled bool, grants []string) error {
	h.mu.Lock()
	p := h.plugins[id]
	if p == nil {
		h.mu.Unlock()
		return rpc.Fail("unknown_plugin")
	}
	var stop *process
	if enabled {
		if p.problem != "" {
			h.mu.Unlock()
			return rpc.Fail("plugin_invalid")
		}
		if !p.trusted {
			h.mu.Unlock()
			return rpc.Fail("plugin_untrusted")
		}
		requested := p.manifest.PermissionIDs()
		if grants == nil {
			grants = requested
		}
		if !subset(requested, grants) {
			h.mu.Unlock()
			return rpc.Fail("grants_incomplete")
		}
		kept := []string{}
		for _, grant := range grants {
			if p.manifest.Wants(grant) {
				kept = append(kept, grant)
			}
		}
		sort.Strings(kept)
		h.consents.Plugins[id] = &consent{Enabled: true, Grants: kept, Signer: p.signature.Signer, Version: p.manifest.Version}
		p.failed = false
		p.crashes = nil
	} else {
		if granted := h.consents.Plugins[id]; granted != nil {
			granted.Enabled = false
		}
		stop = p.proc
		p.proc = nil
	}
	err := h.saveConsentsLocked()
	boot := enabled && p.manifest.Service.Start == StartBoot
	h.mu.Unlock()
	if stop != nil {
		stop.stop()
	}
	if err != nil {
		return rpc.Fail("consent_write_failed")
	}
	if boot {
		go func() {
			if _, err := h.ensure(id); err != nil {
				h.logf("%s: %v", id, err)
			}
		}()
	}
	h.broadcastChanged()
	return nil
}

func (h *Host) restart(id string) error {
	h.mu.Lock()
	p := h.plugins[id]
	if p == nil {
		h.mu.Unlock()
		return rpc.Fail("unknown_plugin")
	}
	stop := p.proc
	p.proc = nil
	p.failed = false
	p.crashes = nil
	h.mu.Unlock()
	if stop != nil {
		stop.stop()
	}
	_, err := h.ensure(id)
	return err
}

func (h *Host) register() {
	local := func(handler rpc.Handler) rpc.Handler {
		return func(ctx context.Context, req *rpc.Request) (any, error) {
			if req.Peer == nil || rpc.Remote(req.Peer) {
				return nil, rpc.Fail("forbidden")
			}
			return handler(ctx, req)
		}
	}
	type target struct {
		ID     string   `json:"id"`
		Grants []string `json:"grants"`
	}
	registry := h.core.Registry
	h.gate = &phonefamily.Gate{Core: h.core}
	registry.Handle("plugins.list", "plugins", h.gate.Wrap(func(ctx context.Context, req *rpc.Request) (any, error) {
		return map[string]any{"plugins": h.RemoteList(), "api": APIVersion}, nil
	}))
	registry.Handle("core.plugins.list", "plugins", local(func(ctx context.Context, req *rpc.Request) (any, error) {
		return map[string]any{"plugins": h.List(), "packagesDir": h.options.PackagesDir,
			"networkIsolation": networkIsolationSupported(), "api": APIVersion}, nil
	}))
	registry.Handle("core.plugins.rescan", "plugins", local(func(ctx context.Context, req *rpc.Request) (any, error) {
		h.Rescan()
		return map[string]any{"plugins": h.List()}, nil
	}))
	registry.Handle("core.plugins.enable", "plugins", local(func(ctx context.Context, req *rpc.Request) (any, error) {
		var params target
		if err := req.Bind(&params); err != nil {
			return nil, err
		}
		if err := h.setConsent(params.ID, true, params.Grants); err != nil {
			return nil, err
		}
		return map[string]any{"plugins": h.List()}, nil
	}))
	registry.Handle("core.plugins.disable", "plugins", local(func(ctx context.Context, req *rpc.Request) (any, error) {
		var params target
		if err := req.Bind(&params); err != nil {
			return nil, err
		}
		if err := h.setConsent(params.ID, false, nil); err != nil {
			return nil, err
		}
		return map[string]any{"plugins": h.List()}, nil
	}))
	registry.Handle("core.plugins.restart", "plugins", local(func(ctx context.Context, req *rpc.Request) (any, error) {
		var params target
		if err := req.Bind(&params); err != nil {
			return nil, err
		}
		if err := h.restart(params.ID); err != nil {
			return nil, err
		}
		return map[string]any{"plugins": h.List()}, nil
	}))
	registry.HandleFamily("plugin.", "plugins", func(ctx context.Context, req *rpc.Request) (any, error) {
		rest := strings.TrimPrefix(req.Method, "plugin.")
		id, method, ok := strings.Cut(rest, ".")
		if !ok || id == "" || method == "" {
			return nil, rpc.Fail("unknown_method")
		}
		if req.Peer == nil {
			return nil, rpc.Fail("forbidden")
		}
		if rpc.Remote(req.Peer) {
			h.mu.Lock()
			p := h.plugins[id]
			remote := p != nil && p.manifest.Service != nil && p.manifest.Service.Remote(method)
			h.mu.Unlock()
			if !remote {
				return nil, rpc.Fail("forbidden")
			}
			if !h.gate.IsOwner(req.Peer.Device()) {
				return nil, rpc.Fail("not_owner")
			}
		}
		return h.Call(ctx, req, id, method)
	})
}
