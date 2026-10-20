package mirrornode

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"strings"
)

const (
	branchMergeExecutorType  = "forkmesh.branch-merge-executor-v1"
	branchDeleteExecutorType = "forkmesh.branch-delete-executor-v1"
	branchMergeJobType       = "forkmesh.branch-merge-job-v1"
	branchDeleteJobType      = "forkmesh.branch-delete-job-v1"
	agentBranchPrefix        = "agent/"
	agentBranchTrashPrefix   = "refs/forkmesh/trash/agent-branch/"
	branchTrashPrefix        = "refs/forkmesh/trash/branch/"
	agentBranchMergeOp       = "merge-agent-branch"
	agentBranchDeleteOp      = "delete-agent-branch"
)

var agentBranchGatewayOperations = []string{agentBranchMergeOp, agentBranchDeleteOp}

var branchMergeProfile = pullMutationProfile{
	executorType:  branchMergeExecutorType,
	jobType:       branchMergeJobType,
	terminal:      "merged",
	completedRefs: "refs/forkmesh/branch-merge-completed/",
	afterFields:   []string{"baseAfter"},
	failures: map[string]bool{
		"already_merged": true, "branch_not_found": true, "merge_conflict": true,
		"stale_base": true, "stale_head": true, "unsupported_branch": true,
	},
}

var branchDeleteProfile = pullMutationProfile{
	executorType:  branchDeleteExecutorType,
	jobType:       branchDeleteJobType,
	terminal:      "deleted",
	completedRefs: "refs/forkmesh/branch-delete-completed/",
	failures: map[string]bool{
		"branch_not_found": true, "not_merged": true, "stale_base": true,
		"stale_head": true, "unsupported_branch": true,
	},
}

func isBranchProfile(profile pullMutationProfile) bool {
	return profile.executorType == branchMergeExecutorType || profile.executorType == branchDeleteExecutorType
}

func validAgentBranch(value string) string {
	branch := validPullBranch(value)
	if !strings.HasPrefix(branch, agentBranchPrefix) || len(branch) == len(agentBranchPrefix) {
		return ""
	}
	return branch
}

func validManagedBranch(value string) string {
	branch := validPullBranch(value)
	if branch == "" || branch == "HEAD" || strings.HasPrefix(branch, "forkmesh/") ||
		branch == agentBranchPrefix {
		return ""
	}
	return branch
}

const gitBranchMaxBytes = 255

func gitBranchName(value string) string {
	name := strings.TrimSpace(value)
	if name == "" || len(name) > gitBranchMaxBytes || name == "HEAD" || name == "@" ||
		strings.HasPrefix(name, "-") || strings.HasPrefix(name, "/") ||
		strings.HasSuffix(name, "/") || strings.HasSuffix(name, ".") ||
		strings.Contains(name, "//") || strings.Contains(name, "..") || strings.Contains(name, "@{") ||
		strings.ContainsAny(name, " ~^:?*[\\") {
		return ""
	}
	for _, r := range name {
		if r < 0x20 || r == 0x7f {
			return ""
		}
	}
	for _, part := range strings.Split(name, "/") {
		if strings.HasPrefix(part, ".") || strings.HasSuffix(part, ".lock") {
			return ""
		}
	}
	return name
}

func validDeletableBranch(value string) string {
	branch := gitBranchName(value)
	if branch == "" || strings.HasPrefix(branch, "forkmesh/") || branch == agentBranchPrefix {
		return ""
	}
	return branch
}

func validMergeBase(value string) string {
	return validManagedBranch(value)
}

func branchTrashPrefixFor(branch string) string {
	if validAgentBranch(branch) != "" {
		return agentBranchTrashPrefix
	}
	return branchTrashPrefix
}

