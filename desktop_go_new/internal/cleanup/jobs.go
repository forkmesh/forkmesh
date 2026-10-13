package cleanup

import (
	"context"
	"os"
	"path/filepath"
	"runtime"
	"sync"
	"sync/atomic"

	"forkmesh.com/desktop-core/internal/ownergate"
	"forkmesh.com/desktop-core/internal/rpc"
)

type scanJob struct {
	id     int64
	tokens []string
	cancel atomic.Bool
	done   chan struct{}
}

type deleteJob struct {
	id     int64
	tokens []string
	done   chan struct{}
}

type ScanReply struct {
	ID          int64    `json:"id"`
	Tokens      []string `json:"tokens"`
	Items       []Item   `json:"items"`
	Notes       []string `json:"notes"`
	Cancelled   bool     `json:"cancelled"`
	ScannedAtMs int64    `json:"scannedAtMs"`
}

type DeleteReply struct {
	ID               int64             `json:"id"`
	Tokens           []string          `json:"tokens"`
	Outcomes         []Outcome         `json:"outcomes"`
	WorktreeRefusals map[string]string `json:"worktreeRefusals"`
	Logged           bool              `json:"logged"`
	Freed            int64             `json:"freed"`
	FinishedAtMs     int64             `json:"finishedAtMs"`
}

type DeleteRequest struct {
	Token     string    `json:"token"`
	Files     []Step    `json:"files"`
	Sandboxes []Step    `json:"sandboxes"`
	Worktrees []Step    `json:"worktrees"`
	Skipped   []Outcome `json:"skipped"`
	Claimed   []string  `json:"claimed"`
	Context   Context   `json:"context"`
	LogPath   string    `json:"logPath"`
}

type jobs struct {
	mu         sync.Mutex
	seq        int64
	scanning   *scanJob
	deleting   *deleteJob
	lastScan   *ScanReply
	lastDelete *DeleteReply
	wg         sync.WaitGroup
}

func (m *Module) registerJobs() {
	m.core.Registry.Handle("core.cleanup.scan", owner, ownergate.DesktopOnly(m.handleScan))
	m.core.Registry.Handle("core.cleanup.cancel", owner, ownergate.DesktopOnly(m.handleCancel))
	m.core.Registry.Handle("core.cleanup.state", owner, ownergate.DesktopOnly(m.handleState))
	m.core.Registry.Handle("core.cleanup.deleteFiles", owner, ownergate.DesktopOnly(m.handleDelete))
	m.core.Registry.Handle("core.cleanup.appendLog", owner, ownergate.DesktopOnly(m.handleAppendLog))
}

func (m *Module) Busy() bool {
	m.jobs.mu.Lock()
	defer m.jobs.mu.Unlock()
	return m.jobs.scanning != nil || m.jobs.deleting != nil
}

func (m *Module) Stop() {
	m.jobs.mu.Lock()
	if m.jobs.scanning != nil {
		m.jobs.scanning.cancel.Store(true)
	}
	m.jobs.mu.Unlock()
}

func (m *Module) Wait() { m.jobs.wg.Wait() }

func (m *Module) broadcast(event string, data any) {
	if m.core != nil {
		m.core.BroadcastUI(event, data)
	}
}

func (m *Module) logf(format string, args ...any) {
	if m.core != nil && m.core.Logger != nil {
		m.core.Logger.Printf(format, args...)
	}
}

func (m *Module) nowMs() int64 { return m.now().UnixMilli() }

func (m *Module) fill(c Context) Context {
	if c.Home == "" {
		c.Home, _ = os.UserHomeDir()
	}
	if len(c.TmpRoots) == 0 {
		c.TmpRoots = []string{filepath.Clean(os.TempDir())}
		if runtime.GOOS != "windows" && isDir("/tmp") && !contains(c.TmpRoots, "/tmp") {
			c.TmpRoots = append(c.TmpRoots, "/tmp")
		}
	}
	if c.UID == "" {
		c.UID = currentUID()
	}
	c.Processes = ReadProcesses(m.ProcRoot)
	c.NowMs = m.nowMs()
	c.Git = m.Git
	c.Ccache = m.Ccache
	return c
}

