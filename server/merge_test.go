package mirrornode

import (
	"context"
	"crypto/ed25519"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strings"
	"sync/atomic"
	"testing"
	"time"
)

// mergeFixture is one bare mirror with a main branch, a branch-backed pull
// (feature) whose ledger record carries creation OIDs and one peer approval,
// plus the gateway configuration the executor pins.
type mergeFixture struct {
	source, bare, gatewayPath string
	identity                  *Identity
	executor                  *PullMutationExecutor
	mainOid, featureOid       string
	pullsOid                  string
}

func writeFile(t *testing.T, path, content string) {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(path), 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, []byte(content), 0600); err != nil {
		t.Fatal(err)
	}
}

// extra carries owner-applied header fields (a reward estimate, say) that live
// outside the author's signature, exactly as the desktop writes them.
func pullRecord(number int, title, base, head, author string, branchBacked bool, baseOid, headOid string, extra ...string) string {
	lines := []string{
		"---", "schema: forkmesh-pull-v1", fmt.Sprintf("number: %d", number),
		"title: " + title, "base: " + base, "head: " + head, "status: open",
		"ts: 1700000000000", "author: " + author, "authorName: " + author,
		"sig: " + author + "-signature",
	}
	if branchBacked {
		lines = append(lines, "derive: branch", "creationBaseOid: "+baseOid, "creationHeadOid: "+headOid)
	}
	lines = append(lines, extra...)
	lines = append(lines, "---", "", "Reviewed change.", "")
	return strings.Join(lines, "\n")
}

func reviewRecord(author, state string) string {
	return strings.Join([]string{
		"---", "type: review", "id: review-" + author, "author: " + author,
		"authorName: " + author, "ts: 1700000001000", "state: " + state,
		"sig: " + author + "-review-signature", "---", "", "Looks good.", "",
	}, "\n")
}

func newMergeFixture(t *testing.T) *mergeFixture {
	t.Helper()
	return newMergeFixtureWith(t)
}

// extraPullFields are owner-applied header fields on the pull record.
func newMergeFixtureWith(t *testing.T, extraPullFields ...string) *mergeFixture {
	t.Helper()
	source, bare := makeBareRepo(t)
	identity, _ := testIdentity(t)
	// A reviewed feature branch on top of main plus the pull ledger branch.
	run(t, source, "git", "checkout", "-q", "-b", "feature")
	writeFile(t, filepath.Join(source, "feature.txt"), "feature\n")
	run(t, source, "git", "add", "feature.txt")
	run(t, source, "git", "commit", "-q", "-m", "add feature", "-m",
		"Co-authored-by: Pat <Pat@Example.TEST>\nCo-authored-by: Sam Lee <sam@example.test>")
	run(t, source, "git", "checkout", "-q", "main")
	run(t, source, "git", "push", "-q", bare, "feature:refs/heads/feature")
	mainOid := run(t, source, "git", "rev-parse", "main")
	featureOid := run(t, source, "git", "rev-parse", "feature")

	run(t, source, "git", "checkout", "-q", "--orphan", "forkmesh/pulls")
	run(t, source, "git", "rm", "-rfq", ".")
	pullDir := filepath.Join(source, ".forkmesh", "pulls", "7")
	writeFile(t, filepath.Join(pullDir, "pull.md"),
		pullRecord(7, "Add feature", "main", "feature", "alice", true, mainOid, featureOid, extraPullFields...))
	writeFile(t, filepath.Join(pullDir, "0002-review.md"), reviewRecord("bob", "approved"))
	run(t, source, "git", "add", "-A")
	run(t, source, "git", "commit", "-q", "-m", "pull #7: open")
	run(t, source, "git", "push", "-q", bare, "forkmesh/pulls:refs/heads/forkmesh/pulls")
	pullsOid := run(t, source, "git", "rev-parse", "forkmesh/pulls")
	run(t, source, "git", "checkout", "-q", "main")

	stateDir := t.TempDir()
	gatewayPath := filepath.Join(stateDir, "mirror-gateway", "config.json")
	var gateway GatewayConfig
	gateway.SchemaVersion = 1
	gateway.Node.Name = "mirror9"
	gateway.Node.PublicKey = identity.PublicKey()
	gateway.RouterPublicKey = identity.PublicKey()
	digest, err := refsSHA256(context.Background(), bare)
	if err != nil {
		t.Fatal(err)
	}
	for _, owner := range []string{"forkmesh", "mirror9"} {
		repo := Repository{Owner: owner, Name: "forkmesh", Visibility: "public", Enabled: true, GitDir: bare,
			Operations: append(append([]string(nil), defaultGatewayOperations...), writeGatewayOperations...)}
		repo.Integrity.ExpectedRefsSHA256 = digest
		gateway.Repositories = append(gateway.Repositories, repo)
	}
	if err := os.MkdirAll(filepath.Dir(gatewayPath), 0700); err != nil {
		t.Fatal(err)
	}
	if _, err := writeGatewayConfig(gatewayPath, gateway); err != nil {
		t.Fatal(err)
	}
	executor := &PullMutationExecutor{
		Config:   Config{GatewayConfig: gatewayPath, Listen: ""},
		Gateway:  gateway,
		Identity: identity,
	}
	return &mergeFixture{source: source, bare: bare, gatewayPath: gatewayPath, identity: identity,
		executor: executor, mainOid: mainOid, featureOid: featureOid, pullsOid: pullsOid}
}

