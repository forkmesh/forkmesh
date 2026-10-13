package mirrornode

// Mirror Actions runner.
//
// A mirror that opts in (`enableActions` in mirror-node.json) runs the
// .forkmesh/*.yml workflows of the repositories it serves, publishes a
// redacted summary the gateway's actions-status operation reads, and consumes
// the owner-authorized control requests the gateway's actions-control
// operation couriers in (actions_control.go). Wire contract:
// www/docs/operations/mirror-actions-control.md.
//
// This executes shell commands a repository supplies, on a machine that also
// holds the node's signing key, so the rules are strict:
//
//   - Opt-in, and fail closed. Without a working bubblewrap sandbox nothing
//     runs and the two Actions operations are never advertised.
//   - A step sees a read-only /usr, an exported copy of the commit, and a
//     scratch home. It has no network, a cleared environment, and no view of
//     this node's state directory, identity, or served repositories.
//   - A mirror holds no Actions variables. It is somebody else's machine, so a
//     secret placed here would be readable by its operator; `${{ vars.X }}`
//     expands to nothing and the log says so.
//   - One run at a time: these are small nodes whose first job is serving Git.

import (
	"bufio"
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
	"sort"
	"strings"
	"sync"
	"syscall"
	"time"
	"unicode/utf8"
)

const (
	actionsDirectoryName     = "actions"
	actionsSummaryFileName   = "actions-summary.json"
	actionsSummaryType       = "forkmesh.mirror-actions-summary"
	actionsTickInterval      = 4 * time.Second
	actionsSummaryLease      = 10 * time.Minute
	actionsSummaryRefresh    = 3 * time.Minute
	actionsSandboxRetry      = 10 * time.Minute
	actionsRunTimeout        = time.Hour
	actionsRunRetention      = 50
	actionsQueueLimit        = 20
	maxActionsSummaryBytes   = 256 << 10
	maxActionsSummaryRuns    = 20
	maxActionsLogTailBytes   = 16 << 10
	maxActionsLogBytes       = 8 << 20
	maxActionsRepositories   = 64
	maxActionsRepoWorkflows  = 32
	maxActionsChangedPaths   = 4000
	actionsInternalRefPrefix = "refs/heads/forkmesh/"

	actionQueued    = "queued"
	actionRunning   = "running"
	actionSuccess   = "success"
	actionFailed    = "failed"
	actionCancelled = "cancelled"
	actionSkipped   = "skipped"

	stepPending   = "pending"
	stepRunning   = "running"
	stepSuccess   = "success"
	stepFailed    = "failed"
	stepSkipped   = "skipped"
	stepCancelled = "cancelled"
)

type actionRun struct {
	ID           int64    `json:"id"`
	Owner        string   `json:"owner"`
	Repository   string   `json:"repository"`
	WorkflowPath string   `json:"workflowPath"`
	WorkflowName string   `json:"workflowName"`
	Commit       string   `json:"commit"`
	Ref          string   `json:"ref"`
	Event        string   `json:"event"`
	Actor        string   `json:"actor,omitempty"`
	Status       string   `json:"status"`
	StepNames    []string `json:"stepNames"`
	StepStatuses []string `json:"stepStatuses"`
	Needs        []string `json:"needs,omitempty"`
	CreatedAt    int64    `json:"createdAt"`
	StartedAt    int64    `json:"startedAt"`
	FinishedAt   int64    `json:"finishedAt"`
}

func (r *actionRun) terminal() bool {
	return r.Status != actionQueued && r.Status != actionRunning
}

type actionsRepositoryState struct {
	Heads             map[string]string `json:"heads"`
	DisabledWorkflows []string          `json:"disabledWorkflows,omitempty"`
	// Set from the web. It can only narrow: the node's operator decides
	// whether this runner exists at all.
	WebDisabled bool `json:"webDisabled,omitempty"`
}

type actionsControlOutcome struct {
	Owner      string `json:"owner"`
	Repository string `json:"repository"`
	RequestID  string `json:"requestId"`
	Action     string `json:"action"`
	Status     string `json:"status"`
	Reason     string `json:"reason"`
	RunID      int64  `json:"runId"`
	FinishedAt int64  `json:"finishedAt"`
}

type actionsState struct {
	NextRunID    int64                              `json:"nextRunId"`
	Runs         []actionRun                        `json:"runs"`
	Repositories map[string]*actionsRepositoryState `json:"repositories"`
	Seen         []string                           `json:"seen,omitempty"`
	Outcomes     []actionsControlOutcome            `json:"outcomes,omitempty"`
}

type actionsCatalogEntry struct {
	head      string
	workflows []map[string]any
}

type actionsRunner struct {
	daemon *Daemon
	dir    string
	now    func() time.Time
	// Test seams: the sandbox argv prefix and its probe.
	sandbox      func(work string, env []string) (string, []string, error)
	probeSandbox func(ctx context.Context) error

	mu             sync.Mutex
	loaded         bool
	state          actionsState
	cancelRun      map[int64]context.CancelFunc
	running        int64
	catalog        map[string]actionsCatalogEntry
	summaryDirty   bool
	summaryWritten time.Time
	sandboxOK      bool
	sandboxChecked time.Time
	sandboxProblem string
	refsMoved      chan struct{}
	runCtx         context.Context
	runs           sync.WaitGroup
}

func newActionsRunner(d *Daemon) *actionsRunner {
	runner := &actionsRunner{
		daemon:    d,
		dir:       filepath.Join(filepath.Dir(d.config.GatewayConfig), actionsDirectoryName),
		now:       time.Now,
		cancelRun: map[int64]context.CancelFunc{},
		catalog:   map[string]actionsCatalogEntry{},
		refsMoved: make(chan struct{}, 1),
	}
	runner.sandbox = runner.bubblewrap
	runner.probeSandbox = runner.probeBubblewrap
	return runner
}

func (r *actionsRunner) gatewayDirectory() string {
	return filepath.Dir(r.daemon.config.GatewayConfig)
}
func (r *actionsRunner) summaryPath() string {
	return filepath.Join(r.gatewayDirectory(), actionsSummaryFileName)
}
func (r *actionsRunner) controlDirectory() string {
	return filepath.Join(r.gatewayDirectory(), actionsControlDirectoryName)
}
func (r *actionsRunner) statePath() string { return filepath.Join(r.dir, "state.json") }
func (r *actionsRunner) runDirectory(id int64) string {
	return filepath.Join(r.dir, "runs", fmt.Sprintf("%d", id))
}
func (r *actionsRunner) logPath(id int64) string {
	return filepath.Join(r.runDirectory(id), "log.txt")
}
func (r *actionsRunner) node() string { return r.daemon.gateway.Node.Name }

