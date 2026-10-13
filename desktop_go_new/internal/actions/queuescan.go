package actions

import (
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"math"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"time"
)

type Trigger string

const (
	TriggerPush              Trigger = "push"
	TriggerPullRequest       Trigger = "pull_request"
	TriggerPullRequestManual Trigger = "pull_request_manual"
	TriggerRelease           Trigger = "release"
)

type PullContext struct {
	Number          int    `json:"number"`
	Author          string `json:"author"`
	Base            string `json:"base"`
	FirstTimeAuthor bool   `json:"firstTimeAuthor"`
}

type workflowScanEntry struct {
	path                     string
	content                  string
	baseTriggersPullRequest  bool
	defaultBranchRetiresPush bool
}

type workflowScan struct {
	mirrorMissing  bool
	metadataOnly   bool
	changedPaths   []string
	workflows      []workflowScanEntry
	snapshotOK     bool
	repositoryTree string
	digest         string
	snapshotError  string
	pushBefore     string
}

var (
	externalCommitPattern = regexp.MustCompile(`^(?:[0-9a-f]{40}|[0-9a-f]{64})$`)
	safeBranchPattern     = regexp.MustCompile(`^[A-Za-z0-9._/-]{1,200}$`)
	refBodyPattern        = regexp.MustCompile(`^[A-Za-z0-9._/-]{1,120}$`)
)

func validExternalRef(ref string) bool {
	if !strings.HasPrefix(ref, "refs/heads/") {
		return false
	}
	body := ref[len("refs/heads/"):]
	return refBodyPattern.MatchString(body) && !strings.HasPrefix(body, "/") && !strings.HasSuffix(body, "/") &&
		!strings.Contains(body, "..") && !strings.Contains(body, "//")
}

func hashedKey(prefix, material string) string {
	sum := sha256.Sum256([]byte(material))
	return prefix + hex.EncodeToString(sum[:])
}

func externalActionHeadKey(repo RepoEntry) string {
	return hashedKey("actions/externalHeads/", repo.Owner+"/"+repo.Name+"/"+repo.ExternalRef)
}

func servedActionHeadKey(repo RepoEntry) string {
	return hashedKey("actions/servedHeads/", repo.Owner+"/"+repo.Name)
}

func gitHeadRef(repository string) string {
	out, ok := gitCapture("", 5*time.Second, "--git-dir", repository, "symbolic-ref", "--quiet", "HEAD")
	if !ok {
		return ""
	}
	if len(out) > 512 {
		out = out[:512]
	}
	ref := qtTrim(qtFromUtf8(out))
	if !validExternalRef(ref) {
		return ""
	}
	return ref
}

func gitCommitAt(repository, ref string) string {
	out, code := gitCaptureMerged("", 5*time.Second, "--git-dir", repository, "rev-parse", "--verify", ref+"^{commit}")
	if code != 0 {
		return ""
	}
	if len(out) > 256 {
		out = out[:256]
	}
	commit := strings.ToLower(qtTrim(qtFromUtf8(out)))
	if !externalCommitPattern.MatchString(commit) {
		return ""
	}
	return commit
}

func fetchExternalActionRef(repo RepoEntry) bool {
	_, code := gitCaptureMerged("", 30*time.Second, "--git-dir", repo.MirrorPath, "fetch", "--no-tags",
		repo.ExternalSource, "+"+repo.ExternalRef+":"+repo.ExternalRef)
	return code == 0
}

func readSpoolFile(path string) (string, bool) {
	data, err := os.ReadFile(path)
	if err != nil {
		return "", false
	}
	return qtFromUtf8(stripCarriageReturns(data)), true
}

func spoolFields(text string) (owner, name, ref, commit, before string) {
	for _, line := range splitNonEmpty(text, "\n") {
		switch {
		case strings.HasPrefix(line, "owner "):
			owner = qtTrim(line[6:])
		case strings.HasPrefix(line, "name "):
			name = qtTrim(line[5:])
		case strings.HasPrefix(line, "ref "):
			parts := splitNonEmpty(line[4:], " ")
			if len(parts) >= 3 && strings.HasPrefix(parts[2], "refs/heads/") {
				before, commit, ref = parts[0], parts[1], parts[2]
			}
		}
	}
	return
}