func (f *mergeFixture) request(t *testing.T, action, kind string, changes map[string]any) map[string]any {
	t.Helper()
	executorType := pullMergeExecutorType
	if kind == "update" {
		executorType = pullUpdateExecutorType
	}
	value := map[string]any{
		"schemaVersion": 1, "type": executorType, "action": action,
		"owner": "forkmesh", "repository": "forkmesh", "pullNumber": 7,
		"requestId":       "merge_request_0001",
		"expectedBaseOid": f.mainOid, "expectedHeadOid": f.featureOid,
		"expectedPullsOid": f.pullsOid,
	}
	for key, item := range changes {
		value[key] = item
	}
	raw, _ := json.Marshal(value)
	result, err := f.executor.Handle(strings.NewReader(string(raw)))
	if err != nil {
		t.Fatalf("%s %s: %v", action, kind, err)
	}
	return result
}

func (f *mergeFixture) ref(t *testing.T, ref string) string {
	t.Helper()
	return run(t, ".", "git", "--git-dir="+f.bare, "rev-parse", "--verify", ref)
}

func (f *mergeFixture) show(t *testing.T, rev, path string) string {
	t.Helper()
	return run(t, ".", "git", "--git-dir="+f.bare, "show", rev+":"+path)
}

func verifyAttestation(t *testing.T, identity *Identity, result map[string]any, repo string) {
	t.Helper()
	state, _ := result["state"].(string)
	ts, _ := result["stateTs"].(string)
	sig, _ := result["stateSig"].(string)
	if !digestPattern.MatchString(state) || ts == "" || sig == "" {
		t.Fatalf("missing attestation: %+v", result)
	}
	canonical := strings.Join([]string{"forkmesh-repostate-v1", "mirror9", repo, state, ts}, "\n")
	raw, err := base64.RawURLEncoding.DecodeString(sig)
	if err != nil || !ed25519.Verify(identity.public, []byte(canonical), raw) {
		t.Fatalf("attestation does not verify: %v", err)
	}
}

