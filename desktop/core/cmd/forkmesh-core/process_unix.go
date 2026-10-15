//go:build !windows

package main

import (
	"os"
	"syscall"
)

func spawnDetached(binary string, args []string) (int, error) {
	null, err := os.OpenFile(os.DevNull, os.O_RDWR, 0)
	if err != nil {
		return 0, err
	}
	defer null.Close()
	home, _ := os.UserHomeDir()
	process, err := os.StartProcess(binary, append([]string{binary}, args...), &os.ProcAttr{
		Dir: home, Env: os.Environ(), Files: []*os.File{null, null, null},
		Sys: &syscall.SysProcAttr{Setsid: true}})
	if err != nil {
		return 0, err
	}
	pid := process.Pid
	_ = process.Release()
	return pid, nil
}

func reexec() error {
	return syscall.Exec(selfBinary(), os.Args, os.Environ())
}

func detachConsole() {}
