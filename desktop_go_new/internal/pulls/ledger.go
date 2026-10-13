package pulls

import (
	"encoding/json"
	"io/fs"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"

	"forkmesh.com/desktop-core/internal/safedelete"
)

const (
	gitRewriteTimeout   = 10 * time.Minute
	pullLoadSettledMs   = 3000
	pullLoadCacheLimit  = 8
	readOnlyMessage     = "This repository is read-only on this node."
	unrelatedChangesMsg = "Commit or stash unrelated tracked changes before deleting a pull request."
)

var (
	healMu   sync.Mutex
	healed   = map[string]bool{}
	cacheMu  sync.Mutex
	cacheMap = map[string]loadCacheEntry{}
)

type loadCacheEntry struct {
	stamp string
	pulls []*Pull
}

func claimPullMetadataHeal(dir string) bool {
	healMu.Lock()
	defer healMu.Unlock()
	if healed[dir] {
		return false
	}
	healed[dir] = true
	return true
}

func (s *Store) resyncPullMetadataCheckout(dir string) {
	s.runGit(dir, "reset", "--mixed", "-q", "HEAD")
	listing := s.runGit(dir, "diff", "--name-only", "-z", "HEAD", "--", ".forkmesh/pulls/", ".forkmesh/branch-reviews/")
	if !listing.ok {
		return
	}
	stale := []string{}
	for _, entry := range strings.Split(listing.out(), "\x00") {
		if entry != "" {
			stale = append(stale, qtFromUtf8([]byte(entry)))
		}
	}
	for len(stale) > 0 {
		n := len(stale)
		if n > 200 {
			n = 200
		}
		batch := stale[:n]
		stale = stale[n:]
		s.runGit(dir, append([]string{"checkout", "-f", "HEAD", "--"}, batch...)...)
	}
}

func clonePull(p *Pull) *Pull {
	copied := *p
	copied.Events = append([]Event(nil), p.Events...)
	return &copied
}

func clonePulls(list []*Pull) []*Pull {
	out := make([]*Pull, 0, len(list))
	for _, p := range list {
		out = append(out, clonePull(p))
	}
	return out
}

func loadCacheGet(key, stamp string) ([]*Pull, bool) {
	cacheMu.Lock()
	defer cacheMu.Unlock()
	entry, ok := cacheMap[key]
	if !ok || entry.stamp != stamp {
		return nil, false
	}
	return clonePulls(entry.pulls), true
}

func loadCachePut(key, stamp string, list []*Pull) {
	cacheMu.Lock()
	defer cacheMu.Unlock()
	if _, ok := cacheMap[key]; !ok && len(cacheMap) >= pullLoadCacheLimit {
		cacheMap = map[string]loadCacheEntry{}
	}
	cacheMap[key] = loadCacheEntry{stamp: stamp, pulls: clonePulls(list)}
}

type loadStamp struct {
	newest  int64
	entries int64
	bytes   int64
}

func (st *loadStamp) note(path string) {
	info, err := os.Stat(path)
	if err != nil {
		return
	}
	if ms := info.ModTime().UnixMilli(); ms > st.newest {
		st.newest = ms
	}
	st.entries++
	if info.Mode().IsRegular() {
		st.bytes += info.Size()
	}
}

func (st *loadStamp) noteTree(root string) {
	_ = filepath.WalkDir(root, func(path string, d fs.DirEntry, err error) error {
		if err != nil {
			return nil
		}
		if path == root {
			return nil
		}
		st.note(path)
		return nil
	})
}

func loadGitDir(dir string) string {
	dotGit := filepath.Join(dir, ".git")
	info, err := os.Stat(dotGit)
	if err == nil && info.IsDir() {
		return absolutePath(dotGit)
	}
	if err == nil {
		line, ok := firstLine(dotGit)
		if !ok || !strings.HasPrefix(line, "gitdir:") {
			return ""
		}
		target := strings.TrimSpace(line[7:])
		if target == "" {
			return ""
		}
		if !filepath.IsAbs(target) {
			target = filepath.Join(absolutePath(dir), target)
		}
		return filepath.Clean(target)
	}
	if exists(filepath.Join(dir, "HEAD")) {
		if objects, err := os.Stat(filepath.Join(dir, "objects")); err == nil && objects.IsDir() {
			return absolutePath(dir)
		}
	}
	return ""
}

func loadCommonDir(gitDir string) string {
	line, ok := firstLine(filepath.Join(gitDir, "commondir"))
	if !ok || line == "" {
		return gitDir
	}
	if !filepath.IsAbs(line) {
		line = filepath.Join(gitDir, line)
	}
	return filepath.Clean(line)
}

func noteLoadRepository(dir string, gitDirs *[]string) bool {
	if strings.TrimSpace(dir) == "" {
		return true
	}
	gitDir := loadGitDir(dir)
	if gitDir == "" {
		return false
	}
	for _, candidate := range []string{gitDir, loadCommonDir(gitDir)} {
		if exists(filepath.Join(candidate, "reftable")) {
			return false
		}
		if !containsString(*gitDirs, candidate) {
			*gitDirs = append(*gitDirs, candidate)
		}
	}
	return true
}

