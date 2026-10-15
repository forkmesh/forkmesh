//go:build windows

package daemon

import (
	"os/exec"
	"syscall"
)

func detachTray(cmd *exec.Cmd) {
	cmd.SysProcAttr = &syscall.SysProcAttr{CreationFlags: 0x00000200 | 0x00000008}
}
