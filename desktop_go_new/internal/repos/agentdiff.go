package repos

import (
	"bytes"
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"sync"

	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/git"
	"forkmesh.com/desktop-core/internal/rpc"
)

const (
	AgentDiffStatEvent    = "core.git.agentDiffStat"
	maxAgentDiffSessions  = 5000
	agentDiffWorkers      = 4
	agentDiffCacheEntries = 8192
	untrackedCountLimit   = 1024 * 1024
)

func (m *Module) startDesktopPages(core *daemon.Core) {
	core.Registry.Handle("core.git.run", m.Name(), m.track(m.gitRun))
	core.Registry.Handle("core.git.agentDiffStats", m.Name(), m.track(m.agentDiffStats))
}

type AgentDiffStat struct {
	Files      int    `json:"files"`
	Ahead      int    `json:"ahead"`
	Behind     int    `json:"behind"`
	Conflicted bool   `json:"conflicted"`
	Base       string `json:"base"`
	Worktree   string `json:"worktree"`
	Dirty      int    `json:"dirty"`
	Added      int    `json:"added"`
	Removed    int    `json:"removed"`
}

func newAgentDiffStat() AgentDiffStat {
	return AgentDiffStat{Files: -1, Ahead: -1, Behind: -1, Dirty: -1, Added: -1, Removed: -1}
}

type AgentDiffSession struct {
	ID           int    `json:"id"`
	Status       string `json:"status"`
	Branch       string `json:"branch"`
	BaseBranch   string `json:"baseBranch"`
	Merged       bool   `json:"merged"`
	FinishedAtMs int64  `json:"finishedAtMs"`
	PRNumber     int    `json:"prNumber"`
	PatchPath    string `json:"patchPath"`
	Known        bool   `json:"known"`
	Signature    string `json:"signature"`
}

type agentDiffParams struct {
	Path           string             `json:"path"`
	ConfiguredBase string             `json:"configuredBase"`
	CheckedOut     string             `json:"checkedOut"`
	Selected       int                `json:"selected"`
	Token          string             `json:"token"`
	Sessions       []AgentDiffSession `json:"sessions"`
}

type agentDiffEntry struct {
	ID        int           `json:"id"`
	Stat      AgentDiffStat `json:"stat"`
	Signature string        `json:"signature"`
}

type agentDiffSignature struct {
	ID        int    `json:"id"`
	Signature string `json:"signature"`
}

type committedDiff struct {
	ownedOK bool
	owned   []string
	diffOK  bool
	files   int
	added   int
	removed int
}

type aheadBehind struct {
	behind int
	ahead  int
}

type diffCache struct {
	mu        sync.Mutex
	committed map[string]committedDiff
	counts    map[string]aheadBehind
	conflicts map[string]bool
}

func newDiffCache() *diffCache {
	return &diffCache{committed: map[string]committedDiff{}, counts: map[string]aheadBehind{}, conflicts: map[string]bool{}}
}

func (c *diffCache) size() int {
	return len(c.committed) + len(c.counts) + len(c.conflicts)
}

func (c *diffCache) trim() {
	if c.size() > agentDiffCacheEntries {
		c.committed = map[string]committedDiff{}
		c.counts = map[string]aheadBehind{}
		c.conflicts = map[string]bool{}
	}
}

var diffCaches sync.Map

func (m *Module) diffCache() *diffCache {
	if cache, ok := diffCaches.Load(m); ok {
		return cache.(*diffCache)
	}
	cache, _ := diffCaches.LoadOrStore(m, newDiffCache())
	return cache.(*diffCache)
}

func agentActive(status string) bool {
	return status == "running" || status == "waiting" || status == "queued"
}

func boolDigit(value bool) string {
	if value {
		return "1"
	}
	return "0"
}

func AgentDiffSignature(session AgentDiffSession, base string, tips map[string]string) string {
	return strings.Join([]string{
		session.Status, session.Branch, base, tips[base], boolDigit(session.Merged),
		strconv.FormatInt(session.FinishedAtMs, 10), strconv.Itoa(session.PRNumber), tips[session.Branch],
	}, "|")
}