func parseBranchMutation(request *pullMutationRequest) error {
	if request.PullNumber.String() != "" || request.ExpectedPullsOid != "" || request.OverrideReview {
		return errors.New("unsupported executor request protocol")
	}
	if request.profile.executorType == branchDeleteExecutorType && request.DeleteBranch {
		return errors.New("unsupported executor request protocol")
	}
	if request.profile.executorType != branchDeleteExecutorType && request.Discard {
		return errors.New("unsupported executor request protocol")
	}
	request.Owner = strings.ToLower(strings.TrimSpace(request.Owner))
	request.Repository = strings.TrimSpace(request.Repository)
	request.RequestID = strings.TrimSpace(request.RequestID)
	request.ExpectedBaseOid = strings.ToLower(strings.TrimSpace(request.ExpectedBaseOid))
	request.ExpectedHeadOid = strings.ToLower(strings.TrimSpace(request.ExpectedHeadOid))
	branch := validManagedBranch(request.Branch)
	if request.profile.executorType == branchDeleteExecutorType {
		branch = validDeletableBranch(request.Branch)
	}
	base := validMergeBase(request.Base)
	if branch == base {
		branch = ""
	}
	baseRequired := request.profile.executorType == branchMergeExecutorType
	if !nodePattern.MatchString(request.Owner) || !repositoryPattern.MatchString(request.Repository) ||
		!requestIDPattern.MatchString(request.RequestID) || branch == "" || base == "" ||
		!gitObjectIDPattern.MatchString(request.ExpectedHeadOid) ||
		(baseRequired && !gitObjectIDPattern.MatchString(request.ExpectedBaseOid)) ||
		(!baseRequired && request.ExpectedBaseOid != "") ||
		(request.ExpectedBaseOid != "" && len(request.ExpectedBaseOid) != len(request.ExpectedHeadOid)) {
		return errors.New("invalid executor request")
	}
	request.Branch, request.Base = branch, base
	canonicalValues := map[string]any{
		"owner": request.Owner, "repository": request.Repository,
		"requestId": request.RequestID, "type": request.Type,
		"branch": branch, "base": base,
		"expectedBaseOid": request.ExpectedBaseOid,
		"expectedHeadOid": request.ExpectedHeadOid,
	}
	if request.DeleteBranch {
		canonicalValues["deleteBranch"] = true
	}
	if request.Discard {
		canonicalValues["discard"] = true
	}
	canonical, _ := json.Marshal(canonicalValues)
	digest := sha256.Sum256(canonical)
	request.digest = hex.EncodeToString(digest[:])
	return nil
}

func (e *PullMutationExecutor) agentBranchTrashRef(request pullMutationRequest) string {
	return branchTrashPrefixFor(request.Branch) + request.Branch + "/" + request.token()[:16]
}

func (e *PullMutationExecutor) branchDeletes(request pullMutationRequest) bool {
	return request.profile.executorType == branchDeleteExecutorType || request.DeleteBranch
}

