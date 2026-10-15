package insights

import (
	"errors"
	"math"
	"os"
	"sort"
	"strconv"
	"strings"
	"time"

	"forkmesh.com/desktop-core/internal/qtjson"
)

const (
	RecentSamples        = 24
	ActivityBuckets      = 24
	ActivityBucketMs     = 5 * 60 * 1000
	MaxMatchedFiles      = 400
	MaxPendingCalls      = 4096
	HeldBehindSiblingMs  = 1000
	MaxUsageRows         = 400
	ledgerSchemaVersion  = 1
	maxResultLines       = 5000
	usageKeySeparator    = " · "
	pendingKeySeparator  = "\x1f"
	modelUsageKeySeparat = "\x1f"
)

type FamilyStats struct {
	Family         string
	Calls          int64
	Errors         int64
	InFlight       int64
	MeasuredCalls  int64
	LatencyMsTotal int64
	LatencyMsMax   int64
	LatencySamples int64
	InputTokens    int64
	ResultTokens   int64
	BaselineTokens int64
	SavedTokens    int64
	SavedUsd       float64
	ConsumedUsd    float64
	LastCallMs     int64
	CallsWhileOff  int64
	RecentSaved    []int64
	RuntimeNames   map[string]int64
}

func (s FamilyStats) ConsumedTokens() int64 { return s.InputTokens + s.ResultTokens }

func (s FamilyStats) AverageLatencyMs() float64 {
	if s.LatencySamples > 0 {
		return float64(s.LatencyMsTotal) / float64(s.LatencySamples)
	}
	return 0
}

func (s FamilyStats) SavingsRatio() float64 {
	if s.BaselineTokens > 0 {
		return float64(s.SavedTokens) / float64(s.BaselineTokens)
	}
	return 0
}

type UsageStats struct {
	Key            string
	Tool           string
	Command        string
	Family         string
	Calls          int64
	Errors         int64
	LatencyMsTotal int64
	LatencyMsMax   int64
	LatencySamples int64
	InputTokens    int64
	ResultTokens   int64
	SavedTokens    int64
	ConsumedUsd    float64
	LastCallMs     int64
}

func (u UsageStats) ConsumedTokens() int64 { return u.InputTokens + u.ResultTokens }

func (u UsageStats) ErrorRate() float64 {
	if u.Calls > 0 {
		return float64(u.Errors) / float64(u.Calls)
	}
	return 0
}

func (u UsageStats) AverageLatencyMs() float64 {
	if u.LatencySamples > 0 {
		return float64(u.LatencyMsTotal) / float64(u.LatencySamples)
	}
	return 0
}

func (u UsageStats) TokensPerCall() float64 {
	if u.Calls > 0 {
		return float64(u.ConsumedTokens()) / float64(u.Calls)
	}
	return 0
}

type ModelStats struct {
	Provider       string
	Model          string
	Calls          int64
	Errors         int64
	LatencyMsTotal int64
	LatencySamples int64
	InputTokens    int64
	ResultTokens   int64
	SavedTokens    int64
	ConsumedUsd    float64
	LastCallMs     int64
}

type Observation struct {
	SessionID         int
	Family            string
	RuntimeName       string
	Provider          string
	Model             string
	Workdir           string
	Input             qtjson.Value
	StartedMs         int64
	CompletedMs       int64
	InputTokens       int64
	ResultTokens      int64
	IsError           bool
	Abandoned         bool
	WhileOff          bool
	HeldBehindSibling bool
	ResultText        string
	Measured          bool
	BaselineTokens    int64
	ActualTokens      int64
	OutputSide        bool
}

type Totals struct {
	Calls            int64
	Errors           int64
	InFlight         int64
	ConsumedTokens   int64
	SavedTokens      int64
	SavedUsd         float64
	ConsumedUsd      float64
	FamiliesObserved int
	FamiliesTotal    int
	FirstCallMs      int64
	LastCallMs       int64
	RecentCalls      []int64
	RecentSaved      []int64
}

type SessionContext struct {
	SessionID int
	Provider  string
	Model     string
	Workdir   string
}

type pendingCall struct {
	sessionID   int
	family      string
	runtimeName string
	usageKey    string
	provider    string
	model       string
	workdir     string
	input       qtjson.Value
	startedMs   int64
	inputTokens int64
	whileOff    bool
}

type lastCompletion struct {
	startedMs   int64
	completedMs int64
}

type baselineResult struct {
	measured       bool
	baselineTokens int64
	actualTokens   int64
	outputSide     bool
}

type Ledger struct {
	path                string
	families            map[string]*FamilyStats
	usage               map[string]*UsageStats
	models              map[string]*ModelStats
	pending             map[string]*pendingCall
	lastCompletion      map[int]lastCompletion
	disabled            map[string]bool
	activityCalls       []int64
	activitySaved       []int64
	activityBucketStart int64
	totalCalls          int64
	firstCallMs         int64
	lastCallMs          int64

	Changed      func()
	CallObserved func(Observation)
}

