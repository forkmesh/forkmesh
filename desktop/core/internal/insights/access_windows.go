//go:build windows

package insights

import "os"

func executableFile(path string, info os.FileInfo) bool { return false }

func terminate(process *os.Process) { _ = process.Kill() }
