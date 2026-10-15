package actions

import (
	"fmt"
	"io"
	"os"
	"os/exec"
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
	processLogChunkBytes   = 16 * 1024
	processLogMaxBytes     = 1024 * 1024
	processLogTailBytes    = 128 * 1024
	processLogMaxLineChars = 4096
	crashOutputTailBytes   = 12 * 1024
	crashContextMaxChars   = 12000
	maxLandedArtifactBytes = int64(1) << 30
	terminationGrace       = 1500 * time.Millisecond
	outputDrainAfterExit   = 250 * time.Millisecond
)

type phase int

const (
	phaseIdle phase = iota
	phaseCheckout
	phaseWorkspaceClone
	phaseWorkspaceCheckout
	phaseSnapshotVerification
	phaseStep
	phaseQuotaCheck
)

func (p phase) name() string {
	switch p {
	case phaseIdle:
		return "idle"
	case phaseCheckout:
		return "checkout"
	case phaseWorkspaceClone:
		return "workspace clone"
	case phaseWorkspaceCheckout:
		return "workspace checkout"
	case phaseSnapshotVerification:
		return "repository approval verification"
	case phaseStep:
		return "step"
	case phaseQuotaCheck:
		return "workspace quota check"
	}
	return "invalid phase"
}

type runnerHost interface {
	store() *Store
	emit(event string, data map[string]any)
	logf(format string, args ...any)
	shuttingDown() bool
	finishedRun(r *runner, ok, landed, prepared bool)
	interruptedRun(r *runner)
}

type StartRequest struct {
	Run          Run
	Workflow     Workflow
	Mirror       string
	RepoWorkTree string
	Variables    map[string]string
	Slice        string
	Limits       Limits
}

type queue struct {
	mu      sync.Mutex
	items   []func()
	signal  chan struct{}
	closed  bool
	stopped chan struct{}
}

func newQueue() *queue {
	return &queue{signal: make(chan struct{}, 1), stopped: make(chan struct{})}
}

func (q *queue) post(fn func()) {
	q.mu.Lock()
	if q.closed {
		q.mu.Unlock()
		return
	}
	q.items = append(q.items, fn)
	q.mu.Unlock()
	select {
	case q.signal <- struct{}{}:
	default:
	}
}

func (q *queue) close() {
	q.mu.Lock()
	q.closed = true
	q.items = nil
	q.mu.Unlock()
}

func (q *queue) loop() {
	defer close(q.stopped)
	for {
		q.mu.Lock()
		if q.closed {
			q.mu.Unlock()
			return
		}
		items := q.items
		q.items = nil
		q.mu.Unlock()
		for _, fn := range items {
			fn()
			q.mu.Lock()
			closed := q.closed
			q.mu.Unlock()
			if closed {
				return
			}
		}
		if len(items) == 0 {
			<-q.signal
		}
	}
}

type timer struct {
	q          *queue
	generation int
	t          *time.Timer
	active     bool
}

func (t *timer) start(d time.Duration, fn func()) {
	t.stop()
	t.generation++
	generation := t.generation
	t.active = true
	t.t = time.AfterFunc(d, func() {
		t.q.post(func() {
			if t.generation != generation || !t.active {
				return
			}
			t.active = false
			fn()
		})
	})
}

func (t *timer) stop() {
	if t.t != nil {
		t.t.Stop()
	}
	t.active = false
	t.generation++
}

type childProcess struct {
	cmd        *exec.Cmd
	generation int
	exited     bool
}

type runner struct {
	host        runnerHost
	finishExtra map[string]any
	q           *queue

	run          Run
	wf           Workflow
	mirror       string
	repoWorkTree string
	slice        string
	limits       Limits
	variables    map[string]string
	exposed      map[string]string
	secrets      []string
	network      bool

	sandboxRoot    string
	worktree       string
	workspace      string
	sandboxHome    string
	sandboxTmp     string
	releaseStaging string
	buildCache     string
	cacheLock      *fileLock

	stepLabel string
	command   string
	unit      string
	stepIndex int
	phase     phase

	busy       bool
	stopping   bool
	completing bool
	ended      bool

	outBytes        int
	suppressedBytes int
	lineChars       int
	outTail         []byte
	crashTail       []byte
	truncated       bool
	processTimedOut bool
	jobTimedOut     bool
	stdin           []byte

	proc       *childProcess
	procCount  int
	stepTimer  *timer
	jobTimer   *timer
	termTimer  *timer
	crashState string
	statusMu   sync.Mutex
	snapshot   Run
}

func newRunner(host runnerHost, request StartRequest) *runner {
	q := newQueue()
	r := &runner{host: host, q: q, limits: request.Limits}
	r.stepTimer = &timer{q: q}
	r.jobTimer = &timer{q: q}
	r.termTimer = &timer{q: q}
	go q.loop()
	return r
}

func (r *runner) post(fn func()) { r.q.post(fn) }

func (r *runner) record() Run {
	r.statusMu.Lock()
	defer r.statusMu.Unlock()
	return r.snapshot
}

func (r *runner) publish() {
	r.statusMu.Lock()
	r.snapshot = r.run
	r.snapshot.StepStatuses = append([]string(nil), r.run.StepStatuses...)
	r.statusMu.Unlock()
}

func (r *runner) save() {
	_ = r.host.store().SaveRun(r.run)
	r.publish()
}

func nowMs() int64 { return time.Now().UnixMilli() }