func TestMergeExecutorMergesBranchBackedPullAtomicallyAndIdempotently(t *testing.T) {
	f := newMergeFixture(t)
	// main moved since the pull was opened (a non-conflicting commit), so the
	// owner merges the current base tip and the current head tip: the merge is
	// a real two-parent commit rather than a fast-forward.
	writeFile(t, filepath.Join(f.source, "README.md"), "two\n")
	run(t, f.source, "git", "add", "README.md")
	run(t, f.source, "git", "commit", "-q", "-m", "two")
	run(t, f.source, "git", "push", "-q", f.bare, "main:refs/heads/main")
	f.mainOid = run(t, f.source, "git", "rev-parse", "main")
	missing := f.request(t, "status", "merge", nil)
	if missing["status"] != "missing" || missing["generationReady"] != false {
		t.Fatalf("unexpected status before execute: %+v", missing)
	}
	result := f.request(t, "execute", "merge", nil)
	if result["status"] != "merged" || result["ok"] != true || result["generationReady"] != true {
		t.Fatalf("merge did not land: %+v", result)
	}
	baseAfter, pullsAfter := result["baseAfter"].(string), result["pullsAfter"].(string)
	if f.ref(t, "refs/heads/main") != baseAfter || f.ref(t, "refs/heads/forkmesh/pulls") != pullsAfter {
		t.Fatalf("refs did not move to the published result: %+v", result)
	}
	if gitRefValue(context.Background(), f.bare, "refs/heads/forkmesh/pull/7") != "" {
		t.Fatalf("merge kept the merged pull's metadata branch")
	}
	metadataAfter := f.ref(t, "refs/pr/7/metadata")
	if !pullRecordRetired(pullRecordAt(context.Background(), f.bare, metadataAfter, 7)) {
		t.Fatalf("merge did not publish the merged record on the PR metadata ref")
	}
	isolatedPaths := run(t, ".", "git", "--git-dir="+f.bare, "ls-tree", "-r",
		"--name-only", metadataAfter)
	for _, path := range strings.Fields(isolatedPaths) {
		if !strings.HasPrefix(path, ".forkmesh/pulls/7/") {
			t.Fatalf("isolated PR branch contains unrelated path %q", path)
		}
	}
	if result["baseBefore"] != f.mainOid || result["head"] != f.featureOid || result["pullsBefore"] != f.pullsOid {
		t.Fatalf("result does not pin the reviewed OIDs: %+v", result)
	}
	// The merge commit joins exactly the reviewed base and head.
	parents := strings.Fields(run(t, ".", "git", "--git-dir="+f.bare, "show", "-s", "--format=%P", baseAfter))
	if len(parents) != 2 || parents[0] != f.mainOid || parents[1] != f.featureOid {
		t.Fatalf("merge parents %v", parents)
	}
	if f.show(t, baseAfter, "README.md") != "two" || f.show(t, baseAfter, "feature.txt") != "feature" {
		t.Fatal("merge result lacks one side")
	}
	record := f.show(t, pullsAfter, ".forkmesh/pulls/7/pull.md")
	for _, want := range []string{"status: merged", "mergeBase: " + f.mainOid, "mergeHead: " + f.featureOid,
		"sig: alice-signature", "creationBaseOid: ", "Reviewed change."} {
		if !strings.Contains(record, want) {
			t.Fatalf("merged record lacks %q:\n%s", want, record)
		}
	}
	if strings.Contains(record, "status: open") {
		t.Fatalf("merged record still open:\n%s", record)
	}
	// Only the lifecycle fields changed: the signed header order and the body
	// bytes (including the trailing newline) are exactly what the author
	// committed, so the PullStore signature still verifies on the desktop.
	before := exec.Command("git", "--git-dir="+f.bare, "cat-file", "blob", f.pullsOid+":.forkmesh/pulls/7/pull.md")
	after := exec.Command("git", "--git-dir="+f.bare, "cat-file", "blob", pullsAfter+":.forkmesh/pulls/7/pull.md")
	beforeRaw, _ := before.Output()
	afterRaw, _ := after.Output()
	expected := strings.Replace(string(beforeRaw), "status: open\n", "status: merged\n", 1)
	mergedAt := regexp.MustCompile(`\nmergedAt: ([0-9]{13})\n`).FindStringSubmatch(string(afterRaw))
	if mergedAt == nil {
		t.Fatalf("merged record has no mergedAt:\n%s", afterRaw)
	}
	expected = strings.Replace(expected, "\n---\n\nReviewed", "\nmergeBase: "+f.mainOid+"\nmergeHead: "+f.featureOid+
		"\nmergedAt: "+mergedAt[1]+"\ncoAuthors: pat@example.test,sam@example.test\n---\n\nReviewed", 1)
	if string(afterRaw) != expected {
		t.Fatalf("merged record rewrote more than the lifecycle fields:\n%q\nwant\n%q", afterRaw, expected)
	}
	verifyAttestation(t, f.identity, result, "forkmesh")
	digest, _ := refsSHA256(context.Background(), f.bare)
	if result["state"] != digest {
		t.Fatalf("attested %v want live digest %s", result["state"], digest)
	}
	// The gateway configuration was re-pinned for both owner aliases.
	gateway, _, err := loadGatewayConfig(f.gatewayPath)
	if err != nil {
		t.Fatal(err)
	}
	for _, repo := range gateway.Repositories {
		if repo.Integrity.ExpectedRefsSHA256 != digest {
			t.Fatalf("%s/%s still pinned to %s", repo.Owner, repo.Name, repo.Integrity.ExpectedRefsSHA256)
		}
	}
	// Replaying the exact request returns the recorded result without touching
	// the repository again; a different body under the same id is refused.
	again := f.request(t, "execute", "merge", nil)
	if again["baseAfter"] != baseAfter || again["pullsAfter"] != pullsAfter || f.ref(t, "refs/heads/main") != baseAfter {
		t.Fatalf("replay changed the outcome: %+v", again)
	}
	status := f.request(t, "status", "merge", nil)
	if status["status"] != "merged" || status["generationReady"] != true {
		t.Fatalf("status after merge: %+v", status)
	}
	verifyAttestation(t, f.identity, status, "forkmesh")
	raw, _ := json.Marshal(map[string]any{
		"schemaVersion": 1, "type": pullMergeExecutorType, "action": "execute",
		"owner": "forkmesh", "repository": "forkmesh", "pullNumber": 7,
		"requestId": "merge_request_0001", "expectedBaseOid": f.mainOid,
		"expectedHeadOid": strings.Repeat("e", 40), "expectedPullsOid": f.pullsOid,
	})
	if _, err := f.executor.Handle(strings.NewReader(string(raw))); err == nil || !strings.Contains(err.Error(), "already used") {
		t.Fatalf("reused request id accepted: %v", err)
	}
	// The hidden completion markers exist and stay out of the public digest.
	if f.ref(t, "refs/forkmesh/merge-completed/"+pullMutationRequest{RequestID: "merge_request_0001"}.token()+"/base") != baseAfter {
		t.Fatal("completion marker missing")
	}
}

func TestMergeExecutorDeletesTheMergedPullBranchInTheSameTransaction(t *testing.T) {
	f := newMergeFixtureWith(t, "rewardEstimateLamports: 1000", "rewardId: rw_merge",
		"rewardCondition: merged", "rewardStatus: pending_merge")
	ctx := context.Background()
	branchTip, old, err := materializePullMetadataCommit(ctx, f.bare, 7, f.pullsOid, "mirror9", "mirror9@test")
	if err != nil || old != "" {
		t.Fatalf("materialize: %v %q", err, old)
	}
	if err := publishPullMetadataRefs(ctx, f.bare, 7, branchTip, ""); err != nil {
		t.Fatal(err)
	}
	if f.ref(t, "refs/heads/forkmesh/pull/7") != branchTip {
		t.Fatal("open pull did not get its metadata branch")
	}
	result := f.request(t, "execute", "merge", nil)
	if result["status"] != "merged" {
		t.Fatalf("merge did not land: %+v", result)
	}
	if gitRefValue(ctx, f.bare, "refs/heads/forkmesh/pull/7") != "" {
		t.Fatal("merge kept the merged pull's metadata branch")
	}
	kept := f.ref(t, "refs/pr/7/metadata")
	parents := strings.Fields(run(t, ".", "git", "--git-dir="+f.bare, "show", "-s", "--format=%P", kept))
	if len(parents) != 1 || parents[0] != branchTip {
		t.Fatalf("metadata ref %s does not extend the deleted branch tip %s: %v", kept, branchTip, parents)
	}
	record := pullRecordAt(ctx, f.bare, kept, 7)
	if !pullRecordRetired(record) || record.values["rewardId"] != "rw_merge" ||
		record.values["rewardStatus"] != "pending_wallet" {
		t.Fatalf("metadata ref record: %+v", record)
	}
	if !reservedPullNumbers(ctx, f.bare)[7] {
		t.Fatal("merged pull number is no longer reserved")
	}
}

