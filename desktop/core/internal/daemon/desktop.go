package daemon

import (
	"bufio"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"

	"forkmesh.com/desktop-core/internal/autostart"
	"forkmesh.com/desktop-core/internal/ipc"
	"forkmesh.com/desktop-core/internal/paths"
)

const (
	headlessGrace        = time.Minute
	headlessQuickExit    = 2 * time.Minute
	headlessBackoffMin   = time.Minute
	headlessBackoffMax   = 30 * time.Minute
	headlessStopWait     = 20 * time.Second
	headlessAttachWait   = 5 * time.Minute
	headlessBinarySettle = 10 * time.Second
	signedInFile         = "control-info.json"
	windowLockFile       = "forkmesh.lock"
)

type headlessDesktop struct {
	mu        sync.Mutex
	allowed   bool
	start     func(cmd *exec.Cmd) error
	alive     func(pid int) bool
	cmd       *exec.Cmd
	exited    chan struct{}
	startedAt time.Time
	binary    string
	binaryMod time.Time
	stopping  bool
	attached  bool
	failed    bool
	backoff   time.Duration
	retryAt   time.Time
}

func HeadlessArguments(project string) []string {
	return []string{"--headless", "--project", project}
}

func HeadlessLabel(project string) string { return "forkmesh-desktop-" + project }

func headlessCommand(goos, executable, project, systemdRun string, environ []string) (string, []string, []string, bool) {
	if goos != "linux" || executable == "" || !paths.IsValidSlug(project) {
		return "", nil, nil, false
	}
	desktop := autostart.DesktopBinary(goos, executable)
	if info, err := os.Stat(desktop); err != nil || info.IsDir() {
		return "", nil, nil, false
	}
	program, args := desktop, HeadlessArguments(project)
	if systemdRun != "" {
		program = systemdRun
		args = append([]string{"--user", "--scope", "--quiet", "--collect", "--unit=" + HeadlessLabel(project), "--", desktop}, args...)
	}
	env := append(withoutServiceVariables(environ), "QT_QPA_PLATFORM=offscreen", "FORKMESH_NO_SPLASH=1")
	return program, args, env, true
}

func lockHolder(path string) int {
	file, err := os.Open(path)
	if err != nil {
		return 0
	}
	defer file.Close()
	line, _ := bufio.NewReader(file).ReadString('\n')
	pid, err := strconv.Atoi(strings.TrimSpace(line))
	if err != nil {
		return 0
	}
	return pid
}

func (c *Core) initHeadless(options Options) {
	defaultState := options.StateDir == "" || filepath.Clean(options.StateDir) == filepath.Clean(c.Layout.CoreState())
	c.desktop.allowed = runtime.GOOS == "linux" && defaultState && options.Env == nil && options.Home == ""
	c.desktop.start = func(cmd *exec.Cmd) error { return cmd.Start() }
	c.desktop.alive = processAlive
}

func (c *Core) HeadlessDesktopEnabled() bool {
	enabled := c.reloadConfig().HeadlessDesktop
	return enabled == nil || *enabled
}

func (c *Core) headlessWanted() bool {
	if !c.desktop.allowed || !c.Background() || !c.HeadlessDesktopEnabled() {
		return false
	}
	_, err := os.Stat(c.rememberedPath(signedInFile))
	return err == nil
}

func (c *Core) headlessRunning() (int, bool) {
	c.desktop.mu.Lock()
	defer c.desktop.mu.Unlock()
	if c.desktop.cmd == nil {
		return 0, false
	}
	return c.desktop.cmd.Process.Pid, true
}

func (c *Core) tickHeadless(now time.Time, ui *ipc.Conn, idle time.Duration) {
	if !c.desktop.allowed {
		return
	}
	if ui != nil && !ui.Headless() {
		c.desktop.mu.Lock()
		c.desktop.backoff = 0
		c.desktop.retryAt = time.Time{}
		c.desktop.mu.Unlock()
		return
	}
	wanted := c.headlessWanted()
	if ui != nil && ui.Headless() && !wanted {
		c.stopAttachedHeadless(ui, "the engine no longer runs ForkMesh without a window")
		return
	}
	if _, running := c.headlessRunning(); running {
		c.desktop.mu.Lock()
		c.desktop.attached = c.desktop.attached || ui != nil
		silent := !c.desktop.attached && !c.desktop.stopping && now.Sub(c.desktop.startedAt) >= headlessAttachWait
		if silent {
			c.desktop.failed = true
		}
		c.desktop.mu.Unlock()
		if silent {
			c.stopHeadless("it never connected to the engine", false)
		} else if !wanted {
			c.stopHeadless("the engine no longer runs ForkMesh without a window", false)
		} else if c.headlessBinaryReplaced(now) {
			c.stopHeadless("the ForkMesh binary was replaced; it starts again on the new one", false)
		}
		return
	}
	if !wanted || ui != nil || idle < headlessGrace {
		return
	}
	c.desktop.mu.Lock()
	wait := now.Before(c.desktop.retryAt)
	c.desktop.mu.Unlock()
	if wait {
		return
	}
	if pid := lockHolder(filepath.Join(c.Layout.State, windowLockFile)); pid > 0 && c.desktop.alive(pid) {
		return
	}
	c.launchHeadless(now)
}