func pullLoadStamp(workTree, mirror, pullsDir string) string {
	gitDirs := []string{}
	if !noteLoadRepository(workTree, &gitDirs) || !noteLoadRepository(mirror, &gitDirs) {
		return ""
	}
	var st loadStamp
	for _, gitDir := range gitDirs {
		st.note(filepath.Join(gitDir, "HEAD"))
		st.note(filepath.Join(gitDir, "packed-refs"))
		st.noteTree(filepath.Join(gitDir, "refs"))
	}
	if pullsDir != "" {
		st.noteTree(pullsDir)
	}
	if st.newest >= time.Now().UnixMilli()-pullLoadSettledMs {
		return ""
	}
	return strings.Join(gitDirs, "|") + "#" + pullsDir + "#" + strconv.FormatInt(st.newest, 10) + ":" +
		strconv.FormatInt(st.entries, 10) + ":" + strconv.FormatInt(st.bytes, 10)
}

func (s *Store) metaDir() string {
	if meta := s.MetaWorkTree(); meta != "" {
		return meta
	}
	return s.WorkTree
}

type batchEntry struct {
	content []byte
	ok      bool
}

func parseCatFileBatch(batch []byte, count int) []batchEntry {
	out := make([]batchEntry, count)
	pos := 0
	for i := 0; i < count; i++ {
		nl := indexByteFrom(batch, '\n', pos)
		if nl < 0 {
			break
		}
		header := strings.Split(string(batch[pos:nl]), " ")
		pos = nl + 1
		if len(header) != 3 || header[1] != "blob" || !validCommitOid(header[0]) {
			continue
		}
		size, err := strconv.ParseInt(header[2], 10, 64)
		if err != nil || size < 0 || size >= int64(len(batch)-pos) {
			break
		}
		out[i] = batchEntry{content: batch[pos : pos+int(size)], ok: true}
		pos += int(size) + 1
	}
	return out
}

func indexByteFrom(data []byte, b byte, from int) int {
	if from >= len(data) {
		return -1
	}
	for i := from; i < len(data); i++ {
		if data[i] == b {
			return i
		}
	}
	return -1
}

var legacyPullPath = regexp.MustCompile(`^\.forkmesh/pulls/open/(?:pull-)?([1-9][0-9]*)\.json$`)

func (s *Store) LoadAllIncludingArchived(derive bool) []*Pull {
	pulls := s.LoadAll(derive)
	known := map[int]bool{}
	for _, pull := range pulls {
		known[pull.Number] = true
	}
	sources := map[string]bool{}
	appendSource := func(source string) {
		trimmedSource := strings.TrimSpace(source)
		if trimmedSource == "" {
			return
		}
		clean := filepath.Clean(trimmedSource)
		if sources[clean] || !exists(clean) {
			return
		}
		sources[clean] = true
		codeView := &Store{Mirror: clean, DataRoot: s.DataRoot, Runner: s.Runner, Ctx: s.Ctx, Env: s.Env}
		for _, pull := range codeView.loadFromMirror(clean, "HEAD", derive) {
			if pull.Number <= 0 || known[pull.Number] {
				continue
			}
			pull.ArchivedOnCodeBranch = true
			known[pull.Number] = true
			pulls = append(pulls, pull)
		}
		listing := s.runGit(clean, "ls-tree", "-r", "--name-only", "HEAD", "--", ".forkmesh/pulls/open/")
		if !listing.ok {
			return
		}
		type legacyFile struct {
			number int
			path   string
		}
		files := []legacyFile{}
		var query strings.Builder
		for _, raw := range strings.Split(listing.out(), "\n") {
			path := strings.TrimSpace(qtFromUtf8([]byte(raw)))
			m := legacyPullPath.FindStringSubmatch(path)
			if m == nil {
				continue
			}
			number, _ := qtToInt(m[1])
			if known[number] {
				continue
			}
			files = append(files, legacyFile{number, path})
			query.WriteString("HEAD:" + path + "\n")
		}
		if len(files) == 0 {
			return
		}
		batch := s.runGitInput(clean, []byte(query.String()), "cat-file", "--batch")
		if !batch.ok {
			return
		}
		entries := parseCatFileBatch(batch.output, len(files))
		byPath := map[string][]byte{}
		for i, file := range files {
			if entries[i].ok {
				byPath[file.path] = entries[i].content
			}
		}
		for _, file := range files {
			content, ok := byPath[file.path]
			if known[file.number] || !ok {
				continue
			}
			var document map[string]any
			if json.Unmarshal(content, &document) != nil || document == nil {
				continue
			}
			pull := PullFromJSON(document)
			pull.Number = file.number
			pull.ArchivedOnCodeBranch = true
			if list, ok := document["events"].([]any); ok {
				for _, item := range list {
					if event, ok := item.(map[string]any); ok {
						pull.Events = append(pull.Events, EventFromJSON(event))
					}
				}
			}
			known[file.number] = true
			pulls = append(pulls, &pull)
		}
	}
	appendSource(s.WorkTree)
	appendSource(s.Mirror)
	sort.SliceStable(pulls, func(i, j int) bool { return pulls[i].Number < pulls[j].Number })
	return pulls
}

