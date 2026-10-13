package insights

import (
	"bufio"
	"crypto/sha1"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"time"

	"forkmesh.com/desktop-core/internal/qtjson"
)

const (
	KindRepeatedCall    = "repeated-call"
	KindWholeFileRead   = "whole-file-read"
	KindWholeFileEdit   = "whole-file-edit"
	KindOversizedResult = "oversized-result"
	KindToolError       = "tool-error"
	KindSlowCall        = "slow-call"
	KindCalledWhileOff  = "called-while-off"
	KindUncatalogued    = "uncatalogued-tool"
	KindAbandonedCall   = "abandoned-call"

	MaxSignals            = 200
	MaxSeenPerSession     = 256
	RepeatWindowMs        = 10 * 60 * 1000
	OversizedResultTokens = 8000
	WholeFileMinTokens    = 3000
	WholeEditMinTokens    = 1000
	SlowCallMs            = 60 * 1000
	RotateBytes           = 2 * 1024 * 1024
	MaxAgeMs              = 7 * 24 * 60 * 60 * 1000

	improvementSchemaVersion = 1
	maxDetailChars           = 160
)

type Improvement struct {
	Key           string
	Kind          string
	Family        string
	RuntimeName   string
	Signature     string
	Topic         string
	Detail        string
	Suggestion    string
	Occurrences   int64
	TokensAtStake int64
	FirstMs       int64
	LastMs        int64
}

type occurrence struct {
	kind        string
	family      string
	runtimeName string
	signature   string
	topic       string
	detail      string
	suggestion  string
	provider    string
	model       string
	sessionID   int
	tokens      int64
	ms          int64
}

type ImprovementLog struct {
	path        string
	signals     []Improvement
	index       map[string]int
	seen        map[int]map[[20]byte]int64
	occurrences int64

	Changed func()
}

func NewImprovementLog(path string) *ImprovementLog {
	return &ImprovementLog{path: path, index: map[string]int{}, seen: map[int]map[[20]byte]int64{}}
}

func (g *ImprovementLog) Path() string { return g.path }

func (g *ImprovementLog) SentLogPath() string {
	if g.path == "" {
		return ""
	}
	return g.path + ".sent"
}

func (g *ImprovementLog) SignalCount() int { return len(g.signals) }

func (g *ImprovementLog) OccurrenceCount() int64 { return g.occurrences }

func KindLabel(kind string) string {
	switch kind {
	case KindRepeatedCall:
		return "Repeated call"
	case KindWholeFileRead:
		return "Whole-file read"
	case KindWholeFileEdit:
		return "Edit rewrites most of the file"
	case KindOversizedResult:
		return "Oversized result"
	case KindToolError:
		return "Tool error"
	case KindSlowCall:
		return "Slow call"
	case KindCalledWhileOff:
		return "Used while switched off"
	case KindUncatalogued:
		return "Uncatalogued tool"
	case KindAbandonedCall:
		return "Unanswered call"
	}
	return kind
}

func KindIsFailure(kind string) bool {
	return kind == KindToolError || kind == KindCalledWhileOff || kind == KindAbandonedCall
}

func tokensText(tokens int64) string {
	if tokens < 10000 {
		return strconv.FormatInt(tokens, 10)
	}
	if tokens < 1000000 {
		precision := 0
		if tokens < 100000 {
			precision = 1
		}
		return strconv.FormatFloat(float64(tokens)/1000.0, 'f', precision, 64) + "K"
	}
	return strconv.FormatFloat(float64(tokens)/1000000.0, 'f', 2, 64) + "M"
}

func oneLine(text string, maxChars int) string {
	line := ""
	for _, part := range strings.Split(text, "\n") {
		if t := trimmed(part); t != "" {
			line = t
			break
		}
	}
	line = toolUseErrorTag.ReplaceAllLiteralString(line, "")
	line = simplified(line)
	if u16len(line) > maxChars {
		line = left16(line, maxChars-1) + "…"
	}
	return line
}

var (
	signaturePathRe   = regexp.MustCompile(`(?:~|\.{0,2})/[^\t\n\v\f\r :,;)]+`)
	signatureDigitsRe = regexp.MustCompile(`\d+`)
)

