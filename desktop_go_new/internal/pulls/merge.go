package pulls

import (
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"time"

	"forkmesh.com/desktop-core/internal/safedelete"
)

func (s *Store) IsBranchBehindBase(number int) (behind bool, count int, err error) {
	if !s.CanWrite() {
		return false, 0, errorString("This repository is read-only on this node.")
	}
	raw, readErr := os.ReadFile(s.pullDir(number) + "/pull.md")
	if readErr != nil {
		return false, 0, errorString("Pull request #" + strconv.Itoa(number) + " not found.")
	}
	fm := parseFrontMatter(raw)
	base, head := fm.get("base"), fm.get("head")
	if base == "" || head == "" {
		return false, 0, errorString("This pull request does not name a base and head branch.")
	}
	r := s.runGit(s.WorkTree, "rev-list", "--count", head+".."+base)
	if !r.ok {
		return false, 0, errorString("Could not compare branches: " + r.err)
	}
	count, _ = qtToInt(trimmed(r.output))
	return count > 0, count, nil
}

func (s *Store) releaseWorktreeHoldingBranch(dir, branch string) bool {
	if strings.TrimSpace(branch) == "" {
		return false
	}
	r := s.runGit(dir, "worktree", "list", "--porcelain")
	if !r.ok {
		return false
	}
	want := "refs/heads/" + branch
	current, held := "", ""
	for _, raw := range strings.Split(r.out(), "\n") {
		line := strings.TrimSpace(raw)
		if strings.HasPrefix(line, "worktree ") {
			current = strings.TrimSpace(line[len("worktree "):])
		} else if strings.HasPrefix(line, "branch ") && strings.TrimSpace(line[len("branch "):]) == want &&
			current != "" && absolutePath(current) != absolutePath(dir) {
			held = current
			break
		}
	}
	if held == "" {
		return false
	}
	if !s.runGit(dir, "worktree", "remove", held).ok {
		return false
	}
	s.runGit(dir, "worktree", "prune")
	return true
}

func (s *Store) UpdateBranchFromBase(number int) error {
	if !s.CanWrite() {
		return errorString("Updating needs a local working tree.")
	}
	pr, ok := s.ReadPull(number, true)
	if !ok {
		return errorString("Pull request #" + strconv.Itoa(number) + " not found.")
	}
	if pr.Status != "open" {
		return errorString(DecidedPullMessage(pr.Status))
	}
	behind, _, err := s.IsBranchBehindBase(number)
	if err != nil {
		return err
	}
	if !behind {
		return nil
	}
	status := s.runGit(s.WorkTree, "status", "--porcelain")
	if !status.ok || trimmed(status.output) != "" {
		return errorString("Commit or stash local changes before updating the branch.")
	}
	originalBranch := trimmed(s.runGit(s.WorkTree, "rev-parse", "--abbrev-ref", "HEAD").output)
	originalCommit := trimmed(s.runGit(s.WorkTree, "rev-parse", "--verify", "HEAD").output)
	restoreRef := originalBranch
	if originalBranch == "" || originalBranch == "HEAD" {
		restoreRef = originalCommit
	}
	if r := s.runGit(s.WorkTree, "checkout", pr.Head); !r.ok {
		errText := r.err
		recovered := false
		if s.releaseWorktreeHoldingBranch(s.WorkTree, pr.Head) {
			again := s.runGit(s.WorkTree, "checkout", pr.Head)
			recovered = again.ok
			if !again.ok {
				errText = again.err
			}
		}
		if !recovered {
			return errorString("Could not check out " + pr.Head + ": " + errText)
		}
	}
	merge := s.runGitTimeout(s.WorkTree, []string{"merge", "--no-edit", pr.Base}, gitMergeTimeout)
	if !merge.ok {
		s.runGit(s.WorkTree, "merge", "--abort")
		if restoreRef != "" && restoreRef != pr.Head {
			s.runGit(s.WorkTree, "checkout", restoreRef)
		}
		detail := simplified(merge.err)
		if detail == "" {
			detail = leftRunes(simplified(qtFromUtf8(merge.output)), 300)
		}
		if detail == "" {
			detail = "git merge failed without saying why"
		}
		return errorString("Could not merge " + pr.Base + " into " + pr.Head + ": " + detail)
	}
	if restoreRef != "" && restoreRef != pr.Head {
		if r := s.runGit(s.WorkTree, "checkout", restoreRef); !r.ok {
			return errorString("Updated " + pr.Head + ", but could not return to " + restoreRef + ": " + r.err)
		}
	}
	diff := s.runGit(s.WorkTree, "diff", "--binary", pr.Base+".."+pr.Head)
	if !diff.ok {
		return errorString("Could not refresh the pull request patch: " + diff.err)
	}
	pr.Patch = qtFromUtf8(diff.output)
	ComputeStats(pr)
	if err := s.writeAndCommit(pr, "pull #"+strconv.Itoa(number)+": update branch from "+pr.Base); err != nil {
		return err
	}
	_ = s.materializePullRef(pr)
	return nil
}