func (s *Store) RestoreArchivedPull(number int) error {
	if !s.CanWrite() {
		return errorString("Restoring a pull request needs a local working tree.")
	}
	if number <= 0 {
		return errorString("The pull request number is invalid.")
	}
	if exists(s.pullDir(number)) {
		return nil
	}
	prefix := ".forkmesh/pulls/" + strconv.Itoa(number) + "/"
	source := ""
	var paths []string
	for _, candidate := range []string{s.WorkTree, s.Mirror} {
		if strings.TrimSpace(candidate) == "" || !exists(candidate) {
			continue
		}
		listing := s.runGit(candidate, "ls-tree", "-r", "--name-only", "HEAD", "--", prefix)
		if !listing.ok {
			continue
		}
		found := []string{}
		for _, raw := range strings.Split(listing.out(), "\n") {
			path := strings.TrimSpace(qtFromUtf8([]byte(raw)))
			if strings.HasPrefix(path, prefix) {
				found = append(found, path)
			}
		}
		if containsString(found, prefix+"pull.md") {
			source = candidate
			paths = found
			break
		}
	}
	if source == "" {
		for _, candidate := range []string{s.WorkTree, s.Mirror} {
			if strings.TrimSpace(candidate) == "" || !exists(candidate) {
				continue
			}
			for _, path := range []string{".forkmesh/pulls/open/pull-" + strconv.Itoa(number) + ".json", ".forkmesh/pulls/open/" + strconv.Itoa(number) + ".json"} {
				shown := s.runGit(candidate, "show", "HEAD:"+path)
				if !shown.ok {
					continue
				}
				var document map[string]any
				if json.Unmarshal(shown.output, &document) != nil || document == nil {
					continue
				}
				pull := PullFromJSON(document)
				pull.Number = number
				if err := s.writeAndCommit(&pull, "pull #"+strconv.Itoa(number)+": restore archived record"); err != nil {
					return err
				}
				_ = s.materializePullRef(&pull)
				return nil
			}
		}
		return errorString("Archived pull request #" + strconv.Itoa(number) + " could not be read from the repository branch.")
	}
	root := s.pullsDir()
	dest := root + "/" + strconv.Itoa(number)
	if os.MkdirAll(dest, 0o755) != nil {
		return errorString("Could not create the pull ledger folder.")
	}
	var query strings.Builder
	for _, path := range paths {
		query.WriteString("HEAD:" + path + "\n")
	}
	contents := map[string][]byte{}
	if batch := s.runGitInput(source, []byte(query.String()), "cat-file", "--batch"); batch.ok {
		entries := parseCatFileBatch(batch.output, len(paths))
		for i, path := range paths {
			if entries[i].ok {
				contents[path] = entries[i].content
			}
		}
	}
	for _, path := range paths {
		content, ok := contents[path]
		if !ok {
			_ = safedelete.RemoveTreeUnder(root, dest)
			return errorString("Could not read archived pull metadata.")
		}
		target := dest + "/" + path[len(prefix):]
		if os.MkdirAll(filepath.Dir(target), 0o755) != nil {
			_ = safedelete.RemoveTreeUnder(root, dest)
			return errorString("Could not create the pull ledger folder.")
		}
		if os.WriteFile(target, content, 0o666) != nil {
			_ = safedelete.RemoveTreeUnder(root, dest)
			return errorString("Could not restore archived pull metadata.")
		}
	}
	unlock := s.lockLedger()
	err := s.commit("pull #" + strconv.Itoa(number) + ": restore archived record")
	unlock()
	if err != nil {
		return err
	}
	if restored, ok := s.ReadPull(number, true); ok {
		_ = s.materializePullRef(restored)
	}
	return nil
}

func (s *Store) NextNumber() int {
	max := 0
	for _, pull := range s.LoadAllIncludingArchived(false) {
		if pull.Number > max {
			max = pull.Number
		}
	}
	return max + 1
}

func (s *Store) deriveFromImmutableOids(dir, baseOid, headOid string) (string, string, bool) {
	if dir == "" || !validCommitOid(baseOid) || !validCommitOid(headOid) {
		return "", "", false
	}
	base := s.runGit(dir, "rev-parse", "--verify", "--quiet", "--end-of-options", baseOid+"^{commit}")
	head := s.runGit(dir, "rev-parse", "--verify", "--quiet", "--end-of-options", headOid+"^{commit}")
	if !base.ok || !head.ok {
		return "", "", false
	}
	if !strings.EqualFold(trimmed(base.output), baseOid) || !strings.EqualFold(trimmed(head.output), headOid) {
		return "", "", false
	}
	diff := s.runGit(dir, "diff", "--no-ext-diff", "--no-textconv", "--binary", baseOid+"..."+headOid)
	if !diff.ok {
		return "", "", false
	}
	mbox := s.runGit(dir, "format-patch", "--binary", "--stdout", baseOid+".."+headOid)
	if !mbox.ok {
		return "", "", false
	}
	return qtFromUtf8(diff.output), qtFromUtf8(mbox.output), true
}

