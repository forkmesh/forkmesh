package agentmodel

import (
	"math"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"unicode"

	"forkmesh.com/desktop-core/internal/qtjson"
)

const (
	maxBulkIDs        = 50
	maxBinAgents      = 500
	maxWorktrees      = 60
	maxGrants         = 200
	maxDiffChars      = 400000
	maxDiffFiles      = 500
	maxUntracked      = 200
	maxDiffImageBytes = 2 * 1024 * 1024
)

var (
	opsActions    = []string{"approve", "deny", "merge", "stop", "continue", "revoke", "delete", "purge"}
	opsLifetimes  = []string{"once", "session", "worktree", "persistent"}
	windowsPathRe = regexp.MustCompile(`^[A-Za-z]:[\\/]`)
	shortstatRe   = regexp.MustCompile(`(\d+) files? changed(?:, (\d+) insertions?\(\+\))?(?:, (\d+) deletions?\(-\))?`)
	approvalIDRe  = regexp.MustCompile(`^[0-9a-f]{32}$`)
	grantIDRe     = regexp.MustCompile(`^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$`)
	whitespaceRe  = regexp.MustCompile(`\s+`)
)

func containsStr(list []string, value string) bool {
	for _, item := range list {
		if item == value {
			return true
		}
	}
	return false
}

func isKnownAction(action string) bool { return containsStr(opsActions, action) }

func isDestructiveAction(action string) bool {
	return action == "merge" || action == "delete" || action == "purge"
}

func validLifetime(lifetime string) bool { return containsStr(opsLifetimes, lifetime) }

func answerText(allow bool, lifetime string) string {
	if !allow {
		return "Deny"
	}
	switch lifetime {
	case "session":
		return "Allow for session"
	case "worktree":
		return "Allow for worktree"
	case "persistent":
		return "Always allow"
	}
	return "Allow once"
}

func oneLine(text string, maxChars int) string {
	out := trimmed(strings.ReplaceAll(text, "\r", " "))
	if len16(out) > maxChars {
		out = left16(out, maxChars-1) + "…"
	}
	return out
}

func lifetimePhrase(lifetime string) string {
	switch lifetime {
	case "session":
		return "for the rest of this session"
	case "worktree":
		return "for everything in this worktree"
	case "persistent":
		return "from now on, on this desktop (saved until you revoke it)"
	}
	return "this once"
}

func shortResource(resource string) string {
	if strings.HasPrefix(resource, "/") || windowsPathRe.MatchString(resource) {
		trimmedPath := resource
		for len16(trimmedPath) > 1 && (strings.HasSuffix(trimmedPath, "/") || strings.HasSuffix(trimmedPath, "\\")) {
			trimmedPath = trimmedPath[:len(trimmedPath)-1]
		}
		cut := max(strings.LastIndex(trimmedPath, "/"), strings.LastIndex(trimmedPath, "\\"))
		name := trimmedPath[cut+1:]
		if name == "" {
			return "(a folder)"
		}
		return left16(name, 200)
	}
	return left16(resource, 200)
}

func approvalSummary(allow bool, lifetime, question, sessionTitle string) string {
	who := "The agent"
	if trimmed(sessionTitle) != "" {
		who = "\"" + oneLine(sessionTitle, 80) + "\""
	}
	what := oneLine(question, 300)
	if !allow {
		return "Deny " + who + "'s request: " + what + ". The agent is told no and carries on without it."
	}
	return "Allow " + who + "'s request " + lifetimePhrase(lifetime) + ": " + what
}

func mergeSummary(branch, base, repo string) string {
	if base == "" {
		base = "the default branch"
	}
	return "Merge " + branch + " into " + base + " in " + repo + ", then delete its worktree, the branch and the agent record. Uncommitted work in the worktree stops the merge. This cannot be undone."
}

func deleteSummary(branch, repo string, running bool) string {
	stop := "Delete this agent"
	if running {
		stop = "Stop this agent and delete it"
	}
	if branch == "" {
		return stop + " from the desktop in " + repo + ". Its record moves to the recycle bin."
	}
	return stop + " in " + repo + " together with its worktree and the unmerged branch " + branch + ". Work that was not merged is lost."
}

func outsideDeleteSummary(label, cwd string) string {
	if label == "" {
		label = "the agent"
	}
	if cwd == "" {
		cwd = "a terminal"
	}
	return "End " + label + ", which was started outside ForkMesh in " + cwd + ", and remove it from the list. Anything it has not saved is lost."
}

