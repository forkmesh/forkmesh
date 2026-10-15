package update

import (
	"context"
	"encoding/json"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"reflect"
	"strings"
	"sync"
	"time"

	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/ownergate"
	"forkmesh.com/desktop-core/internal/rpc"
)

const (
	owner          = "update"
	actionName     = "updateRestart"
	subjectName    = "desktop"
	flushEvery     = 500 * time.Millisecond
	outputEvery    = 100 * time.Millisecond
	instanceVar    = "FORKMESH_DESKTOP_INSTANCE"
	quitWaitTime   = 30 * time.Second
	relaunchWindow = 20 * time.Minute
)

type Config struct {
	ClientDir     string   `json:"clientDir"`
	AppPath       string   `json:"appPath"`
	SelfPath      string   `json:"selfPath"`
	Args          []string `json:"args"`
	Cwd           string   `json:"cwd"`
	Unit          string   `json:"unit"`
	BuildType     string   `json:"buildType"`
	Version       string   `json:"version"`
	StartedAtMs   int64    `json:"startedAtMs"`
	PID           int      `json:"pid"`
	AutoUpdateDir string   `json:"autoUpdateDir"`
}

type desktopState struct {
	Running  bool   `json:"running"`
	Kind     string `json:"kind"`
	Percent  int    `json:"percent"`
	Phase    string `json:"phase"`
	Activity string `json:"activity"`
}

type watcher struct {
	peer      rpc.Peer
	sentSeq   int64
	sentState map[string]any
}

type Module struct {
	Owners    func() []string
	Env       *Env
	Now       func() time.Time
	Launch    func(program string, args []string, dir string) error
	CallUI    func(ctx context.Context, method string, params any) ([]byte, error)
	NotifyUI  func(method string, params any) bool
	HasUI     func() bool
	LogSystem func(text string)

	core     *daemon.Core
	gate     *ownergate.Gate
	confirms *confirmations
	ctx      context.Context

	mu               sync.Mutex
	feed             Feed
	config           *Config
	desktop          desktopState
	job              *job
	jobKind          string
	jobPercent       int
	jobPhase         string
	jobPass          int
	relaunching      bool
	relaunchPID      int
	relaunchAt       int64
	watchers         map[string]*watcher
	ticking          bool
	pending          strings.Builder
	outputArmed      bool
	lastResult       map[string]any
	waitingLoggedFor string
	autoChecking     bool
}

func New() *Module {
	return &Module{confirms: newConfirmations(), watchers: map[string]*watcher{}}
}

func (m *Module) Name() string { return owner }

func (m *Module) now() time.Time {
	if m.Now != nil {
		return m.Now()
	}
	return time.Now()
}

func (m *Module) env() *Env {
	if m.Env == nil {
		m.Env = defaultEnv()
	}
	return m.Env
}

func (m *Module) Start(ctx context.Context, core *daemon.Core) error {
	m.core = core
	m.ctx = ctx
	m.gate = &ownergate.Gate{Core: core, Owners: m.Owners}
	if m.confirms == nil {
		m.confirms = newConfirmations()
	}
	if m.watchers == nil {
		m.watchers = map[string]*watcher{}
	}
	phone := func(method string, handler rpc.Handler) {
		core.Registry.Handle(method, owner, m.gate.Wrap(m.configured(handler)))
	}
	phone("update.status", m.handleStatus)
	phone("update.unwatch", m.handleUnwatch)
	phone("update.prepare", m.handlePrepare)
	phone("update.run", m.handleRun)
	desktop := func(method string, handler rpc.Handler) {
		core.Registry.Handle(method, owner, ownergate.DesktopOnly(handler))
	}
	desktop("core.update.config", m.handleConfig)
	desktop("core.update.start", m.handleStart)
	desktop("core.update.prebuilt", m.handlePrebuilt)
	desktop("core.update.cancel", m.handleCancel)
	desktop("core.update.state", m.handleState)
	desktop("core.update.desktop", m.handleDesktop)
	desktop("core.update.reset", m.handleReset)
	desktop("core.update.append", m.handleAppend)
	desktop("core.update.note", m.handleNote)
	go m.autoUpdateLoop(ctx)
	return nil
}