type CreateRequest struct {
	Title        string
	Description  string
	Base         string
	Head         string
	Patch        string
	Commits      string
	BranchBacked bool
}

func (s *Store) CreatePull(req CreateRequest) (int, error) {
	if !s.CanWrite() {
		return -1, errorString(readOnlyMessage)
	}
	if req.Head != "" {
		for _, existing := range s.LoadAll(false) {
			if existing.Status == "open" && existing.Head == req.Head {
				return existing.Number, nil
			}
		}
	}
	pr := Pull{Title: req.Title, Description: req.Description, Base: req.Base, Head: req.Head, Patch: req.Patch,
		Commits: req.Commits, Status: "open"}
	if req.BranchBacked {
		baseOid := s.resolvedCommitOid(s.WorkTree, req.Base)
		headOid := s.resolvedCommitOid(s.WorkTree, req.Head)
		if baseOid != "" && headOid != "" {
			if patch, commits, ok := s.deriveFromImmutableOids(s.WorkTree, baseOid, headOid); ok && strings.TrimSpace(patch) != "" {
				pr.BranchBacked = true
				pr.CreationBaseOid = baseOid
				pr.CreationHeadOid = headOid
				pr.Patch = patch
				pr.Commits = commits
			}
		}
	}
	pr = s.Signer.SignPull(pr)
	unlock := s.lockLedger()
	pr.Number = s.NextNumber()
	if err := s.writePull(&pr); err != nil {
		unlock()
		return -1, err
	}
	if err := s.commit("pull #" + strconv.Itoa(pr.Number) + ": open"); err != nil {
		unlock()
		return -1, err
	}
	unlock()
	_ = s.materializePullRef(&pr)
	return pr.Number, nil
}

func (s *Store) purgePayload(number int) error {
	if s.Purge != nil {
		return s.Purge(number)
	}
	prefix := ".forkmesh/pulls/" + strconv.Itoa(number) + "/"
	return s.rewrite(number, []string{prefix + "changes.patch", prefix + "commits.mbox"})
}

func (s *Store) rewrite(number int, paths []string) error {
	if s.Rewrite == nil {
		return errorString("no desktop to run it")
	}
	return s.Rewrite(number, paths)
}

var errNoMessage = errorString("")

func (s *Store) SetStatus(number int, status string) error {
	if !s.CanWrite() {
		return errNoMessage
	}
	if status == "closed" {
		return s.DeletePull(number, true)
	}
	if err := s.setStatusLedger(number, status); err != nil {
		return err
	}
	if status == "open" {
		return nil
	}
	return s.purgePayload(number)
}

func (s *Store) setStatusLedger(number int, status string) error {
	defer s.lockLedger()()
	pr, ok := s.ReadPull(number, true)
	if !ok {
		return errorString("Pull request #" + strconv.Itoa(number) + " not found.")
	}
	pr.Status = status
	if err := s.writePull(pr); err != nil {
		return err
	}
	return s.commit("pull #" + strconv.Itoa(number) + ": " + status)
}

func (s *Store) RecordRewardPayment(number int, rewardID, chainSignature string) error {
	if !s.CanWrite() {
		return errorString(readOnlyMessage)
	}
	defer s.lockLedger()()
	pr, ok := s.ReadPull(number, true)
	if !ok {
		return errorString("Pull request #" + strconv.Itoa(number) + " not found.")
	}
	reward := strings.TrimSpace(rewardID)
	signature := strings.TrimSpace(chainSignature)
	if reward == "" || signature == "" || strings.TrimSpace(pr.RewardID) != reward {
		return errorString("Pull request #" + strconv.Itoa(number) + " does not carry reward " + rewardID + ".")
	}
	recorded := strings.TrimSpace(pr.RewardTxSignature)
	if recorded == signature && strings.TrimSpace(pr.RewardStatus) == "paid" {
		return nil
	}
	if recorded != "" && recorded != signature {
		return errorString("Pull request #" + strconv.Itoa(number) + " already records a different reward transaction.")
	}
	if strings.TrimSpace(pr.RewardStatus) == "cancelled" {
		return errorString("Pull request #" + strconv.Itoa(number) + "'s reward was cancelled.")
	}
	pr.RewardTxSignature = signature
	pr.RewardStatus = "paid"
	if err := s.writePull(pr); err != nil {
		return err
	}
	return s.commit("pull #" + strconv.Itoa(number) + ": reward paid")
}

func shortAuthor(name, author string) string {
	if name != "" {
		return name
	}
	return leftRunes(author, 8)
}

