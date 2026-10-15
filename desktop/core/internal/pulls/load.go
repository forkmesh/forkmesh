package pulls

import (
	"regexp"
	"sort"
	"strconv"
	"strings"
)

var (
	metadataRefPattern = regexp.MustCompile(`^refs/heads/forkmesh/pull/([1-9][0-9]*)$`)
	deletedPathPattern = regexp.MustCompile(`^\.forkmesh/pulls/([1-9][0-9]*)/pull\.md$`)
)

func (s *Store) LoadAll(derive bool) []*Pull {
	list, _ := s.LoadAllStamped(derive)
	return list
}

func (s *Store) CurrentLoadStamp() string {
	if !s.CanWrite() {
		return ""
	}
	return pullLoadStamp(s.WorkTree, s.Mirror, s.pullsDir())
}

func (s *Store) LoadAllStamped(derive bool) ([]*Pull, string) {
	writable := s.CanWrite()
	cacheKey := s.WorkTree + "\x1f" + s.Mirror + "\x1f" + map[bool]string{true: "w", false: "r"}[writable] + map[bool]string{true: "d", false: "n"}[derive]
	cacheStamp := ""
	if writable {
		cacheStamp = pullLoadStamp(s.WorkTree, s.Mirror, s.pullsDir())
	}
	if cacheStamp != "" {
		if cached, ok := loadCacheGet(cacheKey, cacheStamp); ok {
			return cached, cacheStamp
		}
	}
	complete := true
	repo := s.WorkTree
	if strings.TrimSpace(repo) == "" {
		repo = s.Mirror
	}
	var loaded []*Pull
	if !writable {
		loaded = s.loadFromMirror(s.Mirror, "", derive)
	} else {
		for _, entry := range listDir(s.pullsDir(), true) {
			number, ok := qtToInt(entry)
			if !ok {
				continue
			}
			if pr, ok := s.ReadPull(number, derive); ok {
				loaded = append(loaded, pr)
			} else {
				complete = false
			}
		}
	}
	byNumber := map[int]*Pull{}
	for _, pr := range loaded {
		byNumber[pr.Number] = pr
	}
	refsByNumber := map[int]string{}
	snapshotTimes := map[int]int64{}
	if strings.TrimSpace(repo) != "" {
		listed := s.runGit(repo, "for-each-ref", "--format=%(refname) %(committerdate:unix)", "refs/heads/forkmesh/pull/")
		if !listed.ok {
			complete = false
		}
		if listed.ok {
			for _, raw := range strings.Split(listed.out(), "\n") {
				line := strings.TrimSpace(raw)
				space := strings.IndexByte(line, ' ')
				ref := line
				if space >= 0 {
					ref = line[:space]
				}
				m := metadataRefPattern.FindStringSubmatch(ref)
				if m == nil {
					continue
				}
				number, _ := qtToInt(m[1])
				refsByNumber[number] = ref
				if space >= 0 {
					snapshotTimes[number] = qtToLongLong(line[space+1:])
				}
			}
		}
	}
	s.dropDeletedPullMetadataRefs(repo, byNumber, refsByNumber, snapshotTimes)
	if s.CanWrite() {
		for _, pull := range loaded {
			if _, ok := refsByNumber[pull.Number]; ok {
				continue
			}
			if s.materializePullMetadataRef(pull.Number) == nil {
				refsByNumber[pull.Number] = pullMetadataBranchRef(pull.Number)
			}
		}
	}
	numbers := make([]int, 0, len(refsByNumber))
	for number := range refsByNumber {
		numbers = append(numbers, number)
	}
	sort.Ints(numbers)
	var batchInput strings.Builder
	for _, number := range numbers {
		path := ":.forkmesh/pulls/" + strconv.Itoa(number)
		batchInput.WriteString(refsByNumber[number] + path + "\n")
		batchInput.WriteString("refs/heads/forkmesh/pulls" + path + "\n")
	}
	compared := true
	var treeLines []string
	if batchInput.Len() > 0 {
		r := s.runGitInput(repo, []byte(batchInput.String()), "cat-file", "--batch-check")
		compared = r.ok
		if !compared {
			complete = false
		}
		treeLines = strings.Split(r.out(), "\n")
	}
	objectID := func(line string) string {
		if space := strings.IndexByte(line, ' '); space >= 0 {
			return line[:space]
		}
		return line
	}
	refView := &Store{Mirror: repo, IdentityValid: s.IdentityValid, DataRoot: s.DataRoot, Runner: s.Runner, Ctx: s.Ctx, Env: s.Env}
	treeLine := 0
	for _, number := range numbers {
		var isolatedTree, aggregateTree string
		if compared && treeLine+1 < len(treeLines) {
			isolatedTree = objectID(treeLines[treeLine])
			aggregateTree = objectID(treeLines[treeLine+1])
		}
		treeLine += 2
		if _, ok := byNumber[number]; ok && validCommitOid(isolatedTree) && isolatedTree == aggregateTree {
			continue
		}
		ref := refsByNumber[number]
		if ref == "" {
			continue
		}
		found := false
		for _, pull := range refView.loadFromMirror(repo, ref, derive) {
			if pull.Number != number {
				continue
			}
			found = true
			if ledger, ok := byNumber[number]; ok {
				adoptLedgerMerge(pull, ledger)
			}
			byNumber[number] = pull
		}
		if !found {
			complete = false
		}
	}
	out := make([]*Pull, 0, len(byNumber))
	for _, pull := range byNumber {
		out = append(out, pull)
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Number < out[j].Number })
	if cacheStamp != "" && complete {
		loadCachePut(cacheKey, cacheStamp, out)
		return out, cacheStamp
	}
	return out, ""
}

