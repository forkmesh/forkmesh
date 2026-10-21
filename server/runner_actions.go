package mirrornode

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"
)

const (
	runnerActionsReportPath = "/api/runners/actions-report"
	runnerActionsDir        = ".actions"
	runnerActionsLogTail    = 16 << 10
	runnerActionsLogMax     = 1 << 20
	runnerActionsLogEnd     = 256 << 10
	runnerActionsMaxRuns    = 32
	runnerActionsMaxCatalog = 64
	runnerActionsMaxMinutes = 90
	runnerActionsTick       = 5 * time.Second
	runnerActionsPayloadMax = 1 << 20
	runnerActionsSecretMax  = 48 << 10
	runnerActionsReportMax  = 8<<20 - 64<<10
)

var (
	runnerActionsEventPattern  = regexp.MustCompile(`^[a-z_]{1,40}$`)
	runnerActionsSecretPattern = regexp.MustCompile(`^[A-Z_][A-Z0-9_]{0,63}$`)
	runnerActionsZeroOID       = regexp.MustCompile(`^0+$`)
)

type runnerActionsJob struct {
	Kind            string `json:"kind"`
	JobID           int64  `json:"jobId"`
	LeaseID         string `json:"leaseId"`
	RepositoryOwner string `json:"repositoryOwner"`
	RepositoryName  string `json:"repositoryName"`
	Organization    string `json:"organization"`
	Branch          string `json:"branch"`
	Commit          string `json:"commit"`
	CloneURL        string `json:"cloneUrl"`
	ReadGrant       string `json:"readGrant"`
	GrantExpiresAt  int64  `json:"grantExpiresAt"`
	TimeoutMs       int64  `json:"timeoutMs"`

	Event        string            `json:"event"`
	Workflow     string            `json:"workflow"`
	Before       string            `json:"before"`
	EventPayload json.RawMessage   `json:"eventPayload"`
	Inputs       map[string]string `json:"inputs"`
	Secrets      map[string]string `json:"secrets"`
	PushGrant    string            `json:"pushGrant"`
	PushRef      string            `json:"pushRef"`
	JobToken     string            `json:"jobToken"`
	APIURL       string            `json:"apiUrl"`
	Trusted      bool              `json:"trusted"`
	BotName      string            `json:"botName"`
	BotEmail     string            `json:"botEmail"`

	catalog []runnerActionsCatalogEntry
}

type runnerActionsCatalogEntry struct {
	Path      string          `json:"path"`
	Name      string          `json:"name"`
	Events    []string        `json:"events"`
	Schedules []string        `json:"schedules"`
	Inputs    []workflowInput `json:"inputs"`
}

func (job runnerActionsJob) event() string {
	if job.Event == "" {
		return "push"
	}
	return job.Event
}

type runnerActionsRun struct {
	Workflow     string `json:"workflow"`
	Commit       string `json:"commit"`
	Ref          string `json:"ref"`
	Status       string `json:"status"`
	CreatedAt    int64  `json:"createdAt"`
	StartedAt    int64  `json:"startedAt"`
	FinishedAt   int64  `json:"finishedAt"`
	LogTail      string `json:"logTail"`
	Event        string `json:"event"`
	WorkflowPath string `json:"workflowPath"`
	Log          string `json:"-"`

	sentLog string
	path    string
	name    string
	needs   []string
	wf      *workflow
}

type runnerActions struct {
	b    *runnerBackend
	tick time.Duration
	busy atomic.Bool
	runs sync.WaitGroup
	now  func() time.Time
	git  func(ctx context.Context, gitDir string, args ...string) (string, error)
	step func(ctx context.Context, work string, env []string, script string, sink io.Writer) (int, error)
	post func(ctx context.Context, body []byte) (int, []byte, error)
}

func newRunnerActions(b *runnerBackend) *runnerActions {
	a := &runnerActions{b: b, now: time.Now, tick: runnerActionsTick}
	a.git = func(ctx context.Context, gitDir string, args ...string) (string, error) {
		return b.daemonGit(ctx, "", "", "", false, append([]string{"--git-dir=" + gitDir}, args...)...)
	}
	a.step = a.runStep
	a.post = func(ctx context.Context, body []byte) (int, []byte, error) {
		return b.signedPost(ctx, runnerActionsReportPath, body)
	}
	return a
}

func (a *runnerActions) labels() []string {
	return []string{a.b.node, "linux", "runner"}
}

func (a *runnerActions) free() bool {
	return a != nil && !a.busy.Load()
}

func isRunnerActionsJob(raw json.RawMessage) bool {
	var probe struct {
		Kind string `json:"kind"`
	}
	return json.Unmarshal(raw, &probe) == nil && probe.Kind == "actions"
}

func (a *runnerActions) accept(ctx context.Context, raw json.RawMessage) {
	var job runnerActionsJob
	if err := json.Unmarshal(raw, &job); err != nil || job.JobID <= 0 || job.LeaseID == "" {
		log.Printf("runner actions: dropped a malformed job")
		return
	}
	if !a.busy.CompareAndSwap(false, true) {
		a.report(ctx, job, "failed", nil, "", "This runner was already running an Actions job.")
		return
	}
	a.runs.Add(1)
	go func() {
		defer a.runs.Done()
		defer a.busy.Store(false)
		timeout := time.Duration(job.TimeoutMs) * time.Millisecond
		if timeout <= 0 || timeout > 2*actionsRunTimeout {
			timeout = 2 * actionsRunTimeout
		}
		runCtx, cancel := context.WithTimeout(ctx, timeout)
		defer cancel()
		a.execute(runCtx, job)
		if a.b.wake != nil {
			a.b.wake()
		}
	}()
}

