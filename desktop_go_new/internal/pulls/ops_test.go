package pulls

import (
	"encoding/json"
	"os"
	"path/filepath"
	"reflect"
	"sort"
	"strconv"
	"strings"
	"testing"
	"time"

	"forkmesh.com/desktop-core/internal/identity"
)

func (f *fixture) signing(t *testing.T) *Signer {
	t.Helper()
	signer := &Signer{Identity: testIdentity(t), AuthorName: "Alice", RepoKey: "alice/demo",
		Now: func() time.Time { return time.UnixMilli(1790000000000) }}
	f.store.Signer = signer
	return signer
}

func readText(t *testing.T, path string) string {
	t.Helper()
	raw, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	return string(raw)
}

func lastCommitMessage(t *testing.T, dir string) string {
	t.Helper()
	return sh(t, dir, "log", "-1", "--format=%s")
}

func TestEventsAreSignedWrittenAndCommittedLikeTheDesktop(t *testing.T) {
	f := newFixture(t)
	f.signing(t)
	f.addPull(t, &Pull{Number: 1, Title: "One", Base: "main", Head: "feature", Status: "open", Author: "other", Sig: "s", Patch: "p"})
	meta := f.store.MetaWorkTree()

	ev, err := f.store.AddEvent(1, EventInput{Type: "comment", Body: "\n\nhello\n"})
	if err != nil {
		t.Fatal(err)
	}
	if ev.Body != "hello" || ev.AuthorName != "Alice" || ev.Ts != 1790000000000 || len(ev.ID) != 36 {
		t.Fatalf("event %+v", ev)
	}
	if !identity.Verify(ev.Author, ev.Sig, EventCanonical(1, &ev, "alice/demo")) {
		t.Fatal("comment signature does not verify")
	}
	want := "---\ntype: comment\nid: " + ev.ID + "\nauthor: " + ev.Author + "\nauthorName: Alice\nts: 1790000000000\nsig: " + ev.Sig + "\n---\n\nhello\n"
	if got := readText(t, filepath.Join(f.store.pullDir(1), "0002-comment.md")); got != want {
		t.Fatalf("event file\n%q\nwant\n%q", got, want)
	}
	if msg := lastCommitMessage(t, meta); msg != "pull #1: comment" {
		t.Fatalf("commit message %q", msg)
	}

	thread, err := f.store.AddEvent(1, EventInput{Type: "thread-comment", Path: "a.txt", Side: "new", LineStart: 2,
		Body: "b", SuggestionPatch: "\n+x\n", Patchset: "abc", AnchorLines: "two"})
	if err != nil {
		t.Fatal(err)
	}
	if thread.ThreadID != thread.ID || thread.LineEnd != 2 || thread.SuggestionPatch != "+x" {
		t.Fatalf("thread %+v", thread)
	}
	text := readText(t, filepath.Join(f.store.pullDir(1), "0003-thread-comment.md"))
	for _, line := range []string{"threadId: " + thread.ID, "path: a.txt", "side: new", "lineStart: 2", "lineEnd: 2",
		"suggestionPatchB64: " + encodeFrontMatterBlob("+x"), "patchset: abc", "anchorLinesB64: " + encodeFrontMatterBlob("two")} {
		if !strings.Contains(text, "\n"+line+"\n") {
			t.Fatalf("thread file lacks %q:\n%s", line, text)
		}
	}
	if msg := lastCommitMessage(t, meta); msg != "pull #1: thread on a.txt:2" {
		t.Fatalf("commit message %q", msg)
	}
	for _, step := range []struct {
		in  EventInput
		msg string
	}{
		{EventInput{Type: "review", State: "approved", Body: "ok"}, "pull #1: review (approved)"},
		{EventInput{Type: "line-comment", Path: "b.txt", Side: "old", Line: 1, Body: "x"}, "pull #1: comment on b.txt:1"},
		{EventInput{Type: "thread-reply", ThreadID: thread.ID, ParentID: thread.ID, Body: "r"}, "pull #1: thread reply"},
		{EventInput{Type: "thread-state", ThreadID: thread.ID, State: "resolved"}, "pull #1: thread resolved"},
		{EventInput{Type: "suggestion-state", ThreadID: thread.ID, State: "applied", AppliedCommit: "c0"}, "pull #1: suggestion applied"},
	} {
		if _, err := f.store.AddEvent(1, step.in); err != nil {
			t.Fatal(err)
		}
		if msg := lastCommitMessage(t, meta); msg != step.msg {
			t.Fatalf("commit message %q want %q", msg, step.msg)
		}
	}
	state := readText(t, filepath.Join(f.store.pullDir(1), "0007-thread-state.md"))
	if !strings.Contains(state, "\nstate: resolved\nsig: ") {
		t.Fatalf("thread-state writes its state after the blobs:\n%s", state)
	}
	if _, err := f.store.AddEvent(9, EventInput{Type: "comment", Body: "x"}); err == nil || err.Error() != "Pull request #9 not found." {
		t.Fatalf("missing pull: %v", err)
	}
	remote := Event{Type: "review", ID: "r1", Author: "BOBKEY123456", State: "changes_requested", Ts: 3, Sig: "sig", Body: "no"}
	if err := f.store.ApplyRemoteEvent(1, remote); err != nil {
		t.Fatal(err)
	}
	if msg := lastCommitMessage(t, meta); msg != "pull #1: review (from BOBKEY12)" {
		t.Fatalf("remote message %q", msg)
	}
	pr, _ := f.store.ReadPull(1, false)
	if len(pr.Events) != 8 || pr.Events[7].Author != "BOBKEY123456" {
		t.Fatalf("events %d", len(pr.Events))
	}
	if ref := sh(t, f.repo, "rev-parse", "--verify", "refs/heads/forkmesh/pull/1"); ref == "" {
		t.Fatal("metadata branch missing")
	}
	readOnly := &Store{WorkTree: f.repo, DataRoot: f.data}
	if _, err := readOnly.AddEvent(1, EventInput{Type: "comment"}); err == nil || err.Error() != readOnlyMessage {
		t.Fatalf("read-only: %v", err)
	}
}