func (m *Module) gitOutput(ctx context.Context, dir string, args []string) ([]byte, bool) {
	result := m.Git.Run(ctx, dir, args, git.Options{})
	if !result.OK {
		return nil, false
	}
	return result.Output, true
}

func (m *Module) gitOK(ctx context.Context, dir string, args []string) (ok bool, timedOut bool) {
	result := m.Git.Run(ctx, dir, args, git.Options{})
	return result.OK, result.TimedOut
}

func splitLines(output []byte) []string {
	return strings.Split(string(output), "\n")
}

func (m *Module) DefaultAgentBase(ctx context.Context, gitDir, configured, checkedOut string) string {
	configured = strings.TrimSpace(configured)
	if configured != "" {
		return configured
	}
	var branches []string
	if out, ok := m.gitOutput(ctx, gitDir, []string{"for-each-ref", "--format=%(refname:short)", "refs/heads/"}); ok {
		for _, line := range splitLines(out) {
			if branch := strings.TrimSpace(line); branch != "" {
				branches = append(branches, branch)
			}
		}
	}
	has := func(name string) bool {
		for _, branch := range branches {
			if branch == name {
				return true
			}
		}
		return false
	}
	switch {
	case has("main"):
		return "main"
	case has("master"):
		return "master"
	case checkedOut != "" && has(checkedOut):
		return checkedOut
	case len(branches) > 0:
		return branches[0]
	}
	return ""
}

func absoluteClean(path string) string {
	if abs, err := filepath.Abs(path); err == nil {
		return abs
	}
	return filepath.Clean(path)
}

func (m *Module) AgentWorktrees(ctx context.Context, gitDir string) map[string]string {
	result := map[string]string{}
	if gitDir == "" {
		return result
	}
	out, ok := m.gitOutput(ctx, gitDir, []string{"worktree", "list", "--porcelain"})
	if !ok {
		return result
	}
	self := absoluteClean(gitDir)
	path := ""
	for _, raw := range splitLines(out) {
		line := strings.TrimSpace(raw)
		switch {
		case strings.HasPrefix(line, "worktree "):
			path = strings.TrimSpace(line[len("worktree "):])
		case strings.HasPrefix(line, "branch refs/heads/") && path != "" && absoluteClean(path) != self:
			result[strings.TrimSpace(line[len("branch refs/heads/"):])] = path
		case line == "":
			path = ""
		}
	}
	return result
}

func (m *Module) AgentBranchTips(ctx context.Context, gitDir string) map[string]string {
	result := map[string]string{}
	if gitDir == "" {
		return result
	}
	out, ok := m.gitOutput(ctx, gitDir, []string{"for-each-ref", "--format=%(refname:short)\t%(objectname)", "refs/heads/"})
	if !ok {
		return result
	}
	for _, raw := range splitLines(out) {
		tab := strings.IndexByte(raw, '\t')
		if tab <= 0 {
			continue
		}
		result[strings.TrimSpace(raw[:tab])] = strings.TrimSpace(raw[tab+1:])
	}
	return result
}

func SummarizeAgentPatch(patch string, stat *AgentDiffStat) {
	files := 0
	if strings.HasPrefix(patch, "diff --git ") {
		files = 1
	}
	files += strings.Count(patch, "\ndiff --git ")
	added, removed := 0, 0
	for _, line := range strings.Split(patch, "\n") {
		if strings.HasPrefix(line, "+++") || strings.HasPrefix(line, "---") {
			continue
		}
		if strings.HasPrefix(line, "+") {
			added++
		} else if strings.HasPrefix(line, "-") {
			removed++
		}
	}
	stat.Files, stat.Added, stat.Removed = files, added, removed
}

func numstatField(text string) int {
	n, err := strconv.Atoi(strings.TrimSpace(text))
	if err != nil {
		return 0
	}
	return n
}

func SummarizeAgentNumstat(numstat []byte) (files, added, removed int) {
	for _, line := range bytes.Split(numstat, []byte{'\n'}) {
		first := bytes.IndexByte(line, '\t')
		if first < 0 {
			continue
		}
		second := bytes.IndexByte(line[first+1:], '\t')
		if second < 0 {
			continue
		}
		second += first + 1
		files++
		added += numstatField(string(line[:first]))
		removed += numstatField(string(line[first+1 : second]))
	}
	return files, added, removed
}