func bulkSummary(action string, count int) string {
	n := "1 agent"
	if count != 1 {
		n = strconv.Itoa(count) + " agents"
	}
	switch action {
	case "stop":
		return "Stop " + n + " on the desktop. Their work so far stays in their worktrees."
	case "continue":
		return "Continue " + n + " on the desktop where they left off."
	}
	return ""
}

func purgeSummary(count int) string {
	n := "1 deleted agent"
	if count != 1 {
		n = strconv.Itoa(count) + " deleted agents"
	}
	return "Permanently delete " + n + " from the desktop's recycle bin, with their transcripts. This cannot be undone."
}

func revokeSummary(capability, resource string) string {
	return "Revoke the saved permission for " + capability + " on " + shortResource(resource) + ". Agents are asked again next time."
}

func answerTarget(approvalID string, allow bool, lifetime string) string {
	if allow {
		return approvalID + ":" + lifetime
	}
	return approvalID + ":deny"
}

func validApprovalID(id string) bool { return approvalIDRe.MatchString(id) }

func validGrantID(id string) bool { return grantIDRe.MatchString(id) }

func approvalKind(capability string) string {
	switch capability {
	case "process.execute":
		return "command"
	case "network.connect":
		return "network"
	case "file.read", "file.write":
		return "file"
	case "secret.read":
		return "secret"
	case "tool.invoke", "resource.read", "device.control":
		return "tool"
	}
	return "other"
}

func canonicalIDs(ids qtjson.Value, limit int) ([]int, bool) {
	items := ids.Items()
	if !ids.IsArray() || len(items) == 0 || len(items) > limit {
		return nil, false
	}
	seen := map[int]bool{}
	out := []int{}
	for _, value := range items {
		if !value.IsNumber() {
			return nil, false
		}
		raw := value.Float()
		if raw != math.Floor(raw) || raw <= 0 || raw > 2147483647.0 {
			return nil, false
		}
		id := int(raw)
		if !seen[id] {
			seen[id] = true
			out = append(out, id)
		}
	}
	sort.Ints(out)
	return out, true
}

func bulkTarget(action string, ids []int) string {
	parts := make([]string, len(ids))
	for i, id := range ids {
		parts[i] = strconv.Itoa(id)
	}
	return action + ":" + strings.Join(parts, ",")
}

func section0(text string) string {
	if cut := strings.IndexByte(text, '\n'); cut >= 0 {
		return text[:cut]
	}
	return text
}

func binTitle(issueNumber int, issueTitle, prompt string) string {
	title := prompt
	if issueNumber > 0 && issueTitle != "" {
		title = issueTitle
	}
	title = simplified(section0(title))
	if len16(title) > 90 {
		title = left16(title, 89) + "…"
	}
	return title
}

type binEntry struct {
	ID          int
	Title       string
	Owner       string
	Name        string
	Branch      string
	Provider    string
	DeletedAtMs int64
}

func binPayload(entries []binEntry, retentionHours, minHours, maxHours int, nowMs int64) qtjson.Value {
	retentionMs := int64(retentionHours) * 60 * 60 * 1000
	rows := qtjson.NewArray()
	count := 0
	for _, entry := range entries {
		if count >= maxBinAgents {
			break
		}
		removeIn := int64(-1)
		if entry.DeletedAtMs > 0 {
			removeIn = max(0, retentionMs-(nowMs-entry.DeletedAtMs))
		}
		rows.Append(qtjson.ObjectOf("id", entry.ID, "title", entry.Title, "owner", entry.Owner, "name", entry.Name,
			"branch", entry.Branch, "provider", entry.Provider, "deletedAtMs", float64(entry.DeletedAtMs),
			"removeInMs", float64(removeIn)))
		count++
	}
	return qtjson.ObjectOf("agents", rows, "count", len(entries), "truncated", len(entries) > count,
		"retentionHours", retentionHours, "minHours", minHours, "maxHours", maxHours)
}

type mergeShortstat struct {
	SHA        string
	Subject    string
	Files      int
	Insertions int
	Deletions  int
}

func qtToInt(text string) int {
	value, err := strconv.ParseInt(trimmedASCII(text), 10, 32)
	if err != nil {
		return 0
	}
	return int(value)
}

func trimmedASCII(text string) string {
	return strings.TrimFunc(text, unicode.IsSpace)
}