func TestBranchReviewThreads(t *testing.T) {
	f := newFixture(t)
	f.signing(t)
	ev, err := f.store.AddBranchEvent("feature/x", BranchEventInput{Type: "thread-comment", Path: "a.txt", Side: "new", LineStart: 3, Body: "why"})
	if err != nil {
		t.Fatal(err)
	}
	if !identity.Verify(ev.Author, ev.Sig, BranchEventCanonical("feature/x", &ev, "alice/demo")) {
		t.Fatal("branch signature")
	}
	meta := f.store.MetaWorkTree()
	dir := filepath.Join(meta, ".forkmesh/branch-reviews", BranchReviewKey("feature/x"))
	if readText(t, filepath.Join(dir, "branch.md")) != "---\nbranch: feature/x\n---\n" {
		t.Fatal("branch marker")
	}
	if msg := lastCommitMessage(t, meta); msg != "branch feature/x: thread on a.txt:3" {
		t.Fatalf("message %q", msg)
	}
	if _, err := f.store.AddBranchEvent("feature/x", BranchEventInput{Type: "thread-state", ThreadID: ev.ThreadID, State: "resolved"}); err != nil {
		t.Fatal(err)
	}
	if msg := lastCommitMessage(t, meta); msg != "branch feature/x: thread resolved" {
		t.Fatalf("message %q", msg)
	}
	remote := Event{Type: "thread-reply", ID: "x1", ThreadID: ev.ThreadID, Author: "BOB", AuthorName: "Bob", Ts: 2, Sig: "s", Body: "because"}
	if err := f.store.ApplyRemoteBranchEvent("feature/x", remote); err != nil {
		t.Fatal(err)
	}
	if msg := lastCommitMessage(t, meta); msg != "branch feature/x: thread-reply (from Bob)" {
		t.Fatalf("message %q", msg)
	}
	if err := f.store.ApplyRemoteBranchEvent("feature/x", remote); err != nil {
		t.Fatal(err)
	}
	events := f.store.ReadBranchReviewEvents("feature/x")
	if len(events) != 3 || events[2].Body != "because" {
		t.Fatalf("events %+v", events)
	}
	if err := f.store.ApplyRemoteBranchEvent("bad..name", remote); err == nil || err.Error() != "Not a branch review event." {
		t.Fatalf("bad branch: %v", err)
	}
	for name, valid := range map[string]bool{"feature/x": true, "-x": false, "a b": false, "x.lock": false, "a//b": false, "a@{1}": false, "a\x7f": false} {
		if ValidReviewBranchName(name) != valid {
			t.Fatalf("ValidReviewBranchName(%q)", name)
		}
	}
	mirror := &Store{Mirror: meta, DataRoot: f.data}
	if got := mirror.ReadBranchReviewEvents("feature/x"); len(got) != 3 {
		t.Fatalf("mirror read %d", len(got))
	}
}

