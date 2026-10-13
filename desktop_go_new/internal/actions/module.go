package actions

import (
	"context"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/git"
	"forkmesh.com/desktop-core/internal/qsettings"
	"forkmesh.com/desktop-core/internal/rpc"
	"forkmesh.com/desktop-core/internal/vault"
)

const maxFinishedRecords = 200

type finishedRecord struct {
	Run      map[string]any `json:"run"`
	OK       bool           `json:"ok"`
	Landed   bool           `json:"landed"`
	Prepared bool           `json:"prepared"`
	AtMs     int64          `json:"atMs"`
	Served   map[string]any `json:"served,omitempty"`
	AutoFix  bool           `json:"autoFix,omitempty"`
	id       int
}

type activeRun struct {
	runner  *runner
	request StartRequest
	queued  bool
}

type Module struct {
	Root                string
	Facts               FactsLocator
	Perform             Performer
	Concurrency         int
	WriteSettings       SettingsWriter
	PinMirror           func(run Run) (string, bool)
	UnpinMirror         func(id int)
	SetWorkflowDisabled func(owner, name, path string, disabled bool) error
	RenewMirror         func()
	Agents              func() []AgentCount
	LogSink             func(text string)
	MirrorConfig        func(env MirrorConfigEnv) MirrorConfigEnv
	GitRunner           *git.Runner
	PullsDataRoot       string
	AutoFix             func(ctx context.Context, request AutoFixRequest) error
	Vault               func() *vault.Store

	core          *daemon.Core
	q             *actionQueue
	overlay       *settingsOverlay
	mirrorConfig  *mirrorConfigState
	st            *Store
	mu            sync.Mutex
	runs          map[int]*activeRun
	finished      []finishedRecord
	stopping      atomic.Bool
	inflight      atomic.Int64
	confirmations *confirmations
	emitHook      func(event string, data map[string]any)
	pullPasses    atomic.Int64
	autoFixMu     sync.Mutex
	autoFixQueue  []AutoFixRequest
}

func New() *Module {
	return &Module{runs: map[int]*activeRun{}, confirmations: newConfirmations(), overlay: newSettingsOverlay()}
}

func (m *Module) Name() string { return "actions" }

func (m *Module) Busy() bool {
	m.mu.Lock()
	active := len(m.runs)
	m.mu.Unlock()
	return active > 0 || m.inflight.Load() > 0 || m.pullPasses.Load() > 0
}

func (m *Module) Start(ctx context.Context, core *daemon.Core) error {
	m.core = core
	if m.runs == nil {
		m.runs = map[int]*activeRun{}
	}
	if m.confirmations == nil {
		m.confirmations = newConfirmations()
	}
	if m.overlay == nil {
		m.overlay = newSettingsOverlay()
	}
	root := m.Root
	if root == "" {
		root = filepath.Join(core.Layout.Data, "actions")
	}
	m.st = NewStore(root)
	m.q = newActionQueue(m)
	if m.Facts == nil {
		m.Facts = m.factsAuto
	}
	if m.Perform == nil {
		m.Perform = m.performAuto
	}
	handle := func(method string, handler rpc.Handler) {
		core.Registry.Handle(method, m.Name(), m.track(handler))
	}
	handle("core.actions.start", m.local(m.handleStart))
	handle("core.actions.stop", m.local(m.handleStop))
	handle("core.actions.active", m.local(m.handleActive))
	handle("core.actions.ack", m.local(m.handleAck))
	handle("core.actions.parse", m.local(m.handleParse))
	handle("core.actions.workflows", m.local(m.handleWorkflows))
	handle("core.actions.sandbox", m.local(m.handleSandbox))
	handle("core.actions.digest", m.local(m.handleDigest))
	handle("core.actions.runs", m.local(m.handleRuns))
	handle("core.actions.log", m.local(m.handleLog))
	handle("repos.actions", m.phoneActions)
	handle("repos.actions.log", m.phoneActionsLog)
	handle("repos.prepare", m.phonePrepare)
	handle("repos.run", m.phoneRun)
	handle("repos.list", m.gate().Wrap(m.phoneReposList))
	m.registerQueue(handle)
	m.registerMirrorConfig(handle)
	m.q.startTicker()
	go m.reconcileMirrorVaultAtStart()
	go func() {
		_ = ReclaimSandboxes(m.st.SandboxDir(), -1)
	}()
	return nil
}

