package mirrornode

// Per-pull metadata refs. forkmesh/pulls is retained as a compatibility index
// for older clients, but it is no longer the only durable owner of every pull
// record.

import (
	"bufio"
	"bytes"
	"context"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"regexp"
	"sort"
	"strconv"
	"strings"
)

func pullMetadataRef(number int) string {
	return "refs/heads/forkmesh/pull/" + strconv.Itoa(number)
}

func pullMetadataCompatibilityRef(number int) string {
	return "refs/pr/" + strconv.Itoa(number) + "/metadata"
}

func gitInputValue(ctx context.Context, gitDir string, input []byte, env []string, args ...string) (string, error) {
	command := exec.CommandContext(ctx, "git", append([]string{"--git-dir=" + gitDir}, args...)...)
	command.Stdin = bytes.NewReader(input)
	if len(env) != 0 {
		command.Env = append(os.Environ(), env...)
	}
	output, err := command.CombinedOutput()
	if err != nil {
		return "", fmt.Errorf("git %s: %s", args[0], boundedText(output, 240))
	}
	return strings.TrimSpace(string(output)), nil
}

// materializePullMetadataCommit creates an independent commit whose tree
// contains only .forkmesh/pulls/<number>. The returned old tip is the CAS value
// callers use when publishing the commit.
func materializePullMetadataCommit(ctx context.Context, gitDir string, number int,
	aggregateTip, identityName, identityEmail string) (newTip, oldTip string, err error) {
	if number <= 0 || !gitObjectIDPattern.MatchString(aggregateTip) {
		return "", "", fmt.Errorf("invalid pull metadata source")
	}
	if strings.TrimSpace(identityName) == "" {
		identityName = "ForkMesh mirror"
	}
	if strings.TrimSpace(identityEmail) == "" {
		identityEmail = "mirror@users.noreply.forkmesh.com"
	}
	pullPath := fmt.Sprintf(".forkmesh/pulls/%d", number)
	pullTree := gitValue(ctx, gitDir, "rev-parse", aggregateTip+":"+pullPath)
	if !gitObjectIDPattern.MatchString(pullTree) {
		return "", "", fmt.Errorf("pull #%d metadata tree is missing", number)
	}
	branchRef := pullMetadataRef(number)
	oldTip = gitValue(ctx, gitDir, "rev-parse", "--verify", "-q", branchRef+"^{commit}")
	parent := oldTip
	if parent == "" {
		parent = gitValue(ctx, gitDir, "rev-parse", "--verify", "-q",
			pullMetadataCompatibilityRef(number)+"^{commit}")
	}
	if parent != "" {
		oldTree := gitValue(ctx, gitDir, "rev-parse", parent+":"+pullPath)
		if oldTree == pullTree {
			return parent, oldTip, nil
		}
	}
	pullsTree, err := gitInputValue(ctx, gitDir,
		[]byte(fmt.Sprintf("040000 tree %s\t%d\n", pullTree, number)), nil, "mktree")
	if err != nil {
		return "", oldTip, fmt.Errorf("build pulls tree: %w", err)
	}
	if !gitObjectIDPattern.MatchString(pullsTree) {
		return "", oldTip, fmt.Errorf("build pulls tree: invalid object ID")
	}
	forkmeshTree, err := gitInputValue(ctx, gitDir,
		[]byte(fmt.Sprintf("040000 tree %s\tpulls\n", pullsTree)), nil, "mktree")
	if err != nil {
		return "", oldTip, fmt.Errorf("build forkmesh tree: %w", err)
	}
	if !gitObjectIDPattern.MatchString(forkmeshTree) {
		return "", oldTip, fmt.Errorf("build forkmesh tree: invalid object ID")
	}
	rootTree, err := gitInputValue(ctx, gitDir,
		[]byte(fmt.Sprintf("040000 tree %s\t.forkmesh\n", forkmeshTree)), nil, "mktree")
	if err != nil {
		return "", oldTip, fmt.Errorf("build metadata root tree: %w", err)
	}
	if !gitObjectIDPattern.MatchString(rootTree) {
		return "", oldTip, fmt.Errorf("build metadata root tree: invalid object ID")
	}
	args := []string{"commit-tree", rootTree}
	if parent != "" {
		args = append(args, "-p", parent)
	}
	args = append(args, "-m", fmt.Sprintf("pull #%d: metadata snapshot", number))
	env := []string{
		"GIT_AUTHOR_NAME=" + identityName,
		"GIT_AUTHOR_EMAIL=" + identityEmail,
		"GIT_COMMITTER_NAME=" + identityName,
		"GIT_COMMITTER_EMAIL=" + identityEmail,
	}
	newTip, err = gitInputValue(ctx, gitDir, nil, env, args...)
	if err != nil {
		return "", oldTip, fmt.Errorf("commit pull metadata: %w", err)
	}
	if !gitObjectIDPattern.MatchString(newTip) {
		return "", oldTip, fmt.Errorf("commit pull metadata: invalid object ID")
	}
	return newTip, oldTip, nil
}