func TestCreatePullBranchBackedAndNumbering(t *testing.T) {
	f := newFixture(t)
	f.signing(t)
	f.branch(t, "feature", "a.txt", "one\nTWO\nthree\n")
	number, err := f.store.CreatePull(CreateRequest{Title: "Feature", Description: "Body", Base: "main", Head: "feature", BranchBacked: true})
	if err != nil || number != 1 {
		t.Fatalf("create %d %v", number, err)
	}
	pr, ok := f.store.ReadPull(1, true)
	if !ok || !pr.BranchBacked || !validCommitOid(pr.CreationBaseOid) || !validCommitOid(pr.CreationHeadOid) || pr.Additions != 1 {
		t.Fatalf("pull %+v", pr)
	}
	if exists(filepath.Join(f.store.pullDir(1), "changes.patch")) {
		t.Fatal("a branch-backed pull commits no patch")
	}
	signed := *pr
	if !identity.Verify(pr.Author, pr.Sig, PullCanonical(&signed, "alice/demo")) {
		t.Fatal("pull signature does not cover the derived patch")
	}
	if msg := lastCommitMessage(t, f.store.MetaWorkTree()); msg != "pull #1: open" {
		t.Fatalf("message %q", msg)
	}
	if again, err := f.store.CreatePull(CreateRequest{Title: "Again", Base: "main", Head: "feature", BranchBacked: true}); err != nil || again != 1 {
		t.Fatalf("an open pull for the same head is returned: %d %v", again, err)
	}
	sh(t, f.repo, "checkout", "-q", "main")
	writeFile(t, filepath.Join(f.repo, ".forkmesh/pulls/5/pull.md"), "---\nnumber: 5\ntitle: Archived\nbase: main\nhead: old\nts: 1\nauthor: A\nsig: S\n---\n\nold\n")
	sh(t, f.repo, "add", "-A")
	sh(t, f.repo, "commit", "-q", "-m", "archived record on the code branch")
	all := f.store.LoadAllIncludingArchived(false)
	if len(all) != 2 || !all[1].ArchivedOnCodeBranch || all[1].Number != 5 {
		t.Fatalf("archived %+v", all)
	}
	number, err = f.store.CreatePull(CreateRequest{Title: "Patch", Base: "main", Head: "", Patch: "diff --git a/b.txt b/b.txt\n"})
	if err != nil || number != 6 {
		t.Fatalf("archived records reserve their numbers: %d %v", number, err)
	}
	if readText(t, filepath.Join(f.store.pullDir(6), "changes.patch")) != "diff --git a/b.txt b/b.txt\n" {
		t.Fatal("patch pull stores its patch")
	}
	if err := f.store.RestoreArchivedPull(5); err != nil {
		t.Fatal(err)
	}
	if restored, ok := f.store.ReadPull(5, false); !ok || restored.Title != "Archived" {
		t.Fatal("restore")
	}
	if msg := lastCommitMessage(t, f.store.MetaWorkTree()); msg != "pull #5: restore archived record" {
		t.Fatalf("message %q", msg)
	}
}

func TestLegacyJsonRecordsAreListedAndRestored(t *testing.T) {
	f := newFixture(t)
	f.signing(t)
	writeFile(t, filepath.Join(f.repo, ".forkmesh/pulls/open/pull-4.json"),
		`{"number":99,"title":"Legacy","body":"from body","base":"main","head":"h","ts":12,"author":"A","sig":"S","patch":"diff --git a/a b/a\n+x\n","events":[{"type":"comment","body":"hi","ts":3}]}`)
	sh(t, f.repo, "add", "-A")
	sh(t, f.repo, "commit", "-q", "-m", "legacy")
	all := f.store.LoadAllIncludingArchived(true)
	if len(all) != 1 || all[0].Number != 4 || all[0].Description != "from body" || len(all[0].Events) != 1 || all[0].Additions != 1 {
		t.Fatalf("legacy %+v", all)
	}
	if err := f.store.RestoreArchivedPull(4); err != nil {
		t.Fatal(err)
	}
	pr, ok := f.store.ReadPull(4, true)
	if !ok || pr.Title != "Legacy" || pr.Status != "open" || pr.Patch != "diff --git a/a b/a\n+x\n" {
		t.Fatalf("restored %+v", pr)
	}
}

func TestIntakeDeduplicatesAndRenumbers(t *testing.T) {
	f := newFixture(t)
	f.signing(t)
	f.addPull(t, &Pull{Number: 1, Title: "Mine", Base: "main", Head: "x", Status: "open", Author: "A", Sig: "S1", Patch: "p"})
	incoming := PullFromJSON(map[string]any{"number": float64(1), "title": "Theirs", "base": "main", "head": "node:y",
		"ts": float64(7), "author": "REMOTEKEY99", "sig": "S2", "patch": "diff --git a/q b/q\n", "commits": ""})
	if err := f.store.ApplyRemotePull(incoming); err != nil {
		t.Fatal(err)
	}
	pr, ok := f.store.ReadPull(2, true)
	if !ok || pr.Title != "Theirs" || pr.Status != "open" {
		t.Fatalf("taken slot gets the next number: %+v", pr)
	}
	if msg := lastCommitMessage(t, f.store.MetaWorkTree()); msg != "pull #2: opened (from REMOTEKE)" {
		t.Fatalf("message %q", msg)
	}
	if err := f.store.ApplyRemotePull(incoming); err != nil {
		t.Fatal(err)
	}
	if exists(f.store.pullDir(3)) {
		t.Fatal("a replayed delivery is deduplicated by signature")
	}
}

