package backup

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"time"

	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/ownergate"
	"forkmesh.com/desktop-core/internal/rpc"
	"forkmesh.com/desktop-core/internal/safedelete"
)

const owner = "backups"

type Config struct {
	Enabled      bool     `json:"enabled"`
	Keep         int      `json:"keep"`
	Root         string   `json:"root"`
	DataRoot     string   `json:"dataRoot"`
	SettingsFile string   `json:"settingsFile"`
	Excludes     []string `json:"excludes"`
	Arm          bool     `json:"arm"`
}

type Module struct {
	Owners     func() []string
	Now        func() time.Time
	LookPath   func(string) (string, error)
	FirstDelay time.Duration
	Interval   time.Duration

	core *daemon.Core
	gate *ownergate.Gate
	ctx  context.Context

	mu          sync.Mutex
	config      *Config
	running     bool
	archiving   bool
	failure     string
	failureAtMs int64
	tarCmd      *exec.Cmd
	cancelled   bool
	timerStop   chan struct{}
	active      int
}

func New() *Module { return &Module{} }

func (m *Module) Name() string { return owner }

func (m *Module) now() time.Time {
	if m.Now != nil {
		return m.Now()
	}
	return time.Now()
}

func (m *Module) lookPath(name string) (string, error) {
	if m.LookPath != nil {
		return m.LookPath(name)
	}
	return exec.LookPath(name)
}

func (m *Module) Start(ctx context.Context, core *daemon.Core) error {
	m.core = core
	m.ctx = ctx
	m.gate = &ownergate.Gate{Core: core, Owners: m.Owners}
	core.Registry.Handle("diag.backups", "diag", m.gate.Wrap(m.handleDiag))
	desktop := func(method string, handler rpc.Handler) {
		core.Registry.Handle(method, owner, ownergate.DesktopOnly(handler))
	}
	desktop("core.backups.config", m.handleConfig)
	desktop("core.backups.status", m.handleStatus)
	desktop("core.backups.now", m.handleNow)
	desktop("core.backups.prune", m.handlePrune)
	desktop("core.backups.unpack", m.handleUnpack)
	desktop("core.backups.export", m.handleExport)
	desktop("core.backups.cancel", m.handleCancel)
	return nil
}

func (m *Module) Stop() {
	m.mu.Lock()
	if m.timerStop != nil {
		close(m.timerStop)
		m.timerStop = nil
	}
	m.mu.Unlock()
}

func (m *Module) Busy() bool {
	m.mu.Lock()
	defer m.mu.Unlock()
	return m.active > 0
}

func (m *Module) track() func() {
	m.mu.Lock()
	m.active++
	m.mu.Unlock()
	return func() {
		m.mu.Lock()
		m.active--
		m.mu.Unlock()
	}
}

func (m *Module) defaults() Config {
	config := Config{Keep: KeepDefault}
	if m.core != nil {
		config.DataRoot = m.core.Layout.Data
		config.Root = filepath.Join(m.core.Layout.Data, "backups")
		config.SettingsFile = m.core.Layout.SettingsFile()
		if m.core.Settings != nil {
			settings := m.core.Settings.Get()
			config.Enabled = settings.Bool("backup/hourlyEnabled", false)
			config.Keep = int(settings.Int("backup/keepCount", KeepDefault))
		}
	}
	config.Keep = ClampKeep(config.Keep)
	return config
}

func (m *Module) currentLocked() Config {
	if m.config != nil {
		return *m.config
	}
	return m.defaults()
}

func (m *Module) logUI(format string, args ...any) {
	if m.core != nil {
		m.core.LogUI(format, args...)
	}
}

func (m *Module) statusEvent(text string, isError bool) {
	if m.core != nil {
		m.core.BroadcastUI("core.backups.status", map[string]any{"text": text, "error": isError})
	}
}

func (m *Module) changedEvent() {
	m.mu.Lock()
	data := map[string]any{"running": m.running, "failure": m.failure, "failureAtMs": m.failureAtMs}
	m.mu.Unlock()
	if m.core != nil {
		m.core.BroadcastUI("core.backups.changed", data)
	}
}

func (m *Module) noteResult(failure string) {
	m.mu.Lock()
	m.failure = failure
	m.failureAtMs = m.now().UnixMilli()
	m.mu.Unlock()
}

