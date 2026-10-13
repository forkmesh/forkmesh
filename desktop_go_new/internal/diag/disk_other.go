//go:build !linux && !darwin && !windows

package diag

func diskSpace(path string) (int64, int64) {
	return 0, 0
}