func (s *Store) reachableHeadCommit(head string) string {
	if s.WorkTree == "" || head == "" || strings.Contains(head, ":") {
		return ""
	}
	if r := s.runGit(s.WorkTree, "rev-parse", "--verify", "--quiet", head+"^{commit}"); r.ok && trimmed(r.output) != "" {
		return trimmed(r.output)
	}
	if s.Mirror != "" && s.runGit(s.WorkTree, "fetch", "--no-tags", "--quiet", s.Mirror, "refs/heads/"+head).ok {
		if r := s.runGit(s.WorkTree, "rev-parse", "--verify", "--quiet", "FETCH_HEAD^{commit}"); r.ok && trimmed(r.output) != "" {
			return trimmed(r.output)
		}
	}
	return ""
}

var stageLinePattern = regexp.MustCompile(`^\d+ [0-9a-f]+ [123]\t(.+)$`)

func (s *Store) CheckMergeable(number int) (clean bool, conflicts []string, err error) {
	conflicts = []string{}
	if !s.CanWrite() {
		return false, conflicts, errorString("Checking the merge needs a local working tree.")
	}
	pr, ok := s.ReadPull(number, true)
	if !ok {
		return false, conflicts, errorString("Pull request #" + strconv.Itoa(number) + " not found.")
	}
	if pr.BranchBacked {
		headSha := s.reachableHeadCommit(pr.Head)
		baseSha := s.runGit(s.WorkTree, "rev-parse", "--verify", "--quiet", "HEAD")
		if headSha != "" && baseSha.ok && trimmed(baseSha.output) != "" {
			tree := s.runGitRaw(s.WorkTree, "merge-tree", "--write-tree", trimmed(baseSha.output), headSha)
			if tree.timedOut {
				return false, conflicts, errorString("git timed out checking the merge.")
			}
			if tree.exitCode == 0 {
				return true, conflicts, nil
			}
			if tree.exitCode == 1 {
				lines := strings.Split(qtFromUtf8(tree.output), "\n")
				for i := 1; i < len(lines); i++ {
					if strings.TrimSpace(lines[i]) == "" {
						break
					}
					if m := stageLinePattern.FindStringSubmatch(lines[i]); m != nil && !containsString(conflicts, m[1]) {
						conflicts = append(conflicts, m[1])
					}
				}
				return false, conflicts, nil
			}
		}
	}
	patchPath := s.pullDir(number) + "/changes.patch"
	tempFile := ""
	if !exists(patchPath) {
		if strings.TrimSpace(pr.Patch) == "" {
			return false, conflicts, errorString("This pull request has no patch to merge.")
		}
		tempFile = filepath.Join(os.TempDir(), "forkmesh-check-"+strconv.Itoa(number)+".patch")
		if err := writeTextFile(tempFile, pr.Patch); err != nil {
			return false, conflicts, err
		}
		patchPath = tempFile
	}
	check := s.runner().Run(s.ctx(), s.WorkTree, []string{"apply", "--check", "--3way", patchPath}, s.gitOptionsRaw())
	if tempFile != "" {
		_ = os.Remove(tempFile)
	}
	if check.TimedOut {
		return false, conflicts, errorString("git timed out checking the merge.")
	}
	output := check.Stderr + qtFromUtf8(check.Output)
	if check.OK && !strings.Contains(output, "with conflicts") {
		return true, conflicts, nil
	}
	return false, ParseApplyConflicts(output), nil
}

