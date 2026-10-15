package actions

import (
	"context"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"time"

	"forkmesh.com/desktop-core/internal/git"
	"forkmesh.com/desktop-core/internal/pulls"
	"forkmesh.com/desktop-core/internal/rpc"
)

const (
	pullCheckScanLimit        = 25
	pullCheckQueueLimit       = 4
	pullCheckMaterializeLimit = 2
	pullCheckRescanFloorMs    = 60 * 1000
	pullCheckGitTimeout       = 20 * time.Second
	pullCheckPassTimeout      = 10 * time.Minute
)

var badObjectRefPattern = regexp.MustCompile(`\bbad object (refs/[^\s'"]+)`)

type pullCheckState struct {
	inFlight    bool
	fingerprint map[string]string
	scanAt      map[string]int64
	noHead      map[string]bool
	queued      map[string]string
}

func newPullCheckState() *pullCheckState {
	return &pullCheckState{fingerprint: map[string]string{}, scanAt: map[string]int64{}, noHead: map[string]bool{}, queued: map[string]string{}}
}

type pullCheckRequest struct {
	Owner    string   `json:"owner"`
	Name     string   `json:"name"`
	Trigger  Trigger  `json:"trigger"`
	Force    bool     `json:"force"`
	Numbers  []int    `json:"numbers"`
	Trusted  []string `json:"trusted"`
	Identity bool     `json:"identity"`
}

type pullCheckCandidate struct {
	number          int
	pull            *pulls.Pull
	head            string
	base            string
	branchBacked    bool
	author          string
	firstTimeAuthor bool
	mayMaterialize  bool
}

type pullCheckHead struct {
	number          int
	commit          string
	ref             string
	author          string
	base            string
	firstTimeAuthor bool
	published       bool
	errorText       string
}

func pullAuthorIdentity(pr *pulls.Pull) string {
	if key := qtTrim(pr.Author); key != "" {
		return key
	}
	return strings.ToLower(qtTrim(pr.AuthorName))
}

func PullAuthorEstablished(pr *pulls.Pull, history []*pulls.Pull, trusted []string) bool {
	identity := pullAuthorIdentity(pr)
	if identity == "" {
		return false
	}
	locallyAuthored := qtTrim(pr.Author) == ""
	for _, entry := range trusted {
		candidate := qtTrim(entry)
		if candidate == "" {
			continue
		}
		if candidate == identity {
			return true
		}
		if locallyAuthored && strings.EqualFold(candidate, qtTrim(pr.AuthorName)) {
			return true
		}
	}
	for _, other := range history {
		if other.Number == pr.Number || other.Status != "merged" || pullAuthorIdentity(other) != identity {
			continue
		}
		return true
	}
	return false
}

func newPullCheckCandidate(pr *pulls.Pull, history []*pulls.Pull, trusted []string) pullCheckCandidate {
	head := qtTrim(pr.Head)
	if separator := strings.IndexByte(head, ':'); separator > 0 {
		head = qtTrim(head[separator+1:])
	}
	author := qtTrim(pr.AuthorName)
	if author == "" {
		author = qtLeft(pr.Author, 10)
	}
	return pullCheckCandidate{
		number:          pr.Number,
		pull:            pr,
		head:            head,
		base:            qtTrim(pr.Base),
		branchBacked:    pr.BranchBacked,
		author:          author,
		firstTimeAuthor: !PullAuthorEstablished(pr, history, trusted),
	}
}

func pullCheckKey(owner, name string, number int) string {
	return owner + "\x1f" + name + "\x1f" + strconv.Itoa(number)
}

func (q *actionQueue) pullRecord(owner, name string) (RepoEntry, bool) {
	for _, repo := range q.snap.Repos {
		if repo.Owner == owner && repo.Name == name && !repo.Preview {
			return repo, true
		}
	}
	return RepoEntry{}, false
}