func (a *runnerActions) wait() {
	if a != nil {
		a.runs.Wait()
	}
}

func (a *runnerActions) validate(job runnerActionsJob) string {
	owner := strings.ToLower(job.RepositoryOwner)
	if job.Kind != "actions" || !nodePattern.MatchString(owner) ||
		!repositoryPattern.MatchString(job.RepositoryName) || validPullBranch(job.Branch) == "" ||
		(job.Commit != "" && !runnerObjectID.MatchString(job.Commit)) ||
		(job.Before != "" && !runnerObjectID.MatchString(job.Before)) ||
		!runnerActionsEventPattern.MatchString(job.event()) ||
		((job.event() != "push" || job.Workflow != "") && !workflowPathPattern.MatchString(job.Workflow)) {
		return "Malformed or unsafe Actions job."
	}
	if problem := validRunnerActionsExtras(job); problem != "" {
		return problem
	}
	organization := strings.ToLower(job.Organization)
	if organization == "" {
		organization = owner
	}
	if !nodePattern.MatchString(organization) {
		return "Malformed or unsafe Actions job."
	}
	if len(a.b.cfg.AllowedOwners) != 0 && !containsString(a.b.cfg.AllowedOwners, organization) {
		return "This runner does not accept jobs for that owner."
	}
	read, ok := parseRunnerGrantToken(job.ReadGrant)
	if !ok || read.Access != "read" || read.Runner != a.b.node ||
		!strings.EqualFold(read.Owner, owner) || read.Repository != job.RepositoryName {
		return "The lease's read grant does not match the job."
	}
	if time.UnixMilli(read.ExpiresAt).Before(a.now()) {
		return "The lease's read grant has expired."
	}
	if _, err := runnerCloneURL(agentJob{RepositoryOwner: owner, RepositoryName: job.RepositoryName,
		CloneURL: job.CloneURL}); err != nil {
		return err.Error()
	}
	return ""
}

func validRunnerActionsExtras(job runnerActionsJob) string {
	if len(job.EventPayload) > runnerActionsPayloadMax {
		return "The Actions event payload is too large."
	}
	if len(job.EventPayload) != 0 && string(job.EventPayload) != "null" {
		var probe map[string]any
		if json.Unmarshal(job.EventPayload, &probe) != nil {
			return "The Actions event payload is not a JSON object."
		}
	}
	if len(job.Inputs) > maxWorkflowInputs {
		return "The Actions job has too many inputs."
	}
	for name, value := range job.Inputs {
		if !workflowInputName.MatchString(name) || len(value) > 64<<10 || strings.ContainsRune(value, 0) {
			return "An Actions input is malformed."
		}
	}
	if len(job.Secrets) > 256 {
		return "The Actions job has too many secrets."
	}
	for name, value := range job.Secrets {
		if !runnerActionsSecretPattern.MatchString(name) || len(value) > runnerActionsSecretMax ||
			strings.ContainsRune(value, 0) {
			return "An Actions secret is malformed."
		}
	}
	for _, value := range []string{job.JobToken, job.PushGrant, job.BotName, job.BotEmail, job.APIURL, job.PushRef} {
		if len(value) > 4096 || strings.ContainsAny(value, "\x00\r\n") {
			return "Malformed or unsafe Actions job."
		}
	}
	return ""
}

func (a *runnerActions) pushGrant(job runnerActionsJob) string {
	if !job.Trusted || job.PushGrant == "" {
		return ""
	}
	grant, ok := parseRunnerGrantToken(job.PushGrant)
	ref := "refs/heads/" + job.Branch
	if !ok || grant.Access != "actions-push" || grant.Runner != a.b.node ||
		!strings.EqualFold(grant.Owner, job.RepositoryOwner) || grant.Repository != job.RepositoryName ||
		grant.Ref != ref || (job.PushRef != "" && job.PushRef != ref) {
		return ""
	}
	return job.PushGrant
}