func replaceQuoted(s string) string {
	var b strings.Builder
	for i := 0; i < len(s); {
		c := s[i]
		if c == '"' || c == '\'' || c == '`' {
			rest := s[i+1:]
			newline := strings.IndexByte(rest, '\n')
			scope := rest
			if newline >= 0 {
				scope = rest[:newline]
			}
			if close := strings.IndexByte(scope, c); close >= 0 {
				b.WriteString("…")
				i += 1 + close + 1
				continue
			}
		}
		b.WriteByte(c)
		i++
	}
	return b.String()
}

func errorSignature(line string) string {
	signature := strings.ToLower(line)
	signature = replaceQuoted(signature)
	signature = signaturePathRe.ReplaceAllLiteralString(signature, "<path>")
	signature = signatureDigitsRe.ReplaceAllLiteralString(signature, "#")
	return left16(signature, 80)
}

func cleanForRelative(path string) []string {
	parts := []string{}
	for _, part := range strings.Split(filepath.ToSlash(filepath.Clean(path)), "/") {
		if part != "" {
			parts = append(parts, part)
		}
	}
	return parts
}

func relativeFilePath(dir, file string) string {
	if !qtIsAbsolute(file) || !qtIsAbsolute(dir) {
		return file
	}
	dirParts := cleanForRelative(dir)
	fileParts := cleanForRelative(file)
	i := 0
	for i < len(dirParts) && i < len(fileParts) && dirParts[i] == fileParts[i] {
		i++
	}
	result := strings.Repeat("../", len(dirParts)-i)
	if i < len(fileParts) {
		result += strings.Join(fileParts[i:], "/")
	}
	if result == "" {
		return "."
	}
	return result
}

func subjectOf(call Observation) string {
	input := call.Input
	for _, key := range []string{"file_path", "path", "notebook_path"} {
		path := trimmed(input.Get(key).Str())
		if path == "" {
			continue
		}
		if call.Workdir != "" && qtIsAbsolute(path) {
			if relative := relativeFilePath(call.Workdir, path); !strings.HasPrefix(relative, "..") {
				path = relative
			}
		}
		if pattern := input.Get("pattern").Str(); pattern != "" {
			return oneLine(pattern+" in "+path, maxDetailChars)
		}
		return oneLine(path, maxDetailChars)
	}
	for _, key := range []string{"pattern", "command", "cmd", "url", "query", "description"} {
		if value := input.Get(key).Str(); trimmed(value) != "" {
			return oneLine(value, 120)
		}
	}
	return ""
}

func orDefault(value, fallback string) string {
	if value == "" {
		return fallback
	}
	return value
}

