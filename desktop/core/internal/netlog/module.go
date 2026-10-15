package netlog

import (
	"context"
	"encoding/json"
	"time"

	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/phonefamily"
	"forkmesh.com/desktop-core/internal/rpc"
)

type Module struct {
	Owners   func() []string
	Now      func() time.Time
	LivePath string

	core    *daemon.Core
	gate    *phonefamily.Gate
	tracker *Tracker
}

func New() *Module { return &Module{} }

func (m *Module) Name() string { return "netlog" }

func (m *Module) now() time.Time {
	if m.Now != nil {
		return m.Now()
	}
	return time.Now()
}

func (m *Module) Tracker() *Tracker { return m.tracker }

func (m *Module) Start(ctx context.Context, core *daemon.Core) error {
	m.core = core
	path := m.LivePath
	if path == "" {
		path = LivePath(core.Layout.Data)
	}
	m.tracker = NewTracker(path)
	m.tracker.Now = m.now
	m.gate = &phonefamily.Gate{Core: core, Owners: m.Owners}
	core.Registry.Handle("stats.timeline", m.Name(), m.gate.Wrap(m.handleTimeline))
	core.Registry.Handle("core.log.append", m.Name(), func(ctx context.Context, req *rpc.Request) (any, error) {
		if req.Peer == nil || rpc.Remote(req.Peer) {
			return nil, rpc.Fail("forbidden")
		}
		return m.handleAppend(ctx, req)
	})
	go m.tracker.Snapshot()
	return nil
}

func (m *Module) handleTimeline(ctx context.Context, req *rpc.Request) (any, error) {
	var params map[string]json.RawMessage
	if len(req.Params) > 0 {
		_ = json.Unmarshal(req.Params, &params)
	}
	days := ClampDays(params["days"])
	return Timeline(m.tracker.Snapshot(), m.now(), days), nil
}

func (m *Module) handleAppend(ctx context.Context, req *rpc.Request) (any, error) {
	var params struct {
		Lines []AppendLine `json:"lines"`
		Line  string       `json:"line"`
		Badge string       `json:"badge"`
	}
	if len(req.Params) > 0 {
		if err := json.Unmarshal(req.Params, &params); err != nil {
			return nil, rpc.Fail("bad_request")
		}
	}
	lines := params.Lines
	if params.Line != "" {
		lines = append(lines, AppendLine{Line: params.Line, Badge: params.Badge})
	}
	return map[string]any{"accepted": m.tracker.Append(lines)}, nil
}
