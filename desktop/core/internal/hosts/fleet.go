package hosts

import (
	"fmt"
	"regexp"
	"sort"
	"strconv"
	"strings"
)

const (
	gatewayRetiredAfterMs = 24 * 60 * 60 * 1000
	gatewayMaxRows        = 50
	provisionLogLines     = 80
	provisionLogTailBytes = 48 * 1024
	maxFleetRepos         = 300
)

var provisionStageNames = []string{"Credentials", "Plan + image", "Create server", "Boot + connect", "Install", "Live traffic"}

type costBucket struct {
	key   string
	count int
	cost  float64
}

func costSummary(hosts []map[string]any) map[string]any {
	vultr := 0
	monthly := 0.0
	byRegion := map[string]*costBucket{}
	byPlan := map[string]*costBucket{}
	add := func(table map[string]*costBucket, key string, cost float64) {
		bucket := table[key]
		if bucket == nil {
			bucket = &costBucket{key: key}
			table[key] = bucket
		}
		bucket.count++
		bucket.cost += cost
	}
	for _, host := range hosts {
		if !strings.EqualFold(jsonString(host["provider"]), "Vultr") {
			continue
		}
		vultr++
		cost := jsonDouble(host["monthlyCost"])
		if cost < 0 {
			cost = 0
		}
		monthly += cost
		region := strings.TrimSpace(jsonString(host["region"]))
		plan := strings.TrimSpace(jsonString(host["planType"]))
		if region == "" {
			region = "unknown"
		}
		if plan == "" {
			plan = "unknown"
		}
		add(byRegion, region, cost)
		add(byPlan, plan, cost)
	}
	rows := func(table map[string]*costBucket, field string) []map[string]any {
		list := make([]*costBucket, 0, len(table))
		for _, bucket := range table {
			list = append(list, bucket)
		}
		sort.Slice(list, func(i, j int) bool { return list[i].key < list[j].key })
		sort.SliceStable(list, func(i, j int) bool { return list[i].cost > list[j].cost })
		if len(list) > 20 {
			list = list[:20]
		}
		out := make([]map[string]any, 0, len(list))
		for _, bucket := range list {
			out = append(out, map[string]any{field: bucket.key, "count": bucket.count, "monthlyCost": bucket.cost})
		}
		return out
	}
	return map[string]any{
		"hosts":       len(hosts),
		"vultr":       vultr,
		"monthlyCost": monthly,
		"byRegion":    rows(byRegion, "region"),
		"byPlan":      rows(byPlan, "plan"),
	}
}

var htmlTagPattern = regexp.MustCompile(`<[^>]*>`)

func plainText(html string, maxChars int) string {
	text := htmlTagPattern.ReplaceAllString(html, " ")
	text = strings.ReplaceAll(text, "&nbsp;", " ")
	text = strings.ReplaceAll(text, "&amp;", "&")
	text = strings.ReplaceAll(text, "&lt;", "<")
	text = strings.ReplaceAll(text, "&gt;", ">")
	text = strings.ReplaceAll(text, "&quot;", "\"")
	text = strings.Join(strings.Fields(text), " ")
	if utf16Len(text) > maxChars {
		text = leftUTF16(text, maxChars-1) + "…"
	}
	return text
}

type provisionState struct {
	Active       bool     `json:"active"`
	Node         string   `json:"node"`
	Stage        int      `json:"stage"`
	State        string   `json:"state"`
	Detail       string   `json:"detail"`
	Message      string   `json:"message"`
	Gap          string   `json:"gap"`
	StageDetails []string `json:"stageDetails"`
	Resumable    bool     `json:"resumable"`
	Attempts     int      `json:"attempts"`
}

func provisionStatus(p provisionState) map[string]any {
	current := p.Stage
	if current < 0 {
		current = 0
	}
	if current > len(provisionStageNames) {
		current = len(provisionStageNames)
	}
	failed := p.State == "failed"
	stages := make([]map[string]any, 0, len(provisionStageNames))
	for i, name := range provisionStageNames {
		number := i + 1
		status := "pending"
		if current > number || (current == number && p.State == "done") {
			status = "done"
		} else if current == number {
			status = "active"
			if failed {
				status = "failed"
			}
		}
		detail := ""
		if i < len(p.StageDetails) {
			detail = p.StageDetails[i]
		}
		stages = append(stages, map[string]any{"number": number, "name": name, "status": status, "detail": plainText(detail, 300)})
	}
	return map[string]any{
		"active":     p.Active,
		"node":       p.Node,
		"stage":      current,
		"stageCount": len(provisionStageNames),
		"state":      p.State,
		"detail":     plainText(p.Detail, 600),
		"message":    plainText(p.Message, 600),
		"gap":        plainText(p.Gap, 300),
		"resumable":  p.Resumable,
		"stages":     stages,
		"attempts":   p.Attempts,
	}
}