func (s *Store) ApplyRemotePull(incoming Pull) error {
	if !s.CanWrite() {
		return errNoMessage
	}
	pr := incoming
	unlock := s.lockLedger()
	reserved := s.LoadAllIncludingArchived(false)
	for _, existing := range reserved {
		sameSig := pr.Sig != "" && existing.Sig == pr.Sig
		sameAuthorTs := pr.Sig == "" && existing.Author == pr.Author && existing.Ts == pr.Ts && existing.Title == pr.Title
		if sameSig || sameAuthorTs {
			unlock()
			return nil
		}
	}
	taken := false
	for _, existing := range reserved {
		if existing.Number == pr.Number {
			taken = true
			break
		}
	}
	if pr.Number <= 0 || taken || exists(s.pullDir(pr.Number)+"/pull.md") {
		pr.Number = s.NextNumber()
	}
	pr.Status = "open"
	if err := s.writePull(&pr); err != nil {
		unlock()
		return err
	}
	if err := s.commit("pull #" + strconv.Itoa(pr.Number) + ": opened (from " + shortAuthor(pr.AuthorName, pr.Author) + ")"); err != nil {
		unlock()
		return err
	}
	unlock()
	_ = s.materializePullRef(&pr)
	return nil
}

func MissingPullMessage(number int) string {
	return "Pull request #" + strconv.Itoa(number) + " not found."
}

func (s *Store) removeMaterializedPullRefs(number int) {
	n := strconv.Itoa(number)
	compatibility := "refs/pr/" + n + "/head"
	visible := "refs/heads/pr/" + n
	if tip := s.resolvedCommitOid(s.WorkTree, compatibility); tip != "" {
		s.runGit(s.WorkTree, "update-ref", "-d", visible, tip)
		s.runGit(s.WorkTree, "update-ref", "-d", compatibility, tip)
	}
	s.runGit(s.WorkTree, "update-ref", "-d", "refs/pr/"+n+"/metadata")
	s.runGit(s.WorkTree, "update-ref", "-d", pullMetadataBranchRef(number))
}

func (s *Store) unrelatedTrackedChanges(metaDir string, relPaths []string) error {
	status := s.runGit(metaDir, "status", "--porcelain", "--untracked-files=no")
	if !status.ok {
		return errorString("git status failed: " + status.err)
	}
	for _, line := range strings.Split(status.out(), "\n") {
		if len(line) < 4 {
			continue
		}
		path := strings.TrimSpace(qtFromUtf8([]byte(line[3:])))
		related := false
		for _, rel := range relPaths {
			if path == rel || strings.HasPrefix(path, rel+"/") {
				related = true
				break
			}
		}
		if !related {
			return errorString(unrelatedChangesMsg)
		}
	}
	return nil
}

func (s *Store) DeletePull(number int, rewriteHistory bool) error {
	if !s.CanWrite() {
		return errorString(readOnlyMessage)
	}
	dir := s.pullDir(number)
	if !exists(dir) {
		s.removeMaterializedPullRefs(number)
		return errorString(MissingPullMessage(number))
	}
	relPath := ".forkmesh/pulls/" + strconv.Itoa(number)
	metaDir := s.metaDir()
	if err := s.deleteLedger(number, dir, relPath, metaDir, rewriteHistory); err != nil {
		return err
	}
	if !rewriteHistory {
		return nil
	}
	return s.rewrite(number, []string{relPath})
}

func (s *Store) deleteLedger(number int, dir, relPath, metaDir string, rewriteHistory bool) error {
	defer s.lockLedger()()
	if rewriteHistory {
		if err := s.unrelatedTrackedChanges(metaDir, []string{relPath}); err != nil {
			return err
		}
	}
	s.runGit(metaDir, "rm", "-r", "--ignore-unmatch", "--", relPath)
	if isDir(dir) && safedelete.RemoveTreeUnder(s.pullsDir(), dir) != nil {
		return errorString("Could not remove pull request folder.")
	}
	if r := s.runGit(metaDir, "commit", "-m", "pull #"+strconv.Itoa(number)+": deleted", "--", relPath); !r.ok {
		if !strings.Contains(r.err, "nothing to commit") && r.err != "" {
			return errorString("git commit failed: " + r.err)
		}
	}
	s.removeMaterializedPullRefs(number)
	return nil
}

func isDir(path string) bool {
	info, err := os.Stat(path)
	return err == nil && info.IsDir()
}

