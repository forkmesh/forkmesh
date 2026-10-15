package cleanup

import (
	"context"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"time"

	"forkmesh.com/desktop-core/internal/actions"
	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/git"
	"forkmesh.com/desktop-core/internal/ownergate"
	"forkmesh.com/desktop-core/internal/rpc"
	"forkmesh.com/desktop-core/internal/safedelete"
)

const (
	owner                 = "cleanup"
	sandboxRunPrefix      = "forkmesh-run-"
	sandboxDiscardPrefix  = "forkmesh-discard-"
	cleanupTrashRefPrefix = "refs/forkmesh/trash/cleanup/"
)

type Module struct {
	Git      *git.Runner
	Now      func() time.Time
	ProcRoot string
	Ccache   string

	core *daemon.Core
	base context.Context
	jobs jobs
}

func New() *Module { return &Module{} }

func (m *Module) Name() string { return owner }

func (m *Module) Start(ctx context.Context, core *daemon.Core) error {
	m.core = core
	m.base = ctx
	if m.Git == nil {
		m.Git = &git.Runner{Logger: core.Logger}
	}
	core.Registry.Handle("core.cleanup.removeWorktree", owner, ownergate.DesktopOnly(m.handleRemoveWorktree))
	core.Registry.Handle("core.cleanup.removeSandbox", owner, ownergate.DesktopOnly(m.handleRemoveSandbox))
	m.registerJobs()
	return nil
}

func (m *Module) now() time.Time {
	if m.Now != nil {
		return m.Now()
	}
	return time.Now()
}

type WorktreeRemoval struct {
	RepoPath       string `json:"repoPath"`
	WorktreePath   string `json:"worktreePath"`
	Branch         string `json:"branch"`
	DiscardChanges bool   `json:"discardChanges"`
	RetireBranch   bool   `json:"retireBranch"`
}

type WorktreeResult struct {
	Removed       bool     `json:"removed"`
	BranchRetired bool     `json:"branchRetired"`
	TrashRef      string   `json:"trashRef"`
	WipRef        string   `json:"wipRef"`
	Message       string   `json:"message"`
	Logs          []string `json:"logs"`
}

type worktreeEntry struct {
	path     string
	head     string
	branch   string
	bare     bool
	detached bool
}

func parseWorktreeList(porcelain string) []worktreeEntry {
	var entries []worktreeEntry
	var current worktreeEntry
	open := false
	for _, raw := range strings.Split(porcelain, "\n") {
		line := strings.TrimSuffix(raw, "\r")
		switch {
		case line == "":
			if open {
				entries = append(entries, current)
			}
			current = worktreeEntry{}
			open = false
		case strings.HasPrefix(line, "worktree "):
			if open {
				entries = append(entries, current)
			}
			current = worktreeEntry{path: line[9:]}
			open = true
		case strings.HasPrefix(line, "HEAD "):
			current.head = line[5:]
		case strings.HasPrefix(line, "branch "):
			current.branch = line[7:]
		case line == "bare":
			current.bare = true
		case line == "detached":
			current.detached = true
		}
	}
	if open {
		entries = append(entries, current)
	}
	return entries
}

func cleanAbsolute(path string) string {
	absolute, err := filepath.Abs(path)
	if err != nil {
		return filepath.Clean(path)
	}
	return filepath.Clean(absolute)
}

func samePath(a, b string) bool {
	if strings.TrimSpace(a) == "" || strings.TrimSpace(b) == "" {
		return false
	}
	if cleanAbsolute(a) == cleanAbsolute(b) {
		return true
	}
	ca, errA := filepath.EvalSymlinks(a)
	cb, errB := filepath.EvalSymlinks(b)
	return errA == nil && errB == nil && ca == cb
}

var branchPattern = regexp.MustCompile(`^[A-Za-z0-9._/+-]{1,200}$`)

func validBranch(branch string) bool {
	if !branchPattern.MatchString(branch) {
		return false
	}
	if strings.HasPrefix(branch, "-") || strings.HasPrefix(branch, "/") || strings.HasPrefix(branch, ".") ||
		strings.HasSuffix(branch, "/") || strings.HasSuffix(branch, ".") || strings.HasSuffix(branch, ".lock") ||
		strings.Contains(branch, "..") || strings.Contains(branch, "//") || branch == "HEAD" {
		return false
	}
	for _, part := range strings.Split(branch, "/") {
		if strings.HasPrefix(part, ".") || strings.HasSuffix(part, ".lock") {
			return false
		}
	}
	return true
}

func branchRefusal(branch, mainCheckoutBranch, defaultBranch string) string {
	if !validBranch(branch) {
		return fmt.Sprintf("%s is not a usable branch name", branch)
	}
	if branch == "main" || branch == "master" {
		return fmt.Sprintf("%s is a protected branch", branch)
	}
	if mainCheckoutBranch != "" && branch == mainCheckoutBranch {
		return fmt.Sprintf("%s is checked out in the main checkout", branch)
	}
	if defaultBranch != "" && branch == defaultBranch {
		return fmt.Sprintf("%s is the default branch", branch)
	}
	return ""
}

func shortBranch(ref string) string {
	return strings.TrimPrefix(ref, "refs/heads/")
}

func (m *Module) git(ctx context.Context, dir string, args ...string) (string, bool, string) {
	result := m.Git.Run(ctx, dir, args, git.Options{})
	errText := result.Stderr
	if errText == "" {
		errText = result.Error
	}
	return strings.TrimSpace(string(result.Output)), result.OK, errText
}

func (m *Module) handleRemoveWorktree(ctx context.Context, req *rpc.Request) (any, error) {
	var request WorktreeRemoval
	if err := req.Bind(&request); err != nil {
		return nil, err
	}
	if strings.TrimSpace(request.RepoPath) == "" || strings.TrimSpace(request.WorktreePath) == "" {
		return nil, rpc.Fail("bad_params")
	}
	return m.RemoveWorktree(ctx, request), nil
}