func (g *ImprovementLog) Observe(call Observation) int {
	ms := call.CompletedMs
	if ms <= 0 {
		ms = nowMs()
	}
	subject := subjectOf(call)
	found := []occurrence{}
	add := func(kind string, tokens int64, detail, suggestion, signature, topic string) {
		found = append(found, occurrence{kind: kind, family: call.Family, runtimeName: call.RuntimeName,
			signature: signature, topic: topic, detail: detail, suggestion: suggestion, provider: call.Provider,
			model: call.Model, sessionID: call.SessionID, tokens: max(0, tokens), ms: ms})
	}
	if call.WhileOff {
		add(KindCalledWhileOff, call.InputTokens+call.ResultTokens, orDefault(subject, call.RuntimeName),
			"The runtime still reached "+orDefault(call.RuntimeName, call.Family)+
				" after the family was switched off; its enforcement needs to cover this tool name.", "", "")
	}
	if call.Family == "other" {
		add(KindUncatalogued, 0, call.RuntimeName,
			"Map \""+call.RuntimeName+"\" to a tool family in AgentToolCatalog so its cost is attributed and it can be switched off.",
			"", "")
	}
	command := ""
	if call.Family == "bash" {
		command = unwrapShellLauncher(call.Input.Get("command").Str())
	}
	switch {
	case call.Abandoned:
		shape := ""
		if command != "" {
			shape = slowCommandShape(command)
		}
		add(KindAbandonedCall, call.InputTokens, subject,
			"The turn ended (stopped, interrupted, or crashed) while this call was still running, so its result never reached the transcript; keep work this long in the background so a stop does not lose it.",
			shape, shape)
	case call.IsError && command != "":
		failure := classifyShellFailure(command, call.ResultText)
		if failure.isResult() {
			if call.ResultTokens >= OversizedResultTokens {
				topic := oversizedShellTopic(command)
				add(KindOversizedResult, call.ResultTokens,
					subject+" — "+tokensText(call.ResultTokens)+" tokens returned",
					oversizedShellSuggestion(command), topic, topic)
			}
		} else {
			what := ""
			if failure.exitCode >= 0 {
				what = "exit " + strconv.Itoa(failure.exitCode)
			}
			if failure.message != "" {
				if what != "" {
					what += ": "
				}
				what += oneLine(failure.message, maxDetailChars)
			}
			topic := shellFailureTopic(failure)
			signature := topic
			if failure.kind == failureOther {
				signature = topic + "\x1f" + errorSignature(failure.message)
			}
			detail := subject
			if what != "" {
				detail = subject + " — " + what
			}
			add(KindToolError, call.InputTokens+call.ResultTokens, detail, shellFailureSuggestion(failure), signature, topic)
		}
	case call.IsError:
		line := oneLine(call.ResultText, maxDetailChars)
		detail := line
		if subject != "" {
			detail = subject + " — " + line
		}
		lowered := strings.ToLower(call.ResultText)
		signature, topic, suggestion := errorSignature(line), "", genericFailureSuggestion
		if strings.Contains(lowered, "while in plan mode") {
			signature, topic = "refused in plan mode", "refused in plan mode"
			suggestion = "Plan mode refuses every file change; keep to reading and planning, and present the plan with ExitPlanMode before the first Edit or Write."
		} else if strings.Contains(lowered, "file does not exist") {
			signature, topic = "guessed path", "guessed path"
			suggestion = "The path was guessed; Glob the file name (or rg --files | rg name) first and Read the absolute path it returns."
		} else if strings.Contains(lowered, "has not been read yet") || strings.Contains(lowered, "modified since read") {
			signature, topic = "write before read", "write before read"
			suggestion = "Write and Edit refuse a file not Read in this conversation, and the whole new content is paid for again; Read a range of an existing file first and change it with Edit, and Write only new files."
		} else if strings.Contains(lowered, "concurrent subagent limit") {
			signature, topic = "subagent limit", "subagent limit"
			suggestion = "The runtime caps concurrent subagents; launch at most ten at once, fold small audits into one subagent, and start the next batch when earlier ones report back."
		}
		add(KindToolError, call.InputTokens+call.ResultTokens, detail, suggestion, signature, topic)
	default:
		wholeFile := false
		if call.Measured && call.BaselineTokens > 0 {
			floor := int64(WholeFileMinTokens)
			if call.OutputSide {
				floor = WholeEditMinTokens
			}
			if call.BaselineTokens >= floor && call.ActualTokens*10 >= call.BaselineTokens*9 {
				wholeFile = true
				if call.OutputSide {
					add(KindWholeFileEdit, call.ActualTokens, subject,
						"Emit small anchored edits around the changed lines instead of replacing most of the file.", "", "")
				} else {
					detail := subject + " — " + tokensText(call.ActualTokens) + " of " + tokensText(call.BaselineTokens) + " tokens"
					if call.ActualTokens >= call.BaselineTokens {
						detail = subject + " — whole file, ~" + tokensText(call.ActualTokens) + " tokens"
					}
					add(KindWholeFileRead, call.ActualTokens, detail,
						"Search for the symbol first, then read a line range (offset/limit) instead of the whole file.", "", "")
				}
			}
		}
		if !wholeFile && call.ResultTokens >= OversizedResultTokens {
			suggestion := oversizedSuggestion(call.Family)
			if command != "" {
				suggestion = oversizedShellSuggestion(command)
			}
			topic := oversizedShellTopic(command)
			add(KindOversizedResult, call.ResultTokens,
				orDefault(subject, call.RuntimeName)+" — "+tokensText(call.ResultTokens)+" tokens returned",
				suggestion, topic, topic)
		}
		if g.noteRepeat(call) {
			add(KindRepeatedCall, call.ResultTokens, subject,
				"The same call already ran this turn; reuse the earlier result instead of paying for it again.", "", "")
		}
	}
	latency := call.CompletedMs - call.StartedMs
	if !call.Abandoned && !call.HeldBehindSibling && call.StartedMs > 0 && latency >= SlowCallMs &&
		!longRunningFamily(call.Family) {
		shape := ""
		if command != "" {
			shape = slowCommandShape(command)
		}
		suggestion := "Run long work in the background or bound it with a timeout so the turn is not blocked."
		if shape == "foreground wait" {
			suggestion = "A foreground poll loop holds the turn for its whole bound, and a timeout bash -c wrapper only slips past the sleep guard; run the until-loop with Bash run_in_background (one notice when it ends) or Monitor, and keep working meanwhile."
		} else if command != "" {
			suggestion = "A foreground command this long blocks the turn and risks the harness timeout; run it with run_in_background, tee its output to a file, and wait with Monitor."
		}
		add(KindSlowCall, 0, orDefault(subject, call.RuntimeName)+" — "+strconv.FormatInt(latency/1000, 10)+" s",
			suggestion, shape, shape)
	}
	for _, item := range found {
		g.record(item, true)
	}
	if len(found) > 0 && g.Changed != nil {
		g.Changed()
	}
	return len(found)
}

