package insights

import (
	"errors"
	"math"
	"os"
	"sort"
	"strings"

	"forkmesh.com/desktop-core/internal/qtjson"
)

const (
	GuardrailActivityBuckets  = 24
	GuardrailActivityBucketMs = 60 * 60 * 1000
	GuardrailRecentPaths      = 8
	GuardrailMaxPathLength    = 300
	guardrailLedgerVersion    = 1
)

type GuardrailRepoStats struct {
	Repository     string
	Blocked        int64
	BlockedOutside int64
	Approved       int64
	Denied         int64
	AgentsStopped  int64
	LastEventMs    int64
	Tools          map[string]int64
	RecentPaths    []string
}

type GuardrailTotals struct {
	Blocked                int64
	BlockedOutside         int64
	Approved               int64
	Denied                 int64
	AgentsStopped          int64
	LastEventMs            int64
	RepositoriesWithEvents int
	RecentBlocked          []int64
}

type GuardrailLedger struct {
	path                string
	repositories        map[string]*GuardrailRepoStats
	activity            []int64
	activityBucketStart int64

	Changed func()
}

func NewGuardrailLedger(path string) *GuardrailLedger {
	return &GuardrailLedger{path: path, repositories: map[string]*GuardrailRepoStats{}}
}

func (g *GuardrailLedger) Path() string { return g.path }

func alignedActivity(values []int64, startMs, now int64) []int64 {
	if len(values) != GuardrailActivityBuckets || startMs <= 0 {
		return make([]int64, GuardrailActivityBuckets)
	}
	shift := ((now/GuardrailActivityBucketMs)*GuardrailActivityBucketMs - startMs) / GuardrailActivityBucketMs
	if shift <= 0 {
		return append([]int64(nil), values...)
	}
	if shift >= GuardrailActivityBuckets {
		return make([]int64, GuardrailActivityBuckets)
	}
	out := append([]int64(nil), values[shift:]...)
	for len(out) < GuardrailActivityBuckets {
		out = append(out, 0)
	}
	return out
}

func (g *GuardrailLedger) touch(repository string, now int64) *GuardrailRepoStats {
	stats := g.repositories[repository]
	if stats == nil {
		stats = &GuardrailRepoStats{Tools: map[string]int64{}}
		g.repositories[repository] = stats
	}
	stats.Repository = repository
	stats.LastEventMs = max(stats.LastEventMs, now)
	return stats
}

func (g *GuardrailLedger) bumpActivity(now int64) {
	bucket := (now / GuardrailActivityBucketMs) * GuardrailActivityBucketMs
	g.activity = alignedActivity(g.activity, g.activityBucketStart, now)
	g.activityBucketStart = max(g.activityBucketStart, bucket)
	g.activity[len(g.activity)-1]++
}

func (g *GuardrailLedger) RecordBlocked(repository, tool, path string, outside bool, now int64) {
	if repository == "" {
		return
	}
	now = nowOr(now)
	stats := g.touch(repository, now)
	stats.Blocked++
	if outside {
		stats.BlockedOutside++
	}
	toolName := trimmed(tool)
	if toolName == "" {
		toolName = "tool call"
	}
	stats.Tools[toolName]++
	shown := left16(trimmed(path), GuardrailMaxPathLength)
	if shown != "" {
		kept := []string{shown}
		for _, existing := range stats.RecentPaths {
			if existing != shown {
				kept = append(kept, existing)
			}
		}
		if len(kept) > GuardrailRecentPaths {
			kept = kept[:GuardrailRecentPaths]
		}
		stats.RecentPaths = kept
	}
	g.bumpActivity(now)
	g.notify()
}

func (g *GuardrailLedger) RecordDecision(repository string, approved bool, now int64) {
	if repository == "" {
		return
	}
	stats := g.touch(repository, nowOr(now))
	if approved {
		stats.Approved++
	} else {
		stats.Denied++
	}
	g.notify()
}

func (g *GuardrailLedger) RecordAgentsStopped(repository string, count int, now int64) {
	if repository == "" || count <= 0 {
		return
	}
	g.touch(repository, nowOr(now)).AgentsStopped += int64(count)
	g.notify()
}

func (g *GuardrailLedger) Stats(repository string) GuardrailRepoStats {
	out := GuardrailRepoStats{Repository: repository, Tools: map[string]int64{}}
	if stats := g.repositories[repository]; stats != nil {
		out = *stats
		out.Repository = repository
		out.Tools = map[string]int64{}
		for tool, calls := range stats.Tools {
			out.Tools[tool] = calls
		}
		out.RecentPaths = append([]string(nil), stats.RecentPaths...)
	}
	return out
}

func caseInsensitiveLess(a, b string) bool {
	la, lb := strings.ToLower(a), strings.ToLower(b)
	if la != lb {
		return la < lb
	}
	return false
}

func (g *GuardrailLedger) Repositories() []string {
	keys := make([]string, 0, len(g.repositories))
	for key := range g.repositories {
		keys = append(keys, key)
	}
	sort.Strings(keys)
	sort.SliceStable(keys, func(i, j int) bool { return caseInsensitiveLess(keys[i], keys[j]) })
	return keys
}