func (m *Module) Stop() {
	m.mu.Lock()
	running := m.job
	m.mu.Unlock()
	if running != nil {
		running.cancel()
	}
}

func (m *Module) Busy() bool {
	m.mu.Lock()
	defer m.mu.Unlock()
	return m.job != nil || m.relaunchPendingLocked()
}

func (m *Module) configured(handler rpc.Handler) rpc.Handler {
	return func(ctx context.Context, req *rpc.Request) (any, error) {
		m.mu.Lock()
		ready := m.config != nil
		m.mu.Unlock()
		if !ready {
			return nil, rpc.ErrDelegate
		}
		return handler(ctx, req)
	}
}

func (m *Module) callUI(ctx context.Context, method string, params any) ([]byte, error) {
	if m.CallUI != nil {
		return m.CallUI(ctx, method, params)
	}
	return m.core.CallUI(ctx, method, params)
}

func (m *Module) notifyUI(method string, params any) bool {
	if m.NotifyUI != nil {
		return m.NotifyUI(method, params)
	}
	return m.core.NotifyUI(method, params)
}

func (m *Module) hasUI() bool {
	if m.HasUI != nil {
		return m.HasUI()
	}
	return m.core.UI() != nil
}

func (m *Module) logf(format string, args ...any) {
	if m.core != nil && m.core.Logger != nil {
		m.core.Logger.Printf("update: "+format, args...)
	}
}

func (m *Module) relaunchPendingLocked() bool {
	if !m.relaunching {
		return false
	}
	if m.job == nil && m.now().UnixMilli()-m.relaunchAt > relaunchWindow.Milliseconds() {
		m.relaunching = false
		return false
	}
	return true
}

func (m *Module) stateLocked() RunState {
	if m.job != nil || m.relaunchPendingLocked() {
		return RunState{Running: true, Kind: m.jobKind, Percent: m.jobPercent, Phase: m.jobPhase, Activity: m.desktop.Activity}
	}
	return RunState{Running: m.desktop.Running, Kind: m.desktop.Kind, Percent: m.desktop.Percent,
		Phase: m.desktop.Phase, Activity: m.desktop.Activity}
}

func (m *Module) stateObjectLocked() map[string]any {
	return stateObject(&m.feed, m.stateLocked())
}

func (m *Module) uiStateLocked() map[string]any {
	state := m.stateObjectLocked()
	state["job"] = m.job != nil
	state["pass"] = m.jobPass
	state["relaunchPending"] = m.relaunchPendingLocked()
	return state
}

func (m *Module) broadcastUI(event string, data any) {
	if m.core != nil {
		m.core.BroadcastUI(event, data)
	}
}

func (m *Module) flushOutputLocked() (map[string]any, bool) {
	if m.pending.Len() == 0 {
		return nil, false
	}
	data := map[string]any{"run": m.feed.Run(), "text": m.pending.String()}
	m.pending.Reset()
	return data, true
}

func (m *Module) sendOutput() {
	m.mu.Lock()
	m.outputArmed = false
	data, ok := m.flushOutputLocked()
	m.mu.Unlock()
	if ok {
		m.broadcastUI("core.update.output", data)
	}
}

func (m *Module) emitLocked(event string, data any) func() {
	output, hasOutput := m.flushOutputLocked()
	return func() {
		if hasOutput {
			m.broadcastUI("core.update.output", output)
		}
		m.broadcastUI(event, data)
	}
}

func (m *Module) appendLog(text string) {
	if text == "" {
		return
	}
	m.mu.Lock()
	m.feed.Append(text)
	m.pending.WriteString(text)
	arm := !m.outputArmed
	m.outputArmed = true
	m.mu.Unlock()
	if arm {
		time.AfterFunc(outputEvery, m.sendOutput)
	}
	m.ensureTicker()
}

func (m *Module) status(text string, isError bool) {
	prefix := "==> "
	if isError {
		prefix = "!! "
	}
	if isError {
		m.restartLog("ERROR: " + text)
	} else {
		m.restartLog(text)
	}
	m.appendLog(prefix + text + "\n")
	m.mu.Lock()
	m.feed.NoteStatus(text, isError)
	send := m.emitLocked("core.update.state", m.withStatusLocked(text, isError))
	m.mu.Unlock()
	send()
	m.flushWatchers()
}