// labels are what `runs-on` is matched against.
func (r *actionsRunner) labels() []string { return []string{r.node(), "linux", "mirror"} }

// requestRefScan is the daemon cycle's doorbell: refs may have moved.
func (r *actionsRunner) requestRefScan() {
	select {
	case r.refsMoved <- struct{}{}:
	default:
	}
}

func (r *actionsRunner) loadLocked() {
	if r.loaded {
		return
	}
	r.loaded = true
	r.state = actionsState{NextRunID: 1, Repositories: map[string]*actionsRepositoryState{}}
	raw, err := os.ReadFile(r.statePath())
	if err == nil {
		var stored actionsState
		if json.Unmarshal(raw, &stored) == nil {
			r.state = stored
		}
	}
	if r.state.Repositories == nil {
		r.state.Repositories = map[string]*actionsRepositoryState{}
	}
	if r.state.NextRunID < 1 {
		r.state.NextRunID = 1
	}
	// A run that was in flight when the daemon stopped did not finish.
	now := r.now().UnixMilli()
	for i := range r.state.Runs {
		run := &r.state.Runs[i]
		if run.Status != actionRunning {
			continue
		}
		run.Status = actionCancelled
		run.FinishedAt = now
		for step, status := range run.StepStatuses {
			if status == stepRunning || status == stepPending {
				run.StepStatuses[step] = stepCancelled
			}
		}
		r.appendLog(run.ID, "\n==> The mirror daemon restarted while this run was in flight.\n")
	}
	r.summaryDirty = true
}

func (r *actionsRunner) saveLocked() {
	// Newest runs are kept; a pruned run's log and workspace go with it.
	if extra := len(r.state.Runs) - actionsRunRetention; extra > 0 {
		sort.SliceStable(r.state.Runs, func(i, j int) bool {
			return r.state.Runs[i].ID < r.state.Runs[j].ID
		})
		kept := r.state.Runs[:0:0]
		for _, run := range r.state.Runs {
			if extra > 0 && run.terminal() {
				extra--
				_ = removeTreeUnder(filepath.Join(r.dir, "runs"), r.runDirectory(run.ID))
				continue
			}
			kept = append(kept, run)
		}
		r.state.Runs = kept
	}
	if err := writePrivateJSON(r.statePath(), r.state); err != nil {
		log.Printf("mirror actions state was not saved: %v", err)
	}
	r.summaryDirty = true
}

func (r *actionsRunner) repositoryStateLocked(repo Repository) *actionsRepositoryState {
	key := repositoryKey(repo)
	state := r.state.Repositories[key]
	if state == nil {
		state = &actionsRepositoryState{}
		r.state.Repositories[key] = state
	}
	return state
}

func (r *actionsRunner) findRunLocked(id int64) *actionRun {
	for i := range r.state.Runs {
		if r.state.Runs[i].ID == id {
			return &r.state.Runs[i]
		}
	}
	return nil
}

func (r *actionsRunner) repositories() []Repository {
	r.daemon.mu.RLock()
	defer r.daemon.mu.RUnlock()
	var out []Repository
	for _, repo := range r.daemon.gateway.Repositories {
		if repo.Enabled && repo.Visibility == "public" && filepath.IsAbs(repo.GitDir) {
			out = append(out, repo)
		}
	}
	return out
}

func (r *actionsRunner) findRepository(owner, name string) (Repository, bool) {
	for _, repo := range r.repositories() {
		if strings.EqualFold(repo.Owner, owner) && strings.EqualFold(repo.Name, name) {
			return repo, true
		}
	}
	return Repository{}, false
}

// ---------------------------------------------------------------------------
// Sandbox.

// available reports whether runs may execute, probing at most every ten
// minutes while the sandbox is unusable.
func (r *actionsRunner) available(ctx context.Context) bool {
	r.mu.Lock()
	checked, ok := r.sandboxChecked, r.sandboxOK
	r.mu.Unlock()
	if ok || (!checked.IsZero() && r.now().Sub(checked) < actionsSandboxRetry) {
		return ok
	}
	err := r.probeSandbox(ctx)
	r.mu.Lock()
	defer r.mu.Unlock()
	r.sandboxChecked = r.now()
	r.sandboxOK = err == nil
	problem := ""
	if err != nil {
		problem = err.Error()
	}
	if problem != r.sandboxProblem {
		r.sandboxProblem = problem
		if err != nil {
			log.Printf("mirror actions are unavailable: %s", problem)
		} else {
			log.Printf("mirror actions sandbox is ready")
		}
	}
	return r.sandboxOK
}

func (r *actionsRunner) probeBubblewrap(ctx context.Context) error {
	work, err := os.MkdirTemp(r.ensureDirectory("probe"), "probe-")
	if err != nil {
		return err
	}
	defer removeTreeUnder(filepath.Dir(work), work)
	if err := prepareActionsWorkspace(work); err != nil {
		return err
	}
	name, args, err := r.bubblewrap(work, nil)
	if err != nil {
		return err
	}
	ctx, cancel := context.WithTimeout(ctx, 15*time.Second)
	defer cancel()
	command := exec.CommandContext(ctx, name, append(args, "/bin/sh", "-c", "exit 0")...)
	command.Env = []string{"PATH=/usr/bin:/bin"}
	output, err := command.CombinedOutput()
	if err != nil {
		// The packaged unit's RestrictNamespaces=true is the usual cause.
		return fmt.Errorf("bubblewrap cannot start a sandbox (%v): %s", err,
			boundedText(output, 300))
	}
	return nil
}

func (r *actionsRunner) ensureDirectory(name string) string {
	dir := filepath.Join(r.dir, name)
	_ = os.MkdirAll(dir, 0o700)
	return dir
}

// prepareActionsWorkspace lays out one run's scratch tree: the exported
// commit, a home, and the two account files a sandbox needs to name its user.
func prepareActionsWorkspace(work string) error {
	for _, dir := range []string{"src", "home", "etc"} {
		if err := os.MkdirAll(filepath.Join(work, dir), 0o700); err != nil {
			return err
		}
	}
	uid, gid := os.Getuid(), os.Getgid()
	passwd := fmt.Sprintf("forkmesh:x:%d:%d:ForkMesh Actions:/home/forkmesh:/bin/sh\n", uid, gid)
	group := fmt.Sprintf("forkmesh:x:%d:\n", gid)
	if err := os.WriteFile(filepath.Join(work, "etc", "passwd"), []byte(passwd), 0o644); err != nil {
		return err
	}
	return os.WriteFile(filepath.Join(work, "etc", "group"), []byte(group), 0o644)
}

