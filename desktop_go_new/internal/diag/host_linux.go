//go:build linux

package diag

import (
	"os"
	"runtime"
	"strconv"
	"syscall"
)

func readSmall(path string) []byte {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil
	}
	return data
}

func readHost(dataDir string) reading {
	r := reading{CPUCount: runtime.NumCPU()}
	meminfo := readSmall("/proc/meminfo")
	r.MemoryTotal, _ = meminfoField(meminfo, "MemTotal")
	if avail, ok := meminfoField(meminfo, "MemAvailable"); ok {
		r.MemoryAvailable = avail
	} else {
		r.MemoryAvailable, _ = meminfoField(meminfo, "MemFree")
	}
	r.SwapTotal, _ = meminfoField(meminfo, "SwapTotal")
	r.SwapFree, _ = meminfoField(meminfo, "SwapFree")
	r.CPUBusy, r.CPUTotal, r.CPUValid = parseCPULine(readSmall("/proc/stat"))
	r.DiskTotal, r.DiskFree = diskSpace(dataDir)
	p := pressure{Load1: -1}
	p.Valid = parsePressureStall(readSmall("/proc/pressure/memory"), &p.MemorySome, &p.MemoryFull, &p.MemoryFull60)
	parsePressureStall(readSmall("/proc/pressure/io"), &p.IOSome, &p.IOFull, nil)
	parsePressureStall(readSmall("/proc/pressure/cpu"), &p.CPUSome, nil, nil)
	if load, ok := parseLoadAverage(readSmall("/proc/loadavg")); ok {
		p.Load1 = load
	}
	r.Pressure = p
	return r
}

func diskSpace(path string) (int64, int64) {
	if path == "" {
		return 0, 0
	}
	var vfs syscall.Statfs_t
	if err := syscall.Statfs(path, &vfs); err != nil {
		return 0, 0
	}
	size := int64(vfs.Frsize)
	if size <= 0 {
		size = int64(vfs.Bsize)
	}
	return int64(vfs.Blocks) * size, int64(vfs.Bavail) * size
}

func countEntries(path string) int {
	dir, err := os.Open(path)
	if err != nil {
		return -1
	}
	defer dir.Close()
	names, err := dir.Readdirnames(-1)
	if err != nil {
		return -1
	}
	return len(names)
}

func readProcess(pid int) processStats {
	out := unknownProcess()
	if pid <= 0 {
		return out
	}
	base := "/proc/" + strconv.Itoa(pid)
	status := readSmall(base + "/status")
	if status == nil {
		return out
	}
	out.Resident = kilobyteField(status, "VmRSS")
	out.Swapped = kilobyteField(status, "VmSwap")
	if threads := countEntries(base + "/task"); threads > 0 {
		out.Threads = threads
	}
	out.OpenFiles = countEntries(base + "/fd")
	out.OpenFileLimit = parseOpenFileLimit(readSmall(base + "/limits"))
	return out
}
