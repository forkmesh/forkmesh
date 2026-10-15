//go:build !linux && !darwin

package plugins

import (
	"errors"
	"os"
	"os/exec"
	"syscall"
)

func configureSandbox(cmd *exec.Cmd, isolateNetwork bool) {}

func networkIsolationSupported() bool { return false }

func applyLimits(nofile uint64, memoryBytes uint64) error { return nil }

func execRestricted(argv []string, env []string) error {
	return errors.New("plugins: the shim is not used on this platform")
}

func killGroup(pid int, signal syscall.Signal) {
	if process, err := os.FindProcess(pid); err == nil {
		_ = process.Kill()
	}
}
