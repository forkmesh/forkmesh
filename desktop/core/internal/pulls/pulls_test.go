package pulls

import (
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
	"time"
)

func gitEnv(t *testing.T) {
	t.Helper()
	home := t.TempDir()
	t.Setenv("HOME", home)
	t.Setenv("XDG_CONFIG_HOME", filepath.Join(home, ".config"))
	t.Setenv("XDG_DATA_HOME", filepath.Join(home, ".local/share"))
	t.Setenv("XDG_CACHE_HOME", filepath.Join(home, ".cache"))
	t.Setenv("GIT_CONFIG_NOSYSTEM", "1")
	t.Setenv("GIT_AUTHOR_NAME", "Tester")
	t.Setenv("GIT_AUTHOR_EMAIL", "tester@example.com")
	t.Setenv("GIT_COMMITTER_NAME", "Tester")
	t.Setenv("GIT_COMMITTER_EMAIL", "tester@example.com")
}

func sh(t *testing.T, dir string, args ...string) string {
	t.Helper()
	cmd := exec.Command("git", append([]string{"-C", dir}, args...)...)
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("git %v: %v\n%s", args, err, out)
	}
	return strings.TrimSpace(string(out))
}

func writeFile(t *testing.T, path, text string) {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, []byte(text), 0o644); err != nil {
		t.Fatal(err)
	}
}

type fixture struct {
	repo  string
	data  string
	store *Store
}

func newFixture(t *testing.T) *fixture {
	t.Helper()
	gitEnv(t)
	root := t.TempDir()
	repo := filepath.Join(root, "repo")
	if err := os.MkdirAll(repo, 0o755); err != nil {
		t.Fatal(err)
	}
	sh(t, repo, "init", "-q", "-b", "main")
	writeFile(t, filepath.Join(repo, "a.txt"), "one\ntwo\nthree\n")
	writeFile(t, filepath.Join(repo, "b.txt"), "bee\n")
	sh(t, repo, "add", "-A")
	sh(t, repo, "commit", "-q", "-m", "base")
	data := filepath.Join(root, "data")
	store := &Store{WorkTree: repo, IdentityValid: true, DataRoot: data}
	return &fixture{repo: repo, data: data, store: store}
}

func (f *fixture) branch(t *testing.T, name, file, text string) {
	t.Helper()
	sh(t, f.repo, "checkout", "-q", "-b", name, "main")
	writeFile(t, filepath.Join(f.repo, file), text)
	sh(t, f.repo, "commit", "-q", "-am", "change on "+name)
	sh(t, f.repo, "checkout", "-q", "main")
}

func (f *fixture) advanceMain(t *testing.T, file, text string) {
	t.Helper()
	writeFile(t, filepath.Join(f.repo, file), text)
	sh(t, f.repo, "add", "-A")
	sh(t, f.repo, "commit", "-q", "-m", "main moves")
}

func (f *fixture) addPull(t *testing.T, pr *Pull) {
	t.Helper()
	if err := f.store.writePull(pr); err != nil {
		t.Fatal(err)
	}
	if err := f.store.commit("pull #" + strconv.Itoa(pr.Number) + ": open"); err != nil {
		t.Fatal(err)
	}
}

func (f *fixture) addReview(t *testing.T, number int, index, author, state string) {
	t.Helper()
	dir := f.store.pullDir(number)
	text := "---\ntype: review\nid: r" + index + "\nauthor: " + author + "\nauthorName: " + author + "\nts: 5\nstate: " + state + "\nsig: s\n---\n\nlooks fine\n"
	writeFile(t, filepath.Join(dir, index+"-review.md"), text)
	if err := f.store.commit("pull #" + strconv.Itoa(number) + ": review"); err != nil {
		t.Fatal(err)
	}
}

