package pulls

import (
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"sync"
	"time"

	"forkmesh.com/desktop-core/internal/safedelete"
)

type Session struct {
	AmBranch     string
	AmBase       string
	AmRestoreRef string
	EditWorkTree string
}

func (s *Session) Empty() bool {
	return s == nil || (s.AmBranch == "" && s.AmBase == "" && s.AmRestoreRef == "" && s.EditWorkTree == "")
}

func (s *Session) clear() {
	s.AmBranch, s.AmBase, s.AmRestoreRef = "", "", ""
}

var (
	liveMu sync.Mutex
	liveAm = map[string]bool{}
)

func WorkTreeKey(workTree string) string {
	abs := absolutePath(workTree)
	if resolved, err := filepath.EvalSymlinks(abs); err == nil {
		return resolved
	}
	return filepath.Clean(abs)
}

func setAmSessionLive(workTree string, live bool) {
	if workTree == "" {
		return
	}
	key := WorkTreeKey(workTree)
	liveMu.Lock()
	defer liveMu.Unlock()
	if live {
		liveAm[key] = true
	} else {
		delete(liveAm, key)
	}
}

func SessionLive(workTree string) bool {
	if workTree == "" {
		return false
	}
	key := WorkTreeKey(workTree)
	liveMu.Lock()
	defer liveMu.Unlock()
	return liveAm[key]
}

func ClearSessionLive(workTree string) { setAmSessionLive(workTree, false) }

func (s *Store) session() *Session {
	if s.Session == nil {
		s.Session = &Session{}
	}
	return s.Session
}

func amBranchBackupRef(branch string) string { return "refs/forkmesh/am-backup/" + branch }

func (s *Store) unmergedFiles(dir string) []string {
	r := s.runGit(dir, "diff", "--name-only", "--diff-filter=U")
	files := []string{}
	for _, line := range strings.Split(qtFromUtf8(r.output), "\n") {
		if line != "" {
			files = append(files, line)
		}
	}
	return files
}

const anotherOperationMessage = "Another pull-request operation is already in progress; finish or cancel it first."

func (s *Store) InProgress() bool {
	if s.ConflictMergeInProgress() {
		return true
	}
	edit := s.session().EditWorkTree
	return edit != "" && s.amStateDir(edit) != ""
}

func (s *Store) AgentEditWorkTree() string { return s.session().EditWorkTree }

func tempPath(name string) string { return filepath.Join(os.TempDir(), name) }

func (s *Store) scratchRun(dir string, args []string) (bool, string) {
	r := s.runGit(dir, args...)
	return r.ok, r.err
}

func (s *Store) removeScratchWorktree(repo, dir string) {
	_, _, _ = safedelete.ForceRemoveWorktree(s.scratchRun, repo, dir)
}

