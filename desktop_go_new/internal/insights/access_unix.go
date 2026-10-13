//go:build !windows

package insights

import (
	"os"
	"syscall"
)

func executableFile(path string, info os.FileInfo) bool { return syscall.Access(path, 1) == nil }

func terminate(process *os.Process) { _ = process.Signal(syscall.SIGTERM) }