func (a *runnerActions) execute(ctx context.Context, job runnerActionsJob) {
	if problem := a.validate(job); problem != "" {
		a.report(ctx, job, "failed", nil, "", problem)
		return
	}
	if err := a.ensureRoot(); err != nil {
		a.report(ctx, job, "failed", nil, "", "The runner's work directory is unavailable.")
		return
	}
	cloneURL, _ := runnerCloneURL(agentJob{RepositoryOwner: strings.ToLower(job.RepositoryOwner),
		RepositoryName: job.RepositoryName, CloneURL: job.CloneURL})
	cache, ok := a.b.refreshCache(ctx, agentJob{ReadGrant: job.ReadGrant}, cloneURL)
	if !ok {
		a.report(ctx, job, "failed", nil, "", "The runner could not fetch the repository from the core.")
		return
	}
	lock := a.b.cacheLock(cache)
	lock.Lock()
	runs, commit, catalog, problem := a.plan(ctx, cache, job)
	lock.Unlock()
	job.catalog = catalog
	if problem != "" {
		a.report(ctx, job, "completed", nil, commit, problem)
		return
	}
	if len(runs) == 0 {
		message := "No workflow runs on a push to " + job.Branch + " on a runner."
		if job.event() != "push" {
			message = "No workflow runs on " + job.event() + " for " + job.Workflow + " on a runner."
		}
		a.report(ctx, job, "completed", nil, commit, message)
		return
	}
	ctx, cancelJob := context.WithCancel(ctx)
	defer cancelJob()
	stopIfCancelled := func(cancel bool) {
		if cancel {
			cancelJob()
		}
	}
	stopIfCancelled(a.report(ctx, job, "running", runs, commit, ""))
	for ctx.Err() == nil {
		index, verdict := nextRunnerActionsRun(runs)
		if index < 0 {
			break
		}
		run := runs[index]
		if verdict == "skip" {
			run.Status, run.FinishedAt = actionSkipped, a.now().UnixMilli()
			run.LogTail = "==> Skipped: a workflow it needs did not succeed.\n"
			run.Log = run.LogTail
			continue
		}
		run.Status, run.StartedAt = actionRunning, a.now().UnixMilli()
		stopIfCancelled(a.report(ctx, job, "running", runs, commit, ""))
		if ctx.Err() != nil {
			break
		}
		live := &runnerActionsLog{}
		stop, done := make(chan struct{}), make(chan struct{})
		go a.tickReports(ctx, cancelJob, job, runs, commit, run, live, stop, done)
		status := a.runWorkflow(ctx, job, cache, lock, commit, index, run, live)
		close(stop)
		<-done
		run.Status, run.Log, run.LogTail, run.FinishedAt = status, live.String(), live.Tail(), a.now().UnixMilli()
		if ctx.Err() != nil {
			break
		}
	}
	for _, run := range runs {
		if run.Status == actionQueued || run.Status == actionRunning {
			run.Status, run.FinishedAt = actionCancelled, a.now().UnixMilli()
		}
	}
	a.report(context.WithoutCancel(ctx), job, "completed", runs, commit, "")
}

func (a *runnerActions) tickReports(ctx context.Context, cancelJob context.CancelFunc, job runnerActionsJob, runs []*runnerActionsRun, commit string,
	run *runnerActionsRun, live *runnerActionsLog, stop, done chan struct{}) {
	defer close(done)
	if a.tick <= 0 {
		return
	}
	ticker := time.NewTicker(a.tick)
	defer ticker.Stop()
	for {
		select {
		case <-stop:
			return
		case <-ctx.Done():
			return
		case <-ticker.C:
			run.Log, run.LogTail = live.String(), live.Tail()
			body, sent, err := a.reportBody(job, "running", runs, commit, "")
			if err != nil {
				continue
			}
			code, reply, err := a.post(ctx, body)
			switch {
			case err == nil && code >= 200 && code < 300:
				markRunnerActionsLogsSent(sent)
				if runnerActionsCancelRequested(reply) {
					cancelJob()
					return
				}
			case err == nil && code >= 400 && code < 500:
				log.Printf("runner actions: progress for job %d refused (HTTP %d): %s", job.JobID, code, boundedText(reply, 200))
			}
		}
	}
}

func (a *runnerActions) ensureRoot() error {
	if err := os.MkdirAll(a.b.cfg.WorkRoot, 0o700); err != nil {
		return err
	}
	_ = os.Chmod(a.b.cfg.WorkRoot, 0o711)
	root := filepath.Join(a.b.cfg.WorkRoot, runnerActionsDir)
	if err := ensureDaemonDir(root, 0o711); err != nil {
		return err
	}
	entries, err := os.ReadDir(root)
	if err != nil {
		return err
	}
	for _, entry := range entries {
		if err := removeTreeUnder(root, filepath.Join(root, entry.Name())); err != nil {
			log.Printf("runner actions: %v", err)
		}
	}
	return nil
}

func (a *runnerActions) resolveCommit(ctx context.Context, cache string, job runnerActionsJob) (string, string) {
	ref := "refs/heads/" + job.Branch
	tip, err := a.git(ctx, cache, "rev-parse", "--verify", "-q", ref+"^{commit}")
	if err != nil || !runnerObjectID.MatchString(tip) {
		tip = ""
	}
	if job.Commit == "" {
		if tip == "" {
			return "", "The branch " + job.Branch + " is not on the core."
		}
		return tip, ""
	}
	if tip != "" {
		if _, err := a.git(ctx, cache, "merge-base", "--is-ancestor", job.Commit, tip); err == nil {
			return job.Commit, ""
		}
	}
	exists := false
	if out, err := a.git(ctx, cache, "rev-parse", "--verify", "-q", job.Commit+"^{commit}"); err == nil && out == job.Commit {
		exists = true
	}
	if exists && (job.event() != "push" || !a.isDefaultBranch(ctx, cache, job.Branch)) {
		return job.Commit, ""
	}
	if tip == "" && !exists {
		return "", "The branch " + job.Branch + " is not on the core."
	}
	return "", "The pushed commit is not on " + job.Branch + "; nothing ran."
}

func (a *runnerActions) isDefaultBranch(ctx context.Context, cache, branch string) bool {
	head, err := a.git(ctx, cache, "symbolic-ref", "-q", "HEAD")
	if err != nil || !strings.HasPrefix(head, "refs/heads/") {
		return true
	}
	return strings.TrimPrefix(head, "refs/heads/") == branch
}