// bubblewrap builds the sandbox argv. Nothing of this node is visible inside
// except a read-only /usr: not the state directory, not the identity key, not
// the served repositories, and no network namespace with a route out.
func (r *actionsRunner) bubblewrap(work string, env []string) (string, []string, error) {
	name, err := exec.LookPath("bwrap")
	if err != nil {
		return "", nil, errors.New("bubblewrap (bwrap) is not installed")
	}
	args := []string{
		"--unshare-all", "--die-with-parent", "--new-session", "--clearenv",
		"--hostname", "forkmesh-actions",
		"--ro-bind", "/usr", "/usr",
	}
	// /bin, /lib and friends are symlinks into /usr on a merged-/usr system
	// and real directories elsewhere; mirror whichever this host has.
	for _, dir := range []string{"/bin", "/sbin", "/lib", "/lib32", "/lib64", "/libx32"} {
		info, err := os.Lstat(dir)
		if err != nil {
			continue
		}
		if info.Mode()&os.ModeSymlink != 0 {
			if target, err := os.Readlink(dir); err == nil {
				args = append(args, "--symlink", target, dir)
			}
			continue
		}
		args = append(args, "--ro-bind", dir, dir)
	}
	for _, file := range []string{"/etc/alternatives", "/etc/ld.so.cache",
		"/etc/ld.so.conf", "/etc/ld.so.conf.d", "/etc/localtime"} {
		args = append(args, "--ro-bind-try", file, file)
	}
	args = append(args,
		"--ro-bind", filepath.Join(work, "etc", "passwd"), "/etc/passwd",
		"--ro-bind", filepath.Join(work, "etc", "group"), "/etc/group",
		"--proc", "/proc", "--dev", "/dev",
		"--tmpfs", "/tmp", "--tmpfs", "/run",
		"--bind", filepath.Join(work, "src"), "/workspace",
		"--bind", filepath.Join(work, "home"), "/home/forkmesh",
		"--chdir", "/workspace",
		"--setenv", "PATH", "/usr/local/bin:/usr/bin:/bin",
		"--setenv", "HOME", "/home/forkmesh",
		"--setenv", "LANG", "C.UTF-8",
		"--setenv", "CI", "true",
		"--setenv", "FORKMESH_ACTIONS", "1",
	)
	for _, pair := range env {
		if key, value, ok := strings.Cut(pair, "="); ok && validActionsEnvName(key) {
			args = append(args, "--setenv", key, value)
		}
	}
	return name, args, nil
}

func validActionsEnvName(name string) bool {
	if name == "" || len(name) > 128 {
		return false
	}
	for i, c := range name {
		switch {
		case c == '_', c >= 'A' && c <= 'Z', c >= 'a' && c <= 'z':
		case c >= '0' && c <= '9' && i > 0:
		default:
			return false
		}
	}
	// A workflow may not redirect the sandbox's own lookups.
	switch name {
	case "PATH", "HOME", "LD_PRELOAD", "LD_LIBRARY_PATH", "LD_AUDIT":
		return false
	}
	return true
}

// ---------------------------------------------------------------------------
// Git reads. Hooks never run, and nothing but this file tree is reachable.

func (r *actionsRunner) git(ctx context.Context, gitDir string, args ...string) (string, error) {
	command := exec.CommandContext(ctx, "git",
		append([]string{"--git-dir=" + gitDir, "-c", "core.hooksPath=/dev/null"}, args...)...)
	command.Env = pullGitEnvironment(nil)
	var stdout bytes.Buffer
	command.Stdout = &stdout
	if err := command.Run(); err != nil {
		return "", err
	}
	return stdout.String(), nil
}

func (r *actionsRunner) heads(ctx context.Context, gitDir string) (map[string]string, error) {
	out, err := r.git(ctx, gitDir, "for-each-ref", "--format=%(objectname) %(refname)", "refs/heads")
	if err != nil {
		return nil, err
	}
	heads := map[string]string{}
	for _, line := range strings.Split(strings.TrimSpace(out), "\n") {
		oid, ref, ok := strings.Cut(line, " ")
		// forkmesh/* are the node's own ledgers (pulls, issues); they move
		// constantly and are not anybody's push.
		if ok && !strings.HasPrefix(ref, actionsInternalRefPrefix) {
			heads[ref] = oid
		}
	}
	return heads, nil
}

func (r *actionsRunner) defaultBranch(ctx context.Context, gitDir string) string {
	out, err := r.git(ctx, gitDir, "symbolic-ref", "-q", "HEAD")
	if err != nil {
		return ""
	}
	return strings.TrimSpace(out)
}

// workflowsAt lists the workflow files at one commit. A file that does not
// parse is returned with its error so the catalog can say it is invalid.
type workflowAt struct {
	path     string
	workflow *workflow
	err      error
}

func (r *actionsRunner) workflowsAt(ctx context.Context, gitDir, commit string) []workflowAt {
	out, err := r.git(ctx, gitDir, "ls-tree", "--name-only", commit, ".forkmesh/")
	if err != nil {
		return nil
	}
	var found []workflowAt
	for _, name := range strings.Split(strings.TrimSpace(out), "\n") {
		if !workflowPathPattern.MatchString(name) {
			continue
		}
		if len(found) == maxActionsRepoWorkflows {
			break
		}
		found = append(found, r.workflowAt(ctx, gitDir, commit, name))
	}
	return found
}

func (r *actionsRunner) workflowAt(ctx context.Context, gitDir, commit, name string) workflowAt {
	content, err := r.git(ctx, gitDir, "show", commit+":"+name)
	if err != nil {
		return workflowAt{path: name, err: errors.New("workflow file is not on that commit")}
	}
	parsed, err := parseWorkflow(name, content)
	return workflowAt{path: name, workflow: parsed, err: err}
}

func (r *actionsRunner) changedPaths(ctx context.Context, gitDir, from, to string) ([]string, bool) {
	if from == "" {
		return nil, false
	}
	out, err := r.git(ctx, gitDir, "diff", "--name-only", "--no-renames", from, to)
	if err != nil {
		return nil, false
	}
	paths := strings.Split(strings.TrimSpace(out), "\n")
	if len(paths) > maxActionsChangedPaths {
		return nil, false
	}
	return paths, true
}

// ---------------------------------------------------------------------------
// Triggers.

