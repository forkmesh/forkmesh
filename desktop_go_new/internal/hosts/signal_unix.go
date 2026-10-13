//go:build !windows

package hosts

import (
	"os"
	"syscall"
)

func signalNumber(state *os.ProcessState) int {
	if status, ok := state.Sys().(syscall.WaitStatus); ok && status.Signaled() {
		return int(status.Signal())
	}
	return -1
}