func (q *actionQueue) pullStore(repo RepoEntry, identity bool) *pulls.Store {
	workTree := qtTrim(repo.WritablePath)
	if workTree == "" {
		workTree = qtTrim(repo.LocalPath)
	}
	store := &pulls.Store{WorkTree: workTree, Mirror: qtTrim(repo.MirrorPath), IdentityValid: identity, Runner: q.m.gitRunner()}
	if q.m.core != nil {
		store.DataRoot = q.m.core.Layout.Data
	}
	if q.m.PullsDataRoot != "" {
		store.DataRoot = q.m.PullsDataRoot
	}
	store.Warn = func(text string) { q.m.logf("pulls: %s", text) }
	return store
}

func (m *Module) gitRunner() *git.Runner {
	if m.GitRunner != nil {
		return m.GitRunner
	}
	return git.Default
}

func (q *actionQueue) requestPullChecks(request pullCheckRequest, done func(any, error)) {
	repo, ok := q.pullRecord(request.Owner, request.Name)
	if !ok {
		done(nil, rpc.Fail("unknown_repo"))
		return
	}
	trigger := request.Trigger
	if trigger == "" {
		trigger = TriggerPullRequest
	}
	if trigger != TriggerPullRequest && trigger != TriggerPullRequestManual {
		done(nil, rpc.Fail("bad_request"))
		return
	}
	request.Trigger = trigger
	if !request.Force && q.pullChecks.inFlight {
		done(map[string]any{"accepted": false, "reason": "in_flight"}, nil)
		return
	}
	if !repo.Enabled || repo.ExternallyManaged {
		done(map[string]any{"accepted": false, "reason": "actions_off"}, nil)
		return
	}
	store := q.pullStore(repo, request.Identity)
	if store.WorkTree == "" || store.Mirror == "" || !dirExists(store.Mirror) {
		done(map[string]any{"accepted": false, "reason": "no_mirror"}, nil)
		return
	}
	if !request.Force && !store.CanWrite() {
		done(map[string]any{"accepted": false, "reason": "not_writable"}, nil)
		return
	}
	if !request.Force {
		q.pullChecks.inFlight = true
	}
	q.m.pullPasses.Add(1)
	done(map[string]any{"accepted": true}, nil)
	go func() {
		ctx, cancel := context.WithTimeout(context.Background(), pullCheckPassTimeout)
		store.Ctx = ctx
		loaded := store.LoadAll(true)
		q.post(func() {
			candidates := q.pullCheckCandidates(repo, request, loaded)
			if len(candidates) == 0 {
				cancel()
				q.finishPullPass(request)
				return
			}
			go func() {
				defer cancel()
				heads := resolvePullCheckHeads(ctx, q.m.gitRunner(), store, candidates)
				q.post(func() {
					q.finishPullPass(request)
					q.applyPullCheckHeads(request.Owner, request.Name, store.Mirror, request.Trigger, request.Force, heads)
				})
			}()
		})
	}()
}

func (q *actionQueue) finishPullPass(request pullCheckRequest) {
	if !request.Force {
		q.pullChecks.inFlight = false
	}
	q.m.pullPasses.Add(-1)
}