// scanRefs turns moved branch heads into queued push runs. The first sight of
// a repository only records where its branches are: a node that just joined
// must not re-run the history it cloned.
func (r *actionsRunner) scanRefs(ctx context.Context) {
	for _, repo := range r.repositories() {
		heads, err := r.heads(ctx, repo.GitDir)
		if err != nil {
			continue
		}
		defaultRef := r.defaultBranch(ctx, repo.GitDir)
		r.mu.Lock()
		r.loadLocked()
		state := r.repositoryStateLocked(repo)
		first := state.Heads == nil
		previous := state.Heads
		state.Heads = heads
		webDisabled := state.WebDisabled
		disabled := append([]string(nil), state.DisabledWorkflows...)
		changed := first || !sameStringMap(previous, heads)
		if changed {
			r.saveLocked()
		}
		r.mu.Unlock()
		if first || !changed || webDisabled {
			continue
		}
		refs := make([]string, 0, len(heads))
		for ref := range heads {
			refs = append(refs, ref)
		}
		sort.Strings(refs)
		for _, ref := range refs {
			if previous[ref] == heads[ref] {
				continue
			}
			r.queuePush(ctx, repo, ref, previous[ref], heads[ref], ref == defaultRef, disabled)
		}
	}
	r.startNext()
}

func sameStringMap(left, right map[string]string) bool {
	if len(left) != len(right) {
		return false
	}
	for key, value := range left {
		if right[key] != value {
			return false
		}
	}
	return true
}

func (r *actionsRunner) queuePush(ctx context.Context, repo Repository, ref, from, to string, isDefault bool, disabled []string) {
	changed, known := r.changedPaths(ctx, repo.GitDir, from, to)
	for _, found := range r.workflowsAt(ctx, repo.GitDir, to) {
		wf := found.workflow
		if found.err != nil || !wf.on("push") || containsString(disabled, wf.Path) ||
			!wf.runsOnThisNode(r.labels()) {
			continue
		}
		// Without `on.push.branches` a push workflow follows the default
		// branch only. Mirrors receive every agent and topic branch, and a
		// small node cannot run CI for all of them.
		if len(wf.PushBranches) == 0 {
			if !isDefault {
				continue
			}
		} else if !wf.matchesPushBranch(strings.TrimPrefix(ref, "refs/heads/")) {
			continue
		}
		// An unknown diff (new branch, forced history) counts as a match.
		if known && !wf.matchesPushPaths(changed) {
			continue
		}
		r.createRun(repo, wf, to, ref, "push", "")
	}
}

func containsString(list []string, value string) bool {
	for _, item := range list {
		if item == value {
			return true
		}
	}
	return false
}

// createRun queues one run and supersedes older queued runs of the same
// workflow on the same ref: only the newest commit is worth a small node's
// time. It returns 0 when the queue is full.
func (r *actionsRunner) createRun(repo Repository, wf *workflow, commit, ref, event, actor string) int64 {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.loadLocked()
	queued := 0
	now := r.now().UnixMilli()
	for i := range r.state.Runs {
		run := &r.state.Runs[i]
		if run.Status != actionQueued {
			continue
		}
		if event == "push" && run.Event == "push" && run.Ref == ref &&
			run.WorkflowPath == wf.Path && strings.EqualFold(run.Owner, repo.Owner) &&
			strings.EqualFold(run.Repository, repo.Name) {
			run.Status = actionCancelled
			run.FinishedAt = now
			for step := range run.StepStatuses {
				run.StepStatuses[step] = stepCancelled
			}
			r.appendLog(run.ID, "==> Superseded by a newer push to "+ref+".\n")
			continue
		}
		queued++
	}
	if queued >= actionsQueueLimit {
		log.Printf("mirror actions queue is full; %s for %s/%s was not queued",
			wf.Path, repo.Owner, repo.Name)
		return 0
	}
	run := actionRun{
		ID: r.state.NextRunID, Owner: repo.Owner, Repository: repo.Name,
		WorkflowPath: wf.Path, WorkflowName: wf.Name, Commit: commit, Ref: ref,
		Event: event, Actor: actor, Status: actionQueued, Needs: wf.Needs,
		CreatedAt: now,
	}
	for _, step := range wf.Steps {
		run.StepNames = append(run.StepNames, step.Name)
		run.StepStatuses = append(run.StepStatuses, stepPending)
	}
	r.state.NextRunID++
	r.state.Runs = append(r.state.Runs, run)
	r.saveLocked()
	log.Printf("mirror actions queued %q for %s/%s at %s (%s)", wf.Name,
		repo.Owner, repo.Name, boundedRunes(commit, 12), event)
	return run.ID
}

// needsVerdict applies `needs:`. A needed workflow is looked up by name among
// this commit's runs. "wait" holds the run, "skip" means a dependency ended
// without succeeding, and a workflow that never ran here does not block.
func (r *actionsRunner) needsVerdictLocked(run *actionRun) string {
	verdict := "go"
	for _, needed := range run.Needs {
		var latest *actionRun
		for i := range r.state.Runs {
			other := &r.state.Runs[i]
			if other.ID != run.ID && other.Commit == run.Commit &&
				strings.EqualFold(other.Owner, run.Owner) &&
				strings.EqualFold(other.Repository, run.Repository) &&
				other.WorkflowName == needed && (latest == nil || other.ID > latest.ID) {
				latest = other
			}
		}
		switch {
		case latest == nil:
		case !latest.terminal():
			verdict = "wait"
		case latest.Status != actionSuccess:
			return "skip"
		}
	}
	return verdict
}

// startNext starts the oldest runnable queued run when the runner is idle.
func (r *actionsRunner) startNext() {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.loadLocked()
	if r.running != 0 || !r.sandboxOK || r.runCtx == nil {
		return
	}
	sort.SliceStable(r.state.Runs, func(i, j int) bool { return r.state.Runs[i].ID < r.state.Runs[j].ID })
	dirty := false
	for i := range r.state.Runs {
		run := &r.state.Runs[i]
		if run.Status != actionQueued {
			continue
		}
		switch r.needsVerdictLocked(run) {
		case "wait":
			continue
		case "skip":
			run.Status = actionSkipped
			run.FinishedAt = r.now().UnixMilli()
			for step := range run.StepStatuses {
				run.StepStatuses[step] = stepSkipped
			}
			r.appendLog(run.ID, "==> Skipped: a workflow this one needs did not succeed.\n")
			dirty = true
			continue
		}
		run.Status = actionRunning
		run.StartedAt = r.now().UnixMilli()
		r.running = run.ID
		ctx, cancel := context.WithTimeout(r.runCtx, actionsRunTimeout)
		r.cancelRun[run.ID] = cancel
		r.saveLocked()
		r.runs.Add(1)
		go r.execute(ctx, cancel, *run)
		return
	}
	if dirty {
		r.saveLocked()
	}
}

// ---------------------------------------------------------------------------
// Execution.

func (r *actionsRunner) appendLog(id int64, text string) {
	_ = os.MkdirAll(r.runDirectory(id), 0o700)
	file, err := os.OpenFile(r.logPath(id), os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0o600)
	if err != nil {
		return
	}
	defer file.Close()
	_, _ = file.WriteString(text)
}