func TestMergeExecutorAppliesPatchBackedPullFromTheLedger(t *testing.T) {
	f := newMergeFixture(t)
	// Turn the ledger record into a patch-backed (cross-node) pull: no derive,
	// a head the mirror cannot resolve, and the reviewed mbox in the ledger.
	mbox := run(t, f.source, "git", "format-patch", "--stdout", "main..feature")
	run(t, f.source, "git", "checkout", "-q", "forkmesh/pulls")
	pullDir := filepath.Join(f.source, ".forkmesh", "pulls", "7")
	writeFile(t, filepath.Join(pullDir, "pull.md"),
		pullRecord(7, "Add feature", "main", "contributor:feature", "alice", false, "", ""))
	writeFile(t, filepath.Join(pullDir, "commits.mbox"), mbox+"\n")
	writeFile(t, filepath.Join(pullDir, "changes.patch"), "unused\n")
	run(t, f.source, "git", "add", "-A")
	run(t, f.source, "git", "commit", "-q", "-m", "pull #7: patch backed")
	run(t, f.source, "git", "push", "-q", f.bare, "forkmesh/pulls:refs/heads/forkmesh/pulls")
	f.pullsOid = run(t, f.source, "git", "rev-parse", "forkmesh/pulls")
	run(t, f.source, "git", "checkout", "-q", "main")
	// The mirror does not carry the contributor's branch at all.
	run(t, ".", "git", "--git-dir="+f.bare, "update-ref", "-d", "refs/heads/feature")

	// A patch-backed request must pin the ledger commit as its head.
	wrong := f.request(t, "execute", "merge", map[string]any{"requestId": "merge_request_0002"})
	if wrong["status"] != "failed" || wrong["error"] != "unsupported_pull" {
		t.Fatalf("head pin mismatch accepted: %+v", wrong)
	}
	result := f.request(t, "execute", "merge", map[string]any{"expectedHeadOid": f.pullsOid})
	if result["status"] != "merged" {
		t.Fatalf("patch merge failed: %+v", result)
	}
	baseAfter := result["baseAfter"].(string)
	if f.ref(t, "refs/heads/main") != baseAfter {
		t.Fatal("main did not move")
	}
	// The replayed commit sits directly on the reviewed base, keeps the
	// contributor's authorship, and carries the change.
	if parent := run(t, ".", "git", "--git-dir="+f.bare, "show", "-s", "--format=%P", baseAfter); parent != f.mainOid {
		t.Fatalf("applied commit parent %s want %s", parent, f.mainOid)
	}
	if author := run(t, ".", "git", "--git-dir="+f.bare, "show", "-s", "--format=%an", baseAfter); author != "Test" {
		t.Fatalf("author %q", author)
	}
	if f.show(t, baseAfter, "feature.txt") != "feature" {
		t.Fatal("feature file missing after patch merge")
	}
	pullsAfter := result["pullsAfter"].(string)
	record := f.show(t, pullsAfter, ".forkmesh/pulls/7/pull.md")
	if !strings.Contains(record, "status: merged") || strings.Contains(record, "mergeHead:") ||
		!regexp.MustCompile(`\nmergedAt: [0-9]{13}\n`).MatchString(record) {
		t.Fatalf("patch-backed record:\n%s", record)
	}
	listing := run(t, ".", "git", "--git-dir="+f.bare, "ls-tree", "--name-only", pullsAfter, ".forkmesh/pulls/7/")
	if strings.Contains(listing, "commits.mbox") || strings.Contains(listing, "changes.patch") {
		t.Fatalf("payload survived the merge: %s", listing)
	}
	if !strings.Contains(listing, "0002-review.md") {
		t.Fatalf("review history dropped: %s", listing)
	}
	if entries, _ := os.ReadDir(filepath.Join(filepath.Dir(f.gatewayPath), pullJobsDirectoryName)); len(entries) == 0 {
		t.Fatal("no job record written")
	} else {
		for _, entry := range entries {
			if strings.HasPrefix(entry.Name(), ".apply-") {
				t.Fatal("temporary worktree left behind")
			}
		}
	}
}