func (m *Module) handleConfig(ctx context.Context, req *rpc.Request) (any, error) {
	var config Config
	if err := req.Bind(&config); err != nil {
		return nil, err
	}
	defaults := m.defaults()
	if config.Root == "" {
		config.Root = defaults.Root
	}
	if config.DataRoot == "" {
		config.DataRoot = defaults.DataRoot
	}
	if config.SettingsFile == "" {
		config.SettingsFile = defaults.SettingsFile
	}
	config.Keep = ClampKeep(config.Keep)
	m.mu.Lock()
	m.config = &config
	m.mu.Unlock()
	if config.Arm {
		m.arm(config.Enabled)
	}
	return m.statusObject(), nil
}

func (m *Module) arm(enabled bool) {
	m.mu.Lock()
	if m.timerStop != nil {
		close(m.timerStop)
		m.timerStop = nil
	}
	if !enabled {
		m.mu.Unlock()
		return
	}
	stop := make(chan struct{})
	m.timerStop = stop
	config := m.currentLocked()
	m.mu.Unlock()
	interval := m.Interval
	if interval <= 0 {
		interval = time.Duration(IntervalMs) * time.Millisecond
	}
	first := m.FirstDelay
	if first <= 0 {
		first = 15 * time.Second
	}
	snapshots := List(config.Root)
	var last time.Time
	if len(snapshots) > 0 {
		last = snapshots[0].Taken
	}
	due := IsDue(last, m.now(), IntervalMs)
	var done <-chan struct{}
	if m.ctx != nil {
		done = m.ctx.Done()
	}
	go func() {
		ticker := time.NewTicker(interval)
		defer ticker.Stop()
		var firstShot <-chan time.Time
		if due {
			timer := time.NewTimer(first)
			defer timer.Stop()
			firstShot = timer.C
		}
		for {
			select {
			case <-stop:
				return
			case <-done:
				return
			case <-firstShot:
				firstShot = nil
				m.take(true)
			case <-ticker.C:
				m.take(true)
			}
		}
	}()
}

func (m *Module) statusObject() map[string]any {
	m.mu.Lock()
	config := m.currentLocked()
	running := m.running
	failure := m.failure
	failureAt := m.failureAtMs
	m.mu.Unlock()
	snapshots := List(config.Root)
	if len(snapshots) > MaxSnapshots {
		snapshots = snapshots[:MaxSnapshots]
	}
	return Status(StatusInput{Enabled: config.Enabled, Keep: config.Keep, Running: running,
		Location: LocationLabel(config.Root, config.DataRoot), Snapshots: snapshots, Failure: failure,
		FailureAtMs: failureAt, IntervalMs: IntervalMs, NowMs: m.now().UnixMilli()})
}

func (m *Module) handleDiag(ctx context.Context, req *rpc.Request) (any, error) {
	m.mu.Lock()
	configured := m.config != nil
	m.mu.Unlock()
	if !configured {
		return nil, rpc.ErrDelegate
	}
	return m.statusObject(), nil
}

func (m *Module) handleStatus(ctx context.Context, req *rpc.Request) (any, error) {
	return m.statusObject(), nil
}

func (m *Module) adopt(req *rpc.Request) {
	var params struct {
		Config *Config `json:"config"`
	}
	if req.Bind(&params) != nil || params.Config == nil {
		return
	}
	config := *params.Config
	defaults := m.defaults()
	if config.Root == "" {
		config.Root = defaults.Root
	}
	if config.DataRoot == "" {
		config.DataRoot = defaults.DataRoot
	}
	if config.SettingsFile == "" {
		config.SettingsFile = defaults.SettingsFile
	}
	config.Keep = ClampKeep(config.Keep)
	config.Arm = false
	m.mu.Lock()
	m.config = &config
	m.mu.Unlock()
}

func (m *Module) handleCancel(ctx context.Context, req *rpc.Request) (any, error) {
	m.mu.Lock()
	cmd := m.tarCmd
	if cmd != nil {
		m.cancelled = true
	}
	m.mu.Unlock()
	if cmd != nil && cmd.Process != nil {
		_ = cmd.Process.Kill()
	}
	return map[string]any{"cancelled": cmd != nil}, nil
}

