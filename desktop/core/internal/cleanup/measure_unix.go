//go:build !windows

package cleanup

import (
	"os"
	"path/filepath"
	"strconv"
	"sync/atomic"
	"syscall"
)

type inode struct {
	dev uint64
	ino uint64
}

func Measure(path string, cancel *atomic.Bool) Usage {
	var usage Usage
	info, err := os.Lstat(path)
	if err != nil {
		return usage
	}
	stat, ok := info.Sys().(*syscall.Stat_t)
	if !ok {
		return usage
	}
	usage.Bytes = int64(stat.Blocks) * 512
	usage.NewestMs = info.ModTime().Unix() * 1000
	if !info.IsDir() {
		usage.Files = 1
		return usage
	}
	walk(path, &usage, map[inode]bool{}, cancel, 0)
	return usage
}

func walk(dir string, usage *Usage, seen map[inode]bool, cancel *atomic.Bool, depth int) {
	names, err := readNames(dir)
	if err != nil {
		return
	}
	for _, name := range names {
		if cancelled(cancel) {
			return
		}
		child := filepath.Join(dir, name)
		info, err := os.Lstat(child)
		if err != nil {
			continue
		}
		stat, ok := info.Sys().(*syscall.Stat_t)
		if !ok {
			continue
		}
		isDir := info.IsDir()
		if !isDir && uint64(stat.Nlink) > 1 {
			key := inode{uint64(stat.Dev), uint64(stat.Ino)}
			if seen[key] {
				continue
			}
			seen[key] = true
		}
		usage.Bytes += int64(stat.Blocks) * 512
		if !isDir {
			usage.Files++
		}
		if ms := info.ModTime().Unix() * 1000; ms > usage.NewestMs {
			usage.NewestMs = ms
		}
		if isDir && depth < 200 {
			walk(child, usage, seen, cancel, depth+1)
		}
	}
}

func ownedByUs(path, uid string) bool {
	if uid == "" {
		return true
	}
	info, err := os.Stat(path)
	if err != nil {
		return false
	}
	stat, ok := info.Sys().(*syscall.Stat_t)
	return ok && strconv.FormatUint(uint64(stat.Uid), 10) == uid
}

func currentUID() string { return strconv.Itoa(os.Getuid()) }