func parseMergeShortstatLog(log string) []mergeShortstat {
	out := []mergeShortstat{}
	for _, record := range strings.Split(log, "\x1e") {
		if record == "" {
			continue
		}
		lines := strings.Split(record, "\n")
		head := strings.Split(lines[0], "\x1f")
		merge := mergeShortstat{SHA: trimmed(head[0])}
		if merge.SHA == "" {
			continue
		}
		merge.Subject = trimmed(strings.Join(head[1:], "\x1f"))
		for _, line := range lines[1:] {
			match := shortstatRe.FindStringSubmatch(line)
			if match == nil {
				continue
			}
			merge.Files = qtToInt(match[1])
			merge.Insertions = qtToInt(match[2])
			merge.Deletions = qtToInt(match[3])
			break
		}
		out = append(out, merge)
	}
	return out
}

func mergedBranchFromMergeSubject(subject string) string {
	s := trimmed(subject)
	if !strings.HasPrefix(s, "Merge ") {
		return ""
	}
	if strings.HasPrefix(s, "Merge pull request #") {
		from := strings.Index(s, " from ")
		if from < 0 {
			return ""
		}
		head := trimmed(s[from+6:])
		if cut := strings.IndexByte(head, ' '); cut >= 0 {
			head = head[:cut]
		}
		if slash := strings.IndexByte(head, '/'); slash >= 0 {
			return head[slash+1:]
		}
		return head
	}
	if strings.HasPrefix(s, "Merge branch '") {
		u := toU16(s)
		end := -1
		for i := 14; i < len(u); i++ {
			if u[i] == '\'' {
				end = i
				break
			}
		}
		if end < 0 {
			return ""
		}
		return fromU16(u[14:end])
	}
	u := toU16(s)
	marker := toU16(" into ")
	into := -1
	for i := len(u) - len(marker); i >= 0; i-- {
		match := true
		for j := range marker {
			if u[i+j] != marker[j] {
				match = false
				break
			}
		}
		if match {
			into = i
			break
		}
	}
	if into <= 6 {
		return ""
	}
	return trimmed(fromU16(u[6:into]))
}

type mergeVelocity struct {
	minutely, hourly, daily, monthly, weekly                  []int
	lastHour, last24Hours, last7Days, last30Days, last52Weeks int
	periodTop                                                 []int
	periodAvg                                                 []float64
}

func mergeVelocityForTimestamps(timestamps []int64, nowSecs int64) mergeVelocity {
	const minute, hour = int64(60), int64(3600)
	const day = 24 * hour
	v := mergeVelocity{minutely: make([]int, 60), hourly: make([]int, 24), daily: make([]int, 7),
		monthly: make([]int, 30), weekly: make([]int, 52)}
	for _, timestamp := range timestamps {
		age := max(0, nowSecs-timestamp)
		if timestamp > nowSecs+5*60 || age >= 52*7*day {
			continue
		}
		v.weekly[51-int(age/(7*day))]++
		v.last52Weeks++
		if age >= 30*day {
			continue
		}
		v.monthly[29-int(age/day)]++
		v.last30Days++
		if age >= 7*day {
			continue
		}
		v.daily[6-int(age/day)]++
		v.last7Days++
		if age >= day {
			continue
		}
		v.hourly[23-int(age/hour)]++
		v.last24Hours++
		if age >= hour {
			continue
		}
		v.minutely[59-int(age/minute)]++
		v.lastHour++
	}
	lengths := []int64{hour, day, 7 * day, 30 * day, 52 * 7 * day}
	totals := []int{v.lastHour, v.last24Hours, v.last7Days, v.last30Days, v.last52Weeks}
	sorted := []int64{}
	for _, timestamp := range timestamps {
		if timestamp > nowSecs+5*60 {
			continue
		}
		sorted = append(sorted, min(timestamp, nowSecs))
	}
	sort.Slice(sorted, func(i, j int) bool { return sorted[i] < sorted[j] })
	v.periodTop = make([]int, 5)
	v.periodAvg = make([]float64, 5)
	for k := 0; k < 5; k++ {
		length := lengths[k]
		top := totals[k]
		first := 0
		for last := range sorted {
			for sorted[first] <= sorted[last]-length {
				first++
			}
			top = max(top, last-first+1)
		}
		v.periodTop[k] = top
		if len(sorted) > 0 {
			periods := (nowSecs-sorted[0])/length + 1
			v.periodAvg[k] = float64(len(sorted)) / float64(periods)
		}
	}
	return v
}