func publishPullMetadataRefs(ctx context.Context, gitDir string, number int,
	newTip, oldTip string) error {
	if !gitObjectIDPattern.MatchString(newTip) {
		return fmt.Errorf("invalid pull metadata commit")
	}
	if pullRecordMerged(pullRecordAt(ctx, gitDir, newTip, number)) {
		return retirePullMetadataBranch(ctx, gitDir, number, newTip, oldTip)
	}
	branchRef := pullMetadataRef(number)
	args := []string{"--git-dir=" + gitDir, "update-ref", branchRef, newTip}
	if oldTip != "" {
		args = append(args, oldTip)
	}
	if output, err := exec.CommandContext(ctx, "git", args...).CombinedOutput(); err != nil {
		return fmt.Errorf("publish pull metadata branch: %s", boundedText(output, 240))
	}
	if output, err := exec.CommandContext(ctx, "git", "--git-dir="+gitDir,
		"update-ref", pullMetadataCompatibilityRef(number), newTip).CombinedOutput(); err != nil {
		return fmt.Errorf("publish pull metadata compatibility ref: %s", boundedText(output, 240))
	}
	return nil
}

const pullMetadataBranchPrefix = "refs/heads/forkmesh/pull/"

var pullMetadataBranchPattern = regexp.MustCompile(`^refs/heads/forkmesh/pull/([1-9][0-9]{0,8})$`)

func pullMetadataBranchNumber(ref string) int {
	match := pullMetadataBranchPattern.FindStringSubmatch(ref)
	if match == nil {
		return 0
	}
	number, err := strconv.Atoi(match[1])
	if err != nil {
		return 0
	}
	return number
}

func pullRecordPath(number int) string {
	return fmt.Sprintf(".forkmesh/pulls/%d/pull.md", number)
}

func pullRecordAt(ctx context.Context, gitDir, rev string, number int) *frontMatter {
	if rev == "" || number <= 0 {
		return nil
	}
	raw, err := exec.CommandContext(ctx, "git", "--git-dir="+gitDir, "cat-file", "blob",
		rev+":"+pullRecordPath(number)).Output()
	if err != nil {
		return nil
	}
	matter, ok := parseFrontMatter(raw)
	if !ok {
		return nil
	}
	return matter
}

func pullRecordMerged(record *frontMatter) bool {
	if record == nil {
		return false
	}
	status := strings.ToLower(strings.TrimSpace(record.values["status"]))
	return status == "merged" || status == "closed-merged"
}

func pullBranchRetirable(branch, ledger *frontMatter) bool {
	if branch == nil || !pullRecordMerged(ledger) {
		return false
	}
	if pullRecordMerged(branch) {
		return true
	}
	ts := strings.TrimSpace(branch.values["ts"])
	author := strings.TrimSpace(branch.values["author"])
	return ts != "" && author != "" &&
		ts == strings.TrimSpace(ledger.values["ts"]) &&
		author == strings.TrimSpace(ledger.values["author"])
}

