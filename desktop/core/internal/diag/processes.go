package diag

import (
	"bytes"
	"context"
	"errors"
	"os/exec"
	"strconv"
	"strings"
	"time"

	"forkmesh.com/desktop-core/internal/rpc"
)

const (
	maxProcessRows        = 30
	processQueryTimeout   = 4 * time.Second
	commandSummaryMaxChar = 160
)

var errPSUnavailable = errors.New("ps_unavailable")

type processRow struct {
	Pid     int64
	Ppid    int64
	User    string
	RssKb   int64
	Percent float64
	Name    string
	Command string
}

func asciiSpace(r rune) bool {
	return r == ' ' || r == '\t' || r == '\n' || r == '\v' || r == '\f' || r == '\r'
}

func qtLongLong(field []byte) (int64, bool) {
	n, err := strconv.ParseInt(string(field), 10, 64)
	return n, err == nil
}

func qtDouble(field string) float64 {
	v, err := strconv.ParseFloat(strings.TrimSpace(field), 64)
	if err != nil {
		return 0
	}
	return v
}

func parseProcessTable(output []byte, maxRows int, parents map[int64]int64) []processRow {
	var rows []processRow
	for _, raw := range bytes.Split(output, []byte{'\n'}) {
		fields := bytes.FieldsFunc(raw, asciiSpace)
		if len(fields) < 6 {
			continue
		}
		var row processRow
		pid, pidOK := qtLongLong(fields[0])
		row.Pid = pid
		row.Ppid, _ = qtLongLong(fields[1])
		row.User = left(fromUTF8(fields[2]), 32)
		rss, rssOK := qtLongLong(fields[3])
		row.RssKb = rss
		row.Percent = qtDouble(fromUTF8(fields[4]))
		row.Command = fromUTF8(bytes.Join(fields[5:], []byte{' '}))
		if !pidOK || !rssOK || row.Pid <= 0 || row.Command == "" {
			continue
		}
		if parents != nil && row.Ppid > 0 {
			parents[row.Pid] = row.Ppid
		}
		name := row.Command
		if i := strings.IndexByte(name, ' '); i >= 0 {
			name = name[:i]
		}
		if slash := strings.LastIndexByte(name, '/'); slash >= 0 {
			name = name[slash+1:]
		}
		if name == "" {
			name = row.Command
		}
		row.Name = left(name, 80)
		if len(rows) < maxRows {
			rows = append(rows, row)
		}
	}
	return rows
}

func commandSummary(command string, maxChars int) string {
	var kept []string
	for _, part := range strings.Split(command, " ") {
		if part == "" {
			continue
		}
		lower := strings.ToLower(part)
		if strings.Contains(lower, "token") || strings.Contains(lower, "key=") ||
			strings.Contains(lower, "password") || strings.Contains(lower, "secret") {
			kept = append(kept, "[redacted]")
			continue
		}
		kept = append(kept, part)
	}
	text := redactSecrets(strings.Join(kept, " "))
	if utf16Len(text) > maxChars {
		text = left(text, maxChars-1) + "…"
	}
	return text
}

func runPS(ctx context.Context) ([]byte, error) {
	ctx, cancel := context.WithTimeout(ctx, processQueryTimeout)
	defer cancel()
	cmd := exec.CommandContext(ctx, "ps", "-eo", "pid=,ppid=,user=,rss=,%mem=,args=", "--sort=-rss")
	var stdout bytes.Buffer
	cmd.Stdout = &stdout
	if err := cmd.Start(); err != nil {
		return nil, errPSUnavailable
	}
	err := cmd.Wait()
	if ctx.Err() == context.DeadlineExceeded {
		return nil, rpc.Fail("timeout")
	}
	if err != nil {
		return nil, rpc.Fail("ps_failed")
	}
	return stdout.Bytes(), nil
}

func (m *Module) handleProcesses(ctx context.Context, req *rpc.Request) (any, error) {
	run := m.RunPS
	if run == nil {
		run = runPS
	}
	output, err := run(ctx)
	if errors.Is(err, errPSUnavailable) {
		return nil, rpc.Fail("ps_unavailable")
	}
	if err != nil {
		return nil, err
	}
	parents := map[int64]int64{}
	rows := parseProcessTable(output, maxProcessRows, parents)
	state := m.stateForPhone(ctx, true)
	roots := map[int64]agentRoot{}
	for _, root := range state.Roots {
		if root.Pid > 0 {
			roots[root.Pid] = root
		}
	}
	out := make([]any, 0, len(rows))
	for _, row := range rows {
		item := map[string]any{
			"pid":         row.Pid,
			"name":        row.Name,
			"description": processDescription(row.Name),
			"user":        row.User,
			"rssBytes":    float64(row.RssKb) * 1024.0,
			"percent":     row.Percent,
			"command":     commandSummary(row.Command, commandSummaryMaxChar),
		}
		pid := row.Pid
		for hops := 0; pid > 1 && hops < 64; hops++ {
			if found, ok := roots[pid]; ok {
				item["agent"] = map[string]any{"session": found.Session, "label": found.Label,
					"stoppable": found.Stoppable}
				break
			}
			parent := parents[pid]
			if parent <= 0 || parent == pid {
				break
			}
			pid = parent
		}
		out = append(out, item)
	}
	m.mu.Lock()
	total := m.latest.MemoryTotal
	m.mu.Unlock()
	return map[string]any{"processes": out, "memoryTotal": total}, nil
}