func (s *Store) beginPullBranch(number int) (conflicted []string, cleanApply bool, err error) {
	conflicted = []string{}
	sess := s.session()
	if !s.CanWrite() {
		return conflicted, false, errorString("This needs a local working tree.")
	}
	if s.InProgress() {
		return conflicted, false, errorString(anotherOperationMessage)
	}
	if !s.healStaleAmState() {
		return conflicted, false, errorString(s.amBlockedMessage())
	}
	if porcelain := s.runGit(s.WorkTree, "status", "--porcelain"); strings.TrimSpace(porcelain.out()) != "" {
		return conflicted, false, errorString("Commit or stash your local changes first.")
	}
	pr, ok := s.ReadPull(number, true)
	if !ok {
		return conflicted, false, errorString(MissingPullMessage(number))
	}
	if pr.Status != "open" {
		return conflicted, false, errorString(DecidedPullMessage(pr.Status))
	}
	current := trimmed(s.runGit(s.WorkTree, "rev-parse", "--abbrev-ref", "HEAD").output)
	originalCommit := trimmed(s.runGit(s.WorkTree, "rev-parse", "--verify", "HEAD").output)
	if current == "" || current == "HEAD" {
		sess.AmRestoreRef = originalCommit
	} else {
		sess.AmRestoreRef = current
	}
	branchName := strings.TrimSpace(pr.Head)
	if strings.Contains(branchName, ":") || branchName == "" || branchName == sess.AmRestoreRef || branchName == strings.TrimSpace(pr.Base) {
		branchName = "pull/" + strconv.Itoa(number)
	}
	sess.AmBranch = branchName
	startPoint := sess.AmRestoreRef
	if base := strings.TrimSpace(pr.Base); base != "" {
		if r := s.runGit(s.WorkTree, "rev-parse", "--verify", "--quiet", base+"^{commit}"); r.ok && trimmed(r.output) != "" {
			startPoint = base
		}
	}
	sess.AmBase = trimmed(s.runGit(s.WorkTree, "rev-parse", "--verify", startPoint).output)
	if sess.AmBase == "" {
		sess.AmBase = startPoint
	}
	backupRef := amBranchBackupRef(sess.AmBranch)
	s.runGit(s.WorkTree, "update-ref", "-d", backupRef)
	if previousTip := s.resolvedCommitOid(s.WorkTree, "refs/heads/"+sess.AmBranch); previousTip != "" {
		s.runGit(s.WorkTree, "update-ref", backupRef, previousTip)
	}
	if r := s.runGit(s.WorkTree, "checkout", "-B", sess.AmBranch, startPoint); !r.ok {
		s.runGit(s.WorkTree, "update-ref", "-d", backupRef)
		branch := sess.AmBranch
		sess.clear()
		return conflicted, false, errorString("Could not create the pull request branch " + branch + ": " + r.err)
	}
	realMbox := s.pullDir(number) + "/commits.mbox"
	mboxPath, tempMbox := "", ""
	if pr.Commits != "" && !pr.BranchBacked && exists(realMbox) {
		mboxPath = realMbox
	} else {
		tempMbox = tempPath("forkmesh-pull-" + strconv.Itoa(number) + ".mbox")
		content := pr.Commits
		if content == "" {
			content = SyntheticMbox(pr, time.Now())
		}
		if werr := writeTextFile(tempMbox, content); werr != nil {
			s.AbortConflictMerge()
			return conflicted, false, werr
		}
		mboxPath = tempMbox
	}
	am := s.runGitTimeout(s.WorkTree, []string{"am", "--3way", mboxPath}, 60*time.Second)
	if tempMbox != "" {
		_ = os.Remove(tempMbox)
	}
	if am.ok {
		return conflicted, true, nil
	}
	files := s.unmergedFiles(s.WorkTree)
	if s.amStateDir(s.WorkTree) == "" || len(files) == 0 {
		detail := am.err
		s.AbortConflictMerge()
		if strings.Contains(detail, "corrupt") {
			if detail == "" {
				detail = "corrupt patch"
			}
			return conflicted, false, errorString("This pull request's patch is corrupt, so it could not be applied. Sync the repository to fetch an intact copy of its commits, then try again. (" + detail + ")")
		}
		if detail == "" {
			detail = "patch did not apply"
		}
		return conflicted, false, errorString("The pull request could not be applied: " + detail)
	}
	setAmSessionLive(s.WorkTree, true)
	return files, false, nil
}

func (s *Store) StartConflictMerge(number int) (conflicted []string, resolvedClean bool, err error) {
	conflicted, clean, err := s.beginPullBranch(number)
	if err != nil {
		return conflicted, false, err
	}
	if clean {
		return conflicted, true, s.FinishConflictMerge(number)
	}
	return conflicted, false, nil
}

func (s *Store) StartConflictAgentEdit(number int) (conflicted []string, resolvedClean bool, err error) {
	conflicted, clean, err := s.beginPullConflictWorkTree(number)
	if err != nil {
		return conflicted, false, err
	}
	if clean {
		return conflicted, true, s.FinishConflictMerge(number)
	}
	return conflicted, false, nil
}

