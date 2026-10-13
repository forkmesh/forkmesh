//go:build windows

package actions

import "os"

func inspectOwnership(path, parent string, inspection summaryInspection) summaryInspection {
	if info, err := os.Stat(path); err == nil && (!info.Mode().IsRegular() || info.Mode().Perm()&0o077 != 0) {
		return inspection
	}
	inspection.policy = summarySameAccount
	return inspection
}

func chownRootGroup(file *os.File, gid uint64) bool { return false }

func processAlive(pid int) bool {
	process, err := os.FindProcess(pid)
	return err == nil && process != nil
}

func ownedByEffectiveUser(info os.FileInfo) bool { return true }