func (r *runner) setStepStatus(index int, status string) {
	if index < 0 || index >= len(r.wf.Steps) {
		return
	}
	if len(r.run.StepStatuses) != len(r.wf.Steps) {
		r.run.StepStatuses = make([]string, len(r.wf.Steps))
		for i := range r.run.StepStatuses {
			r.run.StepStatuses[i] = StepPending
		}
	}
	if r.run.StepStatuses[index] == status && r.run.CurrentStepIndex == index {
		return
	}
	r.run.StepStatuses[index] = status
	if status == StepRunning {
		r.run.CurrentStepIndex = index
	}
	r.save()
	r.host.emit("actions.step", map[string]any{"run": r.run.ID, "step": index, "status": status, "record": r.run.Record()})
}

func (r *runner) emitStatus() {
	r.host.emit("actions.status", map[string]any{"run": r.run.ID, "status": r.run.Status, "record": r.run.Record()})
}

func (r *runner) explicitWorkflowVariables() map[string]string {
	referenced := map[string]bool{}
	collect := func(text string) {
		for _, expression := range []*regexp.Regexp{varsReference, braceReference, plainReference} {
			for _, match := range expression.FindAllStringSubmatch(text, -1) {
				referenced[match[1]] = true
			}
		}
	}
	collect(r.wf.Content)
	for _, value := range r.wf.Env {
		collect(value)
	}
	exposed := map[string]string{}
	for name := range referenced {
		if name == "FORKMESH_ACTIONS_ALLOW_NETWORK" || name == "FORKMESH_ACTIONS_DISABLE_CACHE" || DangerousEnvironmentName(name) {
			continue
		}
		value, ok := r.variables[name]
		if !ok {
			continue
		}
		exposed[name] = value
	}
	return exposed
}

func sortedKeys(values map[string]string) []string {
	keys := make([]string, 0, len(values))
	for key := range values {
		keys = append(keys, key)
	}
	sort.Slice(keys, func(i, j int) bool { return qtKeyLess(keys[i], keys[j]) })
	return keys
}

func (r *runner) start(request StartRequest) {
	r.busy = true
	r.stopping = false
	r.completing = false
	r.run = request.Run
	r.wf = request.Workflow
	r.mirror = request.Mirror
	r.repoWorkTree = request.RepoWorkTree
	r.slice = request.Slice
	r.variables = request.Variables
	if r.variables == nil {
		r.variables = map[string]string{}
	}
	r.exposed = r.explicitWorkflowVariables()
	r.network = truthy(r.variables["FORKMESH_ACTIONS_ALLOW_NETWORK"])
	r.stepIndex = 0
	r.run.StepStatuses = make([]string, len(r.wf.Steps))
	for i := range r.run.StepStatuses {
		r.run.StepStatuses[i] = StepPending
	}
	r.run.CurrentStepIndex = -1
	r.outBytes, r.suppressedBytes, r.lineChars = 0, 0, 0
	r.outTail, r.crashTail = nil, nil
	r.stepLabel = "Checkout"
	r.command = ""
	r.truncated, r.processTimedOut, r.jobTimedOut = false, false, false
	r.stdin = nil
	r.secrets = nil
	for _, key := range sortedKeys(r.exposed) {
		if value := r.exposed[key]; valueIsWorthRedacting(value) {
			r.secrets = append(r.secrets, value)
		}
	}
	r.run.Status = StatusRunning
	r.run.StartedAtMs = nowMs()
	r.save()
	r.emitStatus()

	r.emitLog(fmt.Sprintf("==> \U0001F680 Workflow: %s (%s)", r.wf.Name, r.wf.Path))
	r.emitLog(fmt.Sprintf("==> \U0001F4CD Commit:   %s on %s", qtLeft(r.run.Commit, 12), r.run.Ref))
	if r.wf.Checkout {
		r.emitLog("==> \U0001F33F Checking out into a temporary worktree…")
	}
	if qtTrim(r.mirror) == "" || !dirExists(r.mirror) {
		where := r.mirror
		if where == "" {
			where = "(unset)"
		}
		r.complete(false, fmt.Sprintf("The served mirror for %s/%s is no longer available at %s.", r.run.Owner, r.run.Name, where))
		return
	}
	if available, reason := SandboxAvailable(); !available {
		r.complete(false, fmt.Sprintf("Actions isolation unavailable: %s.", reason))
		return
	}
	base := r.sandboxBase()
	r.sandboxRoot = base + "/" + runPrefix + strconv.Itoa(r.run.ID) + "-" + strconv.FormatInt(nowMs(), 10)
	r.worktree = r.sandboxRoot + "/source"
	r.workspace = r.sandboxRoot + "/workspace"
	r.sandboxHome = r.sandboxRoot + "/home"
	r.sandboxTmp = r.sandboxRoot + "/tmp"
	r.releaseStaging = r.sandboxRoot + "/release-cas"
	claims.claim(r.sandboxRoot)
	r.host.emit("actions.sandbox", map[string]any{"run": r.run.ID, "name": filepath.Base(r.sandboxRoot), "claimed": true})
	for _, dir := range []string{r.sandboxRoot, r.sandboxHome, r.sandboxHome + "/.local/bin", r.sandboxTmp, r.releaseStaging} {
		if os.MkdirAll(dir, 0o777) != nil {
			r.complete(false, "Could not create the Actions sandbox.")
			return
		}
	}
	_ = os.Chmod(r.sandboxRoot, 0o700)
	claims.lockFile(r.sandboxRoot)

	r.releaseBuildCache()
	if truthy(r.variables["FORKMESH_ACTIONS_DISABLE_CACHE"]) {
		r.emitLog("==> Persistent build cache disabled by this node's Actions policy.")
	} else {
		cache := r.host.store().BuildCacheDir(r.run)
		reason := ""
		if cache == "" || os.MkdirAll(cache+"/ccache", 0o777) != nil {
			reason = "its directory is not writable"
		} else if !r.acquireBuildCacheLock(cache) {
			reason = "another run of this repository is using it"
		} else {
			r.buildCache = cache
		}
		if r.buildCache == "" {
			r.emitLog(fmt.Sprintf("==> Building without the persistent cache: %s.", reason))
		} else {
			r.emitLog(fmt.Sprintf("==> ♻️  Build cache kept between runs at /cache (%s)", r.buildCache))
		}
	}
	jobTimeout := r.limits.JobTimeoutMs
	if jobTimeout < 1000 {
		jobTimeout = 1000
	}
	r.jobTimer.start(time.Duration(jobTimeout)*time.Millisecond, r.onJobTimeout)
	r.reclaimSandboxSpaceThenCheckout()
}

