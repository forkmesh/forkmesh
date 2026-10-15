package hosts

import (
	"context"
	"strings"
	"sync"
	"time"

	"forkmesh.com/desktop-core/internal/rpc"
)

const (
	minExecTimeout = time.Second
	maxExecTimeout = 30 * time.Minute
	probeRefreshMs = 5 * 60 * 1000
)

func (m *Module) handleState(ctx context.Context, req *rpc.Request) (any, error) {
	hc := m.desktopContext(ctx, false)
	payload := m.hostsPayload(hc)
	m.mu.Lock()
	probes := make(map[string]probeOutcome, len(m.probes))
	for key, outcome := range m.probes {
		probes[key] = outcome
	}
	streams := len(m.streams)
	m.mu.Unlock()
	payload["probes"] = probes
	payload["operations"] = m.operationList()
	payload["streams"] = streams
	return payload, nil
}

func (m *Module) rememberPassword(key, password string) {
	if password == "" {
		return
	}
	m.mu.Lock()
	m.passwords[key] = password
	m.mu.Unlock()
}

func (m *Module) handleProbe(ctx context.Context, req *rpc.Request) (any, error) {
	var params struct {
		Name     string  `json:"name"`
		IP       string  `json:"ip"`
		User     string  `json:"user"`
		Status   string  `json:"status"`
		Password *string `json:"password"`
		Identity string  `json:"identityFile"`
	}
	if err := req.Bind(&params); err != nil {
		return nil, err
	}
	name, ip, user := strings.TrimSpace(params.Name), strings.TrimSpace(params.IP), strings.TrimSpace(params.User)
	if name == "" || ip == "" || user == "" {
		return nil, rpc.Fail("bad_params")
	}
	key := credentialKey(name, ip, user)
	m.savedHosts()
	password := ""
	if params.Password != nil {
		password = *params.Password
		m.rememberPassword(key, password)
	} else {
		password = m.password(key)
	}
	return m.probeAndStore(ctx, name, ip, user, strings.TrimSpace(params.Status), password, params.Identity), nil
}

func (m *Module) probeAndStore(ctx context.Context, name, ip, user, status, password, identity string) probeOutcome {
	done := m.track()
	defer done()
	if strings.TrimSpace(identity) == "" {
		identity = savedHostIdentityFile(m.savedHosts(), m.sshDir(), name, ip, user)
	}
	outcome := m.probeHost(ctx, name, ip, user, status, password, identity)
	m.mu.Lock()
	m.probes[outcome.Key] = outcome
	m.mu.Unlock()
	m.broadcastUI("core.hosts.probed", outcome)
	return outcome
}

func (m *Module) handleProbeAll(ctx context.Context, req *rpc.Request) (any, error) {
	return map[string]any{"started": m.probeAll()}, nil
}

func (m *Module) probeAll() int {
	m.mu.Lock()
	if m.probingAll {
		m.mu.Unlock()
		return 0
	}
	m.probingAll = true
	m.lastProbeAll = m.nowMs()
	m.mu.Unlock()
	type target struct{ name, ip, user, status, password string }
	targets := []target{}
	for _, host := range m.savedHosts() {
		name := strings.TrimSpace(jsonString(host["name"]))
		ip := strings.TrimSpace(jsonString(host["ip"]))
		user := strings.TrimSpace(jsonString(host["user"]))
		if name == "" || ip == "" || user == "" {
			continue
		}
		targets = append(targets, target{name, ip, user, strings.TrimSpace(jsonString(host["status"])), m.password(credentialKey(name, ip, user))})
	}
	if len(targets) == 0 {
		m.mu.Lock()
		m.probingAll = false
		m.mu.Unlock()
		return 0
	}
	done := m.track()
	var group sync.WaitGroup
	for _, t := range targets {
		group.Add(1)
		go func() {
			defer group.Done()
			m.probeAndStore(m.ctx, t.name, t.ip, t.user, t.status, t.password, "")
		}()
	}
	go func() {
		group.Wait()
		m.mu.Lock()
		m.probingAll = false
		m.mu.Unlock()
		done()
		m.broadcastPhones("hosts.changed", nil)
		m.broadcastUI("core.hosts.changed", map[string]any{})
	}()
	return len(targets)
}

func (m *Module) refreshProbesWithoutDesktop() {
	if m.desktopAttached() {
		return
	}
	m.mu.Lock()
	stale := m.nowMs()-m.lastProbeAll >= probeRefreshMs
	m.mu.Unlock()
	if stale {
		m.probeAll()
	}
}

func (m *Module) handleExec(ctx context.Context, req *rpc.Request) (any, error) {
	var params struct {
		Node      string `json:"node"`
		Command   string `json:"command"`
		TimeoutMs int64  `json:"timeoutMs"`
		IP        string `json:"ip"`
		Identity  string `json:"identityFile"`
		Kind      string `json:"kind"`
	}
	if err := req.Bind(&params); err != nil {
		return nil, err
	}
	if strings.TrimSpace(params.Node) == "" && strings.TrimSpace(params.IP) == "" {
		return nil, rpc.Fail("bad_params")
	}
	timeout := time.Duration(params.TimeoutMs) * time.Millisecond
	if timeout < minExecTimeout {
		timeout = minExecTimeout
	}
	if timeout > maxExecTimeout {
		timeout = maxExecTimeout
	}
	kind := params.Kind
	if kind == "" {
		kind = "exec"
	}
	op := m.beginOperation(kind, "", params.Node, "desktop")
	ok, output := m.runMirrorNodeSSH(m.ctx, params.Node, params.Command, timeout, params.IP, params.Identity)
	m.endOperation(op, ok, output)
	return map[string]any{"ok": ok, "output": output, "operation": op.ID}, nil
}

func (m *Module) handleOperations(ctx context.Context, req *rpc.Request) (any, error) {
	m.mu.Lock()
	running := make([]string, 0, len(m.running))
	for key := range m.running {
		running = append(running, key)
	}
	m.mu.Unlock()
	return map[string]any{"operations": m.operationList(), "running": running}, nil
}
