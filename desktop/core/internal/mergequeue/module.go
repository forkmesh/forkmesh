package mergequeue

import (
	"context"
	"encoding/json"
	"strconv"
	"strings"
	"sync"
	"time"

	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/git"
	"forkmesh.com/desktop-core/internal/ownergate"
	"forkmesh.com/desktop-core/internal/phonefamily"
	"forkmesh.com/desktop-core/internal/pulls"
	"forkmesh.com/desktop-core/internal/rpc"
)

const (
	owner        = "mergequeue"
	retryDelay   = 20000 * time.Millisecond
	nextDelay    = 1500 * time.Millisecond
	viewTimeout  = 10 * time.Second
	applyTimeout = 30 * time.Second
	purgeTimeout = 11 * time.Minute
	applyRetries = 3
)

type Record struct {
	Owner               string   `json:"owner"`
	Name                string   `json:"name"`
	Enabled             bool     `json:"enabled"`
	Paused              bool     `json:"paused"`
	Rows                []string `json:"rows"`
	RequirePeerApproval bool     `json:"requirePeerApproval"`
	PreviewOnly         bool     `json:"previewOnly"`
	ActiveRelay         bool     `json:"activeRelay"`
	LocalPath           string   `json:"localPath"`
	MirrorPath          string   `json:"mirrorPath"`
}

type View struct {
	Identity      bool     `json:"identity"`
	Open          int      `json:"open"`
	Busy          bool     `json:"busy"`
	AiFix         bool     `json:"aiFix"`
	Deleting      bool     `json:"deleting"`
	ConflictMerge bool     `json:"conflictMerge"`
	Repos         []Record `json:"repos"`
}

func (v *View) indexFor(owner, name string) int {
	for i, r := range v.Repos {
		if !r.PreviewOnly && r.Owner == owner && r.Name == name && r.ActiveRelay {
			return i
		}
	}
	return -1
}

func (v *View) openIndex() int {
	if v.Open < 0 || v.Open >= len(v.Repos) {
		return -1
	}
	return v.Open
}

type Module struct {
	Owners func() []string
	Now    func() time.Time
	CallUI func(ctx context.Context, method string, params any) ([]byte, error)
	Runner *git.Runner

	core          *daemon.Core
	ctx           context.Context
	gate          *phonefamily.Gate
	confirmations *phonefamily.Confirmations

	mu         sync.Mutex
	view       *View
	configured bool
	running    bool
	timer      *time.Timer
	deadline   time.Time
	passDone   chan struct{}
}

func New() *Module { return &Module{confirmations: phonefamily.NewConfirmations()} }

func (m *Module) Name() string { return owner }

func (m *Module) Start(ctx context.Context, core *daemon.Core) error {
	m.core = core
	m.ctx = ctx
	m.gate = &phonefamily.Gate{Core: core, Owners: m.Owners}
	if m.confirmations == nil {
		m.confirmations = phonefamily.NewConfirmations()
	}
	phone := func(method string, handler rpc.Handler) {
		core.Registry.Handle(method, owner, m.gate.Wrap(m.whenConfigured(handler)))
	}
	phone("mergequeue.list", m.handleList)
	phone("mergequeue.prepare", m.handlePrepare)
	phone("mergequeue.run", m.handleRun)
	desktop := func(method string, handler rpc.Handler) {
		core.Registry.Handle(method, owner, ownergate.DesktopOnly(handler))
	}
	desktop("core.mergequeue.view", m.handleView)
	desktop("core.mergequeue.schedule", m.handleSchedule)
	desktop("core.mergequeue.state", m.handleState)
	return nil
}

func (m *Module) Stop() {
	m.mu.Lock()
	if m.timer != nil {
		m.timer.Stop()
	}
	m.mu.Unlock()
}

func (m *Module) Busy() bool {
	m.mu.Lock()
	defer m.mu.Unlock()
	return m.running
}

func (m *Module) whenConfigured(handler rpc.Handler) rpc.Handler {
	return func(ctx context.Context, req *rpc.Request) (any, error) {
		m.mu.Lock()
		ready := m.configured
		m.mu.Unlock()
		if !ready {
			return nil, rpc.ErrDelegate
		}
		return handler(ctx, req)
	}
}

func (m *Module) nowMs() int64 {
	if m.Now != nil {
		return m.Now().UnixMilli()
	}
	return time.Now().UnixMilli()
}

func (m *Module) baseCtx() context.Context {
	if m.ctx != nil {
		return m.ctx
	}
	return context.Background()
}