func NewLedger(path string) *Ledger {
	return &Ledger{path: path, families: map[string]*FamilyStats{}, usage: map[string]*UsageStats{},
		models: map[string]*ModelStats{}, pending: map[string]*pendingCall{}, lastCompletion: map[int]lastCompletion{},
		disabled: map[string]bool{}}
}

func (l *Ledger) Path() string { return l.path }

func nowMs() int64 { return time.Now().UnixMilli() }

func nowOr(ms int64) int64 {
	if ms >= 0 {
		return ms
	}
	return nowMs()
}

func EstimateTokens(bytes int64) int64 {
	if bytes <= 0 {
		return 0
	}
	return (bytes + 3) / 4
}

type tokenPrice struct {
	input  float64
	output float64
}

func priceFor(provider, model string) tokenPrice {
	m := strings.ToLower(model)
	if provider == "cloudflare-ai" || provider == "local-llama" {
		return tokenPrice{}
	}
	if strings.HasPrefix(provider, "claude") {
		if strings.Contains(m, "haiku") {
			return tokenPrice{1.0, 5.0}
		}
		if strings.Contains(m, "sonnet") {
			return tokenPrice{3.0, 15.0}
		}
		return tokenPrice{5.0, 25.0}
	}
	return tokenPrice{1.25, 10.0}
}

func commandLine(input qtjson.Value) string {
	value := input.Get("command")
	if value.IsArray() {
		parts := make([]string, 0, value.Len())
		for _, part := range value.Items() {
			parts = append(parts, part.Str())
		}
		return strings.Join(parts, " ")
	}
	return value.Str()
}

func UsageKey(runtimeToolName, family string, input qtjson.Value) (string, string) {
	tool := runtimeToolName
	if tool == "" {
		tool = "?"
	}
	shape := ""
	if family == "bash" {
		shape = left16(CommandShape(commandLine(input)), 80)
	}
	if shape == "" {
		return tool, shape
	}
	return tool + usageKeySeparator + shape, shape
}

func ModelUsageKey(provider, model string) string {
	return trimmed(provider) + modelUsageKeySeparat + strings.ToLower(trimmed(model))
}

func pendingKey(sessionID int, toolUseID string) string {
	return strconv.Itoa(sessionID) + pendingKeySeparator + toolUseID
}

func toolResultText(block qtjson.Value) string {
	content := block.Get("content")
	if content.IsString() {
		return content.Str()
	}
	var b strings.Builder
	for _, part := range content.Items() {
		b.WriteString(part.Get("text").Str())
	}
	return b.String()
}

func (l *Ledger) Ingest(context SessionContext, event qtjson.Value, now int64) bool {
	now = nowOr(now)
	kind := event.Get("type").Str()
	content := event.Get("message").Get("content")
	changed := false
	switch kind {
	case "assistant":
		for _, block := range content.Items() {
			if block.Get("type").Str() != "tool_use" {
				continue
			}
			input := block.Get("input")
			if !input.IsObject() {
				input = qtjson.NewObject()
			}
			if l.BeginCall(context, block.Get("id").Str(), block.Get("name").Str(), input, now) {
				changed = true
			}
		}
	case "user":
		for _, block := range content.Items() {
			if block.Get("type").Str() != "tool_result" {
				continue
			}
			if l.CompleteCall(context.SessionID, block.Get("tool_use_id").Str(), toolResultText(block),
				block.Get("is_error").BoolValue(), now) {
				changed = true
			}
		}
	}
	return changed
}

func (l *Ledger) family(id string) *FamilyStats {
	stats := l.families[id]
	if stats == nil {
		stats = &FamilyStats{Family: id, RuntimeNames: map[string]int64{}}
		l.families[id] = stats
	}
	return stats
}

func (l *Ledger) trimUsage() {
	for len(l.usage) > MaxUsageRows {
		oldest := ""
		var oldestMs int64
		for key, usage := range l.usage {
			if oldest == "" || usage.LastCallMs < oldestMs || (usage.LastCallMs == oldestMs && key < oldest) {
				oldest, oldestMs = key, usage.LastCallMs
			}
		}
		delete(l.usage, oldest)
	}
}

