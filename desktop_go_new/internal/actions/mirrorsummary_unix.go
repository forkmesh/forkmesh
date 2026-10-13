//go:build !windows

package actions

import (
	"fmt"
	"os"
	"syscall"
)

func inspectOwnership(path, parent string, inspection summaryInspection) summaryInspection {
	var parentState syscall.Stat_t
	if syscall.Lstat(parent, &parentState) != nil {
		return inspection
	}
	var targetState syscall.Stat_t
	err := syscall.Lstat(path, &targetState)
	targetExists := err == nil
	if !targetExists && err != syscall.ENOENT {
		return inspection
	}
	mode := func(raw uint32) uint32 { return raw & 0o7777 }
	isDir := parentState.Mode&syscall.S_IFMT == syscall.S_IFDIR
	isRegular := targetExists && targetState.Mode&syscall.S_IFMT == syscall.S_IFREG
	isLink := targetExists && targetState.Mode&syscall.S_IFMT == syscall.S_IFLNK
	var targetUID, targetGID uint64
	var targetMode uint32
	var links uint64
	if targetExists {
		targetUID, targetGID = uint64(targetState.Uid), uint64(targetState.Gid)
		targetMode = mode(uint32(targetState.Mode))
		links = uint64(targetState.Nlink)
	}
	inspection.policy = ClassifySummaryWritePolicy(path, uint64(os.Geteuid()), uint64(parentState.Uid), uint64(parentState.Gid),
		mode(uint32(parentState.Mode)), isDir, targetExists, isRegular, isLink, targetUID, targetGID, targetMode, links)
	if inspection.policy != summaryReject {
		inspection.parentGID = uint64(parentState.Gid)
		inspection.parentIdent = fmt.Sprintf("%d:%d:%d", uint64(parentState.Dev), uint64(parentState.Ino), uint64(parentState.Gid))
	}
	return inspection
}

func chownRootGroup(file *os.File, gid uint64) bool {
	return os.Geteuid() == 0 && file.Chown(0, int(gid)) == nil
}

func processAlive(pid int) bool {
	if pid <= 0 {
		return false
	}
	err := syscall.Kill(pid, 0)
	return err == nil || err == syscall.EPERM
}

func ownedByEffectiveUser(info os.FileInfo) bool {
	stat, ok := info.Sys().(*syscall.Stat_t)
	return ok && int(stat.Uid) == os.Geteuid()
}