func (m *Module) callUI(ctx context.Context, method string, params any) ([]byte, error) {
	if m.CallUI != nil {
		return m.CallUI(ctx, method, params)
	}
	if m.core == nil {
		return nil, rpc.Fail("desktop_unavailable")
	}
	return m.core.CallUI(ctx, method, params)
}

func (m *Module) logUI(format string, args ...any) {
	if m.core != nil {
		m.core.LogUI(format, args...)
	}
}

func (m *Module) warn(text string) {
	if m.core != nil && m.core.Logger != nil {
		m.core.Logger.Print(text)
	}
}

func (m *Module) setView(view *View) {
	m.mu.Lock()
	m.view = view
	m.configured = true
	m.mu.Unlock()
}

func (m *Module) cachedView() *View {
	m.mu.Lock()
	defer m.mu.Unlock()
	if m.view == nil {
		return &View{Open: -1}
	}
	copied := *m.view
	copied.Repos = append([]Record(nil), m.view.Repos...)
	copied.Open = -1
	copied.Busy, copied.AiFix, copied.Deleting, copied.ConflictMerge = false, false, false, false
	return &copied
}

func (m *Module) fetchView(ctx context.Context) (*View, bool) {
	callCtx, cancel := context.WithTimeout(ctx, viewTimeout)
	defer cancel()
	raw, err := m.callUI(callCtx, "ui.mergequeue.view", map[string]any{})
	if err != nil {
		return nil, false
	}
	view := &View{Open: -1}
	if json.Unmarshal(raw, view) != nil {
		return nil, false
	}
	m.setView(view)
	return view, true
}

func (m *Module) currentView(ctx context.Context) *View {
	if view, ok := m.fetchView(ctx); ok {
		return view
	}
	return m.cachedView()
}

func (m *Module) dataRoot() string {
	if m.core == nil {
		return ""
	}
	return m.core.Layout.Data
}

func (m *Module) store(ctx context.Context, view *View, record Record) *pulls.Store {
	return &pulls.Store{
		WorkTree:      record.LocalPath,
		Mirror:        record.MirrorPath,
		IdentityValid: view.Identity,
		DataRoot:      m.dataRoot(),
		Runner:        m.Runner,
		Ctx:           ctx,
		Warn:          m.warn,
		AmLive: func() bool {
			fresh, ok := m.fetchView(ctx)
			return ok && fresh.ConflictMerge
		},
		Purge: func(number int) error {
			callCtx, cancel := context.WithTimeout(ctx, purgeTimeout)
			defer cancel()
			_, err := m.callUI(callCtx, "ui.mergequeue.purge", map[string]any{"owner": record.Owner, "name": record.Name, "number": number})
			return err
		},
	}
}

func (m *Module) repoState(ctx context.Context, view *View, index int) (repoState, *pulls.Store) {
	record := view.Repos[index]
	store := m.store(ctx, view, record)
	return repoState{
		owner:    record.Owner,
		name:     record.Name,
		enabled:  record.Enabled,
		paused:   record.Paused,
		open:     index == view.openIndex(),
		writable: store.CanWrite(),
		queue:    FromRows(record.Rows),
	}, store
}

func (m *Module) row(ctx context.Context, view *View, index int) map[string]any {
	if index < 0 || index >= len(view.Repos) {
		return map[string]any{}
	}
	state, store := m.repoState(ctx, view, index)
	titles := map[int]string{}
	if state.open {
		for _, pr := range store.LoadAll(false) {
			titles[pr.Number] = pr.Title
		}
	}
	return repoRow(state, titles)
}

func (m *Module) handleList(ctx context.Context, req *rpc.Request) (any, error) {
	params := phonefamily.ParamsOf(req)
	ownerParam := qtTrimmed(params.String("owner"))
	nameParam := qtTrimmed(params.String("name"))
	view := m.currentView(ctx)
	repos := []any{}
	for i, record := range view.Repos {
		if len(repos) >= maxRepos {
			break
		}
		if ownerParam != "" || nameParam != "" {
			if !strings.EqualFold(record.Owner, ownerParam) || !strings.EqualFold(record.Name, nameParam) {
				continue
			}
		} else if !record.Enabled && len(record.Rows) == 0 {
			continue
		}
		repos = append(repos, m.row(ctx, view, i))
	}
	return map[string]any{"repos": repos}, nil
}

type request struct {
	owner  string
	name   string
	action string
	number int
	delta  int
}

func readRequest(req *rpc.Request) request {
	params := phonefamily.ParamsOf(req)
	return request{
		owner:  qtTrimmed(params.String("owner")),
		name:   qtTrimmed(params.String("name")),
		action: params.String("action"),
		number: params.Int("number", 0),
		delta:  params.Int("delta", 0),
	}
}

