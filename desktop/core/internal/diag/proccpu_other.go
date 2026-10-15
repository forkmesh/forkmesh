//go:build !linux

package diag

func processCPUSeconds(pid int) (float64, bool) {
	return 0, false
}