func (g *ImprovementLog) noteRepeat(call Observation) bool {
	if mutatingFamily(call.Family) {
		delete(g.seen, call.SessionID)
		return false
	}
	if !readOnlyFamily(call.Family) {
		return false
	}
	input := call.Input
	if !input.IsObject() {
		input = qtjson.NewObject()
	}
	hash := sha1.New()
	hash.Write([]byte(qtUTF8(call.RuntimeName)))
	hash.Write([]byte{0x1f})
	hash.Write(input.Compact())
	var fingerprint [20]byte
	copy(fingerprint[:], hash.Sum(nil))
	seen := g.seen[call.SessionID]
	if seen == nil {
		seen = map[[20]byte]int64{}
		g.seen[call.SessionID] = seen
	}
	previous, had := seen[fingerprint]
	repeat := had && call.CompletedMs-previous <= RepeatWindowMs
	if _, present := seen[fingerprint]; len(seen) >= MaxSeenPerSession && !present {
		for key, finished := range seen {
			if call.CompletedMs-finished > RepeatWindowMs {
				delete(seen, key)
			}
		}
		if len(seen) >= MaxSeenPerSession {
			for key := range seen {
				delete(seen, key)
			}
		}
	}
	seen[fingerprint] = call.CompletedMs
	return repeat
}

func (g *ImprovementLog) ForgetSession(sessionID int) {
	delete(g.seen, sessionID)
}

func improvementKey(o occurrence) string {
	return strings.Join([]string{o.kind, o.family, o.runtimeName, o.signature}, "\x1f")
}

func (g *ImprovementLog) record(o occurrence, appendLine bool) {
	key := improvementKey(o)
	position, ok := g.index[key]
	if !ok {
		if len(g.signals) >= MaxSignals {
			stalest := 0
			for i := range g.signals {
				if g.signals[i].LastMs < g.signals[stalest].LastMs {
					stalest = i
				}
			}
			g.signals = append(g.signals[:stalest], g.signals[stalest+1:]...)
			g.index = map[string]int{}
			for i, signal := range g.signals {
				g.index[signal.Key] = i
			}
		}
		g.signals = append(g.signals, Improvement{Key: key, Kind: o.kind, Family: o.family, RuntimeName: o.runtimeName,
			Signature: o.signature, FirstMs: o.ms})
		position = len(g.signals) - 1
		g.index[key] = position
	}
	signal := &g.signals[position]
	signal.Occurrences++
	signal.TokensAtStake += o.tokens
	signal.FirstMs = min(signal.FirstMs, o.ms)
	if o.ms >= signal.LastMs {
		signal.LastMs = o.ms
		signal.Detail = o.detail
		signal.Suggestion = o.suggestion
		signal.Topic = o.topic
	}
	g.occurrences++
	if appendLine {
		g.appendLine(o)
	}
}