func TestStatusDeleteAndRewriteRequests(t *testing.T) {
	f := newFixture(t)
	f.signing(t)
	var rewrites [][]string
	f.store.Rewrite = func(number int, paths []string) error {
		rewrites = append(rewrites, append([]string{"#" + strconv.Itoa(number)}, paths...))
		return nil
	}
	for n := 1; n <= 4; n++ {
		f.addPull(t, &Pull{Number: n, Title: "P", Base: "main", Head: "h", Status: "open", Author: "A", Sig: "S", Patch: "patch"})
	}
	if err := f.store.SetStatus(1, "merged"); err != nil {
		t.Fatal(err)
	}
	if !reflect.DeepEqual(rewrites, [][]string{{"#1", ".forkmesh/pulls/1/changes.patch", ".forkmesh/pulls/1/commits.mbox"}}) {
		t.Fatalf("rewrites %v", rewrites)
	}
	if exists(filepath.Join(f.store.pullDir(1), "changes.patch")) {
		t.Fatal("a decided pull drops its payload files")
	}
	if err := f.store.SetStatus(2, "closed"); err != nil {
		t.Fatal(err)
	}
	if exists(f.store.pullDir(2)) || rewrites[1][1] != ".forkmesh/pulls/2" {
		t.Fatalf("close deletes with a history rewrite: %v", rewrites)
	}
	if msg := lastCommitMessage(t, f.store.MetaWorkTree()); msg != "pull #2: deleted" {
		t.Fatalf("message %q", msg)
	}
	if err := f.store.DeletePull(2, false); err == nil || err.Error() != "Pull request #2 not found." {
		t.Fatalf("missing delete: %v", err)
	}
	deleted, missing, err := f.store.DeletePulls([]int{3, 4, 2, 3}, false)
	if err != nil || !reflect.DeepEqual(deleted, []int{3, 4}) || !reflect.DeepEqual(missing, []int{2}) {
		t.Fatalf("bulk delete %v %v %v", deleted, missing, err)
	}
	if msg := lastCommitMessage(t, f.store.MetaWorkTree()); msg != "pulls: deleted #3, #4" {
		t.Fatalf("message %q", msg)
	}
	if out := sh(t, f.repo, "for-each-ref", "refs/heads/forkmesh/pull/"); strings.Contains(out, "pull/3") || strings.Contains(out, "pull/4") {
		t.Fatalf("metadata refs left behind: %s", out)
	}
	f.store.Rewrite = nil
	if err := f.store.SetStatus(1, "open"); err != nil {
		t.Fatal(err)
	}
	f.addPull(t, &Pull{Number: 7, Title: "P", Base: "main", Head: "h", Status: "open", Author: "A", Sig: "S", Patch: "patch"})
	if err := f.store.DeletePull(7, true); err == nil || err.Error() != "no desktop to run it" {
		t.Fatalf("a rewrite without a desktop fails the delete: %v", err)
	}
}

func roundTrip(t *testing.T, value map[string]any) map[string]any {
	t.Helper()
	raw, err := json.Marshal(value)
	if err != nil {
		t.Fatal(err)
	}
	out := map[string]any{}
	if err := json.Unmarshal(raw, &out); err != nil {
		t.Fatal(err)
	}
	return out
}

func TestRecordRewardPayment(t *testing.T) {
	f := newFixture(t)
	f.addPull(t, &Pull{Number: 1, Title: "P", Base: "main", Head: "h", Status: "merged", Author: "A", Sig: "S",
		RewardEstimateLamports: 5, RewardID: "rw", RewardStatus: "ready"})
	if err := f.store.RecordRewardPayment(1, "other", "tx"); err == nil || err.Error() != "Pull request #1 does not carry reward other." {
		t.Fatalf("wrong reward: %v", err)
	}
	if err := f.store.RecordRewardPayment(1, " rw ", "tx1"); err != nil {
		t.Fatal(err)
	}
	pr, _ := f.store.ReadPull(1, false)
	if pr.RewardStatus != "paid" || pr.RewardTxSignature != "tx1" {
		t.Fatalf("paid %+v", pr)
	}
	if err := f.store.RecordRewardPayment(1, "rw", "tx1"); err != nil {
		t.Fatalf("a retry is a no-op: %v", err)
	}
	if err := f.store.RecordRewardPayment(1, "rw", "tx2"); err == nil || err.Error() != "Pull request #1 already records a different reward transaction." {
		t.Fatalf("second transfer: %v", err)
	}
}

