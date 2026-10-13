package update

import (
	"context"
	"encoding/json"
	"math/rand/v2"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"time"
)

const (
	autoUpdateSetting        = "update/autoUpdate"
	autoUpdateRestartSetting = "update/autoUpdateRestart"
	autoQuietTimeout         = 3 * time.Second
)

func autoFirstDelay() time.Duration {
	return 5*time.Minute + time.Duration(rand.Int64N(int64(10*time.Minute)))
}

func autoInterval() time.Duration {
	return time.Hour + time.Duration(rand.Int64N(int64(20*time.Minute)+1)) - 10*time.Minute
}

func (m *Module) autoUpdateLoop(ctx context.Context) {
	env := m.env()
	if env.AutoFirst == nil || env.AutoEvery == nil {
		return
	}
	first := time.NewTimer(env.AutoFirst())
	defer first.Stop()
	every := time.NewTicker(env.AutoEvery())
	defer every.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-first.C:
		case <-every.C:
		}
		m.autoUpdateCheck(ctx)
	}
}

type desktopBusy struct {
	Running     int  `json:"running"`
	AnyRunning  bool `json:"anyRunning"`
	RebuildBusy bool `json:"rebuildBusy"`
}

func (m *Module) desktopBusy(ctx context.Context) (bool, bool) {
	if !m.hasUI() {
		return false, false
	}
	ctx, cancel := context.WithTimeout(ctx, 5*time.Second)
	defer cancel()
	raw, err := m.callUI(ctx, "ui.update.agents", map[string]any{})
	if err != nil {
		return false, false
	}
	var reply desktopBusy
	if json.Unmarshal(raw, &reply) != nil {
		return false, false
	}
	return reply.AnyRunning || reply.RebuildBusy, true
}

func autoGit(ctx context.Context, dir string, timeout time.Duration, args ...string) (string, bool) {
	ctx, cancel := context.WithTimeout(ctx, timeout)
	defer cancel()
	cmd := exec.CommandContext(ctx, "git", args...)
	cmd.Dir = dir
	cmd.WaitDelay = time.Second
	out, err := cmd.Output()
	if err != nil {
		return "", false
	}
	return strings.TrimSpace(string(out)), true
}

func (m *Module) autoUpdateCheck(ctx context.Context) string {
	m.mu.Lock()
	config := m.config
	checking := m.autoChecking
	m.mu.Unlock()
	if config == nil || strings.TrimSpace(config.AutoUpdateDir) == "" {
		return "not_delegated"
	}
	if !m.hasUI() {
		return "no_desktop"
	}
	if m.core == nil || m.core.Settings == nil {
		return "off"
	}
	settings := m.core.Settings.Get()
	if !settings.Bool(autoUpdateSetting, false) || !settings.Bool(autoUpdateRestartSetting, true) {
		return "off"
	}
	if checking {
		return "checking"
	}
	if m.running() {
		return "running"
	}
	busy, answered := m.desktopBusy(ctx)
	if !answered {
		return "no_desktop"
	}
	if busy {
		return "busy"
	}
	clientDir := config.AutoUpdateDir
	if _, err := os.Stat(filepath.Join(clientDir, "CMakeLists.txt")); err != nil {
		return "no_checkout"
	}
	m.mu.Lock()
	if m.autoChecking {
		m.mu.Unlock()
		return "checking"
	}
	m.autoChecking = true
	m.mu.Unlock()
	defer func() {
		m.mu.Lock()
		m.autoChecking = false
		m.mu.Unlock()
	}()
	if upstream, ok := autoGit(ctx, clientDir, autoQuietTimeout, "rev-parse", "--abbrev-ref", "--symbolic-full-name", "@{u}"); !ok || upstream == "" {
		return "no_upstream"
	}
	if _, ok := autoGit(ctx, clientDir, m.env().AutoFetch, "fetch", "--quiet", "--tags"); !ok {
		return "fetch_failed"
	}
	tag, ok := autoGit(ctx, clientDir, autoQuietTimeout, "for-each-ref", "--sort=-creatordate", "--count=1", "refs/tags", "--format=%(refname:short)")
	if !ok || tag == "" {
		return "no_tag"
	}
	commit, ok := autoGit(ctx, clientDir, autoQuietTimeout, "rev-list", "-n1", tag)
	if !ok || commit == "" {
		return "no_tag"
	}
	if _, contained := autoGit(ctx, clientDir, autoQuietTimeout, "merge-base", "--is-ancestor", commit, "HEAD"); contained {
		return "current"
	}
	if m.running() {
		return "running"
	}
	if busy, answered := m.desktopBusy(ctx); !answered {
		return "no_desktop"
	} else if busy {
		return "busy"
	}
	m.logf("auto-update: release %s (%s) is not in %s; asking the desktop to update", tag, commit, clientDir)
	call, cancel := context.WithTimeout(ctx, 10*time.Second)
	defer cancel()
	raw, err := m.callUI(call, "ui.update.auto", map[string]any{"clientDir": clientDir, "tag": tag, "commit": commit})
	if err != nil {
		m.logf("auto-update: the desktop did not take release %s: %v", tag, err)
		return "no_desktop"
	}
	var reply struct {
		Started bool `json:"started"`
	}
	_ = json.Unmarshal(raw, &reply)
	if !reply.Started {
		return "busy"
	}
	return "started"
}