func (s *Store) StartPullFileEdit(number int, relPath string) (string, error) {
	_, clean, err := s.beginPullBranch(number)
	if err != nil {
		return "", err
	}
	if !clean {
		s.AbortConflictMerge()
		return "", errorString("This pull request has conflicts - use \"Resolve conflicts\" first, then edit.")
	}
	abs := s.WorkTree + "/" + relPath
	raw, readErr := os.ReadFile(abs)
	if !exists(abs) || readErr != nil {
		s.AbortConflictMerge()
		return "", errorString("'" + relPath + "' is not a file in this pull request.")
	}
	return qtFromUtf8(raw), nil
}

func (s *Store) FinishPullFileEdit(number int, relPath, content string) error {
	if !s.CanWrite() {
		return errorString("Editing needs a local working tree.")
	}
	if err := writeTextFile(s.WorkTree+"/"+relPath, content); err != nil {
		return err
	}
	if r := s.runGit(s.WorkTree, "add", "--", relPath); !r.ok {
		return errorString("git add failed: " + r.err)
	}
	if staged := s.runGit(s.WorkTree, "diff", "--cached", "--name-only"); strings.TrimSpace(staged.out()) == "" {
		s.AbortConflictMerge()
		return errorString("No changes to commit.")
	}
	message := "pull #" + strconv.Itoa(number) + ": edit " + relPath
	if r := s.runGit(s.WorkTree, "commit", "-m", message); !r.ok {
		return errorString("git commit failed: " + r.err)
	}
	return s.finalizeOnPullBranch(number, message)
}

func (s *Store) beginPullEditWorkTree(number int) (bool, error) {
	s.discardPullEditWorkTree()
	sess := s.session()
	if !s.CanWrite() {
		return false, nil
	}
	pr, ok := s.ReadPull(number, true)
	if !ok {
		return false, errorString(MissingPullMessage(number))
	}
	if pr.Status != "open" {
		return false, errorString(DecidedPullMessage(pr.Status))
	}
	baseRef := strings.TrimSpace(pr.Base)
	baseSha := s.resolvedCommitOid(s.WorkTree, baseRef)
	if baseSha == "" {
		baseSha = s.resolvedCommitOid(s.WorkTree, "HEAD")
	}
	if baseSha == "" {
		return false, nil
	}
	branchName := strings.TrimSpace(pr.Head)
	if strings.Contains(branchName, ":") || branchName == baseRef {
		branchName = ""
	}
	headSha := ""
	if branchName != "" && pr.BranchBacked {
		headSha = s.resolvedCommitOid(s.WorkTree, "refs/heads/"+branchName)
	}
	if branchName == "" {
		branchName = "pull/" + strconv.Itoa(number)
	}
	dir := tempPath("forkmesh-pr-edit-" + strconv.Itoa(number) + "-" + strconv.FormatInt(time.Now().UnixMilli(), 10))
	start := baseSha
	if headSha != "" {
		start = headSha
	}
	if !s.runGit(s.WorkTree, "worktree", "add", "--detach", dir, start).ok {
		s.runGit(s.WorkTree, "worktree", "prune")
		_ = safedelete.RemoveOwnWorktreeUnder(os.TempDir(), dir)
		return false, nil
	}
	sess.EditWorkTree = dir
	if headSha == "" {
		mboxPath := tempPath("forkmesh-pr-edit-" + strconv.Itoa(number) + ".mbox")
		content := pr.Commits
		if content == "" {
			content = SyntheticMbox(pr, time.Now())
		}
		applied := writeTextFile(mboxPath, content) == nil && s.runGit(dir, "am", "--3way", mboxPath).ok
		_ = os.Remove(mboxPath)
		if !applied {
			conflicted := len(s.unmergedFiles(dir)) > 0
			s.discardPullEditWorkTree()
			if conflicted {
				return false, errorString("This pull request has conflicts - use \"Resolve conflicts\" first.")
			}
			return false, errorString("The pull request could not be applied to its base.")
		}
	}
	sess.AmBranch = branchName
	sess.AmBase = baseSha
	sess.AmRestoreRef = ""
	return true, nil
}