func (s *Store) amStateDir(workTree string) string {
	r := s.runGit(workTree, "rev-parse", "--git-path", "rebase-apply")
	if !r.ok {
		return ""
	}
	path := trimmed(r.output)
	if path == "" {
		return ""
	}
	if !filepath.IsAbs(path) {
		path = workTree + "/" + path
	}
	if exists(path) {
		return path
	}
	return ""
}

func (s *Store) amSessionLive() bool {
	return SessionLive(s.WorkTree) || (s.AmLive != nil && s.AmLive())
}

func (s *Store) ConflictMergeInProgress() bool {
	return s.WorkTree != "" && s.amStateDir(s.WorkTree) != "" && s.amSessionLive()
}

func (s *Store) healStaleAmState() bool {
	if s.WorkTree == "" || s.amStateDir(s.WorkTree) == "" {
		return true
	}
	if s.amSessionLive() {
		return false
	}
	tracked := s.runGit(s.WorkTree, "status", "--porcelain", "--untracked-files=no")
	unmerged := strings.TrimSpace(s.runGit(s.WorkTree, "diff", "--name-only", "--diff-filter=U").out()) != ""
	keepEdits := !unmerged && trimmed(tracked.output) != ""
	mode := "--abort"
	if keepEdits {
		mode = "--quit"
	}
	s.runGit(s.WorkTree, "am", mode)
	if s.amStateDir(s.WorkTree) != "" {
		s.runGit(s.WorkTree, "am", "--quit")
	}
	if left := s.amStateDir(s.WorkTree); left != "" && filepath.Base(left) == "rebase-apply" {
		_ = safedelete.RemoveTreeUnder(filepath.Dir(left), left)
	}
	healed := s.amStateDir(s.WorkTree) == ""
	outcome := "could not be cleared"
	if healed {
		outcome = "aborted"
		if keepEdits {
			outcome = "quit, local edits kept"
		}
	}
	s.warn("git: stale am/rebase state in " + s.WorkTree + " " + outcome)
	return healed
}

func (s *Store) amBlockedMessage() string {
	if s.amSessionLive() {
		return "Another pull-request operation is already in progress; finish or cancel it first."
	}
	return "A stale git am session in this checkout could not be cleared; run \"git am --abort\" there."
}

func (s *Store) MergePull(number int, requirePeerReview bool) error {
	return s.MergePullWith(number, requirePeerReview, false)
}