func holder(req *rpc.Request) string {
	return phonefamily.DeviceOf(req).Key
}

func (m *Module) handlePrepare(ctx context.Context, req *rpc.Request) (any, error) {
	r := readRequest(req)
	if !validRequest(r.owner, r.name, r.action, r.number, r.delta) {
		return nil, rpc.Fail("bad_request")
	}
	view := m.currentView(ctx)
	if view.indexFor(r.owner, r.name) < 0 {
		return nil, rpc.Fail("unknown_repo")
	}
	target := confirmationTarget(r.owner, r.name, r.action, r.number, r.delta)
	token := m.confirmations.Issue(holder(req), r.action, target, m.nowMs())
	return map[string]any{
		"token":       token,
		"action":      r.action,
		"summary":     actionSummary(r.owner, r.name, r.action, r.number, r.delta),
		"expiresInMs": phonefamily.ConfirmationLifetimeMs,
	}, nil
}

func (m *Module) handleRun(ctx context.Context, req *rpc.Request) (any, error) {
	r := readRequest(req)
	if !validRequest(r.owner, r.name, r.action, r.number, r.delta) {
		return nil, rpc.Fail("bad_request")
	}
	view := m.currentView(ctx)
	if view.indexFor(r.owner, r.name) < 0 {
		return nil, rpc.Fail("unknown_repo")
	}
	target := confirmationTarget(r.owner, r.name, r.action, r.number, r.delta)
	token := phonefamily.ParamsOf(req).String("token")
	if verdict := m.confirmations.Consume(token, holder(req), r.action, target, m.nowMs()); verdict != phonefamily.Ok {
		m.logUI("Refused a merge queue %s on %s/%s from %s: %s.", r.action, r.owner, r.name, phonefamily.DeviceLabel(req), phonefamily.ConfirmationError(verdict))
		return nil, rpc.Fail(phonefamily.ConfirmationError(verdict))
	}
	var record Record
	for attempt := 0; ; attempt++ {
		index := view.indexFor(r.owner, r.name)
		if index < 0 {
			return nil, rpc.Fail("unknown_repo")
		}
		record = view.Repos[index]
		err := m.runAction(ctx, view, index, r)
		if err == nil {
			break
		}
		if rpc.Code(err) != "conflict" || attempt+1 >= applyRetries {
			if rpc.Code(err) == "conflict" {
				return nil, rpc.Fail("busy")
			}
			return nil, err
		}
		view = m.currentView(ctx)
	}
	after := m.currentView(ctx)
	index := after.indexFor(record.Owner, record.Name)
	queue := Queue{}
	if index >= 0 {
		queue = FromRows(after.Repos[index].Rows)
	}
	suffix := ""
	if r.number > 0 {
		suffix = " #" + strconv.Itoa(r.number)
	}
	m.logUI("Merge queue %s on %s/%s%s from %s.", r.action, record.Owner, record.Name, suffix, phonefamily.DeviceLabel(req))
	row := map[string]any{}
	if index >= 0 {
		row = m.row(ctx, after, index)
	}
	if req.Peer != nil {
		peer := req.Peer
		go func() {
			time.Sleep(20 * time.Millisecond)
			peer.Send("mergequeue.changed", map[string]any{"owner": record.Owner, "name": record.Name})
		}()
	}
	return map[string]any{"message": resultMessage(r.action, r.number, queue), "queue": row}, nil
}

func (m *Module) executorBusy(view *View) bool {
	m.mu.Lock()
	defer m.mu.Unlock()
	return m.running || view.Busy
}

func (m *Module) runAction(ctx context.Context, view *View, index int, r request) error {
	state, store := m.repoState(ctx, view, index)
	record := view.Repos[index]
	pullStatus := ""
	if state.open && r.action == "enqueue" {
		for _, pr := range store.LoadAll(false) {
			if pr.Number == r.number {
				pullStatus = pr.Status
			}
		}
	}
	code := precheck(state, r.action, r.number, pullStatus)
	if code == "" && r.action == "process" && m.executorBusy(view) {
		code = "busy"
	}
	if code != "" {
		return rpc.Fail(code)
	}
	switch r.action {
	case "enable", "disable":
		return m.apply(ctx, map[string]any{"owner": record.Owner, "name": record.Name, "enabled": r.action == "enable"})
	case "pause", "resume":
		return m.apply(ctx, map[string]any{"owner": record.Owner, "name": record.Name, "paused": r.action == "pause"})
	case "process":
		m.Schedule(0)
		return nil
	}
	queue := FromRows(record.Rows)
	if code := applyToQueue(&queue, r.action, r.number, r.delta, m.nowMs()); code != "" {
		return rpc.Fail(code)
	}
	if err := m.apply(ctx, map[string]any{"owner": record.Owner, "name": record.Name, "rows": queue.Rows(), "expect": rowsOrEmpty(record.Rows)}); err != nil {
		return err
	}
	if r.action == "enqueue" && state.open {
		m.Schedule(nextDelay)
	}
	return nil
}

