package pulls

import (
	"bufio"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"

	"forkmesh.com/desktop-core/internal/git"
	"forkmesh.com/desktop-core/internal/safedelete"
)

const (
	gitTimeout      = 15 * time.Second
	gitMergeTimeout = 60 * time.Second
	errorChars      = 300
)

type Store struct {
	WorkTree      string
	Mirror        string
	IdentityValid bool
	DataRoot      string
	Runner        *git.Runner
	Ctx           context.Context
	AmLive        func() bool
	Purge         func(number int) error
	Rewrite       func(number int, relPaths []string) error
	Warn          func(text string)
	Signer        *Signer
	Session       *Session
	Env           []string

	metaCache string
}

func (s *Store) ctx() context.Context {
	if s.Ctx != nil {
		return s.Ctx
	}
	return context.Background()
}

func (s *Store) runner() *git.Runner {
	if s.Runner != nil {
		return s.Runner
	}
	return git.Default
}

func (s *Store) warn(text string) {
	if s.Warn != nil {
		s.Warn(text)
	}
}

type gitResult struct {
	ok       bool
	output   []byte
	err      string
	timedOut bool
	exitCode int
}

func (r gitResult) out() string { return string(r.output) }

func (s *Store) runGitTimeout(dir string, args []string, timeout time.Duration) gitResult {
	result := s.runner().Run(s.ctx(), dir, args, git.Options{Timeout: timeout, Env: s.Env})
	return toGitResult(result)
}

func (s *Store) runGit(dir string, args ...string) gitResult {
	return s.runGitTimeout(dir, args, gitTimeout)
}

func (s *Store) runGitInput(dir string, input []byte, args ...string) gitResult {
	result := s.runner().Run(s.ctx(), dir, args, git.Options{Timeout: gitTimeout, Input: input, NoLane: true, Env: s.Env})
	return toGitResult(result)
}

func (s *Store) runGitRaw(dir string, args ...string) gitResult {
	result := s.runner().Run(s.ctx(), dir, args, git.Options{Timeout: gitTimeout, NoLane: true, Env: s.Env})
	return toGitResult(result)
}

func toGitResult(result git.Result) gitResult {
	out := gitResult{ok: result.OK, timedOut: result.TimedOut, exitCode: result.ExitCode}
	if !result.TimedOut {
		out.output = result.Output
	}
	if !result.OK {
		if result.TimedOut {
			out.err = "git timed out"
		} else {
			out.err = leftRunes(strings.TrimSpace(result.Error), errorChars)
		}
	}
	return out
}

func trimmed(b []byte) string { return strings.TrimSpace(string(b)) }

func exists(path string) bool {
	_, err := os.Stat(path)
	return err == nil
}

func (s *Store) CanWrite() bool {
	if s.WorkTree == "" || !s.IdentityValid {
		return false
	}
	return exists(s.WorkTree + "/.git")
}

func (s *Store) pullsDir() string {
	meta := s.MetaWorkTree()
	if meta == "" {
		meta = s.WorkTree
	}
	return meta + "/.forkmesh/pulls"
}

func (s *Store) pullDir(number int) string {
	return s.pullsDir() + "/" + strconv.Itoa(number)
}

func pullMetadataBranchRef(number int) string {
	return "refs/heads/forkmesh/pull/" + strconv.Itoa(number)
}

func firstLine(path string) (string, bool) {
	file, err := os.Open(path)
	if err != nil {
		return "", false
	}
	defer file.Close()
	reader := bufio.NewReaderSize(file, 4096)
	line, _ := reader.ReadString('\n')
	if len(line) > 4095 {
		line = line[:4095]
	}
	return strings.TrimSpace(line), true
}

func headBranchFromFile(workTree string) string {
	if strings.TrimSpace(workTree) == "" {
		return ""
	}
	var gitDir string
	dotGit := filepath.Join(workTree, ".git")
	info, err := os.Stat(dotGit)
	switch {
	case err == nil && info.IsDir():
		gitDir = dotGit
	case err == nil:
		line, ok := firstLine(dotGit)
		if !ok || !strings.HasPrefix(line, "gitdir:") {
			return ""
		}
		target := strings.TrimSpace(line[7:])
		if target == "" {
			return ""
		}
		if filepath.IsAbs(target) {
			gitDir = target
		} else {
			gitDir = filepath.Join(workTree, target)
		}
	case exists(filepath.Join(workTree, "HEAD")):
		gitDir = workTree
	default:
		return ""
	}
	line, ok := firstLine(filepath.Join(gitDir, "HEAD"))
	if !ok {
		return ""
	}
	const prefix = "ref: refs/heads/"
	if strings.HasPrefix(line, prefix) {
		return line[len(prefix):]
	}
	return ""
}

var metaMu sync.Mutex