func (a *runnerActions) changedPaths(ctx context.Context, cache, before, commit string) ([]string, bool) {
	if before == "" || runnerActionsZeroOID.MatchString(before) {
		return nil, false
	}
	out, err := a.git(ctx, cache, "diff", "--name-only", "--no-renames", before, commit)
	if err != nil {
		return nil, false
	}
	var paths []string
	for _, line := range strings.Split(out, "\n") {
		if line != "" {
			paths = append(paths, line)
		}
	}
	return paths, true
}

func (a *runnerActions) plan(ctx context.Context, cache string, job runnerActionsJob) ([]*runnerActionsRun, string, []runnerActionsCatalogEntry, string) {
	ref := "refs/heads/" + job.Branch
	catalog := []runnerActionsCatalogEntry{}
	commit, problem := a.resolveCommit(ctx, cache, job)
	if problem != "" {
		return nil, commit, catalog, problem
	}
	out, err := a.git(ctx, cache, "ls-tree", "--name-only", commit, ".forkmesh/")
	if err != nil {
		return nil, commit, catalog, "The workflows could not be listed."
	}
	event := job.event()
	action := ""
	if len(job.EventPayload) != 0 {
		var payload struct {
			Action string `json:"action"`
		}
		_ = json.Unmarshal(job.EventPayload, &payload)
		action = payload.Action
	}
	var changed []string
	changedKnown := false
	if event == "push" {
		changed, changedKnown = a.changedPaths(ctx, cache, job.Before, commit)
	}
	created := a.now().UnixMilli()
	var runs []*runnerActionsRun
	for _, name := range strings.Split(out, "\n") {
		if !workflowPathPattern.MatchString(name) {
			continue
		}
		content, err := a.git(ctx, cache, "show", commit+":"+name)
		if err != nil {
			continue
		}
		wf, err := parseWorkflow(name, content)
		if err != nil {
			continue
		}
		if len(catalog) < runnerActionsMaxCatalog {
			catalog = append(catalog, runnerActionsCatalogEntry{Path: wf.Path, Name: wf.Name,
				Events: nonNilStrings(wf.Events), Schedules: nonNilStrings(wf.Schedules),
				Inputs: nonNilInputs(wf.Inputs)})
		}
		if len(runs) == runnerActionsMaxRuns || !wf.runsOnThisNode(a.labels()) ||
			!workflowWantsEvent(wf, job, event, action, changed, changedKnown) {
			continue
		}
		runs = append(runs, &runnerActionsRun{Workflow: wf.Name, Commit: commit, Ref: ref,
			Status: actionQueued, CreatedAt: created, Event: event, WorkflowPath: wf.Path, path: wf.Path, name: wf.Name,
			needs: wf.Needs, wf: wf})
	}
	return runs, commit, catalog, ""
}

func workflowWantsEvent(wf *workflow, job runnerActionsJob, event, action string, changed []string, changedKnown bool) bool {
	if !wf.on(event) || (job.Workflow != "" && wf.Path != job.Workflow) {
		return false
	}
	if event == "push" && job.Workflow != "" {
		return len(wf.PushBranches) == 0 || wf.matchesPushBranch(job.Branch)
	}
	if event == "push" {
		if len(wf.PushBranches) != 0 && !wf.matchesPushBranch(job.Branch) {
			return false
		}
		return !changedKnown || wf.matchesPushPaths(changed)
	}
	if wf.Path != job.Workflow {
		return false
	}
	switch event {
	case "issues":
		return len(wf.IssueTypes) == 0 || containsString(wf.IssueTypes, action)
	case "issue_comment":
		return len(wf.CommentTypes) == 0 || containsString(wf.CommentTypes, action)
	}
	return true
}

func nonNilInputs(inputs []workflowInput) []workflowInput {
	out := make([]workflowInput, 0, len(inputs))
	for _, input := range inputs {
		if input.Options == nil {
			input.Options = []string{}
		}
		out = append(out, input)
	}
	return out
}

func nextRunnerActionsRun(runs []*runnerActionsRun) (int, string) {
	for index, run := range runs {
		if run.Status != actionQueued {
			continue
		}
		verdict := "go"
		for _, needed := range run.needs {
			for _, other := range runs {
				if other == run || (other.name != needed && other.path != needed) {
					continue
				}
				switch {
				case other.Status == actionQueued || other.Status == actionRunning:
					if verdict == "go" {
						verdict = "wait"
					}
				case other.Status != actionSuccess:
					verdict = "skip"
				}
			}
		}
		if verdict != "wait" {
			return index, verdict
		}
	}
	for index, run := range runs {
		if run.Status == actionQueued {
			return index, "skip"
		}
	}
	return -1, ""
}

type runnerActionsEnv struct {
	keys   []string
	values map[string]string
}

func (e *runnerActionsEnv) set(key, value string) {
	if e.values == nil {
		e.values = map[string]string{}
	}
	if _, ok := e.values[key]; !ok {
		e.keys = append(e.keys, key)
	}
	e.values[key] = value
}

func (e *runnerActionsEnv) has(key string) bool {
	_, ok := e.values[key]
	return ok
}

func (e *runnerActionsEnv) clone() *runnerActionsEnv {
	out := &runnerActionsEnv{keys: append([]string(nil), e.keys...), values: make(map[string]string, len(e.values))}
	for key, value := range e.values {
		out.values[key] = value
	}
	return out
}