func (s *Store) DeletePulls(numbers []int, rewriteHistory bool) (deleted, missing []int, err error) {
	deleted, missing = []int{}, []int{}
	if !s.CanWrite() {
		return deleted, missing, errorString(readOnlyMessage)
	}
	present := []int{}
	relPaths := []string{}
	for _, number := range numbers {
		if containsInt(present, number) {
			continue
		}
		if !exists(s.pullDir(number)) {
			s.removeMaterializedPullRefs(number)
			missing = append(missing, number)
			continue
		}
		present = append(present, number)
		relPaths = append(relPaths, ".forkmesh/pulls/"+strconv.Itoa(number))
	}
	if len(present) == 0 {
		return deleted, missing, nil
	}
	metaDir := s.metaDir()
	unlock := s.lockLedger()
	defer func() {
		if unlock != nil {
			unlock()
		}
	}()
	if rewriteHistory {
		if err := s.unrelatedTrackedChanges(metaDir, relPaths); err != nil {
			return deleted, missing, err
		}
	}
	s.runGit(metaDir, append([]string{"rm", "-r", "--ignore-unmatch", "--"}, relPaths...)...)
	for _, number := range present {
		dir := s.pullDir(number)
		if isDir(dir) && safedelete.RemoveTreeUnder(s.pullsDir(), dir) != nil {
			return deleted, missing, errorString("Could not remove the folder of pull request #" + strconv.Itoa(number) + ".")
		}
	}
	labels := make([]string, 0, len(present))
	for _, number := range present {
		labels = append(labels, "#"+strconv.Itoa(number))
	}
	commitArgs := append([]string{"commit", "-m", "pulls: deleted " + strings.Join(labels, ", "), "--"}, relPaths...)
	if r := s.runGit(metaDir, commitArgs...); !r.ok {
		if !strings.Contains(r.err, "nothing to commit") && r.err != "" {
			return deleted, missing, errorString("git commit failed: " + r.err)
		}
	}
	for _, number := range present {
		s.removeMaterializedPullRefs(number)
	}
	deleted = present
	unlock()
	unlock = nil
	if !rewriteHistory {
		return deleted, missing, nil
	}
	return deleted, missing, s.rewrite(present[0], relPaths)
}

func containsInt(list []int, value int) bool {
	for _, item := range list {
		if item == value {
			return true
		}
	}
	return false
}

func (s *Store) PrefetchForeignLedger(remoteURL string) (bool, error) {
	if !s.CanWrite() || strings.TrimSpace(remoteURL) == "" {
		return false, nil
	}
	dir := s.MetaWorkTree()
	if dir == "" {
		return false, nil
	}
	r := s.runGitTimeout(dir, []string{"fetch", "--no-tags", "--quiet", remoteURL, "refs/heads/forkmesh/pulls"}, gitRewriteTimeout)
	if !r.ok {
		return false, errorString("ledger fetch failed: " + r.err)
	}
	return true, nil
}

var (
	pullEntryPath = regexp.MustCompile(`^\.forkmesh/pulls/(\d+)/[^/]+$`)
	sigLine       = regexp.MustCompile(`(?m)^sig:\s*(\S+)`)
)

func signatureOf(text string) string {
	if m := sigLine.FindStringSubmatch(text); m != nil {
		return m[1]
	}
	return ""
}

func (s *Store) AdoptForeignPulls(remoteURL string, fetchFirst bool) ([]int, error) {
	adopted := []int{}
	if !s.CanWrite() || strings.TrimSpace(remoteURL) == "" {
		return adopted, nil
	}
	dir := s.MetaWorkTree()
	if dir == "" {
		return adopted, nil
	}
	defer s.lockLedger()()
	pending := s.runGit(dir, "status", "--porcelain", "--", ".forkmesh/pulls")
	if !pending.ok || strings.TrimSpace(pending.out()) != "" {
		return adopted, nil
	}
	if fetchFirst {
		if ok, err := s.PrefetchForeignLedger(remoteURL); !ok {
			return adopted, err
		}
	}
	tip := s.runGit(dir, "rev-parse", "--verify", "-q", "FETCH_HEAD^{commit}")
	if !tip.ok || trimmed(tip.output) == "" {
		return adopted, nil
	}
	foreign := trimmed(tip.output)
	if s.runGit(dir, "merge-base", "--is-ancestor", foreign, "HEAD").ok {
		return adopted, nil
	}
	revs := s.runGit(dir, "rev-list", "--reverse", "HEAD.."+foreign)
	if !revs.ok {
		return adopted, nil
	}
	var adoptErr error
	allConsumed := true
	for _, line := range strings.Split(revs.out(), "\n") {
		commit := strings.TrimSpace(line)
		if commit == "" {
			continue
		}
		listing := s.runGit(dir, "diff-tree", "--no-commit-id", "--name-status", "-r", "--root", commit)
		if !listing.ok {
			allConsumed = false
			continue
		}
		eligible := false
		numbers := []int{}
		for _, rawRow := range strings.Split(listing.out(), "\n") {
			row := strings.TrimSpace(qtFromUtf8([]byte(rawRow)))
			if row == "" {
				continue
			}
			tab := strings.IndexByte(row, '\t')
			status, path := "", ""
			if tab > 0 {
				status, path = row[:tab], row[tab+1:]
			}
			m := pullEntryPath.FindStringSubmatch(path)
			if status != "A" || m == nil {
				eligible = false
				numbers = numbers[:0]
				break
			}
			eligible = true
			number, _ := qtToInt(m[1])
			if !containsInt(numbers, number) {
				numbers = append(numbers, number)
			}
		}
		if !eligible || len(numbers) == 0 {
			allConsumed = false
			continue
		}
		present := 0
		collision := false
		for _, number := range numbers {
			local := s.pullDir(number) + "/pull.md"
			if !exists(local) {
				continue
			}
			localSig := ""
			if raw, err := os.ReadFile(local); err == nil {
				localSig = signatureOf(qtFromUtf8(raw))
			}
			foreignRecord := s.runGit(dir, "show", commit+":.forkmesh/pulls/"+strconv.Itoa(number)+"/pull.md")
			foreignSig := signatureOf(qtFromUtf8(foreignRecord.output))
			if localSig != "" && localSig == foreignSig {
				present++
			} else {
				collision = true
			}
		}
		if collision || (present > 0 && present < len(numbers)) {
			allConsumed = false
			continue
		}
		if present == len(numbers) {
			continue
		}
		if r := s.runGitTimeout(dir, []string{"cherry-pick", commit}, gitRewriteTimeout); !r.ok {
			s.runGit(dir, "cherry-pick", "--abort")
			adoptErr = errorString("could not adopt " + leftRunes(commit, 12) + ": " + r.err)
			allConsumed = false
			break
		}
		for _, number := range numbers {
			adopted = append(adopted, number)
			_ = s.materializePullMetadataRef(number)
		}
	}
	if allConsumed {
		r := s.runGitTimeout(dir, []string{"merge", "-s", "ours", "--no-edit", "-m", "pull ledger: consume mirror-drained history", foreign}, gitRewriteTimeout)
		if !r.ok {
			s.runGit(dir, "merge", "--abort")
			adoptErr = errorString("could not record consumed ledger history: " + r.err)
		}
	}
	sort.Ints(adopted)
	return adopted, adoptErr
}

