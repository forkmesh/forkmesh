package daemon

import (
	"context"
	"encoding/json"
	"errors"
	"os"
	"os/exec"
	"path/filepath"
	"runtime/debug"
	"strings"
	"sync"
	"time"

	"forkmesh.com/desktop-core/internal/autostart"
	"forkmesh.com/desktop-core/internal/rpc"
)

var (
	ErrRestart        = errors.New("daemon: restart requested")
	ErrAlreadyRunning = errors.New("daemon: another forkmesh-core is already running for this project")
)

type DaemonConfig struct {
	Background      bool  `json:"background"`
	HeadlessDesktop *bool `json:"headlessDesktop,omitempty"`
	UpdatedMs       int64 `json:"updatedMs,omitempty"`
}

type lifecycle struct {
	mu            sync.Mutex
	flagBg        bool
	config        DaemonConfig
	configMod     time.Time
	configPath    string
	executable    string
	executableMod time.Time
	pendingMod    time.Time
	restart       bool
	lastExit      map[string]any
	trimmed       bool
	autostart     autostart.Manager
	verify        func(binary string) bool
}

func (c *Core) DaemonConfigPath() string {
	return filepath.Join(c.Layout.Config, "core", "daemon.json")
}

func (c *Core) initLifecycle(options Options) {
	c.life.flagBg = options.Background
	c.life.configPath = c.DaemonConfigPath()
	c.life.autostart = options.Autostart
	if c.life.autostart.GOOS == "" {
		c.life.autostart = autostart.System()
	}
	c.life.verify = options.VerifyBinary
	if c.life.verify == nil {
		c.life.verify = verifyBinary
	}
	c.life.executable = options.Executable
	if c.life.executable == "" {
		if path, err := os.Executable(); err == nil {
			if resolved, err := filepath.EvalSymlinks(path); err == nil {
				path = resolved
			}
			c.life.executable = path
		}
	}
	if info, err := os.Stat(c.life.executable); err == nil {
		c.life.executableMod = info.ModTime()
	}
	c.reloadConfig()
}

func (c *Core) reloadConfig() DaemonConfig {
	c.life.mu.Lock()
	defer c.life.mu.Unlock()
	info, err := os.Stat(c.life.configPath)
	if err != nil {
		c.life.config = DaemonConfig{}
		c.life.configMod = time.Time{}
		return c.life.config
	}
	if info.ModTime().Equal(c.life.configMod) {
		return c.life.config
	}
	var config DaemonConfig
	if raw, err := os.ReadFile(c.life.configPath); err == nil && json.Unmarshal(raw, &config) == nil {
		c.life.config = config
		c.life.configMod = info.ModTime()
	}
	return c.life.config
}

func (c *Core) saveConfig(config DaemonConfig) error {
	config.UpdatedMs = time.Now().UnixMilli()
	raw, _ := json.MarshalIndent(config, "", "  ")
	if err := os.MkdirAll(filepath.Dir(c.life.configPath), 0o700); err != nil {
		return err
	}
	temp := c.life.configPath + ".tmp"
	if err := os.WriteFile(temp, append(raw, '\n'), 0o600); err != nil {
		return err
	}
	if err := os.Rename(temp, c.life.configPath); err != nil {
		return err
	}
	c.life.mu.Lock()
	c.life.config = config
	if info, err := os.Stat(c.life.configPath); err == nil {
		c.life.configMod = info.ModTime()
	}
	c.life.mu.Unlock()
	return nil
}

func (c *Core) Background() bool {
	if c.life.flagBg {
		return true
	}
	return c.reloadConfig().Background
}

func (c *Core) RestartRequested() bool {
	c.life.mu.Lock()
	defer c.life.mu.Unlock()
	return c.life.restart
}

func (c *Core) Restart() {
	c.life.mu.Lock()
	c.life.restart = true
	c.life.mu.Unlock()
	c.Shutdown()
}

func (c *Core) PreviousStop() (time.Time, bool) {
	c.life.mu.Lock()
	defer c.life.mu.Unlock()
	if c.life.lastExit["kind"] != "clean" {
		return time.Time{}, false
	}
	stopped, _ := c.life.lastExit["stoppedMs"].(float64)
	restart, _ := c.life.lastExit["restart"].(bool)
	if stopped <= 0 {
		return time.Time{}, restart
	}
	return time.UnixMilli(int64(stopped)), restart
}

func markerPath(stateDir string) string { return filepath.Join(stateDir, "running.json") }

func (c *Core) recordStart(stateDir string) {
	path := markerPath(stateDir)
	previous := map[string]any{"kind": "first"}
	if raw, err := os.ReadFile(path); err == nil {
		var marker map[string]any
		if json.Unmarshal(raw, &marker) == nil {
			marker["kind"] = "crash"
			previous = marker
		}
	} else if _, err := os.Stat(filepath.Join(stateDir, "stopped.json")); err == nil {
		previous = map[string]any{"kind": "clean"}
		if raw, err := os.ReadFile(filepath.Join(stateDir, "stopped.json")); err == nil {
			_ = json.Unmarshal(raw, &previous)
			previous["kind"] = "clean"
		}
	}
	c.life.lastExit = previous
	if previous["kind"] == "crash" {
		c.Logger.Printf("the previous forkmesh-core (pid %v) did not stop cleanly; recovering", previous["pid"])
	}
	raw, _ := json.Marshal(map[string]any{"pid": os.Getpid(), "version": c.Version, "startedMs": c.Started.UnixMilli()})
	_ = os.WriteFile(path, raw, 0o600)
}