func (e *runnerActionsEnv) list() []string {
	out := make([]string, 0, len(e.keys))
	for _, key := range e.keys {
		out = append(out, key+"="+e.values[key])
	}
	return out
}

func runnerActionsInputEnvName(name string) string {
	return "INPUT_" + strings.Map(func(r rune) rune {
		switch {
		case r >= 'a' && r <= 'z':
			return r - 'a' + 'A'
		case r >= 'A' && r <= 'Z', r >= '0' && r <= '9', r == '_':
			return r
		}
		return '_'
	}, name)
}

func runnerActionsActor(event map[string]any) string {
	for _, path := range [][]string{{"sender", "login"}, {"pusher", "name"}, {"issue", "user", "login"}} {
		var current any = event
		for _, key := range path {
			object, _ := current.(map[string]any)
			current = object[key]
		}
		if value, _ := current.(string); value != "" {
			return value
		}
	}
	return ""
}

func (a *runnerActions) runWorkflow(ctx context.Context, job runnerActionsJob, cache string, lock *sync.Mutex,
	commit string, index int, run *runnerActionsRun, live *runnerActionsLog) string {
	secrets := map[string]string{}
	if job.Trusted {
		for name, value := range job.Secrets {
			secrets[name] = value
		}
	}
	masks := []string{}
	for _, value := range secrets {
		masks = append(masks, value)
	}
	if job.Trusted {
		masks = append(masks, job.JobToken, job.PushGrant)
	} else if job.JobToken != "" {
		masks = append(masks, job.JobToken)
	}
	sink := newMaskingWriter(live, masks)
	defer sink.Flush()
	root := filepath.Join(a.b.cfg.WorkRoot, runnerActionsDir)
	if err := ensureDaemonDir(root, 0o711); err != nil {
		fmt.Fprintf(sink, "==> ERROR: the run workspace could not be prepared.\n")
		return actionFailed
	}
	work := filepath.Join(root, strconv.FormatInt(job.JobID, 10)+"-"+strconv.Itoa(index))
	if err := removeTreeUnder(root, work); err != nil {
		fmt.Fprintf(sink, "==> ERROR: the run workspace could not be prepared.\n")
		return actionFailed
	}
	defer func() {
		if err := removeTreeUnder(root, work); err != nil {
			log.Printf("runner actions: %v", err)
		}
	}()
	wf := run.wf
	pushGrant := ""
	if wf.ContentsWrite {
		pushGrant = a.pushGrant(job)
	}
	payload := []byte(job.EventPayload)
	if len(payload) == 0 || string(payload) == "null" {
		payload = []byte("{}")
	}
	if err := a.prepare(ctx, cache, lock, job, commit, work, pushGrant, payload); err != nil {
		fmt.Fprintf(sink, "==> ERROR: the run workspace could not be prepared: %s\n", err.Error())
		return actionFailed
	}
	limit := actionsRunTimeout
	if wf.TimeoutMinutes > 0 {
		limit = time.Duration(min(wf.TimeoutMinutes, runnerActionsMaxMinutes)) * time.Minute
	}
	runCtx, cancel := context.WithTimeout(ctx, limit)
	defer cancel()

	var event map[string]any
	_ = json.Unmarshal(payload, &event)
	event = exprEventMap(event)
	repository := strings.ToLower(job.RepositoryOwner) + "/" + job.RepositoryName
	src := filepath.Join(work, "src")
	eventPath := filepath.Join(work, "event.json")
	expr := &exprContext{EventName: job.event(), Ref: run.Ref, RefName: job.Branch, SHA: commit,
		Repository: repository, Actor: runnerActionsActor(event), Event: event, Inputs: job.Inputs,
		Secrets: secrets, Env: map[string]string{}}

	base := &runnerActionsEnv{}
	for _, pair := range [][2]string{
		{"HOME", filepath.Join(work, "home")},
		{"TMPDIR", filepath.Join(work, "tmp")},
		{"PATH", "/usr/local/bin:/usr/bin:/bin"},
		{"LANG", "C.UTF-8"},
		{"CI", "true"},
		{"FORKMESH_ACTIONS", "1"},
		{"FORKMESH_REPO", repository},
		{"FORKMESH_COMMIT", commit},
		{"FORKMESH_REF", run.Ref},
		{"FORKMESH_BEFORE", job.Before},
		{"FORKMESH_EVENT", job.event()},
		{"FORKMESH_NODE", a.b.node},
		{"FORKMESH_EVENT_PATH", eventPath},
		{"FORKMESH_API_URL", job.APIURL},
		{"GITHUB_EVENT_NAME", job.event()},
		{"GITHUB_EVENT_PATH", eventPath},
		{"GITHUB_REPOSITORY", repository},
		{"GITHUB_SHA", commit},
		{"GITHUB_REF", run.Ref},
		{"GITHUB_REF_NAME", job.Branch},
		{"GITHUB_WORKSPACE", src},
		{"GITHUB_ACTOR", expr.Actor},
	} {
		base.set(pair[0], pair[1])
	}
	// A trusted job's token, or the one an untrusted issue or comment job
	// carries for the job API (this repository's issues only).
	if job.JobToken != "" {
		base.set("FORKMESH_TOKEN", job.JobToken)
		base.set("GITHUB_TOKEN", job.JobToken)
	}
	inputNames := make([]string, 0, len(job.Inputs))
	for name := range job.Inputs {
		inputNames = append(inputNames, name)
	}
	sort.Strings(inputNames)
	for _, name := range inputNames {
		base.set(runnerActionsInputEnvName(name), job.Inputs[name])
	}
	reserved := base.clone()
	secretNames := make([]string, 0, len(secrets))
	for name := range secrets {
		if validActionsEnvName(name) && !reserved.has(name) {
			secretNames = append(secretNames, name)
		}
	}
	sort.Strings(secretNames)
	for _, name := range secretNames {
		base.set(name, secrets[name])
	}
	workflowEnv := map[string]string{}
	for _, pair := range wf.Env {
		value, err := interpolateExpr(pair[1], expr)
		if err != nil {
			fmt.Fprintf(sink, "==> ERROR: the workflow env %s has an unsupported expression.\n", singleLine(pair[0], 64))
			return actionFailed
		}
		expr.Env[pair[0]] = value
		workflowEnv[pair[0]] = value
		if validActionsEnvName(pair[0]) {
			base.set(pair[0], value)
		}
	}

	fmt.Fprintf(sink, "==> %s at %s on %s (runner %s; event %s)\n", wf.Name,
		boundedRunes(commit, 12), job.Branch, a.b.node, job.event())
	if pushGrant != "" {
		fmt.Fprintf(sink, "==> Sandbox: the runner's job user and limits; this checkout may push to %s.\n", job.Branch)
	} else {
		fmt.Fprintf(sink, "==> Sandbox: the runner's job user and limits, no Actions variables.\n")
	}
	failed := false
	jobVerdicts := map[string]bool{}
	for i, step := range wf.Steps {
		if runCtx.Err() != nil {
			break
		}
		expr.Failed = failed
		expr.Env = map[string]string{}
		for key, value := range workflowEnv {
			expr.Env[key] = value
		}
		runJob, known := jobVerdicts[step.Job]
		if !known {
			verdict, err := evalCondition(step.JobIf, expr)
			if err != nil {
				fmt.Fprintf(sink, "\n==> ERROR: the job %q has an unsupported if: expression.\n", step.Job)
				failed = true
				verdict = false
			}
			jobVerdicts[step.Job], runJob = verdict, verdict
		}
		if !runJob {
			fmt.Fprintf(sink, "\n==> Skipped: %s\n", step.Name)
			continue
		}
		stepEnv := base.clone()
		envFailed := false
		for _, pair := range step.Env {
			value, err := interpolateExpr(pair[1], expr)
			if err != nil {
				envFailed = true
				break
			}
			expr.Env[pair[0]] = value
			if validActionsEnvName(pair[0]) {
				stepEnv.set(pair[0], value)
			}
		}
		if envFailed {
			fmt.Fprintf(sink, "\n==> ERROR: step %q has an unsupported expression in env.\n", step.Name)
			failed = true
			continue
		}
		verdict, err := evalCondition(step.If, expr)
		if err != nil {
			fmt.Fprintf(sink, "\n==> ERROR: step %q has an unsupported if: expression.\n", step.Name)
			failed = true
			continue
		}
		if !verdict {
			fmt.Fprintf(sink, "\n==> Skipped: %s\n", step.Name)
			continue
		}
		fmt.Fprintf(sink, "\n==> Step %d/%d: %s\n", i+1, len(wf.Steps), step.Name)
		script, blanked := substituteWorkflowVars(step.Run)
		if blanked {
			fmt.Fprintf(sink, "==> Note: this step reads Actions variables; a runner holds none, so they are empty.\n")
		}
		script, exprEnv, err := substituteRunExpr(script, expr, func(name string) bool {
			_, own := expr.Env[name]
			return containsString(secretNames, name) && !own
		})
		if err != nil {
			fmt.Fprintf(sink, "==> ERROR: step %q has an unsupported expression.\n", step.Name)
			failed = true
			continue
		}
		for _, pair := range exprEnv {
			stepEnv.set(pair[0], pair[1])
		}
		dir := src
		if step.WorkingDirectory != "" {
			dir = filepath.Join(src, filepath.FromSlash(step.WorkingDirectory))
			if info, err := os.Stat(dir); err != nil || !info.IsDir() {
				fmt.Fprintf(sink, "==> ERROR: the working directory %s is not in the checkout.\n", step.WorkingDirectory)
				failed = true
				continue
			}
		}
		stepCtx, stepCancel := runCtx, context.CancelFunc(func() {})
		if step.TimeoutMinutes > 0 {
			stepCtx, stepCancel = context.WithTimeout(runCtx, time.Duration(step.TimeoutMinutes)*time.Minute)
		}
		code, err := a.step(stepCtx, dir, stepEnv.list(), script, sink)
		stepTimedOut := runCtx.Err() == nil && errors.Is(stepCtx.Err(), context.DeadlineExceeded)
		stepCancel()
		switch {
		case runCtx.Err() != nil:
		case stepTimedOut:
			fmt.Fprintf(sink, "\n==> FAILED: step %q exceeded its time limit.\n", step.Name)
			failed = true
		case err != nil:
			fmt.Fprintf(sink, "\n==> ERROR: the step could not start: %v\n", err)
			failed = true
		case code != 0:
			fmt.Fprintf(sink, "\n==> FAILED: step %q exited with status %d.\n", step.Name, code)
			failed = true
		}
	}
	switch {
	case errors.Is(runCtx.Err(), context.DeadlineExceeded):
		fmt.Fprintf(sink, "\n==> FAILED: the run exceeded its time limit.\n")
		return actionFailed
	case runCtx.Err() != nil:
		fmt.Fprintf(sink, "\n==> Cancelled.\n")
		return actionCancelled
	case failed:
		return actionFailed
	}
	fmt.Fprintf(sink, "\n==> SUCCESS: all steps completed.\n")
	return actionSuccess
}

