package daemon

import (
	"os"
	"os/exec"
	"runtime"
	"strings"

	"forkmesh.com/desktop-core/internal/autostart"
	"forkmesh.com/desktop-core/internal/paths"
)

func graphicalSession(goos string, env func(string) string) bool {
	switch goos {
	case "darwin", "windows":
		return true
	}
	return strings.TrimSpace(env("DISPLAY")) != "" || strings.TrimSpace(env("WAYLAND_DISPLAY")) != ""
}

func trayCommand(goos string, env func(string) string, executable, project string) (string, []string, bool) {
	if executable == "" || !paths.IsValidSlug(project) || !graphicalSession(goos, env) {
		return "", nil, false
	}
	desktop := autostart.DesktopBinary(goos, executable)
	if info, err := os.Stat(desktop); err != nil || info.IsDir() {
		return "", nil, false
	}
	return desktop, autostart.TrayArguments(project), true
}

func (c *Core) startTray(options Options) {
	if options.StateDir != "" || options.Env != nil || options.Home != "" {
		return
	}
	binary, args, ok := trayCommand(runtime.GOOS, os.Getenv, c.life.executable, c.Layout.Project)
	if !ok {
		return
	}
	cmd := exec.Command(binary, args...)
	if home, err := os.UserHomeDir(); err == nil {
		cmd.Dir = home
	}
	detachTray(cmd)
	if err := cmd.Start(); err != nil {
		c.Logger.Printf("tray: %v", err)
		return
	}
	c.Logger.Printf("tray started (pid %d)", cmd.Process.Pid)
	go func() { _ = cmd.Wait() }()
}
