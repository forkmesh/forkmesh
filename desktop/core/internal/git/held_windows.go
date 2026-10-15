//go:build windows

package git

func lockHeld(lockPath string) bool {
	return false
}
