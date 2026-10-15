//go:build windows

package vault

func isWindows() bool { return true }

func syncDir(dir string) {}
