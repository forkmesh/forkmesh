//go:build windows

package update

import (
	"os/exec"
	"syscall"
)

const createNewProcessGroup = 0x00000200
const detachedProcess = 0x00000008

func prepareStep(cmd *exec.Cmd) {
	cmd.SysProcAttr = &syscall.SysProcAttr{CreationFlags: createNewProcessGroup}
}

func signalGroup(cmd *exec.Cmd, kill bool) {
	if cmd.Process != nil {
		_ = cmd.Process.Kill()
	}
}

func prepareReleased(cmd *exec.Cmd) {
	cmd.SysProcAttr = &syscall.SysProcAttr{CreationFlags: createNewProcessGroup | detachedProcess}
}

func processAlive(pid int) bool {
	if pid <= 0 {
		return false
	}
	handle, err := syscall.OpenProcess(syscall.PROCESS_QUERY_INFORMATION, false, uint32(pid))
	if err != nil {
		return false
	}
	defer syscall.CloseHandle(handle)
	var code uint32
	if err := syscall.GetExitCodeProcess(handle, &code); err != nil {
		return false
	}
	return code == 259
}
