package insights

import (
	"encoding/json"
	"math"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"testing"
	"time"

	"forkmesh.com/desktop-core/internal/qtjson"
)

type goldenFile struct {
	Ledger      json.RawMessage `json:"ledger"`
	Loads       json.RawMessage `json:"loads"`
	Guardrails  json.RawMessage `json:"guardrails"`
	Shapes      json.RawMessage `json:"shapes"`
	FamilyFor   json.RawMessage `json:"familyFor"`
	Policy      json.RawMessage `json:"policy"`
	Directories json.RawMessage `json:"directories"`
	Helpers     json.RawMessage `json:"helpers"`
}

func loadGolden(t *testing.T) goldenFile {
	t.Helper()
	data, err := os.ReadFile(filepath.Join("testdata", "golden.json"))
	if err != nil {
		t.Fatal(err)
	}
	var golden goldenFile
	if err := json.Unmarshal(data, &golden); err != nil {
		t.Fatal(err)
	}
	return golden
}

func decode(t *testing.T, raw json.RawMessage, into any) {
	t.Helper()
	if err := json.Unmarshal(raw, into); err != nil {
		t.Fatal(err)
	}
}

var fillPattern = regexp.MustCompile(`@FILL:(.):(\d+)@`)

func expandFill(text string) string {
	return fillPattern.ReplaceAllStringFunc(text, func(match string) string {
		parts := fillPattern.FindStringSubmatch(match)
		count, _ := strconv.Atoi(parts[2])
		return strings.Repeat(parts[1], count)
	})
}

func substituteValue(value qtjson.Value, workdir string) qtjson.Value {
	switch value.Kind() {
	case qtjson.String:
		return qtjson.NewString(expandFill(strings.ReplaceAll(value.Str(), "@W@", workdir)))
	case qtjson.Array:
		out := qtjson.NewArray()
		for _, item := range value.Items() {
			out.Append(substituteValue(item, workdir))
		}
		return out
	case qtjson.Object:
		out := qtjson.NewObject()
		for _, member := range value.Members() {
			out.Set(member.Key, substituteValue(member.Value, workdir))
		}
		return out
	}
	return value
}

func canonicalLedger(t *testing.T, text string) string {
	t.Helper()
	value, err := qtjson.Parse([]byte(text))
	if err != nil {
		t.Fatalf("parse %q: %v", text, err)
	}
	for _, key := range []string{"usage", "models"} {
		items := append([]qtjson.Value(nil), value.Get(key).Items()...)
		sort.SliceStable(items, func(i, j int) bool {
			a, b := items[i], items[j]
			ka := a.Get("tool").Str() + "\x00" + a.Get("command").Str() + "\x00" + a.Get("provider").Str() + "\x00" + a.Get("model").Str()
			kb := b.Get("tool").Str() + "\x00" + b.Get("command").Str() + "\x00" + b.Get("provider").Str() + "\x00" + b.Get("model").Str()
			return ka < kb
		})
		if value.Has(key) {
			value.Set(key, qtjson.NewArray(items...))
		}
	}
	return string(value.Compact())
}

func jsonOf(t *testing.T, value any) any {
	t.Helper()
	data, err := json.Marshal(value)
	if err != nil {
		t.Fatal(err)
	}
	var out any
	if err := json.Unmarshal(data, &out); err != nil {
		t.Fatal(err)
	}
	return out
}

func near(a, b float64) bool {
	if a == b {
		return true
	}
	return math.Abs(a-b) <= 1e-12*math.Max(math.Abs(a), math.Abs(b))
}

func sameJSON(a, b any) bool {
	switch av := a.(type) {
	case map[string]any:
		bv, ok := b.(map[string]any)
		if !ok || len(av) != len(bv) {
			return false
		}
		for key, value := range av {
			if !sameJSON(value, bv[key]) {
				return false
			}
		}
		return true
	case []any:
		bv, ok := b.([]any)
		if !ok || len(av) != len(bv) {
			return false
		}
		for i := range av {
			if !sameJSON(av[i], bv[i]) {
				return false
			}
		}
		return true
	case float64:
		bv, ok := b.(float64)
		return ok && near(av, bv)
	}
	return a == b
}

