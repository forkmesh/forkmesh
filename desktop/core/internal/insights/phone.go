package insights

import (
	"context"
	"encoding/json"
	"path"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"time"

	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/qsettings"
	"forkmesh.com/desktop-core/internal/rpc"
)

const (
	maxUsageRows       = 60
	maxImprovementRows = 40
	maxGuardrailRepos  = 60
	maxPeersPerAgent   = 12
	desktopCallTimeout = 5 * time.Second
	repositoriesArray  = "repositories/items"
	insightsChanged    = "insights.changed"
)

func (m *Module) registerPhone(core *daemon.Core) {
	handlers := map[string]rpc.Handler{
		"insights.tools":      m.handleTools,
		"insights.guardrails": m.handleGuardrails,
		"insights.network":    m.handleNetwork,
		"insights.prepare":    m.handlePrepare,
		"insights.run":        m.handleRun,
	}
	for method, handler := range handlers {
		core.Registry.Handle(method, m.Name(), m.gate.Wrap(handler))
	}
}

func recentArray(values []int64) []any {
	out := make([]any, 0, len(values))
	for _, value := range values {
		out = append(out, float64(value))
	}
	return out
}

func familyRow(stats FamilyStats, family *Family, disabled bool, enforcement string) map[string]any {
	name, category, summary, availability := stats.Family, "", "", ""
	if family != nil {
		name, category, summary, availability = family.DisplayName, family.Category, left16(family.Summary, 300), family.Availability
	}
	return map[string]any{
		"family":           stats.Family,
		"name":             name,
		"category":         category,
		"summary":          summary,
		"availability":     availability,
		"calls":            float64(stats.Calls),
		"errors":           float64(stats.Errors),
		"inFlight":         float64(stats.InFlight),
		"averageLatencyMs": stats.AverageLatencyMs(),
		"maxLatencyMs":     float64(stats.LatencyMsMax),
		"consumedTokens":   float64(stats.ConsumedTokens()),
		"savedTokens":      float64(stats.SavedTokens),
		"savingsRatio":     stats.SavingsRatio(),
		"savedUsd":         stats.SavedUsd,
		"consumedUsd":      stats.ConsumedUsd,
		"lastCallMs":       float64(stats.LastCallMs),
		"callsWhileOff":    float64(stats.CallsWhileOff),
		"disabled":         disabled,
		"enforcement":      left16(enforcement, 300),
		"recentSaved":      recentArray(stats.RecentSaved),
	}
}

func totalsRow(totals Totals) map[string]any {
	return map[string]any{
		"calls":            float64(totals.Calls),
		"errors":           float64(totals.Errors),
		"inFlight":         float64(totals.InFlight),
		"consumedTokens":   float64(totals.ConsumedTokens),
		"savedTokens":      float64(totals.SavedTokens),
		"savedUsd":         totals.SavedUsd,
		"consumedUsd":      totals.ConsumedUsd,
		"familiesObserved": totals.FamiliesObserved,
		"familiesTotal":    totals.FamiliesTotal,
		"firstCallMs":      float64(totals.FirstCallMs),
		"lastCallMs":       float64(totals.LastCallMs),
		"recentCalls":      recentArray(totals.RecentCalls),
		"recentSaved":      recentArray(totals.RecentSaved),
	}
}

func usageRow(usage UsageStats) map[string]any {
	return map[string]any{
		"tool":             left16(usage.Tool, 120),
		"family":           usage.Family,
		"calls":            float64(usage.Calls),
		"errors":           float64(usage.Errors),
		"errorRate":        usage.ErrorRate(),
		"averageLatencyMs": usage.AverageLatencyMs(),
		"tokensPerCall":    usage.TokensPerCall(),
		"consumedTokens":   float64(usage.ConsumedTokens()),
		"savedTokens":      float64(usage.SavedTokens),
		"consumedUsd":      usage.ConsumedUsd,
		"lastCallMs":       float64(usage.LastCallMs),
	}
}

