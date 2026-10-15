//go:build !windows

package vault

import "os"

func isWindows() bool { return false }

func syncDir(dir string) {
	handle, err := os.Open(dir)
	if err != nil {
		return
	}
	_ = handle.Sync()
	_ = handle.Close()
}
