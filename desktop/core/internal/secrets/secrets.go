package secrets

import (
	"context"
	"sort"
	"time"

	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/phonefamily"
	"forkmesh.com/desktop-core/internal/rpc"
	"forkmesh.com/desktop-core/internal/vault"
)

const (
	maxNameLength    = 128
	maxValueBytes    = 16 * 1024
	maxRows          = 500
	variablesSetting = "actions/variables"
)

type Module struct {
	Owners    func() []string
	Now       func() time.Time
	Variables func() map[string]string
	Apply     func(ctx context.Context, params map[string]any) error

	core          *daemon.Core
	gate          *phonefamily.Gate
	confirmations *phonefamily.Confirmations
}

func New() *Module { return &Module{confirmations: phonefamily.NewConfirmations()} }

func (m *Module) Name() string { return "secrets" }

func (m *Module) Start(ctx context.Context, core *daemon.Core) error {
	m.core = core
	m.gate = &phonefamily.Gate{Core: core, Owners: m.Owners}
	if m.confirmations == nil {
		m.confirmations = phonefamily.NewConfirmations()
	}
	register := func(method string, handler rpc.Handler) {
		core.Registry.Handle(method, m.Name(), m.gate.Wrap(handler))
	}
	register("secrets.list", m.handleList)
	register("secrets.prepare", m.handlePrepare)
	register("secrets.set", m.handleWrite)
	register("secrets.delete", m.handleWrite)
	return nil
}

func (m *Module) nowMs() int64 {
	if m.Now != nil {
		return m.Now().UnixMilli()
	}
	return time.Now().UnixMilli()
}

func (m *Module) variables() map[string]string {
	if m.Variables != nil {
		return m.Variables()
	}
	return vault.Variables(m.core)
}

func (m *Module) apply(ctx context.Context, params map[string]any) error {
	if m.Apply != nil {
		return m.Apply(ctx, params)
	}
	if m.core == nil {
		return rpc.Fail("desktop_unavailable")
	}
	_, err := m.core.CallUI(ctx, "ui.secrets.apply", params)
	return err
}

func validName(name string) bool {
	units := utf16Length(name)
	if units == 0 || units > maxNameLength {
		return false
	}
	for i := 0; i < len(name); i++ {
		c := name[i]
		letter := (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'
		digit := c >= '0' && c <= '9'
		if !letter && !(digit && i > 0) {
			return false
		}
	}
	return true
}

func utf16Length(text string) int {
	units := 0
	for _, r := range text {
		if r > 0xFFFF {
			units += 2
		} else {
			units++
		}
	}
	return units
}

func validValue(value string) bool { return len(value) <= maxValueBytes }

func mutating(action string) bool { return action == "set" || action == "delete" }

func actionSummary(action, name string) string {
	switch action {
	case "set":
		return "Set the Actions variable " + name + " on this desktop."
	case "delete":
		return "Delete the Actions variable " + name + " from this desktop."
	}
	return ""
}

func listRows(variables map[string]string) []any {
	names := make([]string, 0, len(variables))
	for name := range variables {
		names = append(names, name)
	}
	sort.Strings(names)
	rows := []any{}
	for _, name := range names {
		if len(rows) >= maxRows {
			break
		}
		if !validName(name) {
			continue
		}
		rows = append(rows, map[string]any{"name": name})
	}
	return rows
}

func (m *Module) handleList(ctx context.Context, req *rpc.Request) (any, error) {
	return map[string]any{"variables": listRows(m.variables())}, nil
}

func (m *Module) handlePrepare(ctx context.Context, req *rpc.Request) (any, error) {
	params := phonefamily.ParamsOf(req)
	action := params.String("action")
	name := params.String("name")
	if !mutating(action) {
		return nil, rpc.Fail("bad_request")
	}
	if !validName(name) {
		return nil, rpc.Fail("invalid_name")
	}
	if action == "delete" {
		if _, ok := m.variables()[name]; !ok {
			return nil, rpc.Fail("not_found")
		}
	}
	token := m.confirmations.Issue(phonefamily.Holder(req), action, name, m.nowMs())
	return map[string]any{
		"token":       token,
		"action":      action,
		"summary":     actionSummary(action, name),
		"expiresInMs": phonefamily.ConfirmationLifetimeMs,
	}, nil
}

func (m *Module) handleWrite(ctx context.Context, req *rpc.Request) (any, error) {
	params := phonefamily.ParamsOf(req)
	setting := req.Method == "secrets.set"
	name := params.String("name")
	value := params.String("value")
	if !validName(name) {
		return nil, rpc.Fail("invalid_name")
	}
	if setting && !validValue(value) {
		return nil, rpc.Fail("value_too_large")
	}
	action := "delete"
	if setting {
		action = "set"
	}
	if verdict := m.confirmations.Consume(params.String("token"), phonefamily.Holder(req), action, name, m.nowMs()); verdict != phonefamily.Ok {
		return nil, rpc.Fail(phonefamily.ConfirmationError(verdict))
	}
	apply := map[string]any{"action": action, "name": name}
	if setting {
		apply["value"] = value
	}
	if err := m.apply(ctx, apply); err != nil {
		if phonefamily.DesktopUnavailable(err) {
			return nil, rpc.Fail("desktop_unavailable")
		}
		return nil, err
	}
	verb := "Deleted"
	if setting {
		verb = "Set"
	}
	m.core.LogUI("%s the Actions variable %s, requested by %s.", verb, name, phonefamily.DeviceLabel(req))
	go func() {
		time.Sleep(20 * time.Millisecond)
		phonefamily.BroadcastPhones(m.core, "secrets.changed", nil)
	}()
	return map[string]any{"ok": true}, nil
}