func (m *Module) withStatusLocked(text string, isError bool) map[string]any {
	state := m.uiStateLocked()
	state["statusChanged"] = true
	state["statusText"] = text
	state["statusError"] = isError
	return state
}

func (m *Module) phase(name string, pass int) {
	m.mu.Lock()
	m.jobPhase = name
	m.jobPass = pass
	send := m.emitLocked("core.update.state", m.uiStateLocked())
	m.mu.Unlock()
	send()
}

func (m *Module) percent(value int) {
	m.mu.Lock()
	m.jobPercent = value
	send := m.emitLocked("core.update.state", m.uiStateLocked())
	m.mu.Unlock()
	send()
}

func (m *Module) restartLog(text string) {
	m.logf("%s", text)
	m.mu.Lock()
	send := m.emitLocked("core.update.log", map[string]any{"text": text})
	m.mu.Unlock()
	send()
}

func (m *Module) ensureTicker() {
	m.mu.Lock()
	if m.ticking || len(m.watchers) == 0 {
		m.mu.Unlock()
		return
	}
	m.ticking = true
	m.mu.Unlock()
	go func() {
		ticker := time.NewTicker(flushEvery)
		defer ticker.Stop()
		for {
			var done <-chan struct{}
			if m.ctx != nil {
				done = m.ctx.Done()
			}
			select {
			case <-done:
				m.mu.Lock()
				m.ticking = false
				m.mu.Unlock()
				return
			case <-ticker.C:
			}
			if !m.flushWatchers() {
				m.mu.Lock()
				m.ticking = false
				m.mu.Unlock()
				return
			}
		}
	}()
}

func (m *Module) flushWatchers() bool {
	type delivery struct {
		w    *watcher
		data map[string]any
		seq  int64
	}
	m.mu.Lock()
	if len(m.watchers) == 0 {
		m.mu.Unlock()
		return false
	}
	state := m.stateObjectLocked()
	var out []delivery
	for _, w := range m.watchers {
		batch := m.feed.BatchSince(w.sentSeq)
		if len(batch.Lines) == 0 && reflect.DeepEqual(state, w.sentState) {
			continue
		}
		data := map[string]any{}
		for key, value := range state {
			data[key] = value
		}
		data["first"] = batch.First
		data["seq"] = batch.Seq
		data["skipped"] = batch.Skipped
		data["lines"] = batch.Lines
		out = append(out, delivery{w, data, m.feed.Seq()})
	}
	m.mu.Unlock()
	for _, d := range out {
		if !d.w.peer.Send("update.progress", d.data) {
			m.removeWatcher(d.w.peer)
			continue
		}
		m.mu.Lock()
		if current := m.watchers[d.w.peer.ID()]; current == d.w {
			d.w.sentSeq = d.seq
			d.w.sentState = state
		}
		m.mu.Unlock()
	}
	m.mu.Lock()
	defer m.mu.Unlock()
	return len(m.watchers) > 0
}

func (m *Module) watch(peer rpc.Peer) {
	if peer == nil {
		return
	}
	m.mu.Lock()
	current, exists := m.watchers[peer.ID()]
	w := &watcher{peer: peer, sentSeq: m.feed.Seq(), sentState: m.stateObjectLocked()}
	m.watchers[peer.ID()] = w
	m.mu.Unlock()
	if !exists || current.peer != peer {
		go func() {
			select {
			case <-peer.Done():
				m.removeWatcher(peer)
			case <-m.doneChan():
			}
		}()
	}
	m.ensureTicker()
}

func (m *Module) doneChan() <-chan struct{} {
	if m.ctx == nil {
		return nil
	}
	return m.ctx.Done()
}

func (m *Module) removeWatcher(peer rpc.Peer) {
	m.mu.Lock()
	defer m.mu.Unlock()
	if current, ok := m.watchers[peer.ID()]; ok && current.peer == peer {
		delete(m.watchers, peer.ID())
	}
}