func (s *Store) beginPullConflictWorkTree(number int) (conflicted []string, cleanApply bool, err error) {
	conflicted = []string{}
	s.discardPullEditWorkTree()
	sess := s.session()
	if !s.CanWrite() {
		return conflicted, false, errorString("Conflict fixing needs a local working tree.")
	}
	if s.InProgress() {
		return conflicted, false, errorString(anotherOperationMessage)
	}
	pr, ok := s.ReadPull(number, true)
	if !ok {
		return conflicted, false, errorString(MissingPullMessage(number))
	}
	if pr.Status != "open" {
		return conflicted, false, errorString(DecidedPullMessage(pr.Status))
	}
	baseSha := s.resolvedCommitOid(s.WorkTree, strings.TrimSpace(pr.Base))
	if baseSha == "" {
		baseSha = s.resolvedCommitOid(s.WorkTree, "HEAD")
	}
	if baseSha == "" {
		return conflicted, false, errorString("Could not resolve the pull request base.")
	}
	dir := tempPath("forkmesh-pr-conflict-" + strconv.Itoa(number) + "-" + strconv.FormatInt(time.Now().UnixMilli(), 10))
	if r := s.runGit(s.WorkTree, "worktree", "add", "--detach", dir, baseSha); !r.ok {
		s.runGit(s.WorkTree, "worktree", "prune")
		_ = safedelete.RemoveOwnWorktreeUnder(os.TempDir(), dir)
		return conflicted, false, errorString("Could not create an isolated worktree for the agent: " + r.err)
	}
	sess.EditWorkTree = dir
	branchName := strings.TrimSpace(pr.Head)
	if strings.Contains(branchName, ":") || branchName == "" || branchName == strings.TrimSpace(pr.Base) {
		branchName = "pull/" + strconv.Itoa(number)
	}
	sess.AmBranch = branchName
	sess.AmBase = baseSha
	sess.AmRestoreRef = ""
	mboxPath := tempPath("forkmesh-pr-conflict-" + strconv.Itoa(number) + ".mbox")
	content := pr.Commits
	if content == "" {
		content = SyntheticMbox(pr, time.Now())
	}
	if werr := writeTextFile(mboxPath, content); werr != nil {
		s.AbortConflictMerge()
		return conflicted, false, werr
	}
	am := s.runGitTimeout(dir, []string{"am", "--3way", mboxPath}, 60*time.Second)
	_ = os.Remove(mboxPath)
	if am.ok {
		return conflicted, true, nil
	}
	files := s.unmergedFiles(dir)
	if s.amStateDir(dir) == "" || len(files) == 0 {
		detail := am.err
		s.AbortConflictMerge()
		if detail == "" {
			detail = "patch did not apply"
		}
		return conflicted, false, errorString("The pull request could not be applied in the agent worktree: " + detail)
	}
	return files, false, nil
}

func (s *Store) discardPullEditWorkTree() {
	sess := s.session()
	if sess.EditWorkTree == "" {
		return
	}
	dir := sess.EditWorkTree
	sess.EditWorkTree = ""
	if s.amStateDir(dir) != "" {
		s.runGit(dir, "am", "--abort")
	}
	s.removeScratchWorktree(s.WorkTree, dir)
	_ = safedelete.RemoveOwnWorktreeUnder(os.TempDir(), dir)
	s.runGit(s.WorkTree, "worktree", "prune")
}

func (s *Store) StartPullAgentEdit(number int) error {
	ok, worktreeErr := s.beginPullEditWorkTree(number)
	if ok {
		return nil
	}
	if worktreeErr != nil {
		return worktreeErr
	}
	_, clean, err := s.beginPullBranch(number)
	if err != nil {
		return err
	}
	if !clean {
		s.AbortConflictMerge()
		return errorString("This pull request has conflicts - use \"Resolve conflicts\" first.")
	}
	return nil
}