func (g *ImprovementLog) appendLine(o occurrence) {
	if g.path == "" {
		return
	}
	info, err := os.Stat(g.path)
	if err == nil && info.Size() >= RotateBytes {
		rotated := g.path + ".1"
		_ = os.Remove(rotated)
		_ = os.Rename(g.path, rotated)
	} else if err != nil {
		_ = os.MkdirAll(filepath.Dir(g.path), 0o755)
	}
	file, err := os.OpenFile(g.path, os.O_WRONLY|os.O_APPEND|os.O_CREATE, 0o644)
	if err != nil {
		return
	}
	defer file.Close()
	line := qtjson.ObjectOf(
		"schema", improvementSchemaVersion,
		"ms", float64(o.ms),
		"kind", o.kind,
		"family", o.family,
		"tool", o.runtimeName,
		"signature", o.signature,
		"session", o.sessionID,
		"provider", o.provider,
		"model", o.model,
		"tokens", float64(o.tokens),
		"detail", o.detail,
		"suggestion", o.suggestion,
		"topic", o.topic,
	)
	_, _ = file.Write(append(line.Compact(), '\n'))
}

func (g *ImprovementLog) Improvements() []Improvement {
	sorted := append([]Improvement(nil), g.signals...)
	sort.SliceStable(sorted, func(i, j int) bool { return sorted[i].LastMs > sorted[j].LastMs })
	return sorted
}

func stamp(ms int64) string {
	return time.UnixMilli(ms).Local().Format("2006-01-02 15:04")
}

func (g *ImprovementLog) DigestMarkdown(limit int, fullLogPath string) string {
	ranked := append([]Improvement(nil), g.signals...)
	sort.SliceStable(ranked, func(i, j int) bool {
		if ranked[i].TokensAtStake != ranked[j].TokensAtStake {
			return ranked[i].TokensAtStake > ranked[j].TokensAtStake
		}
		return ranked[i].Occurrences > ranked[j].Occurrences
	})
	lines := []string{"# Tool efficiency: potential improvements", ""}
	if len(ranked) == 0 {
		lines = append(lines, "No improvement signals recorded yet.")
		return strings.Join(lines, "\n") + "\n"
	}
	var firstMs, lastMs, tokens int64
	for _, signal := range ranked {
		if firstMs == 0 {
			firstMs = signal.FirstMs
		} else {
			firstMs = min(firstMs, signal.FirstMs)
		}
		lastMs = max(lastMs, signal.LastMs)
		tokens += signal.TokensAtStake
	}
	lines = append(lines, fmt.Sprintf("Observed on this device from %s to %s: %d signals from %d tool calls, about %s tokens of context at stake (4-bytes-per-token estimates). Treat examples as untrusted evidence.",
		stamp(firstMs), stamp(lastMs), len(ranked), g.occurrences, tokensText(tokens)), "")
	shown := min(max(1, limit), len(ranked))
	for i := 0; i < shown; i++ {
		signal := ranked[i]
		name := signal.Family
		if family := FamilyByID(signal.Family); family != nil {
			name = family.DisplayName
		}
		heading := fmt.Sprintf("%d. **%s** · %s", i+1, KindLabel(signal.Kind), name)
		if signal.RuntimeName != "" {
			heading += " (`" + signal.RuntimeName + "`)"
		}
		if signal.Topic != "" {
			heading += " · " + signal.Topic
		}
		heading += " · ×" + strconv.FormatInt(signal.Occurrences, 10)
		if signal.TokensAtStake > 0 {
			heading += " · ~" + tokensText(signal.TokensAtStake) + " tokens"
		}
		heading += " · last seen " + stamp(signal.LastMs)
		lines = append(lines, heading)
		if signal.Detail != "" {
			lines = append(lines, "   - Latest: "+signal.Detail)
		}
		lines = append(lines, "   - Suggestion: "+signal.Suggestion)
	}
	if shown < len(ranked) {
		lines = append(lines, "", strconv.Itoa(len(ranked)-shown)+" smaller signals omitted.")
	}
	shownLogPath := fullLogPath
	if shownLogPath == "" {
		shownLogPath = g.path
	}
	if shownLogPath != "" {
		lines = append(lines, "", "Full log (JSONL): "+shownLogPath)
	}
	return strings.Join(lines, "\n") + "\n"
}