func (a *runnerActions) prepare(ctx context.Context, cache string, lock *sync.Mutex, job runnerActionsJob,
	commit, work, pushGrant string, payload []byte) error {
	for _, dir := range []string{work, filepath.Join(work, "home"), filepath.Join(work, "tmp")} {
		if err := os.Mkdir(dir, 0o700); err != nil {
			return err
		}
	}
	src := filepath.Join(work, "src")
	lock.Lock()
	_, err := a.b.daemonGit(ctx, "", "", "", true, "clone", "--quiet", "--no-hardlinks", "--no-checkout",
		"--", cache, src)
	lock.Unlock()
	if err != nil {
		return errors.New("the repository could not be cloned")
	}
	steps := [][]string{
		{"checkout", "--quiet", "-B", job.Branch, commit},
		{"remote", "set-url", "origin", job.CloneURL},
	}
	if job.BotName != "" && job.BotEmail != "" {
		steps = append(steps, []string{"config", "user.name", job.BotName},
			[]string{"config", "user.email", job.BotEmail})
	}
	if pushGrant != "" {
		steps = append(steps, []string{"config", "http.extraHeader", runnerGrantHeader + ": " + pushGrant},
			[]string{"config", "branch." + job.Branch + ".remote", "origin"},
			[]string{"config", "branch." + job.Branch + ".merge", "refs/heads/" + job.Branch})
	}
	for _, args := range steps {
		if _, err := a.b.daemonGit(ctx, src, "", "", false, args...); err != nil {
			return errors.New("the commit could not be checked out")
		}
	}
	if err := os.WriteFile(filepath.Join(work, "event.json"), payload, 0o600); err != nil {
		return err
	}
	if a.b.box.uid != 0 {
		return chownTree(work, a.b.box.uid, a.b.box.gidOrUID())
	}
	return nil
}

