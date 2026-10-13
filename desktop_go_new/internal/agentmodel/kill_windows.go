//go:build windows

package agentmodel

import "os"

func killStragglers(pids []int64) {
	for _, pid := range pids {
		if pid <= 0 || pid > 0xFFFFFFFF {
			continue
		}
		if process, err := os.FindProcess(int(pid)); err == nil {
			_ = process.Kill()
			_ = process.Release()
		}
	}
}