func (m *Module) handleNow(ctx context.Context, req *rpc.Request) (any, error) {
	m.adopt(req)
	automatic, _ := req.Object()["automatic"].(bool)
	started := m.take(automatic)
	return map[string]any{"started": started}, nil
}

func (m *Module) take(automatic bool) bool {
	m.mu.Lock()
	config := m.currentLocked()
	if automatic && !config.Enabled {
		m.mu.Unlock()
		return false
	}
	if m.running {
		m.mu.Unlock()
		if !automatic {
			m.statusEvent("A backup is already running…", false)
		}
		return false
	}
	m.mu.Unlock()
	tar, err := m.lookPath("tar")
	if err != nil {
		m.statusEvent("Can't back up: the 'tar' tool was not found on this system.", true)
		m.noteResult("tar_missing")
		m.logUI("Backup: skipped — 'tar' was not found on this system.")
		m.changedEvent()
		return false
	}
	if err := os.MkdirAll(config.Root, 0o700); err != nil {
		m.statusEvent("Backup failed: could not create "+config.Root, true)
		m.noteResult("failed")
		m.changedEvent()
		return false
	}
	finalPath := filepath.Join(config.Root, FileName(m.now()))
	partPath := finalPath + ".part"
	args := ConfigArchiveTarArgs(partPath, config.DataRoot, config.SettingsFile, append(append([]string{}, config.Excludes...), config.Root))
	if len(args) == 0 {
		m.statusEvent("There's nothing to back up yet.", true)
		m.noteResult("nothing_to_back_up")
		m.changedEvent()
		return false
	}
	m.mu.Lock()
	if m.running {
		m.mu.Unlock()
		return false
	}
	m.running = true
	m.mu.Unlock()
	release := m.track()
	m.changedEvent()
	if !automatic {
		m.statusEvent("Backing up…", false)
	}
	go func() {
		defer release()
		cmd := exec.Command(tar, args...)
		var stderr bytes.Buffer
		cmd.Stderr = &stderr
		runErr := cmd.Start()
		if runErr == nil {
			m.mu.Lock()
			m.tarCmd = cmd
			m.cancelled = false
			m.mu.Unlock()
			runErr = cmd.Wait()
		}
		m.mu.Lock()
		m.tarCmd = nil
		cancelled := m.cancelled
		m.cancelled = false
		m.mu.Unlock()
		if cancelled {
			_ = os.Remove(partPath)
			m.mu.Lock()
			m.running = false
			m.mu.Unlock()
			m.changedEvent()
			return
		}
		code := 0
		normal := true
		if runErr != nil {
			var exitErr *exec.ExitError
			if errors.As(runErr, &exitErr) && exitErr.ExitCode() >= 0 {
				code = exitErr.ExitCode()
			} else {
				normal = false
				code = -1
			}
		}
		ok := normal && (code == 0 || code == 1)
		renamed := ok && os.Rename(partPath, finalPath) == nil
		m.mu.Lock()
		m.running = false
		m.mu.Unlock()
		if renamed {
			m.prune(config.Keep)
			m.noteResult("")
			name := filepath.Base(finalPath)
			var size int64
			if info, err := os.Stat(finalPath); err == nil {
				size = info.Size()
			}
			prefix := "Backup "
			if automatic {
				prefix = "Hourly backup "
			}
			sizeText := FormattedDataSize(size)
			m.statusEvent(prefix+"saved: "+name+" ("+sizeText+").", false)
			m.logUI("Backup: wrote %s (%s).", name, sizeText)
		} else {
			_ = os.Remove(partPath)
			errText := utf16Left(strings.TrimSpace(stderr.String()), 300)
			why := errText
			if why == "" {
				why = fmt.Sprintf("tar exited with %d", code)
			}
			m.statusEvent("Backup failed: "+why, true)
			if ok {
				m.noteResult("rename_failed")
			} else {
				m.noteResult("tar_failed")
			}
			m.logUI("Backup: failed (%s).", why)
		}
		m.changedEvent()
	}()
	return true
}