func TestReconcileFlipsLandedPullsAndAdvancesRewards(t *testing.T) {
	f := newFixture(t)
	f.branch(t, "landed", "a.txt", "one\nLANDED\nthree\n")
	f.addPull(t, &Pull{Number: 1, Title: "Landed", Base: "main", Head: "landed", Status: "open", Author: "A", Sig: "S",
		BranchBacked: true, RewardEstimateLamports: 9, RewardCondition: "merged", RewardStatus: "pending_merge"})
	f.addPull(t, &Pull{Number: 2, Title: "Old merge", Base: "main", Head: "gone", Status: "merged", Author: "A", Sig: "S",
		RewardEstimateLamports: 9, RewardStatus: "pending_merge"})
	sh(t, f.repo, "merge", "-q", "--no-edit", "landed")
	result := f.store.ReconcileMergedPulls()
	if !reflect.DeepEqual(result.Flipped, []int{1}) || !reflect.DeepEqual(result.Rewards, []int{1, 2}) || result.Error != "" {
		t.Fatalf("reconcile %+v", result)
	}
	pr, _ := f.store.ReadPull(1, false)
	if pr.Status != "merged" || pr.RewardStatus != "pending_wallet" || pr.MergeHead == "" || pr.MergeBase == "" {
		t.Fatalf("flipped %+v", pr)
	}
}

func TestAdoptForeignPulls(t *testing.T) {
	f := newFixture(t)
	f.addPull(t, &Pull{Number: 1, Title: "Mine", Base: "main", Head: "x", Status: "open", Author: "A", Sig: "S1", Patch: "p"})
	served := filepath.Join(filepath.Dir(f.repo), "served")
	sh(t, filepath.Dir(f.repo), "clone", "-q", "--branch", "forkmesh/pulls", f.repo, served)
	writeFile(t, filepath.Join(served, ".forkmesh/pulls/2/pull.md"), "---\nnumber: 2\ntitle: Drained\nbase: main\nhead: y\nstatus: open\nts: 1\nauthor: B\nsig: S2\n---\n\nbody\n")
	writeFile(t, filepath.Join(served, ".forkmesh/pulls/2/changes.patch"), "patch\n")
	sh(t, served, "add", "-A")
	sh(t, served, "commit", "-q", "-m", "pull #2: opened (from mirror)")
	adopted, err := f.store.AdoptForeignPulls(served, true)
	if err != nil || !reflect.DeepEqual(adopted, []int{2}) {
		t.Fatalf("adopted %v %v", adopted, err)
	}
	if pr, ok := f.store.ReadPull(2, false); !ok || pr.Title != "Drained" {
		t.Fatal("adopted pull missing")
	}
	meta := f.store.MetaWorkTree()
	foreign := sh(t, served, "rev-parse", "HEAD")
	sh(t, meta, "merge-base", "--is-ancestor", foreign, "HEAD")
	again, err := f.store.AdoptForeignPulls(served, true)
	if err != nil || len(again) != 0 {
		t.Fatalf("second adopt %v %v", again, err)
	}
}

func TestEnsureLocalBranch(t *testing.T) {
	f := newFixture(t)
	f.signing(t)
	if source, _, err := f.store.EnsureLocalBranch("main"); source != BranchLocal || err != nil {
		t.Fatalf("local %s %v", source, err)
	}
	if _, _, err := f.store.EnsureLocalBranch("-x"); err == nil || err.Error() != "\"-x\" is not a valid branch name." {
		t.Fatalf("invalid %v", err)
	}
	mirror := filepath.Join(filepath.Dir(f.repo), "mirror")
	sh(t, filepath.Dir(f.repo), "clone", "-q", f.repo, mirror)
	sh(t, mirror, "checkout", "-q", "-b", "remote-only")
	writeFile(t, filepath.Join(mirror, "c.txt"), "c\n")
	sh(t, mirror, "add", "-A")
	sh(t, mirror, "commit", "-q", "-m", "remote")
	f.store.Mirror = mirror
	if source, _, err := f.store.EnsureLocalBranch("remote-only"); source != BranchMirror || err != nil {
		t.Fatalf("mirror %s %v", source, err)
	}
	_, _, err := f.store.EnsureLocalBranch("nowhere")
	if err == nil || !strings.HasPrefix(err.Error(), "nowhere is not in this checkout, on the mirror, or in any open pull request") {
		t.Fatalf("missing %v", err)
	}
	f.addPull(t, &Pull{Number: 3, Title: "Patch", Base: "main", Head: "carried", Status: "open", Author: "A", AuthorName: "Bob", Sig: "S", Ts: 1790000000000,
		Patch: "diff --git a/b.txt b/b.txt\nindex 3f0a5c1..b55f3d2 100644\n--- a/b.txt\n+++ b/b.txt\n@@ -1 +1 @@\n-bee\n+BEE\n"})
	source, number, err := f.store.EnsureLocalBranch("carried")
	if source != BranchPull || number != 3 || err != nil {
		t.Fatalf("pull %s %d %v", source, number, err)
	}
	if got := sh(t, f.repo, "show", "carried:b.txt"); got != "BEE" {
		t.Fatalf("rebuilt branch content %q", got)
	}
}

