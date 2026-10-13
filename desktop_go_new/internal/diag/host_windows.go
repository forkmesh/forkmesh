//go:build windows

package diag

import (
	"runtime"
	"syscall"
	"unsafe"
)

var (
	kernel32             = syscall.NewLazyDLL("kernel32.dll")
	globalMemoryStatusEx = kernel32.NewProc("GlobalMemoryStatusEx")
	getDiskFreeSpaceExW  = kernel32.NewProc("GetDiskFreeSpaceExW")
	getSystemTimes       = kernel32.NewProc("GetSystemTimes")
)

type memoryStatusEx struct {
	Length               uint32
	MemoryLoad           uint32
	TotalPhys            uint64
	AvailPhys            uint64
	TotalPageFile        uint64
	AvailPageFile        uint64
	TotalVirtual         uint64
	AvailVirtual         uint64
	AvailExtendedVirtual uint64
}

func fileTime(ft syscall.Filetime) float64 {
	return float64(uint64(ft.HighDateTime)<<32 | uint64(ft.LowDateTime))
}

func readHost(dataDir string) reading {
	r := reading{CPUCount: runtime.NumCPU(), Pressure: pressure{Load1: -1}}
	status := memoryStatusEx{}
	status.Length = uint32(unsafe.Sizeof(status))
	if ok, _, _ := globalMemoryStatusEx.Call(uintptr(unsafe.Pointer(&status))); ok != 0 {
		r.MemoryTotal = int64(status.TotalPhys)
		r.MemoryAvailable = int64(status.AvailPhys)
	}
	var idle, kernel, user syscall.Filetime
	if ok, _, _ := getSystemTimes.Call(uintptr(unsafe.Pointer(&idle)), uintptr(unsafe.Pointer(&kernel)), uintptr(unsafe.Pointer(&user))); ok != 0 {
		total := fileTime(kernel) + fileTime(user)
		r.CPUTotal = total
		r.CPUBusy = total - fileTime(idle)
		r.CPUValid = total > 0
	}
	r.DiskTotal, r.DiskFree = diskSpace(dataDir)
	return r
}

func diskSpace(path string) (int64, int64) {
	if path == "" {
		return 0, 0
	}
	name, err := syscall.UTF16PtrFromString(path)
	if err != nil {
		return 0, 0
	}
	var available, total, free uint64
	ok, _, _ := getDiskFreeSpaceExW.Call(uintptr(unsafe.Pointer(name)), uintptr(unsafe.Pointer(&available)),
		uintptr(unsafe.Pointer(&total)), uintptr(unsafe.Pointer(&free)))
	if ok == 0 {
		return 0, 0
	}
	return int64(total), int64(available)
}

func readProcess(pid int) processStats {
	return unknownProcess()
}