func (a *runnerActions) runStep(ctx context.Context, work string, env []string, script string, sink io.Writer) (int, error) {
	shell := "/bin/sh"
	if _, err := os.Stat("/bin/bash"); err == nil {
		shell = "/bin/bash"
	}
	if !strings.HasSuffix(script, "\n") {
		script += "\n"
	}
	args, stdin := runnerStepInvocation(shell, env, script)
	stderr, err := a.b.box.runner()(ctx, work, stdin, sink, "/usr/bin/env", args...)
	_, _ = sink.Write(stderr)
	var exit *exec.ExitError
	if errors.As(err, &exit) {
		return exit.ExitCode(), nil
	}
	return 0, err
}

var runnerStepEnvName = regexp.MustCompile(`^[A-Za-z_][A-Za-z0-9_]*$`)

// runnerStepInvocation is the `env` argv and the stdin script of one step.
// The step's environment (secrets, the job token) travels in the script on
// stdin, never in argv: a process's command line is readable by every local
// user through /proc/<pid>/cmdline and ps, and is what exec auditing logs,
// while the pipe and the shell's own environment are not.
func runnerStepInvocation(shell string, env []string, script string) ([]string, string) {
	var prelude strings.Builder
	prelude.WriteString("exec 2>&1\n")
	for _, pair := range env {
		name, value, ok := strings.Cut(pair, "=")
		if !ok || !runnerStepEnvName.MatchString(name) {
			continue
		}
		prelude.WriteString("export " + name + "='" +
			strings.ReplaceAll(value, "'", `'\''`) + "' || :\n")
	}
	prelude.WriteString("ulimit -c 0 2>/dev/null || true\nulimit -f 2097152 2>/dev/null || true\n")
	return []string{"-i", shell, "-e", "-s"}, prelude.String() + script
}

type runnerActionsRunWire struct {
	*runnerActionsRun
	Log *string `json:"log,omitempty"`
}

type runnerActionsSentLog struct {
	run *runnerActionsRun
	log string
}

func markRunnerActionsLogsSent(sent []runnerActionsSentLog) {
	for _, item := range sent {
		item.run.sentLog = item.log
	}
}

func runnerActionsCancelRequested(reply []byte) bool {
	var answer struct {
		Cancel bool `json:"cancel"`
	}
	return json.Unmarshal(reply, &answer) == nil && answer.Cancel
}