func (m *Module) protected() []string {
	if m.core == nil {
		return nil
	}
	layout := m.core.Layout
	var roots []string
	for _, root := range []string{layout.Data, layout.Config, layout.SSH, layout.State, layout.Logs} {
		if root != "" {
			roots = append(roots, root)
		}
	}
	return roots
}

func (m *Module) logPath(requested string) string {
	if requested != "" {
		return requested
	}
	if m.core == nil {
		return ""
	}
	return filepath.Join(m.core.Layout.Logs, "cleanup.log")
}

func appendToken(tokens []string, token string) []string {
	if token == "" || contains(tokens, token) {
		return tokens
	}
	return append(tokens, token)
}

func (m *Module) handleScan(ctx context.Context, req *rpc.Request) (any, error) {
	var params struct {
		Token   string  `json:"token"`
		Context Context `json:"context"`
	}
	if err := req.Bind(&params); err != nil {
		return nil, err
	}
	id, attached := m.StartScan(params.Token, params.Context)
	return map[string]any{"id": id, "attached": attached}, nil
}

func (m *Module) StartScan(token string, c Context) (int64, bool) {
	m.jobs.mu.Lock()
	if job := m.jobs.scanning; job != nil {
		job.tokens = appendToken(job.tokens, token)
		m.jobs.mu.Unlock()
		return job.id, true
	}
	m.jobs.seq++
	job := &scanJob{id: m.jobs.seq, tokens: appendToken(nil, token), done: make(chan struct{})}
	m.jobs.scanning = job
	wait := m.jobs.deleting
	m.jobs.wg.Add(1)
	m.jobs.mu.Unlock()
	go m.runScan(job, wait, c)
	return job.id, false
}

func (m *Module) runScan(job *scanJob, wait *deleteJob, c Context) {
	defer m.jobs.wg.Done()
	defer close(job.done)
	if wait != nil {
		<-wait.done
	}
	c = m.fill(c)
	tokens := func() []string {
		m.jobs.mu.Lock()
		defer m.jobs.mu.Unlock()
		return append([]string{}, job.tokens...)
	}
	result := Scan(m.base, c, &job.cancel, func(stage string) {
		m.broadcast("core.cleanup.progress", map[string]any{"id": job.id, "tokens": tokens(), "stage": stage})
	})
	m.jobs.mu.Lock()
	reply := &ScanReply{ID: job.id, Tokens: append([]string{}, job.tokens...), Items: result.Items, Notes: result.Notes,
		Cancelled: result.Cancelled, ScannedAtMs: m.nowMs()}
	m.jobs.lastScan = reply
	m.jobs.scanning = nil
	m.jobs.mu.Unlock()
	m.logf("cleanup: scan %d found %d items (stopped: %v)", job.id, len(reply.Items), reply.Cancelled)
	m.broadcast("core.cleanup.scanned", reply)
}

func (m *Module) handleCancel(ctx context.Context, req *rpc.Request) (any, error) {
	m.jobs.mu.Lock()
	defer m.jobs.mu.Unlock()
	job := m.jobs.scanning
	if job == nil {
		return map[string]any{"cancelled": false}, nil
	}
	job.cancel.Store(true)
	return map[string]any{"cancelled": true, "id": job.id}, nil
}

func (m *Module) handleState(ctx context.Context, req *rpc.Request) (any, error) {
	m.jobs.mu.Lock()
	defer m.jobs.mu.Unlock()
	state := map[string]any{"scanning": m.jobs.scanning != nil, "deleting": m.jobs.deleting != nil,
		"lastScan": m.jobs.lastScan, "lastDelete": m.jobs.lastDelete}
	if m.jobs.scanning != nil {
		state["scanId"] = m.jobs.scanning.id
	}
	if m.jobs.deleting != nil {
		state["deleteId"] = m.jobs.deleting.id
	}
	return state, nil
}

func (m *Module) handleDelete(ctx context.Context, req *rpc.Request) (any, error) {
	var request DeleteRequest
	if err := req.Bind(&request); err != nil {
		return nil, err
	}
	id, err := m.StartDelete(request)
	if err != nil {
		return nil, err
	}
	return map[string]any{"id": id}, nil
}

