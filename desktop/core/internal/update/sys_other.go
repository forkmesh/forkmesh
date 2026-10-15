//go:build !linux && !darwin

package update

func volumeFreeBytes(path string) int64 { return -1 }

func memoryBytes() (int64, int64) { return 0, 0 }