// cappedLog stops writing at the cap instead of failing the step.
type cappedLog struct {
	mu      sync.Mutex
	file    *os.File
	written int64
	noted   bool
}

func (c *cappedLog) Write(p []byte) (int, error) {
	c.mu.Lock()
	defer c.mu.Unlock()
	if room := maxActionsLogBytes - c.written; room > 0 {
		chunk := p
		if int64(len(chunk)) > room {
			chunk = chunk[:room]
		}
		n, _ := c.file.Write(chunk)
		c.written += int64(n)
	} else if !c.noted {
		c.noted = true
		_, _ = c.file.WriteString("\n==> Log limit reached; further output is not kept.\n")
	}
	return len(p), nil
}

func (r *actionsRunner) setStep(id int64, step int, status string) {
	r.mu.Lock()
	defer r.mu.Unlock()
	if run := r.findRunLocked(id); run != nil && step < len(run.StepStatuses) {
		run.StepStatuses[step] = status
		r.saveLocked()
	}
}

func (r *actionsRunner) execute(ctx context.Context, cancel context.CancelFunc, run actionRun) {
	defer r.runs.Done()
	defer cancel()
	status, note := r.executeSteps(ctx, run)
	if note != "" {
		r.appendLog(run.ID, note)
	}
	r.mu.Lock()
	if live := r.findRunLocked(run.ID); live != nil {
		live.Status = status
		live.FinishedAt = r.now().UnixMilli()
		for step, state := range live.StepStatuses {
			switch {
			case state == stepRunning && status == actionCancelled:
				live.StepStatuses[step] = stepCancelled
			case state == stepRunning:
				live.StepStatuses[step] = stepFailed
			case state == stepPending && status == actionCancelled:
				live.StepStatuses[step] = stepCancelled
			case state == stepPending:
				live.StepStatuses[step] = stepSkipped
			}
		}
	}
	delete(r.cancelRun, run.ID)
	r.running = 0
	r.saveLocked()
	r.mu.Unlock()
	_ = removeOwnCloneUnder(r.runDirectory(run.ID), filepath.Join(r.runDirectory(run.ID), "work"))
	log.Printf("mirror actions run #%d %q for %s/%s ended %s", run.ID,
		run.WorkflowName, run.Owner, run.Repository, status)
	r.startNext()
}

func (r *actionsRunner) executeSteps(ctx context.Context, run actionRun) (string, string) {
	repo, ok := r.findRepository(run.Owner, run.Repository)
	if !ok {
		return actionFailed, "==> ERROR: this node no longer serves the repository.\n"
	}
	found := r.workflowAt(ctx, repo.GitDir, run.Commit, run.WorkflowPath)
	if found.err != nil {
		return actionFailed, "==> ERROR: " + found.err.Error() + ".\n"
	}
	wf := found.workflow
	work := filepath.Join(r.runDirectory(run.ID), "work")
	_ = removeOwnCloneUnder(r.runDirectory(run.ID), work)
	if err := prepareActionsWorkspace(work); err != nil {
		return actionFailed, "==> ERROR: the run workspace could not be prepared.\n"
	}
	if err := r.exportCommit(ctx, repo.GitDir, run.Commit, filepath.Join(work, "src")); err != nil {
		return actionFailed, "==> ERROR: the commit could not be checked out.\n"
	}
	env := []string{
		"FORKMESH_REPO=" + run.Owner + "/" + run.Repository,
		"FORKMESH_COMMIT=" + run.Commit,
		"FORKMESH_REF=" + run.Ref,
		"FORKMESH_EVENT=" + run.Event,
		"FORKMESH_NODE=" + r.node(),
	}
	for _, pair := range wf.Env {
		env = append(env, pair[0]+"="+pair[1])
	}
	file, err := os.OpenFile(r.logPath(run.ID), os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0o600)
	if err != nil {
		return actionFailed, ""
	}
	defer file.Close()
	sink := &cappedLog{file: file}
	fmt.Fprintf(sink, "==> %s at %s on %s (%s)\n", wf.Name, boundedRunes(run.Commit, 12),
		strings.TrimPrefix(run.Ref, "refs/heads/"), r.node())
	fmt.Fprintf(sink, "==> Sandbox: no network, no Actions variables, read-only system.\n")

	for index, step := range wf.Steps {
		if ctx.Err() != nil {
			break
		}
		r.setStep(run.ID, index, stepRunning)
		fmt.Fprintf(sink, "\n==> Step %d/%d: %s\n", index+1, len(wf.Steps), step.Name)
		script, blanked := substituteWorkflowVars(step.Run)
		if blanked {
			fmt.Fprintf(sink, "==> Note: this step reads Actions variables; a mirror holds none, so they are empty.\n")
		}
		code, err := r.runStep(ctx, work, env, script, sink)
		switch {
		case ctx.Err() != nil:
		case err != nil:
			r.setStep(run.ID, index, stepFailed)
			fmt.Fprintf(sink, "\n==> ERROR: the step could not start: %v\n", err)
			return actionFailed, ""
		case code != 0:
			r.setStep(run.ID, index, stepFailed)
			fmt.Fprintf(sink, "\n==> FAILED: step %q exited with status %d.\n", step.Name, code)
			return actionFailed, ""
		default:
			r.setStep(run.ID, index, stepSuccess)
			continue
		}
		break
	}
	switch {
	case errors.Is(ctx.Err(), context.DeadlineExceeded):
		fmt.Fprintf(sink, "\n==> FAILED: the run exceeded %s.\n", actionsRunTimeout)
		return actionFailed, ""
	case ctx.Err() != nil:
		fmt.Fprintf(sink, "\n==> Cancelled.\n")
		return actionCancelled, ""
	}
	fmt.Fprintf(sink, "\n==> SUCCESS: all steps completed.\n")
	return actionSuccess, ""
}

// exportCommit writes the commit's tree into dir. The workspace is a plain
// export, not a clone: a clone's alternates would point outside the sandbox.
func (r *actionsRunner) exportCommit(ctx context.Context, gitDir, commit, dir string) error {
	return exportGitCommit(ctx, gitDir, commit, dir)
}

func exportGitCommit(ctx context.Context, gitDir, commit, dir string) error {
	archive := exec.CommandContext(ctx, "git", "--git-dir="+gitDir, "archive", "--format=tar", commit)
	archive.Env = pullGitEnvironment(nil)
	extract := exec.CommandContext(ctx, "tar", "-x", "--no-same-owner", "-C", dir)
	extract.Env = []string{"PATH=" + os.Getenv("PATH"), "LANG=C"}
	pipe, err := archive.StdoutPipe()
	if err != nil {
		return err
	}
	extract.Stdin = pipe
	if err := extract.Start(); err != nil {
		return err
	}
	if err := archive.Run(); err != nil {
		_ = extract.Wait()
		return err
	}
	return extract.Wait()
}

