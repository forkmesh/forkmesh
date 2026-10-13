package mirrornode

import (
	"os"
	"syscall"
)

func volumeUsageFor(path string) (volumeUsage, error) {
	var stat syscall.Statfs_t
	if err := syscall.Statfs(path, &stat); err != nil {
		return volumeUsage{}, err
	}
	blockSize := uint64(stat.Bsize)
	return volumeUsage{
		FreeBytes:   uint64(stat.Bavail) * blockSize,
		TotalBytes:  uint64(stat.Blocks) * blockSize,
		FreeInodes:  uint64(stat.Ffree),
		TotalInodes: uint64(stat.Files),
	}, nil
}

// alignOwner gives path the owner of reference when running as root, so an
// operation run by root leaves files the service account can still read.
func alignOwner(path, reference string) error {
	if os.Geteuid() != 0 {
		return nil
	}
	info, err := os.Stat(reference)
	if err != nil {
		return err
	}
	stat, ok := info.Sys().(*syscall.Stat_t)
	if !ok {
		return nil
	}
	return os.Lchown(path, int(stat.Uid), int(stat.Gid))
}