func TestMergeExecutorSettlesTheContributorRewardEstimate(t *testing.T) {
	// Regression for adhoc #86: pull #32 merged with its owner-authored
	// estimate still at pending_merge, the status that holds the payment out
	// of the payout queue and keeps the public ledger printing "PENDING
	// MERGE" for work that already landed.
	f := newMergeFixtureWith(t,
		"rewardEstimateLamports: 4000000",
		"rewardRecipient: alice",
		"rewardId: reward-forkmesh-forkmesh-pull-7",
		"rewardCondition: merged",
		"rewardStatus: pending_merge",
	)
	result := f.request(t, "execute", "merge", nil)
	if result["status"] != "merged" {
		t.Fatalf("merge did not land: %+v", result)
	}
	record := f.show(t, result["pullsAfter"].(string), ".forkmesh/pulls/7/pull.md")
	for _, want := range []string{
		"status: merged", "rewardStatus: pending_wallet",
		"rewardId: reward-forkmesh-forkmesh-pull-7",
		"rewardEstimateLamports: 4000000", "rewardRecipient: alice",
		"rewardCondition: merged",
	} {
		if !strings.Contains(record, want) {
			t.Fatalf("merged record lacks %q:\n%s", want, record)
		}
	}
	if strings.Contains(record, "pending_merge") {
		t.Fatalf("merged record still waits on the merge:\n%s", record)
	}
}

func TestMergeExecutorNeverWalksASettledRewardEstimateBackwards(t *testing.T) {
	f := newMergeFixtureWith(t, "rewardCondition: merged", "rewardStatus: paid")
	result := f.request(t, "execute", "merge", nil)
	if result["status"] != "merged" {
		t.Fatalf("merge did not land: %+v", result)
	}
	record := f.show(t, result["pullsAfter"].(string), ".forkmesh/pulls/7/pull.md")
	if !strings.Contains(record, "rewardStatus: paid") {
		t.Fatalf("merging rewrote a paid receipt:\n%s", record)
	}
}

func TestMergeExecutorRefusesStaleUnreviewedAndConflictingRequests(t *testing.T) {
	f := newMergeFixture(t)
	stale := f.request(t, "execute", "merge", map[string]any{
		"requestId": "merge_request_stale", "expectedBaseOid": strings.Repeat("1", 40)})
	if stale["status"] != "failed" || stale["error"] != "stale_base" {
		t.Fatalf("stale base: %+v", stale)
	}
	staleHead := f.request(t, "execute", "merge", map[string]any{
		"requestId": "merge_request_head", "expectedHeadOid": strings.Repeat("2", 40)})
	if staleHead["error"] != "stale_head" {
		t.Fatalf("stale head: %+v", staleHead)
	}
	staleLedger := f.request(t, "execute", "merge", map[string]any{
		"requestId": "merge_request_ledger", "expectedPullsOid": strings.Repeat("3", 40)})
	if staleLedger["error"] != "stale_pull_metadata" {
		t.Fatalf("stale ledger: %+v", staleLedger)
	}
	if f.ref(t, "refs/heads/main") != f.mainOid {
		t.Fatal("a refused request moved main")
	}

	// Withdraw the approval: the merge is locked until a peer approves again.
	run(t, f.source, "git", "checkout", "-q", "forkmesh/pulls")
	writeFile(t, filepath.Join(f.source, ".forkmesh", "pulls", "7", "0003-review.md"),
		reviewRecord("bob", "changes_requested"))
	run(t, f.source, "git", "add", "-A")
	run(t, f.source, "git", "commit", "-q", "-m", "pull #7: review")
	run(t, f.source, "git", "push", "-q", f.bare, "forkmesh/pulls:refs/heads/forkmesh/pulls")
	f.pullsOid = run(t, f.source, "git", "rev-parse", "forkmesh/pulls")
	locked := f.request(t, "execute", "merge", map[string]any{"requestId": "merge_request_locked"})
	if locked["error"] != "review_required" {
		t.Fatalf("review gate: %+v", locked)
	}
	// The author's own approval never counts.
	writeFile(t, filepath.Join(f.source, ".forkmesh", "pulls", "7", "0004-review.md"),
		reviewRecord("bob", "approved"))
	writeFile(t, filepath.Join(f.source, ".forkmesh", "pulls", "7", "0005-review.md"),
		reviewRecord("alice", "approved"))
	run(t, f.source, "git", "add", "-A")
	run(t, f.source, "git", "commit", "-q", "-m", "pull #7: re-review")
	run(t, f.source, "git", "push", "-q", f.bare, "forkmesh/pulls:refs/heads/forkmesh/pulls")
	f.pullsOid = run(t, f.source, "git", "rev-parse", "forkmesh/pulls")

	// Move main so the feature conflicts.
	run(t, f.source, "git", "checkout", "-q", "main")
	writeFile(t, filepath.Join(f.source, "feature.txt"), "conflicting\n")
	run(t, f.source, "git", "add", "feature.txt")
	run(t, f.source, "git", "commit", "-q", "-m", "conflict")
	run(t, f.source, "git", "push", "-q", f.bare, "main:refs/heads/main")
	f.mainOid = run(t, f.source, "git", "rev-parse", "main")
	conflict := f.request(t, "execute", "merge", map[string]any{"requestId": "merge_request_conflict"})
	if conflict["error"] != "merge_conflict" {
		t.Fatalf("conflict: %+v", conflict)
	}
	if f.ref(t, "refs/heads/main") != f.mainOid || f.ref(t, "refs/heads/forkmesh/pulls") != f.pullsOid {
		t.Fatal("a conflicting merge moved refs")
	}
}

