//go:build !linux && !darwin

package actions

import (
	"errors"
	"os"
	"os/exec"
	"path/filepath"
)

func resolveProgram(program string) (string, error) {
	if filepath.IsAbs(program) {
		return program, nil
	}
	return exec.LookPath(program)
}

func helperCommand(program string, args []string, sandboxed bool, limits Limits) (*exec.Cmd, error) {
	resolved, err := resolveProgram(program)
	if err != nil {
		return nil, err
	}
	cmd := exec.Command(resolved, args...)
	cmd.Args[0] = program
	hideWindow(cmd)
	return cmd, nil
}

func terminateGroup(cmd *exec.Cmd) {
	if cmd != nil && cmd.Process != nil {
		_ = cmd.Process.Kill()
	}
}

func killGroup(cmd *exec.Cmd) {
	if cmd != nil && cmd.Process != nil {
		_ = cmd.Process.Kill()
	}
}

func exitStatus(err error) (int, bool) {
	if err == nil {
		return 0, false
	}
	var exit *exec.ExitError
	if errors.As(err, &exit) {
		return exit.ExitCode(), false
	}
	return -1, true
}

func configureCapture(cmd *exec.Cmd) {
	hideWindow(cmd)
	cmd.Cancel = func() error {
		if cmd.Process == nil {
			return os.ErrProcessDone
		}
		return cmd.Process.Kill()
	}
}

func isRoot() bool { return false }

func currentUID() int { return os.Getuid() }

type fileLock struct {
	file *os.File
}

func tryLock(path string) (*fileLock, bool) { return nil, false }

func (l *fileLock) release() {}

func lockHeldElsewhere(path string) bool { return false }

func availableBytes(path string) int64 { return -1 }
