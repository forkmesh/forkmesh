package mirrornode

import (
	"context"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
)

func (f *mergeFixture) publishPatchPull(t *testing.T, mbox, patch string, extra ...string) {
	t.Helper()
	run(t, f.source, "git", "checkout", "-q", "forkmesh/pulls")
	pullDir := filepath.Join(f.source, ".forkmesh", "pulls", "7")
	writeFile(t, filepath.Join(pullDir, "pull.md"),
		pullRecord(7, "Add feature", "main", "contributor:feature", "alice", false, "", "", extra...))
	if mbox != "" {
		writeFile(t, filepath.Join(pullDir, "commits.mbox"), mbox+"\n")
	}
	if patch != "" {
		writeFile(t, filepath.Join(pullDir, "changes.patch"), patch+"\n")
	}
	run(t, f.source, "git", "add", "-A")
	run(t, f.source, "git", "commit", "-q", "-m", "pull #7: patch backed")
	run(t, f.source, "git", "push", "-q", f.bare, "forkmesh/pulls:refs/heads/forkmesh/pulls")
	f.pullsOid = run(t, f.source, "git", "rev-parse", "forkmesh/pulls")
	run(t, f.source, "git", "checkout", "-q", "main")
}

func objectExists(dir, object string) bool {
	return exec.Command("git", "--git-dir="+dir, "cat-file", "-e", object).Run() == nil
}

func TestMergeExecutorMergesAPatchPullFromTheBaseItWasCutFrom(t *testing.T) {
	f := newMergeFixture(t)
	writeFile(t, filepath.Join(f.source, "guide.txt"), "alpha\nbeta\ngamma\n")
	run(t, f.source, "git", "add", "guide.txt")
	run(t, f.source, "git", "commit", "-q", "-m", "guide")
	cut := run(t, f.source, "git", "rev-parse", "HEAD")

	run(t, f.source, "git", "checkout", "-q", "-b", "contrib")
	writeFile(t, filepath.Join(f.source, "guide.txt"), "alpha\nbeta draft\ngamma\n")
	run(t, f.source, "git", "commit", "-q", "-am", "draft")
	writeFile(t, filepath.Join(f.source, "guide.txt"), "alpha\nbeta\ngamma\n")
	writeFile(t, filepath.Join(f.source, "extra.txt"), "extra\n")
	run(t, f.source, "git", "add", "-A")
	run(t, f.source, "git", "commit", "-q", "-m", "settle")
	writeFile(t, filepath.Join(f.source, "guide.txt"), "alpha\nbeta\ngamma\ndelta\n")
	run(t, f.source, "git", "commit", "-q", "-am", "delta")
	mbox := run(t, f.source, "git", "format-patch", "--stdout", cut+"..contrib")
	patch := run(t, f.source, "git", "diff", "--binary", cut, "contrib")

	run(t, f.source, "git", "checkout", "-q", "main")
	if err := os.MkdirAll(filepath.Join(f.source, "docs"), 0700); err != nil {
		t.Fatal(err)
	}
	run(t, f.source, "git", "mv", "guide.txt", "docs/guide.txt")
	writeFile(t, filepath.Join(f.source, "docs", "guide.txt"), "alpha\nbeta main\ngamma\n")
	run(t, f.source, "git", "commit", "-q", "-am", "move the guide")
	run(t, f.source, "git", "push", "-q", f.bare, "main:refs/heads/main")
	f.mainOid = run(t, f.source, "git", "rev-parse", "main")

	worktree := filepath.Join(t.TempDir(), "replay")
	run(t, ".", "git", "--git-dir="+f.bare, "worktree", "add", "-q", "--detach", worktree, f.mainOid)
	mboxPath := filepath.Join(t.TempDir(), "series.mbox")
	writeFile(t, mboxPath, mbox+"\n")
	if exec.Command("git", "-C", worktree, "am", "--3way", mboxPath).Run() == nil {
		t.Fatal("the series replays onto the moved base; the test no longer shows the difference")
	}
	_ = exec.Command("git", "-C", worktree, "am", "--abort").Run()
	run(t, ".", "git", "--git-dir="+f.bare, "worktree", "remove", "--force", worktree)

	f.publishPatchPull(t, mbox, patch, "creationBaseOid: "+cut)
	result := f.request(t, "execute", "merge", map[string]any{"expectedHeadOid": f.pullsOid})
	if result["status"] != "merged" {
		t.Fatalf("patch pull did not merge: %+v", result)
	}
	baseAfter := result["baseAfter"].(string)
	parents := strings.Fields(run(t, ".", "git", "--git-dir="+f.bare, "show", "-s", "--format=%P", baseAfter))
	if len(parents) != 2 || parents[0] != f.mainOid {
		t.Fatalf("merge parents %v", parents)
	}
	head := parents[1]
	if got := f.show(t, baseAfter, "docs/guide.txt"); got != "alpha\nbeta main\ngamma\ndelta" {
		t.Fatalf("merged guide:\n%s", got)
	}
	if f.show(t, baseAfter, "extra.txt") != "extra" || objectExists(f.bare, baseAfter+":guide.txt") {
		t.Fatal("merged tree is wrong")
	}
	if count := run(t, ".", "git", "--git-dir="+f.bare, "rev-list", "--count", cut+".."+head); count != "3" ||
		!gitIsAncestor(context.Background(), f.bare, cut, head) {
		t.Fatalf("pull head is not the series on its own base: %s commits", count)
	}
	identity := run(t, ".", "git", "--git-dir="+f.bare, "show", "-s", "--format=%an|%cn|%ce", head)
	if identity != "Test|"+pullHeadCommitterName+"|"+pullHeadCommitterEmail {
		t.Fatalf("pull head identity %q", identity)
	}
	record := f.show(t, result["pullsAfter"].(string), ".forkmesh/pulls/7/pull.md")
	if !strings.Contains(record, "\nmergeHead: "+head+"\n") || !strings.Contains(record, "\nmergeBase: "+f.mainOid+"\n") {
		t.Fatalf("merged record:\n%s", record)
	}
	again, err := replayPullSeries(context.Background(), f.bare, t.TempDir(), os.DevNull, cut, []byte(mbox+"\n"))
	if err != nil || again != head {
		t.Fatalf("replaying the series again gave %q (%v), want %s", again, err, head)
	}
}