func TestWritePullBytesMatchTheDesktop(t *testing.T) {
	f := newFixture(t)
	pr := &Pull{Number: 7, Title: "Add: colon", Base: "main", Head: "feature", Status: "open", Ts: 1700000000000,
		Author: "AUTH", AuthorName: "Alice", Sig: "SIG", BranchBacked: true, CreationBaseOid: "c0", Client: "mcp",
		RewardEstimateLamports: 4000000, RewardCondition: "merged", RewardStatus: "pending_merge", Description: "Body line\n\nmore"}
	if err := f.store.writePull(pr); err != nil {
		t.Fatal(err)
	}
	got, err := os.ReadFile(filepath.Join(f.store.pullDir(7), "pull.md"))
	if err != nil {
		t.Fatal(err)
	}
	want := "---\nschema: forkmesh-pull-v1\nnumber: 7\ntitle: Add: colon\nbase: main\nhead: feature\nstatus: open\nderive: branch\n" +
		"creationBaseOid: c0\nts: 1700000000000\nauthor: AUTH\nauthorName: Alice\nsig: SIG\nclient: mcp\n" +
		"rewardEstimateLamports: 4000000\nrewardCondition: merged\nrewardStatus: pending_merge\n---\n\nBody line\n\nmore\n"
	if string(got) != want {
		t.Fatalf("pull.md:\n%q\nwant\n%q", got, want)
	}
	back, ok := f.store.ReadPull(7, false)
	if !ok || back.Title != "Add: colon" || back.Description != "Body line\n\nmore" || !back.BranchBacked ||
		back.RewardEstimateLamports != 4000000 || back.Status != "open" {
		t.Fatalf("read back %+v", back)
	}
}

func TestFrontMatterRules(t *testing.T) {
	fm := parseFrontMatter([]byte("---\nkey: a: b\nempty:\nnosep\n---\n\n\nbody\n\n"))
	if !fm.valid || fm.get("key") != "a: b" || !fm.has("empty") || fm.has("nosep") || fm.body != "body" {
		t.Fatalf("%+v", fm)
	}
	open := parseFrontMatter([]byte("---\ntitle: x\n"))
	if open.valid || open.body != "" || open.get("title") != "x" {
		t.Fatalf("unterminated %+v", open)
	}
	plain := parseFrontMatter([]byte("no front matter"))
	if plain.valid || plain.body != "no front matter" {
		t.Fatalf("plain %+v", plain)
	}
}

func TestReviewGate(t *testing.T) {
	pr := &Pull{Author: "me"}
	if pr.IndependentReviewGateSatisfied() {
		t.Fatal("no reviews")
	}
	pr.Events = []Event{{Type: "review", Author: "me", State: "approved"}}
	if pr.IndependentReviewGateSatisfied() {
		t.Fatal("self approval counts")
	}
	pr.Events = append(pr.Events, Event{Type: "review", Author: "peer", State: "approved"})
	if !pr.IndependentReviewGateSatisfied() {
		t.Fatal("peer approval ignored")
	}
	pr.Events = append(pr.Events, Event{Type: "review", Author: "other", AuthorName: "Olga", State: "changes_requested"})
	if pr.IndependentReviewGateSatisfied() || !pr.HasIndependentChangesRequested() || pr.IndependentChangesRequestedBy()[0] != "Olga" {
		t.Fatal("changes requested ignored")
	}
	pr.Events = append(pr.Events, Event{Type: "review", Author: "other", State: "commented"})
	if !pr.IndependentReviewGateSatisfied() {
		t.Fatal("comment review should clear the standing request")
	}
}

func TestRewardAdvance(t *testing.T) {
	pr := &Pull{RewardStatus: "pending_merge"}
	if !AdvanceRewardForMerge(pr) || pr.RewardStatus != "pending_wallet" {
		t.Fatal(pr.RewardStatus)
	}
	other := &Pull{RewardStatus: "pending_merge", RewardCondition: "released"}
	if AdvanceRewardForMerge(other) {
		t.Fatal("non-merge condition advanced")
	}
}

func TestMetaWorkTreeIsTheDesktopsLedgerCheckout(t *testing.T) {
	f := newFixture(t)
	meta := f.store.MetaWorkTree()
	if meta != MetaWorkTreePath(f.data, f.repo) || headBranchFromFile(meta) != "forkmesh/pulls" {
		t.Fatalf("meta %q", meta)
	}
	again := &Store{WorkTree: f.repo, IdentityValid: true, DataRoot: f.data}
	if again.MetaWorkTree() != meta {
		t.Fatal("second store did not reuse the checkout")
	}
}