func (r *runner) sandboxBase() string {
	base := r.host.store().SandboxDir()
	if base == "" || !writable(base) {
		return os.TempDir()
	}
	return base
}

func writable(dir string) bool {
	probe, err := os.CreateTemp(dir, ".forkmesh-writable-*")
	if err != nil {
		return false
	}
	name := probe.Name()
	_ = probe.Close()
	_ = os.Remove(name)
	return true
}

func (r *runner) onJobTimeout() {
	r.jobTimedOut = true
	if r.proc != nil && !r.proc.exited {
		r.terminateCurrentProcess(true)
	} else if r.busy {
		r.complete(false, "Workflow exceeded the total time limit.")
	}
}

func (r *runner) reclaimSandboxSpaceThenCheckout() {
	runID := r.run.ID
	base := r.sandboxBase()
	go func() {
		sweep := ReclaimSandboxes(base, -1)
		r.post(func() {
			if !r.busy || r.completing || r.run.ID != runID {
				return
			}
			if sweep.Removed > 0 {
				r.emitLog(fmt.Sprintf("==> Reclaimed %d disposable tree(s) left by earlier runs (%s freed).", sweep.Removed, humanBytes(sweep.FreedBytes)))
			}
			needed := r.limits.MaxWorkspaceBytes
			if needed < 0 {
				needed = 0
			}
			if sweep.FreeBytes >= 0 && sweep.FreeBytes < needed {
				r.complete(false, fmt.Sprintf("Not enough free disk space on this node: %s available where a run needs %s.", humanBytes(sweep.FreeBytes), humanBytes(needed)))
				return
			}
			r.launchCheckout()
		})
	}()
}

func (r *runner) launchCheckout() {
	if !r.wf.Checkout {
		if os.MkdirAll(r.worktree, 0o777) != nil || os.MkdirAll(r.workspace, 0o777) != nil {
			r.complete(false, "Could not create the Actions sandbox.")
			return
		}
		r.emitLog(fmt.Sprintf("==> ⏭️  No local checkout (checkout: false); the steps get commit %s as FORKMESH_COMMIT.", qtLeft(r.run.Commit, 12)))
		r.startSteps()
		return
	}
	r.launch(phaseCheckout, "git", []string{"-C", r.mirror, "worktree", "add", "--detach", r.worktree, r.run.Commit}, "")
}

func (r *runner) stop() {
	if !r.busy {
		return
	}
	r.stopping = true
	r.emitLog("")
	r.emitLog("==> \U0001F6D1 Stop requested by user.")
	if r.proc != nil && !r.proc.exited {
		r.terminateCurrentProcess(false)
		return
	}
	r.complete(false, "Stopped.")
}

func (r *runner) interruptNow() {
	if !r.busy {
		return
	}
	if r.proc != nil && !r.proc.exited {
		r.terminateCurrentProcess(false)
		return
	}
	if !r.completing {
		r.interruptForShutdown(false, 0)
	}
}

func (r *runner) terminateCurrentProcess(timedOut bool) {
	if r.proc == nil || r.proc.exited {
		return
	}
	if timedOut {
		r.processTimedOut = true
	}
	if r.unit != "" {
		if systemctl, err := exec.LookPath("systemctl"); err == nil {
			kill := exec.Command(systemctl, "--user", "kill", "--kill-whom=all", r.unit)
			kill.Stdout, kill.Stderr = io.Discard, io.Discard
			if kill.Start() == nil {
				go func() { _ = kill.Wait() }()
			}
		}
	}
	terminateGroup(r.proc.cmd)
	if !r.termTimer.active {
		proc := r.proc
		r.termTimer.start(terminationGrace, func() {
			if proc != r.proc || proc.exited {
				return
			}
			killGroup(proc.cmd)
		})
	}
}