func ParseAgentOwnedLog(output []byte) []string {
	seen := map[string]bool{}
	for _, raw := range strings.Split(string(output), "\n") {
		line := strings.TrimSpace(raw)
		if line == "" || strings.HasPrefix(line, "\x1e") {
			continue
		}
		seen[line] = true
	}
	return sortedSet(seen)
}

func sortedSet(set map[string]bool) []string {
	out := make([]string, 0, len(set))
	for path := range set {
		out = append(out, path)
	}
	sort.Strings(out)
	return out
}

func literalPathspecs(paths []string) []string {
	out := make([]string, 0, len(paths))
	for _, path := range paths {
		out = append(out, ":(literal)"+path)
	}
	return out
}

func readSmallText(path string) string {
	data, err := os.ReadFile(path)
	if err != nil {
		return ""
	}
	return string(bytes.ReplaceAll(data, []byte("\r\n"), []byte("\n")))
}

func untrackedLines(path string) int {
	info, err := os.Stat(path)
	if err != nil || info.Size() > untrackedCountLimit {
		return 0
	}
	data, err := os.ReadFile(path)
	if err != nil || bytes.IndexByte(data, 0) >= 0 {
		return 0
	}
	lines := bytes.Count(data, []byte{'\n'})
	if len(data) > 0 && data[len(data)-1] != '\n' {
		lines++
	}
	return lines
}

func isDirectory(path string) bool {
	info, err := os.Stat(path)
	return err == nil && info.IsDir()
}

type agentDiffJob struct {
	gitDir   string
	repoKey  string
	base     string
	worktree string
	tips     map[string]string
	probe    bool
	idle     bool
}

func (m *Module) committedDiff(ctx context.Context, job agentDiffJob, session AgentDiffSession) committedDiff {
	baseTip, branchTip := job.tips[job.base], job.tips[session.Branch]
	cacheable := baseTip != "" && branchTip != ""
	baseRef, branchRef := job.base, session.Branch
	key := ""
	if cacheable {
		baseRef, branchRef = baseTip, branchTip
		key = job.repoKey + "\x00" + baseTip + "\x00" + branchTip
		cache := m.diffCache()
		cache.mu.Lock()
		hit, ok := cache.committed[key]
		cache.mu.Unlock()
		if ok {
			return hit
		}
	}
	var out committedDiff
	log, ok := m.gitOutput(ctx, job.gitDir, []string{"log", "--no-merges", "--cherry-pick", "--right-only",
		"--format=%x1e%h %s", "--name-only", baseRef + "..." + branchRef})
	if ok {
		out.ownedOK = true
		out.owned = ParseAgentOwnedLog(log)
		out.diffOK = true
		if len(out.owned) > 0 {
			args := append([]string{"diff", "--numstat", baseRef, branchRef, "--"}, literalPathspecs(out.owned)...)
			if numstat, ok := m.gitOutput(ctx, job.gitDir, args); ok {
				out.files, out.added, out.removed = SummarizeAgentNumstat(numstat)
			}
		}
	} else if numstat, ok := m.gitOutput(ctx, job.gitDir, []string{"diff", "--numstat", baseRef + "..." + branchRef}); ok {
		out.diffOK = true
		out.files, out.added, out.removed = SummarizeAgentNumstat(numstat)
	}
	if cacheable && out.ownedOK {
		cache := m.diffCache()
		cache.mu.Lock()
		cache.trim()
		cache.committed[key] = out
		cache.mu.Unlock()
	}
	return out
}