func (l *Ledger) BeginCall(context SessionContext, toolUseID, runtimeToolName string, input qtjson.Value, now int64) bool {
	id := trimmed(toolUseID)
	if id == "" || u16len(id) > 256 {
		return false
	}
	key := pendingKey(context.SessionID, id)
	if _, ok := l.pending[key]; ok {
		return false
	}
	if len(l.pending) >= MaxPendingCalls {
		oldest := ""
		var oldestMs int64
		for pk, call := range l.pending {
			if oldest == "" || call.startedMs < oldestMs || (call.startedMs == oldestMs && pk < oldest) {
				oldest, oldestMs = pk, call.startedMs
			}
		}
		stale := l.family(l.pending[oldest].family)
		stale.InFlight = max(0, stale.InFlight-1)
		delete(l.pending, oldest)
	}
	now = nowOr(now)
	call := &pendingCall{sessionID: context.SessionID, provider: context.Provider, model: context.Model,
		workdir: context.Workdir, input: input, startedMs: now}
	call.runtimeName = left16(trimmed(runtimeToolName), 200)
	call.family = FamilyFor(call.runtimeName, input)
	call.inputTokens = EstimateTokens(int64(len(input.Compact())))
	var shape string
	call.usageKey, shape = UsageKey(call.runtimeName, call.family, input)

	stats := l.family(call.family)
	stats.Calls++
	stats.InFlight++
	stats.InputTokens += call.inputTokens
	stats.LastCallMs = now
	name := call.runtimeName
	if name == "" {
		name = "?"
	}
	stats.RuntimeNames[name]++
	call.whileOff = l.disabled[call.family]
	if call.whileOff {
		stats.CallsWhileOff++
	}
	price := priceFor(call.provider, call.model)
	stats.ConsumedUsd += float64(call.inputTokens) * price.output / 1e6

	usage := l.usage[call.usageKey]
	if usage == nil {
		usage = &UsageStats{}
		l.usage[call.usageKey] = usage
	}
	usage.Key = call.usageKey
	usage.Tool = name
	usage.Command = shape
	usage.Family = call.family
	usage.Calls++
	usage.InputTokens += call.inputTokens
	usage.ConsumedUsd += float64(call.inputTokens) * price.output / 1e6
	usage.LastCallMs = now
	l.trimUsage()

	modelKey := ModelUsageKey(call.provider, call.model)
	model := l.models[modelKey]
	if model == nil {
		model = &ModelStats{}
		l.models[modelKey] = model
	}
	model.Provider = trimmed(call.provider)
	model.Model = strings.ToLower(trimmed(call.model))
	model.Calls++
	model.InputTokens += call.inputTokens
	model.ConsumedUsd += float64(call.inputTokens) * price.output / 1e6
	model.LastCallMs = now

	l.pending[key] = call
	l.totalCalls++
	if l.firstCallMs == 0 || now < l.firstCallMs {
		l.firstCallMs = now
	}
	l.lastCallMs = max(l.lastCallMs, now)
	l.bumpActivity(now, 0)
	l.activityCalls[len(l.activityCalls)-1]++
	l.notify()
	return true
}

func (l *Ledger) CompleteCall(sessionID int, toolUseID, resultText string, isError bool, now int64) bool {
	key := pendingKey(sessionID, trimmed(toolUseID))
	call, ok := l.pending[key]
	if !ok {
		return false
	}
	delete(l.pending, key)
	now = nowOr(now)
	previous, had := l.lastCompletion[call.sessionID]
	heldBehindSibling := had && previous.startedMs <= call.startedMs && previous.completedMs > call.startedMs &&
		now-previous.completedMs <= HeldBehindSiblingMs
	l.lastCompletion[call.sessionID] = lastCompletion{startedMs: call.startedMs, completedMs: now}
	stats := l.family(call.family)
	stats.InFlight = max(0, stats.InFlight-1)
	latency := now - call.startedMs
	if latency >= 0 {
		stats.LatencyMsTotal += latency
		stats.LatencyMsMax = max(stats.LatencyMsMax, latency)
		stats.LatencySamples++
	}
	resultTokens := EstimateTokens(int64(len(qtUTF8(resultText))))
	stats.ResultTokens += resultTokens
	price := priceFor(call.provider, call.model)
	stats.ConsumedUsd += float64(resultTokens) * price.input / 1e6
	if isError {
		stats.Errors++
	}
	var saved int64
	var baseline baselineResult
	if !isError {
		baseline = l.measureBaseline(call, resultText, resultTokens)
		if baseline.measured {
			stats.MeasuredCalls++
			stats.BaselineTokens += baseline.baselineTokens
			saved = max(0, baseline.baselineTokens-baseline.actualTokens)
			stats.SavedTokens += saved
			rate := price.input
			if baseline.outputSide {
				rate = price.output
			}
			stats.SavedUsd += float64(saved) * rate / 1e6
		}
	}
	if usage := l.usage[call.usageKey]; usage != nil {
		if latency >= 0 {
			usage.LatencyMsTotal += latency
			usage.LatencyMsMax = max(usage.LatencyMsMax, latency)
			usage.LatencySamples++
		}
		usage.ResultTokens += resultTokens
		usage.ConsumedUsd += float64(resultTokens) * price.input / 1e6
		usage.SavedTokens += saved
		if isError {
			usage.Errors++
		}
		usage.LastCallMs = max(usage.LastCallMs, now)
	}
	if model := l.models[ModelUsageKey(call.provider, call.model)]; model != nil {
		if latency >= 0 {
			model.LatencyMsTotal += latency
			model.LatencySamples++
		}
		model.ResultTokens += resultTokens
		model.ConsumedUsd += float64(resultTokens) * price.input / 1e6
		model.SavedTokens += saved
		if isError {
			model.Errors++
		}
		model.LastCallMs = max(model.LastCallMs, now)
	}
	stats.RecentSaved = append(stats.RecentSaved, saved)
	if len(stats.RecentSaved) > RecentSamples {
		stats.RecentSaved = append([]int64(nil), stats.RecentSaved[len(stats.RecentSaved)-RecentSamples:]...)
	}
	stats.LastCallMs = max(stats.LastCallMs, now)
	l.lastCallMs = max(l.lastCallMs, now)
	l.bumpActivity(now, saved)
	l.notify()
	if l.CallObserved != nil {
		observation := observationFor(call)
		observation.CompletedMs = now
		observation.ResultTokens = resultTokens
		observation.IsError = isError
		observation.HeldBehindSibling = heldBehindSibling
		if isError {
			if u16len(resultText) <= 2048 {
				observation.ResultText = resultText
			} else {
				observation.ResultText = left16(resultText, 1792) + "\n…\n" + right16(resultText, 256)
			}
		}
		observation.Measured = baseline.measured
		observation.BaselineTokens = baseline.baselineTokens
		observation.ActualTokens = baseline.actualTokens
		observation.OutputSide = baseline.outputSide
		l.CallObserved(observation)
	}
	return true
}

