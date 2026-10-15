//go:build darwin

package update

import (
	"encoding/binary"
	"syscall"
)

func volumeFreeBytes(path string) int64 {
	var stat syscall.Statfs_t
	if syscall.Statfs(path, &stat) != nil {
		return -1
	}
	return int64(stat.Bavail) * int64(stat.Bsize)
}

func memoryBytes() (int64, int64) {
	raw, err := syscall.Sysctl("hw.memsize")
	if err != nil {
		return 0, 0
	}
	data := []byte(raw)
	for len(data) < 8 {
		data = append(data, 0)
	}
	return int64(binary.LittleEndian.Uint64(data[:8])), 0
}