func (m *Module) aheadBehind(ctx context.Context, job agentDiffJob, session AgentDiffSession) (aheadBehind, bool) {
	baseTip, branchTip := job.tips[job.base], job.tips[session.Branch]
	cacheable := baseTip != "" && branchTip != ""
	baseRef, branchRef := job.base, session.Branch
	key := ""
	if cacheable {
		baseRef, branchRef = baseTip, branchTip
		key = job.repoKey + "\x00" + baseTip + "\x00" + branchTip
		cache := m.diffCache()
		cache.mu.Lock()
		hit, ok := cache.counts[key]
		cache.mu.Unlock()
		if ok {
			return hit, true
		}
	}
	out, ok := m.gitOutput(ctx, job.gitDir, []string{"rev-list", "--left-right", "--count", baseRef + "..." + branchRef})
	if !ok {
		return aheadBehind{}, false
	}
	parts := strings.Fields(string(out))
	if len(parts) < 2 {
		return aheadBehind{}, false
	}
	counts := aheadBehind{behind: numstatField(parts[0]), ahead: numstatField(parts[1])}
	if cacheable {
		cache := m.diffCache()
		cache.mu.Lock()
		cache.trim()
		cache.counts[key] = counts
		cache.mu.Unlock()
	}
	return counts, true
}

func (m *Module) mergeConflicts(ctx context.Context, job agentDiffJob, session AgentDiffSession) bool {
	baseTip, branchTip := job.tips[job.base], job.tips[session.Branch]
	cacheable := baseTip != "" && branchTip != ""
	baseRef, branchRef := job.base, session.Branch
	key := ""
	if cacheable {
		baseRef, branchRef = baseTip, branchTip
		key = job.repoKey + "\x00" + baseTip + "\x00" + branchTip
		cache := m.diffCache()
		cache.mu.Lock()
		hit, ok := cache.conflicts[key]
		cache.mu.Unlock()
		if ok {
			return hit
		}
	}
	ok, timedOut := m.gitOK(ctx, job.gitDir, []string{"merge-tree", "--write-tree", branchRef, baseRef})
	conflicted := !ok
	if cacheable && !timedOut {
		cache := m.diffCache()
		cache.mu.Lock()
		cache.trim()
		cache.conflicts[key] = conflicted
		cache.mu.Unlock()
	}
	return conflicted
}

func (m *Module) readAgentDiffStat(ctx context.Context, job agentDiffJob, session AgentDiffSession) AgentDiffStat {
	stat := newAgentDiffStat()
	if session.Branch == "forkmesh/pulls" || strings.HasPrefix(session.Branch, "forkmesh/pull/") {
		return stat
	}
	if session.PatchPath != "" {
		if patch := readSmallText(session.PatchPath); patch != "" {
			SummarizeAgentPatch(patch, &stat)
		}
	}
	dirtyTracked := map[string]bool{}
	var dirtyUntracked []string
	worktree := job.worktree
	if worktree != "" && isDirectory(worktree) {
		stat.Worktree = worktree
		if out, ok := m.gitOutput(ctx, worktree, []string{"status", "--porcelain", "-z", "--untracked-files=all"}); ok {
			entries := bytes.Split(out, []byte{0})
			dirty := 0
			for i := 0; i < len(entries); i++ {
				entry := entries[i]
				if len(entry) < 4 {
					continue
				}
				dirty++
				path := string(entry[3:])
				if bytes.HasPrefix(entry, []byte("??")) {
					dirtyUntracked = append(dirtyUntracked, path)
				} else {
					dirtyTracked[path] = true
				}
				if entry[0] == 'R' || entry[0] == 'C' {
					i++
				}
			}
			stat.Dirty = dirty
		}
	}
	if job.gitDir == "" || job.base == "" || session.Branch == "" || session.Branch == job.base {
		return stat
	}
	if ok, _ := m.gitOK(ctx, job.gitDir, []string{"rev-parse", "--verify", "--quiet", "refs/heads/" + session.Branch}); !ok {
		return stat
	}
	committed := m.committedDiff(ctx, job, session)
	if committed.ownedOK {
		stat.Files, stat.Added, stat.Removed = committed.files, committed.added, committed.removed
	} else if committed.diffOK {
		stat.Files, stat.Added, stat.Removed = committed.files, committed.added, committed.removed
	}
	if len(dirtyTracked) > 0 || len(dirtyUntracked) > 0 {
		files, added, removed := 0, 0, 0
		if committed.ownedOK {
			paths := map[string]bool{}
			for _, path := range committed.owned {
				paths[path] = true
			}
			for path := range dirtyTracked {
				paths[path] = true
			}
			if len(paths) > 0 {
				args := append([]string{"diff", "--numstat", job.base, "--"}, literalPathspecs(sortedSet(paths))...)
				if out, ok := m.gitOutput(ctx, worktree, args); ok {
					files, added, removed = SummarizeAgentNumstat(out)
				}
			}
		} else if out, ok := m.gitOutput(ctx, worktree, []string{"diff", "--numstat", "--merge-base", job.base}); ok {
			files, added, removed = SummarizeAgentNumstat(out)
		}
		for _, path := range dirtyUntracked {
			files++
			added += untrackedLines(filepath.Join(worktree, path))
		}
		stat.Files, stat.Added, stat.Removed = files, added, removed
	}
	if counts, ok := m.aheadBehind(ctx, job, session); ok {
		stat.Behind = counts.behind
		stat.Ahead = counts.ahead
		stat.Base = job.base
	}
	if (job.probe || job.idle) && stat.Behind > 0 && !session.Merged && m.mergeConflicts(ctx, job, session) {
		stat.Conflicted = true
	}
	return stat
}