func observationFor(call *pendingCall) Observation {
	return Observation{SessionID: call.sessionID, Family: call.family, RuntimeName: call.runtimeName,
		Provider: call.provider, Model: call.model, Workdir: call.workdir, Input: call.input,
		StartedMs: call.startedMs, InputTokens: call.inputTokens, WhileOff: call.whileOff}
}

func (l *Ledger) SetDisabledFamilies(families []string) {
	next := map[string]bool{}
	for _, family := range families {
		next[family] = true
	}
	if len(next) == len(l.disabled) {
		same := true
		for family := range next {
			if !l.disabled[family] {
				same = false
				break
			}
		}
		if same {
			return
		}
	}
	l.disabled = next
	l.notify()
}

func (l *Ledger) DisabledFamilies() []string {
	out := make([]string, 0, len(l.disabled))
	for family := range l.disabled {
		out = append(out, family)
	}
	sort.Strings(out)
	return out
}

func (l *Ledger) AbandonSession(sessionID int, now int64) {
	delete(l.lastCompletion, sessionID)
	abandoned := []Observation{}
	for key, call := range l.pending {
		if call.sessionID != sessionID {
			continue
		}
		stats := l.family(call.family)
		stats.InFlight = max(0, stats.InFlight-1)
		observation := observationFor(call)
		observation.Abandoned = true
		abandoned = append(abandoned, observation)
		delete(l.pending, key)
	}
	if len(abandoned) == 0 {
		return
	}
	l.notify()
	if l.CallObserved == nil {
		return
	}
	now = nowOr(now)
	sort.SliceStable(abandoned, func(i, j int) bool { return abandoned[i].StartedMs < abandoned[j].StartedMs })
	for _, observation := range abandoned {
		observation.CompletedMs = now
		l.CallObserved(observation)
	}
}

func pathOf(input qtjson.Value) string {
	path := input.Get("file_path").Str()
	if path == "" {
		path = input.Get("path").Str()
	}
	if path == "" {
		path = input.Get("notebook_path").Str()
	}
	return path
}

func (l *Ledger) measureBaseline(call *pendingCall, resultText string, resultTokens int64) baselineResult {
	result := baselineResult{}
	family := FamilyByID(call.family)
	if family == nil {
		return result
	}
	input := call.input
	switch family.Baseline {
	case BaselinePartialRead:
		path := pathOf(input)
		if path == "" || strings.Contains(path, "://") {
			return result
		}
		bytes := fileBytes(call.workdir, path)
		if bytes <= 0 {
			return result
		}
		return baselineResult{measured: true, baselineTokens: EstimateTokens(bytes), actualTokens: resultTokens}
	case BaselineAnchoredEdit:
		var bytes int64
		if changes := input.Get("changes"); changes.Len() > 0 && changes.IsArray() {
			seen := map[string]bool{}
			for _, change := range changes.Items() {
				path := change.Get("path").Str()
				if path == "" || seen[path] {
					continue
				}
				seen[path] = true
				bytes += fileBytes(call.workdir, path)
			}
		} else {
			path := pathOf(input)
			if path == "" {
				return result
			}
			bytes = fileBytes(call.workdir, path)
		}
		if bytes <= 0 {
			return result
		}
		return baselineResult{measured: true, outputSide: true, baselineTokens: EstimateTokens(bytes),
			actualTokens: call.inputTokens}
	case BaselineMatchedFiles:
		bytes := matchedFileBytes(call.workdir, resultText)
		if bytes <= 0 {
			return result
		}
		return baselineResult{measured: true, baselineTokens: EstimateTokens(bytes), actualTokens: resultTokens}
	}
	return result
}