func (s *Store) MergePullWith(number int, requirePeerReview, ownerReviewOverride bool) error {
	if !s.CanWrite() {
		return errorString("Merging needs a local working tree.")
	}
	pr, ok := s.ReadPull(number, true)
	if !ok {
		return errorString("Pull request #" + strconv.Itoa(number) + " not found.")
	}
	if pr.Status != "open" {
		return errorString(DecidedPullMessage(pr.Status))
	}
	reviewReady := pr.IndependentReviewGateSatisfied()
	if requirePeerReview && !reviewReady && !ownerReviewOverride {
		return errorString("At least one approval from a peer other than the pull request author is required, with no unresolved peer request for changes.")
	}
	if !s.healStaleAmState() {
		return errorString(s.amBlockedMessage())
	}
	headSha := ""
	if r := s.runGit(s.WorkTree, "rev-parse", "--verify", "-q", "refs/pr/"+strconv.Itoa(number)+"/head^{commit}"); r.ok && trimmed(r.output) != "" {
		headSha = trimmed(r.output)
	} else if pr.BranchBacked {
		headSha = s.reachableHeadCommit(pr.Head)
	}
	refMerge := headSha != ""
	preBase, preHead := "", ""
	if pr.BranchBacked {
		if r := s.runGit(s.WorkTree, "rev-parse", "--verify", pr.Base+"^{commit}"); r.ok {
			preBase = trimmed(r.output)
		}
		preHead = headSha
		if preHead == "" {
			if r := s.runGit(s.WorkTree, "rev-parse", "--verify", pr.Head+"^{commit}"); r.ok {
				preHead = trimmed(r.output)
			}
		}
	}
	title := "merge pull #" + strconv.Itoa(number) + ": " + pr.Title
	if refMerge {
		if r := s.runGit(s.WorkTree, "rev-parse", "-q", "--verify", "MERGE_HEAD"); r.ok && trimmed(r.output) == headSha {
			s.runGit(s.WorkTree, "merge", "--abort")
		}
		merged := s.runGit(s.WorkTree, "merge", "--no-edit", "-m", title, headSha)
		if !merged.ok {
			s.runGit(s.WorkTree, "merge", "--abort")
			if OverLocalChanges(merged.err) {
				return errorString(LocalChangesBlockMessage(number, filepath.FromSlash(s.WorkTree), merged.err))
			}
			return errorString("Could not merge the pull request's branch cleanly: " + merged.err)
		}
	} else {
		storedPatch := s.pullDir(number) + "/changes.patch"
		storedMbox := s.pullDir(number) + "/commits.mbox"
		tempFile := ""
		materialize := func(onDisk, content, suffix string) (string, error) {
			if !pr.BranchBacked && exists(onDisk) {
				return onDisk, nil
			}
			tempFile = filepath.Join(os.TempDir(), "forkmesh-merge-"+strconv.Itoa(number)+"."+suffix)
			if err := writeTextFile(tempFile, content); err != nil {
				return "", err
			}
			return tempFile, nil
		}
		var applyErr error
		if pr.Commits != "" {
			mbox, err := materialize(storedMbox, pr.Commits, "mbox")
			if err != nil {
				return err
			}
			if r := s.runGit(s.WorkTree, "am", "--3way", mbox); !r.ok {
				s.runGit(s.WorkTree, "am", "--abort")
				applyErr = errorString("Could not replay the pull request's commits cleanly: " + r.err)
			}
		} else {
			patch, err := materialize(storedPatch, pr.Patch, "patch")
			if err != nil {
				return err
			}
			if r := s.runGit(s.WorkTree, "apply", "--index", "--3way", patch); !r.ok {
				applyErr = errorString("Could not apply the patch cleanly: " + r.err)
			}
		}
		if tempFile != "" {
			_ = os.Remove(tempFile)
		}
		if applyErr != nil {
			return applyErr
		}
	}
	pr.Status = "merged"
	AdvanceRewardForMerge(pr)
	if pr.BranchBacked && preBase != "" && preHead != "" {
		pr.MergeBase = preBase
		pr.MergeHead = preHead
	}
	ledgerMessage := "pull #" + strconv.Itoa(number) + ": merged"
	if requirePeerReview && !reviewReady && ownerReviewOverride {
		ledgerMessage += " (review gate waived by owner)"
	}
	if err := s.writeAndCommit(pr, ledgerMessage); err != nil {
		return err
	}
	if err := s.purgePayload(number); err != nil {
		s.warn("pull " + strconv.Itoa(number) + " payload purge skipped after merge: " + err.Error())
	}
	if refMerge || pr.Commits != "" {
		return nil
	}
	if r := s.runGit(s.WorkTree, "add", "-A"); !r.ok {
		return errorString("git add failed: " + r.err)
	}
	if r := s.runGit(s.WorkTree, "commit", "-m", title); !r.ok {
		if strings.Contains(r.err, "nothing to commit") {
			return nil
		}
		return errorString("git commit failed: " + r.err)
	}
	return nil
}

func SyntheticMbox(pr *Pull, now time.Time) string {
	name := strings.TrimSpace(pr.AuthorName)
	if name == "" {
		name = "ForkMesh contributor"
	}
	email := pr.Author
	if email == "" {
		email = "contributor"
	}
	email += "@forkmesh"
	when := now
	if pr.Ts > 0 {
		when = time.UnixMilli(pr.Ts)
	}
	subject := pr.Title
	if subject == "" {
		subject = "Pull request"
	}
	var b strings.Builder
	b.WriteString("From 0000000000000000000000000000000000000000 Mon Sep 17 00:00:00 2001\n")
	b.WriteString("From: " + name + " <" + email + ">\n")
	b.WriteString("Date: " + when.Local().Format("02 Jan 2006 15:04:05 -0700") + "\n")
	b.WriteString("Subject: [PATCH] " + subject + "\n\n")
	if description := strings.TrimSpace(pr.Description); description != "" {
		b.WriteString(description + "\n\n")
	}
	b.WriteString("---\n")
	patch := pr.Patch
	if !strings.HasSuffix(patch, "\n") {
		patch += "\n"
	}
	b.WriteString(patch)
	return b.String()
}

