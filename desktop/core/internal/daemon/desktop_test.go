package daemon

import (
	"io"
	"log"
	"os"
	"os/exec"
	"path/filepath"
	"reflect"
	"runtime"
	"slices"
	"strconv"
	"sync"
	"testing"
	"time"

	"forkmesh.com/desktop-core/internal/paths"
)

func TestHeadlessCommandRunsDesktopOffscreen(t *testing.T) {
	dir := t.TempDir()
	engine := filepath.Join(dir, "forkmesh-core")
	environ := []string{"HOME=/home/u", "INVOCATION_ID=abc", "QT_QPA_PLATFORM=xcb"}
	if _, _, _, ok := headlessCommand("linux", engine, "work", "", environ); ok {
		t.Fatal("started without a desktop binary")
	}
	desktop := filepath.Join(dir, "forkmesh")
	if err := os.WriteFile(desktop, []byte("x"), 0o755); err != nil {
		t.Fatal(err)
	}
	for _, goos := range []string{"darwin", "windows"} {
		if _, _, _, ok := headlessCommand(goos, engine, "work", "", environ); ok {
			t.Fatalf("started on %s", goos)
		}
	}
	if _, _, _, ok := headlessCommand("linux", engine, "../bad", "", environ); ok {
		t.Fatal("started for a bad project")
	}
	program, args, env, ok := headlessCommand("linux", engine, "work", "", environ)
	if !ok || program != desktop || !reflect.DeepEqual(args, []string{"--headless", "--project", "work"}) {
		t.Fatalf("plain: %v %v %v", program, args, ok)
	}
	cmd := exec.Command(program)
	cmd.Env = env
	if !slices.Contains(cmd.Environ(), "QT_QPA_PLATFORM=offscreen") || slices.Contains(cmd.Environ(), "QT_QPA_PLATFORM=xcb") ||
		!slices.Contains(env, "FORKMESH_NO_SPLASH=1") || slices.Contains(env, "INVOCATION_ID=abc") || !slices.Contains(env, "HOME=/home/u") {
		t.Fatalf("environment %v", cmd.Environ())
	}
	program, args, _, _ = headlessCommand("linux", engine, "work", "/usr/bin/systemd-run", environ)
	want := []string{"--user", "--scope", "--quiet", "--collect", "--unit=forkmesh-desktop-work", "--", desktop, "--headless", "--project", "work"}
	if program != "/usr/bin/systemd-run" || !reflect.DeepEqual(args, want) {
		t.Fatalf("scoped: %v %v", program, args)
	}
}

func TestLockHolderReadsQLockFilePid(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "forkmesh.lock")
	if lockHolder(path) != 0 {
		t.Fatal("missing lock has a holder")
	}
	_ = os.WriteFile(path, []byte("4242\nForkMesh\nhost\n"), 0o600)
	if got := lockHolder(path); got != 4242 {
		t.Fatalf("holder %d", got)
	}
	_ = os.WriteFile(path, []byte("garbage"), 0o600)
	if lockHolder(path) != 0 {
		t.Fatal("garbage has a holder")
	}
}

type headlessFixture struct {
	core    *Core
	dir     string
	mu      sync.Mutex
	starts  int
	failing bool
	locked  int
}

func newHeadlessFixture(t *testing.T) *headlessFixture {
	if runtime.GOOS == "windows" {
		t.Skip("the desktop without a window is Linux only")
	}
	sleep, err := exec.LookPath("sleep")
	if err != nil {
		t.Skip("no sleep binary")
	}
	dir := t.TempDir()
	layout := paths.Layout{Project: "work", State: filepath.Join(dir, "state"), Config: filepath.Join(dir, "config")}
	core := NewCore(layout, "test", log.New(io.Discard, "", 0))
	f := &headlessFixture{core: core, dir: dir}
	core.life.executable = filepath.Join(dir, "bin", "forkmesh-core")
	_ = os.MkdirAll(filepath.Join(dir, "bin"), 0o755)
	_ = os.WriteFile(filepath.Join(dir, "bin", "forkmesh"), []byte("x"), 0o755)
	if err := core.saveConfig(DaemonConfig{Background: true}); err != nil {
		t.Fatal(err)
	}
	if err := core.Remember(signedInFile, map[string]string{"name": "box"}); err != nil {
		t.Fatal(err)
	}
	core.desktop.allowed = true
	core.desktop.alive = func(pid int) bool {
		f.mu.Lock()
		defer f.mu.Unlock()
		return pid == f.locked
	}
	core.desktop.start = func(cmd *exec.Cmd) error {
		f.mu.Lock()
		f.starts++
		failing := f.failing
		f.mu.Unlock()
		if failing {
			cmd.Path, cmd.Args = sleep, []string{"sleep", "0"}
		} else {
			cmd.Path, cmd.Args = sleep, []string{"sleep", "30"}
		}
		return cmd.Start()
	}
	t.Cleanup(func() { core.stopHeadless("test over", true) })
	return f
}

