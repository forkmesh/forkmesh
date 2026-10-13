package actions

import (
	"context"
	"os"
	"path/filepath"
	"strings"

	"forkmesh.com/desktop-core/internal/agents"
	"forkmesh.com/desktop-core/internal/rpc"
)

type AgentCount struct {
	Owner  string
	Name   string
	Active bool
}

func (m *Module) agentCounts() []AgentCount {
	if m.Agents != nil {
		return m.Agents()
	}
	if m.core == nil {
		return nil
	}
	module, ok := m.core.Module("agents").(*agents.Module)
	if !ok || module == nil || module.Store() == nil {
		return nil
	}
	var out []AgentCount
	for _, session := range module.Store().LoadAllSessions() {
		if session.DeletedAtMs > 0 || session.AssociationOnly || session.Owner == "" {
			continue
		}
		out = append(out, AgentCount{Owner: session.Owner, Name: session.Name, Active: session.Active()})
	}
	return out
}

func checkoutBranch(dir string) string {
	file, err := os.Open(filepath.Join(dir, ".git", "HEAD"))
	if err != nil {
		return ""
	}
	defer file.Close()
	buffer := make([]byte, 512)
	n, _ := file.Read(buffer)
	text := qtTrim(qtFromUtf8(buffer[:n]))
	if strings.HasPrefix(text, "ref: refs/heads/") {
		return text[16:]
	}
	return qtLeft(text, 7)
}

func (m *Module) currentSnapshot(ctx context.Context) (RepoSnapshot, bool) {
	if m.q == nil {
		return RepoSnapshot{}, false
	}
	type state struct {
		snap  RepoSnapshot
		armed bool
	}
	out, err := await(ctx, m.q, func(done func(state)) { done(state{m.q.snap, m.q.armed}) })
	if err != nil {
		return RepoSnapshot{}, false
	}
	return out.snap, out.armed
}

func (m *Module) phoneReposList(ctx context.Context, req *rpc.Request) (any, error) {
	snap, armed := m.currentSnapshot(ctx)
	if !armed {
		if m.core != nil && m.core.UI() != nil {
			return nil, rpc.ErrDelegate
		}
		snap = SettingsSnapshot(m.settings(), filepath.Dir(m.st.Root))
	}
	return ReposListReply(snap, m.st.LoadAllRuns(), m.agentCounts()), nil
}

func ReposListReply(snap RepoSnapshot, runs []Run, sessions []AgentCount) map[string]any {
	runsByRepo := map[string][]Run{}
	for _, run := range runs {
		key := strings.ToLower(run.Owner + "/" + run.Name)
		runsByRepo[key] = append(runsByRepo[key], run)
	}
	type counts struct{ running, total int }
	agentsByRepo := map[string]*counts{}
	for _, session := range sessions {
		key := strings.ToLower(session.Owner + "/" + session.Name)
		if agentsByRepo[key] == nil {
			agentsByRepo[key] = &counts{}
		}
		agentsByRepo[key].total++
		if session.Active {
			agentsByRepo[key].running++
		}
	}
	rows := []map[string]any{}
	seen := map[string]bool{}
	for _, repo := range snap.Repos {
		if repo.Owner == "" || repo.Name == "" || repo.Preview {
			continue
		}
		key := strings.ToLower(repo.Owner + "/" + repo.Name)
		if _, _, ok := ParseRepoSlug(repo.Owner + "/" + repo.Name); seen[key] || !ok {
			continue
		}
		seen[key] = true
		writable := repo.WritablePath
		checkout := hasGitCheckout(writable)
		branch := ""
		if checkout {
			branch = checkoutBranch(writable)
		}
		agentCounts := counts{}
		if found := agentsByRepo[key]; found != nil {
			agentCounts = *found
		}
		rows = append(rows, map[string]any{
			"repo":           repo.Owner + "/" + repo.Name,
			"owner":          repo.Owner,
			"name":           repo.Name,
			"description":    qtLeft(repo.Description, 300),
			"branch":         branch,
			"checkout":       checkout,
			"mirror":         repo.MirrorPath != "",
			"private":        repo.Private,
			"published":      repo.Published,
			"actionsEnabled": repo.Enabled,
			"lastSyncMs":     float64(repo.LastSyncMs),
			"agents":         map[string]any{"running": agentCounts.running, "total": agentCounts.total},
			"actions":        ActionsSummary(runsByRepo[key]),
		})
	}
	return map[string]any{"repos": rows, "desktop": snap.Machine}
}