// runStep feeds the script on stdin, so it never appears in a process list.
func (r *actionsRunner) runStep(ctx context.Context, work string, env []string, script string, sink io.Writer) (int, error) {
	name, args, err := r.sandbox(work, env)
	if err != nil {
		return 0, err
	}
	shell := "/bin/sh"
	if _, err := os.Stat("/bin/bash"); err == nil {
		shell = "/bin/bash"
	}
	command := exec.CommandContext(ctx, name, append(args, shell, "-e", "-s")...)
	command.Env = []string{"PATH=/usr/bin:/bin"}
	if !strings.HasSuffix(script, "\n") {
		script += "\n"
	}
	// One file may grow to 1 GiB (512-byte blocks), and no core dumps.
	command.Stdin = strings.NewReader("ulimit -c 0 2>/dev/null || true\nulimit -f 2097152 2>/dev/null || true\n" + script)
	command.Stdout, command.Stderr = sink, sink
	command.SysProcAttr = &syscall.SysProcAttr{Setpgid: true}
	command.Cancel = func() error { return syscall.Kill(-command.Process.Pid, syscall.SIGKILL) }
	command.WaitDelay = 10 * time.Second
	err = command.Run()
	var exit *exec.ExitError
	if errors.As(err, &exit) {
		return exit.ExitCode(), nil
	}
	return 0, err
}

// ---------------------------------------------------------------------------
// Control requests.

func (r *actionsRunner) recordOutcome(request actionsControlRequest, reason string, runID int64) {
	status := "applied"
	if reason != "" {
		status = "rejected"
	}
	outcome := actionsControlOutcome{
		Owner: request.Owner, Repository: request.Repository,
		RequestID: request.RequestID, Action: request.Action,
		Status: status, Reason: reason, RunID: runID, FinishedAt: r.now().UnixMilli(),
	}
	r.mu.Lock()
	r.loadLocked()
	r.state.Outcomes = append([]actionsControlOutcome{outcome}, r.state.Outcomes...)
	if len(r.state.Outcomes) > maxActionsControlOutcomes {
		r.state.Outcomes = r.state.Outcomes[:maxActionsControlOutcomes]
	}
	r.saveLocked()
	r.mu.Unlock()
	actor := request.Actor
	if actor == "" {
		actor = "unknown"
	}
	if reason == "" {
		log.Printf("mirror actions applied web control %s by %s for %s/%s", request.Action,
			actor, request.Owner, request.Repository)
	} else {
		log.Printf("mirror actions rejected web control %s by %s for %s/%s: %s",
			request.Action, actor, request.Owner, request.Repository, reason)
	}
}

// scanControl consumes the couriered requests. Each file is removed before it
// is judged, and the seen ring is saved before anything is applied, so a
// crash can lose a request but can never run one twice.
func (r *actionsRunner) scanControl(ctx context.Context) {
	files := scanActionsControlDirectory(r.controlDirectory(), maxActionsControlFiles)
	if len(files) == 0 {
		return
	}
	r.daemon.mu.RLock()
	routerKey := r.daemon.gateway.RouterPublicKey
	r.daemon.mu.RUnlock()
	var accepted, refused []actionsControlRequest
	r.mu.Lock()
	r.loadLocked()
	for _, file := range files {
		removeActionsControlFile(r.controlDirectory(), file.Name)
		if file.Problem != "" {
			log.Printf("mirror actions discarded control file %s: %s", file.Name, file.Problem)
			continue
		}
		request, verdict, err := parseActionsControlEnvelope(file.Bytes, r.node(), routerKey, r.now())
		if verdict == actionsControlInvalid {
			log.Printf("mirror actions discarded control file %s: %v", file.Name, err)
			continue
		}
		if actionsControlSeen(r.state.Seen, request.RequestID) {
			continue // the first delivery's outcome already answers this id
		}
		r.state.Seen = rememberActionsControl(r.state.Seen, request.RequestID)
		if verdict == actionsControlRejected {
			refused = append(refused, request)
			continue
		}
		accepted = append(accepted, request)
	}
	r.saveLocked()
	r.mu.Unlock()
	for _, request := range refused {
		r.recordOutcome(request, "invalid_request", 0)
	}
	sort.SliceStable(accepted, func(i, j int) bool { return accepted[i].IssuedAt < accepted[j].IssuedAt })
	for _, request := range accepted {
		reason, runID := r.applyControl(ctx, request)
		r.recordOutcome(request, reason, runID)
	}
	r.startNext()
}