func (r *runner) baseEnvironment(step bool) map[string]string {
	env := map[string]string{
		"LANG":                "C.UTF-8",
		"LC_ALL":              "C.UTF-8",
		"CI":                  "true",
		"GIT_CONFIG_NOSYSTEM": "1",
		"GIT_CONFIG_GLOBAL":   "/dev/null",
		"GIT_CONFIG_COUNT":    "1",
		"GIT_CONFIG_KEY_0":    "init.defaultBranch",
		"GIT_CONFIG_VALUE_0":  "main",
	}
	if step {
		env["PATH"] = "/home/forkmesh/.local/bin:/home/forkmesh/.cargo/bin:/usr/local/bin:/usr/bin:/bin"
	} else {
		env["PATH"] = "/usr/local/bin:/usr/bin:/bin"
	}
	if !step {
		env["HOME"] = r.sandboxHome
		env["TMPDIR"] = r.sandboxTmp
		return env
	}
	if osLabel() == "linux" {
		runtimeDir := fmt.Sprintf("/run/user/%d", currentUID())
		env["XDG_RUNTIME_DIR"] = runtimeDir
		env["DBUS_SESSION_BUS_ADDRESS"] = "unix:path=" + runtimeDir + "/bus"
	}
	env["HOME"] = "/home/forkmesh"
	env["TMPDIR"] = "/tmp"
	env["XDG_CACHE_HOME"] = "/home/forkmesh/.cache"
	env["XDG_CONFIG_HOME"] = "/home/forkmesh/.config"
	env["XDG_DATA_HOME"] = "/home/forkmesh/.local/share"
	env["XDG_STATE_HOME"] = "/home/forkmesh/.local/state"
	cpus := CPUQuotaPercent() / 100
	if cpus < 1 {
		cpus = 1
	}
	env["FORKMESH_ACTIONS_CPUS"] = strconv.Itoa(cpus)
	memoryMB := ScopeMemoryBytes(r.limits) / (1024 * 1024)
	if memoryMB < 64 {
		memoryMB = 64
	}
	env["FORKMESH_ACTIONS_MEMORY_MB"] = strconv.FormatInt(memoryMB, 10)
	if r.buildCache != "" {
		ccache := r.limits.MaxCacheBytes / 2 / (1 << 30)
		if ccache < 1 {
			ccache = 1
		}
		env["FORKMESH_ACTIONS_CACHE"] = "/cache"
		env["CCACHE_DIR"] = "/cache/ccache"
		env["CCACHE_TEMPDIR"] = "/tmp"
		env["CCACHE_MAXSIZE"] = strconv.FormatInt(ccache, 10) + "G"
	}
	for _, key := range sortedKeys(r.exposed) {
		env[key] = r.exposed[key]
	}
	for _, key := range sortedKeys(r.wf.Env) {
		if !DangerousEnvironmentName(key) {
			env[key] = Substitute(r.wf.Env[key], r.exposed)
		}
	}
	env["FORKMESH_COMMIT"] = r.run.Commit
	env["FORKMESH_REF"] = r.run.Ref
	env["FORKMESH_RUN_ID"] = strconv.Itoa(r.run.ID)
	if strings.HasPrefix(r.run.Ref, "refs/tags/") {
		env["FORKMESH_TAG"] = strings.TrimPrefix(r.run.Ref, "refs/tags/")
	}
	if r.isReleaseRun() {
		env["FORKMESH_RELEASE_CAS"] = "/release-cas"
		if r.run.Owner != "" && r.run.Name != "" {
			env["FORKMESH_REPO"] = r.run.Owner + "/" + r.run.Name
		}
	}
	return env
}

func environmentList(env map[string]string) []string {
	keys := make([]string, 0, len(env))
	for key := range env {
		keys = append(keys, key)
	}
	sort.Strings(keys)
	out := make([]string, 0, len(keys))
	for _, key := range keys {
		out = append(out, key+"="+env[key])
	}
	return out
}

func (r *runner) launch(p phase, program string, args []string, workingDir string) {
	r.phase = p
	r.outBytes, r.suppressedBytes, r.lineChars = 0, 0, 0
	r.outTail = nil
	r.truncated = false
	r.processTimedOut = false
	if r.stepLabel == "" {
		if p == phaseCheckout {
			r.stepLabel = "Checkout"
		} else {
			r.stepLabel = fmt.Sprintf("Step %d", r.stepIndex+1)
		}
	}
	if p != phaseStep {
		r.command = program
		for _, arg := range args {
			r.command += " " + arg
		}
	}
	r.updateCrashContext()
	timeout := 0
	if p == phaseStep {
		timeout = r.limits.StepTimeoutMs
		if timeout < 250 {
			timeout = 250
		}
	} else {
		timeout = r.limits.JobTimeoutMs
		if timeout > 5*60*1000 {
			timeout = 5 * 60 * 1000
		}
		if timeout < 30000 {
			timeout = 30000
		}
	}
	r.procCount++
	child := &childProcess{generation: r.procCount}
	r.proc = child
	cmd, err := helperCommand(program, args, p == phaseStep, r.limits)
	if err != nil {
		r.emitLog("!! ⚠️  Process failed to start: " + err.Error())
		child.exited = true
		r.post(func() {
			if r.proc == child {
				r.onProcessFinished(-1, false)
			}
		})
		return
	}
	child.cmd = cmd
	cmd.Env = environmentList(r.baseEnvironment(p == phaseStep))
	if workingDir != "" {
		cmd.Dir = workingDir
	}
	reader, writer, err := os.Pipe()
	if err != nil {
		r.emitLog("!! ⚠️  Process failed to start: " + err.Error())
		child.exited = true
		r.post(func() {
			if r.proc == child {
				r.onProcessFinished(-1, false)
			}
		})
		return
	}
	cmd.Stdout = writer
	cmd.Stderr = writer
	var stdinWriter *os.File
	if len(r.stdin) > 0 {
		stdinReader, w, perr := os.Pipe()
		if perr == nil {
			cmd.Stdin = stdinReader
			stdinWriter = w
			defer stdinReader.Close()
		}
	}
	if err := cmd.Start(); err != nil {
		_ = writer.Close()
		_ = reader.Close()
		if stdinWriter != nil {
			_ = stdinWriter.Close()
		}
		r.emitLog("!! ⚠️  Process failed to start: " + err.Error())
		child.exited = true
		r.post(func() {
			if r.proc == child {
				r.onProcessFinished(-1, false)
			}
		})
		return
	}
	_ = writer.Close()
	if stdinWriter != nil {
		payload := r.stdin
		r.stdin = nil
		go func() {
			_, _ = stdinWriter.Write(payload)
			_ = stdinWriter.Close()
		}()
	}
	outputDone := make(chan struct{})
	go func() {
		defer close(outputDone)
		buffer := make([]byte, processLogChunkBytes)
		for {
			n, err := reader.Read(buffer)
			if n > 0 {
				chunk := append([]byte(nil), buffer[:n]...)
				r.post(func() {
					if r.proc == child {
						r.emitProcessOutput(chunk)
					}
				})
			}
			if err != nil {
				return
			}
		}
	}()
	go func() {
		waitErr := cmd.Wait()
		select {
		case <-outputDone:
		case <-time.After(outputDrainAfterExit):
		}
		_ = reader.Close()
		code, crashed := exitStatus(waitErr)
		r.post(func() {
			if r.proc != child {
				return
			}
			child.exited = true
			r.onProcessFinished(code, crashed)
		})
	}()
	r.stepTimer.start(time.Duration(timeout)*time.Millisecond, func() { r.terminateCurrentProcess(true) })
}