func (c *Core) headlessBinaryReplaced(now time.Time) bool {
	c.desktop.mu.Lock()
	defer c.desktop.mu.Unlock()
	if c.desktop.stopping || c.desktop.binary == "" {
		return false
	}
	info, err := os.Stat(c.desktop.binary)
	if err != nil || info.Size() == 0 || info.ModTime().Equal(c.desktop.binaryMod) {
		return false
	}
	return now.Sub(info.ModTime()) >= headlessBinarySettle
}

func (c *Core) launchHeadless(now time.Time) {
	systemdRun := ""
	if os.Getenv("INVOCATION_ID") != "" {
		systemdRun, _ = exec.LookPath("systemd-run")
	}
	program, args, env, ok := headlessCommand(runtime.GOOS, c.life.executable, c.Layout.Project, systemdRun, os.Environ())
	if !ok {
		return
	}
	cmd := exec.Command(program, args...)
	cmd.Env = env
	if home, err := os.UserHomeDir(); err == nil {
		cmd.Dir = home
	}
	detachTray(cmd)
	binary := autostart.DesktopBinary(runtime.GOOS, c.life.executable)
	var binaryMod time.Time
	if info, err := os.Stat(binary); err == nil {
		binaryMod = info.ModTime()
	}
	c.desktop.mu.Lock()
	if err := c.desktop.start(cmd); err != nil {
		c.desktop.mu.Unlock()
		c.Logger.Printf("desktop without a window: could not start %s: %v", binary, err)
		c.headlessExited(now, false)
		return
	}
	exited := make(chan struct{})
	c.desktop.cmd = cmd
	c.desktop.exited = exited
	c.desktop.startedAt = now
	c.desktop.binary = binary
	c.desktop.binaryMod = binaryMod
	c.desktop.stopping = false
	c.desktop.attached = false
	c.desktop.failed = false
	c.desktop.mu.Unlock()
	c.Logger.Printf("desktop without a window started (pid %d), so the phone has every desktop feature while no window is open", cmd.Process.Pid)
	go func() {
		err := cmd.Wait()
		c.desktop.mu.Lock()
		stopping := c.desktop.stopping && !c.desktop.failed
		c.desktop.cmd = nil
		c.desktop.exited = nil
		c.desktop.mu.Unlock()
		close(exited)
		if err != nil {
			c.Logger.Printf("desktop without a window (pid %d) exited: %v", cmd.Process.Pid, err)
		} else {
			c.Logger.Printf("desktop without a window (pid %d) exited", cmd.Process.Pid)
		}
		c.headlessExited(time.Now(), stopping)
	}()
}

func (c *Core) headlessExited(now time.Time, requested bool) {
	c.desktop.mu.Lock()
	defer c.desktop.mu.Unlock()
	if requested || (!c.desktop.startedAt.IsZero() && now.Sub(c.desktop.startedAt) >= headlessQuickExit) {
		c.desktop.backoff = 0
		c.desktop.retryAt = time.Time{}
		return
	}
	c.desktop.backoff = min(max(c.desktop.backoff*2, headlessBackoffMin), headlessBackoffMax)
	c.desktop.retryAt = now.Add(c.desktop.backoff)
}

func (c *Core) stopHeadless(reason string, wait bool) {
	c.desktop.mu.Lock()
	cmd, exited := c.desktop.cmd, c.desktop.exited
	if cmd != nil {
		c.desktop.stopping = true
	}
	c.desktop.mu.Unlock()
	if cmd == nil {
		return
	}
	c.Logger.Printf("stopping the desktop without a window (pid %d): %s", cmd.Process.Pid, reason)
	_ = cmd.Process.Signal(syscall.SIGTERM)
	if !wait {
		return
	}
	select {
	case <-exited:
	case <-time.After(headlessStopWait):
		c.Logger.Printf("the desktop without a window (pid %d) did not stop within %s", cmd.Process.Pid, headlessStopWait)
	}
}

func (c *Core) stopAttachedHeadless(ui *ipc.Conn, reason string) {
	if _, ours := c.headlessRunning(); ours {
		c.stopHeadless(reason, false)
		return
	}
	pid, ok := ui.PeerPID()
	if !ok || pid <= 1 {
		return
	}
	if process, err := os.FindProcess(pid); err == nil {
		c.Logger.Printf("stopping the desktop without a window (pid %d): %s", pid, reason)
		_ = process.Signal(syscall.SIGTERM)
	}
}

func (c *Core) headlessState() map[string]any {
	state := map[string]any{"enabled": c.HeadlessDesktopEnabled(), "supported": c.desktop.allowed, "running": false}
	if pid, running := c.headlessRunning(); running {
		state["running"] = true
		state["pid"] = pid
	}
	if ui := c.IPC.UI(); ui != nil && ui.Headless() {
		state["attached"] = true
	}
	return state
}
