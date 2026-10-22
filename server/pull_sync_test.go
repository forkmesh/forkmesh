package mirrornode

import (
	"context"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
)

type pullSyncFixture struct {
	source, bare string
	repo         Repository
	mainOid      string
	number       int
}

func newPullSyncFixture(t *testing.T, head string) *pullSyncFixture {
	t.Helper()
	ctx := context.Background()
	source, bare := makeBareRepo(t)
	f := &pullSyncFixture{source: source, bare: bare,
		repo: Repository{Owner: "forkmesh", Name: "forkmesh", GitDir: bare}}
	f.mainOid = run(t, source, "git", "rev-parse", "main")
	run(t, source, "git", "checkout", "-q", "-b", "work")
	writeFile(t, filepath.Join(source, "work.txt"), "first\n")
	run(t, source, "git", "add", "work.txt")
	run(t, source, "git", "commit", "-q", "-m", "first")
	run(t, source, "git", "push", "-q", bare, "work:refs/heads/pr/carol/work")
	mbox := run(t, source, "git", "format-patch", "--stdout", "main..work")
	patch := run(t, source, "git", "diff", "--binary", "main", "work")
	number, err := materializeDirectCollaboration(ctx, f.repo, "pulls", collaborationItem{
		Number: 3,
		Pull: map[string]any{
			"title": "Work", "base": "main", "head": head,
			"patch": patch + "\n", "commits": mbox + "\n", "author": "carol",
			"authorName": "Carol", "ts": 1700000005000, "sig": "carol-signature",
		},
	}, "mirror9")
	if err != nil {
		t.Fatal(err)
	}
	f.number = number
	return f
}

func (f *pullSyncFixture) push(t *testing.T, name, content string) string {
	t.Helper()
	writeFile(t, filepath.Join(f.source, name), content)
	run(t, f.source, "git", "add", name)
	run(t, f.source, "git", "commit", "-q", "-m", "change "+name)
	run(t, f.source, "git", "push", "-q", "-f", f.bare, "work:refs/heads/pr/carol/work")
	return run(t, f.source, "git", "rev-parse", "work")
}

func (f *pullSyncFixture) synchronize(t *testing.T, before, after string) {
	t.Helper()
	_, err := materializeDirectCollaboration(context.Background(), f.repo, "pulls", collaborationItem{
		Number: f.number,
		Event: map[string]any{
			"type": "synchronize", "id": "synchronize-" + strconv.Itoa(f.number) + "-" + after[:16],
			"author": "carol", "authorName": "carol", "ts": 1700000009,
			"ref": "refs/heads/pr/carol/work", "before": before, "after": after,
			"commits": 1, "body": "pushed 1 commit", "sig": "",
		},
	}, "mirror9")
	if err != nil {
		t.Fatal(err)
	}
}

func (f *pullSyncFixture) record(t *testing.T) string {
	t.Helper()
	return run(t, ".", "git", "--git-dir="+f.bare, "show",
		"refs/heads/forkmesh/pulls:.forkmesh/pulls/"+strconv.Itoa(f.number)+"/pull.md")
}

func (f *pullSyncFixture) events(t *testing.T) []string {
	t.Helper()
	var names []string
	for _, name := range strings.Fields(run(t, ".", "git", "--git-dir="+f.bare, "ls-tree", "--name-only",
		"refs/heads/forkmesh/pulls:.forkmesh/pulls/"+strconv.Itoa(f.number))) {
		if strings.HasSuffix(name, "-synchronize.md") {
			names = append(names, name)
		}
	}
	return names
}

