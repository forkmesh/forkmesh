//go:build !windows

package daemon

import (
	"os/exec"
	"syscall"
)

func detachTray(cmd *exec.Cmd) {
	cmd.SysProcAttr = &syscall.SysProcAttr{Setsid: true}
}
