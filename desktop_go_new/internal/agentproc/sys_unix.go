//go:build !windows

package agentproc

import (
	"os"
	"os/signal"
	"strings"
	"syscall"
)

const (
	sigTerm = int(syscall.SIGTERM)
	sigKill = int(syscall.SIGKILL)
)

var signalNames = map[string]int{
	"TERM": int(syscall.SIGTERM), "KILL": int(syscall.SIGKILL), "INT": int(syscall.SIGINT),
	"HUP": int(syscall.SIGHUP), "QUIT": int(syscall.SIGQUIT), "USR1": int(syscall.SIGUSR1),
	"USR2": int(syscall.SIGUSR2), "STOP": int(syscall.SIGSTOP), "CONT": int(syscall.SIGCONT),
}

func shimAttr() *syscall.SysProcAttr  { return &syscall.SysProcAttr{Setsid: true} }
func childAttr() *syscall.SysProcAttr { return &syscall.SysProcAttr{Setpgid: true} }

func signalPid(pid, sig int) error {
	if pid <= 1 {
		return syscall.ESRCH
	}
	return syscall.Kill(pid, syscall.Signal(sig))
}

func signalGroup(pgid, sig int) error {
	if pgid <= 1 {
		return syscall.ESRCH
	}
	return syscall.Kill(-pgid, syscall.Signal(sig))
}

func pidAlive(pid int) bool {
	if pid <= 1 {
		return false
	}
	err := syscall.Kill(pid, 0)
	return err == nil || err == syscall.EPERM
}

func exitStatus(state *os.ProcessState) (code, sig int, crashed bool) {
	if status, ok := state.Sys().(syscall.WaitStatus); ok && status.Signaled() {
		return int(status.Signal()), int(status.Signal()), true
	}
	return state.ExitCode(), 0, false
}

func shimSignals() (<-chan os.Signal, func()) {
	signal.Ignore(syscall.SIGHUP, syscall.SIGPIPE)
	terms := make(chan os.Signal, 4)
	signal.Notify(terms, syscall.SIGTERM, syscall.SIGINT)
	return terms, func() { signal.Stop(terms) }
}

func bootID() string {
	raw, err := os.ReadFile("/proc/sys/kernel/random/boot_id")
	if err != nil {
		return ""
	}
	return strings.TrimSpace(string(raw))
}

func privateDir(path string) bool {
	info, err := os.Lstat(path)
	if err != nil || !info.IsDir() || info.Mode().Perm()&0o077 != 0 {
		return false
	}
	stat, ok := info.Sys().(*syscall.Stat_t)
	return ok && int(stat.Uid) == os.Getuid()
}