func TestConflictMergeSessionResolveAndAbort(t *testing.T) {
	f := newFixture(t)
	f.signing(t)
	f.branch(t, "feature", "a.txt", "one\nFEATURE\nthree\n")
	mbox := sh(t, f.repo, "format-patch", "--stdout", "main..feature")
	f.advanceMain(t, "a.txt", "one\nMAIN\nthree\n")
	f.addPull(t, &Pull{Number: 1, Title: "Conflicting", Base: "main", Head: "node:feature", Status: "open", Author: "A", Sig: "S",
		Patch: sh(t, f.repo, "diff", "main...feature") + "\n", Commits: mbox + "\n"})
	session := &Session{}
	f.store.Session = session
	conflicted, clean, err := f.store.StartConflictMerge(1)
	if err != nil || clean || !reflect.DeepEqual(conflicted, []string{"a.txt"}) {
		t.Fatalf("start %v %v %v", conflicted, clean, err)
	}
	if !SessionLive(f.repo) || !f.store.InProgress() || session.AmBranch != "pull/1" {
		t.Fatalf("session %+v live %v", session, SessionLive(f.repo))
	}
	other := &Store{WorkTree: f.repo, IdentityValid: true, DataRoot: f.data}
	if err := other.MergePull(1, false); err == nil || err.Error() != anotherOperationMessage {
		t.Fatalf("a live session blocks a merge: %v", err)
	}
	if err := f.store.FinishConflictMerge(1); err == nil || err.Error() != "Resolve every conflict marker in a.txt first." {
		t.Fatalf("markers %v", err)
	}
	writeFile(t, filepath.Join(f.repo, "a.txt"), "one\nBOTH\nthree\n")
	if err := f.store.FinishConflictMerge(1); err != nil {
		t.Fatal(err)
	}
	if SessionLive(f.repo) || !session.Empty() {
		t.Fatal("finish ends the session")
	}
	pr, _ := f.store.ReadPull(1, true)
	if pr.Head != "pull/1" || !strings.Contains(pr.Patch, "+BOTH") {
		t.Fatalf("refreshed pull %+v", pr)
	}
	if branch := sh(t, f.repo, "rev-parse", "--abbrev-ref", "HEAD"); branch != "main" {
		t.Fatalf("returned to %s", branch)
	}
	if msg := lastCommitMessage(t, f.store.MetaWorkTree()); msg != "pull #1: resolve conflicts on pull/1" {
		t.Fatalf("message %q", msg)
	}

	f.addPull(t, &Pull{Number: 2, Title: "Again", Base: "main", Head: "node:feature", Status: "open", Author: "A", Sig: "S",
		Patch: "x", Commits: mbox + "\n"})
	if _, _, err := f.store.StartConflictMerge(2); err != nil {
		t.Fatal(err)
	}
	f.store.AbortConflictMerge()
	if SessionLive(f.repo) || f.store.amStateDir(f.repo) != "" {
		t.Fatal("abort clears the am session")
	}
	if branch := sh(t, f.repo, "rev-parse", "--abbrev-ref", "HEAD"); branch != "main" {
		t.Fatalf("abort returned to %s", branch)
	}
	if out := sh(t, f.repo, "branch", "--list", "pull/2"); out != "" {
		t.Fatalf("abort drops the new pull branch: %q", out)
	}
}