func TestPullSynchronizeMovesAPatchPullToItsPushedBranch(t *testing.T) {
	f := newPullSyncFixture(t, "pr/carol/work")
	replay := run(t, ".", "git", "--git-dir="+f.bare, "rev-parse", pullHeadRef(f.number))
	pushed := f.push(t, "second.txt", "second\n")
	f.synchronize(t, replay, pushed)

	record := f.record(t)
	for _, want := range []string{"\nheadOid: " + pushed + "\n", "\nsyncedHeadOid: " + pushed + "\n",
		"\nheadBaseOid: " + f.mainOid + "\n", "\nsig: carol-signature\n"} {
		if !strings.Contains(record, want) {
			t.Fatalf("record lacks %q:\n%s", strings.TrimSpace(want), record)
		}
	}
	if got := run(t, ".", "git", "--git-dir="+f.bare, "rev-parse", pullHeadRef(f.number)); got != pushed {
		t.Fatalf("refs/pr head %s, want %s", got, pushed)
	}
	isolated := run(t, ".", "git", "--git-dir="+f.bare, "show",
		"refs/heads/forkmesh/pull/"+strconv.Itoa(f.number)+":.forkmesh/pulls/"+strconv.Itoa(f.number)+"/pull.md")
	if !strings.Contains(isolated, "\nsyncedHeadOid: "+pushed+"\n") {
		t.Fatalf("isolated record:\n%s", isolated)
	}
	events := f.events(t)
	if len(events) != 1 {
		t.Fatalf("events %v", events)
	}
	event := run(t, ".", "git", "--git-dir="+f.bare, "show",
		"refs/heads/forkmesh/pulls:.forkmesh/pulls/"+strconv.Itoa(f.number)+"/"+events[0])
	for _, want := range []string{"\nafter: " + pushed + "\n", "\nbefore: " + replay + "\n", "\ncommits: 1\n"} {
		if !strings.Contains(event, want) {
			t.Fatalf("event lacks %q:\n%s", strings.TrimSpace(want), event)
		}
	}

	f.synchronize(t, replay, pushed)
	if again := f.events(t); len(again) != 1 {
		t.Fatalf("a repeated synchronize was written again: %v", again)
	}
	if published, err := ensurePullHeadRefs(context.Background(), f.bare); err != nil || published != 0 {
		t.Fatalf("the head sweep moved the synced head (%d, %v)", published, err)
	}

	forced := f.push(t, "third.txt", "third\n")
	run(t, f.source, "git", "reset", "-q", "--hard", "HEAD~2")
	rewritten := f.push(t, "rewrite.txt", "rewrite\n")
	f.synchronize(t, pushed, forced)
	if strings.Contains(f.record(t), forced) {
		t.Fatal("a commit the branch no longer contains became the head")
	}
	f.synchronize(t, forced, rewritten)
	if !strings.Contains(f.record(t), "\nheadOid: "+rewritten+"\n") {
		t.Fatalf("the force-pushed tip is not the head:\n%s", f.record(t))
	}
}

func TestPullSynchronizeIgnoresUnknownCommitsAndForkHeads(t *testing.T) {
	f := newPullSyncFixture(t, "pr/carol/work")
	before := f.record(t)
	run(t, f.source, "git", "checkout", "-q", "-b", "other", "main")
	writeFile(t, filepath.Join(f.source, "other.txt"), "other\n")
	run(t, f.source, "git", "add", "other.txt")
	run(t, f.source, "git", "commit", "-q", "-m", "other")
	run(t, f.source, "git", "push", "-q", f.bare, "other:refs/heads/other")
	stranger := run(t, f.source, "git", "rev-parse", "other")
	f.synchronize(t, f.mainOid, stranger)
	if f.record(t) != before || len(f.events(t)) != 0 {
		t.Fatal("a commit outside the head branch moved the pull")
	}

	fork := newPullSyncFixture(t, "carol:pr/carol/work")
	head := run(t, ".", "git", "--git-dir="+fork.bare, "rev-parse", pullHeadRef(fork.number))
	pushed := fork.push(t, "second.txt", "second\n")
	fork.synchronize(t, head, pushed)
	if strings.Contains(fork.record(t), "syncedHeadOid") || len(fork.events(t)) != 0 {
		t.Fatal("a patch-backed pull from another copy followed a local branch")
	}
}

func TestMergeExecutorMergesTheSyncedHead(t *testing.T) {
	f := newMergeFixture(t)
	f.publishPatchPull(t, "", "", "headBaseOid: "+f.mainOid, "headOid: "+f.featureOid, "syncedHeadOid: "+f.featureOid)
	result := f.request(t, "execute", "merge", map[string]any{"expectedHeadOid": f.pullsOid})
	if result["status"] != "merged" {
		t.Fatalf("synced pull did not merge: %+v", result)
	}
	baseAfter := result["baseAfter"].(string)
	if !gitIsAncestor(context.Background(), f.bare, f.featureOid, baseAfter) || f.show(t, baseAfter, "feature.txt") != "feature" {
		t.Fatalf("the merge %s does not contain the synced head %s", baseAfter, f.featureOid)
	}
	record := f.show(t, result["pullsAfter"].(string), ".forkmesh/pulls/7/pull.md")
	if !strings.Contains(record, "\nmergeHead: "+f.featureOid+"\n") {
		t.Fatalf("merged record:\n%s", record)
	}
}

func TestMergeExecutorRefusesASyncedHeadItCannotResolve(t *testing.T) {
	f := newMergeFixture(t)
	missing := strings.Repeat("ab", 20)
	f.publishPatchPull(t, "", "", "headOid: "+missing, "syncedHeadOid: "+missing)
	result := f.request(t, "execute", "merge", map[string]any{"expectedHeadOid": f.pullsOid})
	if result["status"] == "merged" || result["error"] != "stale_head" {
		t.Fatalf("an unresolvable synced head merged: %+v", result)
	}
}
