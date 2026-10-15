//go:build !windows

package stats

import (
	"os"
	"syscall"
)

func readableFile(path string) bool { return syscall.Access(path, 4) == nil }

func executableFile(path string, info os.FileInfo) bool { return syscall.Access(path, 1) == nil }
