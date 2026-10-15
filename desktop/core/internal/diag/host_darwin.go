//go:build darwin

package diag

import (
	"encoding/binary"
	"runtime"
	"syscall"
)

func sysctlBytes(name string, size int) ([]byte, bool) {
	raw, err := syscall.Sysctl(name)
	if err != nil {
		return nil, false
	}
	data := []byte(raw)
	for len(data) < size {
		data = append(data, 0)
	}
	return data, true
}

func sysctlUint64(name string) (uint64, bool) {
	data, ok := sysctlBytes(name, 8)
	if !ok {
		return 0, false
	}
	return binary.LittleEndian.Uint64(data[:8]), true
}

func readHost(dataDir string) reading {
	r := reading{CPUCount: runtime.NumCPU(), Pressure: pressure{Load1: -1}}
	if total, ok := sysctlUint64("hw.memsize"); ok {
		r.MemoryTotal = int64(total)
	}
	page, errPage := syscall.SysctlUint32("hw.pagesize")
	free, errFree := syscall.SysctlUint32("vm.page_free_count")
	if errPage == nil && errFree == nil {
		available := uint64(free)
		if speculative, err := syscall.SysctlUint32("vm.page_speculative_count"); err == nil {
			available += uint64(speculative)
		}
		r.MemoryAvailable = int64(available * uint64(page))
	}
	if swap, ok := sysctlBytes("vm.swapusage", 24); ok {
		r.SwapTotal = int64(binary.LittleEndian.Uint64(swap[0:8]))
		r.SwapFree = int64(binary.LittleEndian.Uint64(swap[8:16]))
	}
	if load, ok := sysctlBytes("vm.loadavg", 24); ok {
		scale := binary.LittleEndian.Uint64(load[16:24])
		if scale > 0 {
			r.Pressure.Load1 = float64(binary.LittleEndian.Uint32(load[0:4])) / float64(scale)
		}
	}
	r.DiskTotal, r.DiskFree = diskSpace(dataDir)
	return r
}

func diskSpace(path string) (int64, int64) {
	if path == "" {
		return 0, 0
	}
	var fs syscall.Statfs_t
	if err := syscall.Statfs(path, &fs); err != nil {
		return 0, 0
	}
	size := int64(fs.Bsize)
	return int64(fs.Blocks) * size, int64(fs.Bavail) * size
}

func readProcess(pid int) processStats {
	return unknownProcess()
}