// The peer gate is the repository's default, not a lock on its owner: the
// relay authorizes and audits the waiver, and the executor honours it for
// merges only.
func TestMergeExecutorHonoursTheOwnerReviewWaiver(t *testing.T) {
	f := newMergeFixture(t)
	// Withdraw the only peer approval, then object outright.
	run(t, f.source, "git", "checkout", "-q", "forkmesh/pulls")
	writeFile(t, filepath.Join(f.source, ".forkmesh", "pulls", "7", "0002-review.md"),
		reviewRecord("bob", "changes_requested"))
	run(t, f.source, "git", "add", "-A")
	run(t, f.source, "git", "commit", "-q", "-m", "pull #7: changes requested")
	run(t, f.source, "git", "push", "-q", f.bare, "forkmesh/pulls:refs/heads/forkmesh/pulls")
	f.pullsOid = run(t, f.source, "git", "rev-parse", "forkmesh/pulls")
	run(t, f.source, "git", "checkout", "-q", "main")

	locked := f.request(t, "execute", "merge", map[string]any{"requestId": "merge_request_locked"})
	if locked["error"] != "review_required" {
		t.Fatalf("review gate: %+v", locked)
	}
	waived := f.request(t, "execute", "merge", map[string]any{
		"requestId": "merge_request_waived", "overrideReview": true})
	if waived["status"] != "merged" || waived["generationReady"] != true {
		t.Fatalf("owner waiver: %+v", waived)
	}
	if f.ref(t, "refs/heads/main") == f.mainOid {
		t.Fatal("the waived merge did not move main")
	}
	// The waiver is legible in the ledger the merge published.
	message := run(t, ".", "git", "--git-dir="+f.bare, "log", "-1", "--format=%s",
		"refs/heads/"+pullMetadataBranch)
	if !strings.Contains(message, "review gate waived by owner") {
		t.Fatalf("ledger commit does not record the waiver: %q", message)
	}
	if !strings.Contains(f.show(t, "refs/heads/"+pullMetadataBranch,
		".forkmesh/pulls/7/pull.md"), "status: merged") {
		t.Fatal("the waived merge left the pull open")
	}
}

// A waived request is a different request: it must not resolve to the record
// the reviewed merge of the same objects wrote, and it may not travel on the
// update executor, which has no review gate to waive.
func TestReviewWaiverIsDigestedAndMergeOnly(t *testing.T) {
	base, head, pulls := strings.Repeat("a", 40), strings.Repeat("b", 40), strings.Repeat("c", 40)
	body := func(executorType string, override bool) string {
		value := map[string]any{
			"schemaVersion": 1, "type": executorType, "action": "execute",
			"owner": "forkmesh", "repository": "forkmesh", "pullNumber": 7,
			"requestId":       "merge_request_0001",
			"expectedBaseOid": base, "expectedHeadOid": head, "expectedPullsOid": pulls,
		}
		if override {
			value["overrideReview"] = true
		}
		raw, _ := json.Marshal(value)
		return string(raw)
	}
	reviewed, err := parsePullMutationRequest(strings.NewReader(body(pullMergeExecutorType, false)))
	if err != nil {
		t.Fatal(err)
	}
	waived, err := parsePullMutationRequest(strings.NewReader(body(pullMergeExecutorType, true)))
	if err != nil {
		t.Fatal(err)
	}
	if reviewed.digest == waived.digest {
		t.Fatal("a waived merge shares its idempotency digest with the reviewed one")
	}
	if reviewed.OverrideReview || !waived.OverrideReview {
		t.Fatalf("waiver not parsed: %v %v", reviewed.OverrideReview, waived.OverrideReview)
	}
	if _, err := parsePullMutationRequest(
		strings.NewReader(body(pullUpdateExecutorType, true))); err == nil {
		t.Fatal("the update executor accepted a review waiver")
	}
}

func TestUpdateExecutorMergesBaseIntoHeadOnly(t *testing.T) {
	f := newMergeFixture(t)
	// Advance main so the feature branch is behind it.
	writeFile(t, filepath.Join(f.source, "README.md"), "two\n")
	run(t, f.source, "git", "add", "README.md")
	run(t, f.source, "git", "commit", "-q", "-m", "two")
	run(t, f.source, "git", "push", "-q", f.bare, "main:refs/heads/main")
	f.mainOid = run(t, f.source, "git", "rev-parse", "main")
	result := f.request(t, "execute", "update", nil)
	if result["status"] != "updated" {
		t.Fatalf("update failed: %+v", result)
	}
	headAfter := result["headAfter"].(string)
	if f.ref(t, "refs/heads/feature") != headAfter || f.ref(t, "refs/heads/main") != f.mainOid ||
		f.ref(t, "refs/heads/forkmesh/pulls") != f.pullsOid {
		t.Fatalf("update moved the wrong refs: %+v", result)
	}
	parents := strings.Fields(run(t, ".", "git", "--git-dir="+f.bare, "show", "-s", "--format=%P", headAfter))
	if len(parents) != 2 || parents[0] != f.featureOid || parents[1] != f.mainOid {
		t.Fatalf("update parents %v", parents)
	}
	verifyAttestation(t, f.identity, result, "forkmesh")
	f.featureOid = headAfter
	current := f.request(t, "execute", "update", map[string]any{"requestId": "merge_request_current"})
	if current["error"] != "already_current" {
		t.Fatalf("already current: %+v", current)
	}
}

