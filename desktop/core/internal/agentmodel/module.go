package agentmodel

import (
	"context"
	"encoding/json"
	"sync"
	"time"

	"forkmesh.com/desktop-core/internal/agentrun"
	"forkmesh.com/desktop-core/internal/agents"
	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/phonefamily"
	"forkmesh.com/desktop-core/internal/qtjson"
	"forkmesh.com/desktop-core/internal/rpc"
)

const agentsBellSpacing = 300 * time.Millisecond

type Module struct {
	core    *daemon.Core
	store   *agents.Module
	run     *agentrun.Module
	gate    *phonefamily.Gate
	now     func() time.Time
	backlog func() int

	CallUI   func(ctx context.Context, method string, params any) ([]byte, error)
	NotifyUI func(method string, params any) bool
	Attached func() bool

	mu         sync.Mutex
	ctx        desktopContext
	pushed     bool
	bellAt     time.Time
	bellArm    *time.Timer
	stopped    bool
	unwatch    func()
	follows    *followPump
	confirms   *confirmations
	background *backgroundTracker

	opsArmed   bool
	opsArmSent bool
	usage      qtjson.Value
	sessions   sessionCache
}

func New() *Module {
	return &Module{now: time.Now}
}

func (m *Module) Name() string { return "agentmodel" }

func (m *Module) Start(ctx context.Context, core *daemon.Core) error {
	m.core = core
	m.store, _ = core.Module("agents").(*agents.Module)
	m.run, _ = core.Module("agentrun").(*agentrun.Module)
	m.gate = &phonefamily.Gate{Core: core}
	m.follows = newFollowPump(m)
	m.confirms = newConfirmations()
	m.background = newBackgroundTracker(m)
	if m.backlog == nil {
		m.backlog = relayBacklog(core)
	}
	if writer := m.writer(); writer != nil {
		m.unwatch = writer.Observe(m.storeChanged)
	}
	core.Registry.Handle("core.agents.model.push", m.Name(), m.local(m.handlePush))
	core.Registry.Handle("core.agents.model.state", m.Name(), m.local(m.handleState))
	core.Registry.Handle("agents.list", m.Name(), m.handleList)
	core.Registry.Handle("agents.queue", m.Name(), m.handleQueue)
	m.registerFollow(core)
	m.registerOps(core)
	m.registerActions(core)
	m.registerBackground(core)
	m.wrapTranscript(core)
	return nil
}

func (m *Module) Stop() {
	m.mu.Lock()
	m.stopped = true
	if m.bellArm != nil {
		m.bellArm.Stop()
	}
	unwatch := m.unwatch
	m.mu.Unlock()
	if unwatch != nil {
		unwatch()
	}
	m.follows.stop()
	m.background.stopAll()
}

func (m *Module) local(handler rpc.Handler) rpc.Handler {
	return func(ctx context.Context, req *rpc.Request) (any, error) {
		if req.Peer == nil || rpc.Remote(req.Peer) {
			return nil, rpc.Fail("forbidden")
		}
		return handler(ctx, req)
	}
}

func (m *Module) writer() *agents.Writer {
	if m.store == nil {
		return nil
	}
	return m.store.Writer()
}

func (m *Module) salt() []byte {
	if m.run == nil {
		return nil
	}
	return m.run.Salt()
}

func (m *Module) credentials() ([]string, bool) {
	if m.run == nil || !m.run.HaveCredentials() {
		return nil, false
	}
	return m.run.CredentialValues(), true
}

func (m *Module) snapshot() (desktopContext, bool) {
	m.mu.Lock()
	defer m.mu.Unlock()
	return m.ctx, m.pushed
}

func (m *Module) desktopAttached() bool {
	if m.Attached != nil {
		return m.Attached()
	}
	return m.core != nil && m.core.UI() != nil
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

func (m *Module) notifyUI(method string, params any) bool {
	if m.NotifyUI != nil {
		return m.NotifyUI(method, params)
	}
	return m.core != nil && m.core.NotifyUI(method, params)
}

func (m *Module) logUI(format string, args ...any) {
	if m.core != nil {
		m.core.LogUI(format, args...)
	}
}

func paramsObject(req *rpc.Request) qtjson.Value {
	if params, ok := qtjson.ParseObject(req.Params); ok {
		return params
	}
	return qtjson.NewObject()
}

func raw(value qtjson.Value) json.RawMessage {
	return json.RawMessage(value.Compact())
}

func (m *Module) handlePush(ctx context.Context, req *rpc.Request) (any, error) {
	next := parseContext(paramsObject(req))
	m.mu.Lock()
	if m.pushed && next.Seq > 0 && next.Seq < m.ctx.Seq {
		m.mu.Unlock()
		return map[string]any{"applied": false, "confirm": m.confirms.secret}, nil
	}
	m.ctx = next
	m.pushed = true
	m.mu.Unlock()
	m.follows.markAllDirty()
	m.ringAgents()
	return map[string]any{"applied": true, "confirm": m.confirms.secret}, nil
}

func (m *Module) handleState(ctx context.Context, req *rpc.Request) (any, error) {
	snapshot, pushed := m.snapshot()
	var sessions []agents.Session
	sessions, _, _ = m.sessionList()
	byID := indexSessions(sessions)
	order := queueOrder(&snapshot, byID)
	return map[string]any{"pushed": pushed, "seq": snapshot.Seq, "confirm": m.confirms.secret, "queue": order, "running": snapshot.Running,
		"limit": m.maxRunningAgents(&snapshot), "follows": m.follows.count(), "sessions": len(sessions)}, nil
}

func (m *Module) storeChanged(change agents.Change) {
	if change.Kind != agents.ChangeLog && change.Kind != agents.ChangeEvent {
		m.sessions.invalidate()
	}
	switch change.Kind {
	case agents.ChangeLog:
		m.follows.markDirty(change.ID)
	case agents.ChangeEvent:
	default:
		if !m.desktopAttached() {
			m.follows.markDirty(change.ID)
			m.ringAgents()
		}
	}
}

func (m *Module) ringAgents() {
	m.mu.Lock()
	defer m.mu.Unlock()
	if m.stopped || m.bellArm != nil {
		return
	}
	wait := m.bellAt.Add(agentsBellSpacing).Sub(m.now())
	wait = min(max(wait, 0), agentsBellSpacing)
	m.bellArm = time.AfterFunc(wait, func() {
		m.mu.Lock()
		m.bellArm = nil
		m.bellAt = m.now()
		stopped := m.stopped
		m.mu.Unlock()
		if !stopped {
			phonefamily.BroadcastPhones(m.core, "agents.changed", nil)
		}
	})
}

func (m *Module) loadSessions() ([]agents.Session, *agents.Store) {
	writer := m.writer()
	if writer == nil {
		return nil, nil
	}
	list, _, store := m.sessionList()
	return append([]agents.Session(nil), list...), store
}

func (m *Module) handleList(ctx context.Context, req *rpc.Request) (any, error) {
	snapshot, pushed := m.snapshot()
	credentials, ok := m.credentials()
	sessions, store := m.loadSessions()
	if !pushed || !ok || store == nil {
		return nil, rpc.ErrDelegate
	}
	return raw(m.agentsPayload(&snapshot, sessions, credentials)), nil
}

func (m *Module) handleQueue(ctx context.Context, req *rpc.Request) (any, error) {
	snapshot, pushed := m.snapshot()
	sessions, store := m.loadSessions()
	if !pushed || store == nil {
		return nil, rpc.ErrDelegate
	}
	m.detachedRunning(&snapshot, sessions)
	return raw(m.queuePayload(&snapshot, indexSessions(sessions))), nil
}