func gitBatchBlobs(ctx context.Context, gitDir string, names []string) ([][]byte, error) {
	blobs := make([][]byte, len(names))
	if len(names) == 0 {
		return blobs, nil
	}
	var input bytes.Buffer
	for _, name := range names {
		if strings.ContainsAny(name, "\n\r") {
			return nil, errors.New("invalid object name")
		}
		input.WriteString(name + "\n")
	}
	command := exec.CommandContext(ctx, "git", "--git-dir="+gitDir, "cat-file", "--batch")
	command.Stdin = &input
	output, err := command.Output()
	if err != nil {
		return nil, fmt.Errorf("git cat-file: %w", err)
	}
	reader := bufio.NewReader(bytes.NewReader(output))
	for i := range names {
		header, err := reader.ReadString('\n')
		if err != nil {
			return nil, errors.New("git cat-file: truncated batch output")
		}
		fields := strings.Fields(header)
		if len(fields) != 3 {
			continue
		}
		size, err := strconv.Atoi(fields[2])
		if err != nil || size < 0 {
			return nil, errors.New("git cat-file: invalid batch header")
		}
		body := make([]byte, size+1)
		if _, err := io.ReadFull(reader, body); err != nil {
			return nil, errors.New("git cat-file: truncated batch body")
		}
		if fields[1] == "blob" {
			blobs[i] = body[:size]
		}
	}
	return blobs, nil
}

func parsedPullRecord(raw []byte) *frontMatter {
	if raw == nil {
		return nil
	}
	matter, ok := parseFrontMatter(raw)
	if !ok {
		return nil
	}
	return matter
}

func retirePullMetadataBranch(ctx context.Context, gitDir string, number int, keep, branchTip string) error {
	if number <= 0 || !gitObjectIDPattern.MatchString(keep) ||
		(branchTip != "" && !gitObjectIDPattern.MatchString(branchTip)) {
		return fmt.Errorf("invalid pull metadata retirement")
	}
	compatibilityRef := pullMetadataCompatibilityRef(number)
	current := gitRefValue(ctx, gitDir, compatibilityRef)
	if current != "" && current != keep && gitIsAncestor(ctx, gitDir, keep, current) {
		keep = current
	}
	if branchTip != "" && branchTip != keep && !gitIsAncestor(ctx, gitDir, branchTip, keep) {
		return fmt.Errorf("pull #%d metadata ref does not contain its branch tip", number)
	}
	lines := []string{"start"}
	switch current {
	case keep:
		lines = append(lines, "verify "+compatibilityRef+" "+keep)
	case "":
		lines = append(lines, "create "+compatibilityRef+" "+keep)
	default:
		lines = append(lines, "update "+compatibilityRef+" "+keep+" "+current)
	}
	if branchTip != "" {
		lines = append(lines, "delete "+pullMetadataRef(number)+" "+branchTip)
	}
	lines = append(lines, "prepare", "commit", "")
	if _, err := gitInputValue(ctx, gitDir, []byte(strings.Join(lines, "\n")), nil,
		"update-ref", "--stdin"); err != nil {
		return fmt.Errorf("retire pull #%d metadata branch: %w", number, err)
	}
	return nil
}