func (q *actionQueue) scanSpool() {
	if !q.armed {
		return
	}
	q.scanExternalSources()
	dir := q.m.st.SpoolDir()
	entries, _ := os.ReadDir(dir)
	var files []string
	for _, entry := range entries {
		if !entry.IsDir() && strings.HasSuffix(entry.Name(), ".push") && !strings.HasPrefix(entry.Name(), ".") {
			files = append(files, entry.Name())
		}
	}
	sort.Strings(files)
	reattested := map[string]bool{}
	pushed := map[string]string{}
	for _, file := range files {
		full := filepath.Join(dir, file)
		text, ok := readSpoolFile(full)
		if !ok {
			continue
		}
		_ = os.Remove(full)
		owner, name, ref, commit, before := spoolFields(text)
		if owner != "" && name != "" {
			if repo, found := q.snap.lookup(owner, name); found && !reattested[repo.Owner+"\x1f"+repo.Name] {
				reattested[repo.Owner+"\x1f"+repo.Name] = true
				q.m.emit("actions.pushed", map[string]any{"owner": owner, "name": name})
			}
		}
		if owner == "" || name == "" || commit == "" {
			continue
		}
		if strings.Count(commit, "0") == len(commit) {
			continue
		}
		if repo, found := q.snap.lookup(owner, name); found {
			pushed[repo.Owner+"\x1f"+repo.Name] = strings.ToLower(commit)
		}
		q.enqueuePushEvent(owner, name, commit, ref, before)
	}
	q.scanServedHeads(pushed)
	q.processQueue()
	q.updateMirrorRuntimeState()
}

type servedProbe struct {
	owner, name, mirror string
}

type servedResult struct {
	servedProbe
	ref, commit string
}

func (q *actionQueue) servedCandidate(repo RepoEntry) bool {
	return !repo.Preview && repo.Enabled && !repo.ExternallyManaged && qtTrim(repo.LocalPath) != "" &&
		qtTrim(repo.MirrorPath) != "" && dirExists(repo.MirrorPath)
}

func (q *actionQueue) currentServed(probe servedProbe) (RepoEntry, bool) {
	for _, repo := range q.snap.Repos {
		if repo.Owner == probe.owner && repo.Name == probe.name && repo.MirrorPath == probe.mirror {
			return repo, !repo.Preview && repo.Enabled && !repo.ExternallyManaged && qtTrim(repo.LocalPath) != ""
		}
	}
	return RepoEntry{}, false
}

func (q *actionQueue) scanServedHeads(pushed map[string]string) {
	for key, commit := range pushed {
		if q.servedPushed[key] == nil {
			q.servedPushed[key] = map[string]bool{}
		}
		q.servedPushed[key][commit] = true
	}
	q.servedPending = true
	if q.servedInFlight {
		return
	}
	q.servedPending = false
	var probes []servedProbe
	for _, repo := range q.snap.Repos {
		if q.servedCandidate(repo) {
			probes = append(probes, servedProbe{repo.Owner, repo.Name, repo.MirrorPath})
		}
	}
	if len(probes) == 0 {
		q.servedPushed = map[string]map[string]bool{}
		return
	}
	q.servedInFlight = true
	q.servedGeneration++
	generation := q.servedGeneration
	go func() {
		results := make([]servedResult, 0, len(probes))
		for _, probe := range probes {
			result := servedResult{servedProbe: probe}
			result.ref = gitHeadRef(probe.mirror)
			if result.ref != "" {
				result.commit = gitCommitAt(probe.mirror, result.ref)
			}
			results = append(results, result)
		}
		q.post(func() { q.persistServedHeads(generation, results) })
	}()
}

func (q *actionQueue) persistServedHeads(generation uint64, results []servedResult) {
	if generation != q.servedGeneration {
		q.finishServedScan(false)
		return
	}
	type persistence struct {
		result servedResult
		key    string
	}
	var items []persistence
	for _, result := range results {
		if result.ref == "" || result.commit == "" {
			continue
		}
		repo, ok := q.currentServed(result.servedProbe)
		if !ok {
			continue
		}
		items = append(items, persistence{result, servedActionHeadKey(repo)})
	}
	if len(items) == 0 {
		q.finishServedScan(false)
		return
	}
	settings := q.m.settings()
	previous := make([]string, len(items))
	writes := map[string]string{}
	for index, item := range items {
		previous[index] = q.m.settingValue(settings, item.key)
		if previous[index] != item.result.commit {
			writes[item.key] = item.result.commit
		}
	}
	go func() {
		ok := q.m.persistValues(writes)
		q.post(func() {
			queued := false
			if ok && generation == q.servedGeneration {
				for index, item := range items {
					result := item.result
					repo, current := q.currentServed(result.servedProbe)
					if !current {
						continue
					}
					if previous[index] == "" || previous[index] == result.commit {
						continue
					}
					if q.servedPushed[repo.Owner+"\x1f"+repo.Name][result.commit] {
						continue
					}
					q.enqueuePushEvent(repo.Owner, repo.Name, result.commit, result.ref, previous[index])
					queued = true
				}
			}
			q.finishServedScan(queued)
		})
	}()
}