type mergeVelocityWindow struct {
	ID     string
	Label  string
	Count  int
	Top    int
	Avg    float64
	Series []int
}

type mergeStats struct {
	Base       string
	Commit     string
	Commits    int
	Files      int
	Insertions int
	Deletions  int
	Velocity   []mergeVelocityWindow
}

func qtRound(value float64) float64 {
	return math.Round(value)
}

func mergeStatsJSON(stats mergeStats) qtjson.Value {
	velocity := qtjson.NewArray()
	for _, window := range stats.Velocity {
		row := qtjson.ObjectOf("id", window.ID, "label", window.Label, "count", window.Count,
			"top", max(window.Count, window.Top), "avg", qtRound(window.Avg*10.0)/10.0)
		if len(window.Series) > 0 {
			series := qtjson.NewArray()
			for _, bucket := range window.Series {
				series.Append(qtjson.NewInt(int64(bucket)))
			}
			row.Set("series", series)
		}
		velocity.Append(row)
	}
	out := qtjson.ObjectOf("base", stats.Base, "files", stats.Files, "insertions", stats.Insertions,
		"deletions", stats.Deletions, "velocity", velocity)
	if stats.Commit != "" {
		out.Set("commit", qtjson.NewString(stats.Commit))
	}
	if stats.Commits >= 0 {
		out.Set("commits", qtjson.NewInt(int64(stats.Commits)))
	}
	return out
}

func velocityWindows(v mergeVelocity) []mergeVelocityWindow {
	window := func(period int, id, label string, count int, series []int) mergeVelocityWindow {
		row := mergeVelocityWindow{ID: id, Label: label, Count: count, Series: series}
		if period < len(v.periodTop) {
			row.Top = v.periodTop[period]
		} else {
			row.Top = count
		}
		if period < len(v.periodAvg) {
			row.Avg = v.periodAvg[period]
		} else {
			row.Avg = float64(count)
		}
		return row
	}
	return []mergeVelocityWindow{
		window(0, "1h", "1 hour", v.lastHour, v.minutely),
		window(1, "24h", "24 hours", v.last24Hours, v.hourly),
		window(2, "7d", "7 days", v.last7Days, v.daily),
		window(3, "30d", "30 days", v.last30Days, v.monthly),
		window(4, "52w", "52 weeks", v.last52Weeks, v.weekly),
	}
}

func splitSkipEmpty(text, sep string) []string {
	out := []string{}
	for _, part := range strings.Split(text, sep) {
		if part != "" {
			out = append(out, part)
		}
	}
	return out
}

func numstatRows(numstat string, limit int) qtjson.Value {
	rows := qtjson.NewArray()
	count := 0
	for _, line := range splitSkipEmpty(numstat, "\n") {
		if count >= limit {
			break
		}
		parts := strings.Split(line, "\t")
		if len(parts) < 3 {
			continue
		}
		binary := parts[0] == "-"
		added, removed := 0, 0
		if !binary {
			added = qtToInt(parts[0])
			removed = qtToInt(parts[1])
		}
		rows.Append(qtjson.ObjectOf("path", left16(strings.Join(parts[2:], "\t"), 500), "added", added,
			"removed", removed, "binary", binary))
		count++
	}
	return rows
}

func capDiff(text []uint16, budget int, truncated *bool) []uint16 {
	if budget <= 0 {
		if len(text) > 0 {
			*truncated = true
		}
		return nil
	}
	if len(text) <= budget {
		return text
	}
	*truncated = true
	cut := -1
	for i := min(budget-1, len(text)-1); i >= 0; i-- {
		if text[i] == '\n' {
			cut = i
			break
		}
	}
	if cut > 0 {
		return text[:cut+1]
	}
	return text[:budget]
}

func untrackedPaths(porcelain string, limit int) []string {
	out := []string{}
	for _, line := range splitSkipEmpty(porcelain, "\n") {
		if len(out) >= limit {
			break
		}
		if strings.HasPrefix(line, "?? ") {
			out = append(out, left16(line[3:], 500))
		}
	}
	return out
}

