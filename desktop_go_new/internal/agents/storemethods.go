package agents

import (
	"context"
	"encoding/json"
	"errors"
	"strconv"
	"strings"

	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/qtjson"
	"forkmesh.com/desktop-core/internal/rpc"
)

func (m *Module) registerWriter(core *daemon.Core) {
	handlers := map[string]rpc.Handler{
		"core.agentstore.root":         m.handleRoot,
		"core.agentstore.create":       m.handleCreate,
		"core.agentstore.save":         m.handleSave,
		"core.agentstore.delete":       m.handleDelete,
		"core.agentstore.recycle":      m.handleRecycle,
		"core.agentstore.restore":      m.handleRestore,
		"core.agentstore.purge":        m.handlePurge,
		"core.agentstore.purgeExpired": m.handlePurgeExpired,
		"core.agentstore.appendLog":    m.handleAppendLog,
		"core.agentstore.appendEvent":  m.handleAppendEvent,
		"core.agentstore.clearEvents":  m.handleClearEvents,
		"core.agentstore.writePatch":   m.handleWritePatch,
		"core.agentstore.sync":         m.handleSync,
		"core.agentstore.outcomes":     m.handleOutcomes,
	}
	for method, handler := range handlers {
		core.Registry.Handle(method, m.Name(), m.local(handler))
	}
}

func (m *Module) handleRoot(ctx context.Context, req *rpc.Request) (any, error) {
	return map[string]any{"root": m.Store().Root, "writer": true}, nil
}

type recordParams struct {
	Session json.RawMessage `json:"session"`
	ID      int             `json:"id"`
}

func bindRecord(req *rpc.Request) (qtjson.Value, error) {
	var params recordParams
	if err := req.Bind(&params); err != nil {
		return qtjson.Value{}, err
	}
	record, err := qtjson.ParseKeepNumbers(params.Session)
	if err != nil || !record.IsObject() {
		return qtjson.Value{}, rpc.Fail("bad_session")
	}
	return record, nil
}

func failure(err error) error {
	switch {
	case err == nil:
		return nil
	case errors.Is(err, ErrUnsupportedSchema):
		return rpc.Fail("unsupported_schema")
	case errors.Is(err, ErrRetired):
		return rpc.Fail("retired")
	case errors.Is(err, ErrBadSession):
		return rpc.Fail("bad_session")
	}
	return rpc.Fail("write_failed")
}

func (m *Module) handleCreate(ctx context.Context, req *rpc.Request) (any, error) {
	record, err := bindRecord(req)
	if err != nil {
		return nil, err
	}
	created, err := m.Writer().CreateRecord(record)
	if err != nil {
		return nil, failure(err)
	}
	return map[string]any{"session": json.RawMessage(created.Compact()), "id": created.Get("id").Int(),
		"createdAtMs": created.Get("createdAtMs").Int64()}, nil
}

func (m *Module) handleSave(ctx context.Context, req *rpc.Request) (any, error) {
	record, err := bindRecord(req)
	if err != nil {
		return nil, err
	}
	saved, err := m.Writer().SaveRecord(record)
	if err != nil && !errors.Is(err, ErrRetired) && !errors.Is(err, ErrUnsupportedSchema) {
		return nil, failure(err)
	}
	return map[string]any{"saved": saved}, nil
}

func (m *Module) handleDelete(ctx context.Context, req *rpc.Request) (any, error) {
	record, err := bindRecord(req)
	if err != nil {
		return nil, err
	}
	ok, err := m.Writer().DeleteRecord(record)
	if err != nil && errors.Is(err, ErrBadSession) {
		return nil, failure(err)
	}
	return map[string]any{"ok": ok}, nil
}

func (m *Module) handleRecycle(ctx context.Context, req *rpc.Request) (any, error) {
	record, err := bindRecord(req)
	if err != nil {
		return nil, err
	}
	ok, err := m.Writer().RecycleRecord(record)
	if err != nil && errors.Is(err, ErrBadSession) {
		return nil, failure(err)
	}
	return map[string]any{"ok": ok}, nil
}

func (m *Module) handleRestore(ctx context.Context, req *rpc.Request) (any, error) {
	var params recordParams
	if err := req.Bind(&params); err != nil {
		return nil, err
	}
	ok, path, _ := m.Writer().RestoreRecycledPath(params.ID)
	return map[string]any{"ok": ok, "path": path}, nil
}