func (r *runner) launchSandboxedStep(command string) {
	if osLabel() != "linux" {
		r.complete(false, "Actions isolation is unavailable on this platform.")
		return
	}
	bwrap, _ := exec.LookPath("bwrap")
	systemdRun, _ := exec.LookPath("systemd-run")
	shell := "/bin/sh"
	if pathExists("/bin/bash") {
		shell = "/bin/bash"
	}
	r.stdin = []byte(command)
	if len(r.stdin) == 0 || r.stdin[len(r.stdin)-1] != '\n' {
		r.stdin = append(r.stdin, '\n')
	}
	r.unit = fmt.Sprintf("forkmesh-action-%d-%d-%d.scope", os.Getpid(), r.run.ID, r.stepIndex)
	hostHome, _ := os.UserHomeDir()
	paths := sandboxPaths{
		worktree: r.worktree, workspace: r.workspace, home: r.sandboxHome, tmp: r.sandboxTmp,
		releaseStaging: r.releaseStaging, buildCache: r.buildCache, toolchainBin: r.host.store().ToolchainBinDir(),
		hostHome: hostHome, network: r.network,
	}
	args := SystemdRunArguments(r.unit, r.limits, r.slice, bwrap, SandboxArguments(paths, shell))
	r.launch(phaseStep, systemdRun, args, "")
}

func (r *runner) verifyWorkspaceSnapshotAsync() {
	r.phase = phaseSnapshotVerification
	r.stepLabel = "Repository approval verification"
	r.updateCrashContext()
	runID := r.run.ID
	workspace := r.workspace
	expectedTree := r.run.RepositoryTree
	expectedDigest := r.run.ExecutionDigest
	go func() {
		ok, reason := true, ""
		tree, digest, err := RepositoryStateDigest(workspace, "HEAD")
		if err != "" {
			ok, reason = false, err
		} else if tree != expectedTree || digest != expectedDigest {
			ok, reason = false, "the checked-out repository state no longer matches the approved execution digest"
		}
		r.post(func() {
			if !r.busy || r.run.ID != runID || r.phase != phaseSnapshotVerification {
				return
			}
			if !ok {
				r.complete(false, fmt.Sprintf("Repository approval verification failed: %s.", reason))
				return
			}
			r.emitLog(fmt.Sprintf("==> ✅ Approval bound to tree %s and state SHA-256 %s.", qtLeft(r.run.RepositoryTree, 12), qtLeft(r.run.ExecutionDigest, 16)))
			r.startSteps()
		})
	}()
}

func (r *runner) startSteps() {
	if r.network {
		r.emitLog("==> Network access explicitly enabled by this node's Actions policy.")
	} else {
		r.emitLog("==> Network namespace isolated (egress denied).")
	}
	r.runNextStep()
}

func (r *runner) checkWorkspaceQuotaAsync() {
	r.phase = phaseQuotaCheck
	r.updateCrashContext()
	runID := r.run.ID
	stepIndex := r.stepIndex
	root := r.sandboxRoot
	limit := r.limits.MaxWorkspaceBytes
	go func() {
		_, reason := WorkspaceWithinQuota(root, limit)
		r.post(func() {
			if !r.busy || r.completing || r.run.ID != runID || r.phase != phaseQuotaCheck || r.stepIndex != stepIndex {
				return
			}
			r.phase = phaseStep
			if reason != "" {
				r.setStepStatus(r.stepIndex, StepFailed)
				r.complete(false, reason+".")
				return
			}
			r.setStepStatus(r.stepIndex, StepSuccess)
			r.stepIndex++
			r.runNextStep()
		})
	}()
}

func (r *runner) acquireBuildCacheLock(cache string) bool {
	r.releaseBuildCache()
	lock, ok := tryLock(cache + "/.lock")
	if !ok {
		return false
	}
	r.cacheLock = lock
	return true
}

func (r *runner) releaseBuildCache() {
	r.buildCache = ""
	if r.cacheLock != nil {
		r.cacheLock.release()
		r.cacheLock = nil
	}
}