func (m *Module) Stop() {
	m.stopping.Store(true)
	if m.q != nil {
		m.q.stop()
	}
	m.mu.Lock()
	active := make([]*activeRun, 0, len(m.runs))
	for _, run := range m.runs {
		active = append(active, run)
	}
	m.mu.Unlock()
	for _, run := range active {
		r := run.runner
		r.post(r.interruptNow)
	}
	deadline := time.After(5 * time.Second)
	for _, run := range active {
		select {
		case <-run.runner.q.stopped:
		case <-deadline:
			return
		}
	}
}

func (m *Module) track(handler rpc.Handler) rpc.Handler {
	return func(ctx context.Context, req *rpc.Request) (any, error) {
		m.inflight.Add(1)
		defer m.inflight.Add(-1)
		return handler(ctx, req)
	}
}

func (m *Module) local(handler rpc.Handler) rpc.Handler {
	return func(ctx context.Context, req *rpc.Request) (any, error) {
		if req.Peer == nil || rpc.Remote(req.Peer) {
			return nil, rpc.Fail("forbidden")
		}
		return handler(ctx, req)
	}
}

func (m *Module) store() *Store { return m.st }

func (m *Module) emit(event string, data map[string]any) {
	if m.emitHook != nil {
		m.emitHook(event, data)
	}
	if m.core != nil {
		m.core.BroadcastUI(event, data)
	}
	if q := m.q; q != nil {
		switch event {
		case "actions.log":
			q.post(func() { q.scheduleSummary(2000) })
		case "actions.step":
			q.post(func() {
				q.scheduleSummary(0)
				if q.armed {
					q.ring()
				}
			})
		case "actions.status":
			q.post(func() {
				q.scheduleSummary(0)
				q.updateMirrorRuntimeState()
				if q.armed {
					q.ring()
				}
			})
		}
	}
}

func (m *Module) logf(format string, args ...any) {
	if m.core != nil && m.core.Logger != nil {
		m.core.Logger.Printf("actions: "+format, args...)
	}
}

func (m *Module) logUI(format string, args ...any) {
	if m.LogSink != nil {
		m.LogSink(fmt.Sprintf(format, args...))
	}
	if m.core != nil {
		m.core.LogUI(format, args...)
	}
}

func (m *Module) shuttingDown() bool { return m.stopping.Load() }

func (m *Module) settings() *qsettings.Settings {
	if m.core == nil || m.core.Settings == nil {
		return qsettings.Parse(nil)
	}
	return m.core.Settings.Get()
}

func (m *Module) finishedRun(r *runner, ok, landed, prepared bool) {
	m.mu.Lock()
	defer m.mu.Unlock()
	if active, found := m.runs[r.run.ID]; found && active.runner == r {
		delete(m.runs, r.run.ID)
	}
	record := finishedRecord{Run: r.run.Record(), OK: ok, Landed: landed, Prepared: prepared, AtMs: nowMs(), id: r.run.ID}
	if served, ok := r.finishExtra["served"].(map[string]any); ok {
		record.Served = served
	}
	record.AutoFix = r.finishExtra["autoFix"] == true
	m.finished = append(m.finished, record)
	if len(m.finished) > maxFinishedRecords {
		m.finished = append([]finishedRecord(nil), m.finished[len(m.finished)-maxFinishedRecords:]...)
	}
	if q := m.q; q != nil {
		id := r.run.ID
		q.post(func() { q.runFinished(id) })
	}
	if record.AutoFix {
		run := r.run
		go func() { m.deliverAutoFix(m.autoFixRequest(run)) }()
	}
}

func (m *Module) interruptedRun(r *runner) {
	m.mu.Lock()
	if active, found := m.runs[r.run.ID]; found && active.runner == r {
		delete(m.runs, r.run.ID)
	}
	m.mu.Unlock()
	if q := m.q; q != nil {
		id := r.run.ID
		q.post(func() { q.runInterrupted(id) })
	}
}

type startParams struct {
	Run      json.RawMessage `json:"run"`
	Workflow struct {
		Path    string `json:"path"`
		Content string `json:"content"`
	} `json:"workflow"`
	Mirror    string            `json:"mirror"`
	Worktree  string            `json:"worktree"`
	Variables map[string]string `json:"variables"`
	Slice     string            `json:"slice"`
	Root      string            `json:"root"`
	Limits    *Limits           `json:"limits"`
}

func samePath(a, b string) bool {
	if a == "" || b == "" {
		return true
	}
	return filepath.Clean(absolutePath(a)) == filepath.Clean(absolutePath(b))
}