func (m *Module) versionAndStart() (string, int64) {
	version := ""
	var started int64
	if m.config != nil {
		version = m.config.Version
		started = m.config.StartedAtMs
	}
	if version == "" && m.core != nil {
		version = m.core.Version
	}
	if started == 0 && m.core != nil {
		started = m.core.Started.UnixMilli()
	}
	return version, started
}

func (m *Module) handleStatus(ctx context.Context, req *rpc.Request) (any, error) {
	m.mu.Lock()
	reply := m.stateObjectLocked()
	lines := m.feed.Backlog(maxBacklogLines)
	reply["lines"] = lines
	reply["seq"] = m.feed.Seq()
	reply["first"] = m.feed.Seq() - int64(len(lines)) + 1
	version, started := m.versionAndStart()
	reply["version"] = version
	reply["startedAtMs"] = started
	m.mu.Unlock()
	m.watch(req.Peer)
	return reply, nil
}

func (m *Module) handleUnwatch(ctx context.Context, req *rpc.Request) (any, error) {
	if req.Peer != nil {
		m.removeWatcher(req.Peer)
	}
	return map[string]any{}, nil
}

func (m *Module) running() bool {
	m.mu.Lock()
	defer m.mu.Unlock()
	return m.stateLocked().Running
}

func (m *Module) agentsRunning(ctx context.Context) int {
	if !m.hasUI() {
		return 0
	}
	ctx, cancel := context.WithTimeout(ctx, 5*time.Second)
	defer cancel()
	raw, err := m.callUI(ctx, "ui.update.agents", map[string]any{})
	if err != nil {
		return 0
	}
	var reply struct {
		Running int `json:"running"`
	}
	_ = json.Unmarshal(raw, &reply)
	return reply.Running
}

func actionSummary(agentsRunning int, version string) string {
	text := "Rebuild ForkMesh from this desktop's source checkout and restart it"
	if version != "" {
		text += " (now on " + version + ")"
	}
	text += ". The desktop drops this connection while it restarts."
	if agentsRunning == 1 {
		text += " 1 agent is running; it is queued again after the restart."
	} else if agentsRunning > 1 {
		text += fmt.Sprintf(" %d agents are running; they are queued again after the restart.", agentsRunning)
	}
	return text
}

func paramString(req *rpc.Request, key string) string {
	value, _ := req.Object()[key].(string)
	return value
}

func (m *Module) handlePrepare(ctx context.Context, req *rpc.Request) (any, error) {
	if paramString(req, "action") != actionName {
		return nil, rpc.Fail("unknown_action")
	}
	if m.running() {
		return nil, rpc.Fail("already_running")
	}
	agents := m.agentsRunning(ctx)
	m.mu.Lock()
	version, _ := m.versionAndStart()
	m.mu.Unlock()
	token := m.confirms.issue(ownergate.DeviceKey(req.Peer), actionName, subjectName, m.now().UnixMilli())
	return map[string]any{
		"token":       token,
		"action":      actionName,
		"subject":     subjectName,
		"summary":     actionSummary(agents, version),
		"expiresInMs": confirmationLifetimeMs,
	}, nil
}

func (m *Module) handleRun(ctx context.Context, req *rpc.Request) (any, error) {
	action := paramString(req, "action")
	verdict := m.confirms.consume(paramString(req, "token"), ownergate.DeviceKey(req.Peer), action, subjectName, m.now().UnixMilli())
	label := ownergate.DeviceLabel(req.Peer)
	if action != actionName || verdict != confirmOK {
		why := confirmationError(verdict)
		if action != actionName {
			why = "unknown_action"
		}
		m.core.LogUI("Refused a rebuild and restart from %s: %s.", label, why)
		return nil, rpc.Fail(why)
	}
	if m.running() {
		return nil, rpc.Fail("already_running")
	}
	m.core.LogUI("Rebuild and restart requested from %s.", label)
	m.watch(req.Peer)
	go m.startFromPhone(label)
	return map[string]any{
		"started": true,
		"action":  actionName,
		"subject": subjectName,
		"message": "Rebuilding. The desktop restarts when the build is done.",
	}, nil
}