func (s *Store) FinishPullAgentEdit(number int, commitMsg string) error {
	if !s.CanWrite() {
		return errorString("Editing needs a local working tree.")
	}
	sess := s.session()
	dir := sess.EditWorkTree
	if dir == "" {
		dir = s.WorkTree
	}
	if r := s.runGit(dir, "add", "-A"); !r.ok {
		return errorString("git add failed: " + r.err)
	}
	if staged := s.runGit(dir, "diff", "--cached", "--name-only"); strings.TrimSpace(staged.out()) == "" {
		s.AbortConflictMerge()
		return errorString("No changes to commit.")
	}
	if r := s.runGit(dir, "commit", "-m", commitMsg); !r.ok {
		return errorString("git commit failed: " + r.err)
	}
	if sess.EditWorkTree != "" {
		if err := s.moveBranchToEditTip(); err != nil {
			return err
		}
	}
	return s.finalizeOnPullBranch(number, commitMsg)
}

func (s *Store) moveBranchToEditTip() error {
	sess := s.session()
	tip := s.resolvedCommitOid(sess.EditWorkTree, "HEAD")
	if tip == "" {
		return errorString("Could not read the edited commit.")
	}
	r := s.runGit(s.WorkTree, "branch", "-f", sess.AmBranch, tip)
	if r.ok {
		return nil
	}
	errText := r.err
	if s.releaseWorktreeHoldingBranch(s.WorkTree, sess.AmBranch) {
		again := s.runGit(s.WorkTree, "branch", "-f", sess.AmBranch, tip)
		if again.ok {
			return nil
		}
		errText = again.err
	}
	current := trimmed(s.runGit(s.WorkTree, "rev-parse", "--abbrev-ref", "HEAD").output)
	if current == sess.AmBranch {
		ff := s.runGit(s.WorkTree, "merge", "--ff-only", tip)
		if ff.ok {
			return nil
		}
		errText = ff.err
	}
	branch := sess.AmBranch
	s.discardPullEditWorkTree()
	return errorString("The fixes could not be put on " + branch + ": " + errText)
}

func diffHeaderPath(line string) string {
	at := strings.Index(line, " b/")
	if at < 0 {
		return ""
	}
	return line[at+3:]
}

func bodyHasFileDiff(body string) bool {
	for _, line := range strings.Split(body, "\n") {
		if strings.HasPrefix(line, "diff --git ") {
			return true
		}
	}
	return false
}

func removeFileSectionsFromDiff(body, relPath string) (string, bool) {
	lines := strings.Split(body, "\n")
	kept := make([]string, 0, len(lines))
	skipping, any := false, false
	for _, line := range lines {
		if strings.HasPrefix(line, "diff --git ") {
			skipping = diffHeaderPath(line) == relPath
			if skipping {
				any = true
			}
		}
		if !skipping {
			kept = append(kept, line)
		}
	}
	return strings.Join(kept, "\n"), any
}

func splitMboxSignature(message string) (string, string) {
	lines := strings.Split(message, "\n")
	sig := -1
	for i := len(lines) - 1; i >= 0; i-- {
		if lines[i] == "-- " {
			sig = i
			break
		}
	}
	if sig < 0 {
		return message, ""
	}
	return strings.Join(lines[:sig], "\n"), strings.Join(lines[sig:], "\n")
}

func stripDiffstat(message string) string {
	lines := strings.Split(message, "\n")
	firstDiff := -1
	for i, line := range lines {
		if strings.HasPrefix(line, "diff --git ") {
			firstDiff = i
			break
		}
	}
	if firstDiff < 0 {
		return message
	}
	cut := -1
	for i := firstDiff - 1; i >= 0; i-- {
		if lines[i] == "---" {
			cut = i
			break
		}
	}
	if cut < 0 {
		return message
	}
	kept := append([]string{}, lines[:cut+1]...)
	i := cut + 1
	for i < firstDiff && (lines[i] == "" || strings.HasPrefix(lines[i], " ")) {
		i++
	}
	kept = append(kept, "")
	kept = append(kept, lines[i:]...)
	return strings.Join(kept, "\n")
}

var mboxBoundary = regexp.MustCompile(`^From [0-9a-f]+ Mon Sep 17 00:00:00 2001$`)

