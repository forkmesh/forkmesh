package cleanup

import (
	"context"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"forkmesh.com/desktop-core/internal/coretest"
	"forkmesh.com/desktop-core/internal/git"
	"forkmesh.com/desktop-core/internal/rpc"
)

func run(t *testing.T, dir string, args ...string) string {
	t.Helper()
	cmd := exec.Command("git", args...)
	cmd.Dir = dir
	cmd.Env = append(os.Environ(), "GIT_AUTHOR_NAME=t", "GIT_AUTHOR_EMAIL=t@x", "GIT_COMMITTER_NAME=t", "GIT_COMMITTER_EMAIL=t@x",
		"GIT_CONFIG_GLOBAL=/dev/null", "GIT_CONFIG_SYSTEM=/dev/null")
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("git %v: %v\n%s", args, err, out)
	}
	return strings.TrimSpace(string(out))
}

func repoWithWorktree(t *testing.T) (string, string) {
	t.Helper()
	if _, err := exec.LookPath("git"); err != nil {
		t.Skip("git is not installed")
	}
	root, err := filepath.EvalSymlinks(t.TempDir())
	if err != nil {
		t.Fatal(err)
	}
	t.Setenv("HOME", filepath.Join(root, "home"))
	t.Setenv("GIT_CONFIG_GLOBAL", "/dev/null")
	t.Setenv("GIT_CONFIG_SYSTEM", "/dev/null")
	for _, name := range []string{"FORKMESH_HOME", "FORKMESH_PROJECTS_DIR", "FORKMESH_PROJECT_DIR", "FORKMESH_DATA_DIR", "FORKMESH_CONFIG_DIR"} {
		t.Setenv(name, "")
	}
	repo := filepath.Join(root, "repo")
	_ = os.MkdirAll(repo, 0o755)
	run(t, repo, "init", "-q", "-b", "main")
	_ = os.WriteFile(filepath.Join(repo, "a.txt"), []byte("a\n"), 0o644)
	run(t, repo, "add", "a.txt")
	run(t, repo, "commit", "-q", "-m", "first")
	worktree := filepath.Join(root, "wt")
	run(t, repo, "worktree", "add", "-q", "-b", "agent/42-fix", worktree)
	return repo, worktree
}

func module(t *testing.T) *Module {
	m := New()
	m.Git = &git.Runner{}
	m.Now = func() time.Time { return time.Date(2026, 10, 2, 9, 0, 0, 0, time.Local) }
	return m
}

func TestCleanWorktreeIsRemovedAndBranchRetiredToATrashRef(t *testing.T) {
	repo, worktree := repoWithWorktree(t)
	result := module(t).RemoveWorktree(context.Background(), WorktreeRemoval{RepoPath: repo, WorktreePath: worktree, RetireBranch: true})
	if !result.Removed || !result.BranchRetired || result.TrashRef != "refs/forkmesh/trash/cleanup/2026-10-02/agent/42-fix" {
		t.Fatalf("result %+v", result)
	}
	if _, err := os.Stat(worktree); !os.IsNotExist(err) {
		t.Fatal("worktree still on disk")
	}
	if out := run(t, repo, "branch", "--list", "agent/42-fix"); out != "" {
		t.Fatalf("branch survived: %q", out)
	}
	if out := run(t, repo, "rev-parse", "--verify", result.TrashRef); out == "" {
		t.Fatal("trash ref missing")
	}
}

func TestDirtyWorktreeIsKeptUnlessDiscardedAndWorkIsSaved(t *testing.T) {
	repo, worktree := repoWithWorktree(t)
	_ = os.WriteFile(filepath.Join(worktree, "a.txt"), []byte("changed\n"), 0o644)
	m := module(t)
	kept := m.RemoveWorktree(context.Background(), WorktreeRemoval{RepoPath: repo, WorktreePath: worktree})
	if kept.Removed || kept.Message != worktree+" has uncommitted changes; kept." {
		t.Fatalf("kept %+v", kept)
	}
	result := m.RemoveWorktree(context.Background(), WorktreeRemoval{RepoPath: repo, WorktreePath: worktree, DiscardChanges: true})
	if !result.Removed || result.WipRef != "refs/forkmesh/trash/cleanup/2026-10-02/agent/42-fix-wip" || result.BranchRetired {
		t.Fatalf("result %+v", result)
	}
	if out := run(t, repo, "show", result.WipRef+":a.txt"); out != "changed" {
		t.Fatalf("saved work %q", out)
	}
	if out := run(t, repo, "branch", "--list", "agent/42-fix"); out == "" {
		t.Fatal("the branch went although retiring was not asked for")
	}
}