func (m *Module) agentDiffStats(ctx context.Context, req *rpc.Request) (any, error) {
	if err := local(req); err != nil {
		return nil, err
	}
	var params agentDiffParams
	if len(req.Params) > 0 {
		if err := json.Unmarshal(req.Params, &params); err != nil {
			return nil, rpc.Fail("bad_request")
		}
	}
	gitDir, err := runDirectory(params.Path)
	if err != nil {
		return nil, err
	}
	if len(params.Sessions) > maxAgentDiffSessions {
		return nil, rpc.Fail("bad_request")
	}
	base := m.DefaultAgentBase(ctx, gitDir, params.ConfiguredBase, params.CheckedOut)
	worktrees := m.AgentWorktrees(ctx, gitDir)
	tips := m.AgentBranchTips(ctx, gitDir)
	repoKey := git.RepositoryKey(gitDir)
	signatures := make([]agentDiffSignature, 0, len(params.Sessions))
	live := make([]int, 0, len(params.Sessions))
	type work struct {
		session   AgentDiffSession
		job       agentDiffJob
		signature string
	}
	var jobs []work
	for _, session := range params.Sessions {
		live = append(live, session.ID)
		active := agentActive(session.Status)
		requested := strings.TrimSpace(session.BaseBranch)
		sessionBase := base
		if _, ok := tips[requested]; requested != "" && ok {
			sessionBase = requested
		}
		signature := AgentDiffSignature(session, sessionBase, tips)
		signatures = append(signatures, agentDiffSignature{ID: session.ID, Signature: signature})
		changed := session.Signature != signature
		selected := session.ID == params.Selected
		if !(active || selected || changed || !session.Known) {
			continue
		}
		jobs = append(jobs, work{session: session, signature: signature, job: agentDiffJob{
			gitDir: gitDir, repoKey: repoKey, base: sessionBase, worktree: worktrees[session.Branch], tips: tips,
			probe: selected, idle: !active && (selected || changed || !session.Known),
		}})
	}
	stats := make([]agentDiffEntry, len(jobs))
	next := make(chan int)
	var wg sync.WaitGroup
	for w := 0; w < min(agentDiffWorkers, max(1, len(jobs))); w++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for i := range next {
				item := jobs[i]
				stat := m.readAgentDiffStat(ctx, item.job, item.session)
				stats[i] = agentDiffEntry{ID: item.session.ID, Stat: stat, Signature: item.signature}
				if req.Peer != nil {
					req.Peer.Send(AgentDiffStatEvent, map[string]any{
						"token": params.Token, "id": item.session.ID, "stat": stat, "signature": item.signature,
					})
				}
			}
		}()
	}
	for i := range jobs {
		if ctx.Err() != nil {
			break
		}
		next <- i
	}
	close(next)
	wg.Wait()
	if ctx.Err() != nil {
		return nil, rpc.Fail("cancelled")
	}
	return map[string]any{
		"path": gitDir, "base": base, "token": params.Token,
		"stats": stats, "signatures": signatures, "liveIds": live,
	}, nil
}