func (m *Module) handlePurge(ctx context.Context, req *rpc.Request) (any, error) {
	var params recordParams
	if err := req.Bind(&params); err != nil {
		return nil, err
	}
	ok, _ := m.Writer().PurgeRecycled(params.ID)
	return map[string]any{"ok": ok}, nil
}

func (m *Module) handlePurgeExpired(ctx context.Context, req *rpc.Request) (any, error) {
	var params struct {
		RetentionMs int64 `json:"retentionMs"`
		NowMs       int64 `json:"nowMs"`
	}
	if err := req.Bind(&params); err != nil {
		return nil, err
	}
	return map[string]any{"purged": m.Writer().PurgeExpired(params.RetentionMs, params.NowMs)}, nil
}

type appendParams struct {
	Path  string          `json:"path"`
	Key   string          `json:"key"`
	ID    int             `json:"id"`
	Text  string          `json:"text"`
	Event json.RawMessage `json:"event"`
	Patch string          `json:"patch"`
}

func (m *Module) resolveDir(params appendParams, file string) (string, int, bool) {
	key, id := params.Key, params.ID
	if params.Path != "" {
		parts := strings.Split(params.Path, "/")
		if len(parts) != 4 || parts[0] != "sessions" || parts[3] != file {
			return "", 0, false
		}
		key = parts[1]
		n, err := strconv.Atoi(parts[2])
		if err != nil || strconv.Itoa(n) != parts[2] {
			return "", 0, false
		}
		id = n
	}
	dir, ok := m.Writer().DirFor(key, id)
	return dir, id, ok
}

func (m *Module) handleAppendLog(ctx context.Context, req *rpc.Request) (any, error) {
	var params appendParams
	if err := req.Bind(&params); err != nil {
		return nil, err
	}
	dir, id, ok := m.resolveDir(params, "transcript.txt")
	if !ok {
		return nil, rpc.Fail("bad_session")
	}
	m.Writer().AppendLog(dir, id, params.Text)
	return map[string]any{}, nil
}

func (m *Module) handleAppendEvent(ctx context.Context, req *rpc.Request) (any, error) {
	var params appendParams
	if err := req.Bind(&params); err != nil {
		return nil, err
	}
	dir, id, ok := m.resolveDir(params, "events.jsonl")
	if !ok || len(params.Event) == 0 || params.Event[0] != '{' {
		return nil, rpc.Fail("bad_session")
	}
	m.Writer().AppendEventLine(dir, id, params.Event)
	return map[string]any{}, nil
}

func (m *Module) handleClearEvents(ctx context.Context, req *rpc.Request) (any, error) {
	var params appendParams
	if err := req.Bind(&params); err != nil {
		return nil, err
	}
	dir, _, ok := m.resolveDir(params, "events.jsonl")
	if !ok {
		return nil, rpc.Fail("bad_session")
	}
	m.Writer().ClearEvents(dir)
	return map[string]any{}, nil
}

func (m *Module) handleWritePatch(ctx context.Context, req *rpc.Request) (any, error) {
	var params appendParams
	if err := req.Bind(&params); err != nil {
		return nil, err
	}
	dir, _, ok := m.resolveDir(params, "changes.patch")
	if !ok {
		return nil, rpc.Fail("bad_session")
	}
	return map[string]any{"ok": m.Writer().WritePatch(dir, params.Patch)}, nil
}

func (m *Module) handleSync(ctx context.Context, req *rpc.Request) (any, error) {
	done := make(chan struct{})
	m.Writer().WhenDrained(func() { close(done) })
	select {
	case <-done:
		return map[string]any{"synced": true}, nil
	case <-ctx.Done():
		return nil, rpc.Fail("cancelled")
	}
}

func (m *Module) handleOutcomes(ctx context.Context, req *rpc.Request) (any, error) {
	outcomes := []ModelOutcome{}
	for _, outcome := range m.Store().RetiredModelOutcomes() {
		outcomes = append(outcomes, outcome)
	}
	sortOutcomes(outcomes)
	return map[string]any{"outcomes": outcomes}, nil
}
