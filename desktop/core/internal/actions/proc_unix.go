//go:build linux || darwin

package actions

import (
	"errors"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strconv"
	"syscall"
)

const execSentinel = "__forkmesh-actions-exec"

func init() {
	if len(os.Args) > 2 && os.Args[1] == execSentinel {
		os.Exit(execHelper(os.Args[2:]))
	}
}

func setLimit(resource int, value uint64) {
	_ = syscall.Setrlimit(resource, &syscall.Rlimit{Cur: value, Max: value})
}

func execHelper(args []string) int {
	if len(args) < 8 {
		return 127
	}
	sandboxed := args[0] == "1"
	addressSpace, _ := strconv.ParseUint(args[1], 10, 64)
	fileBytes, _ := strconv.ParseUint(args[2], 10, 64)
	openFiles, _ := strconv.ParseUint(args[3], 10, 64)
	cpuSeconds, _ := strconv.ParseUint(args[4], 10, 64)
	program := args[5]
	argv := append([]string{args[6]}, args[8:]...)
	syscall.Umask(0o077)
	setLimit(syscall.RLIMIT_CORE, 0)
	if sandboxed {
		setLimit(syscall.RLIMIT_AS, addressSpace)
		setLimit(syscall.RLIMIT_FSIZE, fileBytes)
		var inherited syscall.Rlimit
		if syscall.Getrlimit(syscall.RLIMIT_NOFILE, &inherited) == nil && inherited.Max != rlimInfinity && openFiles > inherited.Max {
			openFiles = inherited.Max
		}
		setLimit(syscall.RLIMIT_NOFILE, openFiles)
		setLimit(syscall.RLIMIT_CPU, cpuSeconds)
	}
	err := syscall.Exec(program, argv, os.Environ())
	_, _ = os.Stderr.WriteString("Process failed to start: " + err.Error() + "\n")
	return 127
}

const rlimInfinity = ^uint64(0)

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
	self := "/proc/self/exe"
	if runtime.GOOS != "linux" {
		if self, err = os.Executable(); err != nil {
			return nil, err
		}
	}
	openFiles := limits.MaxOpenFiles
	if openFiles < 32 {
		openFiles = 32
	}
	cpu := limits.MaxCPUSeconds
	if cpu < 10 {
		cpu = 10
	}
	fileBytes := limits.MaxFileBytes
	if fileBytes < 1024*1024 {
		fileBytes = 1024 * 1024
	}
	flag := "0"
	if sandboxed {
		flag = "1"
	}
	full := []string{execSentinel, flag, strconv.FormatInt(AddressSpaceBytes(limits), 10), strconv.FormatInt(fileBytes, 10),
		strconv.Itoa(openFiles), strconv.Itoa(cpu), resolved, program, "--"}
	full = append(full, args...)
	cmd := exec.Command(self, full...)
	cmd.SysProcAttr = &syscall.SysProcAttr{Setsid: true}
	return cmd, nil
}

func signalGroup(pid int, sig syscall.Signal) {
	if pid > 0 {
		_ = syscall.Kill(-pid, sig)
	}
}

func terminateGroup(cmd *exec.Cmd) {
	if cmd == nil || cmd.Process == nil {
		return
	}
	signalGroup(cmd.Process.Pid, syscall.SIGTERM)
	_ = cmd.Process.Signal(syscall.SIGTERM)
}

func killGroup(cmd *exec.Cmd) {
	if cmd == nil || cmd.Process == nil {
		return
	}
	signalGroup(cmd.Process.Pid, syscall.SIGKILL)
	_ = cmd.Process.Kill()
}

func exitStatus(err error) (int, bool) {
	if err == nil {
		return 0, false
	}
	var exit *exec.ExitError
	if errors.As(err, &exit) {
		if status, ok := exit.Sys().(syscall.WaitStatus); ok {
			if status.Signaled() {
				return int(status.Signal()), true
			}
			return status.ExitStatus(), false
		}
		return exit.ExitCode(), false
	}
	return -1, true
}

func configureCapture(cmd *exec.Cmd) {
	cmd.Cancel = func() error {
		if cmd.Process == nil {
			return os.ErrProcessDone
		}
		return cmd.Process.Kill()
	}
}

func isRoot() bool { return os.Geteuid() == 0 }

func currentUID() int { return os.Getuid() }

type fileLock struct {
	file *os.File
}

func tryLock(path string) (*fileLock, bool) {
	file, err := os.OpenFile(path, os.O_RDWR|os.O_CREATE|syscall.O_CLOEXEC, 0o600)
	if err != nil {
		return nil, false
	}
	if syscall.Flock(int(file.Fd()), syscall.LOCK_EX|syscall.LOCK_NB) != nil {
		_ = file.Close()
		return nil, false
	}
	return &fileLock{file: file}, true
}

func (l *fileLock) release() {
	if l != nil && l.file != nil {
		_ = l.file.Close()
		l.file = nil
	}
}

func lockHeldElsewhere(path string) bool {
	file, err := os.OpenFile(path, os.O_RDWR|syscall.O_CLOEXEC, 0)
	if err != nil {
		return false
	}
	defer file.Close()
	if err := syscall.Flock(int(file.Fd()), syscall.LOCK_EX|syscall.LOCK_NB); err != nil {
		return errors.Is(err, syscall.EWOULDBLOCK)
	}
	_ = syscall.Flock(int(file.Fd()), syscall.LOCK_UN)
	return false
}

func availableBytes(path string) int64 {
	var stat syscall.Statfs_t
	if syscall.Statfs(path, &stat) != nil {
		return -1
	}
	return int64(stat.Bavail) * int64(stat.Bsize)
}