func improvementRow(item Improvement) map[string]any {
	return map[string]any{
		"key":           left16(item.Key, 200),
		"kind":          item.Kind,
		"family":        item.Family,
		"tool":          left16(item.RuntimeName, 120),
		"topic":         left16(item.Topic, 200),
		"detail":        left16(item.Detail, 400),
		"suggestion":    left16(item.Suggestion, 600),
		"occurrences":   float64(item.Occurrences),
		"tokensAtStake": float64(item.TokensAtStake),
		"lastMs":        float64(item.LastMs),
	}
}

func guardrailRow(stats GuardrailRepoStats, maxTools, maxPaths int) map[string]any {
	type toolCount struct {
		tool  string
		count int64
	}
	tools := []toolCount{}
	for tool, count := range stats.Tools {
		tools = append(tools, toolCount{tool, count})
	}
	sort.Slice(tools, func(i, j int) bool {
		if tools[i].count != tools[j].count {
			return tools[i].count > tools[j].count
		}
		return tools[i].tool < tools[j].tool
	})
	toolRows := []any{}
	for i, tool := range tools {
		if i >= maxTools {
			break
		}
		toolRows = append(toolRows, map[string]any{"tool": left16(tool.tool, 120), "blocked": float64(tool.count)})
	}
	paths := []any{}
	for i, p := range stats.RecentPaths {
		if i >= maxPaths {
			break
		}
		paths = append(paths, left16(p, 300))
	}
	return map[string]any{
		"repository":     stats.Repository,
		"blocked":        float64(stats.Blocked),
		"blockedOutside": float64(stats.BlockedOutside),
		"approved":       float64(stats.Approved),
		"denied":         float64(stats.Denied),
		"agentsStopped":  float64(stats.AgentsStopped),
		"lastEventMs":    float64(stats.LastEventMs),
		"tools":          toolRows,
		"recentPaths":    paths,
	}
}

func guardrailTotalsRow(totals GuardrailTotals) map[string]any {
	return map[string]any{
		"blocked":                float64(totals.Blocked),
		"blockedOutside":         float64(totals.BlockedOutside),
		"approved":               float64(totals.Approved),
		"denied":                 float64(totals.Denied),
		"agentsStopped":          float64(totals.AgentsStopped),
		"lastEventMs":            float64(totals.LastEventMs),
		"repositoriesWithEvents": totals.RepositoriesWithEvents,
	}
}

func (m *Module) ToolsPayload(disabled []string) map[string]any {
	disabledSet := map[string]bool{}
	for _, family := range disabled {
		disabledSet[family] = true
	}
	familyRows := []any{}
	usageRows := []any{}
	improvementRows := []any{}
	totals := map[string]any{}
	m.mu.Lock()
	if m.ledger != nil {
		for _, stats := range m.ledger.AllStats() {
			family := FamilyByID(stats.Family)
			if family != nil && family.Hidden {
				continue
			}
			familyRows = append(familyRows, familyRow(stats, family, disabledSet[stats.Family], EnforcementNote(stats.Family)))
		}
		tools := m.ledger.Usage()
		sort.SliceStable(tools, func(i, j int) bool { return tools[i].Calls > tools[j].Calls })
		for i, tool := range tools {
			if i >= maxUsageRows {
				break
			}
			usageRows = append(usageRows, usageRow(tool))
		}
		totals = totalsRow(m.ledger.Totals(-1))
	}
	if m.improvements != nil {
		for i, item := range m.improvements.Improvements() {
			if i >= maxImprovementRows {
				break
			}
			improvementRows = append(improvementRows, improvementRow(item))
		}
	}
	m.mu.Unlock()
	return map[string]any{
		"families":     familyRows,
		"totals":       totals,
		"usage":        usageRows,
		"improvements": improvementRows,
		"disabled":     nonNil(disabled),
	}
}

func (m *Module) handleTools(ctx context.Context, req *rpc.Request) (any, error) {
	return m.ToolsPayload(m.settingsDisabled()), nil
}

type guardrailPolicy struct {
	Repository         string
	Enabled            bool
	AllowedDirectories []string
}

var opaqueIDPattern = regexp.MustCompile(`^[0-9a-f]{64}$`)