func qtIsAbsolute(path string) bool {
	if strings.HasPrefix(path, "/") || strings.HasPrefix(path, ":") {
		return true
	}
	return len(path) >= 3 && path[1] == ':' && (path[2] == '/' || path[2] == '\\') &&
		((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z'))
}

func joinDir(dir, name string) string {
	if qtIsAbsolute(name) {
		return name
	}
	if len(dir) > 1 && strings.HasSuffix(dir, "/") {
		dir = dir[:len(dir)-1]
	}
	if dir == "" {
		return name
	}
	if strings.HasSuffix(dir, "/") {
		return dir + name
	}
	return dir + "/" + name
}

func fileBytes(workdir, path string) int64 {
	clean := trimmed(path)
	if clean == "" || u16len(clean) > 1024 || strings.Contains(clean, "\n") || strings.Contains(clean, "\x00") {
		return 0
	}
	resolved := ""
	if qtIsAbsolute(clean) {
		resolved = clean
	} else if workdir != "" {
		resolved = joinDir(workdir, clean)
	}
	if resolved == "" {
		return 0
	}
	info, err := os.Stat(resolved)
	if err != nil || !info.Mode().IsRegular() {
		return 0
	}
	return info.Size()
}

func matchedFileBytes(workdir, resultText string) int64 {
	var total int64
	files := 0
	lines := 0
	seen := map[string]bool{}
	consider := func(candidate string) {
		path := trimmed(candidate)
		if path == "" || seen[path] {
			return
		}
		seen[path] = true
		bytes := fileBytes(workdir, path)
		if bytes <= 0 {
			return
		}
		total += bytes
		files++
	}
	for _, line := range strings.Split(resultText, "\n") {
		lines++
		if lines > maxResultLines || files >= MaxMatchedFiles {
			break
		}
		if trimmed(line) == "" {
			continue
		}
		consider(line)
		if colon := strings.IndexByte(line, ':'); colon > 0 {
			consider(line[:colon])
		}
	}
	return total
}

func (l *Ledger) bumpActivity(now int64, savedTokens int64) {
	bucket := (now / ActivityBucketMs) * ActivityBucketMs
	if len(l.activityCalls) != ActivityBuckets || len(l.activitySaved) != ActivityBuckets {
		l.activityCalls = make([]int64, ActivityBuckets)
		l.activitySaved = make([]int64, ActivityBuckets)
		l.activityBucketStart = bucket
	}
	shift := (bucket - l.activityBucketStart) / ActivityBucketMs
	if shift >= ActivityBuckets {
		for i := range l.activityCalls {
			l.activityCalls[i] = 0
			l.activitySaved[i] = 0
		}
		l.activityBucketStart = bucket
	} else if shift > 0 {
		calls := append([]int64(nil), l.activityCalls[shift:]...)
		saved := append([]int64(nil), l.activitySaved[shift:]...)
		for len(calls) < ActivityBuckets {
			calls = append(calls, 0)
			saved = append(saved, 0)
		}
		l.activityCalls, l.activitySaved = calls, saved
		l.activityBucketStart = bucket
	}
	l.activitySaved[len(l.activitySaved)-1] += savedTokens
}

func (l *Ledger) notify() {
	if l.Changed != nil {
		l.Changed()
	}
}

func (l *Ledger) Stats(family string) FamilyStats {
	if stats := l.families[family]; stats != nil {
		copy := *stats
		copy.Family = family
		copy.RecentSaved = append([]int64(nil), stats.RecentSaved...)
		copy.RuntimeNames = map[string]int64{}
		for name, calls := range stats.RuntimeNames {
			copy.RuntimeNames[name] = calls
		}
		return copy
	}
	return FamilyStats{Family: family, RuntimeNames: map[string]int64{}}
}

func (l *Ledger) AllStats() []FamilyStats {
	out := make([]FamilyStats, 0, len(families))
	for _, family := range families {
		out = append(out, l.Stats(family.ID))
	}
	return out
}

func (l *Ledger) Usage() []UsageStats {
	out := make([]UsageStats, 0, len(l.usage))
	keys := make([]string, 0, len(l.usage))
	for key := range l.usage {
		keys = append(keys, key)
	}
	sort.Strings(keys)
	for _, key := range keys {
		out = append(out, *l.usage[key])
	}
	return out
}

func (l *Ledger) ModelUsage() []ModelStats {
	keys := make([]string, 0, len(l.models))
	for key := range l.models {
		keys = append(keys, key)
	}
	sort.Strings(keys)
	out := make([]ModelStats, 0, len(keys))
	for _, key := range keys {
		out = append(out, *l.models[key])
	}
	return out
}

func (l *Ledger) HasSamples() bool { return l.totalCalls > 0 }

func (l *Ledger) PendingCount() int { return len(l.pending) }

func (l *Ledger) Totals(now int64) Totals {
	totals := Totals{FamiliesTotal: len(families)}
	keys := make([]string, 0, len(l.families))
	for key := range l.families {
		keys = append(keys, key)
	}
	sort.Strings(keys)
	for _, key := range keys {
		stats := l.families[key]
		if stats.Calls <= 0 {
			continue
		}
		totals.FamiliesObserved++
		totals.Calls += stats.Calls
		totals.Errors += stats.Errors
		totals.InFlight += stats.InFlight
		totals.ConsumedTokens += stats.ConsumedTokens()
		totals.SavedTokens += stats.SavedTokens
		totals.SavedUsd += stats.SavedUsd
		totals.ConsumedUsd += stats.ConsumedUsd
	}
	totals.FirstCallMs = l.firstCallMs
	totals.LastCallMs = l.lastCallMs
	calls := make([]int64, ActivityBuckets)
	saved := make([]int64, ActivityBuckets)
	if len(l.activityCalls) == ActivityBuckets && len(l.activitySaved) == ActivityBuckets {
		now = nowOr(now)
		bucket := (now / ActivityBucketMs) * ActivityBucketMs
		shift := (bucket - l.activityBucketStart) / ActivityBucketMs
		shift = max(0, min(shift, ActivityBuckets))
		for i := int64(0); i+shift < ActivityBuckets; i++ {
			calls[i] = l.activityCalls[i+shift]
			saved[i] = l.activitySaved[i+shift]
		}
	}
	totals.RecentCalls = calls
	totals.RecentSaved = saved
	return totals
}

func int64Array(values []int64) qtjson.Value {
	out := qtjson.NewArray()
	for _, value := range values {
		out.Append(qtjson.NewDouble(float64(value)))
	}
	return out
}

func (l *Ledger) ToJSON() qtjson.Value {
	return l.toJSON(false)
}

func (l *Ledger) SnapshotJSON() qtjson.Value {
	return l.toJSON(true)
}

func (l *Ledger) toJSON(withInFlight bool) qtjson.Value {
	familiesJSON := qtjson.NewObject()
	for key, s := range l.families {
		names := qtjson.NewObject()
		for name, calls := range s.RuntimeNames {
			names.Set(name, qtjson.NewDouble(float64(calls)))
		}
		row := qtjson.ObjectOf(
			"calls", float64(s.Calls),
			"errors", float64(s.Errors),
			"measuredCalls", float64(s.MeasuredCalls),
			"latencyMsTotal", float64(s.LatencyMsTotal),
			"latencyMsMax", float64(s.LatencyMsMax),
			"latencySamples", float64(s.LatencySamples),
			"inputTokens", float64(s.InputTokens),
			"resultTokens", float64(s.ResultTokens),
			"baselineTokens", float64(s.BaselineTokens),
			"savedTokens", float64(s.SavedTokens),
			"savedUsd", s.SavedUsd,
			"consumedUsd", s.ConsumedUsd,
			"lastCallMs", float64(s.LastCallMs),
			"callsWhileOff", float64(s.CallsWhileOff),
			"recentSaved", int64Array(s.RecentSaved),
			"runtimeNames", names,
		)
		if withInFlight {
			row.Set("inFlight", qtjson.NewDouble(float64(s.InFlight)))
		}
		familiesJSON.Set(key, row)
	}
	usage := qtjson.NewArray()
	for _, u := range l.Usage() {
		usage.Append(qtjson.ObjectOf(
			"tool", u.Tool,
			"command", u.Command,
			"family", u.Family,
			"calls", float64(u.Calls),
			"errors", float64(u.Errors),
			"latencyMsTotal", float64(u.LatencyMsTotal),
			"latencyMsMax", float64(u.LatencyMsMax),
			"latencySamples", float64(u.LatencySamples),
			"inputTokens", float64(u.InputTokens),
			"resultTokens", float64(u.ResultTokens),
			"savedTokens", float64(u.SavedTokens),
			"consumedUsd", u.ConsumedUsd,
			"lastCallMs", float64(u.LastCallMs),
		))
	}
	models := qtjson.NewArray()
	for _, m := range l.ModelUsage() {
		models.Append(qtjson.ObjectOf(
			"provider", m.Provider,
			"model", m.Model,
			"calls", float64(m.Calls),
			"errors", float64(m.Errors),
			"latencyMsTotal", float64(m.LatencyMsTotal),
			"latencySamples", float64(m.LatencySamples),
			"inputTokens", float64(m.InputTokens),
			"resultTokens", float64(m.ResultTokens),
			"savedTokens", float64(m.SavedTokens),
			"consumedUsd", m.ConsumedUsd,
			"lastCallMs", float64(m.LastCallMs),
		))
	}
	return qtjson.ObjectOf(
		"schema", ledgerSchemaVersion,
		"usage", usage,
		"models", models,
		"totalCalls", float64(l.totalCalls),
		"firstCallMs", float64(l.firstCallMs),
		"lastCallMs", float64(l.lastCallMs),
		"activity", qtjson.ObjectOf(
			"bucketStartMs", float64(l.activityBucketStart),
			"calls", int64Array(l.activityCalls),
			"saved", int64Array(l.activitySaved),
		),
		"families", familiesJSON,
	)
}

func qtDoubleToInt64(d float64) int64 {
	if math.IsNaN(d) {
		return 0
	}
	if d >= 9.2233720368547758e18 {
		return math.MaxInt64
	}
	if d < -9.2233720368547758e18 {
		return math.MinInt64
	}
	return int64(d)
}

func nonNegative(value qtjson.Value) int64 {
	if !value.IsNumber() {
		return 0
	}
	return max(0, qtDoubleToInt64(value.Float()))
}

func nonNegativeDouble(value qtjson.Value) float64 {
	return math.Max(0, value.Float())
}

func fromJSONArray(value qtjson.Value, limit int) ([]int64, bool) {
	if !value.IsArray() {
		return nil, false
	}
	parsed := []int64{}
	for _, item := range value.Items() {
		if !item.IsNumber() || item.Float() < 0 {
			return nil, false
		}
		parsed = append(parsed, qtDoubleToInt64(item.Float()))
	}
	if len(parsed) > limit {
		parsed = parsed[len(parsed)-limit:]
	}
	return parsed, true
}

var ledgerFamilyFields = []string{"calls", "errors", "measuredCalls", "latencyMsTotal", "latencyMsMax",
	"latencySamples", "inputTokens", "resultTokens", "baselineTokens", "savedTokens", "lastCallMs"}

func (l *Ledger) FromJSON(json qtjson.Value) error {
	if json.Get("schema").IntOr(0) != ledgerSchemaVersion {
		return errors.New("unsupported ledger schema")
	}
	familiesValue := json.Get("families")
	if !familiesValue.IsObject() {
		return errors.New("ledger families missing")
	}
	parsedFamilies := map[string]*FamilyStats{}
	for _, member := range familiesValue.Members() {
		if FamilyByID(member.Key) == nil {
			continue
		}
		if !member.Value.IsObject() {
			return errors.New("ledger family is not an object")
		}
		o := member.Value
		for _, key := range ledgerFamilyFields {
			v := o.Get(key)
			if !v.IsNumber() || v.Float() < 0 {
				return errors.New("ledger field " + key + " invalid")
			}
		}
		s := &FamilyStats{Family: member.Key, RuntimeNames: map[string]int64{}}
		s.Calls = nonNegative(o.Get("calls"))
		s.Errors = nonNegative(o.Get("errors"))
		s.MeasuredCalls = nonNegative(o.Get("measuredCalls"))
		s.LatencyMsTotal = nonNegative(o.Get("latencyMsTotal"))
		s.LatencyMsMax = nonNegative(o.Get("latencyMsMax"))
		s.LatencySamples = nonNegative(o.Get("latencySamples"))
		s.InputTokens = nonNegative(o.Get("inputTokens"))
		s.ResultTokens = nonNegative(o.Get("resultTokens"))
		s.BaselineTokens = nonNegative(o.Get("baselineTokens"))
		s.SavedTokens = nonNegative(o.Get("savedTokens"))
		s.SavedUsd = nonNegativeDouble(o.Get("savedUsd"))
		s.ConsumedUsd = nonNegativeDouble(o.Get("consumedUsd"))
		s.LastCallMs = nonNegative(o.Get("lastCallMs"))
		if off := o.Get("callsWhileOff"); !off.IsUndefined() {
			if !off.IsNumber() || off.Float() < 0 {
				return errors.New("ledger callsWhileOff invalid")
			}
			s.CallsWhileOff = nonNegative(off)
		}
		if o.Has("recentSaved") {
			recent, ok := fromJSONArray(o.Get("recentSaved"), RecentSamples)
			if !ok {
				return errors.New("ledger recentSaved invalid")
			}
			s.RecentSaved = recent
		}
		for _, name := range o.Get("runtimeNames").Members() {
			if name.Value.IsNumber() && name.Value.Float() > 0 {
				s.RuntimeNames[name.Key] = qtDoubleToInt64(name.Value.Float())
			}
		}
		parsedFamilies[member.Key] = s
	}
	activity := json.Get("activity")
	var calls, saved []int64
	var bucketStart int64
	if activity.IsObject() && activity.Len() > 0 {
		bucketStart = nonNegative(activity.Get("bucketStartMs"))
		var okCalls, okSaved bool
		calls, okCalls = fromJSONArray(activity.Get("calls"), ActivityBuckets)
		if okCalls {
			saved, okSaved = fromJSONArray(activity.Get("saved"), ActivityBuckets)
		}
		if !okCalls || !okSaved || len(calls) != ActivityBuckets || len(saved) != ActivityBuckets {
			return errors.New("ledger activity invalid")
		}
	}
	usage := map[string]*UsageStats{}
	for _, value := range json.Get("usage").Items() {
		u := &UsageStats{}
		u.Tool = left16(value.Get("tool").Str(), 200)
		u.Command = left16(value.Get("command").Str(), 80)
		u.Family = value.Get("family").Str()
		u.Calls = nonNegative(value.Get("calls"))
		if u.Tool == "" || u.Calls <= 0 {
			continue
		}
		u.Key = u.Tool
		if u.Command != "" {
			u.Key = u.Tool + usageKeySeparator + u.Command
		}
		u.Errors = min(u.Calls, nonNegative(value.Get("errors")))
		u.LatencyMsTotal = nonNegative(value.Get("latencyMsTotal"))
		u.LatencyMsMax = nonNegative(value.Get("latencyMsMax"))
		u.LatencySamples = nonNegative(value.Get("latencySamples"))
		u.InputTokens = nonNegative(value.Get("inputTokens"))
		u.ResultTokens = nonNegative(value.Get("resultTokens"))
		u.SavedTokens = nonNegative(value.Get("savedTokens"))
		u.ConsumedUsd = nonNegativeDouble(value.Get("consumedUsd"))
		u.LastCallMs = nonNegative(value.Get("lastCallMs"))
		usage[u.Key] = u
	}
	models := map[string]*ModelStats{}
	for _, value := range json.Get("models").Items() {
		m := &ModelStats{}
		m.Provider = left16(trimmed(value.Get("provider").Str()), 80)
		m.Model = left16(strings.ToLower(trimmed(value.Get("model").Str())), 200)
		m.Calls = nonNegative(value.Get("calls"))
		if m.Provider == "" || m.Calls <= 0 {
			continue
		}
		m.Errors = min(m.Calls, nonNegative(value.Get("errors")))
		m.LatencyMsTotal = nonNegative(value.Get("latencyMsTotal"))
		m.LatencySamples = nonNegative(value.Get("latencySamples"))
		m.InputTokens = nonNegative(value.Get("inputTokens"))
		m.ResultTokens = nonNegative(value.Get("resultTokens"))
		m.SavedTokens = nonNegative(value.Get("savedTokens"))
		m.ConsumedUsd = nonNegativeDouble(value.Get("consumedUsd"))
		m.LastCallMs = nonNegative(value.Get("lastCallMs"))
		models[ModelUsageKey(m.Provider, m.Model)] = m
	}
	l.families = parsedFamilies
	l.usage = usage
	l.models = models
	l.trimUsage()
	l.pending = map[string]*pendingCall{}
	l.activityCalls = calls
	l.activitySaved = saved
	l.activityBucketStart = bucketStart
	l.totalCalls = nonNegative(json.Get("totalCalls"))
	l.firstCallMs = nonNegative(json.Get("firstCallMs"))
	l.lastCallMs = nonNegative(json.Get("lastCallMs"))
	return nil
}

func (l *Ledger) Load() error {
	if l.path == "" {
		return errors.New("no ledger path")
	}
	data, err := os.ReadFile(l.path)
	if errors.Is(err, os.ErrNotExist) {
		return nil
	}
	if err != nil {
		return errors.New("cannot open " + l.path)
	}
	doc, err := qtjson.Parse(data)
	if err != nil || !doc.IsObject() {
		return errors.New("ledger is not valid JSON")
	}
	return l.FromJSON(doc)
}

func (l *Ledger) Encoded() []byte {
	return l.ToJSON().Compact()
}

func (l *Ledger) Clear() {
	l.families = map[string]*FamilyStats{}
	l.usage = map[string]*UsageStats{}
	l.models = map[string]*ModelStats{}
	l.pending = map[string]*pendingCall{}
	l.activityCalls = nil
	l.activitySaved = nil
	l.activityBucketStart = 0
	l.totalCalls = 0
	l.firstCallMs = 0
	l.lastCallMs = 0
	l.notify()
}

func qtUTF8(s string) string {
	if !strings.Contains(s, "\xed") {
		return s
	}
	var b strings.Builder
	for i := 0; i < len(s); {
		if s[i] == 0xED && i+2 < len(s) && s[i+1] >= 0xA0 && s[i+1] <= 0xBF {
			b.WriteByte('?')
			i += 3
			continue
		}
		b.WriteByte(s[i])
		i++
	}
	return b.String()
}