// applyControl returns a rejection reason ("" when applied) and the run the
// request created or touched.
func (r *actionsRunner) applyControl(ctx context.Context, request actionsControlRequest) (string, int64) {
	repo, found := r.findRepository(request.Owner, request.Repository)
	// A run always belongs to the envelope's repository; an id from another
	// one is indistinguishable from an id that does not exist.
	ownRun := func() *actionRun {
		run := r.findRunLocked(request.RunID)
		if run == nil || !strings.EqualFold(run.Owner, request.Owner) ||
			!strings.EqualFold(run.Repository, request.Repository) {
			return nil
		}
		return run
	}
	repoState := func() (bool, []string) {
		r.mu.Lock()
		defer r.mu.Unlock()
		r.loadLocked()
		state := r.repositoryStateLocked(repo)
		return state.WebDisabled, append([]string(nil), state.DisabledWorkflows...)
	}

	switch request.Action {
	case "dispatch", "rerun":
		if !found {
			if request.Action == "rerun" {
				return "run_not_found", 0
			}
			return "repository_not_found", 0
		}
		commit, ref, workflowPath := "", request.Ref, request.Workflow
		if request.Action == "rerun" {
			r.mu.Lock()
			run := ownRun()
			if run != nil {
				commit, ref, workflowPath = run.Commit, run.Ref, run.WorkflowPath
			}
			r.mu.Unlock()
			if run == nil {
				return "run_not_found", 0
			}
		}
		webDisabled, disabled := repoState()
		if webDisabled {
			return "actions_disabled", request.RunID
		}
		if request.Action == "dispatch" {
			out, err := r.git(ctx, repo.GitDir, "rev-parse", "--verify", "--quiet", ref+"^{commit}")
			if err != nil || strings.TrimSpace(out) == "" {
				return "ref_not_found", 0
			}
			commit = strings.TrimSpace(out)
			if containsString(disabled, workflowPath) {
				return "workflow_disabled", 0
			}
		}
		at := r.workflowAt(ctx, repo.GitDir, commit, workflowPath)
		switch {
		case at.err != nil && at.workflow == nil && strings.Contains(at.err.Error(), "not on that commit"):
			return "workflow_not_found", request.RunID
		case at.err != nil:
			return "workflow_invalid", request.RunID
		case request.Action == "dispatch" && !at.workflow.manual():
			return "not_manual", 0
		case !at.workflow.runsOnThisNode(r.labels()):
			return "wrong_node", request.RunID
		}
		event := "workflow_dispatch"
		if request.Action == "rerun" {
			event = "rerun"
		}
		id := r.createRun(repo, at.workflow, commit, ref, event, request.Actor)
		if id == 0 {
			return "invalid_request", request.RunID
		}
		return "", id

	case "cancel":
		r.mu.Lock()
		defer r.mu.Unlock()
		run := ownRun()
		switch {
		case run == nil:
			return "run_not_found", 0
		case run.Status == actionRunning:
			if cancel := r.cancelRun[run.ID]; cancel != nil {
				cancel()
			}
			return "", run.ID
		case run.Status == actionQueued:
			run.Status = actionCancelled
			run.FinishedAt = r.now().UnixMilli()
			for step := range run.StepStatuses {
				run.StepStatuses[step] = stepCancelled
			}
			r.appendLog(run.ID, "==> Cancelled before it started.\n")
			r.saveLocked()
			return "", run.ID
		}
		return "not_cancellable", run.ID

	case "approve", "reject":
		// Nothing here waits for approval: a push was already authorized by
		// whoever could move the branch, and a dispatch by the owner's
		// session. Pull-request runs, the case approval exists for, are not
		// run on mirrors.
		r.mu.Lock()
		defer r.mu.Unlock()
		if run := ownRun(); run != nil {
			return "not_awaiting_approval", run.ID
		}
		return "run_not_found", 0

	case "set-workflow-enabled":
		if !found {
			return "repository_not_found", 0
		}
		head := r.defaultBranch(ctx, repo.GitDir)
		if at := r.workflowAt(ctx, repo.GitDir, head, request.Workflow); at.workflow == nil &&
			at.err != nil && strings.Contains(at.err.Error(), "not on that commit") {
			return "workflow_not_found", 0
		}
		r.mu.Lock()
		defer r.mu.Unlock()
		state := r.repositoryStateLocked(repo)
		kept := state.DisabledWorkflows[:0:0]
		for _, path := range state.DisabledWorkflows {
			if path != request.Workflow {
				kept = append(kept, path)
			}
		}
		if !request.Enabled {
			kept = append(kept, request.Workflow)
		}
		sort.Strings(kept)
		state.DisabledWorkflows = kept
		delete(r.catalog, repositoryKey(repo))
		r.saveLocked()
		return "", 0

	case "set-repository-enabled":
		if !found {
			return "repository_not_found", 0
		}
		// The web can turn a repository off here and back on. It cannot turn
		// the runner itself on: that is `enableActions`, the operator's.
		r.mu.Lock()
		defer r.mu.Unlock()
		r.repositoryStateLocked(repo).WebDisabled = !request.Enabled
		delete(r.catalog, repositoryKey(repo))
		r.saveLocked()
		return "", 0
	}
	return "invalid_request", 0
}

// ---------------------------------------------------------------------------
// Summary.

func (r *actionsRunner) logTail(id int64) string {
	file, err := os.Open(r.logPath(id))
	if err != nil {
		return ""
	}
	defer file.Close()
	info, err := file.Stat()
	if err != nil {
		return ""
	}
	offset := info.Size() - maxActionsLogTailBytes
	if offset < 0 {
		offset = 0
	}
	if _, err := file.Seek(offset, io.SeekStart); err != nil {
		return ""
	}
	raw, _ := io.ReadAll(bufio.NewReader(io.LimitReader(file, maxActionsLogTailBytes)))
	return trimLogTail(raw, maxActionsLogTailBytes)
}

// trimLogTail keeps at most limit bytes of valid UTF-8 from the end, dropping
// NULs and the partial first line a byte offset usually lands in.
func trimLogTail(raw []byte, limit int) string {
	raw = bytes.ReplaceAll(raw, []byte{0}, nil)
	if len(raw) > limit {
		raw = raw[len(raw)-limit:]
	}
	text := strings.ToValidUTF8(string(raw), "")
	for len(text) > limit {
		_, size := utf8.DecodeRuneInString(text)
		text = text[size:]
	}
	return text
}

// catalogFor lists a repository's workflows at its default branch head,
// cached by that head so a quiet repository costs no git per summary.
func (r *actionsRunner) catalogFor(ctx context.Context, repo Repository, disabled []string) []map[string]any {
	ref := r.defaultBranch(ctx, repo.GitDir)
	head := ""
	if ref != "" {
		if out, err := r.git(ctx, repo.GitDir, "rev-parse", "--verify", "--quiet", ref+"^{commit}"); err == nil {
			head = strings.TrimSpace(out)
		}
	}
	key := repositoryKey(repo)
	r.mu.Lock()
	cached, ok := r.catalog[key]
	r.mu.Unlock()
	if ok && cached.head == head {
		return cached.workflows
	}
	workflows := []map[string]any{}
	if head != "" {
		for _, at := range r.workflowsAt(ctx, repo.GitDir, head) {
			entry := map[string]any{
				"path": at.path, "name": "", "events": []string{}, "manual": false,
				"enabled": !containsString(disabled, at.path), "runsOn": []string{},
				"valid": at.err == nil,
			}
			if at.workflow != nil {
				entry["name"] = at.workflow.Name
				entry["events"] = nonNilStrings(at.workflow.Events)
				entry["manual"] = at.workflow.manual()
				entry["runsOn"] = nonNilStrings(at.workflow.RunsOn)
			}
			workflows = append(workflows, entry)
		}
	}
	r.mu.Lock()
	r.catalog[key] = actionsCatalogEntry{head: head, workflows: workflows}
	r.mu.Unlock()
	return workflows
}

func nonNilStrings(values []string) []string {
	if values == nil {
		return []string{}
	}
	return values
}

