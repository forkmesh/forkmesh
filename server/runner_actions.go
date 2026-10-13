package mirrornode

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"os"
	"os/exec"
	"path/filepath"
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
	runnerActionsMaxRuns    = 32
)

type runnerActionsJob struct {
	Kind            string `json:"kind"`
	JobID           int64  `json:"jobId"`
	LeaseID         string `json:"leaseId"`
	RepositoryOwner string `json:"repositoryOwner"`
	RepositoryName  string `json:"repositoryName"`
	Branch          string `json:"branch"`
	Commit          string `json:"commit"`
	CloneURL        string `json:"cloneUrl"`
	ReadGrant       string `json:"readGrant"`
	GrantExpiresAt  int64  `json:"grantExpiresAt"`
	TimeoutMs       int64  `json:"timeoutMs"`
}

type runnerActionsRun struct {
	Workflow   string `json:"workflow"`
	Commit     string `json:"commit"`
	Ref        string `json:"ref"`
	Status     string `json:"status"`
	CreatedAt  int64  `json:"createdAt"`
	StartedAt  int64  `json:"startedAt"`
	FinishedAt int64  `json:"finishedAt"`
	LogTail    string `json:"logTail"`

	path  string
	name  string
	needs []string
	wf    *workflow
}

type runnerActions struct {
	b    *runnerBackend
	busy atomic.Bool
	runs sync.WaitGroup
	now  func() time.Time
	git  func(ctx context.Context, gitDir string, args ...string) (string, error)
	step func(ctx context.Context, work string, env []string, script string, sink io.Writer) (int, error)
	post func(ctx context.Context, body []byte) (int, []byte, error)
}