func TestLoadAllPrefersTheIsolatedMetadataBranch(t *testing.T) {
	f := newFixture(t)
	f.branch(t, "feature", "a.txt", "one\nTWO\nthree\n")
	f.addPull(t, &Pull{Number: 1, Title: "First", Base: "main", Head: "feature", Status: "open", Author: "x", Sig: "s", BranchBacked: true})
	f.addPull(t, &Pull{Number: 2, Title: "Second", Base: "main", Head: "feature", Status: "open", Author: "x", Sig: "s", BranchBacked: true})
	if sh(t, f.repo, "rev-parse", "--verify", "-q", "refs/heads/forkmesh/pull/1") == "" {
		t.Fatal("metadata branch not materialized")
	}
	meta := f.store.MetaWorkTree()
	pullMd := filepath.Join(meta, ".forkmesh/pulls/2/pull.md")
	raw, _ := os.ReadFile(pullMd)
	writeFile(t, pullMd, strings.Replace(string(raw), "title: Second", "title: Second edited", 1))
	sh(t, meta, "commit", "-q", "-am", "edit outside PullStore")
	loaded := f.store.LoadAll(false)
	if len(loaded) != 2 || loaded[0].Title != "First" || loaded[1].Title != "Second" {
		t.Fatalf("isolated branch should win: %+v %+v", loaded[0], loaded[1])
	}
	plain := (&Store{WorkTree: f.repo, IdentityValid: false, DataRoot: f.data}).LoadAll(false)
	if len(plain) != 2 {
		t.Fatalf("read-only load: %d", len(plain))
	}
}

func TestBehindBaseUpdateThenMerge(t *testing.T) {
	f := newFixture(t)
	f.branch(t, "feature", "a.txt", "one\nTWO\nthree\n")
	f.advanceMain(t, "b.txt", "bee moved\n")
	f.addPull(t, &Pull{Number: 3, Title: "Two", Base: "main", Head: "feature", Status: "open", Author: "x", Sig: "s",
		BranchBacked: true, RewardStatus: "pending_merge", RewardEstimateLamports: 5})
	behind, count, err := f.store.IsBranchBehindBase(3)
	if err != nil || !behind || count != 1 {
		t.Fatalf("behind=%v count=%d err=%v", behind, count, err)
	}
	if err := f.store.UpdateBranchFromBase(3); err != nil {
		t.Fatal(err)
	}
	if behind, _, _ := f.store.IsBranchBehindBase(3); behind {
		t.Fatal("still behind after update")
	}
	if got := sh(t, f.repo, "rev-parse", "--abbrev-ref", "HEAD"); got != "main" {
		t.Fatalf("checkout not restored: %s", got)
	}
	sh(t, f.repo, "merge-base", "--is-ancestor", "main", "feature")
	if got := sh(t, f.repo, "rev-parse", "refs/pr/3/head"); got != sh(t, f.repo, "rev-parse", "feature") {
		t.Fatalf("refs/pr/3/head %s not the updated head", got)
	}
	clean, conflicts, err := f.store.CheckMergeable(3)
	if err != nil || !clean || len(conflicts) != 0 {
		t.Fatalf("clean=%v conflicts=%v err=%v", clean, conflicts, err)
	}
	purged := 0
	f.store.Purge = func(number int) error { purged = number; return nil }
	if err := f.store.MergePull(3, false); err != nil {
		t.Fatal(err)
	}
	if purged != 3 {
		t.Fatal("payload purge not requested")
	}
	if sh(t, f.repo, "rev-parse", "main") != sh(t, f.repo, "rev-parse", "feature") {
		t.Fatal("main did not fast-forward to the updated head")
	}
	pr, _ := f.store.ReadPull(3, false)
	if pr.Status != "merged" || pr.RewardStatus != "pending_wallet" || pr.MergeHead == "" || pr.MergeBase == "" {
		t.Fatalf("ledger %+v", pr)
	}
	if err := f.store.MergePull(3, false); err == nil || err.Error() != "This pull request is already merged." {
		t.Fatalf("second merge: %v", err)
	}
}