func patchPreimageBlobs(patch string) (map[string]string, []string) {
	blobs := map[string]string{}
	blob, path := "", ""
	flush := func() {
		if blob != "" && path != "" {
			blobs[path] = blob
		}
		blob, path = "", ""
	}
	abbreviated := func(text string) string {
		if len(text) < 7 || len(text) > 64 {
			return ""
		}
		allZero := true
		for i := 0; i < len(text); i++ {
			c := text[i]
			if !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) {
				return ""
			}
			allZero = allZero && c == '0'
		}
		if allZero {
			return ""
		}
		return strings.ToLower(text)
	}
	for _, raw := range strings.Split(patch, "\n") {
		line := strings.TrimSuffix(raw, "\r")
		switch {
		case strings.HasPrefix(line, "diff --git "):
			flush()
		case blob == "" && strings.HasPrefix(line, "index "):
			rest := line[6:]
			if at := strings.Index(rest, ".."); at >= 0 {
				rest = rest[:at]
			}
			blob = abbreviated(strings.TrimSpace(rest))
		case path == "" && strings.HasPrefix(line, "--- a/"):
			candidate := strings.TrimSpace(line[6:])
			if candidate != "" && !strings.HasPrefix(candidate, "\"") {
				path = candidate
			}
		}
	}
	flush()
	keys := make([]string, 0, len(blobs))
	for key := range blobs {
		keys = append(keys, key)
	}
	sortStrings(keys)
	return blobs, keys
}

func (s *Store) patchPreimageBaseCommit(dir, patch, baseRef string) string {
	wanted, paths := patchPreimageBlobs(patch)
	if dir == "" || len(wanted) == 0 || baseRef == "" {
		return ""
	}
	revs := s.runGitTimeout(dir, append([]string{"rev-list", "--max-count=200", "--end-of-options", baseRef, "--"}, paths...), gitTimeout)
	if !revs.ok {
		return ""
	}
	checked := []string{}
	var query strings.Builder
	for _, raw := range strings.Split(revs.out(), "\n") {
		candidate := strings.ToLower(strings.TrimSpace(raw))
		if !validCommitOid(candidate) {
			continue
		}
		checked = append(checked, candidate)
		for _, path := range paths {
			query.WriteString(candidate + ":" + path + "\n")
		}
	}
	if len(checked) == 0 {
		return ""
	}
	answers := s.runGitInput(dir, []byte(query.String()), "cat-file", "--batch-check=%(objectname)")
	if !answers.ok {
		return ""
	}
	lines := strings.Split(answers.out(), "\n")
	for i, candidate := range checked {
		fits := true
		for j := 0; fits && j < len(paths); j++ {
			at := i*len(paths) + j
			line := ""
			if at < len(lines) {
				line = lines[at]
			}
			fits = line != "" && !strings.Contains(line, " ") && strings.HasPrefix(strings.ToLower(line), wanted[paths[j]])
		}
		if fits {
			return candidate
		}
	}
	return ""
}

func (s *Store) gitRun(dir string, args []string) (bool, string) {
	r := s.runGit(dir, args...)
	return r.ok, r.err
}