func removeFileFromMbox(mbox, relPath string) string {
	lines := strings.Split(mbox, "\n")
	var messages [][]string
	for _, line := range lines {
		if len(messages) == 0 || mboxBoundary.MatchString(line) {
			messages = append(messages, []string{})
		}
		messages[len(messages)-1] = append(messages[len(messages)-1], line)
	}
	out := []string{}
	for _, msgLines := range messages {
		message := strings.Join(msgLines, "\n")
		if len(msgLines) == 0 || !mboxBoundary.MatchString(msgLines[0]) {
			out = append(out, message)
			continue
		}
		body, trailer := splitMboxSignature(message)
		newBody, removed := removeFileSectionsFromDiff(body, relPath)
		if !removed {
			out = append(out, message)
			continue
		}
		if !bodyHasFileDiff(newBody) {
			continue
		}
		trimmedBody := stripDiffstat(newBody)
		if trailer == "" {
			out = append(out, trimmedBody)
		} else {
			out = append(out, trimmedBody+"\n"+trailer)
		}
	}
	result := strings.Join(out, "\n")
	if strings.TrimSpace(result) == "" {
		return ""
	}
	return result
}

func (s *Store) DeletePullFile(number int, relPath string) error {
	if !s.CanWrite() {
		return errorString("Editing needs a local working tree.")
	}
	pr, ok := s.ReadPull(number, true)
	if !ok {
		return errorString(MissingPullMessage(number))
	}
	if pr.Status != "open" {
		return errorString(DecidedPullMessage(pr.Status))
	}
	newPatch, removed := removeFileSectionsFromDiff(pr.Patch, relPath)
	if !removed {
		return errorString("'" + relPath + "' is not a file in this pull request.")
	}
	if !bodyHasFileDiff(newPatch) {
		return errorString("'" + relPath + "' is the only file changed by this pull request; close the pull request instead of deleting its last file.")
	}
	pr.Patch = newPatch
	if pr.Commits != "" {
		pr.Commits = removeFileFromMbox(pr.Commits, relPath)
	}
	ComputeStats(pr)
	pr.BranchBacked = false
	pr.CreationBaseOid, pr.CreationHeadOid, pr.MergeBase, pr.MergeHead = "", "", "", ""
	if err := s.deleteFileLedger(pr, relPath); err != nil {
		return err
	}
	_ = s.materializePullRef(pr)
	return nil
}

func (s *Store) deleteFileLedger(pr *Pull, relPath string) error {
	defer s.lockLedger()()
	if err := s.writePull(pr); err != nil {
		return err
	}
	if pr.Commits == "" {
		_ = os.Remove(s.pullDir(pr.Number) + "/commits.mbox")
	}
	return s.commit("pull #" + strconv.Itoa(pr.Number) + ": delete " + relPath)
}

func (s *Store) finalizeOnPullBranch(number int, commitMsg string) error {
	sess := s.session()
	if sess.AmRestoreRef != "" && sess.AmRestoreRef != sess.AmBranch {
		if r := s.runGit(s.WorkTree, "checkout", sess.AmRestoreRef); !r.ok {
			return errorString("Committed on " + sess.AmBranch + ", but could not return to " + sess.AmRestoreRef + ": " + r.err)
		}
	}
	pr, ok := s.ReadPull(number, true)
	if !ok {
		return errorString(MissingPullMessage(number))
	}
	rangeSpec := sess.AmBase + ".." + sess.AmBranch
	diff := s.runGit(s.WorkTree, "diff", "--binary", rangeSpec)
	if !diff.ok {
		return errorString("Could not refresh the pull request patch: " + diff.err)
	}
	mbox := s.runGit(s.WorkTree, "format-patch", "--binary", "--stdout", rangeSpec)
	if !mbox.ok {
		return errorString("Could not refresh the pull request commits: " + mbox.err)
	}
	pr.Patch = qtFromUtf8(diff.output)
	pr.Commits = qtFromUtf8(mbox.output)
	pr.Head = sess.AmBranch
	ComputeStats(pr)
	unlock := s.lockLedger()
	if err := s.writePull(pr); err != nil {
		unlock()
		return err
	}
	s.discardPullEditWorkTree()
	if s.amStateDir(s.WorkTree) == "" {
		setAmSessionLive(s.WorkTree, false)
	}
	sess.clear()
	err := s.commit(commitMsg)
	unlock()
	if err != nil {
		return err
	}
	_ = s.materializePullRef(pr)
	return nil
}