func TestConflictsAreReported(t *testing.T) {
	f := newFixture(t)
	f.branch(t, "feature", "a.txt", "one\nFEATURE\nthree\n")
	f.advanceMain(t, "a.txt", "one\nMAIN\nthree\n")
	f.addPull(t, &Pull{Number: 4, Title: "Clash", Base: "main", Head: "feature", Status: "open", Author: "x", Sig: "s", BranchBacked: true})
	clean, conflicts, err := f.store.CheckMergeable(4)
	if err != nil || clean || len(conflicts) != 1 || conflicts[0] != "a.txt" {
		t.Fatalf("clean=%v conflicts=%v err=%v", clean, conflicts, err)
	}
	if err := f.store.UpdateBranchFromBase(4); err == nil || !strings.HasPrefix(err.Error(), "Could not merge main into feature: ") {
		t.Fatalf("update over a conflict: %v", err)
	}
	if got := sh(t, f.repo, "rev-parse", "--abbrev-ref", "HEAD"); got != "main" {
		t.Fatalf("checkout not restored: %s", got)
	}
	if sh(t, f.repo, "status", "--porcelain") != "" {
		t.Fatal("a failed update left the checkout dirty")
	}
}

func TestPeerReviewGateRefusesTheMerge(t *testing.T) {
	f := newFixture(t)
	f.branch(t, "feature", "a.txt", "one\nTWO\nthree\n")
	f.addPull(t, &Pull{Number: 5, Title: "Gated", Base: "main", Head: "feature", Status: "open", Author: "author", Sig: "s", BranchBacked: true})
	err := f.store.MergePull(5, true)
	if err == nil || !strings.HasPrefix(err.Error(), "At least one approval from a peer") {
		t.Fatalf("gate: %v", err)
	}
	f.addReview(t, 5, "0002", "peer", "approved")
	if err := f.store.MergePull(5, true); err != nil {
		t.Fatal(err)
	}
}

func TestPatchBackedPullMergesWithApply(t *testing.T) {
	f := newFixture(t)
	patch := "diff --git a/b.txt b/b.txt\nindex 1a9a4bc..0000000 100644\n--- a/b.txt\n+++ b/b.txt\n@@ -1 +1 @@\n-bee\n+buzz\n"
	f.addPull(t, &Pull{Number: 6, Title: "Patch", Base: "main", Head: "contrib", Status: "open", Author: "x", Sig: "s", Patch: patch})
	clean, _, err := f.store.CheckMergeable(6)
	if err != nil || !clean {
		t.Fatalf("clean=%v err=%v", clean, err)
	}
	if err := f.store.MergePull(6, false); err != nil {
		t.Fatal(err)
	}
	raw, _ := os.ReadFile(filepath.Join(f.repo, "b.txt"))
	if string(raw) != "buzz\n" {
		t.Fatalf("b.txt %q", raw)
	}
	if got := sh(t, f.repo, "log", "-1", "--format=%s"); got != "merge pull #6: Patch" {
		t.Fatalf("commit %q", got)
	}
	if exists(filepath.Join(f.store.pullDir(6), "changes.patch")) {
		t.Fatal("merged pull kept its patch file")
	}
}

func TestConflictMergeInProgressNeedsTheDesktopSession(t *testing.T) {
	f := newFixture(t)
	if f.store.ConflictMergeInProgress() {
		t.Fatal("no am state")
	}
	gitDir := filepath.Join(f.repo, ".git", "rebase-apply")
	if err := os.MkdirAll(gitDir, 0o755); err != nil {
		t.Fatal(err)
	}
	if f.store.ConflictMergeInProgress() {
		t.Fatal("without a live desktop session the state is stale")
	}
	f.store.AmLive = func() bool { return true }
	if !f.store.ConflictMergeInProgress() {
		t.Fatal("live session ignored")
	}
}

func TestSyntheticMboxDate(t *testing.T) {
	t.Setenv("TZ", "UTC")
	old := time.Local
	time.Local = time.UTC
	defer func() { time.Local = old }()
	got := SyntheticMbox(&Pull{Title: "T", Ts: 1759490000123, Patch: "p"}, time.Now())
	if !strings.Contains(got, "\nDate: 03 Oct 2025 11:13:20 +0000\n") || !strings.HasSuffix(got, "---\np\n") ||
		!strings.Contains(got, "From: ForkMesh contributor <contributor@forkmesh>\n") {
		t.Fatalf("%q", got)
	}
}

func TestParseApplyConflicts(t *testing.T) {
	got := ParseApplyConflicts("error: patch failed: a.txt:3\nerror: a.txt: patch does not apply\nApplied patch to 'b.txt' with conflicts.\n")
	if len(got) != 2 || got[0] != "a.txt" || got[1] != "b.txt" {
		t.Fatal(got)
	}
}
