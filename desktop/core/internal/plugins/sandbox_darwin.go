//go:build darwin

package plugins

import (
	"os/exec"
	"syscall"
)

func configureSandbox(cmd *exec.Cmd, isolateNetwork bool) {
	cmd.SysProcAttr = &syscall.SysProcAttr{Setpgid: true}
}

func networkIsolationSupported() bool { return false }

func applyLimits(nofile uint64, memoryBytes uint64) error {
	if err := syscall.Setrlimit(syscall.RLIMIT_NOFILE, &syscall.Rlimit{Cur: nofile, Max: nofile}); err != nil {
		return err
	}
	_ = syscall.Setrlimit(syscall.RLIMIT_CORE, &syscall.Rlimit{})
	return nil
}

func execRestricted(argv []string, env []string) error {
	return syscall.Exec(argv[0], argv, env)
}

func killGroup(pid int, signal syscall.Signal) {
	_ = syscall.Kill(-pid, signal)
}
