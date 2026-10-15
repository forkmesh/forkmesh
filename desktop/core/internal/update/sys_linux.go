//go:build linux

package update

import (
	"bufio"
	"os"
	"strconv"
	"strings"
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
	file, err := os.Open("/proc/meminfo")
	if err != nil {
		return 0, 0
	}
	defer file.Close()
	var total, available int64
	scanner := bufio.NewScanner(file)
	for scanner.Scan() {
		fields := strings.Fields(scanner.Text())
		if len(fields) < 2 {
			continue
		}
		value, err := strconv.ParseInt(fields[1], 10, 64)
		if err != nil {
			continue
		}
		switch fields[0] {
		case "MemTotal:":
			total = value * 1024
		case "MemAvailable:":
			available = value * 1024
		}
	}
	return total, available
}