func (q *actionQueue) pullCheckCandidates(repo RepoEntry, request pullCheckRequest, loaded []*pulls.Pull) []pullCheckCandidate {
	trusted := append([]string{}, request.Trusted...)
	trusted = append(trusted, repo.Owner)
	state := q.pullChecks
	var candidates []pullCheckCandidate
	if request.Force {
		for _, number := range request.Numbers {
			for _, pr := range loaded {
				if pr.Number == number {
					candidates = append(candidates, newPullCheckCandidate(pr, loaded, trusted))
					break
				}
			}
		}
		for index := range candidates {
			candidates[index].mayMaterialize = true
		}
		return candidates
	}
	var fingerprint strings.Builder
	for _, pr := range loaded {
		if pr.Number <= 0 || pr.Status != "open" {
			continue
		}
		candidates = append(candidates, newPullCheckCandidate(pr, loaded, trusted))
		fingerprint.WriteString(strconv.Itoa(pr.Number) + ":" + pr.Head + "\x1f")
	}
	if len(candidates) == 0 {
		return nil
	}
	scanKey := strings.ToLower(qtTrim(repo.Owner)) + "/" + strings.ToLower(qtTrim(repo.Name))
	now := nowMs()
	print := fingerprint.String()
	previous, seen := state.fingerprint[scanKey]
	if seen && previous == print && now-state.scanAt[scanKey] < pullCheckRescanFloorMs {
		return nil
	}
	if !seen || previous != print {
		prefix := repo.Owner + "\x1f" + repo.Name + "\x1f"
		for key := range state.noHead {
			if strings.HasPrefix(key, prefix) {
				delete(state.noHead, key)
			}
		}
	}
	state.fingerprint[scanKey] = print
	state.scanAt[scanKey] = now
	for index := range candidates {
		candidates[index].mayMaterialize = !state.noHead[pullCheckKey(repo.Owner, repo.Name, candidates[index].number)]
	}
	sort.SliceStable(candidates, func(i, j int) bool { return candidates[i].number > candidates[j].number })
	if len(candidates) > pullCheckScanLimit {
		candidates = candidates[:pullCheckScanLimit]
	}
	return candidates
}

type pullGit struct {
	ctx    context.Context
	runner *git.Runner
}

func (g pullGit) run(dir string, args ...string) (string, string, string) {
	result := g.runner.Run(g.ctx, dir, args, git.Options{Timeout: pullCheckGitTimeout, NoLane: true})
	if result.TimedOut {
		return "", "git did not finish", ""
	}
	if !result.OK {
		stderr := qtTrim(result.Stderr)
		return "", qtRight(stderr, 200), stderr
	}
	return qtTrim(qtFromUtf8(result.Output)), "", ""
}

func (g pullGit) out(dir string, args ...string) string {
	out, _, _ := g.run(dir, args...)
	return out
}

func (g pullGit) resolve(dir, ref string) string {
	return g.out(dir, "rev-parse", "--verify", "--quiet", "--end-of-options", ref+"^{commit}")
}

func (g pullGit) tree(dir, commit string) string {
	return g.out(dir, "rev-parse", "--verify", "--quiet", "--end-of-options", commit+"^{tree}")
}

func (g pullGit) succeeds(dir string, args ...string) bool {
	_, failure, _ := g.run(dir, args...)
	return failure == ""
}

func refsNamedAsBadObjects(gitErrors string) []string {
	var refs []string
	for _, match := range badObjectRefPattern.FindAllStringSubmatch(gitErrors, -1) {
		ref := match[1]
		if !strings.Contains(ref, "..") && !containsString(refs, ref) {
			refs = append(refs, ref)
		}
	}
	return refs
}

func dropUnreadableRefs(g pullGit, gitDir, gitErrors string) []string {
	var dropped []string
	for _, ref := range refsNamedAsBadObjects(gitErrors) {
		target, failure, _ := g.run(gitDir, "rev-parse", "--verify", "--quiet", "--end-of-options", ref)
		if failure != "" || target == "" || g.succeeds(gitDir, "cat-file", "-t", target) {
			continue
		}
		if g.succeeds(gitDir, "update-ref", "-d", ref, target) {
			dropped = append(dropped, ref)
		}
	}
	return dropped
}