func (r *runner) onProcessFinished(exitCode int, crashed bool) {
	r.stepTimer.stop()
	r.termTimer.stop()
	r.unit = ""
	r.proc = nil
	r.emitSuppressedProcessOutputTail()
	if r.stopping {
		r.complete(false, "Stopped by user.")
		return
	}
	if r.processTimedOut {
		if r.jobTimedOut {
			r.complete(false, "Workflow exceeded the total time limit.")
		} else {
			r.complete(false, fmt.Sprintf("%s exceeded the process time limit.", r.stepLabel))
		}
		return
	}
	if (crashed || exitCode != 0) && r.host.shuttingDown() {
		r.interruptForShutdown(crashed, exitCode)
		return
	}
	switch r.phase {
	case phaseCheckout:
		if exitCode != 0 {
			r.complete(false, "Checkout failed.")
			return
		}
		r.stepLabel = "Isolated workspace"
		r.emitLog("==> \U0001F512 Creating a disposable writable clone; the approved source stays read-only.")
		args := []string{"clone", "--no-checkout", "--no-hardlinks", "--no-local",
			"--upload-pack=git -c pack.window=0 -c pack.threads=2 upload-pack", "--single-branch"}
		if r.wf.FetchDepth > 0 {
			args = append(args, fmt.Sprintf("--depth=%d", r.wf.FetchDepth))
		}
		args = append(args, r.worktree, r.workspace)
		r.launch(phaseWorkspaceClone, "git", args, "")
		return
	case phaseWorkspaceClone:
		if exitCode != 0 {
			r.complete(false, "Could not create the isolated workspace.")
			return
		}
		r.launch(phaseWorkspaceCheckout, "git", []string{"-C", r.workspace, "checkout", "--detach", r.run.Commit}, "")
		return
	case phaseWorkspaceCheckout:
		if exitCode != 0 {
			r.complete(false, "Could not check out the approved commit.")
			return
		}
		r.verifyWorkspaceSnapshotAsync()
		return
	}
	if crashed {
		r.complete(false, fmt.Sprintf("%s was killed by signal %d rather than exiting. The sandbox stops a step this way when it exceeds its %d MiB memory budget (FORKMESH_ACTIONS_MEMORY_MB) — build with fewer parallel jobs — or when something outside the run terminates its scope.",
			r.stepLabel, exitCode, ScopeMemoryBytes(r.limits)/(1024*1024)))
		return
	}
	if exitCode != 0 {
		r.setStepStatus(r.stepIndex, StepFailed)
		r.complete(false, fmt.Sprintf("Step failed with exit code %d.", exitCode))
		return
	}
	r.checkWorkspaceQuotaAsync()
}

func firstLine(text string) string {
	if index := strings.IndexByte(text, '\n'); index >= 0 {
		return text[:index]
	}
	return text
}

func (r *runner) runNextStep() {
	if r.stepIndex >= len(r.wf.Steps) {
		r.complete(true, "All steps completed.")
		return
	}
	step := r.wf.Steps[r.stepIndex]
	command := Substitute(step.Run, r.exposed)
	label := step.Name
	if label == "" {
		label = fmt.Sprintf("Step %d", r.stepIndex+1)
	}
	r.stepLabel = label
	r.command = command
	r.run.CurrentStepIndex = r.stepIndex
	r.setStepStatus(r.stepIndex, StepRunning)
	r.updateCrashContext()
	r.emitLog("")
	r.emitLog("==> \U0001F527 " + label)
	r.launchSandboxedStep(command)
}

type artifactLanding struct {
	releaseOK          bool
	releaseLanded      bool
	releaseError       string
	artifactsAttempted bool
	artifactsOK        bool
	artifactsError     string
}

func (r *runner) complete(ok bool, finalMessage string) {
	if r.completing {
		return
	}
	r.completing = true
	r.termTimer.stop()
	r.stepTimer.stop()
	r.jobTimer.stop()
	if !ok {
		r.finishComplete(false, finalMessage, false, false)
		return
	}
	runID := r.run.ID
	releaseRun := r.isReleaseRun()
	releaseStaging := r.releaseStaging
	releaseDestination := absolutePath(filepath.Join(r.mirror, "forkmesh-releases"))
	artifactStaging := absolutePath(filepath.Join(r.sandboxHome, "forkmesh-artifacts"))
	artifactDestination := absolutePath(filepath.Join(r.host.store().ArtifactsDir(), r.run.RepoKey(), strconv.Itoa(r.run.ID)))
	go func() {
		landing := artifactLanding{releaseOK: true, artifactsOK: true}
		func() {
			if releaseRun {
				entries, err := os.ReadDir(releaseStaging)
				if err != nil || len(entries) == 0 {
					landing.releaseOK = false
					return
				}
				landing.releaseOK, landing.releaseError = copySandboxTree(releaseStaging, releaseDestination, maxLandedArtifactBytes)
				if !landing.releaseOK {
					return
				}
				landing.releaseLanded = true
			}
			if dirExists(artifactStaging) {
				landing.artifactsAttempted = true
				landing.artifactsOK, landing.artifactsError = copySandboxTree(artifactStaging, artifactDestination, maxLandedArtifactBytes)
			}
		}()
		r.post(func() {
			if r.run.ID != runID {
				return
			}
			if !landing.releaseOK {
				if landing.releaseError != "" {
					r.emitLog("!! " + landing.releaseError)
				}
				r.finishComplete(false, "Release artifacts failed validation or could not be landed.", false, false)
				return
			}
			landed := false
			if landing.releaseLanded {
				landed = r.landReleaseMetadata()
			}
			if landed {
				r.host.emit("actions.release", map[string]any{"run": r.run.ID, "phase": "prepared"})
			}
			if landing.artifactsAttempted {
				if landing.artifactsOK {
					r.emitLog("==> Artifacts landed at " + artifactDestination)
				} else {
					r.emitLog("!! " + landing.artifactsError)
				}
			}
			if !landing.artifactsOK {
				r.finishComplete(false, "Action artifacts failed validation or could not be landed.", landed, landed)
				return
			}
			r.finishComplete(true, finalMessage, landed, landed)
		})
	}()
}

func absolutePath(path string) string {
	if absolute, err := filepath.Abs(path); err == nil {
		return absolute
	}
	return path
}