func (m *Module) startFromPhone(label string) {
	if m.hasUI() {
		ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
		_, err := m.callUI(ctx, "ui.update.start", map[string]any{"requestedBy": label})
		cancel()
		if err == nil {
			return
		}
		if strings.Contains(err.Error(), "already_running") {
			m.logf("the desktop is already restarting; the phone's request joins that run")
			return
		}
		m.logf("the desktop did not start the rebuild (%v); running it in the core", err)
	}
	m.mu.Lock()
	config := m.config
	m.mu.Unlock()
	if config == nil {
		return
	}
	req := Request{Kind: "rebuild", ClientDir: config.ClientDir, BuildType: config.BuildType, AppPath: config.AppPath,
		Args: config.Args, Cwd: config.Cwd, Unit: config.Unit, SelfPath: config.SelfPath}
	if req.BuildType == "" {
		req.BuildType = "RelWithDebInfo"
	}
	_, _ = m.begin(req, true, label)
}

func (m *Module) handleConfig(ctx context.Context, req *rpc.Request) (any, error) {
	var config Config
	if err := req.Bind(&config); err != nil {
		return nil, err
	}
	m.mu.Lock()
	previous := m.config
	m.config = &config
	if m.relaunching && m.job == nil && previous != nil &&
		(config.StartedAtMs != previous.StartedAtMs || config.PID != previous.PID) {
		m.relaunching = false
	}
	m.mu.Unlock()
	m.flushWatchers()
	return map[string]any{"ok": true, "capabilities": Capabilities}, nil
}

var Capabilities = []string{"installSource", "asUser", "prebuilt", "clean", "quick", "autoUpdate"}

func (m *Module) handleStart(ctx context.Context, req *rpc.Request) (any, error) {
	var start Request
	if err := req.Bind(&start); err != nil {
		return nil, err
	}
	if start.Kind == "" {
		start.Kind = "rebuild"
	}
	if start.Kind != "rebuild" && start.Kind != "update" && start.Kind != "quick" {
		return nil, rpc.Fail("unknown_kind")
	}
	if strings.TrimSpace(start.ClientDir) == "" {
		return nil, rpc.Fail("bad_params")
	}
	run, err := m.begin(start, false, "")
	if err != nil {
		return nil, err
	}
	return map[string]any{"started": true, "run": run}, nil
}

func (m *Module) handlePrebuilt(ctx context.Context, req *rpc.Request) (any, error) {
	var start PrebuiltRequest
	if err := req.Bind(&start); err != nil {
		return nil, err
	}
	if strings.TrimSpace(start.ClientDir) == "" || strings.TrimSpace(start.Tag) == "" {
		return nil, rpc.Fail("bad_params")
	}
	if m.running() {
		return nil, rpc.Fail("already_running")
	}
	plan, handled := m.planPrebuilt(ctx, start)
	if plan == nil {
		return map[string]any{"handled": handled, "started": false}, nil
	}
	start.Kind = "prebuilt"
	start.AsUser = ""
	start.Clean = false
	if start.Label == "" {
		start.Label = "update"
	}
	j := newJob(start.Request, m.env(), m)
	j.prebuilt = plan
	run, err := m.beginJob(j, false, "")
	if err != nil {
		return nil, err
	}
	return map[string]any{"handled": true, "started": true, "run": run}, nil
}

func (m *Module) begin(req Request, reset bool, requestedBy string) (int, error) {
	return m.beginJob(newJob(req, m.env(), m), reset, requestedBy)
}

func (m *Module) beginJob(j *job, reset bool, requestedBy string) (int, error) {
	req := j.req
	m.mu.Lock()
	if m.job != nil || m.relaunchPendingLocked() {
		m.mu.Unlock()
		return 0, rpc.Fail("already_running")
	}
	if reset {
		m.feed.Reset()
	}
	m.job = j
	m.jobKind = req.Kind
	if req.Label != "" {
		m.jobKind = req.Label
	}
	m.jobPercent = 0
	m.jobPhase = ""
	m.jobPass = 0
	m.lastResult = nil
	run := m.feed.Run()
	started := map[string]any{"run": run, "kind": req.Kind, "requestedBy": requestedBy, "reset": reset}
	send := m.emitLocked("core.update.started", started)
	m.mu.Unlock()
	send()
	if reset {
		m.appendLog("==> ForkMesh update started\n")
	}
	m.restartLog(req.Kind + " started in the core")
	go m.execute(j)
	return run, nil
}

