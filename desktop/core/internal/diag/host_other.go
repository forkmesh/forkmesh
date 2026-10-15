//go:build !linux && !darwin && !windows

package diag

import "runtime"

func readHost(dataDir string) reading {
	return reading{CPUCount: runtime.NumCPU(), Pressure: pressure{Load1: -1}}
}

func readProcess(pid int) processStats {
	return unknownProcess()
}