func (a *runnerActions) reportBody(job runnerActionsJob, status string, runs []*runnerActionsRun, commit, message string) ([]byte, []runnerActionsSentLog, error) {
	catalog := job.catalog
	if catalog == nil {
		catalog = []runnerActionsCatalogEntry{}
	}
	wire := make([]runnerActionsRunWire, len(runs))
	for i, run := range runs {
		wire[i].runnerActionsRun = run
	}
	envelope := map[string]any{
		"jobId": job.JobID, "leaseId": job.LeaseID, "status": status,
		"commit": commit, "runs": wire, "message": message, "catalog": catalog,
	}
	base, err := json.Marshal(envelope)
	if err != nil {
		return nil, nil, err
	}
	order := make([]int, 0, len(runs))
	for i, run := range runs {
		if run.Log != run.sentLog {
			order = append(order, i)
		}
	}
	sort.SliceStable(order, func(x, y int) bool {
		return runs[order[x]].StartedAt > runs[order[y]].StartedAt
	})
	size := len(base)
	var sent []runnerActionsSentLog
	for _, i := range order {
		encoded, err := json.Marshal(runs[i].Log)
		if err != nil {
			continue
		}
		cost := len(encoded) + len(`,"log":`)
		if size+cost > runnerActionsReportMax {
			continue
		}
		size += cost
		text := runs[i].Log
		wire[i].Log = &text
		sent = append(sent, runnerActionsSentLog{run: runs[i], log: text})
	}
	if len(sent) == 0 {
		return base, nil, nil
	}
	body, err := json.Marshal(envelope)
	return body, sent, err
}

func (a *runnerActions) report(ctx context.Context, job runnerActionsJob, status string, runs []*runnerActionsRun, commit, message string) bool {
	body, sent, err := a.reportBody(job, status, runs, commit, message)
	if err != nil {
		return false
	}
	for attempt := 0; attempt < 3; attempt++ {
		code, reply, err := a.post(ctx, body)
		if err == nil && code >= 200 && code < 300 {
			markRunnerActionsLogsSent(sent)
			return runnerActionsCancelRequested(reply)
		}
		if err == nil && code >= 400 && code < 500 {
			log.Printf("runner actions: report for job %d refused (HTTP %d): %s", job.JobID, code, boundedText(reply, 200))
			return false
		}
		select {
		case <-ctx.Done():
			return false
		case <-time.After(time.Duration(attempt+1) * 2 * time.Second):
		}
	}
	log.Printf("runner actions: report for job %d was not delivered", job.JobID)
	return false
}

type tailBuffer struct {
	limit int
	data  []byte
}

func (t *tailBuffer) Write(p []byte) (int, error) {
	t.data = append(t.data, p...)
	if over := len(t.data) - t.limit; over > 0 {
		t.data = append(t.data[:0:0], t.data[over:]...)
	}
	return len(p), nil
}

func (t *tailBuffer) String() string {
	return strings.ToValidUTF8(string(t.data), "")
}

type runnerActionsLog struct {
	mu      sync.Mutex
	head    []byte
	tail    tailBuffer
	omitted int64
}

func (l *runnerActionsLog) Write(p []byte) (int, error) {
	l.mu.Lock()
	defer l.mu.Unlock()
	headLimit := runnerActionsLogMax - runnerActionsLogEnd
	rest := p
	if room := headLimit - len(l.head); room > 0 {
		take := min(room, len(rest))
		l.head = append(l.head, rest[:take]...)
		rest = rest[take:]
	}
	if len(rest) > 0 {
		l.tail.limit = runnerActionsLogEnd
		before := len(l.tail.data) + len(rest)
		_, _ = l.tail.Write(rest)
		if dropped := before - len(l.tail.data); dropped > 0 {
			l.omitted += int64(dropped)
		}
	}
	return len(p), nil
}

func (l *runnerActionsLog) String() string {
	l.mu.Lock()
	defer l.mu.Unlock()
	out := string(l.head)
	if l.omitted > 0 {
		out += fmt.Sprintf("\n==> %d bytes of output omitted.\n", l.omitted)
	}
	return strings.ToValidUTF8(out+string(l.tail.data), "")
}

func (l *runnerActionsLog) Tail() string {
	full := l.String()
	if len(full) <= runnerActionsLogTail {
		return full
	}
	return strings.ToValidUTF8(full[len(full)-runnerActionsLogTail:], "")
}

type maskingWriter struct {
	mu      sync.Mutex
	dst     io.Writer
	masks   []string
	pending []byte
}

func newMaskingWriter(dst io.Writer, values []string) *maskingWriter {
	var masks []string
	for _, value := range values {
		for _, line := range strings.Split(value, "\n") {
			line = strings.TrimRight(line, "\r")
			if len(line) >= 4 && !containsString(masks, line) {
				masks = append(masks, line)
			}
		}
	}
	sort.Slice(masks, func(i, j int) bool { return len(masks[i]) > len(masks[j]) })
	return &maskingWriter{dst: dst, masks: masks}
}

func (m *maskingWriter) Write(p []byte) (int, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	m.pending = append(m.pending, p...)
	for {
		end := bytes.IndexByte(m.pending, '\n')
		if end < 0 {
			break
		}
		m.emit(m.pending[:end+1])
		m.pending = m.pending[end+1:]
	}
	if len(m.pending) > 64<<10 {
		m.emit(m.pending)
		m.pending = nil
	}
	return len(p), nil
}

func (m *maskingWriter) Flush() {
	m.mu.Lock()
	defer m.mu.Unlock()
	if len(m.pending) > 0 {
		m.emit(m.pending)
		m.pending = nil
	}
}

func (m *maskingWriter) emit(line []byte) {
	text := string(line)
	for _, mask := range m.masks {
		text = strings.ReplaceAll(text, mask, "***")
	}
	_, _ = io.WriteString(m.dst, text)
}