func (q *actionQueue) finishServedScan(queued bool) {
	q.servedInFlight = false
	if queued {
		q.processQueue()
		q.updateMirrorRuntimeState()
	}
	if q.servedPending {
		q.post(func() { q.scanServedHeads(nil) })
	} else {
		q.servedPushed = map[string]map[string]bool{}
	}
}

func (q *actionQueue) mirrorActionsSuspended() bool {
	return q.snap.MirrorSuspended || q.m.mirrorRecoveryFailed()
}

func (q *actionQueue) scanExternalSources() {
	if q.mirrorActionsSuspended() {
		return
	}
	now := nowMs()
	if now-q.externalLastScan < 3500 {
		return
	}
	type probe struct {
		repo RepoEntry
		key  string
	}
	var probes []probe
	for _, repo := range q.snap.Repos {
		if !repo.ExternallyManaged || !repo.Enabled || repo.Preview || !dirIsReal(repo.ExternalSource) ||
			!dirIsReal(repo.MirrorPath) || !validExternalRef(repo.ExternalRef) {
			continue
		}
		probes = append(probes, probe{repo, externalActionHeadKey(repo)})
	}
	if len(probes) == 0 {
		q.externalLastScan = now
		return
	}
	q.externalLastScan = math.MaxInt64
	settings := q.m.settings()
	type push struct{ owner, name, commit, ref, before string }
	go func() {
		var pushes []push
		writes := map[string]string{}
		for _, p := range probes {
			repo := p.repo
			source := gitCommitAt(repo.ExternalSource, repo.ExternalRef)
			if source == "" {
				continue
			}
			previous := writes[p.key]
			if previous == "" {
				previous = q.m.settingValue(settings, p.key)
			}
			if previous == "" {
				writes[p.key] = source
				continue
			}
			if previous == source {
				continue
			}
			if !fetchExternalActionRef(repo) || gitCommitAt(repo.MirrorPath, repo.ExternalRef) != source {
				continue
			}
			writes[p.key] = source
			pushes = append(pushes, push{repo.Owner, repo.Name, source, repo.ExternalRef, previous})
		}
		if !q.m.persistValues(writes) {
			pushes = nil
		}
		q.post(func() {
			q.externalLastScan = now
			for _, p := range pushes {
				q.enqueuePushEvent(p.owner, p.name, p.commit, p.ref, p.before)
			}
			if len(pushes) > 0 {
				q.processQueue()
				q.updateMirrorRuntimeState()
			}
		})
	}()
}

func explicitKey(owner, name, commit string) string {
	return owner + "\x1f" + name + "\x1f" + strings.ToLower(qtTrim(commit))
}

func (q *actionQueue) claimExplicit(owner, name, commit string) {
	q.explicit[explicitKey(owner, name, commit)] = nowMs() + explicitClaimMs
}

func (q *actionQueue) takeExplicit(owner, name, commit string) bool {
	key := explicitKey(owner, name, commit)
	expires, ok := q.explicit[key]
	delete(q.explicit, key)
	now := nowMs()
	for other, at := range q.explicit {
		if at <= now {
			delete(q.explicit, other)
		}
	}
	return ok && expires > now
}

func (q *actionQueue) enqueuePushEvent(owner, name, commit, ref, before string) {
	repo, ok := q.snap.lookup(owner, name)
	if !ok {
		return
	}
	mirror := repo.MirrorPath
	go func() {
		out, _ := gitStdoutAny(mirror, 5*time.Second, "show", "-s", "--format=%s", commit)
		subject := qtTrim(qtFromUtf8(out))
		q.post(func() { q.applyPushEvent(owner, name, commit, ref, before, subject) })
	}()
}

func (q *actionQueue) applyPushEvent(owner, name, commit, ref, before, subject string) {
	repo, ok := q.snap.lookup(owner, name)
	if !ok {
		return
	}
	branch := strings.TrimPrefix(ref, "refs/heads/")
	suffix := ""
	if subject != "" {
		suffix = " — " + subject
	}
	q.log("Push to %s/%s on %s → %s%s", owner, name, branch, qtLeft(commit, 8), suffix)
	q.m.emit("actions.push", map[string]any{"owner": owner, "name": name, "branch": branch, "commit": commit, "subject": subject})
	if q.takeExplicit(owner, name, commit) {
		q.log("Actions: checks for %s/%s @ %s were already queued by pull-request creation.", owner, name, qtLeft(commit, 8))
		return
	}
	if !repo.Enabled {
		return
	}
	q.queueWorkflowsForCommit(owner, name, commit, ref, TriggerPush, PullContext{}, before)
}