func (m *Module) StartDelete(request DeleteRequest) (int64, error) {
	m.jobs.mu.Lock()
	if m.jobs.scanning != nil || m.jobs.deleting != nil {
		m.jobs.mu.Unlock()
		return 0, rpc.Fail("busy")
	}
	m.jobs.seq++
	job := &deleteJob{id: m.jobs.seq, tokens: appendToken(nil, request.Token), done: make(chan struct{})}
	m.jobs.deleting = job
	m.jobs.wg.Add(1)
	m.jobs.mu.Unlock()
	go m.runDelete(job, request)
	return job.id, nil
}

func refusedOutcome(step Step, why string) Outcome {
	return Outcome{Path: cleanPath(step.Item.Path), Bytes: step.Item.Bytes, Reason: step.Item.ReasonText(), Error: why}
}

func (m *Module) runDelete(job *deleteJob, request DeleteRequest) {
	defer m.jobs.wg.Done()
	defer close(job.done)
	c := m.fill(request.Context)
	recheck := Recheck{Processes: c.Processes, Live: LiveClaudeSessions(c), NowMs: c.NowMs,
		LiveMinutes: c.liveMinutes(), Protected: m.protected()}
	claimed := map[string]bool{}
	for _, name := range request.Claimed {
		claimed[name] = true
	}
	outcomes := []Outcome{}
	var files, sandboxes []Step
	for _, step := range request.Files {
		if why := PlanRefusal(step.Item, step.Choice); why != "" || step.Item.Kind == KindWorktree || step.Item.Kind == KindActionSandbox {
			if why == "" {
				why = "not a files item"
			}
			outcomes = append(outcomes, refusedOutcome(step, why))
			continue
		}
		files = append(files, step)
	}
	for _, step := range request.Sandboxes {
		if why := PlanRefusal(step.Item, step.Choice); why != "" || step.Item.Kind != KindActionSandbox {
			if why == "" {
				why = "not an Actions sandbox"
			}
			outcomes = append(outcomes, refusedOutcome(step, why))
			continue
		}
		sandboxes = append(sandboxes, step)
	}
	outcomes = append(outcomes, DeleteFiles(files, recheck, nil)...)
	outcomes = append(outcomes, DeleteSandboxes(sandboxes, recheck, func(base, path string) string {
		return RemoveUnclaimedSandbox(base, path, claimed)
	})...)
	after := Recheck{Processes: ReadProcesses(m.ProcRoot)}
	refusals := map[string]string{}
	for _, step := range request.Worktrees {
		why := PlanRefusal(step.Item, step.Choice)
		if why == "" {
			why = WorktreeRefusal(step.Item, after)
		}
		if why != "" {
			refusals[step.Item.ID] = why
		}
	}
	now := m.nowMs()
	logged := AppendLog(m.logPath(request.LogPath), append(append([]Outcome{}, request.Skipped...), outcomes...), now)
	m.jobs.mu.Lock()
	reply := &DeleteReply{ID: job.id, Tokens: append([]string{}, job.tokens...), Outcomes: outcomes,
		WorktreeRefusals: refusals, Logged: logged, Freed: FreedBytes(outcomes), FinishedAtMs: now}
	m.jobs.lastDelete = reply
	m.jobs.deleting = nil
	m.jobs.mu.Unlock()
	removed := 0
	for _, outcome := range outcomes {
		if outcome.Removed {
			removed++
		}
	}
	m.logf("cleanup: delete %d removed %d, kept %d, freed %d bytes", job.id, removed, len(outcomes)-removed, reply.Freed)
	m.broadcast("core.cleanup.deleted", reply)
}

func (m *Module) handleAppendLog(ctx context.Context, req *rpc.Request) (any, error) {
	var params struct {
		Outcomes []Outcome `json:"outcomes"`
		LogPath  string    `json:"logPath"`
		NowMs    int64     `json:"nowMs"`
	}
	if err := req.Bind(&params); err != nil {
		return nil, err
	}
	now := params.NowMs
	if now <= 0 {
		now = m.nowMs()
	}
	return map[string]any{"logged": AppendLog(m.logPath(params.LogPath), params.Outcomes, now)}, nil
}
