//go:build windows

package stats

import "os"

func readableFile(path string) bool {
	file, err := os.Open(path)
	if err != nil {
		return false
	}
	_ = file.Close()
	return true
}

func executableFile(path string, info os.FileInfo) bool { return false }