func rowsOrEmpty(rows []string) []string {
	if rows == nil {
		return []string{}
	}
	return rows
}

func (m *Module) apply(ctx context.Context, params map[string]any) error {
	callCtx, cancel := context.WithTimeout(ctx, applyTimeout)
	defer cancel()
	_, err := m.callUI(callCtx, "ui.mergequeue.apply", params)
	if err != nil && phonefamily.DesktopUnavailable(err) {
		return rpc.Fail("desktop_unavailable")
	}
	return err
}

func (m *Module) handleView(ctx context.Context, req *rpc.Request) (any, error) {
	view := &View{Open: -1}
	if err := req.Bind(view); err != nil {
		return nil, err
	}
	m.setView(view)
	return map[string]any{"ok": true}, nil
}

func (m *Module) handleSchedule(ctx context.Context, req *rpc.Request) (any, error) {
	var params struct {
		DelayMs int64 `json:"delayMs"`
	}
	if err := req.Bind(&params); err != nil {
		return nil, err
	}
	m.Schedule(time.Duration(params.DelayMs) * time.Millisecond)
	return map[string]any{"ok": true}, nil
}

func (m *Module) handleState(ctx context.Context, req *rpc.Request) (any, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	pending := int64(-1)
	if m.timer != nil && !m.deadline.IsZero() {
		if left := time.Until(m.deadline); left >= 0 {
			pending = left.Milliseconds()
		}
	}
	return map[string]any{"busy": m.running, "configured": m.configured, "pendingMs": pending}, nil
}

func (m *Module) Schedule(delay time.Duration) {
	if delay < 0 {
		delay = 0
	}
	m.mu.Lock()
	defer m.mu.Unlock()
	if m.timer != nil && !m.deadline.IsZero() {
		if left := time.Until(m.deadline); left >= 0 && left <= delay {
			return
		}
		m.timer.Stop()
	}
	m.deadline = time.Now().Add(delay)
	m.timer = time.AfterFunc(delay, func() {
		m.mu.Lock()
		m.deadline = time.Time{}
		m.mu.Unlock()
		m.process(m.baseCtx())
	})
}

func (m *Module) mutateQueue(ctx context.Context, ownerName, repoName string, change func(*Queue) bool) {
	for attempt := 0; attempt < applyRetries; attempt++ {
		view, ok := m.fetchView(ctx)
		if !ok {
			return
		}
		index := view.indexFor(ownerName, repoName)
		if index < 0 {
			return
		}
		rows := view.Repos[index].Rows
		queue := FromRows(rows)
		if !change(&queue) {
			return
		}
		err := m.apply(ctx, map[string]any{"owner": ownerName, "name": repoName, "rows": queue.Rows(), "expect": rowsOrEmpty(rows)})
		if rpc.Code(err) != "conflict" {
			return
		}
	}
}

func (m *Module) setEntryState(ctx context.Context, record Record, number int, state, detail string) {
	m.mutateQueue(ctx, record.Owner, record.Name, func(q *Queue) bool {
		return q.SetState(number, state, detail, m.nowMs())
	})
}

func (m *Module) dropEntry(ctx context.Context, record Record, number int) {
	m.mutateQueue(ctx, record.Owner, record.Name, func(q *Queue) bool { return q.Remove(number) })
}

func baseName(pr *pulls.Pull) string {
	if pr.Base == "" {
		return "the base branch"
	}
	return pr.Base
}

func plural(n int) string {
	if n == 1 {
		return ""
	}
	return "s"
}

func (m *Module) finishPass() {
	m.mu.Lock()
	m.running = false
	done := m.passDone
	m.passDone = nil
	m.mu.Unlock()
	if done != nil {
		close(done)
	}
}

func (m *Module) broadcast(event string, data map[string]any) {
	if m.core != nil && m.core.Hub != nil {
		m.core.BroadcastUI(event, data)
	}
}

