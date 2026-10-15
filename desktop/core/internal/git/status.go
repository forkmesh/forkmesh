package git

import (
	"bytes"
	"context"
	"strconv"
	"strings"
)

type Status struct {
	Branch       string `json:"branch"`
	Detached     bool   `json:"detached"`
	Head         string `json:"head"`
	Unborn       bool   `json:"unborn"`
	Upstream     string `json:"upstream"`
	UpstreamGone bool   `json:"upstreamGone"`
	Ahead        int    `json:"ahead"`
	Behind       int    `json:"behind"`
	Staged       int    `json:"staged"`
	Unstaged     int    `json:"unstaged"`
	Untracked    int    `json:"untracked"`
	Conflicted   int    `json:"conflicted"`
	Clean        bool   `json:"clean"`
}

func StatusArgs() []string {
	return []string{"status", "--porcelain=v2", "--branch", "-z", "--untracked-files=normal"}
}

func ParseStatus(output []byte) Status {
	var status Status
	hasAB := false
	records := bytes.Split(output, []byte{0})
	for i := 0; i < len(records); i++ {
		record := string(records[i])
		if record == "" {
			continue
		}
		switch {
		case strings.HasPrefix(record, "# branch.oid "):
			oid := strings.TrimPrefix(record, "# branch.oid ")
			if oid == "(initial)" {
				status.Unborn = true
			} else {
				status.Head = oid
			}
		case strings.HasPrefix(record, "# branch.head "):
			head := strings.TrimPrefix(record, "# branch.head ")
			if head == "(detached)" {
				status.Detached = true
			} else {
				status.Branch = head
			}
		case strings.HasPrefix(record, "# branch.upstream "):
			status.Upstream = strings.TrimPrefix(record, "# branch.upstream ")
		case strings.HasPrefix(record, "# branch.ab "):
			hasAB = true
			for _, field := range strings.Fields(strings.TrimPrefix(record, "# branch.ab ")) {
				n, _ := strconv.Atoi(strings.TrimLeft(field, "+-"))
				if strings.HasPrefix(field, "+") {
					status.Ahead = n
				} else if strings.HasPrefix(field, "-") {
					status.Behind = n
				}
			}
		case strings.HasPrefix(record, "1 ") || strings.HasPrefix(record, "2 "):
			if len(record) >= 4 {
				if record[2] != '.' {
					status.Staged++
				}
				if record[3] != '.' {
					status.Unstaged++
				}
			}
			if record[0] == '2' {
				i++
			}
		case strings.HasPrefix(record, "u "):
			status.Conflicted++
		case strings.HasPrefix(record, "? "):
			status.Untracked++
		}
	}
	status.UpstreamGone = status.Upstream != "" && !hasAB
	status.Clean = status.Staged == 0 && status.Unstaged == 0 && status.Untracked == 0 && status.Conflicted == 0
	return status
}

func (r *Runner) Status(ctx context.Context, dir string) (Status, Result) {
	result := r.Run(ctx, dir, StatusArgs(), Options{})
	if !result.OK {
		return Status{}, result
	}
	return ParseStatus(result.Output), result
}
