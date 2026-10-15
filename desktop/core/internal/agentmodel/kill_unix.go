//go:build !windows

package agentmodel

import (
	"syscall"
	"time"
)

func killStragglers(pids []int64) {
	for _, pid := range pids {
		_ = syscall.Kill(int(pid), syscall.SIGTERM)
	}
	time.AfterFunc(4*time.Second, func() {
		for _, pid := range pids {
			if syscall.Kill(int(pid), 0) == nil {
				_ = syscall.Kill(int(pid), syscall.SIGKILL)
			}
		}
	})
}