func (g *GuardrailLedger) Totals(now int64) GuardrailTotals {
	totals := GuardrailTotals{}
	for _, stats := range g.repositories {
		totals.Blocked += stats.Blocked
		totals.BlockedOutside += stats.BlockedOutside
		totals.Approved += stats.Approved
		totals.Denied += stats.Denied
		totals.AgentsStopped += stats.AgentsStopped
		totals.LastEventMs = max(totals.LastEventMs, stats.LastEventMs)
		totals.RepositoriesWithEvents++
	}
	totals.RecentBlocked = alignedActivity(g.activity, g.activityBucketStart, nowOr(now))
	return totals
}

func (g *GuardrailLedger) HasEvents() bool { return len(g.repositories) > 0 }

func (g *GuardrailLedger) Clear() {
	g.repositories = map[string]*GuardrailRepoStats{}
	g.activity = nil
	g.activityBucketStart = 0
	g.notify()
}

func (g *GuardrailLedger) notify() {
	if g.Changed != nil {
		g.Changed()
	}
}

func (g *GuardrailLedger) ToJSON() qtjson.Value {
	repositories := qtjson.NewObject()
	for _, stats := range g.repositories {
		tools := qtjson.NewObject()
		for tool, calls := range stats.Tools {
			tools.Set(tool, qtjson.NewDouble(float64(calls)))
		}
		repositories.Set(stats.Repository, qtjson.ObjectOf(
			"blocked", float64(stats.Blocked),
			"blockedOutside", float64(stats.BlockedOutside),
			"approved", float64(stats.Approved),
			"denied", float64(stats.Denied),
			"agentsStopped", float64(stats.AgentsStopped),
			"lastEventMs", float64(stats.LastEventMs),
			"tools", tools,
			"recentPaths", qtjson.NewStrings(stats.RecentPaths),
		))
	}
	return qtjson.ObjectOf(
		"version", guardrailLedgerVersion,
		"repositories", repositories,
		"activity", int64Array(g.activity),
		"activityBucketStartMs", float64(g.activityBucketStart),
	)
}

func qtToInteger(value qtjson.Value) int64 {
	if value.Kind() == qtjson.Integer {
		return value.Int64()
	}
	if value.Kind() == qtjson.Double {
		d := value.Float()
		if d == math.Trunc(d) && d >= -9.2233720368547758e18 && d < 9.2233720368547758e18 {
			return int64(d)
		}
	}
	return 0
}

func guardrailCount(object qtjson.Value, key string) int64 {
	return max(0, qtToInteger(object.Get(key)))
}

func (g *GuardrailLedger) FromJSON(json qtjson.Value) error {
	if json.Get("version").IntOr(0) > guardrailLedgerVersion {
		return errors.New("guardrail ledger is from a newer version")
	}
	repositories := map[string]*GuardrailRepoStats{}
	for _, member := range json.Get("repositories").Members() {
		if member.Key == "" {
			continue
		}
		object := member.Value
		stats := &GuardrailRepoStats{Repository: member.Key, Tools: map[string]int64{}}
		stats.Blocked = guardrailCount(object, "blocked")
		stats.BlockedOutside = min(stats.Blocked, guardrailCount(object, "blockedOutside"))
		stats.Approved = guardrailCount(object, "approved")
		stats.Denied = guardrailCount(object, "denied")
		stats.AgentsStopped = guardrailCount(object, "agentsStopped")
		stats.LastEventMs = guardrailCount(object, "lastEventMs")
		for _, tool := range object.Get("tools").Members() {
			if calls := max(0, qtToInteger(tool.Value)); calls > 0 {
				stats.Tools[tool.Key] = calls
			}
		}
		for _, path := range object.Get("recentPaths").Items() {
			if path.IsString() && len(stats.RecentPaths) < GuardrailRecentPaths {
				stats.RecentPaths = append(stats.RecentPaths, left16(path.Str(), GuardrailMaxPathLength))
			}
		}
		repositories[stats.Repository] = stats
	}
	activity := []int64{}
	for _, value := range json.Get("activity").Items() {
		activity = append(activity, max(0, qtToInteger(value)))
	}
	g.repositories = repositories
	if len(activity) == GuardrailActivityBuckets {
		g.activity = activity
		g.activityBucketStart = max(0, qtToInteger(json.Get("activityBucketStartMs")))
	} else {
		g.activity = nil
		g.activityBucketStart = 0
	}
	return nil
}

func (g *GuardrailLedger) Load() error {
	if g.path == "" {
		return errors.New("no ledger path")
	}
	data, err := os.ReadFile(g.path)
	if errors.Is(err, os.ErrNotExist) {
		return nil
	}
	if err != nil {
		return errors.New("cannot open " + g.path)
	}
	doc, err := qtjson.Parse(data)
	if err != nil || !doc.IsObject() {
		return errors.New("guardrail ledger is not valid JSON")
	}
	return g.FromJSON(doc)
}

func (g *GuardrailLedger) Encoded() []byte {
	return g.ToJSON().Compact()
}