var (
	secretAssignmentPattern = regexp.MustCompile(`(?i)((?:api[_-]?key|token|secret|password|passwd|authorization|bearer)["']?\s*[:=]?\s*["']?)([^\s"',;]{6,})`)
	privateKeyPattern       = regexp.MustCompile(`-----BEGIN [A-Z ]*PRIVATE KEY-----.*`)
	tokenPattern            = regexp.MustCompile(`\b(sk-[A-Za-z0-9_-]{12,}|gh[pousr]_[A-Za-z0-9]{20,})\b`)
)

func redactSecrets(line string) string {
	out := secretAssignmentPattern.ReplaceAllString(line, "${1}[redacted]")
	out = privateKeyPattern.ReplaceAllString(out, "[redacted private key]")
	return tokenPattern.ReplaceAllString(out, "[redacted]")
}

func tailLines(tail []byte, maxLines, maxLineChars int) []string {
	lines := []string{}
	for _, line := range strings.Split(strings.ToValidUTF8(string(tail), "�"), "\n") {
		if line != "" {
			lines = append(lines, line)
		}
	}
	if len(lines) > maxLines {
		lines = lines[len(lines)-maxLines:]
	}
	for i, line := range lines {
		line = strings.TrimSuffix(line, "\r")
		lines[i] = redactSecrets(leftUTF16(line, maxLineChars))
	}
	return lines
}

func gatewayRows(records []string, nowMs, retiredAfterMs int64, maxRows int) []map[string]any {
	type row struct {
		host  string
		since int64
	}
	rows := []row{}
	for _, record := range records {
		fields := strings.Split(record, "\t")
		if len(fields) < 2 {
			continue
		}
		host := strings.TrimSpace(fields[0])
		since, _ := strconv.ParseInt(strings.TrimSpace(fields[1]), 10, 64)
		if host == "" || since <= 0 {
			continue
		}
		rows = append(rows, row{host: host, since: since})
	}
	sort.SliceStable(rows, func(i, j int) bool { return rows[i].since > rows[j].since })
	if len(rows) > maxRows {
		rows = rows[:maxRows]
	}
	out := make([]map[string]any, 0, len(rows))
	for _, r := range rows {
		downFor := nowMs - r.since
		if downFor < 0 {
			downFor = 0
		}
		out = append(out, map[string]any{"host": leftUTF16(r.host, 200), "downSinceMs": float64(r.since),
			"downForMs": float64(downFor), "retired": downFor >= retiredAfterMs})
	}
	return out
}

var remotePathPattern = regexp.MustCompile(`^/[A-Za-z0-9._/@+-]*$`)

func validRemotePath(path string) bool {
	if path == "" || len(path) > 512 || !strings.HasPrefix(path, "/") || !remotePathPattern.MatchString(path) {
		return false
	}
	for _, part := range strings.Split(path, "/") {
		if part == ".." || part == "." {
			return false
		}
	}
	return true
}

func diskUsageCommand(path string) string {
	if !validRemotePath(path) {
		return ""
	}
	return fmt.Sprintf(`sh -c 'du -x -k --max-depth=1 -- "%s" 2>/dev/null | sort -rn | head -n 80'`, path)
}

var whitespacePattern = regexp.MustCompile(`\s`)

func parseDiskUsage(output, path string, maxRows int) []map[string]any {
	base := path
	for len(base) > 1 && strings.HasSuffix(base, "/") {
		base = base[:len(base)-1]
	}
	type row struct {
		name, full string
		kb         int64
		self       bool
	}
	rows := []row{}
	for _, raw := range strings.Split(output, "\n") {
		if raw == "" {
			continue
		}
		line := strings.TrimSpace(raw)
		location := whitespacePattern.FindStringIndex(line)
		if location == nil || location[0] <= 0 {
			continue
		}
		kb, err := strconv.ParseInt(line[:location[0]], 10, 64)
		if err != nil || kb < 0 {
			continue
		}
		full := strings.TrimSpace(line[location[0]:])
		for len(full) > 1 && strings.HasSuffix(full, "/") {
			full = full[:len(full)-1]
		}
		r := row{kb: kb, full: full, self: full == base}
		if !r.self {
			prefix := base + "/"
			if base == "/" {
				prefix = base
			}
			if !strings.HasPrefix(full, prefix) {
				continue
			}
			r.name = full[len(prefix):]
			if r.name == "" || strings.Contains(r.name, "/") {
				continue
			}
		}
		rows = append(rows, r)
	}
	sort.SliceStable(rows, func(i, j int) bool {
		if rows[i].self != rows[j].self {
			return rows[i].self
		}
		return rows[i].kb > rows[j].kb
	})
	if len(rows) > maxRows+1 {
		rows = rows[:maxRows+1]
	}
	out := make([]map[string]any, 0, len(rows))
	for _, r := range rows {
		name := r.name
		if r.self {
			name = "."
		}
		out = append(out, map[string]any{"name": name, "path": r.full, "bytes": float64(r.kb) * 1024.0, "total": r.self})
	}
	return out
}