func (e *PullMutationExecutor) executeBranch(ctx context.Context, dir, gitDir string, request pullMutationRequest) (*pullJobRecord, error) {
	head, err := e.refValue(ctx, gitDir, "refs/heads/"+request.Branch)
	if err != nil {
		return nil, err
	}
	if head == "" {
		return e.storeRecord(dir, request, e.failure(request, "branch_not_found"))
	}
	if head != request.ExpectedHeadOid {
		return e.storeRecord(dir, request, e.failure(request, "stale_head"))
	}
	if e.branchDeletes(request) && request.Branch == defaultBranchName(ctx, gitDir) {
		return e.storeRecord(dir, request, e.failure(request, "unsupported_branch"))
	}
	base, err := e.refValue(ctx, gitDir, "refs/heads/"+request.Base)
	if err != nil {
		return nil, err
	}
	if base == "" || (request.ExpectedBaseOid != "" && base != request.ExpectedBaseOid) {
		return e.storeRecord(dir, request, e.failure(request, "stale_base"))
	}
	merged := e.isAncestor(ctx, gitDir, head, base)
	completed := e.completedRefs(request)
	var transaction []string
	after := []string{head}
	if request.profile.executorType == branchDeleteExecutorType {
		if !merged && !request.Discard {
			return e.storeRecord(dir, request, e.failure(request, "not_merged"))
		}
		transaction = append(transaction, "verify refs/heads/"+request.Base+" "+base)
	} else {
		baseAfter := base
		if merged {
			if !request.DeleteBranch {
				return e.storeRecord(dir, request, e.failure(request, "already_merged"))
			}
			transaction = append(transaction, "verify refs/heads/"+request.Base+" "+base)
		} else {
			output, code, err := e.git(ctx, gitDir, "merge-tree", "--write-tree", "--no-messages", base, head)
			if err != nil {
				return nil, err
			}
			if code == 1 {
				return e.storeRecord(dir, request, e.failure(request, "merge_conflict"))
			}
			tree := strings.TrimSpace(strings.SplitN(output, "\n", 2)[0])
			if code != 0 || !gitObjectIDPattern.MatchString(tree) {
				return nil, errors.New("merge tree is invalid")
			}
			baseAfter, err = e.commitTree(ctx, gitDir, tree, []string{base, head},
				fmt.Sprintf("Merge branch '%s' into %s", request.Branch, request.Base))
			if err != nil {
				return nil, err
			}
			transaction = append(transaction, "update refs/heads/"+request.Base+" "+baseAfter+" "+base)
		}
		after = []string{baseAfter}
	}
	if e.branchDeletes(request) {
		transaction = append(transaction,
			"create "+e.agentBranchTrashRef(request)+" "+head,
			"delete refs/heads/"+request.Branch+" "+head)
	} else {
		transaction = append(transaction, "verify refs/heads/"+request.Branch+" "+head)
	}
	transaction = append(transaction, "create "+completed[0]+" "+after[0])
	return e.commitTransaction(ctx, dir, gitDir, request, transaction, after)
}

func defaultBranchName(ctx context.Context, gitDir string) string {
	return gitValue(ctx, gitDir, "symbolic-ref", "--quiet", "--short", "HEAD")
}

func agentBranchTombstoned(ctx context.Context, gitDir, branch, oid string) bool {
	if validManagedBranch(branch) == "" || !gitObjectIDPattern.MatchString(oid) {
		return false
	}
	prefix := branchTrashPrefixFor(branch) + branch + "/"
	values, err := gitRefValues(ctx, gitDir, prefix)
	if err != nil {
		return false
	}
	for ref, tomb := range values {
		if strings.Contains(strings.TrimPrefix(ref, prefix), "/") {
			continue
		}
		if tomb == oid || gitIsAncestor(ctx, gitDir, oid, tomb) {
			return true
		}
	}
	return false
}

func ensureGatewayAgentBranchOperations(gateway *GatewayConfig, enabled bool) {
	blocked := map[string]bool{}
	for _, op := range agentBranchGatewayOperations {
		blocked[op] = true
	}
	for i := range gateway.Repositories {
		repo := &gateway.Repositories[i]
		if !repo.Enabled {
			continue
		}
		var ops []string
		for _, op := range repo.Operations {
			if !blocked[op] {
				ops = append(ops, op)
			}
		}
		if enabled && gateway.MergeExecutorCommand != nil && hasOperation(ops, "merge-pull") {
			ops = append(ops, agentBranchGatewayOperations...)
		}
		repo.Operations = ops
	}
}

func hasOperation(ops []string, want string) bool {
	for _, op := range ops {
		if op == want {
			return true
		}
	}
	return false
}

func gatewayScriptSupports(path, op string) bool {
	if strings.TrimSpace(path) == "" {
		return false
	}
	raw, err := os.ReadFile(path)
	if err != nil {
		return false
	}
	return strings.Contains(string(raw), `"`+op+`"`)
}