func newRunnerActions(b *runnerBackend) *runnerActions {
	a := &runnerActions{b: b, now: time.Now}
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
		(job.Commit != "" && !runnerObjectID.MatchString(job.Commit)) {
		return "Malformed or unsafe Actions job."
	}
	if len(a.b.cfg.AllowedOwners) != 0 && !containsString(a.b.cfg.AllowedOwners, owner) {
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
	runs, commit, problem := a.plan(ctx, cache, job)
	lock.Unlock()
	if problem != "" {
		a.report(ctx, job, "completed", nil, commit, problem)
		return
	}
	if len(runs) == 0 {
		a.report(ctx, job, "completed", nil, commit, "No workflow runs on a push to "+job.Branch+" on a runner.")
		return
	}
	a.report(ctx, job, "running", runs, commit, "")
	for {
		index, verdict := nextRunnerActionsRun(runs)
		if index < 0 {
			break
		}
		run := runs[index]
		if verdict == "skip" {
			run.Status, run.FinishedAt = actionSkipped, a.now().UnixMilli()
			run.LogTail = "==> Skipped: a workflow it needs did not succeed.\n"
			continue
		}
		run.Status, run.StartedAt = actionRunning, a.now().UnixMilli()
		a.report(ctx, job, "running", runs, commit, "")
		status, logTail := a.runWorkflow(ctx, job, cache, lock, commit, index, run)
		run.Status, run.LogTail, run.FinishedAt = status, logTail, a.now().UnixMilli()
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

func (a *runnerActions) plan(ctx context.Context, cache string, job runnerActionsJob) ([]*runnerActionsRun, string, string) {
	ref := "refs/heads/" + job.Branch
	tip, err := a.git(ctx, cache, "rev-parse", "--verify", "-q", ref+"^{commit}")
	if err != nil || !runnerObjectID.MatchString(tip) {
		return nil, "", "The branch " + job.Branch + " is not on the core."
	}
	commit := tip
	if job.Commit != "" {
		if _, err := a.git(ctx, cache, "merge-base", "--is-ancestor", job.Commit, tip); err != nil {
			return nil, "", "The pushed commit is not on " + job.Branch + "; nothing ran."
		}
		commit = job.Commit
	}
	out, err := a.git(ctx, cache, "ls-tree", "--name-only", commit, ".forkmesh/")
	if err != nil {
		return nil, commit, "The workflows could not be listed."
	}
	created := a.now().UnixMilli()
	var runs []*runnerActionsRun
	for _, name := range strings.Split(out, "\n") {
		if !workflowPathPattern.MatchString(name) || len(runs) == runnerActionsMaxRuns {
			continue
		}
		content, err := a.git(ctx, cache, "show", commit+":"+name)
		if err != nil {
			continue
		}
		wf, err := parseWorkflow(name, content)
		if err != nil || !wf.on("push") || !wf.runsOnThisNode(a.labels()) ||
			(len(wf.PushBranches) != 0 && !wf.matchesPushBranch(job.Branch)) {
			continue
		}
		runs = append(runs, &runnerActionsRun{Workflow: wf.Name, Commit: commit, Ref: ref,
			Status: actionQueued, CreatedAt: created, path: wf.Path, name: wf.Name,
			needs: wf.Needs, wf: wf})
	}
	return runs, commit, ""
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

func (a *runnerActions) runWorkflow(ctx context.Context, job runnerActionsJob, cache string, lock *sync.Mutex,
	commit string, index int, run *runnerActionsRun) (string, string) {
	sink := &tailBuffer{limit: runnerActionsLogTail}
	root := filepath.Join(a.b.cfg.WorkRoot, runnerActionsDir)
	if err := ensureDaemonDir(root, 0o711); err != nil {
		return actionFailed, "==> ERROR: the run workspace could not be prepared.\n"
	}
	work := filepath.Join(root, strconv.FormatInt(job.JobID, 10)+"-"+strconv.Itoa(index))
	if err := removeTreeUnder(root, work); err != nil {
		return actionFailed, "==> ERROR: the run workspace could not be prepared.\n"
	}
	defer func() {
		if err := removeTreeUnder(root, work); err != nil {
			log.Printf("runner actions: %v", err)
		}
	}()
	if err := a.prepare(ctx, cache, lock, commit, work); err != nil {
		return actionFailed, "==> ERROR: the run workspace could not be prepared: " + err.Error() + "\n"
	}
	runCtx, cancel := context.WithTimeout(ctx, actionsRunTimeout)
	defer cancel()
	wf := run.wf
	env := []string{
		"HOME=" + filepath.Join(work, "home"),
		"TMPDIR=" + filepath.Join(work, "tmp"),
		"PATH=/usr/local/bin:/usr/bin:/bin",
		"LANG=C.UTF-8",
		"CI=true",
		"FORKMESH_ACTIONS=1",
		"FORKMESH_REPO=" + strings.ToLower(job.RepositoryOwner) + "/" + job.RepositoryName,
		"FORKMESH_COMMIT=" + commit,
		"FORKMESH_REF=" + run.Ref,
		"FORKMESH_EVENT=push",
		"FORKMESH_NODE=" + a.b.node,
	}
	for _, pair := range wf.Env {
		if validActionsEnvName(pair[0]) {
			env = append(env, pair[0]+"="+pair[1])
		}
	}
	fmt.Fprintf(sink, "==> %s at %s on %s (runner %s; the desktop was offline)\n", wf.Name,
		boundedRunes(commit, 12), strings.TrimPrefix(run.Ref, "refs/heads/"), a.b.node)
	fmt.Fprintf(sink, "==> Sandbox: the runner's job user and limits, no Actions variables.\n")
	for i, step := range wf.Steps {
		fmt.Fprintf(sink, "\n==> Step %d/%d: %s\n", i+1, len(wf.Steps), step.Name)
		script, blanked := substituteWorkflowVars(step.Run)
		if blanked {
			fmt.Fprintf(sink, "==> Note: this step reads Actions variables; a runner holds none, so they are empty.\n")
		}
		code, err := a.step(runCtx, filepath.Join(work, "src"), env, script, sink)
		switch {
		case runCtx.Err() != nil:
		case err != nil:
			fmt.Fprintf(sink, "\n==> ERROR: the step could not start: %v\n", err)
			return actionFailed, sink.String()
		case code != 0:
			fmt.Fprintf(sink, "\n==> FAILED: step %q exited with status %d.\n", step.Name, code)
			return actionFailed, sink.String()
		default:
			continue
		}
		break
	}
	switch {
	case errors.Is(runCtx.Err(), context.DeadlineExceeded):
		fmt.Fprintf(sink, "\n==> FAILED: the run exceeded its time limit.\n")
		return actionFailed, sink.String()
	case runCtx.Err() != nil:
		fmt.Fprintf(sink, "\n==> Cancelled.\n")
		return actionCancelled, sink.String()
	}
	fmt.Fprintf(sink, "\n==> SUCCESS: all steps completed.\n")
	return actionSuccess, sink.String()
}

func (a *runnerActions) prepare(ctx context.Context, cache string, lock *sync.Mutex, commit, work string) error {
	for _, dir := range []string{work, filepath.Join(work, "src"), filepath.Join(work, "home"), filepath.Join(work, "tmp")} {
		if err := os.Mkdir(dir, 0o700); err != nil {
			return err
		}
	}
	lock.Lock()
	err := exportGitCommit(ctx, cache, commit, filepath.Join(work, "src"))
	lock.Unlock()
	if err != nil {
		return errors.New("the commit could not be checked out")
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
	args := append(append([]string{"-i"}, env...), shell, "-e", "-s")
	stdin := "exec 2>&1\nulimit -c 0 2>/dev/null || true\nulimit -f 2097152 2>/dev/null || true\n" + script
	stderr, err := a.b.box.runner()(ctx, work, stdin, sink, "/usr/bin/env", args...)
	_, _ = sink.Write(stderr)
	var exit *exec.ExitError
	if errors.As(err, &exit) {
		return exit.ExitCode(), nil
	}
	return 0, err
}

func (a *runnerActions) report(ctx context.Context, job runnerActionsJob, status string, runs []*runnerActionsRun, commit, message string) {
	if runs == nil {
		runs = []*runnerActionsRun{}
	}
	body, err := json.Marshal(map[string]any{
		"jobId": job.JobID, "leaseId": job.LeaseID, "status": status,
		"commit": commit, "runs": runs, "message": message,
	})
	if err != nil {
		return
	}
	for attempt := 0; attempt < 3; attempt++ {
		code, reply, err := a.post(ctx, body)
		if err == nil && code >= 200 && code < 300 {
			return
		}
		if err == nil && code >= 400 && code < 500 {
			log.Printf("runner actions: report for job %d refused (HTTP %d): %s", job.JobID, code, boundedText(reply, 200))
			return
		}
		select {
		case <-ctx.Done():
			return
		case <-time.After(time.Duration(attempt+1) * 2 * time.Second):
		}
	}
	log.Printf("runner actions: report for job %d was not delivered", job.JobID)
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