func absolutePath(path string) string {
	abs, err := filepath.Abs(path)
	if err != nil {
		return filepath.Clean(path)
	}
	return abs
}

func MetaWorkTreePath(dataRoot, workTree string) string {
	sum := sha256.Sum256([]byte(absolutePath(workTree)))
	return dataRoot + "/pull-meta/" + hex.EncodeToString(sum[:])[:16]
}

func (s *Store) MetaWorkTree() string {
	if s.WorkTree == "" {
		return ""
	}
	if s.metaCache != "" {
		return s.metaCache
	}
	if headBranchFromFile(s.WorkTree) == "forkmesh/pulls" {
		s.metaCache = s.WorkTree
		return s.metaCache
	}
	if s.DataRoot == "" {
		return ""
	}
	metaMu.Lock()
	defer metaMu.Unlock()
	dir := MetaWorkTreePath(s.DataRoot, s.WorkTree)
	if exists(dir+"/.git") && headBranchFromFile(dir) == "forkmesh/pulls" {
		if claimPullMetadataHeal(dir) {
			s.resyncPullMetadataCheckout(dir)
		}
		_ = os.MkdirAll(dir+"/.forkmesh/pulls", 0o755)
		s.metaCache = dir
		return dir
	}
	_ = os.MkdirAll(filepath.Dir(dir), 0o755)
	branchExists := s.runGit(s.WorkTree, "rev-parse", "--verify", "-q", "refs/heads/forkmesh/pulls^{commit}").ok
	link := func() bool {
		if branchExists {
			return s.runGit(s.WorkTree, "worktree", "add", dir, "forkmesh/pulls").ok
		}
		return s.runGit(s.WorkTree, "worktree", "add", "-b", "forkmesh/pulls", dir, "HEAD").ok
	}
	if !link() {
		if exists(dir) {
			_ = safedelete.RemoveOwnWorktreeUnder(s.DataRoot+"/pull-meta", dir)
		}
		s.runGit(s.WorkTree, "worktree", "prune")
		if !link() {
			return ""
		}
	}
	_ = os.MkdirAll(dir+"/.forkmesh/pulls", 0o755)
	claimPullMetadataHeal(dir)
	s.metaCache = dir
	return dir
}

func listDir(dir string, wantDirs bool) []string {
	entries, err := os.ReadDir(dir)
	if err != nil {
		return nil
	}
	names := []string{}
	for _, entry := range entries {
		name := entry.Name()
		if strings.HasPrefix(name, ".") {
			continue
		}
		info, err := os.Stat(filepath.Join(dir, name))
		if err != nil {
			continue
		}
		if info.IsDir() == wantDirs {
			names = append(names, name)
		}
	}
	sort.Strings(names)
	return names
}

func (s *Store) readEvents(number int) []Event {
	events := []Event{}
	dir := s.pullDir(number)
	for _, name := range listDir(dir, false) {
		if !eventFilePattern.MatchString(name) {
			continue
		}
		raw, err := os.ReadFile(dir + "/" + name)
		if err != nil {
			continue
		}
		events = append(events, eventFromFrontMatter(parseFrontMatter(raw)))
	}
	return events
}

func (s *Store) deriveFromRefs(dir, base, head string, wantPatch bool) (string, string, bool) {
	if dir == "" || base == "" || head == "" || strings.Contains(head, ":") {
		return "", "", false
	}
	if !s.runGit(dir, "rev-parse", "--verify", "--quiet", base+"^{commit}").ok ||
		!s.runGit(dir, "rev-parse", "--verify", "--quiet", head+"^{commit}").ok {
		return "", "", false
	}
	if !wantPatch {
		return "", "", true
	}
	patch := qtFromUtf8(s.runGit(dir, "diff", "--binary", base+"..."+head).output)
	commits := qtFromUtf8(s.runGit(dir, "format-patch", "--binary", "--stdout", base+".."+head).output)
	return patch, commits, true
}

func (s *Store) resolvedCommitOid(dir, ref string) string {
	if dir == "" || ref == "" {
		return ""
	}
	result := s.runGit(dir, "rev-parse", "--verify", "--quiet", "--end-of-options", ref+"^{commit}")
	if !result.ok {
		return ""
	}
	oid := strings.ToLower(trimmed(result.output))
	if !validCommitOid(oid) {
		return ""
	}
	return oid
}

