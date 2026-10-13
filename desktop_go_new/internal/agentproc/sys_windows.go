//go:build windows

package agentproc

import (
	"errors"
	"os"
	"os/signal"
	"syscall"
)

const (
	sigTerm = 15
	sigKill = 9

	createNewProcessGroup = 0x00000200
	detachedProcess       = 0x00000008
	stillActive           = 259
	queryLimited          = 0x1000
)

var signalNames = map[string]int{"TERM": sigTerm, "KILL": sigKill, "INT": 2}

func shimAttr() *syscall.SysProcAttr {
	return &syscall.SysProcAttr{CreationFlags: createNewProcessGroup | detachedProcess}
}

func childAttr() *syscall.SysProcAttr {
	return &syscall.SysProcAttr{CreationFlags: createNewProcessGroup}
}

func signalPid(pid, sig int) error {
	if pid <= 0 {
		return errors.New("no such process")
	}
	process, err := os.FindProcess(pid)
	if err != nil {
		return err
	}
	return process.Kill()
}

func signalGroup(pgid, sig int) error { return signalPid(pgid, sig) }

func pidAlive(pid int) bool {
	if pid <= 0 {
		return false
	}
	handle, err := syscall.OpenProcess(queryLimited, false, uint32(pid))
	if err != nil {
		return false
	}
	defer syscall.CloseHandle(handle)
	var code uint32
	if err := syscall.GetExitCodeProcess(handle, &code); err != nil {
		return false
	}
	return code == stillActive
}

func exitStatus(state *os.ProcessState) (code, sig int, crashed bool) {
	return state.ExitCode(), 0, false
}

func shimSignals() (<-chan os.Signal, func()) {
	terms := make(chan os.Signal, 4)
	signal.Notify(terms, os.Interrupt)
	return terms, func() { signal.Stop(terms) }
}

func bootID() string { return "" }

func privateDir(path string) bool {
	info, err := os.Lstat(path)
	return err == nil && info.IsDir()
}
