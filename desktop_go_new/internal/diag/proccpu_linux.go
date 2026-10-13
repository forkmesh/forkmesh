//go:build linux

package diag

import (
	"bytes"
	"strconv"
)

const clockTicks = 100

func processCPUSeconds(pid int) (float64, bool) {
	if pid <= 0 {
		return 0, false
	}
	data := readSmall("/proc/" + strconv.Itoa(pid) + "/stat")
	rp := bytes.LastIndexByte(data, ')')
	if rp < 0 || rp+2 > len(data) {
		return 0, false
	}
	fields := bytes.Split(data[rp+2:], []byte{' '})
	if len(fields) <= 12 {
		return 0, false
	}
	utime, errU := strconv.ParseUint(string(fields[11]), 10, 64)
	stime, errS := strconv.ParseUint(string(fields[12]), 10, 64)
	if errU != nil || errS != nil {
		return 0, false
	}
	return float64(utime+stime) / clockTicks, true
}
