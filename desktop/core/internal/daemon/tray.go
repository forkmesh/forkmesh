package daemon

import (
	"os"
	"os/exec"
	"runtime"
	"slices"
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

var serviceVariables = []string{"INVOCATION_ID", "JOURNAL_STREAM", "SYSTEMD_EXEC_PID", "MANAGERPID", "NOTIFY_SOCKET",
	"LISTEN_PID", "LISTEN_FDS", "LISTEN_FDNAMES", "WATCHDOG_PID", "WATCHDOG_USEC"}

func withoutServiceVariables(environ []string) []string {
	out := make([]string, 0, len(environ))
	for _, entry := range environ {
		name, _, _ := strings.Cut(entry, "=")
		if !slices.Contains(serviceVariables, name) {
			out = append(out, entry)
		}
	}
	return out
}

func scopedTray(goos, systemdRun, binary string, args []string, project string) (string, []string) {
	if goos != "linux" || systemdRun == "" {
		return binary, args
	}
	scoped := []string{"--user", "--scope", "--quiet", "--collect", "--unit=" + autostart.TrayLabel(project), "--", binary}
	return systemdRun, append(scoped, args...)
}

func (c *Core) startTray(options Options) {
	if options.StateDir != "" || options.Env != nil || options.Home != "" {
		return
	}
	if wrote, err := c.life.autostart.EnsureTrayEntry(c.Layout.Project); err != nil {
		c.Logger.Printf("tray: could not add the tray to the log-in items: %v", err)
	} else if wrote {
		c.Logger.Printf("tray: added the tray to the log-in items, since the engine starts at boot")
	}
	binary, args, ok := trayCommand(runtime.GOOS, os.Getenv, c.life.executable, c.Layout.Project)
	if !ok {
		return
	}
	systemdRun := ""
	if os.Getenv("INVOCATION_ID") != "" {
		systemdRun, _ = exec.LookPath("systemd-run")
	}
	program, arguments := scopedTray(runtime.GOOS, systemdRun, binary, args, c.Layout.Project)
	cmd := exec.Command(program, arguments...)
	cmd.Env = withoutServiceVariables(os.Environ())
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