func TestAgentConflictWorkTreeAndFileEdits(t *testing.T) {
	f := newFixture(t)
	f.signing(t)
	f.branch(t, "feature", "a.txt", "one\nFEATURE\nthree\n")
	mbox := sh(t, f.repo, "format-patch", "--stdout", "main..feature")
	f.advanceMain(t, "a.txt", "one\nMAIN\nthree\n")
	f.addPull(t, &Pull{Number: 1, Title: "Conflicting", Base: "main", Head: "feature", Status: "open", Author: "A", Sig: "S",
		Patch: "x", Commits: mbox + "\n"})
	f.store.Session = &Session{}
	conflicted, clean, err := f.store.StartConflictAgentEdit(1)
	if err != nil || clean || len(conflicted) != 1 {
		t.Fatalf("agent start %v %v %v", conflicted, clean, err)
	}
	edit := f.store.AgentEditWorkTree()
	if !strings.HasPrefix(filepath.Base(edit), "forkmesh-pr-conflict-1-") || !f.store.InProgress() {
		t.Fatalf("edit tree %q", edit)
	}
	writeFile(t, filepath.Join(edit, "a.txt"), "one\nFIXED\nthree\n")
	if err := f.store.FinishConflictMerge(1); err != nil {
		t.Fatal(err)
	}
	if exists(edit) {
		t.Fatal("the agent worktree is removed")
	}
	if got := sh(t, f.repo, "show", "feature:a.txt"); !strings.Contains(got, "FIXED") {
		t.Fatalf("the pull branch moved to the fix: %q", got)
	}

	f.addPull(t, &Pull{Number: 2, Title: "Clean", Base: "main", Head: "node:x", Status: "open", Author: "A", Sig: "S", Ts: 1790000000000,
		Patch: "diff --git a/b.txt b/b.txt\nindex 3f0a5c1..b55f3d2 100644\n--- a/b.txt\n+++ b/b.txt\n@@ -1 +1 @@\n-bee\n+BEE\n"})
	f.store.Session = &Session{}
	content, err := f.store.StartPullFileEdit(2, "b.txt")
	if err != nil || content != "BEE\n" {
		t.Fatalf("file edit %q %v", content, err)
	}
	if err := f.store.FinishPullFileEdit(2, "b.txt", "BEE!\n"); err != nil {
		t.Fatal(err)
	}
	pr, _ := f.store.ReadPull(2, true)
	if !strings.Contains(pr.Patch, "+BEE!") || pr.Head != "pull/2" {
		t.Fatalf("edited %+v", pr)
	}
	if msg := lastCommitMessage(t, f.store.MetaWorkTree()); msg != "pull #2: edit b.txt" {
		t.Fatalf("message %q", msg)
	}
}

func TestDeletePullFileTrimsPatchAndMbox(t *testing.T) {
	f := newFixture(t)
	f.signing(t)
	sh(t, f.repo, "checkout", "-q", "-b", "two")
	writeFile(t, filepath.Join(f.repo, "a.txt"), "one\nA\nthree\n")
	sh(t, f.repo, "commit", "-q", "-am", "touch a")
	writeFile(t, filepath.Join(f.repo, "a.txt"), "one\nAA\nthree\n")
	writeFile(t, filepath.Join(f.repo, "b.txt"), "B\n")
	sh(t, f.repo, "commit", "-q", "-am", "touch both")
	sh(t, f.repo, "checkout", "-q", "main")
	patch := sh(t, f.repo, "diff", "main...two") + "\n"
	mbox := sh(t, f.repo, "format-patch", "--stdout", "main..two") + "\n"
	f.addPull(t, &Pull{Number: 1, Title: "Two", Base: "main", Head: "two", Status: "open", Author: "A", Sig: "S", Patch: patch, Commits: mbox})
	if err := f.store.DeletePullFile(1, "c.txt"); err == nil || err.Error() != "'c.txt' is not a file in this pull request." {
		t.Fatalf("absent file %v", err)
	}
	if err := f.store.DeletePullFile(1, "a.txt"); err != nil {
		t.Fatal(err)
	}
	pr, _ := f.store.ReadPull(1, true)
	if strings.Contains(pr.Patch, "a.txt") || pr.FilesChanged != 1 {
		t.Fatalf("patch %q", pr.Patch)
	}
	if strings.Count(pr.Commits, "Subject: [PATCH") != 1 || strings.Contains(pr.Commits, "a/a.txt") {
		t.Fatalf("mbox keeps only the commit that still changes something:\n%s", pr.Commits)
	}
	if err := f.store.DeletePullFile(1, "b.txt"); err == nil || !strings.Contains(err.Error(), "is the only file changed") {
		t.Fatalf("last file %v", err)
	}
}