func adoptLedgerMerge(isolated, ledger *Pull) {
	if ledger.Status != "merged" || isolated.Status == "merged" {
		return
	}
	isolated.Status = ledger.Status
	isolated.MergeBase = ledger.MergeBase
	isolated.MergeHead = ledger.MergeHead
}

func (s *Store) dropDeletedPullMetadataRefs(repo string, byNumber map[int]*Pull, refsByNumber map[int]string, snapshotTimes map[int]int64) {
	numbers := []int{}
	for number := range refsByNumber {
		if _, ok := byNumber[number]; !ok {
			numbers = append(numbers, number)
		}
	}
	if len(numbers) == 0 || strings.TrimSpace(repo) == "" {
		return
	}
	sort.Ints(numbers)
	args := []string{"log", "--no-renames", "--diff-filter=D", "--format=%x01%ct", "--name-only", "refs/heads/forkmesh/pulls", "--"}
	for _, number := range numbers {
		args = append(args, ".forkmesh/pulls/"+strconv.Itoa(number)+"/pull.md")
	}
	r := s.runGitTimeout(repo, args, gitTimeout)
	if !r.ok {
		return
	}
	deletedAt := map[int]int64{}
	var commitTime int64
	for _, raw := range strings.Split(r.out(), "\n") {
		line := strings.TrimSpace(raw)
		if strings.HasPrefix(line, "\x01") {
			commitTime = qtToLongLong(line[1:])
			continue
		}
		if m := deletedPathPattern.FindStringSubmatch(line); m != nil {
			number, _ := qtToInt(m[1])
			if _, seen := deletedAt[number]; !seen {
				deletedAt[number] = commitTime
			}
		}
	}
	for number, at := range deletedAt {
		snapshot := snapshotTimes[number]
		if snapshot > 0 && at > 0 && snapshot <= at {
			delete(refsByNumber, number)
		}
	}
}

func (s *Store) mirrorRef(mirror string) string {
	if r := s.runGit(mirror, "rev-parse", "--verify", "-q", "refs/heads/forkmesh/pulls^{commit}"); r.ok && trimmed(r.output) != "" {
		return "refs/heads/forkmesh/pulls"
	}
	if r := s.runGit(mirror, "rev-parse", "--verify", "-q", "HEAD"); r.ok && trimmed(r.output) != "" {
		return "HEAD"
	}
	if r := s.runGit(mirror, "for-each-ref", "--format=%(refname)", "--count=1", "refs/heads/"); r.ok {
		if ref := trimmed(r.output); ref != "" {
			return ref
		}
	}
	return ""
}

