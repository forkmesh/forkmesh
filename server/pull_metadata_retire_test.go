package mirrornode

import (
	"context"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
)

type retireFixture struct {
	source, bare string
}

func newRetireFixture(t *testing.T) *retireFixture {
	t.Helper()
	source, bare := makeBareRepo(t)
	run(t, source, "git", "checkout", "-q", "--orphan", "forkmesh/pulls")
	run(t, source, "git", "rm", "-rfq", ".")
	return &retireFixture{source: source, bare: bare}
}

func mergedRecord(record string) string {
	return strings.Replace(record, "status: open", "status: merged", 1)
}

func (f *retireFixture) writeLedger(t *testing.T, message string, records map[int]string) string {
	t.Helper()
	for number, record := range records {
		writeFile(t, filepath.Join(f.source, ".forkmesh", "pulls", strconv.Itoa(number), "pull.md"), record)
	}
	run(t, f.source, "git", "add", "-A")
	run(t, f.source, "git", "commit", "-q", "--allow-empty", "-m", message)
	run(t, f.source, "git", "push", "-q", "-f", f.bare, "forkmesh/pulls:refs/heads/forkmesh/pulls")
	return run(t, f.source, "git", "rev-parse", "forkmesh/pulls")
}

func (f *retireFixture) publishBranch(t *testing.T, number int, ledger string) string {
	t.Helper()
	ctx := context.Background()
	tip, old, err := materializePullMetadataCommit(ctx, f.bare, number, ledger, "mirror9", "mirror9@test")
	if err != nil {
		t.Fatal(err)
	}
	args := []string{"git", "--git-dir=" + f.bare, "update-ref", pullMetadataRef(number), tip}
	if old != "" {
		args = append(args, old)
	}
	run(t, ".", args...)
	run(t, ".", "git", "--git-dir="+f.bare, "update-ref", pullMetadataCompatibilityRef(number), tip)
	return tip
}

func (f *retireFixture) ref(ref string) string {
	return gitRefValue(context.Background(), f.bare, ref)
}

func TestRetireMergedPullBranchesKeepsTheRecordAndDropsTheBranch(t *testing.T) {
	f := newRetireFixture(t)
	reward := []string{"rewardId: rw_14", "rewardStatus: pending_wallet", "rewardCondition: merged"}
	records := map[int]string{
		1: pullRecord(1, "Merged both places", "main", "a", "alice", false, "", "", reward...),
		2: pullRecord(2, "Still open", "main", "b", "bob", false, "", ""),
		3: pullRecord(3, "Ledger merged, branch lagging", "main", "c", "carol", false, "", ""),
		4: pullRecord(4, "Branch merged, ledger lagging", "main", "d", "dave", false, "", ""),
		5: strings.Replace(pullRecord(5, "Reused number", "main", "e", "erin", false, "", ""),
			"ts: 1700000000000", "ts: 1700000009999", 1),
	}
	opened := f.writeLedger(t, "open", records)
	tips := map[int]string{}
	for number := 1; number <= 5; number++ {
		tips[number] = f.publishBranch(t, number, opened)
	}
	mergedBoth := f.writeLedger(t, "merge 1 and 4", map[int]string{
		1: mergedRecord(records[1]), 4: mergedRecord(records[4])})
	tips[1] = f.publishBranch(t, 1, mergedBoth)
	tips[4] = f.publishBranch(t, 4, mergedBoth)
	f.writeLedger(t, "ledger after", map[int]string{
		3: mergedRecord(records[3]), 4: records[4],
		5: mergedRecord(pullRecord(5, "Older pull", "main", "e", "erin", false, "", "")),
	})

	retired, err := retireMergedPullBranches(context.Background(), f.bare, "mirror9")
	if err != nil || retired != 2 {
		t.Fatalf("retired %d, err %v", retired, err)
	}
	for _, number := range []int{1, 3} {
		if f.ref(pullMetadataRef(number)) != "" {
			t.Fatalf("merged pull #%d kept its branch", number)
		}
		kept := f.ref(pullMetadataCompatibilityRef(number))
		if kept == "" || !gitIsAncestor(context.Background(), f.bare, tips[number], kept) {
			t.Fatalf("pull #%d metadata ref %q does not contain the retired tip %s", number, kept, tips[number])
		}
		if !pullRecordMerged(pullRecordAt(context.Background(), f.bare, kept, number)) {
			t.Fatalf("pull #%d metadata ref does not record the merge", number)
		}
	}
	if f.ref(pullMetadataCompatibilityRef(1)) != tips[1] {
		t.Fatal("a branch that already recorded the merge was not kept as is")
	}
	record := pullRecordAt(context.Background(), f.bare, pullMetadataCompatibilityRef(1), 1)
	if record.values["rewardId"] != "rw_14" {
		t.Fatalf("reward fields lost: %+v", record.values)
	}
	for _, number := range []int{2, 4, 5} {
		if f.ref(pullMetadataRef(number)) != tips[number] {
			t.Fatalf("pull #%d branch moved although it is not retirable", number)
		}
	}
	if again, err := retireMergedPullBranches(context.Background(), f.bare, "mirror9"); err != nil || again != 0 {
		t.Fatalf("second sweep retired %d, err %v", again, err)
	}
	run(t, ".", "git", "--git-dir="+f.bare, "update-ref", "refs/pr/40/metadata", tips[1])
	reserved := reservedPullNumbers(context.Background(), f.bare)
	if !reserved[1] || !reserved[3] || !reserved[40] {
		t.Fatalf("metadata refs do not reserve their numbers: %v", reserved)
	}
}