func (m *Module) process(ctx context.Context) {
	m.mu.Lock()
	if m.running {
		m.mu.Unlock()
		m.Schedule(nextDelay)
		return
	}
	m.mu.Unlock()
	view, ok := m.fetchView(ctx)
	if !ok {
		return
	}
	if view.Busy {
		m.Schedule(nextDelay)
		return
	}
	openIndex := view.openIndex()
	if openIndex < 0 {
		return
	}
	repo := view.Repos[openIndex]
	if !repo.Enabled || repo.Paused {
		return
	}
	queue := FromRows(repo.Rows)
	if queue.Empty() {
		return
	}
	store := m.store(ctx, view, repo)
	if !store.CanWrite() {
		return
	}
	loaded := store.LoadAll(false)
	if len(loaded) == 0 {
		m.Schedule(retryDelay)
		return
	}
	if view.AiFix || view.Deleting || store.ConflictMergeInProgress() {
		m.Schedule(retryDelay)
		return
	}
	m.mu.Lock()
	if m.running {
		m.mu.Unlock()
		m.Schedule(nextDelay)
		return
	}
	m.running = true
	m.passDone = make(chan struct{})
	m.mu.Unlock()
	pending := false
	var merged *pulls.Pull
	byNumber := map[int]*pulls.Pull{}
	for _, pr := range loaded {
		byNumber[pr.Number] = pr
	}
	for _, entry := range queue.Entries() {
		number := entry.Number
		live, ok := m.fetchView(ctx)
		if !ok {
			break
		}
		liveIndex := live.indexFor(repo.Owner, repo.Name)
		if liveIndex < 0 || !live.Repos[liveIndex].Enabled || live.Repos[liveIndex].Paused {
			break
		}
		if live.openIndex() != liveIndex {
			break
		}
		if !FromRows(live.Repos[liveIndex].Rows).Contains(number) {
			continue
		}
		pr, found := byNumber[number]
		if !found {
			m.dropEntry(ctx, repo, number)
			m.logUI("Merge queue: pull request #%d no longer exists; dropped from the queue.", number)
			continue
		}
		if pr.Status != "open" {
			m.dropEntry(ctx, repo, number)
			m.logUI("Merge queue: pull request #%d left the queue (already %s).", number, pr.Status)
			continue
		}
		if repo.RequirePeerApproval && !pr.IndependentReviewGateSatisfied() {
			detail := "Waiting for an independent peer approval"
			if pr.HasIndependentChangesRequested() {
				detail = "Changes requested — resolve the review"
			}
			m.setEntryState(ctx, repo, number, StateBlocked, detail)
			pending = true
			continue
		}
		if behind, count, err := store.IsBranchBehindBase(number); err == nil && behind {
			m.setEntryState(ctx, repo, number, StateUpdating,
				"Updating from "+baseName(pr)+" ("+strconv.Itoa(count)+" commit"+plural(count)+" behind)")
			if err := store.UpdateBranchFromBase(number); err != nil {
				reason := strings.TrimSpace(err.Error())
				m.setEntryState(ctx, repo, number, StateFailed, "Could not update from "+baseName(pr)+": "+reason)
				m.logUI("Merge queue: pull request #%d could not be updated from its base: %s", number, leftUTF16(reason, 200))
				pending = true
				continue
			}
			m.logUI("Merge queue: updated pull request #%d from its base branch.", number)
		}
		clean, conflicts, err := store.CheckMergeable(number)
		if err != nil {
			detail := strings.TrimSpace(err.Error())
			if detail == "" {
				detail = "Could not test whether it merges"
			}
			m.setEntryState(ctx, repo, number, StateFailed, detail)
			pending = true
			continue
		}
		if !clean {
			detail := "Conflicts with the base branch"
			if len(conflicts) > 0 {
				detail = "Conflicts in " + strings.Join(conflicts, ", ")
			}
			m.setEntryState(ctx, repo, number, StateBlocked, detail)
			pending = true
			continue
		}
		m.setEntryState(ctx, repo, number, StateMerging, "Merging")
		if err := store.MergePull(number, repo.RequirePeerApproval); err != nil {
			reason := strings.TrimSpace(err.Error())
			m.setEntryState(ctx, repo, number, StateFailed, "Merge failed: "+reason)
			m.logUI("Merge queue: merging pull request #%d failed: %s", number, leftUTF16(reason, 200))
			pending = true
			continue
		}
		m.dropEntry(ctx, repo, number)
		merged = pr
		break
	}
	if merged != nil {
		m.broadcast("core.mergequeue.merged", map[string]any{
			"owner": repo.Owner, "name": repo.Name, "number": merged.Number, "pull": merged.JSON(),
		})
	} else {
		m.broadcast("core.mergequeue.state", map[string]any{"owner": repo.Owner, "name": repo.Name, "pending": pending})
	}
	m.finishPass()
	if merged != nil {
		m.Schedule(nextDelay)
	} else if pending {
		m.Schedule(retryDelay)
	}
}