func (s *Store) loadFromMirror(mirror, ref string, derive bool) []*Pull {
	pulls := []*Pull{}
	if mirror == "" {
		return pulls
	}
	if ref == "" {
		ref = s.mirrorRef(mirror)
	}
	if ref == "" {
		return pulls
	}
	listing := s.runGit(mirror, "ls-tree", ref, ".forkmesh/pulls/")
	if !listing.ok {
		return pulls
	}
	readBlob := func(path string) ([]byte, bool) {
		r := s.runGit(mirror, "show", ref+":"+path)
		if !r.ok {
			return nil, false
		}
		return r.output, true
	}
	bases := []string{}
	for _, line := range strings.Split(listing.out(), "\n") {
		if line == "" {
			continue
		}
		tab := strings.IndexByte(line, '\t')
		if tab < 0 || !strings.Contains(line, " tree ") {
			continue
		}
		path := line[tab+1:]
		if slash := strings.LastIndexByte(path, '/'); slash >= 0 {
			path = path[slash+1:]
		}
		bases = append(bases, path)
	}
	for _, base := range bases {
		number, ok := qtToInt(base)
		if !ok {
			continue
		}
		md, ok := readBlob(".forkmesh/pulls/" + base + "/pull.md")
		if !ok {
			continue
		}
		fm := parseFrontMatter(md)
		pr := &Pull{
			Number:          number,
			Title:           fm.get("title"),
			Base:            fm.get("base"),
			Head:            fm.get("head"),
			Status:          "open",
			Ts:              fm.num("ts"),
			Author:          fm.get("author"),
			AuthorName:      fm.get("authorName"),
			Sig:             fm.get("sig"),
			BranchBacked:    fm.get("derive") == "branch",
			CreationBaseOid: fm.get("creationBaseOid"),
			CreationHeadOid: fm.get("creationHeadOid"),
			MergeBase:       fm.get("mergeBase"),
			MergeHead:       fm.get("mergeHead"),
			Client:          fm.get("client"),
			ClientSig:       fm.get("clientSig"),
			Description:     fm.body,
		}
		if fm.has("status") {
			pr.Status = fm.get("status")
		}
		derived := false
		if derive && pr.BranchBacked && pr.Status == "open" {
			deriveBase, deriveHead := pr.Base, pr.Head
			if pr.MergeBase != "" && pr.MergeHead != "" {
				deriveBase, deriveHead = pr.MergeBase, pr.MergeHead
			}
			if patch, commits, ok := s.deriveFromRefs(mirror, deriveBase, deriveHead, true); ok {
				derived = true
				pr.Patch, pr.Commits = patch, commits
				pr.PatchHeadOid = s.resolvedCommitOid(mirror, deriveHead)
			}
		}
		if pr.PatchHeadOid == "" {
			pr.PatchHeadOid = pr.CreationHeadOid
		}
		if derive && pr.Status == "open" && !derived {
			if patch, ok := readBlob(".forkmesh/pulls/" + base + "/changes.patch"); ok {
				pr.Patch = qtFromUtf8(patch)
			}
			if mbox, ok := readBlob(".forkmesh/pulls/" + base + "/commits.mbox"); ok {
				pr.Commits = qtFromUtf8(mbox)
			}
		}
		ComputeStats(pr)
		names := []string{}
		if dirListing := s.runGit(mirror, "ls-tree", ref, ".forkmesh/pulls/"+base+"/"); dirListing.ok {
			for _, l := range strings.Split(dirListing.out(), "\n") {
				if l == "" {
					continue
				}
				t := strings.IndexByte(l, '\t')
				if t < 0 || !strings.Contains(l, " blob ") {
					continue
				}
				name := l[t+1:]
				if slash := strings.LastIndexByte(name, '/'); slash >= 0 {
					name = name[slash+1:]
				}
				if eventFilePattern.MatchString(name) {
					names = append(names, name)
				}
			}
			sort.Strings(names)
		}
		for _, name := range names {
			raw, ok := readBlob(".forkmesh/pulls/" + base + "/" + name)
			if !ok {
				continue
			}
			pr.Events = append(pr.Events, eventFromFrontMatter(parseFrontMatter(raw)))
		}
		pulls = append(pulls, pr)
	}
	sort.SliceStable(pulls, func(i, j int) bool { return pulls[i].Number < pulls[j].Number })
	return pulls
}