func expectJSON(t *testing.T, what string, got any, want any) {
	t.Helper()
	g := jsonOf(t, got)
	w := jsonOf(t, want)
	if !sameJSON(g, w) {
		gb, _ := json.Marshal(g)
		wb, _ := json.Marshal(w)
		t.Errorf("%s:\n got %s\nwant %s", what, gb, wb)
	}
}

func scrubbed(text, workdir string) string {
	return strings.ReplaceAll(strings.ReplaceAll(text, workdir, "<W>"), filepath.Dir(workdir), "<R>")
}

func TestGoldenLedgerScenario(t *testing.T) {
	time.Local = time.UTC
	var golden struct {
		WorkdirLength int `json:"workdirLength"`
		Files         []struct {
			Path string `json:"path"`
			Size int    `json:"size"`
		} `json:"files"`
		Ops          json.RawMessage            `json:"ops"`
		Changed      []any                      `json:"changed"`
		End          float64                    `json:"end"`
		Ledger       string                     `json:"ledger"`
		Totals       map[string]any             `json:"totals"`
		Families     map[string]any             `json:"families"`
		Usage        map[string]map[string]any  `json:"usage"`
		Models       map[string]map[string]any  `json:"models"`
		Log          string                     `json:"log"`
		Improvements []string                   `json:"improvements"`
		Occurrences  float64                    `json:"occurrences"`
		Digest       string                     `json:"digest"`
		DigestAll    string                     `json:"digestAll"`
		Reloaded     []string                   `json:"reloaded"`
		Loaded       string                     `json:"loaded"`
		Extra        map[string]json.RawMessage `json:"-"`
	}
	decode(t, loadGolden(t).Ledger, &golden)
	root := t.TempDir()
	if len(root)+1 >= golden.WorkdirLength {
		t.Fatalf("temp dir %s too long for a %d-byte workdir", root, golden.WorkdirLength)
	}
	workdir := filepath.Join(root, strings.Repeat("w", golden.WorkdirLength-len(root)-1))
	for _, file := range golden.Files {
		path := filepath.Join(workdir, file.Path)
		if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(path, []byte(strings.Repeat("q", file.Size)), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	ops, err := qtjson.Parse(golden.Ops)
	if err != nil {
		t.Fatal(err)
	}
	ledgerPath := filepath.Join(root, "tool-efficiency.json")
	logPath := filepath.Join(root, "tool-improvements.jsonl")
	ledger := NewLedger(ledgerPath)
	log := NewImprovementLog(logPath)
	ledger.CallObserved = func(call Observation) { log.Observe(call) }
	changes := []any{}
	for _, o := range ops.Items() {
		if o.Get("op").Str() == "disabled" {
			families := []string{}
			for _, family := range o.Get("families").Items() {
				families = append(families, family.Str())
			}
			ledger.SetDisabledFamilies(families)
			continue
		}
		event := substituteValue(o.Get("event"), workdir)
		session := o.Get("session").Int()
		now := o.Get("now").Int64()
		kind := event.Get("type").Str()
		if kind == "result" || kind == "_codex_agent_complete" {
			ledger.AbandonSession(session, now)
			log.ForgetSession(session)
			changes = append(changes, nil)
			continue
		}
		context := SessionContext{SessionID: session, Provider: o.Get("provider").Str(), Model: o.Get("model").Str()}
		if o.Get("workdir").BoolValue() {
			context.Workdir = workdir
		}
		changes = append(changes, ledger.Ingest(context, event, now))
	}
	expectJSON(t, "changed", changes, golden.Changed)
	if got, want := canonicalLedger(t, scrubbed(string(ledger.ToJSON().Compact()), workdir)), canonicalLedger(t, golden.Ledger); got != want {
		t.Errorf("ledger json:\n got %s\nwant %s", got, want)
	}
	end := int64(golden.End)
	expectJSON(t, "totals", totalsRow(ledger.Totals(end)), golden.Totals)
	families := map[string]any{}
	disabled := map[string]bool{}
	for _, family := range ledger.DisabledFamilies() {
		disabled[family] = true
	}
	for _, stats := range ledger.AllStats() {
		families[stats.Family] = familyRow(stats, FamilyByID(stats.Family), disabled[stats.Family], EnforcementNote(stats.Family))
	}
	expectJSON(t, "families", families, golden.Families)
	usage := map[string]any{}
	for _, u := range ledger.Usage() {
		row := usageRow(u)
		row["command"] = u.Command
		usage[u.Key] = row
	}
	expectJSON(t, "usage", usage, golden.Usage)
	models := map[string]any{}
	for _, m := range ledger.ModelUsage() {
		models[ModelUsageKey(m.Provider, m.Model)] = map[string]any{"calls": float64(m.Calls), "errors": float64(m.Errors),
			"consumedUsd": m.ConsumedUsd, "savedTokens": float64(m.SavedTokens), "latencyMsTotal": float64(m.LatencyMsTotal)}
	}
	expectJSON(t, "models", models, golden.Models)
	logData, _ := os.ReadFile(logPath)
	gotLines := strings.Split(scrubbed(string(logData), workdir), "\n")
	wantLines := strings.Split(golden.Log, "\n")
	if len(gotLines) != len(wantLines) {
		t.Errorf("log lines: got %d want %d", len(gotLines), len(wantLines))
	}
	for i := 0; i < len(gotLines) && i < len(wantLines); i++ {
		if gotLines[i] != wantLines[i] {
			t.Errorf("log line %d:\n got %s\nwant %s", i, gotLines[i], wantLines[i])
		}
	}
	improvements := []string{}
	for _, item := range log.Improvements() {
		row := improvementRow(item)
		row["signature"] = item.Signature
		row["firstMs"] = float64(item.FirstMs)
		improvements = append(improvements, scrubbed(string(qtjson.From(row).Compact()), workdir))
	}
	expectJSON(t, "improvements", improvements, golden.Improvements)
	if float64(log.OccurrenceCount()) != golden.Occurrences {
		t.Errorf("occurrences %d want %v", log.OccurrenceCount(), golden.Occurrences)
	}
	if got := scrubbed(log.DigestMarkdown(5, ""), workdir); got != golden.Digest {
		t.Errorf("digest:\n got %q\nwant %q", got, golden.Digest)
	}
	if got := scrubbed(log.DigestMarkdown(100, "/x/sent.jsonl"), workdir); got != golden.DigestAll {
		t.Errorf("digestAll:\n got %q\nwant %q", got, golden.DigestAll)
	}
	reloaded := NewImprovementLog(logPath)
	if err := reloaded.Load(); err != nil {
		t.Fatal(err)
	}
	reloadedRows := []string{}
	for _, item := range reloaded.Improvements() {
		reloadedRows = append(reloadedRows, scrubbed(string(qtjson.From(improvementRow(item)).Compact()), workdir))
	}
	expectJSON(t, "reloaded", reloadedRows, golden.Reloaded)
	if err := writeAtomic(ledgerPath, ledger.Encoded()); err != nil {
		t.Fatal(err)
	}
	loaded := NewLedger(ledgerPath)
	if err := loaded.Load(); err != nil {
		t.Fatal(err)
	}
	if got, want := canonicalLedger(t, scrubbed(string(loaded.ToJSON().Compact()), workdir)), canonicalLedger(t, golden.Loaded); got != want {
		t.Errorf("loaded ledger:\n got %s\nwant %s", got, want)
	}
}

func TestGoldenLoads(t *testing.T) {
	time.Local = time.UTC
	var golden struct {
		Ledger []struct {
			Input  string `json:"input"`
			OK     bool   `json:"ok"`
			Error  string `json:"error"`
			Output string `json:"output"`
		} `json:"ledger"`
		LogLines       []string `json:"logLines"`
		LogRows        []any    `json:"logRows"`
		LogOccurrences float64  `json:"logOccurrences"`
	}
	decode(t, loadGolden(t).Loads, &golden)
	root := t.TempDir()
	for i, c := range golden.Ledger {
		path := filepath.Join(root, "load-"+strconv.Itoa(i)+".json")
		if err := os.WriteFile(path, []byte(c.Input), 0o644); err != nil {
			t.Fatal(err)
		}
		ledger := NewLedger(path)
		err := ledger.Load()
		if (err == nil) != c.OK {
			t.Errorf("case %d ok=%v want %v (%v)", i, err == nil, c.OK, err)
		}
		if err != nil && err.Error() != c.Error {
			t.Errorf("case %d error %q want %q", i, err.Error(), c.Error)
		}
		if got, want := canonicalLedger(t, string(ledger.ToJSON().Compact())), canonicalLedger(t, c.Output); got != want {
			t.Errorf("case %d output:\n got %s\nwant %s", i, got, want)
		}
	}
	logPath := filepath.Join(root, "load.jsonl")
	if err := os.WriteFile(logPath, []byte(strings.Join(golden.LogLines, "\n")), 0o644); err != nil {
		t.Fatal(err)
	}
	log := NewImprovementLog(logPath)
	if err := log.Load(); err != nil {
		t.Fatal(err)
	}
	rows := []any{}
	for _, item := range log.Improvements() {
		row := improvementRow(item)
		row["firstMs"] = float64(item.FirstMs)
		rows = append(rows, row)
	}
	expectJSON(t, "log rows", rows, golden.LogRows)
	if float64(log.OccurrenceCount()) != golden.LogOccurrences {
		t.Errorf("log occurrences %d want %v", log.OccurrenceCount(), golden.LogOccurrences)
	}
}

func TestGoldenGuardrails(t *testing.T) {
	var golden struct {
		Ops           json.RawMessage `json:"ops"`
		End           float64         `json:"end"`
		JSON          string          `json:"json"`
		Repositories  []string        `json:"repositories"`
		Rows          []any           `json:"rows"`
		Totals        map[string]any  `json:"totals"`
		RecentBlocked []any           `json:"recentBlocked"`
		LoadInput     string          `json:"loadInput"`
		LoadOutput    string          `json:"loadOutput"`
		NewerOK       bool            `json:"newerOk"`
		NewerError    string          `json:"newerError"`
	}
	decode(t, loadGolden(t).Guardrails, &golden)
	ops, err := qtjson.Parse(golden.Ops)
	if err != nil {
		t.Fatal(err)
	}
	ledger := NewGuardrailLedger(filepath.Join(t.TempDir(), "guardrails.json"))
	for _, o := range ops.Items() {
		now := o.Get("now").Int64()
		repo := o.Get("repository").Str()
		switch o.Get("op").Str() {
		case "blocked":
			ledger.RecordBlocked(repo, o.Get("tool").Str(), expandFill(o.Get("path").Str()), o.Get("outside").BoolValue(), now)
		case "decision":
			ledger.RecordDecision(repo, o.Get("approved").BoolValue(), now)
		default:
			ledger.RecordAgentsStopped(repo, o.Get("count").Int(), now)
		}
	}
	if got := string(ledger.ToJSON().Compact()); got != golden.JSON {
		t.Errorf("json:\n got %s\nwant %s", got, golden.JSON)
	}
	expectJSON(t, "repositories", ledger.Repositories(), golden.Repositories)
	rows := []any{}
	for _, repo := range ledger.Repositories() {
		rows = append(rows, guardrailRow(ledger.Stats(repo), 5, 5))
	}
	expectJSON(t, "rows", rows, golden.Rows)
	end := int64(golden.End)
	expectJSON(t, "totals", guardrailTotalsRow(ledger.Totals(end)), golden.Totals)
	expectJSON(t, "recentBlocked", recentArray(ledger.Totals(end).RecentBlocked), golden.RecentBlocked)
	input, err := qtjson.Parse([]byte(golden.LoadInput))
	if err != nil {
		t.Fatal(err)
	}
	loaded := NewGuardrailLedger("")
	if err := loaded.FromJSON(input); err != nil {
		t.Fatal(err)
	}
	if got := string(loaded.ToJSON().Compact()); got != golden.LoadOutput {
		t.Errorf("load:\n got %s\nwant %s", got, golden.LoadOutput)
	}
	newer := NewGuardrailLedger("")
	err = newer.FromJSON(qtjson.ObjectOf("version", 2))
	if (err == nil) != golden.NewerOK || err == nil || err.Error() != golden.NewerError {
		t.Errorf("newer: %v want %q", err, golden.NewerError)
	}
}

func TestGoldenShapesAndCatalog(t *testing.T) {
	golden := loadGolden(t)
	var shapes []struct {
		Command string `json:"command"`
		Shape   string `json:"shape"`
	}
	decode(t, golden.Shapes, &shapes)
	for _, c := range shapes {
		if got := CommandShape(c.Command); got != c.Shape {
			t.Errorf("CommandShape(%q) = %q want %q", c.Command, got, c.Shape)
		}
	}
	var familyFor []struct {
		Name   string          `json:"name"`
		Input  json.RawMessage `json:"input"`
		Family string          `json:"family"`
	}
	decode(t, golden.FamilyFor, &familyFor)
	for _, c := range familyFor {
		input, _ := qtjson.Parse(c.Input)
		if got := FamilyFor(c.Name, input); got != c.Family {
			t.Errorf("FamilyFor(%q) = %q want %q", c.Name, got, c.Family)
		}
	}
	var policy struct {
		Notes      map[string]string `json:"notes"`
		Raw        []string          `json:"raw"`
		Normalized []string          `json:"normalized"`
		Claude     []string          `json:"claude"`
		Catalog    []struct {
			ID           string `json:"id"`
			Name         string `json:"name"`
			Category     string `json:"category"`
			Summary      string `json:"summary"`
			Availability string `json:"availability"`
			Note         string `json:"note"`
			Baseline     string `json:"baseline"`
			Expected     string `json:"expected"`
			Hidden       bool   `json:"hidden"`
		} `json:"catalog"`
	}
	decode(t, golden.Policy, &policy)
	for id, note := range policy.Notes {
		if got := EnforcementNote(id); got != note {
			t.Errorf("EnforcementNote(%s) = %q want %q", id, got, note)
		}
	}
	expectJSON(t, "normalized", NormalizeDisabledFamilies(policy.Raw), policy.Normalized)
	expectJSON(t, "claude", ClaudeDisallowedTools(policy.Raw), policy.Claude)
	if len(policy.Catalog) != len(Families()) {
		t.Fatalf("catalog size %d want %d", len(Families()), len(policy.Catalog))
	}
	for i, want := range policy.Catalog {
		got := Families()[i]
		if got.ID != want.ID || got.DisplayName != want.Name || got.Category != want.Category || got.Summary != want.Summary ||
			got.Availability != want.Availability || got.AvailabilityNote != want.Note || got.Baseline != want.Baseline ||
			got.ExpectedSavings != want.Expected || got.Hidden != want.Hidden {
			t.Errorf("family %d: %+v want %+v", i, got, want)
		}
	}
	var directories []struct {
		Input  []string `json:"input"`
		Output []string `json:"output"`
	}
	decode(t, golden.Directories, &directories)
	for _, c := range directories {
		expectJSON(t, "directories "+strings.Join(c.Input, ","), NormalizeDirectories(c.Input), c.Output)
	}
}

func TestGoldenHelpers(t *testing.T) {
	golden := loadGolden(t)
	var helpers struct {
		Subjects []struct {
			Params       map[string]any `json:"params"`
			Subject      string         `json:"subject"`
			Error        string         `json:"error"`
			Summary      string         `json:"summary"`
			SummaryLabel string         `json:"summaryLabel"`
		} `json:"subjects"`
		Traffic []struct {
			Session  int     `json:"session"`
			Peer     string  `json:"peer"`
			Port     int     `json:"port"`
			Sent     float64 `json:"sent"`
			Received float64 `json:"received"`
		} `json:"traffic"`
		Network []any `json:"network"`
	}
	decode(t, golden.Helpers, &helpers)
	for _, c := range helpers.Subjects {
		action, _ := c.Params["action"].(string)
		subject, failure := subjectFor(action, c.Params)
		if subject != c.Subject || (subject == "" && failure != c.Error) {
			t.Errorf("subjectFor(%v) = %q/%q want %q/%q", c.Params, subject, failure, c.Subject, c.Error)
		}
		if got := actionSummary(action, subject, "", false); got != c.Summary {
			t.Errorf("actionSummary(%v) = %q want %q", c.Params, got, c.Summary)
		}
		if got := actionSummary(action, subject, "Label", true); got != c.SummaryLabel {
			t.Errorf("actionSummary label(%v) = %q want %q", c.Params, got, c.SummaryLabel)
		}
	}
	traffic := []peerTraffic{}
	for _, item := range helpers.Traffic {
		traffic = append(traffic, peerTraffic{session: item.Session, peer: item.Peer, port: uint16(item.Port),
			sent: int64(item.Sent), received: int64(item.Received)})
	}
	labels := map[int]string{7: "Fix the build", 3: strings.Repeat("L", 200)}
	expectJSON(t, "network", networkRows(traffic, labels, 2), helpers.Network)
}