// buildSummary is the exact secret-free schemaVersion 2 document the gateway
// validates: no workflow content, commands, environment, or local paths.
func (r *actionsRunner) buildSummary(ctx context.Context) map[string]any {
	now := r.now().UnixMilli()
	repositories := []map[string]any{}
	for _, repo := range r.repositories() {
		if len(repositories) == maxActionsRepositories {
			break
		}
		r.mu.Lock()
		r.loadLocked()
		state := r.repositoryStateLocked(repo)
		webDisabled := state.WebDisabled
		disabled := append([]string(nil), state.DisabledWorkflows...)
		r.mu.Unlock()
		repositories = append(repositories, map[string]any{
			"owner": repo.Owner, "repository": repo.Name,
			"actionsEnabled": !webDisabled,
			"workflows":      r.catalogFor(ctx, repo, disabled),
		})
	}

	r.mu.Lock()
	runs := append([]actionRun(nil), r.state.Runs...)
	outcomes := append([]actionsControlOutcome(nil), r.state.Outcomes...)
	r.mu.Unlock()
	sort.SliceStable(runs, func(i, j int) bool {
		if runs[i].CreatedAt != runs[j].CreatedAt {
			return runs[i].CreatedAt > runs[j].CreatedAt
		}
		return runs[i].ID > runs[j].ID
	})
	if len(runs) > maxActionsSummaryRuns {
		runs = runs[:maxActionsSummaryRuns]
	}
	serialized := make([]map[string]any, 0, len(runs))
	for _, run := range runs {
		steps := []map[string]any{}
		for index, name := range run.StepNames {
			status := stepPending
			if index < len(run.StepStatuses) {
				status = run.StepStatuses[index]
			}
			steps = append(steps, map[string]any{"name": name, "status": status})
		}
		serialized = append(serialized, map[string]any{
			"id": run.ID, "owner": run.Owner, "repository": run.Repository,
			"workflow": run.WorkflowName, "commit": run.Commit, "ref": run.Ref,
			"status": run.Status, "createdAt": run.CreatedAt,
			"startedAt": run.StartedAt, "finishedAt": run.FinishedAt,
			"logTail": r.logTail(run.ID), "workflowPath": run.WorkflowPath,
			"pullNumber": 0, "steps": steps,
		})
	}
	controls := make([]map[string]any, 0, len(outcomes))
	for _, outcome := range outcomes {
		controls = append(controls, map[string]any{
			"owner": outcome.Owner, "repository": outcome.Repository,
			"requestId": outcome.RequestID, "action": outcome.Action,
			"status": outcome.Status, "reason": outcome.Reason,
			"runId": outcome.RunID, "finishedAt": outcome.FinishedAt,
		})
	}
	summary := map[string]any{
		"schemaVersion": 2, "type": actionsSummaryType, "node": r.node(),
		"updatedAt": now, "expiresAt": now + actionsSummaryLease.Milliseconds(),
		"runs": serialized, "repositories": repositories, "controls": controls,
	}
	// Fit the 256 KiB bound by shortening log tails, oldest run first.
	for index := len(serialized) - 1; index >= 0; index-- {
		encoded, err := json.Marshal(summary)
		if err != nil {
			return nil
		}
		excess := len(encoded) + 1 - maxActionsSummaryBytes
		if excess <= 0 {
			return summary
		}
		tail := serialized[index]["logTail"].(string)
		// JSON escaping can double a byte, so give back twice the excess.
		keep := len(tail) - 2*excess - 64
		if keep < 0 {
			keep = 0
		}
		serialized[index]["logTail"] = trimLogTail([]byte(tail), keep)
	}
	if encoded, err := json.Marshal(summary); err != nil || len(encoded)+1 > maxActionsSummaryBytes {
		return nil
	}
	return summary
}

// writeSummary replaces actions-summary.json atomically, mode 0600 in the
// gateway's own 0700 directory: the same-account form the gateway accepts.
func (r *actionsRunner) writeSummary(ctx context.Context) {
	summary := r.buildSummary(ctx)
	if summary == nil {
		return
	}
	encoded, err := json.Marshal(summary)
	if err != nil {
		return
	}
	path := r.summaryPath()
	temporary, err := os.CreateTemp(filepath.Dir(path), ".actions-summary-")
	if err != nil {
		log.Printf("mirror actions summary was not written: %v", err)
		return
	}
	defer os.Remove(temporary.Name())
	ok := temporary.Chmod(0o600) == nil
	if ok {
		_, err = temporary.Write(append(encoded, '\n'))
		ok = err == nil && temporary.Sync() == nil
	}
	if temporary.Close() != nil || !ok || os.Rename(temporary.Name(), path) != nil {
		log.Printf("mirror actions summary was not written")
		return
	}
	r.mu.Lock()
	r.summaryDirty = false
	r.summaryWritten = r.now()
	r.mu.Unlock()
}

// ---------------------------------------------------------------------------

// run is the runner's own loop. The daemon's cycle can be half an hour apart
// on a relay-fed node, which is far too slow to answer someone who just
// pressed "Run workflow".
func (r *actionsRunner) run(ctx context.Context) {
	r.mu.Lock()
	r.runCtx = ctx
	r.loadLocked()
	r.mu.Unlock()
	_ = os.MkdirAll(r.dir, 0o700)
	ticker := time.NewTicker(actionsTickInterval)
	defer ticker.Stop()
	r.tick(ctx, true)
	for {
		select {
		case <-ctx.Done():
			return
		case <-r.refsMoved:
			r.tick(ctx, true)
		case <-ticker.C:
			r.tick(ctx, false)
		}
	}
}

func (r *actionsRunner) tick(ctx context.Context, refs bool) {
	if !r.available(ctx) {
		return
	}
	if refs {
		r.scanRefs(ctx)
	}
	r.scanControl(ctx)
	r.mu.Lock()
	due := r.summaryDirty || r.running != 0 ||
		r.now().Sub(r.summaryWritten) >= actionsSummaryRefresh
	r.mu.Unlock()
	if due {
		r.writeSummary(ctx)
	}
}

// ready reports whether the two Actions operations may be advertised: the
// sandbox works, and the gateway script this daemon is about to start knows
// the configuration keys that come with them.
func (r *actionsRunner) ready(ctx context.Context) bool {
	if !gatewayScriptServesActions(r.daemon.config.GatewayScript) {
		log.Printf("mirror actions are unavailable: the gateway script predates " +
			"actions-control; update it before enabling Actions")
		return false
	}
	return r.available(ctx)
}

// gatewayScriptServesActions guards a split rollout. The daemon binary and
// tools/mirror_gateway.py are deployed separately, and an older gateway
// refuses to start on a configuration key it does not know. Naming
// actionsControlPath to such a script would take the node's whole gateway
// down, not just Actions, so look before writing it.
func gatewayScriptServesActions(script string) bool {
	info, err := os.Stat(script)
	if err != nil || !info.Mode().IsRegular() || info.Size() > 8<<20 {
		return false
	}
	raw, err := os.ReadFile(script)
	return err == nil && bytes.Contains(raw, []byte(`"actionsControlPath"`)) &&
		bytes.Contains(raw, []byte(`"actions-control"`))
}

func (r *actionsRunner) wait() { r.runs.Wait() }
