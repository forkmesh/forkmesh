package mirrornode

import "syscall"

func systemMemoryUsage() (uint64, uint64) {
	var info syscall.Sysinfo_t
	if syscall.Sysinfo(&info) != nil {
		return 0, 0
	}
	total := info.Totalram * uint64(info.Unit)
	free := (info.Freeram + info.Bufferram) * uint64(info.Unit)
	return total - free, total
}