func TestReviewCheckoutReplaysAPatchSubmission(t *testing.T) {
	f := newFixture(t)
	pr := &Pull{Number: 4, Title: "Review me", Base: "main", Status: "open", Author: "A", AuthorName: "Bob", Ts: 1790000000000,
		Patch: "diff --git a/b.txt b/b.txt\nindex 3f0a5c1..b55f3d2 100644\n--- a/b.txt\n+++ b/b.txt\n@@ -1 +1 @@\n-bee\n+BEE\n"}
	dir := filepath.Join(t.TempDir(), "review", "4")
	checkout, err := f.store.CheckoutForReview(pr, dir)
	if err != nil {
		t.Fatal(err)
	}
	if checkout.CommitCount != 1 || checkout.Uncommitted || !strings.Contains(checkout.Patch, "+BEE") || checkout.HeadOid == checkout.BaseOid {
		t.Fatalf("checkout %+v", checkout)
	}
	if got := readText(t, filepath.Join(dir, "b.txt")); got != "BEE\n" {
		t.Fatalf("content %q", got)
	}
	f.store.DiscardReviewCheckout(dir)
	if exists(dir) {
		t.Fatal("discard removes the review worktree")
	}
	if list := sh(t, f.repo, "worktree", "list"); strings.Contains(list, dir) {
		t.Fatalf("worktree still registered: %s", list)
	}
}

func TestOwnerOverrideWaivesTheReviewGate(t *testing.T) {
	f := newFixture(t)
	f.branch(t, "feature", "b.txt", "BEE\n")
	f.addPull(t, &Pull{Number: 1, Title: "Mine", Base: "main", Head: "feature", Status: "open", Author: "A", Sig: "S", BranchBacked: true})
	if err := f.store.MergePullWith(1, true, false); err == nil {
		t.Fatal("the review gate blocks")
	}
	if err := f.store.MergePullWith(1, true, true); err != nil {
		t.Fatal(err)
	}
	if msg := lastCommitMessage(t, f.store.MetaWorkTree()); msg != "pull #1: merged (review gate waived by owner)" {
		t.Fatalf("message %q", msg)
	}
}

func TestLoadAllMigratesMetadataBranchesAndCaches(t *testing.T) {
	f := newFixture(t)
	f.addPull(t, &Pull{Number: 1, Title: "One", Base: "main", Head: "h", Status: "open", Author: "A", Sig: "S", Patch: "p"})
	sh(t, f.repo, "update-ref", "-d", "refs/heads/forkmesh/pull/1")
	loaded := f.store.LoadAll(true)
	if len(loaded) != 1 || loaded[0].Patch != "p" {
		t.Fatalf("load %+v", loaded)
	}
	if sh(t, f.repo, "rev-parse", "--verify", "-q", "refs/heads/forkmesh/pull/1") == "" {
		t.Fatal("a record without an isolated branch gets one")
	}
	if stamp := pullLoadStamp(f.repo, "", f.store.pullsDir()); stamp != "" {
		t.Fatal("a ledger written in the last 3 s is never cached")
	}
	past := time.Now().Add(-time.Minute)
	_ = filepath.Walk(filepath.Dir(f.repo), func(path string, info os.FileInfo, err error) error {
		if err == nil {
			_ = os.Chtimes(path, past, past)
		}
		return nil
	})
	stamp := pullLoadStamp(f.repo, "", f.store.pullsDir())
	if stamp == "" {
		t.Fatal("a settled ledger has a stamp")
	}
	first := f.store.LoadAll(true)
	first[0].Title = "mutated by a caller"
	again := f.store.LoadAll(true)
	if again[0].Title != "One" {
		t.Fatal("cached loads hand out copies")
	}
}

func TestWireJSONRoundTrip(t *testing.T) {
	pr := Pull{Number: 3, Title: "T", Description: "D", Base: "main", Head: "h", Status: "merged", Ts: 1790000000000,
		Author: "A", Sig: "S", Patch: "diff --git a/x b/x\n+y\n", BranchBacked: true, MergeBase: "mb", MergeHead: "mh",
		RewardEstimateLamports: 4000000, ArchivedOnCodeBranch: true, PatchHeadOid: "ph",
		Events: []Event{{Type: "comment", Body: "b", Ts: 2, Line: 0}}}
	ComputeStats(&pr)
	back := PullFromFullJSON(roundTrip(t, pr.FullJSON()))
	if !reflect.DeepEqual(back, pr) {
		t.Fatalf("round trip\n%+v\n%+v", back, pr)
	}
	wire := PullFromJSON(map[string]any{"number": 2.5, "status": nil, "ts": "17", "rewardEstimateLamports": 9.9, "body": "b"})
	if wire.Number != 0 || wire.Status != "open" || wire.Ts != 17 || wire.RewardEstimateLamports != 9 || wire.Description != "b" {
		t.Fatalf("Qt conversions %+v", wire)
	}
	keys := []string{}
	for key := range pr.WireJSON() {
		keys = append(keys, key)
	}
	sort.Strings(keys)
	if len(keys) != 22 {
		t.Fatalf("wire keys %v", keys)
	}
}
