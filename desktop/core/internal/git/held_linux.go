//go:build linux

package git

import (
	"os"
	"path/filepath"
	"strconv"
)

func lockHeld(lockPath string) bool {
	lock := canonicalOrClean(lockPath)
	gitDir := canonicalOrClean(filepath.Dir(lock))
	entries, err := os.ReadDir("/proc")
	if err != nil {
		return false
	}
	for _, entry := range entries {
		if !entry.IsDir() {
			continue
		}
		if _, err := strconv.ParseInt(entry.Name(), 10, 64); err != nil {
			continue
		}
		base := filepath.Join("/proc", entry.Name())
		if readFirstLine(filepath.Join(base, "comm")) == "git" {
			if cwd, err := os.Readlink(filepath.Join(base, "cwd")); err == nil && cwd != "" {
				if pathInside(cwd, gitDir) || canonicalOrClean(GitDirForCheckout(cwd)) == gitDir {
					return true
				}
			}
		}
		fds, err := os.ReadDir(filepath.Join(base, "fd"))
		if err != nil {
			continue
		}
		for _, fd := range fds {
			if target, err := os.Readlink(filepath.Join(base, "fd", fd.Name())); err == nil && target == lock {
				return true
			}
		}
	}
	return false
}
