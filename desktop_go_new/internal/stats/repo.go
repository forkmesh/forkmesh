package stats

import (
	"context"
	"fmt"
	"regexp"
	"strings"
	"time"

	"forkmesh.com/desktop-core/internal/rpc"
)

const changedEventDelay = 20 * time.Millisecond

var repoSlugPattern = regexp.MustCompile(`^([A-Za-z0-9_.-]{1,100})/([A-Za-z0-9_.-]{1,100})$`)

func parseRepoSlug(slug string) (string, string, bool) {
	match := repoSlugPattern.FindStringSubmatch(slug)
	if match == nil {
		return "", "", false
	}
	owner, name := match[1], match[2]
	if owner == "." || owner == ".." || name == "." || name == ".." {
		return "", "", false
	}
	return owner, name, true
}

func byteText(bytes int64) string {
	if bytes < 0 {
		bytes = 0
	}
	value := float64(bytes)
	switch {
	case value >= 1024.0*1024.0*1024.0:
		return fmt.Sprintf("%.1f GB", value/(1024.0*1024.0*1024.0))
	case value >= 1024.0*1024.0:
		return fmt.Sprintf("%.1f MB", value/(1024.0*1024.0))
	case value >= 1024.0:
		return fmt.Sprintf("%.1f KB", value/1024.0)
	}
	return fmt.Sprintf("%d B", bytes)
}

func left(text string, n int) string {
	units := 0
	for i, r := range text {
		width := 1
		if r > 0xFFFF {
			width = 2
		}
		if units+width > n {
			return text[:i]
		}
		units += width
	}
	return text
}

func ratchetNote(facts repoFacts) string {
	if !facts.RatchetEnabled {
		return ""
	}
	if facts.CeilingBytes <= 0 {
		return "Commits may not grow the repository."
	}
	since := ""
	if facts.CeilingDay != "" {
		since = " (set " + left(facts.CeilingDay, 10) + ")"
	}
	return "Commits may not grow the repository past " + byteText(facts.CeilingBytes) + since + "."
}

func nonNegative(n int64) float64 {
	if n < 0 {
		return 0
	}
	return float64(n)
}

func repoStatsJSON(facts repoFacts) map[string]any {
	ratchet := map[string]any{"enabled": facts.RatchetEnabled, "note": ratchetNote(facts)}
	if facts.RatchetEnabled {
		ratchet["ceilingBytes"] = nonNegative(facts.CeilingBytes)
		ratchet["ceilingDay"] = left(facts.CeilingDay, 10)
	}
	return map[string]any{
		"commits": nonNegative(facts.Commits),
		"files":   nonNegative(facts.Files),
		"lines":   nonNegative(facts.Lines),
		"bytes":   nonNegative(facts.Bytes),
		"day":     left(facts.Day, 10),
		"ratchet": ratchet,
	}
}

func ratchetSummary(repo string, enabled bool) string {
	if enabled {
		return "Turn on Ratchet Mode for " + repo + " on the desktop: commits may not grow the repository past the size it is now."
	}
	return "Turn off Ratchet Mode for " + repo + " on the desktop: commits are no longer checked against a size ceiling."
}

func repoParam(params map[string]any) (string, string, string, bool) {
	text, _ := params["repo"].(string)
	owner, name, ok := parseRepoSlug(strings.TrimSpace(text))
	if !ok {
		return "", "", "", false
	}
	return owner, name, owner + "/" + name, true
}

func failRepo(index int) error {
	if index == -2 {
		return rpc.Fail("bad_request")
	}
	return rpc.Fail("unknown_repo")
}

func (m *Module) resolveRepo(params map[string]any) (repoCatalog, int, string) {
	catalog := m.catalog()
	owner, name, slug, ok := repoParam(params)
	if !ok {
		return catalog, -2, ""
	}
	return catalog, catalog.indexFor(owner, name), slug
}

func (m *Module) repoFacts(dir string) (repoFacts, string) {
	unlock := m.repoStore.lock(dir)
	defer unlock()
	return m.repoStore.readRepoFacts(dir)
}

func (m *Module) handleRepo(ctx context.Context, req *rpc.Request) (any, error) {
	defer m.track()()
	catalog, index, _ := m.resolveRepo(req.Object())
	if index < 0 {
		return nil, failRepo(index)
	}
	dir := catalog.checkoutDir(index)
	if dir == "" {
		return nil, rpc.Fail("no_checkout")
	}
	facts, failure := m.repoFacts(dir)
	if failure != "" {
		return nil, rpc.Fail(failure)
	}
	return repoStatsJSON(facts), nil
}

func (m *Module) handleRatchetPrepare(ctx context.Context, req *rpc.Request) (any, error) {
	params := req.Object()
	catalog, index, slug := m.resolveRepo(params)
	enabled, isBool := params["enabled"].(bool)
	if index < 0 || !isBool {
		if index < 0 {
			return nil, failRepo(index)
		}
		return nil, failRepo(-2)
	}
	if catalog.checkoutDir(index) == "" {
		return nil, rpc.Fail("no_checkout")
	}
	token, expires := m.confirmations.issue(peerDevice(req).Key, slug, enabled, m.now().UnixMilli())
	return map[string]any{
		"token":       token,
		"action":      ratchetAction(enabled),
		"summary":     ratchetSummary(slug, enabled),
		"expiresAt":   float64(expires),
		"expiresInMs": float64(confirmationLifetimeMs),
	}, nil
}

func (m *Module) handleRatchetRun(ctx context.Context, req *rpc.Request) (any, error) {
	params := req.Object()
	token, _ := params["token"].(string)
	verdict, slug, enabled := m.confirmations.consume(token, peerDevice(req).Key, m.now().UnixMilli())
	if verdict != confirmOK {
		why := confirmationError(verdict)
		m.logf("Refused a Ratchet Mode change from %s: %s.", deviceLabel(req), why)
		return nil, rpc.Fail(why)
	}
	owner, name, _ := parseRepoSlug(slug)
	catalog := m.catalog()
	dir := catalog.checkoutDir(catalog.indexFor(owner, name))
	if dir == "" {
		return nil, rpc.Fail("no_checkout")
	}
	m.mu.Lock()
	if m.ratchetFlights[dir] {
		m.mu.Unlock()
		return nil, rpc.Fail("busy")
	}
	m.ratchetFlights[dir] = true
	m.active++
	m.mu.Unlock()
	defer func() {
		m.mu.Lock()
		delete(m.ratchetFlights, dir)
		m.active--
		m.mu.Unlock()
	}()
	who := deviceLabel(req)
	unlock := m.repoStore.lock(dir)
	err := m.repoStore.setRatchetEnabled(dir, enabled)
	var facts repoFacts
	if err == nil {
		facts, _ = m.repoStore.readRepoFacts(dir)
	}
	unlock()
	if err != nil {
		return nil, rpc.Fail("ratchet_failed")
	}
	state := "disabled"
	if enabled {
		state = "enabled"
	}
	m.logf("Ratchet Mode %s for %s, requested by %s.", state, slug, who)
	time.AfterFunc(changedEventDelay, func() {
		m.broadcastPhones("stats.changed", map[string]any{"what": "ratchet", "repo": slug})
	})
	return repoStatsJSON(facts), nil
}