func (s *Store) FinishConflictMerge(number int) error {
	if !s.CanWrite() {
		return errorString("Merging needs a local working tree.")
	}
	sess := s.session()
	dir := sess.EditWorkTree
	if dir == "" {
		dir = s.WorkTree
	}
	for _, rel := range s.unmergedFiles(dir) {
		raw, err := os.ReadFile(dir + "/" + rel)
		if err != nil {
			continue
		}
		text := qtFromUtf8(raw)
		if strings.Contains(text, "\n<<<<<<< ") || strings.HasPrefix(text, "<<<<<<< ") || strings.Contains(text, "\n>>>>>>> ") {
			return errorString("Resolve every conflict marker in " + rel + " first.")
		}
	}
	if r := s.runGit(dir, "add", "-A"); !r.ok {
		return errorString("git add failed: " + r.err)
	}
	if s.amStateDir(dir) != "" {
		if r := s.runGit(dir, "am", "--continue"); !r.ok {
			return errorString("Could not complete the merge: " + r.err)
		}
	}
	if sess.EditWorkTree != "" {
		if err := s.moveBranchToEditTip(); err != nil {
			return err
		}
	}
	return s.finalizeOnPullBranch(number, "pull #"+strconv.Itoa(number)+": resolve conflicts on "+sess.AmBranch)
}

func (s *Store) AbortConflictMerge() {
	if s.WorkTree == "" {
		return
	}
	sess := s.session()
	if sess.EditWorkTree != "" {
		s.discardPullEditWorkTree()
		sess.clear()
		return
	}
	if s.amStateDir(s.WorkTree) != "" {
		s.runGit(s.WorkTree, "am", "--abort")
	}
	setAmSessionLive(s.WorkTree, false)
	if sess.AmRestoreRef != "" && sess.AmRestoreRef != sess.AmBranch {
		s.runGit(s.WorkTree, "checkout", sess.AmRestoreRef)
	}
	if sess.AmBranch != "" {
		backupRef := amBranchBackupRef(sess.AmBranch)
		if previousTip := s.resolvedCommitOid(s.WorkTree, backupRef); previousTip != "" {
			if s.runGit(s.WorkTree, "branch", "-f", sess.AmBranch, previousTip).ok {
				s.runGit(s.WorkTree, "update-ref", "-d", backupRef)
			}
		} else {
			s.runGit(s.WorkTree, "branch", "-D", sess.AmBranch)
		}
	}
	sess.clear()
}

type ReviewCheckout struct {
	Path        string   `json:"path"`
	BaseOid     string   `json:"baseOid"`
	HeadOid     string   `json:"headOid"`
	CommitCount int      `json:"commitCount"`
	Patch       string   `json:"patch"`
	Uncommitted bool     `json:"uncommitted"`
	Conflicts   []string `json:"conflicts"`
}

func (s *Store) reviewHost() string {
	if s.CanWrite() {
		return s.WorkTree
	}
	return s.Mirror
}