func cleanRelativeDirectory(raw string) string {
	p := strings.ReplaceAll(trimmed(raw), "\\", "/")
	if p == "" || strings.ContainsRune(p, 0) || qtIsAbsolute(p) {
		return ""
	}
	p = path.Clean(p)
	if p == ".." || strings.HasPrefix(p, "../") {
		return ""
	}
	for strings.HasPrefix(p, "./") {
		p = p[2:]
	}
	if p == "" {
		return "."
	}
	return p
}

func NormalizeDirectories(entries []string) []string {
	normalized := []string{}
	seen := map[string]bool{}
	for _, entry := range entries {
		p := cleanRelativeDirectory(entry)
		if p == "" {
			continue
		}
		if !seen[p] {
			seen[p] = true
			normalized = append(normalized, p)
		}
		if len(normalized) == 64 {
			break
		}
	}
	sort.SliceStable(normalized, func(i, j int) bool {
		a, b := normalized[i], normalized[j]
		if a == b {
			return false
		}
		if a == "." {
			return true
		}
		if b == "." {
			return false
		}
		return a < b
	})
	if seen["."] {
		return []string{"."}
	}
	compact := []string{}
	for _, p := range normalized {
		covered := false
		for _, parent := range compact {
			if strings.HasPrefix(p, parent+"/") {
				covered = true
				break
			}
		}
		if !covered {
			compact = append(compact, p)
		}
	}
	return compact
}

func guardrailPolicies(s *qsettings.Settings, limit int) []any {
	count := int(s.Int(repositoriesArray+"/size", 0))
	out := []any{}
	for i := 1; i <= count; i++ {
		prefix := repositoriesArray + "/" + strconv.Itoa(i) + "/"
		get := func(key string) string { return s.String(prefix+key, "") }
		owner, name, localPath := get("owner"), get("name"), get("localPath")
		source := trimmed(localPath)
		if source == "" {
			source = trimmed(get("cloneUrl"))
		}
		if name == "" || (source == "" && !opaqueIDPattern.MatchString(trimmed(get("publicArchiveId"))) &&
			!opaqueIDPattern.MatchString(trimmed(get("privateReplicaId")))) {
			continue
		}
		if localPath == "" || len(out) >= limit {
			continue
		}
		allowed := NormalizeDirectories(s.Strings(prefix + "agentAllowedDirectories"))
		if len(allowed) > 30 {
			allowed = allowed[:30]
		}
		out = append(out, map[string]any{
			"repository":         owner + "/" + name,
			"enabled":            s.Bool(prefix+"agentGuardrailsEnabled", false),
			"allowedDirectories": nonNil(allowed),
		})
	}
	return out
}

func (m *Module) GuardrailsPayload() map[string]any {
	repositories := []any{}
	totals := map[string]any{}
	m.mu.Lock()
	if m.guardrails != nil {
		for i, repo := range m.guardrails.Repositories() {
			if i >= maxGuardrailRepos {
				break
			}
			repositories = append(repositories, guardrailRow(m.guardrails.Stats(repo), 5, 5))
		}
		totals = guardrailTotalsRow(m.guardrails.Totals(-1))
	}
	m.mu.Unlock()
	policies := []any{}
	if m.core != nil && m.core.Settings != nil {
		policies = guardrailPolicies(m.core.Settings.Get(), maxGuardrailRepos)
	}
	return map[string]any{"totals": totals, "repositories": repositories, "policies": policies}
}

func (m *Module) handleGuardrails(ctx context.Context, req *rpc.Request) (any, error) {
	return m.GuardrailsPayload(), nil
}

func (m *Module) callDesktop(ctx context.Context, method string, params any, out any) error {
	callCtx, cancel := context.WithTimeout(ctx, desktopCallTimeout)
	defer cancel()
	raw, err := m.callUI(callCtx, method, params)
	if err != nil {
		return err
	}
	if out != nil {
		if err := json.Unmarshal(raw, out); err != nil {
			return rpc.Fail("bad_reply")
		}
	}
	return nil
}