func diffImageMime(path string) string {
	dot := strings.LastIndexByte(path, '.')
	if dot < 0 || dot < strings.LastIndexByte(path, '/') {
		return ""
	}
	switch strings.ToLower(path[dot+1:]) {
	case "png":
		return "image/png"
	case "jpg", "jpeg":
		return "image/jpeg"
	case "gif":
		return "image/gif"
	case "webp":
		return "image/webp"
	case "bmp":
		return "image/bmp"
	case "ico":
		return "image/x-icon"
	case "heic":
		return "image/heic"
	case "tif", "tiff":
		return "image/tiff"
	case "svg":
		return "image/svg+xml"
	}
	return ""
}

func validDiffImagePath(path string) bool {
	if path == "" || len16(path) > 1000 || strings.HasPrefix(path, "/") || strings.Contains(path, "\\") ||
		strings.ContainsRune(path, 0) {
		return false
	}
	for _, part := range strings.Split(path, "/") {
		if part == "" || part == "." || part == ".." {
			return false
		}
	}
	return diffImageMime(path) != ""
}

type worktreeEntry struct {
	Path     string
	Head     string
	Branch   string
	Bare     bool
	Detached bool
	Locked   bool
	Prunable bool
}

func parseWorktreePorcelain(text string, limit int) []worktreeEntry {
	out := []worktreeEntry{}
	current := worktreeEntry{}
	open := false
	flush := func() {
		if open && current.Path != "" && len(out) < limit {
			out = append(out, current)
		}
		current = worktreeEntry{}
		open = false
	}
	for _, raw := range strings.Split(text, "\n") {
		line := trimmed(raw)
		switch {
		case line == "":
			flush()
		case strings.HasPrefix(line, "worktree "):
			flush()
			current.Path = line[9:]
			open = true
		case strings.HasPrefix(line, "HEAD "):
			current.Head = line[5:]
		case strings.HasPrefix(line, "branch "):
			current.Branch = strings.TrimPrefix(line[7:], "refs/heads/")
		case line == "bare":
			current.Bare = true
		case line == "detached":
			current.Detached = true
		case line == "locked" || strings.HasPrefix(line, "locked "):
			current.Locked = true
		case line == "prunable" || strings.HasPrefix(line, "prunable "):
			current.Prunable = true
		}
	}
	flush()
	return out
}

func qtFileName(path string) string {
	return path[strings.LastIndexByte(path, '/')+1:]
}

func worktreeRow(entry worktreeEntry, isMain bool, ahead, behind, sessionID int) qtjson.Value {
	name := qtFileName(entry.Path)
	if name == "" {
		name = entry.Branch
	}
	row := qtjson.ObjectOf("name", name, "branch", entry.Branch, "head", left16(entry.Head, 12), "isMain", isMain,
		"detached", entry.Detached, "locked", entry.Locked, "prunable", entry.Prunable, "ahead", ahead,
		"behind", behind)
	if sessionID > 0 {
		row.Set("sessionId", qtjson.NewInt(int64(sessionID)))
	}
	return row
}

func parseLeftRight(text string) (behind, ahead int, ok bool) {
	parts := whitespaceRe.Split(trimmed(text), -1)
	if len(parts) != 2 || parts[0] == "" {
		return 0, 0, false
	}
	left, err1 := strconv.ParseInt(parts[0], 10, 32)
	right, err2 := strconv.ParseInt(parts[1], 10, 32)
	if err1 != nil || err2 != nil || left < 0 || right < 0 {
		return 0, 0, false
	}
	return int(left), int(right), true
}

func grantRows(grants qtjson.Value, limit int) qtjson.Value {
	out := qtjson.NewArray()
	count := 0
	for _, grant := range grants.Items() {
		if count >= limit {
			break
		}
		id := grant.Get("id").Str()
		if !validGrantID(id) {
			continue
		}
		capability := grant.Get("capability").Str()
		row := qtjson.ObjectOf("id", id, "capability", capability, "kind", approvalKind(capability),
			"resource", shortResource(grant.Get("resource").Str()), "decision", grant.Get("decision").Str(),
			"lifetime", grant.Get("lifetime").Str())
		if worktree := grant.Get("worktreePath").Str(); worktree != "" {
			row.Set("worktree", qtjson.NewString(qtFileName(worktree)))
		}
		out.Append(row)
		count++
	}
	return out
}

type usageWindow struct {
	ID          string
	Label       string
	UsedPercent int
	ResetAtMs   int64
	Exhausted   bool
}

type usageAccount struct {
	ID       string
	Label    string
	Active   bool
	SignedIn bool
	Identity string
	Windows  []usageWindow
}