func (s *Store) materializePullRef(pr *Pull) error {
	if err := s.materializePullMetadataRef(pr.Number); err != nil {
		return err
	}
	number := strconv.Itoa(pr.Number)
	sha := ""
	if pr.BranchBacked {
		sha = s.reachableHeadCommit(pr.Head)
	}
	if sha == "" {
		baseOids := []string{}
		for _, ref := range []string{pr.CreationBaseOid, strings.TrimSpace(pr.Base)} {
			if oid := s.resolvedCommitOid(s.WorkTree, ref); oid != "" && !containsString(baseOids, oid) {
				baseOids = append(baseOids, oid)
			}
		}
		replayOnto := func(baseOid string) string {
			tip := ""
			short := baseOid
			if len(short) > 8 {
				short = short[:8]
			}
			scratch := filepath.Join(os.TempDir(), "forkmesh-pr-ref-"+number+"-"+strconv.FormatInt(time.Now().UnixMilli(), 10)+"-"+short)
			if !s.runGit(s.WorkTree, "worktree", "add", "--detach", scratch, baseOid).ok {
				return tip
			}
			mboxPath := filepath.Join(os.TempDir(), "forkmesh-pr-ref-"+number+".mbox")
			content := pr.Commits
			if content == "" {
				content = SyntheticMbox(pr, time.Now())
			}
			if writeTextFile(mboxPath, content) == nil && s.runGit(scratch, "am", "--3way", mboxPath).ok {
				if r := s.runGit(scratch, "rev-parse", "HEAD"); r.ok && trimmed(r.output) != "" {
					tip = trimmed(r.output)
				}
			}
			_ = os.Remove(mboxPath)
			s.runGit(scratch, "am", "--abort")
			_, _, _ = safedelete.ForceRemoveWorktree(s.gitRun, s.WorkTree, scratch)
			if exists(scratch) {
				_ = safedelete.RemoveOwnWorktreeUnder(os.TempDir(), scratch)
			}
			s.runGit(s.WorkTree, "worktree", "prune")
			return tip
		}
		for _, baseOid := range baseOids {
			if sha = replayOnto(baseOid); sha != "" {
				break
			}
		}
		if sha == "" {
			if preimage := s.patchPreimageBaseCommit(s.WorkTree, pr.Patch, strings.TrimSpace(pr.Base)); preimage != "" && !containsString(baseOids, preimage) {
				sha = replayOnto(preimage)
			}
		}
	}
	if sha == "" {
		sha = s.resolvedCommitOid(s.WorkTree, "refs/pr/"+number+"/head")
	}
	if sha == "" && s.Mirror != "" && s.runGit(s.WorkTree, "fetch", "--no-tags", "--quiet", s.Mirror, "refs/pr/"+number+"/head").ok {
		sha = s.resolvedCommitOid(s.WorkTree, "FETCH_HEAD")
	}
	if sha == "" {
		return nil
	}
	if r := s.runGit(s.WorkTree, "update-ref", "refs/pr/"+number+"/head", sha); !r.ok {
		return errorString("Could not record the pull request ref: " + r.err)
	}
	lastErr := ""
	advance := func(branchRef string) bool {
		oldSha := s.resolvedCommitOid(s.WorkTree, branchRef)
		if oldSha == sha {
			return true
		}
		if oldSha != "" && !s.runGit(s.WorkTree, "merge-base", "--is-ancestor", oldSha, sha).ok {
			return true
		}
		expected := oldSha
		if expected == "" {
			expected = strings.Repeat("0", len(sha))
		}
		r := s.runGit(s.WorkTree, "update-ref", branchRef, sha, expected)
		if r.ok {
			return true
		}
		lastErr = r.err
		raced := s.resolvedCommitOid(s.WorkTree, branchRef)
		if raced == sha || (raced != "" && s.runGit(s.WorkTree, "merge-base", "--is-ancestor", sha, raced).ok) {
			return true
		}
		return oldSha != "" && raced != ""
	}
	if !advance("refs/heads/pr/" + number) {
		return errorString("Could not record the pull request branch: " + lastErr)
	}
	if strings.HasPrefix(pr.Head, "pr/") && s.runGit(s.WorkTree, "check-ref-format", "--branch", pr.Head).ok &&
		!advance("refs/heads/"+pr.Head) {
		return errorString("Could not record the stacked pull request branch: " + lastErr)
	}
	return nil
}

func (s *Store) writeAndCommit(pr *Pull, message string) error {
	defer s.lockLedger()()
	if err := s.writePull(pr); err != nil {
		return err
	}
	return s.commit(message)
}