func retireMergedPullBranches(ctx context.Context, gitDir, identityName string) (int, error) {
	branches, err := gitRefValues(ctx, gitDir, pullMetadataBranchPrefix)
	if err != nil {
		return 0, fmt.Errorf("list pull metadata branches: %w", err)
	}
	ledgerTip := gitValue(ctx, gitDir, "rev-parse", "--verify", "-q", "refs/heads/"+pullMetadataBranch+"^{commit}")
	if len(branches) == 0 || !gitObjectIDPattern.MatchString(ledgerTip) {
		return 0, nil
	}
	type candidate struct {
		number int
		tip    string
	}
	var candidates []candidate
	var names []string
	for ref, tip := range branches {
		number := pullMetadataBranchNumber(ref)
		if number == 0 || !gitObjectIDPattern.MatchString(tip) {
			continue
		}
		candidates = append(candidates, candidate{number: number, tip: tip})
	}
	sort.Slice(candidates, func(i, j int) bool { return candidates[i].number < candidates[j].number })
	for _, item := range candidates {
		names = append(names, item.tip+":"+pullRecordPath(item.number),
			ledgerTip+":"+pullRecordPath(item.number))
	}
	records, err := gitBatchBlobs(ctx, gitDir, names)
	if err != nil {
		return 0, err
	}
	if strings.TrimSpace(identityName) == "" {
		identityName = "ForkMesh mirror"
	}
	retired := 0
	var failures []string
	for i, item := range candidates {
		if ctx.Err() != nil {
			break
		}
		branchRecord := parsedPullRecord(records[2*i])
		if !pullBranchRetirable(branchRecord, parsedPullRecord(records[2*i+1])) {
			continue
		}
		keep := item.tip
		if !pullRecordMerged(branchRecord) {
			snapshot, branchTip, err := materializePullMetadataCommit(ctx, gitDir, item.number,
				ledgerTip, identityName, identityName+"@users.noreply.forkmesh.com")
			if err != nil {
				failures = append(failures, fmt.Sprintf("#%d: %v", item.number, err))
				continue
			}
			if branchTip != item.tip {
				continue
			}
			keep = snapshot
		}
		if err := retirePullMetadataBranch(ctx, gitDir, item.number, keep, item.tip); err != nil {
			failures = append(failures, err.Error())
			continue
		}
		retired++
	}
	if len(failures) > 0 {
		return retired, errors.New(boundedText([]byte(strings.Join(failures, "; ")), 600))
	}
	return retired, nil
}

func retiredPullBranchArrivals(ctx context.Context, gitDir string, arrivals map[string]string) map[string]bool {
	skip := map[string]bool{}
	if len(arrivals) == 0 {
		return skip
	}
	ledgers := []string{}
	for _, ref := range []string{"refs/heads/" + pullMetadataBranch,
		"refs/forkmesh/upstream/heads/" + pullMetadataBranch} {
		if tip := gitRefValue(ctx, gitDir, ref); gitObjectIDPattern.MatchString(tip) {
			ledgers = append(ledgers, tip)
		}
	}
	if len(ledgers) == 0 {
		return skip
	}
	refs := make([]string, 0, len(arrivals))
	for ref := range arrivals {
		if pullMetadataBranchNumber(ref) != 0 && gitObjectIDPattern.MatchString(arrivals[ref]) {
			refs = append(refs, ref)
		}
	}
	sort.Strings(refs)
	stride := 1 + len(ledgers)
	names := make([]string, 0, len(refs)*stride)
	for _, ref := range refs {
		path := pullRecordPath(pullMetadataBranchNumber(ref))
		names = append(names, arrivals[ref]+":"+path)
		for _, ledger := range ledgers {
			names = append(names, ledger+":"+path)
		}
	}
	records, err := gitBatchBlobs(ctx, gitDir, names)
	if err != nil {
		return skip
	}
	compatibility, _ := gitRefValues(ctx, gitDir, "refs/pr/")
	for i, ref := range refs {
		branchRecord := parsedPullRecord(records[i*stride])
		retirable := false
		for j := 1; j < stride && !retirable; j++ {
			retirable = pullBranchRetirable(branchRecord, parsedPullRecord(records[i*stride+j]))
		}
		if !retirable {
			continue
		}
		skip[ref] = true
		if !pullRecordMerged(branchRecord) {
			continue
		}
		number := pullMetadataBranchNumber(ref)
		incoming := arrivals[ref]
		current := compatibility[pullMetadataCompatibilityRef(number)]
		if current == incoming || (current != "" && !gitIsAncestor(ctx, gitDir, current, incoming)) {
			continue
		}
		_ = retirePullMetadataBranch(ctx, gitDir, number, incoming, "")
	}
	return skip
}