func (s *Store) ReadPull(number int, derive bool) (*Pull, bool) {
	raw, err := os.ReadFile(s.pullDir(number) + "/pull.md")
	if err != nil {
		return nil, false
	}
	fm := parseFrontMatter(raw)
	out := &Pull{
		Number:                 number,
		Title:                  fm.get("title"),
		Base:                   fm.get("base"),
		Head:                   fm.get("head"),
		Status:                 "open",
		Ts:                     fm.num("ts"),
		Author:                 fm.get("author"),
		AuthorName:             fm.get("authorName"),
		Sig:                    fm.get("sig"),
		BranchBacked:           fm.get("derive") == "branch",
		CreationBaseOid:        fm.get("creationBaseOid"),
		CreationHeadOid:        fm.get("creationHeadOid"),
		MergeBase:              fm.get("mergeBase"),
		MergeHead:              fm.get("mergeHead"),
		Client:                 fm.get("client"),
		ClientSig:              fm.get("clientSig"),
		RewardEstimateLamports: fm.num("rewardEstimateLamports"),
		RewardRecipient:        fm.get("rewardRecipient"),
		RewardID:               fm.get("rewardId"),
		RewardCondition:        fm.get("rewardCondition"),
		RewardStatus:           fm.get("rewardStatus"),
		RewardTxSignature:      fm.get("rewardTxSignature"),
		Description:            fm.body,
	}
	if fm.has("status") {
		out.Status = fm.get("status")
	}
	merged := out.MergeBase != "" && out.MergeHead != ""
	rbase, rhead := out.Base, out.Head
	if merged {
		rbase, rhead = out.MergeBase, out.MergeHead
	}
	derived := false
	if derive && out.Status == "open" && out.BranchBacked {
		if patch, commits, ok := s.deriveFromRefs(s.WorkTree, rbase, rhead, true); ok {
			derived = true
			out.Patch, out.Commits = patch, commits
			out.PatchHeadOid = s.resolvedCommitOid(s.WorkTree, rhead)
		} else if patch, commits, ok := s.deriveFromRefs(s.Mirror, rbase, rhead, true); ok {
			derived = true
			out.Patch, out.Commits = patch, commits
			out.PatchHeadOid = s.resolvedCommitOid(s.Mirror, rhead)
		}
	}
	if out.PatchHeadOid == "" {
		out.PatchHeadOid = out.CreationHeadOid
	}
	if derive && out.Status == "open" && !derived {
		if patch, err := os.ReadFile(s.pullDir(number) + "/changes.patch"); err == nil {
			out.Patch = qtFromUtf8(patch)
		}
		if mbox, err := os.ReadFile(s.pullDir(number) + "/commits.mbox"); err == nil {
			out.Commits = qtFromUtf8(mbox)
		}
	}
	ComputeStats(out)
	out.Events = s.readEvents(number)
	return out, true
}

func writeTextFile(path, text string) error {
	if err := os.WriteFile(path, []byte(text), 0o666); err != nil {
		return errorString("Could not write " + path)
	}
	return nil
}

type errorString string

func (e errorString) Error() string { return string(e) }

func Errorf(text string) error { return errorString(text) }

func (s *Store) writePull(pr *Pull) error {
	dir := s.pullDir(pr.Number)
	_ = os.MkdirAll(dir, 0o755)
	lines := []string{
		"---",
		"schema: forkmesh-pull-v1",
		"number: " + strconv.Itoa(pr.Number),
		"title: " + pr.Title,
		"base: " + pr.Base,
		"head: " + pr.Head,
		"status: " + pr.Status,
	}
	if pr.BranchBacked {
		lines = append(lines, "derive: branch")
	}
	if pr.CreationBaseOid != "" {
		lines = append(lines, "creationBaseOid: "+pr.CreationBaseOid)
	}
	if pr.CreationHeadOid != "" {
		lines = append(lines, "creationHeadOid: "+pr.CreationHeadOid)
	}
	if pr.MergeBase != "" {
		lines = append(lines, "mergeBase: "+pr.MergeBase)
	}
	if pr.MergeHead != "" {
		lines = append(lines, "mergeHead: "+pr.MergeHead)
	}
	lines = append(lines,
		"ts: "+strconv.FormatInt(pr.Ts, 10),
		"author: "+pr.Author,
		"authorName: "+pr.AuthorName,
		"sig: "+pr.Sig)
	if pr.Client != "" {
		lines = append(lines, "client: "+pr.Client)
	}
	if pr.ClientSig != "" {
		lines = append(lines, "clientSig: "+pr.ClientSig)
	}
	if pr.RewardEstimateLamports > 0 {
		lines = append(lines, "rewardEstimateLamports: "+strconv.FormatInt(pr.RewardEstimateLamports, 10))
	}
	if pr.RewardRecipient != "" {
		lines = append(lines, "rewardRecipient: "+pr.RewardRecipient)
	}
	if pr.RewardID != "" {
		lines = append(lines, "rewardId: "+pr.RewardID)
	}
	if pr.RewardCondition != "" {
		lines = append(lines, "rewardCondition: "+pr.RewardCondition)
	}
	if pr.RewardStatus != "" {
		lines = append(lines, "rewardStatus: "+pr.RewardStatus)
	}
	if pr.RewardTxSignature != "" {
		lines = append(lines, "rewardTxSignature: "+pr.RewardTxSignature)
	}
	lines = append(lines, "---", "")
	if err := writeTextFile(dir+"/pull.md", strings.Join(lines, "\n")+"\n"+pr.Description+"\n"); err != nil {
		return err
	}
	if pr.Status != "open" || pr.BranchBacked {
		_ = os.Remove(dir + "/changes.patch")
		_ = os.Remove(dir + "/commits.mbox")
		return nil
	}
	if err := writeTextFile(dir+"/changes.patch", pr.Patch); err != nil {
		return err
	}
	if pr.Commits != "" {
		return writeTextFile(dir+"/commits.mbox", pr.Commits)
	}
	return nil
}