type usageProvider struct {
	ID             string
	Label          string
	Windows        []usageWindow
	Accounts       []usageAccount
	CreditsEnabled bool
	CreditsUsed    float64
	CreditsLimit   float64
	CreditsCurr    string
}

func usageWindowRows(source []usageWindow, nowMs int64) qtjson.Value {
	windows := qtjson.NewArray()
	for _, window := range source {
		elapsed := window.ResetAtMs > 0 && window.ResetAtMs <= nowMs
		used := window.UsedPercent
		if elapsed {
			used = 0
		}
		state := "unknown"
		switch {
		case used >= 99 || (!elapsed && window.Exhausted):
			state = "exhausted"
		case used >= 80:
			state = "high"
		case used >= 0:
			state = "ok"
		}
		row := qtjson.ObjectOf("id", window.ID, "label", window.Label, "usedPercent", used, "state", state)
		if !elapsed && window.ResetAtMs > 0 {
			row.Set("resetAtMs", qtjson.NewDouble(float64(window.ResetAtMs)))
		}
		windows.Append(row)
	}
	return windows
}

func usageSummary(providers []usageProvider, claudeSpend, openAiSpend float64, nowMs int64) qtjson.Value {
	list := qtjson.NewArray()
	for _, provider := range providers {
		accounts := qtjson.NewArray()
		for _, account := range provider.Accounts {
			accounts.Append(qtjson.ObjectOf("id", left16(account.ID, 80), "label", left16(account.Label, 80),
				"active", account.Active, "signedIn", account.SignedIn, "identity", left16(account.Identity, 120),
				"windows", usageWindowRows(account.Windows, nowMs)))
		}
		row := qtjson.ObjectOf("id", provider.ID, "label", provider.Label,
			"windows", usageWindowRows(provider.Windows, nowMs), "accounts", accounts)
		if provider.CreditsEnabled {
			credits := qtjson.ObjectOf("currency", left16(provider.CreditsCurr, 8))
			if provider.CreditsUsed >= 0 {
				credits.Set("used", qtjson.NewDouble(provider.CreditsUsed))
			}
			if provider.CreditsLimit >= 0 {
				credits.Set("limit", qtjson.NewDouble(provider.CreditsLimit))
			}
			row.Set("credits", credits)
		}
		list.Append(row)
	}
	spend := qtjson.NewObject()
	if !math.IsNaN(claudeSpend) && claudeSpend >= 0 {
		spend.Set("claudeUsd", qtjson.NewDouble(claudeSpend))
	}
	if !math.IsNaN(openAiSpend) && openAiSpend >= 0 {
		spend.Set("openAiUsd", qtjson.NewDouble(openAiSpend))
	}
	return qtjson.ObjectOf("providers", list, "spend", spend, "atMs", float64(nowMs))
}

func usageWindowsFrom(value qtjson.Value) []usageWindow {
	out := []usageWindow{}
	for _, item := range value.Items() {
		out = append(out, usageWindow{ID: item.Get("id").Str(), Label: item.Get("label").Str(),
			UsedPercent: item.Get("usedPercent").IntOr(-1), ResetAtMs: item.Get("resetAtMs").Int64(),
			Exhausted: item.Get("exhausted").BoolValue()})
	}
	return out
}

func usageProvidersFrom(value qtjson.Value) []usageProvider {
	out := []usageProvider{}
	for _, item := range value.Items() {
		provider := usageProvider{ID: item.Get("id").Str(), Label: item.Get("label").Str(),
			Windows: usageWindowsFrom(item.Get("windows")), CreditsEnabled: item.Get("creditsEnabled").BoolValue(),
			CreditsUsed: -1, CreditsLimit: -1, CreditsCurr: item.Get("creditsCurrency").Str()}
		if used := item.Get("creditsUsed"); used.IsNumber() {
			provider.CreditsUsed = used.Float()
		}
		if limit := item.Get("creditsLimit"); limit.IsNumber() {
			provider.CreditsLimit = limit.Float()
		}
		for _, account := range item.Get("accounts").Items() {
			provider.Accounts = append(provider.Accounts, usageAccount{ID: account.Get("id").Str(),
				Label: account.Get("label").Str(), Active: account.Get("active").BoolValue(),
				SignedIn: account.Get("signedIn").BoolValue(), Identity: account.Get("identity").Str(),
				Windows: usageWindowsFrom(account.Get("windows"))})
		}
		out = append(out, provider)
	}
	return out
}