func TestMainCheckoutAndStrangersAreRefused(t *testing.T) {
	repo, worktree := repoWithWorktree(t)
	m := module(t)
	if result := m.RemoveWorktree(context.Background(), WorktreeRemoval{RepoPath: repo, WorktreePath: repo}); result.Removed ||
		!strings.Contains(result.Message, "is not a linked worktree") {
		t.Fatalf("main checkout %+v", result)
	}
	other := filepath.Join(filepath.Dir(repo), "other")
	if result := m.RemoveWorktree(context.Background(), WorktreeRemoval{RepoPath: repo, WorktreePath: other}); result.Removed {
		t.Fatal("removed a path git does not know")
	}
	run(t, repo, "checkout", "-q", "-b", "keep")
	run(t, worktree, "checkout", "-q", "main")
	result := m.RemoveWorktree(context.Background(), WorktreeRemoval{RepoPath: repo, WorktreePath: worktree, RetireBranch: true})
	if !result.Removed || result.BranchRetired || result.Message != "Branch kept: main is a protected branch." {
		t.Fatalf("protected branch %+v", result)
	}
	if out := run(t, repo, "branch", "--list", "main"); out == "" {
		t.Fatal("main was deleted")
	}
}

func TestSandboxRemovalFollowsTheActionRunnerRules(t *testing.T) {
	root, err := filepath.EvalSymlinks(t.TempDir())
	if err != nil {
		t.Fatal(err)
	}
	base := filepath.Join(root, "sandboxes")
	for _, dir := range []string{"forkmesh-run-1/a", "forkmesh-run-2", "forkmesh-discard-3", "notes"} {
		_ = os.MkdirAll(filepath.Join(base, dir), 0o755)
	}
	_ = os.Symlink(filepath.Join(base, "forkmesh-run-1"), filepath.Join(base, "forkmesh-run-link"))
	claimed := map[string]bool{"forkmesh-run-2": true}
	cases := map[string]string{
		filepath.Join(base, "forkmesh-run-1"):     "",
		filepath.Join(base, "forkmesh-run-2"):     "a running Action still uses forkmesh-run-2",
		filepath.Join(base, "forkmesh-discard-3"): "",
		filepath.Join(base, "notes"):              filepath.Join(base, "notes") + " is not an Actions sandbox tree",
		filepath.Join(base, "forkmesh-run-link"):  filepath.Join(base, "forkmesh-run-link") + " is a link, not a sandbox tree",
		filepath.Join(root, "forkmesh-run-9"):     filepath.Join(root, "forkmesh-run-9") + " is not directly inside the Actions sandbox folder",
	}
	for path, want := range cases {
		if got := RemoveUnclaimedSandbox(base, path, claimed); got != want {
			t.Errorf("%s: %q, want %q", path, got, want)
		}
	}
	if _, err := os.Stat(filepath.Join(base, "forkmesh-run-1")); !os.IsNotExist(err) {
		t.Fatal("unclaimed sandbox survived")
	}
	if _, err := os.Stat(filepath.Join(base, "forkmesh-run-2")); err != nil {
		t.Fatal("claimed sandbox was removed")
	}
}

func TestDesktopOnlyMethods(t *testing.T) {
	core := coretest.New(t)
	coretest.Start(t, core, New())
	phone := coretest.NewPeer("phone", rpc.LAN)
	if _, err := coretest.Call(t, core, phone, "core.cleanup.removeSandbox", map[string]any{}); rpc.Code(err) != "forbidden" {
		t.Fatalf("phone: %v", err)
	}
	ui := coretest.NewPeer("ui", rpc.UI)
	var reply map[string]any
	coretest.MustCall(t, core, ui, "core.cleanup.removeSandbox", map[string]any{"base": t.TempDir(), "path": "/etc"}, &reply)
	if reply["error"] == "" {
		t.Fatal("removed /etc")
	}
	if _, err := coretest.Call(t, core, ui, "core.cleanup.removeWorktree", map[string]any{}); rpc.Code(err) != "bad_params" {
		t.Fatalf("empty request: %v", err)
	}
}
