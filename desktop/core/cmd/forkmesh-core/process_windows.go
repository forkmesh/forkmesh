//go:build windows

package main

import (
	"os"
	"syscall"
)

const (
	createNewProcessGroup = 0x00000200
	detachedProcess       = 0x00000008
)

func spawnDetached(binary string, args []string) (int, error) {
	home, _ := os.UserHomeDir()
	process, err := os.StartProcess(binary, append([]string{binary}, args...), &os.ProcAttr{
		Dir: home, Env: os.Environ(),
		Sys: &syscall.SysProcAttr{CreationFlags: createNewProcessGroup | detachedProcess, HideWindow: true}})
	if err != nil {
		return 0, err
	}
	pid := process.Pid
	_ = process.Release()
	return pid, nil
}

func reexec() error {
	_, err := spawnDetached(selfBinary(), os.Args[1:])
	return err
}

func detachConsole() {
	_, _, _ = syscall.NewLazyDLL("kernel32.dll").NewProc("FreeConsole").Call()
}
