//go:build !windows

package actions

import "os/exec"

func hideWindow(cmd *exec.Cmd) {}