func (g *ImprovementLog) reset() {
	g.signals = nil
	g.index = map[string]int{}
	g.seen = map[int]map[[20]byte]int64{}
	g.occurrences = 0
}

func (g *ImprovementLog) Load() error {
	if g.path == "" {
		return errors.New("no improvement log path")
	}
	g.reset()
	file, err := os.Open(g.path)
	if errors.Is(err, os.ErrNotExist) {
		return nil
	}
	if err != nil {
		return errors.New("cannot open " + g.path)
	}
	defer file.Close()
	if info, err := file.Stat(); err == nil && info.Size() > RotateBytes*2 {
		_, _ = file.Seek(info.Size()-RotateBytes*2, io.SeekStart)
	}
	reader := bufio.NewReaderSize(file, 64*1024)
	loaded := []occurrence{}
	var newestMs int64
	for {
		raw, err := reader.ReadBytes('\n')
		if len(raw) > 0 {
			if o, ok := parseOccurrence(raw); ok {
				newestMs = max(newestMs, o.ms)
				loaded = append(loaded, o)
			}
		}
		if err != nil {
			break
		}
	}
	for _, o := range loaded {
		if o.ms >= newestMs-MaxAgeMs {
			g.record(o, false)
		}
	}
	return nil
}

func parseOccurrence(raw []byte) (occurrence, bool) {
	text := strings.TrimFunc(string(raw), asciiSpace)
	if text == "" {
		return occurrence{}, false
	}
	line, err := qtjson.Parse([]byte(text))
	if err != nil || !line.IsObject() {
		line = qtjson.NewObject()
	}
	kind := line.Get("kind").Str()
	if line.Get("schema").IntOr(0) != improvementSchemaVersion || KindLabel(kind) == kind || !line.Get("ms").IsNumber() {
		return occurrence{}, false
	}
	return occurrence{
		kind:        kind,
		family:      line.Get("family").Str(),
		runtimeName: line.Get("tool").Str(),
		signature:   line.Get("signature").Str(),
		detail:      line.Get("detail").Str(),
		suggestion:  line.Get("suggestion").Str(),
		topic:       line.Get("topic").Str(),
		sessionID:   line.Get("session").IntOr(0),
		tokens:      max(0, qtDoubleToInt64(line.Get("tokens").Float())),
		ms:          qtDoubleToInt64(line.Get("ms").Float()),
	}, true
}

func (g *ImprovementLog) Clear() {
	g.reset()
	if g.path != "" {
		_ = os.Remove(g.path)
		_ = os.Remove(g.path + ".1")
	}
	if g.Changed != nil {
		g.Changed()
	}
}

func (g *ImprovementLog) ArchiveSent() bool {
	if g.path == "" {
		return false
	}
	sent := g.SentLogPath()
	_ = os.Remove(sent)
	moved := true
	if _, err := os.Stat(g.path); err == nil {
		moved = os.Rename(g.path, sent) == nil
	}
	if !moved {
		return false
	}
	_ = os.Remove(g.path + ".1")
	g.reset()
	if g.Changed != nil {
		g.Changed()
	}
	return true
}

func (g *ImprovementLog) SignalsJSON() qtjson.Value {
	out := qtjson.NewArray()
	for _, signal := range g.signals {
		out.Append(qtjson.ObjectOf(
			"key", signal.Key,
			"kind", signal.Kind,
			"family", signal.Family,
			"tool", signal.RuntimeName,
			"signature", signal.Signature,
			"topic", signal.Topic,
			"detail", signal.Detail,
			"suggestion", signal.Suggestion,
			"occurrences", float64(signal.Occurrences),
			"tokensAtStake", float64(signal.TokensAtStake),
			"firstMs", float64(signal.FirstMs),
			"lastMs", float64(signal.LastMs),
		))
	}
	return out
}