func TestMergeExecutorNeverReplaysOntoABaseOutsideTheTargetBranch(t *testing.T) {
	f := newMergeFixture(t)
	run(t, f.source, "git", "checkout", "-q", "-b", "side", "main")
	writeFile(t, filepath.Join(f.source, "secret.txt"), "unreviewed\n")
	run(t, f.source, "git", "add", "secret.txt")
	run(t, f.source, "git", "commit", "-q", "-m", "unreviewed")
	run(t, f.source, "git", "push", "-q", f.bare, "side:refs/heads/side")
	side := run(t, f.source, "git", "rev-parse", "side")
	run(t, f.source, "git", "checkout", "-q", "main")
	mbox := run(t, f.source, "git", "format-patch", "--stdout", "main..feature")

	f.publishPatchPull(t, mbox, "", "creationBaseOid: "+side)
	result := f.request(t, "execute", "merge", map[string]any{"expectedHeadOid": f.pullsOid})
	if result["status"] != "merged" {
		t.Fatalf("patch pull did not merge: %+v", result)
	}
	baseAfter := result["baseAfter"].(string)
	if f.show(t, baseAfter, "feature.txt") != "feature" {
		t.Fatal("reviewed change is missing")
	}
	if objectExists(f.bare, baseAfter+":secret.txt") || gitIsAncestor(context.Background(), f.bare, side, baseAfter) {
		t.Fatal("a signed base outside main brought its own history into the merge")
	}
}

func TestPullIntakeRecordsACommitHeadEveryCoreDerivesAlike(t *testing.T) {
	ctx := context.Background()
	source, bare := makeBareRepo(t)
	mainOid := run(t, source, "git", "rev-parse", "main")
	run(t, source, "git", "checkout", "-q", "-b", "contrib")
	writeFile(t, filepath.Join(source, "contrib.txt"), "contribution\n")
	run(t, source, "git", "add", "contrib.txt")
	run(t, source, "git", "commit", "-q", "-m", "contribute")
	mbox := run(t, source, "git", "format-patch", "--stdout", "main..contrib")
	patch := run(t, source, "git", "diff", "--binary", "main", "contrib")

	repo := Repository{Owner: "forkmesh", Name: "forkmesh", GitDir: bare}
	number, err := materializeDirectCollaboration(ctx, repo, "pulls", collaborationItem{
		Number: 5,
		Pull: map[string]any{
			"title": "Contribute", "base": "main", "head": "contributor:contrib",
			"patch": patch + "\n", "commits": mbox + "\n", "author": "carol",
			"authorName": "Carol", "ts": 1700000005000, "sig": "carol-signature",
		},
	}, "mirror9")
	if err != nil {
		t.Fatal(err)
	}
	head := run(t, ".", "git", "--git-dir="+bare, "rev-parse", "--verify", pullHeadRef(number))
	record := run(t, ".", "git", "--git-dir="+bare, "show",
		"refs/heads/forkmesh/pulls:.forkmesh/pulls/"+strconv.Itoa(number)+"/pull.md")
	if !strings.Contains(record, "\nheadBaseOid: "+mainOid+"\n") || !strings.Contains(record, "\nheadOid: "+head+"\n") {
		t.Fatalf("intake record:\n%s", record)
	}
	isolated := run(t, ".", "git", "--git-dir="+bare, "show",
		"refs/heads/forkmesh/pull/"+strconv.Itoa(number)+":.forkmesh/pulls/"+strconv.Itoa(number)+"/pull.md")
	if !strings.Contains(isolated, "\nheadOid: "+head+"\n") {
		t.Fatalf("isolated record:\n%s", isolated)
	}
	if run(t, ".", "git", "--git-dir="+bare, "show", head+":contrib.txt") != "contribution" {
		t.Fatal("pull head lacks the contribution")
	}

	peer := filepath.Join(t.TempDir(), "peer.git")
	run(t, ".", "git", "clone", "-q", "--bare", "--no-local", "file://"+bare, peer)
	if objectExists(peer, head) {
		t.Fatal("the peer already has the head; the test no longer shows a re-derivation")
	}
	published, err := ensurePullHeadRefs(ctx, peer)
	if err != nil || published != 1 {
		t.Fatalf("published %d heads: %v", published, err)
	}
	if got := run(t, ".", "git", "--git-dir="+peer, "rev-parse", "--verify", pullHeadRef(number)); got != head {
		t.Fatalf("peer derived %s, intake recorded %s", got, head)
	}
	if again, err := ensurePullHeadRefs(ctx, peer); err != nil || again != 0 {
		t.Fatalf("second sweep published %d: %v", again, err)
	}
}