func resolvePullCheckHeads(ctx context.Context, runner *git.Runner, store *pulls.Store, candidates []pullCheckCandidate) []pullCheckHead {
	g := pullGit{ctx: ctx, runner: runner}
	workTree, mirror := store.WorkTree, store.Mirror
	var heads []pullCheckHead
	materialized := 0
	for _, candidate := range candidates {
		head := pullCheckHead{number: candidate.number, author: candidate.author, base: candidate.base,
			firstTimeAuthor: candidate.firstTimeAuthor, ref: "refs/heads/pr/" + strconv.Itoa(candidate.number)}
		refs := []string{"refs/pr/" + strconv.Itoa(candidate.number) + "/head", head.ref}
		if candidate.branchBacked && candidate.head != "" {
			refs = append([]string{"refs/heads/" + candidate.head}, refs...)
		}
		firstResolved := func() string {
			for _, ref := range refs {
				if commit := g.resolve(workTree, ref); commit != "" {
					return commit
				}
			}
			return ""
		}
		head.commit = firstResolved()
		if head.commit == "" && candidate.mayMaterialize && materialized < pullCheckMaterializeLimit {
			materialized++
			if err := store.MaterializePullRef(candidate.pull); err != nil {
				head.errorText = err.Error()
			}
			head.commit = firstResolved()
			if head.commit != "" {
				head.errorText = ""
			}
		}
		if head.commit == "" {
			if head.errorText == "" {
				head.errorText = "its signed commits do not replay onto the base"
			}
			heads = append(heads, head)
			continue
		}
		publishedHead := g.resolve(mirror, head.ref)
		requestedTree := g.tree(workTree, head.commit)
		publishedTree := g.tree(mirror, publishedHead)
		if publishedHead != "" && publishedHead != head.commit &&
			((requestedTree != "" && publishedTree == requestedTree) || g.out(mirror, "merge-base", head.commit, publishedHead) == head.commit) {
			head.commit = publishedHead
		}
		if g.resolve(mirror, head.ref) != head.commit {
			head.published = true
			pushArgs := []string{"push", "--quiet", "--force-with-lease=" + head.ref + ":" + publishedHead, mirror, "+" + head.commit + ":" + head.ref}
			_, failure, pushErrors := g.run(workTree, pushArgs...)
			head.errorText = failure
			if g.resolve(mirror, head.ref) != head.commit && len(dropUnreadableRefs(g, mirror, pushErrors)) > 0 {
				_, head.errorText, _ = g.run(workTree, pushArgs...)
			}
			if g.resolve(mirror, head.ref) != head.commit {
				if head.errorText == "" {
					head.errorText = "the served mirror did not accept the head"
				}
				head.commit = ""
			} else {
				head.errorText = ""
			}
		}
		heads = append(heads, head)
	}
	return heads
}

func (q *actionQueue) applyPullCheckHeads(owner, name, mirror string, trigger Trigger, force bool, heads []pullCheckHead) {
	repo, ok := q.snap.lookup(owner, name)
	if !ok || repo.Preview || !repo.Enabled || repo.ExternallyManaged || qtTrim(repo.MirrorPath) != mirror {
		return
	}
	state := q.pullChecks
	runs := q.runs()
	queued, deferred := 0, 0
	for _, head := range heads {
		key := pullCheckKey(owner, name, head.number)
		if head.commit == "" {
			if !head.published {
				state.noHead[key] = true
			}
			if force || head.published {
				q.log("Actions: could not publish pull request #%d's head for %s/%s — %s", head.number, owner, name, head.errorText)
			}
			continue
		}
		if !force {
			if state.queued[key] == head.commit {
				continue
			}
			alreadyRan := false
			for _, run := range runs {
				if run.Owner == owner && run.Name == name && run.Commit == head.commit {
					alreadyRan = true
					break
				}
			}
			if alreadyRan {
				state.queued[key] = head.commit
				continue
			}
			if queued >= pullCheckQueueLimit {
				deferred++
				continue
			}
		}
		state.queued[key] = head.commit
		if head.published {
			q.claimExplicit(owner, name, head.commit)
		}
		q.queueWorkflowsForCommit(owner, name, head.commit, head.ref, trigger,
			PullContext{Number: head.number, Author: head.author, Base: head.base, FirstTimeAuthor: head.firstTimeAuthor}, "")
		queued++
	}
	if deferred > 0 {
		q.log("Actions: %d more open pull request(s) on %s/%s still need checks; the next pull refresh queues them.", deferred, owner, name)
	}
}

func (m *Module) handleQueuePullChecks(ctx context.Context, req *rpc.Request) (any, error) {
	var request pullCheckRequest
	if err := req.Bind(&request); err != nil {
		return nil, err
	}
	return m.requireArmed(ctx, func(q *actionQueue, done func(any, error)) {
		q.requestPullChecks(request, done)
	})
}
