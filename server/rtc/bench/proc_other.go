//go:build !linux

package bench

import "os/exec"

func prepareGroup(command *exec.Cmd) {}

func killGroup(command *exec.Cmd) {
	if command.Process == nil {
		return
	}
	_ = command.Process.Kill()
	_ = command.Wait()
}

func cpuSeconds(root int) float64 {
	return 0
}

func selfCPUSeconds() float64 {
	return 0
}