const pullHeadVectorPatch = "diff --git a/shared.txt b/shared.txt\n" +
	"index 5626abf..e265e82 100644\n" +
	"--- a/shared.txt\n" +
	"+++ b/shared.txt\n" +
	"@@ -1 +1,2 @@\n" +
	" one\n" +
	"+contributor\n"

const pullHeadVectorMbox = "From 0000000000000000000000000000000000000000 Mon Sep 17 00:00:00 2001\n" +
	"From: Vector <vector@example.test>\n" +
	"Date: Tue, 1 Sep 2026 11:00:00 +0000\n" +
	"Subject: [PATCH] contribution\n" +
	"\n" +
	"---\n" +
	pullHeadVectorPatch

func pullHeadVectorRepository(t *testing.T) (string, string) {
	t.Helper()
	ctx := context.Background()
	bare := filepath.Join(t.TempDir(), "vector.git")
	run(t, ".", "git", "init", "-q", "--bare", "-b", "main", bare)
	blob, err := gitInputValue(ctx, bare, []byte("one\n"), nil, "hash-object", "-w", "--stdin")
	if err != nil {
		t.Fatal(err)
	}
	tree, err := gitInputValue(ctx, bare, []byte("100644 blob "+blob+"\tshared.txt\n"), nil, "mktree")
	if err != nil {
		t.Fatal(err)
	}
	base, err := gitInputValue(ctx, bare, []byte("base\n"), []string{
		"GIT_AUTHOR_NAME=Vector", "GIT_AUTHOR_EMAIL=vector@example.test",
		"GIT_AUTHOR_DATE=2026-09-01T10:00:00Z", "GIT_COMMITTER_NAME=Vector",
		"GIT_COMMITTER_EMAIL=vector@example.test", "GIT_COMMITTER_DATE=2026-09-01T10:00:00Z",
	}, "commit-tree", tree)
	if err != nil {
		t.Fatal(err)
	}
	run(t, ".", "git", "--git-dir="+bare, "update-ref", "refs/heads/main", base)
	return bare, base
}

func TestPullHeadReplayMatchesTheDesktopVector(t *testing.T) {
	ctx := context.Background()
	bare, base := pullHeadVectorRepository(t)
	if base != "1d1c53be6d9b05fa8a531653a4a940a6c19da46e" {
		t.Fatalf("vector base %s", base)
	}
	head, err := replayPullSeries(ctx, bare, t.TempDir(), os.DevNull, base, []byte(pullHeadVectorMbox))
	if err != nil || head != "0231062d32ad46550ba26e9ed7b812c49bb05aaf" {
		t.Fatalf("mbox head %s (%v)", head, err)
	}
	values := map[string]string{"authorName": "Vector", "author": "vectorkey", "ts": "1788260400000", "title": "contribution"}
	synthetic, err := replayPullSeries(ctx, bare, t.TempDir(), os.DevNull, base,
		syntheticPullMbox(values, []byte(pullHeadVectorPatch)))
	if err != nil || synthetic != "5f915f9b52b6a2cfa6fc6e68d01496ab24f3e6fd" {
		t.Fatalf("synthetic head %s (%v)", synthetic, err)
	}
}