func (m *Module) execute(j *job) {
	result := j.run()
	if result == outcomeInstalled {
		result = m.relaunch(j)
	}
	m.mu.Lock()
	if m.job == j {
		m.job = nil
	}
	if result != outcomeInstalled {
		m.relaunching = false
	}
	finished := map[string]any{
		"run":       m.feed.Run(),
		"ok":        result == outcomeInstalled,
		"cancelled": result == outcomeCancelled,
		"error":     m.feed.Error(),
	}
	if j.flash != "" {
		finished["flash"] = j.flash
	}
	if result == outcomeFallback {
		finished["fallback"] = "source"
	}
	m.lastResult = finished
	send := m.emitLocked("core.update.finished", finished)
	m.mu.Unlock()
	send()
	m.flushWatchers()
	if result == outcomeFallback {
		m.sourceFallback(j.req)
	}
}

func (m *Module) sourceFallback(req Request) {
	if m.hasUI() {
		return
	}
	update := Request{Kind: "update", Label: "update", ClientDir: req.ClientDir, BuildType: "Release", AppPath: req.AppPath,
		Args: req.Args, Cwd: req.Cwd, Unit: req.Unit, SelfPath: req.SelfPath, CatalogURL: req.CatalogURL,
		RepoDir: filepath.Dir(filepath.Clean(req.ClientDir))}
	m.restartLog("no desktop is attached; the core runs the source update itself")
	if _, err := m.begin(update, false, ""); err != nil {
		m.logf("the source update after the prebuilt fallback did not start: %v", err)
	}
}

func samePath(a, b string) bool {
	if a == "" || b == "" {
		return false
	}
	if ca, cb := canonical(a), canonical(b); ca != "" && ca == cb {
		return true
	}
	absA, errA := filepath.Abs(a)
	absB, errB := filepath.Abs(b)
	return errA == nil && errB == nil && filepath.Clean(absA) == filepath.Clean(absB)
}

func (m *Module) launch(program string, args []string, dir string) error {
	if m.Launch != nil {
		return m.Launch(program, args, dir)
	}
	return startReleased(program, args, dir)
}

func releasedEnvironment() []string {
	var env []string
	for _, entry := range os.Environ() {
		if strings.HasPrefix(entry, instanceVar+"=") {
			continue
		}
		env = append(env, entry)
	}
	return env
}

func startReleased(program string, args []string, dir string) error {
	cmd := exec.Command(program, args...)
	cmd.Dir = dir
	cmd.Env = releasedEnvironment()
	prepareReleased(cmd)
	if err := cmd.Start(); err != nil {
		return err
	}
	go func() { _ = cmd.Wait() }()
	return nil
}

func (m *Module) relaunch(j *job) outcome {
	req := j.req
	appPath := req.AppPath
	supervised := req.Unit != "" && samePath(appPath, req.SelfPath)
	m.mu.Lock()
	m.relaunching = true
	if m.config != nil {
		m.relaunchPID = m.config.PID
	}
	m.relaunchAt = m.now().UnixMilli()
	m.mu.Unlock()
	ui := m.hasUI()
	if req.AsUser != "" {
		return m.relaunchAsUser(req, ui)
	}
	if supervised {
		m.restartLog(appPath + " runs under a systemd user service; restarting the unit instead of starting a child it would stop")
		if ui {
			ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
			_, err := m.callUI(ctx, "ui.update.relaunch", map[string]any{"mode": "supervised", "appPath": appPath})
			cancel()
			if err == nil {
				return outcomeInstalled
			}
		}
		if err := m.launch("systemctl", []string{"--user", "restart", "--no-block", req.Unit}, ""); err != nil {
			m.status("Update installed but the relaunch failed; still running the previous version in memory.", true)
			return outcomeFailed
		}
		return outcomeInstalled
	}
	released := false
	if ui {
		ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
		_, err := m.callUI(ctx, "ui.update.relaunch", map[string]any{"mode": "release", "appPath": appPath})
		cancel()
		released = err == nil
		if !released {
			m.notifyUI("ui.update.quit", map[string]any{"appPath": appPath})
			m.waitForDesktopExit()
		}
	}
	dir := req.Cwd
	if err := m.launch(appPath, req.Args, dir); err != nil {
		m.logf("relaunch of %s failed: %v", appPath, err)
		m.status("Update installed but the relaunch failed; still running the previous version in memory.", true)
		return outcomeFailed
	}
	m.restartLog(fmt.Sprintf("relaunched %s; quitting", appPath))
	if released {
		m.notifyUI("ui.update.quit", map[string]any{"appPath": appPath})
	}
	return outcomeInstalled
}