func (s *Store) CheckoutForReview(pr *Pull, dir string) (ReviewCheckout, error) {
	result := ReviewCheckout{Conflicts: []string{}}
	host := s.reviewHost()
	if strings.TrimSpace(host) == "" {
		return ReviewCheckout{Conflicts: []string{}}, errorString("Reviewing a submission needs a local copy of this repository on this computer.")
	}
	if strings.TrimSpace(dir) == "" {
		return ReviewCheckout{Conflicts: []string{}}, errorString("No review folder was given.")
	}
	s.DiscardReviewCheckout(dir)
	_ = os.MkdirAll(filepath.Dir(absolutePath(dir)), 0o755)
	baseSha := s.resolvedCommitOid(host, pr.CreationBaseOid)
	if baseSha == "" {
		baseSha = s.resolvedCommitOid(host, strings.TrimSpace(pr.Base))
	}
	if baseSha == "" {
		baseSha = s.resolvedCommitOid(host, "HEAD")
	}
	if baseSha == "" {
		named := strings.TrimSpace(pr.Base)
		if named == "" {
			named = "unnamed"
		}
		return ReviewCheckout{Conflicts: []string{}}, errorString("Could not resolve the base this pull request applies to (" + named + ").")
	}
	headSha := ""
	if pr.BranchBacked {
		headSha = s.reachableHeadCommit(pr.Head)
	}
	start := baseSha
	if headSha != "" {
		start = headSha
	}
	if r := s.runGit(host, "worktree", "add", "--detach", dir, start); !r.ok {
		s.runGit(host, "worktree", "prune")
		_ = safedelete.RemoveOwnWorktreeUnder(filepath.Dir(absolutePath(dir)), dir)
		return ReviewCheckout{Conflicts: []string{}}, errorString("Could not create the review checkout: " + r.err)
	}
	result.Path = dir
	result.BaseOid = baseSha
	if headSha == "" {
		stem := "forkmesh-pr-review-" + strconv.Itoa(pr.Number) + "-" + strconv.FormatInt(time.Now().UnixMilli(), 10)
		mboxPath := tempPath(stem + ".mbox")
		content := pr.Commits
		if content == "" {
			content = SyntheticMbox(pr, time.Now())
		}
		replayed := writeTextFile(mboxPath, content) == nil && s.runGit(dir, "am", "--3way", mboxPath).ok
		_ = os.Remove(mboxPath)
		if !replayed {
			if s.amStateDir(dir) != "" {
				s.runGit(dir, "am", "--abort")
			}
			patchPath := tempPath(stem + ".patch")
			wrote := pr.Patch != "" && writeTextFile(patchPath, pr.Patch) == nil
			applyErr := ""
			applied := false
			if wrote {
				r := s.runGit(dir, "apply", "--3way", "--whitespace=nowarn", patchPath)
				applied = r.ok
				applyErr = r.err
			}
			conflicts := []string{}
			if wrote {
				conflicts = s.unmergedFiles(dir)
			}
			_ = os.Remove(patchPath)
			if !applied && len(conflicts) == 0 {
				s.DiscardReviewCheckout(dir)
				target := strings.TrimSpace(pr.Base)
				if target == "" {
					target = leftRunes(baseSha, 8)
				}
				if applyErr == "" {
					applyErr = "no reviewable change was submitted"
				}
				return ReviewCheckout{Conflicts: []string{}}, errorString("The pull request could not be applied to " + target + ": " + applyErr)
			}
			result.Uncommitted = true
			result.Conflicts = conflicts
		}
	}
	if tip := s.runGit(dir, "rev-parse", "HEAD"); tip.ok {
		result.HeadOid = trimmed(tip.output)
	}
	if !result.Uncommitted {
		if count := s.runGit(dir, "rev-list", "--count", baseSha+"..HEAD"); count.ok {
			result.CommitCount, _ = qtToInt(trimmed(count.output))
		}
	}
	var diff gitResult
	if result.Uncommitted {
		diff = s.runGit(dir, "diff", "--binary", "HEAD")
	} else {
		diff = s.runGit(dir, "diff", "--binary", baseSha+"..HEAD")
	}
	result.Patch = qtFromUtf8(diff.output)
	if strings.TrimSpace(result.Patch) == "" {
		result.Patch = pr.Patch
	}
	return result, nil
}

func (s *Store) DiscardReviewCheckout(dir string) {
	if strings.TrimSpace(dir) == "" {
		return
	}
	host := s.reviewHost()
	if exists(dir) && s.amStateDir(dir) != "" {
		s.runGit(dir, "am", "--abort")
	}
	if strings.TrimSpace(host) != "" {
		s.removeScratchWorktree(host, absolutePath(dir))
	}
	_ = safedelete.RemoveOwnWorktreeUnder(filepath.Dir(absolutePath(dir)), dir)
	if strings.TrimSpace(host) != "" {
		s.runGit(host, "worktree", "prune")
	}
}