func (f *headlessFixture) startCount() int {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.starts
}

func waitStopped(t *testing.T, core *Core) {
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		if _, running := core.headlessRunning(); !running {
			return
		}
		time.Sleep(10 * time.Millisecond)
	}
	t.Fatal("the desktop without a window never exited")
}

func TestHeadlessDesktopStartsAfterGraceWithoutWindow(t *testing.T) {
	f := newHeadlessFixture(t)
	core := f.core
	core.desktop.seenUI = true
	now := time.Now()
	core.tickHeadless(now, nil, headlessGrace-time.Second)
	if f.startCount() != 0 {
		t.Fatal("started inside the grace period")
	}
	core.tickHeadless(now, nil, headlessGrace)
	if _, running := core.headlessRunning(); !running || f.startCount() != 1 {
		t.Fatalf("not started after the grace: %d", f.startCount())
	}
	core.tickHeadless(now, nil, headlessGrace)
	if f.startCount() != 1 {
		t.Fatal("started twice")
	}
	state := core.headlessState()
	if state["running"] != true || state["enabled"] != true {
		t.Fatalf("state %v", state)
	}
	off := false
	config := core.reloadConfig()
	config.HeadlessDesktop = &off
	if err := core.saveConfig(config); err != nil {
		t.Fatal(err)
	}
	core.tickHeadless(now, nil, headlessGrace)
	waitStopped(t, core)
	core.tickHeadless(now.Add(time.Hour), nil, time.Hour)
	if f.startCount() != 1 {
		t.Fatal("started while turned off")
	}
}

func TestHeadlessDesktopStartsSoonAfterBoot(t *testing.T) {
	f := newHeadlessFixture(t)
	core := f.core
	now := time.Now()
	core.tickHeadless(now, nil, headlessBootGrace-time.Second)
	if f.startCount() != 0 {
		t.Fatal("started before the boot grace")
	}
	core.tickHeadless(now, nil, headlessBootGrace)
	if f.startCount() != 1 {
		t.Fatal("waited for the window grace although no window has opened since the engine started")
	}
}

func TestHeadlessDesktopIgnoresLockFromEarlierBoot(t *testing.T) {
	f := newHeadlessFixture(t)
	core := f.core
	now := time.Now()
	lock := filepath.Join(core.Layout.State, windowLockFile)
	_ = os.MkdirAll(core.Layout.State, 0o700)
	_ = os.WriteFile(lock, []byte("777\nForkMesh\nhost\n"), 0o600)
	f.mu.Lock()
	f.locked = 777
	f.mu.Unlock()
	core.desktop.booted = func() time.Time { return now.Add(-time.Hour) }
	core.tickHeadless(now, nil, time.Hour)
	if f.startCount() != 0 {
		t.Fatal("started while a window from this boot holds the lock")
	}
	written := now.Add(-2 * time.Hour)
	_ = os.Chtimes(lock, written, written)
	core.tickHeadless(now, nil, time.Hour)
	if f.startCount() != 1 {
		t.Fatal("a lock left by the last boot blocked the start because its pid was reused")
	}
}

