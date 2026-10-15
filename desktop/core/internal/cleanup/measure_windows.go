//go:build windows

package cleanup

import (
	"os"
	"path/filepath"
	"sync/atomic"
)

func Measure(path string, cancel *atomic.Bool) Usage {
	var usage Usage
	root, err := os.Lstat(path)
	if err != nil {
		return usage
	}
	usage.NewestMs = root.ModTime().UnixMilli()
	if root.Mode()&os.ModeSymlink != 0 || !root.IsDir() {
		usage.Bytes = root.Size()
		usage.Files = 1
		return usage
	}
	walk(path, &usage, cancel)
	return usage
}

func walk(dir string, usage *Usage, cancel *atomic.Bool) {
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
		if ms := info.ModTime().UnixMilli(); ms > usage.NewestMs {
			usage.NewestMs = ms
		}
		if info.IsDir() {
			walk(child, usage, cancel)
			continue
		}
		usage.Bytes += info.Size()
		usage.Files++
	}
}

func ownedByUs(path, uid string) bool { return true }

func currentUID() string { return "" }
