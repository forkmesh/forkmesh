//go:build linux

package plugins

import (
	"os"
	"os/exec"
	"runtime"
	"syscall"
)

const prSetNoNewPrivs = 38

func configureSandbox(cmd *exec.Cmd, isolateNetwork bool) {
	attr := &syscall.SysProcAttr{Setpgid: true, Pdeathsig: syscall.SIGKILL}
	if isolateNetwork {
		attr.Cloneflags = syscall.CLONE_NEWUSER | syscall.CLONE_NEWNET
		attr.UidMappings = []syscall.SysProcIDMap{{ContainerID: os.Getuid(), HostID: os.Getuid(), Size: 1}}
		attr.GidMappings = []syscall.SysProcIDMap{{ContainerID: os.Getgid(), HostID: os.Getgid(), Size: 1}}
	}
	cmd.SysProcAttr = attr
}

func networkIsolationSupported() bool { return true }

func applyLimits(nofile uint64, memoryBytes uint64) error {
	if err := syscall.Setrlimit(syscall.RLIMIT_NOFILE, &syscall.Rlimit{Cur: nofile, Max: nofile}); err != nil {
		return err
	}
	_ = syscall.Setrlimit(syscall.RLIMIT_CORE, &syscall.Rlimit{})
	if memoryBytes > 0 {
		if err := syscall.Setrlimit(syscall.RLIMIT_DATA, &syscall.Rlimit{Cur: memoryBytes, Max: memoryBytes}); err != nil {
			return err
		}
	}
	return nil
}

func execRestricted(argv []string, env []string) error {
	runtime.LockOSThread()
	if _, _, errno := syscall.RawSyscall6(syscall.SYS_PRCTL, prSetNoNewPrivs, 1, 0, 0, 0, 0); errno != 0 {
		return errno
	}
	return syscall.Exec(argv[0], argv, env)
}

func killGroup(pid int, signal syscall.Signal) {
	_ = syscall.Kill(-pid, signal)
}