func (m *Module) prune(keep int) int {
	m.mu.Lock()
	config := m.currentLocked()
	running := m.running
	m.mu.Unlock()
	removed := 0
	for _, snapshot := range ToPrune(List(config.Root), keep) {
		if os.Remove(snapshot.Path) == nil {
			removed++
		}
	}
	if running {
		return removed
	}
	entries, err := os.ReadDir(config.Root)
	if err != nil {
		return removed
	}
	for _, entry := range entries {
		name := entry.Name()
		if entry.Type().IsRegular() && strings.HasPrefix(name, filePrefix) && strings.HasSuffix(name, fileSuffix+".part") {
			_ = os.Remove(filepath.Join(config.Root, name))
		}
	}
	return removed
}

func (m *Module) handlePrune(ctx context.Context, req *rpc.Request) (any, error) {
	keep := KeepDefault
	if value, ok := req.Object()["keep"].(float64); ok {
		keep = int(value)
	} else {
		m.mu.Lock()
		keep = m.currentLocked().Keep
		m.mu.Unlock()
	}
	return map[string]any{"removed": m.prune(ClampKeep(keep))}, nil
}

func (m *Module) beginArchive() bool {
	m.mu.Lock()
	defer m.mu.Unlock()
	if m.archiving {
		return false
	}
	m.archiving = true
	return true
}

func (m *Module) endArchive() {
	m.mu.Lock()
	m.archiving = false
	m.mu.Unlock()
}

func runTar(tar string, args []string) (bool, string) {
	cmd := exec.Command(tar, args...)
	var stderr bytes.Buffer
	cmd.Stderr = &stderr
	err := cmd.Run()
	return err == nil, strings.TrimSpace(stderr.String())
}

func (m *Module) handleUnpack(ctx context.Context, req *rpc.Request) (any, error) {
	object := req.Object()
	archive, _ := object["archive"].(string)
	workRoot, _ := object["workRoot"].(string)
	stamp, _ := object["stamp"].(string)
	if archive == "" || workRoot == "" || !filepath.IsAbs(workRoot) || !strings.HasPrefix(filepath.Base(workRoot), "tmp-forkmesh-import-") {
		return nil, rpc.Fail("bad_params")
	}
	tar, err := m.lookPath("tar")
	if err != nil {
		return nil, rpc.Fail("tar_missing")
	}
	if !m.beginArchive() {
		return nil, rpc.Fail("busy")
	}
	if err := os.MkdirAll(workRoot, 0o700); err != nil {
		m.endArchive()
		return nil, rpc.Fail("mkdir_failed")
	}
	release := m.track()
	go func() {
		defer release()
		defer m.endArchive()
		ok, why := runTar(tar, []string{"-xzf", archive, "-C", workRoot})
		desktop := m.core != nil && m.core.UI() != nil
		if !ok || !desktop {
			if err := safedelete.RemoveTreeUnder(filepath.Dir(workRoot), workRoot); err != nil && m.core != nil {
				m.core.Logger.Printf("backups: %v", err)
			}
		}
		if !ok && why == "" {
			why = "tar failed"
		}
		if m.core != nil {
			m.core.BroadcastUI("core.backups.unpacked", map[string]any{"ok": ok, "error": why, "workRoot": workRoot,
				"stamp": stamp, "archive": archive})
		}
	}()
	return map[string]any{"started": true}, nil
}

func (m *Module) handleExport(ctx context.Context, req *rpc.Request) (any, error) {
	m.adopt(req)
	out, _ := req.Object()["out"].(string)
	if out == "" || !filepath.IsAbs(out) {
		return nil, rpc.Fail("bad_params")
	}
	tar, err := m.lookPath("tar")
	if err != nil {
		return nil, rpc.Fail("tar_missing")
	}
	m.mu.Lock()
	config := m.currentLocked()
	m.mu.Unlock()
	args := ConfigArchiveTarArgs(out, config.DataRoot, config.SettingsFile, append(append([]string{}, config.Excludes...), config.Root))
	if len(args) == 0 {
		return nil, rpc.Fail("nothing_to_export")
	}
	if !m.beginArchive() {
		return nil, rpc.Fail("busy")
	}
	release := m.track()
	go func() {
		defer release()
		defer m.endArchive()
		ok, why := runTar(tar, args)
		var size int64
		if info, err := os.Stat(out); err == nil {
			size = info.Size()
		}
		if m.core != nil {
			m.core.BroadcastUI("core.backups.exported", map[string]any{"ok": ok, "error": why, "out": out, "bytes": size})
		}
	}()
	return map[string]any{"started": true}, nil
}