func (c *Core) recordStop(stateDir string) {
	raw, _ := json.Marshal(map[string]any{"pid": os.Getpid(), "version": c.Version,
		"startedMs": c.Started.UnixMilli(), "stoppedMs": time.Now().UnixMilli(), "restart": c.RestartRequested()})
	_ = os.WriteFile(filepath.Join(stateDir, "stopped.json"), raw, 0o600)
	_ = os.Remove(markerPath(stateDir))
}

func verifyBinary(binary string) bool {
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	out, err := exec.CommandContext(ctx, binary, "version").Output()
	return err == nil && len(strings.TrimSpace(string(out))) > 0
}

func (c *Core) upgradeReady() bool {
	if c.life.executable == "" || c.life.executableMod.IsZero() {
		return false
	}
	info, err := os.Stat(c.life.executable)
	if err != nil || info.Size() == 0 || !info.ModTime().After(c.life.executableMod.Add(time.Second)) {
		c.life.pendingMod = time.Time{}
		return false
	}
	if !info.ModTime().Equal(c.life.pendingMod) {
		c.life.pendingMod = info.ModTime()
		return false
	}
	if !c.life.verify(c.life.executable) {
		c.Logger.Printf("the forkmesh-core binary changed but does not run; staying on %s", c.Version)
		c.life.executableMod = info.ModTime()
		return false
	}
	return true
}

func (c *Core) supervise(ctx context.Context, linger time.Duration, tick time.Duration) {
	ticker := time.NewTicker(tick)
	defer ticker.Stop()
	upgradeEvery := int((10 * time.Second) / tick)
	if upgradeEvery < 1 {
		upgradeEvery = 1
	}
	ticks := 0
	for {
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
		}
		ticks++
		c.mu.Lock()
		ui := c.IPC.UI()
		online := ui != nil
		window := online && !ui.Headless()
		if online {
			c.lastUI = time.Now()
		}
		idle := time.Since(c.lastUI)
		c.mu.Unlock()
		c.tickHeadless(time.Now(), ui, idle)
		busy := !window && c.busy()
		if window || busy {
			c.life.trimmed = false
		} else if !c.life.trimmed && idle > time.Minute {
			c.life.trimmed = true
			debug.FreeOSMemory()
		}
		if !window && !busy && ticks%upgradeEvery == 0 && c.upgradeReady() {
			c.Logger.Printf("the forkmesh-core binary was replaced; restarting on it")
			c.Restart()
			return
		}
		if linger > 0 && !online && idle > linger && !busy && !c.Background() {
			c.Logger.Printf("no desktop attached for %s and nothing in flight; exiting", idle.Round(time.Second))
			c.Shutdown()
			return
		}
	}
}

func (c *Core) daemonState() map[string]any {
	status := c.life.autostart.Status(c.Layout.Project)
	return map[string]any{
		"background":       c.Background(),
		"backgroundFlag":   c.life.flagBg,
		"backgroundConfig": c.reloadConfig().Background,
		"autostart":        status,
		"binary":           c.life.executable,
		"lastExit":         c.life.lastExit,
		"configPath":       c.life.configPath,
		"headlessDesktop":  c.headlessState(),
	}
}

func (c *Core) registerLifecycle() {
	local := func(handler rpc.Handler) rpc.Handler {
		return func(ctx context.Context, req *rpc.Request) (any, error) {
			if req.Peer == nil || rpc.Remote(req.Peer) {
				return nil, rpc.Fail("forbidden")
			}
			return handler(ctx, req)
		}
	}
	c.Registry.Handle("core.daemon.get", "core", local(func(ctx context.Context, req *rpc.Request) (any, error) {
		return c.daemonState(), nil
	}))
	c.Registry.Handle("core.daemon.set", "core", local(func(ctx context.Context, req *rpc.Request) (any, error) {
		var params struct {
			Background      *bool  `json:"background"`
			HeadlessDesktop *bool  `json:"headlessDesktop"`
			Autostart       *bool  `json:"autostart"`
			Binary          string `json:"binary"`
		}
		if err := req.Bind(&params); err != nil {
			return nil, err
		}
		if params.Background != nil {
			config := c.reloadConfig()
			config.Background = *params.Background
			if err := c.saveConfig(config); err != nil {
				return nil, rpc.Fail("config_write_failed")
			}
			c.Logger.Printf("background mode %s", map[bool]string{true: "on", false: "off"}[*params.Background])
		}
		if params.HeadlessDesktop != nil {
			config := c.reloadConfig()
			config.HeadlessDesktop = params.HeadlessDesktop
			if err := c.saveConfig(config); err != nil {
				return nil, rpc.Fail("config_write_failed")
			}
			c.Logger.Printf("desktop without a window %s", map[bool]string{true: "on", false: "off"}[*params.HeadlessDesktop])
		}
		if params.Autostart != nil {
			var err error
			if *params.Autostart {
				binary := strings.TrimSpace(params.Binary)
				if binary == "" {
					binary = c.life.executable
				}
				_, err = c.life.autostart.Enable(binary, c.Layout.Project)
			} else {
				_, err = c.life.autostart.Disable(c.Layout.Project)
			}
			if err != nil {
				c.Logger.Printf("autostart: %v", err)
				switch {
				case errors.Is(err, autostart.ErrBadBinary):
					return nil, rpc.Fail("bad_binary")
				case errors.Is(err, autostart.ErrBadProject):
					return nil, rpc.Fail("bad_project")
				}
				return nil, rpc.Fail("autostart_failed")
			}
		}
		state := c.daemonState()
		c.BroadcastUI("core.daemon.changed", state)
		return state, nil
	}))
	c.Registry.Handle("core.daemon.restart", "core", local(func(ctx context.Context, req *rpc.Request) (any, error) {
		go func() {
			time.Sleep(50 * time.Millisecond)
			c.Restart()
		}()
		return map[string]any{"restarting": true}, nil
	}))
}
