//go:build linux

package agentmodel

import (
	"bytes"
	"os"
	"strconv"
)

func readProcessTable() processTable {
	table := processTable{entries: map[int64]processEntry{}, children: map[int64][]int64{}}
	dir, err := os.Open("/proc")
	if err != nil {
		return table
	}
	names, _ := dir.Readdirnames(-1)
	dir.Close()
	for _, name := range names {
		if name == "" || name[0] < '0' || name[0] > '9' {
			continue
		}
		pid, err := strconv.ParseInt(name, 10, 64)
		if err != nil || pid <= 0 {
			continue
		}
		data, err := os.ReadFile("/proc/" + name + "/stat")
		if err != nil {
			continue
		}
		lp := bytes.IndexByte(data, '(')
		rp := bytes.LastIndexByte(data, ')')
		if lp < 0 || rp <= lp || rp+2 > len(data) {
			continue
		}
		fields := bytes.Split(data[rp+2:], []byte{' '})
		if len(fields) < 2 || len(fields[0]) == 0 {
			continue
		}
		parent, err := strconv.ParseInt(string(fields[1]), 10, 64)
		if err != nil {
			continue
		}
		ticks := int64(0)
		if len(fields) > 12 {
			utime, _ := strconv.ParseInt(string(fields[11]), 10, 64)
			stime, _ := strconv.ParseInt(string(fields[12]), 10, 64)
			ticks = utime + stime
		}
		table.entries[pid] = processEntry{parent: parent, command: string(data[lp+1 : rp]),
			zombie: fields[0][0] == 'Z', cpuTicks: ticks}
		table.children[parent] = append(table.children[parent], pid)
	}
	return table
}