func TestExecutorRegisterRecordsGenerationAndWakesDaemon(t *testing.T) {
	f := newMergeFixture(t)
	var wakes int32
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Method == http.MethodPost && r.URL.Path == "/v1/control/sync" {
			atomic.AddInt32(&wakes, 1)
			w.WriteHeader(http.StatusAccepted)
			return
		}
		http.NotFound(w, r)
	}))
	defer server.Close()
	f.executor.DaemonBaseURL = server.URL
	if gatewayServesCurrentGeneration(f.gatewayPath) {
		t.Fatal("generation marker present before register")
	}
	registered := f.request(t, "register", "merge", nil)
	if registered["status"] != "registered" || atomic.LoadInt32(&wakes) != 1 {
		t.Fatalf("register: %+v wakes=%d", registered, wakes)
	}
	if !gatewayServesCurrentGeneration(f.gatewayPath) {
		t.Fatal("generation marker not recorded")
	}
	// Any later configuration change invalidates the marker so the daemon
	// restarts the gateway as before.
	gateway, _, _ := loadGatewayConfig(f.gatewayPath)
	gateway.Repositories[0].Integrity.ExpectedRefsSHA256 = strings.Repeat("f", 64)
	if _, err := writeGatewayConfig(f.gatewayPath, gateway); err != nil {
		t.Fatal(err)
	}
	if gatewayServesCurrentGeneration(f.gatewayPath) {
		t.Fatal("stale marker still claims the generation")
	}
	// A merge wakes the daemon once it has re-pinned the configuration.
	result := f.request(t, "execute", "merge", nil)
	if result["status"] != "merged" {
		t.Fatalf("merge: %+v", result)
	}
	deadline := time.Now().Add(2 * time.Second)
	for atomic.LoadInt32(&wakes) < 2 && time.Now().Before(deadline) {
		time.Sleep(10 * time.Millisecond)
	}
	if atomic.LoadInt32(&wakes) < 2 {
		t.Fatal("merge did not wake the daemon")
	}
}

func TestExecutorRejectsMalformedRequestsAndUnknownRepositories(t *testing.T) {
	f := newMergeFixture(t)
	for _, body := range []string{
		"", "{", `{"schemaVersion":1}`,
		`{"schemaVersion":1,"type":"forkmesh.pull-merge-executor-v1","action":"drop","owner":"forkmesh","repository":"forkmesh","pullNumber":7,"requestId":"merge_request_0001","expectedBaseOid":"` + f.mainOid + `","expectedHeadOid":"` + f.featureOid + `","expectedPullsOid":"` + f.pullsOid + `"}`,
		`{"schemaVersion":1,"type":"forkmesh.pull-merge-executor-v1","action":"status","owner":"forkmesh","repository":"other","pullNumber":7,"requestId":"merge_request_0001","expectedBaseOid":"` + f.mainOid + `","expectedHeadOid":"` + f.featureOid + `","expectedPullsOid":"` + f.pullsOid + `"}`,
		`{"schemaVersion":1,"type":"forkmesh.pull-merge-executor-v1","action":"status","owner":"forkmesh","repository":"forkmesh","pullNumber":7,"requestId":"merge_request_0001","expectedBaseOid":"main","expectedHeadOid":"` + f.featureOid + `","expectedPullsOid":"` + f.pullsOid + `","extra":1}`,
	} {
		if _, err := f.executor.Handle(strings.NewReader(body)); err == nil {
			t.Fatalf("accepted %q", body)
		}
	}
	if f.ref(t, "refs/heads/main") != f.mainOid {
		t.Fatal("a rejected request moved main")
	}
}

func TestEnsureGatewayCapabilitiesUpgradesAndDisables(t *testing.T) {
	var gateway GatewayConfig
	gateway.Repositories = []Repository{
		{Owner: "forkmesh", Name: "forkmesh", Enabled: true, Operations: []string{"tree", "blob", "git-upload-pack"}},
		{Owner: "retired", Name: "forkmesh", Enabled: false, Operations: []string{"tree"}},
	}
	command := []string{"/usr/local/bin/forkmesh-mirror-node", "--config", "/etc/forkmesh/mirror-node.json", "--execute-mirror-merge"}
	ensureGatewayCapabilities(&gateway, command, true)
	ops := strings.Join(gateway.Repositories[0].Operations, " ")
	for _, want := range []string{"merge-preview", "merge-pull", "update-pull-branch", "tree", "git-info-refs"} {
		if !strings.Contains(" "+ops+" ", " "+want+" ") {
			t.Fatalf("missing %s in %s", want, ops)
		}
	}
	if strings.Join(gateway.MergeExecutorCommand, " ") != strings.Join(command, " ") {
		t.Fatalf("executor command %v", gateway.MergeExecutorCommand)
	}
	if strings.Join(gateway.Repositories[1].Operations, " ") != "tree" {
		t.Fatal("disabled repository was rewritten")
	}
	ensureGatewayCapabilities(&gateway, command, false)
	ops = strings.Join(gateway.Repositories[0].Operations, " ")
	if strings.Contains(ops, "merge-pull") || strings.Contains(ops, "update-pull-branch") || gateway.MergeExecutorCommand != nil {
		t.Fatalf("read-only node still advertises writes: %s %v", ops, gateway.MergeExecutorCommand)
	}
	if !strings.Contains(ops, "merge-preview") {
		t.Fatal("read-only node lost the preview operation")
	}
}