var commitPullNumber = regexp.MustCompile(`^pull #(\d+)(?:\D|$)`)

func (s *Store) commit(message string) error {
	dir := s.MetaWorkTree()
	if dir == "" {
		dir = s.WorkTree
	}
	if r := s.runGit(dir, "add", ".forkmesh/pulls"); !r.ok {
		return errorString("git add failed: " + r.err)
	}
	if r := s.runGit(dir, "commit", "-m", message, "--", ".forkmesh/pulls"); !r.ok {
		if !(strings.Contains(r.err, "nothing to commit") || r.err == "") {
			return errorString("git commit failed: " + r.err)
		}
	}
	if m := commitPullNumber.FindStringSubmatch(message); m != nil {
		number, _ := qtToInt(m[1])
		if err := s.materializePullMetadataRef(number); err != nil {
			return err
		}
	}
	return nil
}

func (s *Store) materializePullMetadataRef(number int) error {
	if number <= 0 {
		return nil
	}
	dir := s.MetaWorkTree()
	if dir == "" {
		dir = s.WorkTree
	}
	if dir == "" {
		return nil
	}
	pullTreePath := "HEAD:.forkmesh/pulls/" + strconv.Itoa(number)
	if !s.runGit(dir, "cat-file", "-e", pullTreePath).ok {
		return nil
	}
	tree := s.runGit(dir, "rev-parse", "--verify", pullTreePath)
	if !tree.ok || trimmed(tree.output) == "" {
		return errorString("Could not resolve pull metadata: " + tree.err)
	}
	pullTree := trimmed(tree.output)
	branchRef := pullMetadataBranchRef(number)
	metadataRef := "refs/pr/" + strconv.Itoa(number) + "/metadata"
	oldTip := trimmed(s.runGit(dir, "rev-parse", "--verify", "-q", branchRef+"^{commit}").output)
	if oldTip != "" {
		old := s.runGit(dir, "rev-parse", "--verify", branchRef+":.forkmesh/pulls/"+strconv.Itoa(number))
		if old.ok && trimmed(old.output) == pullTree {
			r := s.runGit(dir, "update-ref", metadataRef, oldTip)
			if !r.ok {
				return errorString(r.err)
			}
			return nil
		}
	}
	mk := func(entry string) (string, error) {
		r := s.runGitInput(dir, []byte(entry), "mktree")
		if !r.ok || trimmed(r.output) == "" {
			return "", errorString("Could not build PR metadata tree: " + r.err)
		}
		return trimmed(r.output), nil
	}
	pullsTree, err := mk("040000 tree " + pullTree + "\t" + strconv.Itoa(number) + "\n")
	if err != nil {
		return err
	}
	forkmeshTree, err := mk("040000 tree " + pullsTree + "\tpulls\n")
	if err != nil {
		return err
	}
	rootTree, err := mk("040000 tree " + forkmeshTree + "\t.forkmesh\n")
	if err != nil {
		return err
	}
	commitArgs := []string{"commit-tree", rootTree}
	if oldTip != "" {
		commitArgs = append(commitArgs, "-p", oldTip)
	}
	commitArgs = append(commitArgs, "-m", "pull #"+strconv.Itoa(number)+": metadata snapshot")
	made := s.runGitTimeout(dir, commitArgs, gitTimeout)
	newTip := trimmed(made.output)
	if !made.ok || newTip == "" {
		return errorString("Could not commit PR metadata: " + made.err)
	}
	updateBranch := []string{"update-ref", branchRef, newTip}
	if oldTip != "" {
		updateBranch = append(updateBranch, oldTip)
	}
	if r := s.runGitTimeout(dir, updateBranch, gitTimeout); !r.ok {
		return errorString("Could not update PR metadata branch: " + r.err)
	}
	if r := s.runGit(dir, "update-ref", metadataRef, newTip); !r.ok {
		return errorString("Could not attach metadata to PR #" + strconv.Itoa(number) + ": " + r.err)
	}
	return nil
}

func (s *Store) gitOptionsRaw() git.Options {
	return git.Options{Timeout: gitTimeout, NoLane: true, Env: s.Env}
}

func sortStrings(values []string) { sort.Strings(values) }