func TestPullIntakeOnAMergedPullUpdatesOnlyItsMetadataRef(t *testing.T) {
	f := newRetireFixture(t)
	record := mergedRecord(pullRecord(7, "Merged", "main", "feature", "alice", false, "", "", "rewardId: rw_36"))
	ledger := f.writeLedger(t, "merged", map[int]string{7: record})
	branchTip := f.publishBranch(t, 7, ledger)
	repo := Repository{Owner: "forkmesh", Name: "forkmesh", GitDir: f.bare}
	number, err := materializeDirectCollaboration(context.Background(), repo, "pulls", collaborationItem{
		Number: 7,
		Event: map[string]any{
			"type": "comment", "id": "late-comment", "body": "thanks",
			"author": "bob", "ts": 1700000005000, "sig": "comment-signature",
		},
	}, "mirror9")
	if err != nil || number != 7 {
		t.Fatalf("intake failed: %d %v", number, err)
	}
	if f.ref(pullMetadataRef(7)) != "" {
		t.Fatal("intake recreated a merged pull's branch")
	}
	kept := f.ref(pullMetadataCompatibilityRef(7))
	if kept == branchTip || !gitIsAncestor(context.Background(), f.bare, branchTip, kept) {
		t.Fatalf("metadata ref %s does not extend the retired branch %s", kept, branchTip)
	}
	listing := run(t, ".", "git", "--git-dir="+f.bare, "ls-tree", "-r", "--name-only", kept)
	if !strings.Contains(listing, ".forkmesh/pulls/7/0002-comment.md") {
		t.Fatalf("late event missing from the metadata ref:\n%s", listing)
	}
	if again, err := materializeDirectCollaboration(context.Background(), repo, "pulls", collaborationItem{
		Number: 7,
		Event: map[string]any{
			"type": "comment", "id": "later-comment", "body": "again",
			"author": "bob", "ts": 1700000006000, "sig": "comment-signature-2",
		},
	}, "mirror9"); err != nil || again != 7 || f.ref(pullMetadataRef(7)) != "" {
		t.Fatalf("second event on a merged pull: %d %v", again, err)
	}
	if !gitIsAncestor(context.Background(), f.bare, kept, f.ref(pullMetadataCompatibilityRef(7))) {
		t.Fatal("metadata ref history was not extended")
	}
}

func TestUpstreamSyncDoesNotRecreateAMergedPullBranch(t *testing.T) {
	upstream := newRetireFixture(t)
	opened := pullRecord(9, "Merged upstream", "main", "x", "alice", false, "", "")
	ledger := upstream.writeLedger(t, "merged", map[int]string{
		9: mergedRecord(opened), 10: pullRecord(10, "Open upstream", "main", "y", "bob", false, "", "")})
	mergedTip := upstream.publishBranch(t, 9, ledger)
	openTip := upstream.publishBranch(t, 10, ledger)

	_, local := makeBareRepo(t)
	if err := syncRepository(context.Background(), Repository{GitDir: local}, []string{upstream.bare}, ""); err != nil {
		t.Fatal(err)
	}
	if got := gitRefValue(context.Background(), local, pullMetadataRef(9)); got != "" {
		t.Fatalf("merged pull branch was recreated from the upstream: %s", got)
	}
	if got := gitRefValue(context.Background(), local, pullMetadataCompatibilityRef(9)); got != mergedTip {
		t.Fatalf("merged pull metadata ref = %q, want %s", got, mergedTip)
	}
	if got := gitRefValue(context.Background(), local, pullMetadataRef(10)); got != openTip {
		t.Fatalf("open pull branch = %q, want %s", got, openTip)
	}
	if got := gitRefValue(context.Background(), local, "refs/heads/forkmesh/pulls"); got != ledger {
		t.Fatalf("ledger = %q, want %s", got, ledger)
	}
}