func (m *Module) handleStart(ctx context.Context, req *rpc.Request) (any, error) {
	var params startParams
	if err := req.Bind(&params); err != nil {
		return nil, err
	}
	object, ok := decodeJSONObject(params.Run)
	if !ok {
		return nil, rpc.Fail("bad_request")
	}
	run := RunFromObject(object)
	if run.ID <= 0 || run.Owner == "" || run.Name == "" {
		return nil, rpc.Fail("bad_request")
	}
	if !samePath(params.Root, m.st.Root) {
		return nil, rpc.Fail("store_mismatch")
	}
	workflowPath := params.Workflow.Path
	if workflowPath == "" {
		workflowPath = run.WorkflowPath
	}
	content := params.Workflow.Content
	if content == "" {
		content = run.WorkflowContent
	}
	wf := Parse(workflowPath, content)
	if !wf.Valid {
		return nil, rpc.Fail("invalid_workflow")
	}
	limits := DefaultLimits()
	if params.Limits != nil {
		limits = *params.Limits
	}
	if m.stopping.Load() {
		return nil, rpc.Fail("core_stopping")
	}
	request := StartRequest{Run: run, Workflow: wf, Mirror: params.Mirror, RepoWorkTree: params.Worktree,
		Variables: params.Variables, Slice: params.Slice, Limits: limits}
	m.mu.Lock()
	if _, exists := m.runs[run.ID]; exists {
		m.mu.Unlock()
		return nil, rpc.Fail("already_running")
	}
	r := newRunner(m, request)
	m.runs[run.ID] = &activeRun{runner: r, request: request}
	m.mu.Unlock()
	r.post(func() { r.start(request) })
	m.logf("run #%d %q for %s/%s @ %s started in the core", run.ID, run.WorkflowName, run.Owner, run.Name, qtLeft(run.Commit, 8))
	return map[string]any{"started": true, "run": run.ID, "pid": os.Getpid()}, nil
}

func (m *Module) activeRunner(id int) *runner {
	m.mu.Lock()
	defer m.mu.Unlock()
	if active, ok := m.runs[id]; ok {
		return active.runner
	}
	return nil
}

func (m *Module) handleStop(ctx context.Context, req *rpc.Request) (any, error) {
	params := req.Object()
	id := numberParam(params, "run")
	r := m.activeRunner(id)
	if r == nil {
		return map[string]any{"stopping": false, "run": id}, nil
	}
	r.post(r.stop)
	return map[string]any{"stopping": true, "run": id}, nil
}

func (m *Module) ActiveRuns() []map[string]any {
	m.mu.Lock()
	active := make([]*activeRun, 0, len(m.runs))
	for _, run := range m.runs {
		active = append(active, run)
	}
	m.mu.Unlock()
	sort.Slice(active, func(i, j int) bool { return active[i].request.Run.ID < active[j].request.Run.ID })
	out := make([]map[string]any, 0, len(active))
	for _, run := range active {
		record := run.runner.record()
		if record.ID == 0 {
			record = run.request.Run
		}
		out = append(out, map[string]any{
			"run":      record.Record(),
			"workflow": map[string]any{"path": run.request.Workflow.Path, "content": run.request.Workflow.Content},
			"mirror":   run.request.Mirror,
			"worktree": run.request.RepoWorkTree,
		})
	}
	return out
}

func (m *Module) handleActive(ctx context.Context, req *rpc.Request) (any, error) {
	m.mu.Lock()
	finished := append([]finishedRecord(nil), m.finished...)
	m.mu.Unlock()
	if finished == nil {
		finished = []finishedRecord{}
	}
	return map[string]any{"active": m.ActiveRuns(), "finished": finished, "sandboxes": claims.list(), "pid": os.Getpid()}, nil
}

func (m *Module) handleAck(ctx context.Context, req *rpc.Request) (any, error) {
	var params struct {
		Runs []int `json:"runs"`
	}
	if err := req.Bind(&params); err != nil {
		return nil, err
	}
	drop := map[int]bool{}
	for _, id := range params.Runs {
		drop[id] = true
	}
	m.mu.Lock()
	kept := m.finished[:0]
	for _, record := range m.finished {
		if !drop[record.id] {
			kept = append(kept, record)
		}
	}
	m.finished = kept
	left := len(m.finished)
	m.mu.Unlock()
	return map[string]any{"pending": left}, nil
}

