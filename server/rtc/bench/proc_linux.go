package bench

import (
	"os"
	"os/exec"
	"strconv"
	"strings"
	"syscall"
)

const clockTicks = 100

func prepareGroup(command *exec.Cmd) {
	command.SysProcAttr = &syscall.SysProcAttr{Setpgid: true}
}

func killGroup(command *exec.Cmd) {
	if command.Process == nil {
		return
	}
	_ = syscall.Kill(-command.Process.Pid, syscall.SIGKILL)
	_ = command.Wait()
}

type procStat struct {
	ppid  int
	pgrp  int
	ticks float64
}

func readProcStat(pid string) (procStat, bool) {
	data, err := os.ReadFile("/proc/" + pid + "/stat")
	if err != nil {
		return procStat{}, false
	}
	text := string(data)
	close := strings.LastIndexByte(text, ')')
	if close < 0 {
		return procStat{}, false
	}
	fields := strings.Fields(text[close+1:])
	if len(fields) < 15 {
		return procStat{}, false
	}
	ppid, _ := strconv.Atoi(fields[1])
	pgrp, _ := strconv.Atoi(fields[2])
	var ticks float64
	for _, index := range []int{11, 12, 13, 14} {
		value, _ := strconv.ParseFloat(fields[index], 64)
		ticks += value
	}
	return procStat{ppid: ppid, pgrp: pgrp, ticks: ticks}, true
}

func cpuSeconds(root int) float64 {
	entries, err := os.ReadDir("/proc")
	if err != nil {
		return 0
	}
	procs := map[int]procStat{}
	for _, entry := range entries {
		pid, err := strconv.Atoi(entry.Name())
		if err != nil {
			continue
		}
		if stat, ok := readProcStat(entry.Name()); ok {
			procs[pid] = stat
		}
	}
	inTree := func(pid int) bool {
		for depth := 0; depth < 64 && pid > 1; depth++ {
			if pid == root {
				return true
			}
			stat, ok := procs[pid]
			if !ok {
				return false
			}
			pid = stat.ppid
		}
		return false
	}
	total := 0.0
	for pid, stat := range procs {
		if stat.pgrp == root || inTree(pid) {
			total += stat.ticks
		}
	}
	return total / clockTicks
}

func selfCPUSeconds() float64 {
	var usage syscall.Rusage
	if err := syscall.Getrusage(syscall.RUSAGE_SELF, &usage); err != nil {
		return 0
	}
	return float64(usage.Utime.Nano()+usage.Stime.Nano()) / 1e9
}