// commitLedgerFile publishes one file on the pull ledger and repins the fixture.
func (f *mergeFixture) commitLedgerFile(t *testing.T, path string, content []byte) {
	t.Helper()
	run(t, f.source, "git", "checkout", "-q", "forkmesh/pulls")
	writeFile(t, filepath.Join(f.source, path), string(content))
	run(t, f.source, "git", "add", "-A")
	run(t, f.source, "git", "commit", "-q", "-m", "pull #7: "+filepath.Base(path))
	run(t, f.source, "git", "push", "-q", f.bare, "forkmesh/pulls:refs/heads/forkmesh/pulls")
	f.pullsOid = run(t, f.source, "git", "rev-parse", "forkmesh/pulls")
	run(t, f.source, "git", "checkout", "-q", "main")
}

// A signed push of a required-checks policy gates the web merge: nothing but a
// fresh success, signed by a runner the base branch trusts, for the exact head,
// opens it — and the owner's review waiver does not.
func TestMergeExecutorEnforcesSignedRequiredChecks(t *testing.T) {
	f := newMergeFixture(t)
	now := time.Now()
	f.executor.Now = func() time.Time { return now }
	runner, outsider := newRunnerIdentity(t), newRunnerIdentity(t)
	writeFile(t, filepath.Join(f.source, requiredChecksPolicyPath), string(checkPolicyJSON(t, 3600,
		[]string{"main"}, map[string][]*Identity{"CI tests": {runner}})))
	run(t, f.source, "git", "add", "-A")
	run(t, f.source, "git", "commit", "-q", "-m", "require CI")
	run(t, f.source, "git", "push", "-q", f.bare, "main:refs/heads/main")
	f.mainOid = run(t, f.source, "git", "rev-parse", "main")

	attempt := func(id string, extra map[string]any) map[string]any {
		changes := map[string]any{"requestId": id}
		for key, value := range extra {
			changes[key] = value
		}
		return f.request(t, "execute", "merge", changes)
	}
	if result := attempt("merge_request_nochecks", nil); result["error"] != "checks_required" {
		t.Fatalf("no checks: %+v", result)
	}
	if result := attempt("merge_request_waivedchecks", map[string]any{"overrideReview": true}); result["error"] != "checks_required" {
		t.Fatalf("review waiver bypassed required checks: %+v", result)
	}

	report := checkReport{"forkmesh", "forkmesh", f.featureOid, "CI tests", "success", "run-1", now.Add(-time.Minute)}
	f.commitLedgerFile(t, ".forkmesh/pulls/7/checks/forged.json", signedCheck(t, outsider, report))
	stale := report
	stale.at = now.Add(-2 * time.Hour)
	f.commitLedgerFile(t, ".forkmesh/pulls/7/checks/stale.json", signedCheck(t, runner, stale))
	replayed := report
	replayed.commit = f.mainOid
	f.commitLedgerFile(t, ".forkmesh/pulls/7/checks/replayed.json", signedCheck(t, runner, replayed))
	if result := attempt("merge_request_badchecks", nil); result["error"] != "checks_required" {
		t.Fatalf("forged/stale/replayed checks: %+v", result)
	}
	if f.ref(t, "refs/heads/main") != f.mainOid {
		t.Fatal("a gated merge moved main")
	}

	f.commitLedgerFile(t, ".forkmesh/pulls/7/checks/ci.json", signedCheck(t, runner, report))
	result := attempt("merge_request_checked", nil)
	if result["status"] != "merged" {
		t.Fatalf("a passing signed check did not open the gate: %+v", result)
	}
	if f.show(t, result["baseAfter"].(string), "feature.txt") != "feature" {
		t.Fatal("the checked merge lacks the pull's change")
	}
}

// An unreadable policy locks merges rather than disabling the gate.
func TestMergeExecutorFailsClosedOnAnInvalidChecksPolicy(t *testing.T) {
	f := newMergeFixture(t)
	writeFile(t, filepath.Join(f.source, requiredChecksPolicyPath), `{"schema":"forkmesh-required-checks-v1"}`)
	run(t, f.source, "git", "add", "-A")
	run(t, f.source, "git", "commit", "-q", "-m", "broken policy")
	run(t, f.source, "git", "push", "-q", f.bare, "main:refs/heads/main")
	f.mainOid = run(t, f.source, "git", "rev-parse", "main")
	if result := f.request(t, "execute", "merge", nil); result["error"] != "checks_required" {
		t.Fatalf("invalid policy: %+v", result)
	}
}