func (r *runner) finishComplete(ok bool, finalMessage string, landed, prepared bool) {
	if !ok && !r.stopping {
		r.logFailureDiagnostic(finalMessage)
	}
	r.emitLog("")
	prefix := "==> ❌ FAILED: "
	if r.stopping {
		prefix = "==> \U0001F6D1 STOPPED: "
	} else if ok {
		prefix = "==> ✅ SUCCESS: "
	}
	r.emitLog(prefix + finalMessage)
	r.cleanupWorktree()
	if r.stepIndex >= 0 && r.stepIndex < len(r.run.StepStatuses) && r.run.StepStatuses[r.stepIndex] == StepRunning {
		if r.stopping {
			r.setStepStatus(r.stepIndex, StepCancelled)
		} else {
			r.setStepStatus(r.stepIndex, StepFailed)
		}
	}
	switch {
	case r.stopping:
		r.run.Status = StatusCancelled
	case ok:
		r.run.Status = StatusSuccess
	default:
		r.run.Status = StatusFailed
	}
	if ok {
		for i := range r.run.StepStatuses {
			r.run.StepStatuses[i] = StepSuccess
		}
		r.run.CurrentStepIndex = -1
	} else if r.stopping {
		for i, status := range r.run.StepStatuses {
			if status == StepPending || status == StepQueued {
				r.run.StepStatuses[i] = StepSkipped
			}
		}
	}
	r.run.FinishedAtMs = nowMs()
	r.save()
	r.phase = phaseIdle
	r.busy = false
	r.completing = false
	r.stepTimer.stop()
	r.jobTimer.stop()
	r.crashState = ""
	r.emitStatus()
	finished := map[string]any{"run": r.run.ID, "ok": ok, "landed": landed, "record": r.run.Record()}
	if reactor, isReactor := r.host.(finishReactor); isReactor {
		r.finishExtra = reactor.reactToFinish(r, landed)
		for key, value := range r.finishExtra {
			finished[key] = value
		}
	}
	r.host.emit("actions.finished", finished)
	if landed {
		r.host.emit("actions.release", map[string]any{"run": r.run.ID, "phase": "landed"})
	}
	r.end()
	r.host.finishedRun(r, ok, landed, prepared)
}

func (r *runner) interruptForShutdown(crashed bool, exitCode int) {
	r.termTimer.stop()
	r.stepTimer.stop()
	r.jobTimer.stop()
	r.emitLog("")
	how := fmt.Sprintf("exited with code %d", exitCode)
	if crashed {
		how = fmt.Sprintf("was killed by signal %d", exitCode)
	}
	r.emitLog(fmt.Sprintf("==> ⚠ INTERRUPTED: ForkMesh received a stop signal and %s %s. The run stays active and restarts from its approved snapshot when ForkMesh starts again.", r.stepLabel, how))
	r.cleanupWorktree()
	r.run.Status = StatusRunning
	r.run.FinishedAtMs = 0
	r.save()
	r.phase = phaseIdle
	r.busy = false
	r.completing = false
	r.crashState = ""
	r.host.emit("actions.interrupted", map[string]any{"run": r.run.ID, "record": r.run.Record()})
	r.end()
	r.host.interruptedRun(r)
}

func (r *runner) end() {
	r.ended = true
	r.stepTimer.stop()
	r.jobTimer.stop()
	r.termTimer.stop()
	r.q.close()
}

func (r *runner) isReleaseRun() bool {
	return strings.HasPrefix(r.run.Ref, "refs/tags/") || r.run.WorkflowPath == ".forkmesh/release.yml"
}

func (r *runner) cleanupWorktree() {
	claims.release(r.sandboxRoot)
	if r.sandboxRoot != "" {
		r.host.emit("actions.sandbox", map[string]any{"run": r.run.ID, "name": filepath.Base(r.sandboxRoot), "claimed": false})
	}
	if r.sandboxRoot == "" {
		r.releaseBuildCache()
		return
	}
	cache := r.buildCache
	ceiling := r.limits.MaxCacheBytes
	r.releaseBuildCache()
	parent := filepath.Dir(absolutePath(r.sandboxRoot))
	discarded := filepath.Join(parent, discardPrefix+strconv.Itoa(r.run.ID)+"-"+strconv.FormatInt(nowMs(), 10))
	tree := r.sandboxRoot
	if os.Rename(r.sandboxRoot, discarded) == nil {
		tree = discarded
	}
	mirror := r.mirror
	go func() {
		_, _ = gitCaptureMerged(mirror, 10*time.Minute, "worktree", "prune")
		_ = safedelete.RemoveOwnWorktreeUnder(filepath.Dir(absolutePath(tree)), tree)
		enforceCacheBudget(cache, ceiling)
	}()
	r.sandboxRoot, r.worktree, r.workspace, r.sandboxHome, r.sandboxTmp, r.releaseStaging = "", "", "", "", "", ""
}

func (r *runner) redact(text string) string {
	for _, secret := range r.secrets {
		if secret != "" {
			text = strings.ReplaceAll(text, secret, "***")
		}
	}
	return text
}

func (r *runner) emitLog(text string) {
	safe := r.redact(text)
	r.host.store().AppendLog(r.run, safe)
	r.host.emit("actions.log", map[string]any{"run": r.run.ID, "text": safe})
}