func (q *actionQueue) queueWorkflowsForCommit(owner, name, commit, ref string, trigger Trigger, pull PullContext, before string) {
	repo, ok := q.snap.lookup(owner, name)
	if !ok {
		return
	}
	mirror := repo.MirrorPath
	beforeKey := ""
	if trigger == TriggerPush {
		beforeKey = strings.ToLower(qtTrim(before))
	}
	baseRef := ""
	if (trigger == TriggerPullRequest || trigger == TriggerPullRequestManual) && safeBranchPattern.MatchString(qtTrim(pull.Base)) {
		baseRef = "refs/heads/" + qtTrim(pull.Base)
	}
	go func() {
		scan := scanWorkflows(mirror, commit, ref, trigger, baseRef, beforeKey)
		q.post(func() { q.applyWorkflowScan(owner, name, commit, ref, trigger, pull, scan) })
	}()
}

func scanWorkflows(mirror, commit, ref string, trigger Trigger, baseRef, before string) workflowScan {
	scan := workflowScan{pushBefore: before}
	if !dirExists(mirror) {
		scan.mirrorMissing = true
		return scan
	}
	capture := func(args ...string) string {
		out, ok := gitCapture(mirror, 10*time.Second, args...)
		if !ok {
			return ""
		}
		return qtFromUtf8(out)
	}
	if trigger == TriggerPush {
		scan.changedPaths = PushRangeChangedPaths(mirror, before, commit)
		if IsMetadataOnlyChange(scan.changedPaths) {
			scan.metadataOnly = true
			return scan
		}
	}
	defaultRef := ""
	if trigger == TriggerPush {
		defaultRef = qtTrim(capture("symbolic-ref", "--quiet", "HEAD"))
		if defaultRef == ref || !strings.HasPrefix(defaultRef, "refs/heads/") {
			defaultRef = ""
		}
	}
	for _, path := range splitNonEmpty(capture("ls-tree", "-r", "--name-only", commit, "--", ".forkmesh"), "\n") {
		if !strings.HasSuffix(path, ".yml") && !strings.HasSuffix(path, ".yaml") {
			continue
		}
		content := capture("show", commit+":"+path)
		if content == "" {
			continue
		}
		entry := workflowScanEntry{path: path, content: content}
		if baseRef != "" {
			baseContent := capture("show", baseRef+":"+path)
			entry.baseTriggersPullRequest = baseContent != "" && Parse(path, baseContent).TriggersOnPullRequest()
		}
		if defaultRef != "" {
			entry.defaultBranchRetiresPush = DefaultBranchRetiresPush(path, capture("show", defaultRef+":"+path))
		}
		scan.workflows = append(scan.workflows, entry)
	}
	if len(scan.workflows) > 0 {
		scan.repositoryTree, scan.digest, scan.snapshotError = RepositoryStateDigest(mirror, commit)
		scan.snapshotOK = scan.snapshotError == ""
	}
	return scan
}