func (m *Module) relaunchAsUser(req Request, ui bool) outcome {
	appPath := req.AppPath
	released := false
	if ui {
		ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
		_, err := m.callUI(ctx, "ui.update.relaunch", map[string]any{"mode": "release", "appPath": appPath})
		cancel()
		released = err == nil
		if !released {
			m.notifyUI("ui.update.quit", map[string]any{"appPath": appPath})
			m.waitForDesktopExit()
		}
	}
	args := append([]string{"-u", req.AsUser, "-H", appPath}, req.Args...)
	if err := m.launch("sudo", args, req.Cwd); err != nil {
		m.logf("relaunch of %s as %s failed: %v", appPath, req.AsUser, err)
	}
	m.restartLog(fmt.Sprintf("relaunched %s; quitting", appPath))
	if released {
		m.notifyUI("ui.update.quit", map[string]any{"appPath": appPath})
	}
	return outcomeInstalled
}

func (m *Module) waitForDesktopExit() {
	m.mu.Lock()
	pid := 0
	if m.config != nil {
		pid = m.config.PID
	}
	m.mu.Unlock()
	deadline := time.Now().Add(quitWaitTime)
	for time.Now().Before(deadline) {
		if !m.hasUI() && (pid <= 0 || !processAlive(pid)) {
			return
		}
		time.Sleep(200 * time.Millisecond)
	}
}

func (m *Module) handleCancel(ctx context.Context, req *rpc.Request) (any, error) {
	m.mu.Lock()
	running := m.job
	m.mu.Unlock()
	if running == nil {
		return map[string]any{"cancelled": false}, nil
	}
	running.cancel()
	m.restartLog("cancelled from the desktop")
	return map[string]any{"cancelled": true}, nil
}

func (m *Module) handleState(ctx context.Context, req *rpc.Request) (any, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	state := m.uiStateLocked()
	state["lines"] = m.feed.Backlog(maxBacklogLines)
	state["seq"] = m.feed.Seq()
	if m.lastResult != nil {
		state["last"] = m.lastResult
	}
	return state, nil
}

func (m *Module) handleDesktop(ctx context.Context, req *rpc.Request) (any, error) {
	var state desktopState
	if err := req.Bind(&state); err != nil {
		return nil, err
	}
	m.mu.Lock()
	m.desktop = state
	m.mu.Unlock()
	m.ensureTicker()
	return map[string]any{}, nil
}

func (m *Module) handleReset(ctx context.Context, req *rpc.Request) (any, error) {
	m.mu.Lock()
	if m.job == nil {
		m.feed.Reset()
	}
	run := m.feed.Run()
	m.mu.Unlock()
	return map[string]any{"run": run}, nil
}

func (m *Module) handleAppend(ctx context.Context, req *rpc.Request) (any, error) {
	text := paramString(req, "text")
	m.mu.Lock()
	m.feed.Append(text)
	m.mu.Unlock()
	m.ensureTicker()
	return map[string]any{}, nil
}

func (m *Module) handleNote(ctx context.Context, req *rpc.Request) (any, error) {
	object := req.Object()
	text, _ := object["status"].(string)
	isError, _ := object["error"].(bool)
	m.mu.Lock()
	m.feed.NoteStatus(text, isError)
	m.mu.Unlock()
	m.flushWatchers()
	return map[string]any{}, nil
}
