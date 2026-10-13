package mirrornode

import (
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func cloneCore(t *testing.T, bare string) string {
	t.Helper()
	core := filepath.Join(t.TempDir(), "core.git")
	run(t, ".", "git", "clone", "-q", "--bare", bare, core)
	return core
}

func commitToCore(t *testing.T, core, name, content string) string {
	t.Helper()
	work := filepath.Join(t.TempDir(), "work")
	run(t, ".", "git", "clone", "-q", core, work)
	run(t, work, "git", "config", "user.email", "test@example.test")
	run(t, work, "git", "config", "user.name", "Test")
	if err := os.WriteFile(filepath.Join(work, name), []byte(content), 0600); err != nil {
		t.Fatal(err)
	}
	run(t, work, "git", "add", name)
	run(t, work, "git", "commit", "-q", "-m", name)
	run(t, work, "git", "push", "-q", "origin", "main")
	return run(t, work, "git", "rev-parse", "main")
}

func coreMain(t *testing.T, core string) string {
	t.Helper()
	return run(t, ".", "git", "--git-dir="+core, "rev-parse", "refs/heads/main")
}

func parentsOf(t *testing.T, core, commit string) []string {
	t.Helper()
	return strings.Fields(run(t, ".", "git", "--git-dir="+core, "show", "-s", "--format=%P", commit))
}

func TestDivergentCoreMainsMergeToTheSameCommitOnEveryCore(t *testing.T) {
	_, bare := makeBareRepo(t)
	n1, n2, n3 := cloneCore(t, bare), cloneCore(t, bare), cloneCore(t, bare)
	runnerMerge := commitToCore(t, n1, "runner.txt", "runner merge\n")
	desktopPush := commitToCore(t, n2, "desktop.txt", "desktop push\n")
	run(t, ".", "git", "--git-dir="+n3, "fetch", "-q", n2, "main:main")
	ctx := context.Background()
	if err := syncRepository(ctx, Repository{GitDir: n1}, []string{n2}, ""); err != nil {
		t.Fatal(err)
	}
	if err := syncRepository(ctx, Repository{GitDir: n3}, []string{n1}, ""); err != nil {
		t.Fatal(err)
	}
	if err := syncRepository(ctx, Repository{GitDir: n2}, []string{n3}, ""); err != nil {
		t.Fatal(err)
	}
	merged := coreMain(t, n1)
	if coreMain(t, n2) != merged || coreMain(t, n3) != merged {
		t.Fatalf("cores did not converge: %s %s %s", merged, coreMain(t, n2), coreMain(t, n3))
	}
	parents := parentsOf(t, n1, merged)
	if len(parents) != 2 || !((parents[0] == runnerMerge && parents[1] == desktopPush) ||
		(parents[0] == desktopPush && parents[1] == runnerMerge)) {
		t.Fatalf("merge parents %v", parents)
	}
	files := run(t, ".", "git", "--git-dir="+n1, "ls-tree", "--name-only", merged)
	if !strings.Contains(files, "runner.txt") || !strings.Contains(files, "desktop.txt") {
		t.Fatalf("merge lost a side: %s", files)
	}

	other := cloneCore(t, bare)
	run(t, ".", "git", "--git-dir="+other, "fetch", "-q", n1, runnerMerge+":refs/heads/main")
	twin := cloneCore(t, bare)
	run(t, ".", "git", "--git-dir="+twin, "fetch", "-q", n2, desktopPush+":refs/heads/main")
	if err := syncRepository(ctx, Repository{GitDir: twin}, []string{other}, ""); err != nil {
		t.Fatal(err)
	}
	if coreMain(t, twin) != merged {
		t.Fatalf("the merge is not deterministic: %s vs %s", coreMain(t, twin), merged)
	}
}

func TestConflictingCoreMainsStillConvergeOnTheNewerCommit(t *testing.T) {
	_, bare := makeBareRepo(t)
	n1, n2 := cloneCore(t, bare), cloneCore(t, bare)
	commitToCore(t, n1, "README.md", "n1\n")
	run(t, ".", "git", "--git-dir="+n1, "update-ref", "refs/heads/main",
		run(t, ".", "git", "--git-dir="+n1, "commit-tree", "-p", "main",
			"-m", "older", run(t, ".", "git", "--git-dir="+n1, "rev-parse", "main^{tree}")))
	n2Tip := commitToCore(t, n2, "README.md", "n2\n")
	n1Tip := coreMain(t, n1)
	if err := syncRepository(context.Background(), Repository{GitDir: n1}, []string{n2}, ""); err != nil {
		t.Fatal(err)
	}
	want := n2Tip
	if gitCommitTime(context.Background(), n1, n1Tip) > gitCommitTime(context.Background(), n1, n2Tip) ||
		(gitCommitTime(context.Background(), n1, n1Tip) == gitCommitTime(context.Background(), n1, n2Tip) && n1Tip > n2Tip) {
		want = n1Tip
	}
	if got := coreMain(t, n1); got != want {
		t.Fatalf("conflicting mains chose %s, want the newer %s", got, want)
	}
}

func TestADivergedSideBranchStillConvergesOnTheNewerCommit(t *testing.T) {
	_, bare := makeBareRepo(t)
	run(t, ".", "git", "--git-dir="+bare, "branch", "feature")
	n1, n2 := cloneCore(t, bare), cloneCore(t, bare)
	tree := run(t, ".", "git", "--git-dir="+n1, "rev-parse", "main^{tree}")
	left := run(t, ".", "git", "--git-dir="+n1, "commit-tree", "-p", "feature", "-m", "left", tree)
	run(t, ".", "git", "--git-dir="+n1, "update-ref", "refs/heads/feature", left)
	right := run(t, ".", "git", "--git-dir="+n2, "commit-tree", "-p", "feature", "-m", "right", tree)
	run(t, ".", "git", "--git-dir="+n2, "update-ref", "refs/heads/feature", right)
	if err := syncRepository(context.Background(), Repository{GitDir: n1}, []string{n2}, ""); err != nil {
		t.Fatal(err)
	}
	got := run(t, ".", "git", "--git-dir="+n1, "rev-parse", "refs/heads/feature")
	if got != left && got != right {
		t.Fatalf("a side branch was merged: %s", got)
	}
}

func TestACorePicksUpAPeerMainTheEdgeDoesNotServe(t *testing.T) {
	_, bare := makeBareRepo(t)
	edge, peer, self := cloneCore(t, bare), cloneCore(t, bare), cloneCore(t, bare)
	runnerMerge := commitToCore(t, peer, "runner.txt", "runner merge\n")
	if err := syncRepository(context.Background(), Repository{GitDir: self},
		[]string{edge, peer}, ""); err != nil {
		t.Fatal(err)
	}
	if got := coreMain(t, self); got != runnerMerge {
		t.Fatalf("main %s, want the peer's %s", got, runnerMerge)
	}
	if refs := run(t, ".", "git", "--git-dir="+self, "for-each-ref", "--format=%(refname)",
		peerMainNamespace); refs != "" {
		t.Fatalf("peer staging refs leaked: %s", refs)
	}
	later := commitToCore(t, peer, "later.txt", "later\n")
	if err := syncRepository(context.Background(), Repository{GitDir: self},
		[]string{edge, peer}, ""); err != nil {
		t.Fatal(err)
	}
	if got := coreMain(t, self); got != runnerMerge {
		t.Fatalf("peer mains were read again inside %s: %s (later %s)", peerMainInterval, got, later)
	}
}

func TestPeerMainsAreReadOnlyFromUpstreamsAfterTheOneUsed(t *testing.T) {
	if got := laterUpstreams([]string{"edge", "n2", "n3"}, "n2"); len(got) != 1 || got[0] != "n3" {
		t.Fatalf("laterUpstreams = %v", got)
	}
	if got := laterUpstreams([]string{"edge", "n2"}, "n3"); got != nil {
		t.Fatalf("laterUpstreams for an unknown upstream = %v", got)
	}
}
