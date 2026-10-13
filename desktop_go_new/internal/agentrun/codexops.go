package agentrun

import (
	"context"

	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/rpc"
)

func (m *Module) registerCodex(core *daemon.Core) {
	core.Registry.Handle("core.agentrun.codex", m.Name(), local(m.handleCodex))
}

type codexOp struct {
	Key    string `json:"key"`
	Op     string `json:"op"`
	Text   string `json:"text"`
	Token  string `json:"token"`
	Answer string `json:"answer"`
	Model  string `json:"model"`
	Mode   string `json:"mode"`
	Effort string `json:"effort"`
}

func (m *Module) handleCodex(ctx context.Context, req *rpc.Request) (any, error) {
	var params codexOp
	if err := req.Bind(&params); err != nil {
		return nil, err
	}
	r := m.lookup(params.Key)
	if r == nil || r.codex == nil {
		return nil, rpc.Fail("not_parsed")
	}
	return r.codexOp(params)
}

func (r *run) codexOp(params codexOp) (any, error) {
	r.mu.Lock()
	defer r.mu.Unlock()
	c := r.codex
	ok := true
	switch params.Op {
	case "send":
		ok = c.sendUserText(params.Text)
	case "respond":
		ok = c.respond(params.Token, params.Answer)
	case "options":
		c.setTurnOptions(params.Model, params.Mode, params.Effort)
	case "mode":
		c.mode = params.Mode
	case "interrupt":
		c.interrupt()
	default:
		return nil, rpc.Fail("bad_op")
	}
	c.persist()
	state := r.stateLocked()
	state["ok"] = ok
	return state, nil
}
