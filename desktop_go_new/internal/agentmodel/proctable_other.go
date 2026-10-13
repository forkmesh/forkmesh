//go:build !linux

package agentmodel

func readProcessTable() processTable {
	return processTable{entries: map[int64]processEntry{}, children: map[int64][]int64{}}
}