type ReconcileResult struct {
	Flipped []int
	Rewards []int
	Error   string
}

func (s *Store) ReconcileMergedPulls() ReconcileResult {
	result := ReconcileResult{Flipped: []int{}, Rewards: []int{}}
	if !s.CanWrite() {
		return result
	}
	setError := func(err error) {
		if err != nil {
			result.Error = err.Error()
		}
	}
	defaultBranch, defaultTip := "", ""
	for _, candidate := range []string{"main", "master"} {
		if r := s.runGit(s.WorkTree, "rev-parse", "--verify", "-q", candidate+"^{commit}"); r.ok && trimmed(r.output) != "" {
			defaultBranch, defaultTip = candidate, trimmed(r.output)
			break
		}
	}
	for _, loaded := range s.LoadAll(false) {
		if loaded.Status != "open" {
			if loaded.Status != "merged" {
				continue
			}
			candidate := *loaded
			if !AdvanceRewardForMerge(&candidate) {
				continue
			}
			if number, ok := s.advanceMergedReward(loaded.Number, setError); ok {
				result.Rewards = append(result.Rewards, number)
			}
			continue
		}
		headSha := ""
		if r := s.runGit(s.WorkTree, "rev-parse", "--verify", "-q", "refs/pr/"+strconv.Itoa(loaded.Number)+"/head^{commit}"); r.ok && trimmed(r.output) != "" {
			headSha = trimmed(r.output)
		}
		if headSha == "" {
			headSha = s.reachableHeadCommit(loaded.Head)
		}
		if headSha == "" && !strings.Contains(loaded.Head, ":") {
			if r := s.runGit(s.WorkTree, "rev-parse", "--verify", "-q", "refs/remotes/origin/"+loaded.Head+"^{commit}"); r.ok && trimmed(r.output) != "" {
				headSha = trimmed(r.output)
			}
		}
		if headSha == "" && validCommitOid(loaded.CreationHeadOid) &&
			s.runGit(s.WorkTree, "rev-parse", "--verify", "-q", loaded.CreationHeadOid+"^{commit}").ok {
			headSha = loaded.CreationHeadOid
		}
		if headSha == "" {
			continue
		}
		landedTip := ""
		if base := s.runGit(s.WorkTree, "rev-parse", "--verify", "-q", loaded.Base+"^{commit}"); base.ok && trimmed(base.output) != "" &&
			s.runGit(s.WorkTree, "merge-base", "--is-ancestor", headSha, trimmed(base.output)).ok {
			landedTip = trimmed(base.output)
		} else if defaultBranch != "" && loaded.Base != defaultBranch &&
			s.runGit(s.WorkTree, "merge-base", "--is-ancestor", headSha, defaultTip).ok {
			landedTip = defaultTip
		}
		if landedTip == "" {
			continue
		}
		flipped, rewardAdvanced := s.flipLanded(loaded.Number, landedTip, headSha, setError)
		if !flipped {
			continue
		}
		if rewardAdvanced {
			result.Rewards = append(result.Rewards, loaded.Number)
		}
		result.Flipped = append(result.Flipped, loaded.Number)
	}
	return result
}