func (m *Module) RemoveWorktree(ctx context.Context, request WorktreeRemoval) WorktreeResult {
	result := WorktreeResult{Logs: []string{}}
	listing, ok, _ := m.git(ctx, request.RepoPath, "worktree", "list", "--porcelain")
	var entry *worktreeEntry
	mainBranch := ""
	if ok {
		entries := parseWorktreeList(listing)
		for index := range entries {
			if index == 0 {
				mainBranch = entries[index].branch
				continue
			}
			if samePath(entries[index].path, request.WorktreePath) {
				entry = &entries[index]
				break
			}
		}
	}
	if entry == nil {
		result.Message = fmt.Sprintf("%s is not a linked worktree of %s; nothing was removed.", request.WorktreePath, request.RepoPath)
		return result
	}
	branch := shortBranch(entry.branch)
	status, ok, _ := m.git(ctx, request.WorktreePath, "--no-optional-locks", "status", "--porcelain")
	if !ok {
		result.Message = fmt.Sprintf("Could not read the status of %s.", request.WorktreePath)
		return result
	}
	dirty := status != ""
	if dirty && !request.DiscardChanges {
		result.Message = fmt.Sprintf("%s has uncommitted changes; kept.", request.WorktreePath)
		return result
	}
	stamp := m.now().Format("2006-01-02")
	label := branch
	if label == "" {
		label = filepath.Base(request.WorktreePath)
	}
	if dirty {
		stash, _, _ := m.git(ctx, request.WorktreePath, "stash", "create", "forkmesh cleanup: uncommitted work")
		if stash != "" {
			ref := cleanupTrashRefPrefix + stamp + "/" + label + "-wip"
			if _, ok, _ := m.git(ctx, request.RepoPath, "update-ref", ref, stash); ok {
				result.WipRef = ref
			}
		}
	}
	tip := ""
	if request.RetireBranch && branch != "" {
		if refusal := branchRefusal(branch, shortBranch(mainBranch), "main"); refusal != "" {
			result.Message = fmt.Sprintf("Branch kept: %s.", refusal)
		} else {
			value, ok, _ := m.git(ctx, request.RepoPath, "rev-parse", "--verify", "refs/heads/"+branch)
			if ok && value != "" {
				tip = value
				ref := cleanupTrashRefPrefix + stamp + "/" + branch
				if _, ok, _ := m.git(ctx, request.RepoPath, "update-ref", ref, tip); ok {
					result.TrashRef = ref
				}
			}
		}
	}
	if dirty {
		run := func(dir string, args []string) (bool, string) {
			_, ok, errText := m.git(ctx, dir, args...)
			return ok, errText
		}
		removed, _, err := safedelete.ForceRemoveWorktree(run, request.RepoPath, request.WorktreePath)
		if err != nil {
			result.Message = fmt.Sprintf("git refused to remove %s: %v", request.WorktreePath, err)
			return result
		}
		if !removed {
			if _, statErr := os.Stat(request.WorktreePath); statErr == nil {
				result.Message = fmt.Sprintf("git refused to remove %s.", request.WorktreePath)
				return result
			}
			m.git(ctx, request.RepoPath, "worktree", "prune")
			result.Logs = append(result.Logs, fmt.Sprintf("Git: worktree %s was already removed; pruned its entry and finished the cleanup.", request.WorktreePath))
		}
	} else {
		if _, ok, errText := m.git(ctx, request.RepoPath, "worktree", "remove", request.WorktreePath); !ok {
			result.Message = fmt.Sprintf("git refused to remove %s: %s", request.WorktreePath, strings.TrimSpace(errText))
			return result
		}
	}
	result.Removed = true
	if result.TrashRef != "" {
		_, ok, _ := m.git(ctx, request.RepoPath, "update-ref", "-d", "refs/heads/"+branch, tip)
		result.BranchRetired = ok
	}
	return result
}

func canonical(path string) string {
	absolute, err := filepath.Abs(path)
	if err != nil {
		return ""
	}
	resolved, err := filepath.EvalSymlinks(absolute)
	if err != nil {
		return ""
	}
	return resolved
}

func RemoveUnclaimedSandbox(base, path string, claimed map[string]bool) string {
	name := filepath.Base(path)
	if !claimed[name] {
		return actions.RemoveUnclaimedSandbox(base, path)
	}
	parent := canonical(filepath.Dir(cleanAbsolute(path)))
	root := canonical(base)
	if root == "" || parent != root {
		return fmt.Sprintf("%s is not directly inside the Actions sandbox folder", path)
	}
	if info, err := os.Lstat(path); err == nil && info.Mode()&os.ModeSymlink != 0 {
		return fmt.Sprintf("%s is a link, not a sandbox tree", path)
	}
	if !strings.HasPrefix(name, sandboxRunPrefix) && !strings.HasPrefix(name, sandboxDiscardPrefix) {
		return fmt.Sprintf("%s is not an Actions sandbox tree", path)
	}
	return fmt.Sprintf("a running Action still uses %s", name)
}

func (m *Module) handleRemoveSandbox(ctx context.Context, req *rpc.Request) (any, error) {
	var params struct {
		Base    string   `json:"base"`
		Path    string   `json:"path"`
		Claimed []string `json:"claimed"`
	}
	if err := req.Bind(&params); err != nil {
		return nil, err
	}
	claimed := map[string]bool{}
	for _, name := range params.Claimed {
		claimed[name] = true
	}
	return map[string]any{"error": RemoveUnclaimedSandbox(params.Base, params.Path, claimed)}, nil
}