func (q *actionQueue) applyWorkflowScan(owner, name, commit, ref string, trigger Trigger, pull PullContext, scan workflowScan) {
	repo, ok := q.snap.lookup(owner, name)
	if !ok {
		return
	}
	short := qtLeft(commit, 8)
	if scan.mirrorMissing {
		q.log("Actions: served mirror %s for %s/%s is missing — cannot look up workflows at %s.", repo.MirrorPath, owner, name, short)
		return
	}
	if scan.metadataOnly {
		q.log("Actions: %s/%s @ %s only touches issues/PRs — skipping workflows.", owner, name, short)
		return
	}
	if len(scan.workflows) > 0 && !scan.snapshotOK && q.m.shuttingDown() {
		spooled := trigger == TriggerPush && q.m.st.SpoolPushEvent(owner, name, commit, ref, scan.pushBefore, os.Getpid())
		if spooled {
			q.log("Actions: ForkMesh is stopping, so the push to %s/%s on %s @ %s was not bound (%s); it is saved and its workflows are queued when ForkMesh starts again.",
				owner, name, ref, short, scan.snapshotError)
		} else {
			q.log("Actions: ForkMesh is stopping, so workflows for %s/%s @ %s on %s were not queued (%s). Use Run workflow to run them after it starts again.",
				owner, name, short, ref, scan.snapshotError)
		}
		return
	}
	added := false
	for _, entry := range scan.workflows {
		wf := Parse(entry.path, entry.content)
		runsOnPullRequests := wf.TriggersOnPullRequest() || entry.baseTriggersPullRequest
		matches := false
		switch trigger {
		case TriggerRelease:
			matches = wf.TriggersOnRelease()
		case TriggerPullRequest:
			matches = runsOnPullRequests
		case TriggerPullRequestManual:
			matches = runsOnPullRequests || wf.TriggersOnPush()
		case TriggerPush:
			matches = wf.TriggersOnPush()
		}
		if !wf.Valid || !matches {
			continue
		}
		if trigger == TriggerPush && entry.defaultBranchRetiresPush {
			q.log("Actions: “%s” no longer runs on push on the default branch, so this push to %s does not run it.", wf.Name, ref)
			continue
		}
		if trigger == TriggerPush && !wf.MatchesPushChangedPaths(scan.changedPaths) {
			against := short + " against its parents"
			if IsKnownPushBase(scan.pushBefore) {
				against = qtLeft(scan.pushBefore, 8) + ".." + short
			}
			q.log("Actions: “%s” did not match the workflow's on.push.paths filter for this push (%s).", wf.Name, against)
			continue
		}
		if containsString(repo.DisabledWorkflows, entry.path) {
			q.log("Actions: “%s” is disabled for %s/%s — skipping.", wf.Name, owner, name)
			continue
		}
		if !WorkflowRunsOnThisNode(repo.WorkflowNodes, wf, q.snap.NodeLabels) {
			q.log("Actions: “%s” is dedicated to %s — this node (%s) is skipping it.", wf.Name,
				strings.Join(WorkflowRunsOnLabels(repo.WorkflowNodes, wf), ", "), strings.Join(q.snap.NodeLabels, ", "))
			continue
		}
		run := NewRun()
		run.Owner, run.Name = owner, name
		run.WorkflowPath, run.WorkflowName, run.WorkflowContent = entry.path, wf.Name, entry.content
		run.Commit, run.Ref = commit, ref
		run.PullNumber, run.PullAuthor = pull.Number, pull.Author
		run.FirstTimePullAuthor = pull.Number > 0 && pull.FirstTimeAuthor
		if !scan.snapshotOK {
			q.log("Actions: could not bind \"%s\" to repository state for %s/%s @ %s: %s", wf.Name, owner, name, short, scan.snapshotError)
			continue
		}
		run.RepositoryTree, run.ExecutionDigest = scan.repositoryTree, scan.digest
		approved := q.resolveApproval(repo, run)
		if approved {
			run.Status = StatusQueued
		} else {
			run.Status = StatusAwaitingApproval
		}
		created, ok := q.createRun(run)
		if !ok {
			continue
		}
		extra := map[string]any{"created": true}
		if !approved {
			title, body := "Action waiting for approval", fmt.Sprintf("%s · %s/%s at %s", wf.Name, owner, name, short)
			if run.FirstTimePullAuthor {
				title = "First pull request needs your approval"
				body = fmt.Sprintf("%s · %s/%s #%d by %s — their first contribution here, so this run stays held until you approve it",
					wf.Name, owner, name, run.PullNumber, run.PullAuthor)
			}
			extra["ping"] = map[string]any{"event": "approval", "title": title, "body": body}
		}
		q.emitRun(created, extra)
		q.scheduleSummary(0)
		q.cancelSuperseded(created)
		if approved {
			q.ids = append(q.ids, created.ID)
		}
		added = true
		what := "queued"
		if !approved {
			what = "awaiting approval of"
		}
		pullNote := ""
		if run.PullNumber > 0 {
			pullNote = fmt.Sprintf(" (pull request #%d)", run.PullNumber)
		}
		q.log("Actions: %s \"%s\" for %s/%s @ %s%s", what, wf.Name, owner, name, short, pullNote)
	}
	if added {
		q.ring()
		q.processQueue()
		return
	}
	event := "push"
	switch trigger {
	case TriggerRelease:
		event = "release"
	case TriggerPullRequest:
		event = "pull_request"
	case TriggerPullRequestManual:
		event = "pull_request' or 'push"
	}
	q.log("Actions: no .forkmesh/ workflow with 'on: %s' at %s for %s/%s — nothing to run.", event, short, owner, name)
}