func (s *Store) advanceMergedReward(number int, setError func(error)) (int, bool) {
	defer s.lockLedger()()
	stale, ok := s.ReadPull(number, true)
	if !ok || stale.Status != "merged" || !AdvanceRewardForMerge(stale) {
		return 0, false
	}
	if err := s.writePull(stale); err != nil {
		setError(err)
		return 0, false
	}
	if err := s.commit("pull #" + strconv.Itoa(stale.Number) + ": reward awaiting payout"); err != nil {
		setError(err)
		return 0, false
	}
	return stale.Number, true
}

func (s *Store) flipLanded(number int, landedTip, headSha string, setError func(error)) (bool, bool) {
	defer s.lockLedger()()
	pr, ok := s.ReadPull(number, true)
	if !ok || pr.Status != "open" {
		return false, false
	}
	pr.Status = "merged"
	rewardAdvanced := AdvanceRewardForMerge(pr)
	pr.MergeBase = landedTip
	pr.MergeHead = headSha
	if err := s.writePull(pr); err != nil {
		setError(err)
		return false, false
	}
	if err := s.commit("pull #" + strconv.Itoa(pr.Number) + ": merged"); err != nil {
		setError(err)
		return false, false
	}
	return true, rewardAdvanced
}

const (
	BranchLocal   = "local"
	BranchMirror  = "mirror"
	BranchPull    = "pull"
	BranchMissing = "missing"
)

func (s *Store) EnsureLocalBranch(branch string) (string, int, error) {
	name := strings.TrimSpace(branch)
	ref := "refs/heads/" + name
	if name == "" || strings.HasPrefix(name, "-") || !s.runGit(s.WorkTree, "check-ref-format", ref).ok {
		return BranchMissing, 0, errorString("\"" + branch + "\" is not a valid branch name.")
	}
	if s.resolvedCommitOid(s.WorkTree, ref) != "" {
		return BranchLocal, 0, nil
	}
	adopt := func(sha string) bool {
		if sha == "" {
			return false
		}
		if s.runGit(s.WorkTree, "update-ref", ref, sha, strings.Repeat("0", len(sha))).ok {
			return true
		}
		return s.resolvedCommitOid(s.WorkTree, ref) == sha
	}
	if s.Mirror != "" && s.runGit(s.WorkTree, "fetch", "--no-tags", "--quiet", s.Mirror, ref).ok &&
		adopt(s.resolvedCommitOid(s.WorkTree, "FETCH_HEAD")) {
		return BranchMirror, 0, nil
	}
	var open, closed *Pull
	for _, pr := range s.LoadAll(true) {
		if strings.TrimSpace(pr.Head) != name {
			continue
		}
		if pr.Status == "open" {
			if open == nil || pr.Number > open.Number {
				open = pr
			}
		} else if closed == nil || pr.Number > closed.Number {
			closed = pr
		}
	}
	if open != nil {
		if err := s.materializePullRef(open); err != nil {
			return BranchMissing, 0, errorString("Pull request #" + strconv.Itoa(open.Number) + " carries " + name +
				", but its commits could not be rebuilt: " + err.Error())
		}
		if s.resolvedCommitOid(s.WorkTree, ref) != "" {
			return BranchPull, open.Number, nil
		}
		if adopt(s.resolvedCommitOid(s.WorkTree, "refs/pr/"+strconv.Itoa(open.Number)+"/head")) {
			return BranchPull, open.Number, nil
		}
		base := strings.TrimSpace(open.Base)
		if base == "" {
			base = "the base branch"
		}
		return BranchMissing, 0, errorString("Pull request #" + strconv.Itoa(open.Number) + " carries " + name +
			", but its stored commits no longer apply to " + base + " or the base they were written against. Ask its author to update it from " + base + ".")
	}
	if closed != nil {
		return BranchMissing, 0, errorString(name + " is not in this checkout or on the mirror, and pull request #" +
			strconv.Itoa(closed.Number) + " that carried it is already " + closed.Status + ".")
	}
	return BranchMissing, 0, errorString(name + " is not in this checkout, on the mirror, or in any open pull request, so there is nothing to merge. " +
		"It may already have been merged and deleted; otherwise sync the repository, or ask whoever made the branch to push it again.")
}

func (s *Store) BaseTip() string {
	if !s.CanWrite() {
		return ""
	}
	r := s.runGit(s.WorkTree, "rev-parse", "HEAD")
	if !r.ok {
		return ""
	}
	return trimmed(r.output)
}

func (s *Store) MaterializePullRef(pr *Pull) error { return s.materializePullRef(pr) }

var (
	ledgerMu    sync.Mutex
	ledgerLocks = map[string]*sync.Mutex{}
)

func (s *Store) lockLedger() func() {
	key := WorkTreeKey(s.WorkTree)
	ledgerMu.Lock()
	lock, ok := ledgerLocks[key]
	if !ok {
		lock = &sync.Mutex{}
		ledgerLocks[key] = lock
	}
	ledgerMu.Unlock()
	lock.Lock()
	return lock.Unlock
}