func TestHeadlessDesktopNeedsBackgroundSignInAndFreeLock(t *testing.T) {
	f := newHeadlessFixture(t)
	core := f.core
	now := time.Now()
	if err := core.Forget(signedInFile); err != nil {
		t.Fatal(err)
	}
	core.tickHeadless(now, nil, time.Hour)
	if f.startCount() != 0 {
		t.Fatal("started before the desktop ever signed in")
	}
	_ = core.Remember(signedInFile, map[string]string{"name": "box"})
	_ = core.saveConfig(DaemonConfig{})
	core.tickHeadless(now, nil, time.Hour)
	if f.startCount() != 0 {
		t.Fatal("started outside background mode")
	}
	_ = core.saveConfig(DaemonConfig{Background: true})
	_ = os.MkdirAll(core.Layout.State, 0o700)
	_ = os.WriteFile(filepath.Join(core.Layout.State, windowLockFile), []byte(strconv.Itoa(777)+"\nForkMesh\nhost\n"), 0o600)
	f.mu.Lock()
	f.locked = 777
	f.mu.Unlock()
	core.tickHeadless(now, nil, time.Hour)
	if f.startCount() != 0 {
		t.Fatal("started while a window holds the lock")
	}
	f.mu.Lock()
	f.locked = 0
	f.mu.Unlock()
	core.tickHeadless(now, nil, time.Hour)
	if f.startCount() != 1 {
		t.Fatal("a stale lock blocked the start")
	}
}

func TestHeadlessDesktopBacksOffAfterQuickExit(t *testing.T) {
	f := newHeadlessFixture(t)
	core := f.core
	f.failing = true
	now := time.Now()
	core.tickHeadless(now, nil, time.Hour)
	waitStopped(t, core)
	core.desktop.mu.Lock()
	retry := core.desktop.retryAt
	core.desktop.mu.Unlock()
	if retry.Before(time.Now().Add(headlessBackoffMin - 5*time.Second)) {
		t.Fatalf("no backoff after a quick exit: %v", retry)
	}
	core.tickHeadless(time.Now(), nil, time.Hour)
	if f.startCount() != 1 {
		t.Fatal("restarted inside the backoff")
	}
	core.tickHeadless(retry.Add(time.Second), nil, time.Hour)
	if f.startCount() != 2 {
		t.Fatal("not restarted after the backoff")
	}
	waitStopped(t, core)
	core.desktop.mu.Lock()
	backoff := core.desktop.backoff
	core.desktop.mu.Unlock()
	if backoff != 2*headlessBackoffMin {
		t.Fatalf("backoff did not double: %v", backoff)
	}
}

func TestHeadlessDesktopRestartsOnNewBinary(t *testing.T) {
	f := newHeadlessFixture(t)
	core := f.core
	now := time.Now()
	core.tickHeadless(now, nil, time.Hour)
	if _, running := core.headlessRunning(); !running {
		t.Fatal("not started")
	}
	binary := filepath.Join(f.dir, "bin", "forkmesh")
	replaced := now.Add(-time.Minute)
	_ = os.WriteFile(binary, []byte("new build"), 0o755)
	_ = os.Chtimes(binary, replaced, replaced)
	core.tickHeadless(now, nil, 0)
	waitStopped(t, core)
	core.desktop.mu.Lock()
	retry := core.desktop.retryAt
	core.desktop.mu.Unlock()
	if !retry.IsZero() {
		t.Fatal("a requested stop counted as a failure")
	}
	core.tickHeadless(now, nil, headlessGrace)
	if f.startCount() != 2 {
		t.Fatal("not started again on the new binary")
	}
}

func TestHeadlessDesktopThatNeverConnectsIsStopped(t *testing.T) {
	f := newHeadlessFixture(t)
	core := f.core
	now := time.Now()
	core.tickHeadless(now, nil, time.Hour)
	core.tickHeadless(now.Add(headlessAttachWait-time.Second), nil, time.Hour)
	if _, running := core.headlessRunning(); !running {
		t.Fatal("stopped before the attach wait")
	}
	core.tickHeadless(now.Add(headlessAttachWait), nil, time.Hour)
	waitStopped(t, core)
	core.desktop.mu.Lock()
	retry := core.desktop.retryAt
	core.desktop.mu.Unlock()
	if retry.IsZero() {
		t.Fatal("a desktop that never connected was not backed off")
	}
}
