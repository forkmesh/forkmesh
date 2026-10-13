package mirrornode

import (
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

const agentBranchName = "agent/12-fix-login"

func (f *mergeFixture) pushAgentBranch(t *testing.T, file, content string) string {
	t.Helper()
	run(t, f.source, "git", "checkout", "-q", "-B", "agentwork", "main")
	writeFile(t, filepath.Join(f.source, file), content)
	run(t, f.source, "git", "add", file)
	run(t, f.source, "git", "commit", "-q", "-m", "agent: "+file)
	run(t, f.source, "git", "push", "-q", "-f", f.bare, "agentwork:refs/heads/"+agentBranchName)
	tip := run(t, f.source, "git", "rev-parse", "agentwork")
	run(t, f.source, "git", "checkout", "-q", "main")
	return tip
}

func (f *mergeFixture) advanceMain(t *testing.T, file, content string) {
	t.Helper()
	run(t, f.source, "git", "checkout", "-q", "main")
	run(t, f.source, "git", "fetch", "-q", f.bare, "main")
	run(t, f.source, "git", "merge", "-q", "--ff-only", "FETCH_HEAD")
	writeFile(t, filepath.Join(f.source, file), content)
	run(t, f.source, "git", "add", file)
	run(t, f.source, "git", "commit", "-q", "-m", "main: "+file)
	run(t, f.source, "git", "push", "-q", f.bare, "main:refs/heads/main")
	f.mainOid = run(t, f.source, "git", "rev-parse", "main")
}

func (f *mergeFixture) branchRequest(t *testing.T, action, executorType string, fields map[string]any) (map[string]any, error) {
	t.Helper()
	value := map[string]any{
		"schemaVersion": 1, "type": executorType, "action": action,
		"owner": "forkmesh", "repository": "forkmesh",
		"requestId": "branch_request_0001",
		"branch":    agentBranchName, "base": "main",
	}
	for key, item := range fields {
		if item == nil {
			delete(value, key)
			continue
		}
		value[key] = item
	}
	raw, _ := json.Marshal(value)
	return f.executor.Handle(strings.NewReader(string(raw)))
}

func (f *mergeFixture) mustBranch(t *testing.T, action, executorType string, fields map[string]any) map[string]any {
	t.Helper()
	result, err := f.branchRequest(t, action, executorType, fields)
	if err != nil {
		t.Fatalf("%s %s: %v", action, executorType, err)
	}
	return result
}

func (f *mergeFixture) refOrEmpty(ref string) string {
	return gitRefValue(context.Background(), f.bare, ref)
}

func TestBranchMergeMakesAMergeCommitAndCleansUpAfterSavingTheTip(t *testing.T) {
	f := newMergeFixture(t)
	tip := f.pushAgentBranch(t, "login.txt", "fixed\n")
	f.advanceMain(t, "README.md", "moved\n")
	fields := map[string]any{"expectedBaseOid": f.mainOid, "expectedHeadOid": tip, "deleteBranch": true}
	result := f.mustBranch(t, "execute", branchMergeExecutorType, fields)
	if result["status"] != "merged" || result["branchDeleted"] != true || result["generationReady"] != true {
		t.Fatalf("merge did not land: %+v", result)
	}
	if _, ok := result["pullsBefore"]; ok {
		t.Fatalf("a branch merge leaked pull fields: %+v", result)
	}
	baseAfter := result["baseAfter"].(string)
	if f.ref(t, "refs/heads/main") != baseAfter {
		t.Fatal("main did not move to the merge")
	}
	parents := strings.Fields(run(t, ".", "git", "--git-dir="+f.bare, "show", "-s", "--format=%P", baseAfter))
	if len(parents) != 2 || parents[0] != f.mainOid || parents[1] != tip {
		t.Fatalf("merge parents %v", parents)
	}
	subject := run(t, ".", "git", "--git-dir="+f.bare, "show", "-s", "--format=%s", baseAfter)
	if subject != "Merge branch '"+agentBranchName+"' into main" {
		t.Fatalf("merge subject %q", subject)
	}
	if f.refOrEmpty("refs/heads/"+agentBranchName) != "" {
		t.Fatal("the merged agent branch is still there")
	}
	trash, _ := result["trashRef"].(string)
	if !strings.HasPrefix(trash, agentBranchTrashPrefix+agentBranchName+"/") || f.ref(t, trash) != tip {
		t.Fatalf("the tip was not saved before the delete: %q", trash)
	}
	verifyAttestation(t, f.identity, result, "forkmesh")
	again := f.mustBranch(t, "execute", branchMergeExecutorType, fields)
	if again["baseAfter"] != baseAfter || again["status"] != "merged" {
		t.Fatalf("replay changed the outcome: %+v", again)
	}
	status := f.mustBranch(t, "status", branchMergeExecutorType, fields)
	if status["status"] != "merged" || status["branchDeleted"] != true {
		t.Fatalf("status after merge: %+v", status)
	}
}

func TestBranchMergeOfAFastForwardableBranchStillRecordsAMerge(t *testing.T) {
	f := newMergeFixture(t)
	tip := f.pushAgentBranch(t, "login.txt", "fixed\n")
	result := f.mustBranch(t, "execute", branchMergeExecutorType,
		map[string]any{"expectedBaseOid": f.mainOid, "expectedHeadOid": tip})
	if result["status"] != "merged" || result["branchDeleted"] == true {
		t.Fatalf("merge: %+v", result)
	}
	parents := strings.Fields(run(t, ".", "git", "--git-dir="+f.bare, "show", "-s", "--format=%P", result["baseAfter"].(string)))
	if len(parents) != 2 {
		t.Fatalf("expected a merge commit, got parents %v", parents)
	}
	if f.ref(t, "refs/heads/"+agentBranchName) != tip {
		t.Fatal("a merge without cleanup removed the branch")
	}
}

func TestBranchMergeReportsConflictsAndStaleRefsWithoutMovingAnything(t *testing.T) {
	f := newMergeFixture(t)
	tip := f.pushAgentBranch(t, "README.md", "agent side\n")
	before := f.mainOid
	f.advanceMain(t, "README.md", "main side\n")
	conflict := f.mustBranch(t, "execute", branchMergeExecutorType,
		map[string]any{"expectedBaseOid": f.mainOid, "expectedHeadOid": tip, "deleteBranch": true})
	if conflict["status"] != "failed" || conflict["error"] != "merge_conflict" {
		t.Fatalf("conflict: %+v", conflict)
	}
	if f.ref(t, "refs/heads/main") != f.mainOid || f.ref(t, "refs/heads/"+agentBranchName) != tip {
		t.Fatal("a conflicting merge moved a ref")
	}
	staleBase := f.mustBranch(t, "execute", branchMergeExecutorType, map[string]any{
		"requestId": "branch_request_0002", "expectedBaseOid": before, "expectedHeadOid": tip})
	if staleBase["error"] != "stale_base" {
		t.Fatalf("stale base: %+v", staleBase)
	}
	staleHead := f.mustBranch(t, "execute", branchMergeExecutorType, map[string]any{
		"requestId": "branch_request_0003", "expectedBaseOid": f.mainOid, "expectedHeadOid": f.mainOid})
	if staleHead["error"] != "stale_head" {
		t.Fatalf("stale head: %+v", staleHead)
	}
}

func TestBranchCleanupRefusesUnmergedWorkAndRemovesMergedBranches(t *testing.T) {
	f := newMergeFixture(t)
	tip := f.pushAgentBranch(t, "login.txt", "fixed\n")
	refused := f.mustBranch(t, "execute", branchDeleteExecutorType,
		map[string]any{"expectedHeadOid": tip})
	if refused["status"] != "failed" || refused["error"] != "not_merged" {
		t.Fatalf("cleanup of unmerged work: %+v", refused)
	}
	if f.ref(t, "refs/heads/"+agentBranchName) != tip {
		t.Fatal("unmerged work was deleted")
	}
	merged := f.mustBranch(t, "execute", branchMergeExecutorType, map[string]any{
		"requestId": "branch_request_0002", "expectedBaseOid": f.mainOid, "expectedHeadOid": tip})
	if merged["status"] != "merged" {
		t.Fatalf("merge: %+v", merged)
	}
	again := f.mustBranch(t, "execute", branchMergeExecutorType, map[string]any{
		"requestId": "branch_request_0003", "expectedBaseOid": merged["baseAfter"], "expectedHeadOid": tip})
	if again["error"] != "already_merged" {
		t.Fatalf("second merge: %+v", again)
	}
	cleaned := f.mustBranch(t, "execute", branchDeleteExecutorType, map[string]any{
		"requestId": "branch_request_0004", "expectedHeadOid": tip})
	if cleaned["status"] != "deleted" || cleaned["branchDeleted"] != true {
		t.Fatalf("cleanup: %+v", cleaned)
	}
	if f.refOrEmpty("refs/heads/"+agentBranchName) != "" || f.ref(t, cleaned["trashRef"].(string)) != tip {
		t.Fatal("cleanup did not save the tip and remove the branch")
	}
	gone := f.mustBranch(t, "execute", branchDeleteExecutorType, map[string]any{
		"requestId": "branch_request_0005", "expectedHeadOid": tip})
	if gone["error"] != "branch_not_found" {
		t.Fatalf("cleanup of a removed branch: %+v", gone)
	}
}

func TestBranchDiscardRemovesUnmergedWorkIntoTheTrash(t *testing.T) {
	f := newMergeFixture(t)
	tip := f.pushAgentBranch(t, "login.txt", "fixed\n")
	stale := f.mustBranch(t, "execute", branchDeleteExecutorType,
		map[string]any{"expectedHeadOid": f.mainOid, "discard": true})
	if stale["error"] != "stale_head" || f.ref(t, "refs/heads/"+agentBranchName) != tip {
		t.Fatalf("discard with a stale head: %+v", stale)
	}
	discarded := f.mustBranch(t, "execute", branchDeleteExecutorType, map[string]any{
		"requestId": "branch_request_0002", "expectedHeadOid": tip, "discard": true})
	if discarded["status"] != "deleted" || discarded["branchDeleted"] != true {
		t.Fatalf("discard: %+v", discarded)
	}
	if f.refOrEmpty("refs/heads/"+agentBranchName) != "" || f.ref(t, discarded["trashRef"].(string)) != tip {
		t.Fatal("discard did not save the tip and remove the branch")
	}
	if f.ref(t, "refs/heads/main") != f.mainOid {
		t.Fatal("discard moved the base")
	}
}

func TestBranchMutationsRefuseInternalBranchesAndMixedRequests(t *testing.T) {
	f := newMergeFixture(t)
	tip := f.pushAgentBranch(t, "login.txt", "fixed\n")
	for _, fields := range []map[string]any{
		{"branch": "agent/", "expectedBaseOid": f.mainOid, "expectedHeadOid": tip},
		{"branch": "forkmesh/pulls", "expectedBaseOid": f.mainOid, "expectedHeadOid": tip},
		{"branch": "HEAD", "expectedBaseOid": f.mainOid, "expectedHeadOid": tip},
		{"branch": "main", "expectedBaseOid": f.mainOid, "expectedHeadOid": f.mainOid},
		{"base": "forkmesh/pulls", "expectedBaseOid": f.mainOid, "expectedHeadOid": tip},
		{"base": "forkmesh/pull/7", "expectedBaseOid": f.mainOid, "expectedHeadOid": tip},
		{"expectedHeadOid": tip},
		{"expectedBaseOid": f.mainOid, "expectedHeadOid": tip, "pullNumber": 7},
		{"expectedBaseOid": f.mainOid, "expectedHeadOid": tip, "expectedPullsOid": f.pullsOid},
		{"expectedBaseOid": f.mainOid, "expectedHeadOid": tip, "overrideReview": true},
	} {
		if _, err := f.branchRequest(t, "execute", branchMergeExecutorType, fields); err == nil {
			t.Fatalf("accepted %+v", fields)
		}
	}
	if _, err := f.branchRequest(t, "execute", branchDeleteExecutorType,
		map[string]any{"expectedHeadOid": tip, "deleteBranch": true}); err == nil {
		t.Fatal("a cleanup accepted deleteBranch")
	}
	if _, err := f.branchRequest(t, "execute", branchMergeExecutorType,
		map[string]any{"expectedBaseOid": f.mainOid, "expectedHeadOid": tip, "discard": true}); err == nil {
		t.Fatal("a merge accepted discard")
	}
	if _, err := f.executor.Handle(strings.NewReader(`{"schemaVersion":1,"type":"forkmesh.pull-merge-executor-v1","action":"status","owner":"forkmesh","repository":"forkmesh","pullNumber":7,"requestId":"merge_request_0001","expectedBaseOid":"` + f.mainOid + `","expectedHeadOid":"` + f.featureOid + `","expectedPullsOid":"` + f.pullsOid + `","branch":"` + agentBranchName + `"}`)); err == nil {
		t.Fatal("a pull request accepted branch fields")
	}
	if f.ref(t, "refs/heads/main") != f.mainOid || f.ref(t, "refs/heads/"+agentBranchName) != tip {
		t.Fatal("a refused request moved a ref")
	}
}

func TestReconcileDoesNotFetchACleanedUpAgentBranchBack(t *testing.T) {
	f := newMergeFixture(t)
	tip := f.pushAgentBranch(t, "login.txt", "fixed\n")
	f.mustBranch(t, "execute", branchMergeExecutorType,
		map[string]any{"expectedBaseOid": f.mainOid, "expectedHeadOid": tip, "deleteBranch": true})
	ctx := context.Background()
	run(t, ".", "git", "--git-dir="+f.bare, "update-ref", "refs/forkmesh/upstream/heads/"+agentBranchName, tip)
	if err := reconcileUpstreamRefs(ctx, f.bare); err != nil {
		t.Fatal(err)
	}
	if f.refOrEmpty("refs/heads/"+agentBranchName) != "" {
		t.Fatal("a peer handed the cleaned-up branch back")
	}
	f.advanceMain(t, "later.txt", "later\n")
	newer := f.pushAgentBranch(t, "login.txt", "fixed again\n")
	run(t, ".", "git", "--git-dir="+f.bare, "update-ref", "-d", "refs/heads/"+agentBranchName)
	run(t, ".", "git", "--git-dir="+f.bare, "update-ref", "refs/forkmesh/upstream/heads/"+agentBranchName, newer)
	if err := reconcileUpstreamRefs(ctx, f.bare); err != nil {
		t.Fatal(err)
	}
	if f.refOrEmpty("refs/heads/"+agentBranchName) != newer {
		t.Fatal("new work on the same branch name was not replicated")
	}
}

func TestAgentBranchOperationsNeedTheExecutorAndAGatewayThatKnowsThem(t *testing.T) {
	var gateway GatewayConfig
	gateway.Repositories = []Repository{
		{Owner: "forkmesh", Name: "forkmesh", Enabled: true, Operations: []string{"tree"}},
		{Owner: "retired", Name: "forkmesh", Enabled: false, Operations: []string{"tree"}},
	}
	command := []string{"/usr/local/bin/forkmesh-mirror-node", "--execute-mirror-merge"}
	ensureGatewayCapabilities(&gateway, command, true)
	ensureGatewayAgentBranchOperations(&gateway, true)
	ops := " " + strings.Join(gateway.Repositories[0].Operations, " ") + " "
	for _, want := range agentBranchGatewayOperations {
		if !strings.Contains(ops, " "+want+" ") {
			t.Fatalf("missing %s in %s", want, ops)
		}
	}
	ensureGatewayAgentBranchOperations(&gateway, true)
	if strings.Count(strings.Join(gateway.Repositories[0].Operations, " "), agentBranchMergeOp) != 1 {
		t.Fatal("operations were added twice")
	}
	if strings.Join(gateway.Repositories[1].Operations, " ") != "tree" {
		t.Fatal("a disabled repository was rewritten")
	}
	ensureGatewayAgentBranchOperations(&gateway, false)
	if strings.Contains(strings.Join(gateway.Repositories[0].Operations, " "), "agent-branch") {
		t.Fatal("operations stayed after they were withdrawn")
	}
	ensureGatewayCapabilities(&gateway, command, false)
	ensureGatewayAgentBranchOperations(&gateway, true)
	if strings.Contains(strings.Join(gateway.Repositories[0].Operations, " "), "agent-branch") {
		t.Fatal("a read-only node advertised branch merges")
	}
	dir := t.TempDir()
	old := filepath.Join(dir, "old.py")
	current := filepath.Join(dir, "new.py")
	if err := os.WriteFile(old, []byte(`OPS = {"merge-pull"}`), 0600); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(current, []byte(`OPS = {"merge-pull", "merge-agent-branch"}`), 0600); err != nil {
		t.Fatal(err)
	}
	if gatewayScriptSupports(old, agentBranchMergeOp) || !gatewayScriptSupports(current, agentBranchMergeOp) ||
		gatewayScriptSupports(filepath.Join(dir, "missing.py"), agentBranchMergeOp) {
		t.Fatal("gateway script probe")
	}
}

func TestAnyBranchMergesIntoItsBaseAndIsCleanedUpIntoItsOwnTrash(t *testing.T) {
	f := newMergeFixture(t)
	merged := f.mustBranch(t, "execute", branchMergeExecutorType, map[string]any{
		"branch": "feature", "expectedBaseOid": f.mainOid, "expectedHeadOid": f.featureOid,
		"deleteBranch": true})
	if merged["status"] != "merged" || merged["branchDeleted"] != true {
		t.Fatalf("merge of a plain branch: %+v", merged)
	}
	if f.refOrEmpty("refs/heads/feature") != "" {
		t.Fatal("the merged branch was kept")
	}
	trash := merged["trashRef"].(string)
	if !strings.HasPrefix(trash, branchTrashPrefix+"feature/") || f.ref(t, trash) != f.featureOid {
		t.Fatalf("the tip was not saved under the branch trash: %s", trash)
	}
	ctx := context.Background()
	run(t, ".", "git", "--git-dir="+f.bare, "update-ref", "refs/forkmesh/upstream/heads/feature", f.featureOid)
	if err := reconcileUpstreamRefs(ctx, f.bare); err != nil {
		t.Fatal(err)
	}
	if f.refOrEmpty("refs/heads/feature") != "" {
		t.Fatal("a peer handed the deleted branch back")
	}
}

func TestUpdatingABranchMergesItsBaseIntoIt(t *testing.T) {
	f := newMergeFixture(t)
	tip := f.pushAgentBranch(t, "login.txt", "fixed\n")
	f.advanceMain(t, "later.txt", "later\n")
	updated := f.mustBranch(t, "execute", branchMergeExecutorType, map[string]any{
		"branch": "main", "base": agentBranchName,
		"expectedBaseOid": tip, "expectedHeadOid": f.mainOid})
	if updated["status"] != "merged" || updated["branchDeleted"] == true {
		t.Fatalf("update from main: %+v", updated)
	}
	after := f.ref(t, "refs/heads/"+agentBranchName)
	if after != updated["baseAfter"] || f.ref(t, "refs/heads/main") != f.mainOid {
		t.Fatal("the update moved the wrong branch")
	}
	parents := strings.Fields(run(t, ".", "git", "--git-dir="+f.bare, "rev-list", "--parents", "-n", "1", after))
	if len(parents) != 3 || parents[1] != tip || parents[2] != f.mainOid {
		t.Fatalf("the update is not a merge of main into the branch: %v", parents)
	}
	again := f.mustBranch(t, "execute", branchMergeExecutorType, map[string]any{
		"requestId": "branch_request_0002", "branch": "main", "base": agentBranchName,
		"expectedBaseOid": after, "expectedHeadOid": f.mainOid})
	if again["error"] != "already_merged" {
		t.Fatalf("an up-to-date branch: %+v", again)
	}
}

func TestTheDefaultBranchIsNeverDeleted(t *testing.T) {
	f := newMergeFixture(t)
	merging := f.mustBranch(t, "execute", branchMergeExecutorType, map[string]any{
		"branch": "main", "base": "feature", "expectedBaseOid": f.featureOid,
		"expectedHeadOid": f.mainOid, "deleteBranch": true})
	if merging["error"] != "unsupported_branch" {
		t.Fatalf("merge and delete of the default branch: %+v", merging)
	}
	deleting := f.mustBranch(t, "execute", branchDeleteExecutorType, map[string]any{
		"requestId": "branch_request_0002", "branch": "main", "base": "feature",
		"expectedHeadOid": f.mainOid, "discard": true})
	if deleting["error"] != "unsupported_branch" {
		t.Fatalf("discard of the default branch: %+v", deleting)
	}
	if f.ref(t, "refs/heads/main") != f.mainOid || f.ref(t, "refs/heads/feature") != f.featureOid {
		t.Fatal("a refused request moved a ref")
	}
}