func (r *runner) emitProcessOutput(data []byte) {
	if len(data) == 0 {
		return
	}
	r.rememberCrashOutput(data)
	r.updateCrashContext()
	if r.outBytes >= processLogMaxBytes {
		r.emitTruncationNotice()
		r.rememberSuppressed(data)
		return
	}
	keep := processLogMaxBytes - r.outBytes
	if keep > len(data) {
		keep = len(data)
	}
	for offset := 0; offset < keep; offset += processLogChunkBytes {
		end := offset + processLogChunkBytes
		if end > keep {
			end = keep
		}
		if safe := r.normalizeOutput(qtFromUtf8(data[offset:end])); safe != "" {
			r.emitLog(safe)
		}
	}
	r.outBytes += keep
	if keep < len(data) {
		r.emitTruncationNotice()
		r.rememberSuppressed(data[keep:])
	}
}

func (r *runner) emitTruncationNotice() {
	if r.truncated {
		return
	}
	r.truncated = true
	r.emitLog(fmt.Sprintf("\n!! Action output exceeded %d KiB. Suppressing middle output to keep the app responsive; the last %d KiB will be shown when this step exits.\n", processLogMaxBytes/1024, processLogTailBytes/1024))
}

func (r *runner) rememberSuppressed(data []byte) {
	if len(data) == 0 {
		return
	}
	r.suppressedBytes += len(data)
	r.outTail = append(r.outTail, data...)
	if len(r.outTail) > processLogTailBytes {
		r.outTail = append([]byte(nil), r.outTail[len(r.outTail)-processLogTailBytes:]...)
	}
}

func (r *runner) emitSuppressedProcessOutputTail() {
	if !r.truncated || len(r.outTail) == 0 {
		return
	}
	r.emitLog(fmt.Sprintf("\n!! Showing the final %d KiB of suppressed process output (%d KiB omitted).\n", (len(r.outTail)+1023)/1024, (r.suppressedBytes+1023)/1024))
	r.lineChars = 0
	for offset := 0; offset < len(r.outTail); offset += processLogChunkBytes {
		end := offset + processLogChunkBytes
		if end > len(r.outTail) {
			end = len(r.outTail)
		}
		if safe := r.normalizeOutput(qtFromUtf8(r.outTail[offset:end])); safe != "" {
			r.emitLog(safe)
		}
	}
	r.emitLog("\n!! End of suppressed process output.\n")
	r.outTail = nil
}

const wrapNotice = "\n...[long line wrapped for display]...\n"

func (r *runner) normalizeOutput(text string) string {
	var out strings.Builder
	out.Grow(len(text))
	for _, ch := range text {
		if ch == '\r' || ch == '\n' {
			out.WriteByte('\n')
			r.lineChars = 0
			continue
		}
		if ch == '\t' {
			out.WriteRune(ch)
			r.lineChars += 4
		} else if ch < 0x20 || (ch >= 0x7f && ch <= 0x9f) {
			continue
		} else {
			out.WriteRune(ch)
			if ch > 0xFFFF {
				r.lineChars += 2
			} else {
				r.lineChars++
			}
		}
		if r.lineChars >= processLogMaxLineChars {
			out.WriteString(wrapNotice)
			r.lineChars = 0
		}
	}
	return out.String()
}

func diagnosticSafeText(input string, maxChars int) string {
	text := input
	prefix := ""
	if utf16Len(text) > maxChars {
		text = qtRight(text, maxChars)
		prefix = "...(diagnostic text truncated; showing tail)...\n"
	}
	var out strings.Builder
	col := 0
	for _, ch := range text {
		if ch == '\r' || ch == '\n' {
			out.WriteByte('\n')
			col = 0
			continue
		}
		if ch == '\t' {
			out.WriteRune(ch)
			col += 4
		} else if ch < 0x20 || (ch >= 0x7f && ch <= 0x9f) {
			continue
		} else {
			out.WriteRune(ch)
			col++
		}
		if col >= processLogMaxLineChars {
			out.WriteString(wrapNotice)
			col = 0
		}
	}
	return prefix + out.String()
}

func oneLine(text string) string {
	text = strings.ReplaceAll(text, "\n", " ")
	text = strings.ReplaceAll(text, "\r", " ")
	return qtSimplified(text)
}

func (r *runner) rememberCrashOutput(data []byte) {
	r.crashTail = append(r.crashTail, data...)
	if len(r.crashTail) > crashOutputTailBytes {
		r.crashTail = append([]byte(nil), r.crashTail[len(r.crashTail)-crashOutputTailBytes:]...)
	}
}

func (r *runner) crashContextHeader() string {
	return fmt.Sprintf("action run id: %d\nworkflow: %s (%s)\nrepo: %s/%s\ncommit: %s\nref: %s\nphase: %s\nstep: %s\ncommand: %s\nworktree: %s\n",
		r.run.ID, oneLine(r.wf.Name), oneLine(r.wf.Path), oneLine(r.run.Owner), oneLine(r.run.Name), oneLine(r.run.Commit),
		oneLine(r.run.Ref), r.phase.name(), oneLine(r.stepLabel), oneLine(r.redact(r.command)), oneLine(r.worktree))
}

func (r *runner) crashOutputTail() string {
	tail := diagnosticSafeText(r.redact(qtFromUtf8(r.crashTail)), 8000)
	if qtTrim(tail) == "" {
		return ""
	}
	return tail + "\n"
}

func (r *runner) crashContext() string {
	out := r.crashContextHeader()
	if tail := r.crashOutputTail(); tail != "" {
		out += "recent process output tail:\n" + tail
	}
	if utf16Len(out) > crashContextMaxChars {
		out = "...(crash context truncated; showing tail)...\n" + qtRight(out, crashContextMaxChars)
	}
	return out
}

func (r *runner) updateCrashContext() {
	r.crashState = r.phase.name()
}

func (r *runner) logFailureDiagnostic(finalMessage string) {
	r.host.logf("Action failed before UI refresh\n%sfinal message: %s", r.crashContext(), finalMessage)
}