func (m *Module) handleParse(ctx context.Context, req *rpc.Request) (any, error) {
	var params struct {
		Path    string `json:"path"`
		Content string `json:"content"`
	}
	if err := req.Bind(&params); err != nil {
		return nil, err
	}
	return Parse(params.Path, params.Content), nil
}

func (m *Module) handleWorkflows(ctx context.Context, req *rpc.Request) (any, error) {
	var params struct {
		Path       string `json:"path"`
		MirrorPath string `json:"mirrorPath"`
	}
	if err := req.Bind(&params); err != nil {
		return nil, err
	}
	workflows := WorkflowsInRepoPaths(params.MirrorPath, params.Path)
	if workflows == nil {
		workflows = []Workflow{}
	}
	return map[string]any{"workflows": workflows}, nil
}

func (m *Module) handleSandbox(ctx context.Context, req *rpc.Request) (any, error) {
	available, reason := SandboxAvailable()
	return map[string]any{"available": available, "reason": reason, "claimed": claims.list()}, nil
}

func (m *Module) handleDigest(ctx context.Context, req *rpc.Request) (any, error) {
	var params struct {
		Repository string `json:"repository"`
		Commit     string `json:"commit"`
	}
	if err := req.Bind(&params); err != nil {
		return nil, err
	}
	tree, digest, failure := RepositoryStateDigest(params.Repository, params.Commit)
	if failure != "" {
		return map[string]any{"ok": false, "error": failure}, nil
	}
	return map[string]any{"ok": true, "repositoryTree": tree, "executionDigest": digest}, nil
}

func (m *Module) handleRuns(ctx context.Context, req *rpc.Request) (any, error) {
	params := req.Object()
	owner, _ := params["owner"].(string)
	name, _ := params["name"].(string)
	limit := numberParam(params, "limit")
	var runs []Run
	if owner != "" && name != "" {
		runs = m.st.RepoRuns(owner, name)
	} else {
		runs = m.st.LoadAllRuns()
	}
	if limit > 0 && len(runs) > limit {
		runs = runs[:limit]
	}
	out := make([]map[string]any, 0, len(runs))
	for _, run := range runs {
		out = append(out, run.Record())
	}
	return map[string]any{"runs": out}, nil
}

func (m *Module) handleLog(ctx context.Context, req *rpc.Request) (any, error) {
	params := req.Object()
	id := numberParam(params, "run")
	run, ok := m.st.FindRun(id)
	if !ok {
		return nil, rpc.Fail("unknown_run")
	}
	since := int64(-1)
	if _, present := params["since"]; present {
		since = int64(floatParam(params, "since"))
	}
	slice := LogSliceOf(m.st.ReadLog(run), since, MaxLogChars)
	return map[string]any{"id": run.ID, "status": run.Status, "text": slice.Text, "from": slice.From,
		"next": slice.Next, "reset": slice.Reset, "path": m.st.LogPath(run)}, nil
}

func numberParam(params map[string]any, key string) int {
	value, ok := params[key].(float64)
	if !ok || value != float64(int64(value)) || value > 2147483647 || value < -2147483648 {
		return 0
	}
	return int(value)
}

func floatParam(params map[string]any, key string) float64 {
	value, _ := params[key].(float64)
	return value
}

func WorkflowsInRepoPaths(mirrorPath, localPath string) []Workflow {
	var out []Workflow
	if mirrorPath != "" && dirExists(mirrorPath) {
		listing, _ := gitStdoutAny(mirrorPath, 8*time.Second, "ls-tree", "-r", "--name-only", "HEAD", "--", ".forkmesh")
		for _, p := range splitNonEmpty(qtFromUtf8(listing), "\n") {
			if !strings.HasSuffix(p, ".yml") && !strings.HasSuffix(p, ".yaml") {
				continue
			}
			content, code := gitStdoutAny(mirrorPath, 8*time.Second, "show", "HEAD:"+p)
			if code != 0 {
				continue
			}
			out = append(out, Parse(p, qtFromUtf8(content)))
		}
	}
	if len(out) == 0 && localPath != "" {
		out = ParseDir(localPath)
	}
	return out
}

func gitStdoutAny(dir string, timeout time.Duration, args ...string) ([]byte, int) {
	ctx, cancel := context.WithTimeout(context.Background(), timeout)
	defer cancel()
	cmd := gitCommand(ctx, dir, args...)
	var stdout strings.Builder
	cmd.Stdout = &stdout
	err := cmd.Run()
	code, crashed := exitStatus(err)
	if crashed && code == 0 {
		code = -1
	}
	return []byte(stdout.String()), code
}
