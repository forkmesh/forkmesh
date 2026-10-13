package diag

import (
	"context"
	"encoding/json"
	"math"
	"path/filepath"
	"strings"
	"sync"
	"time"

	"forkmesh.com/desktop-core/internal/agentproc"
	"forkmesh.com/desktop-core/internal/agents"
	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/rpc"
	"forkmesh.com/desktop-core/internal/vault"
)

const owner = "diag"

type watcher struct {
	peer rpc.Peer
	stop chan struct{}
	opts watchOptions
}

type Module struct {
	Owners         func() []string
	Interval       time.Duration
	UIState        func(ctx context.Context, roots bool) (desktopState, error)
	DesktopPID     func() int
	Sessions       func() []agents.Session
	AgentProcesses func() []agentproc.Process
	RunPS          func(ctx context.Context) ([]byte, error)

	core     *daemon.Core
	ctx      context.Context
	cancel   context.CancelFunc
	readHost func() reading
	readProc func(pid int) processStats
	readCPU  func(pid int) (float64, bool)
	readDisk func(path string) (int64, int64)
	mac      *buildMac

	appMu     sync.Mutex
	appMeters map[int]*appMeter

	mu       sync.Mutex
	meter    cpuMeter
	samples  []sample
	latest   reading
	cpu      float64
	watchers map[string]*watcher
	stalls   []string
}

func New() *Module {
	return &Module{
		Interval: time.Second,
		watchers: map[string]*watcher{},
		mac:      &buildMac{probe: sshProbe, now: func() int64 { return time.Now().UnixMilli() }},
		readProc: readProcess,
	}
}

func (m *Module) Name() string { return owner }

func (m *Module) Start(ctx context.Context, core *daemon.Core) error {
	m.core = core
	m.ctx, m.cancel = context.WithCancel(ctx)
	if m.readHost == nil {
		data := core.Layout.Data
		m.readHost = func() reading { return readHost(data) }
	}
	m.mu.Lock()
	m.latest = m.readHost()
	m.cpu = m.meter.update(m.latest)
	m.mu.Unlock()
	core.Registry.Handle("diag.watch", owner, m.gate(m.handleWatch))
	core.Registry.Handle("diag.unwatch", owner, m.gate(m.handleUnwatch))
	core.Registry.Handle("diag.log", owner, m.gate(m.handleLog))
	core.Registry.Handle("diag.buildMac", owner, m.gate(m.handleBuildMac))
	core.Registry.Handle("diag.health", owner, m.gate(m.handleHealth))
	core.Registry.Handle("diag.stalls", owner, m.gate(m.handleStalls))
	core.Registry.Handle("diag.stall", owner, m.gate(m.handleStall))
	core.Registry.Handle("diag.processes", owner, m.gate(m.handleProcesses))
	core.Registry.Handle("core.vitals", owner, m.handleCoreVitals)
	core.Registry.Handle("core.diag.stall", owner, m.handleStallPush)
	core.Registry.Handle("core.diag.stalls", owner, m.handleStallSync)
	go m.sampleLoop()
	return nil
}

func (m *Module) Stop() {
	if m.cancel != nil {
		m.cancel()
	}
}

func (m *Module) sampleLoop() {
	interval := m.Interval
	if interval <= 0 {
		interval = time.Second
	}
	ticker := time.NewTicker(interval)
	defer ticker.Stop()
	for {
		select {
		case <-m.ctx.Done():
			return
		case <-ticker.C:
			m.sampleOnce()
		}
	}
}

func (m *Module) sampleOnce() {
	r := m.readHost()
	m.mu.Lock()
	cpu := m.meter.update(r)
	s := sampleOf(r, cpu)
	m.latest = r
	m.cpu = cpu
	m.samples = appendVitals(m.samples, s)
	type target struct {
		w    *watcher
		opts watchOptions
	}
	targets := make([]target, 0, len(m.watchers))
	for _, w := range m.watchers {
		targets = append(targets, target{w, w.opts})
	}
	m.mu.Unlock()
	now := time.Now()
	for _, t := range targets {
		event, data := "diag.vitals", any(s)
		if t.opts.detail {
			event, data = "core.vitals.sample", m.detailSample(r, s, t.opts, now)
		}
		if !t.w.peer.Send(event, data) {
			m.removeWatcher(t.w)
		}
	}
}

func (m *Module) ownerNames() []string {
	if m.Owners != nil {
		return m.Owners()
	}
	var names []string
	switch value := m.core.Get("control.owners").(type) {
	case []string:
		names = append(names, value...)
	case string:
		names = append(names, value)
	}
	if info, ok := m.core.Get("control.info").(map[string]any); ok {
		if user, ok := info["user"].(string); ok {
			names = append(names, user)
		}
	}
	if m.core.Settings != nil {
		names = append(names, m.core.Settings.Get().String("account/nodeName", ""))
	}
	return names
}

func ownerMayManage(deviceUser, account string) bool {
	device := strings.TrimSpace(deviceUser)
	account = strings.TrimSpace(account)
	return device != "" && account != "" && strings.EqualFold(device, account)
}

func (m *Module) isOwner(device rpc.Device) bool {
	if device.Key == "" {
		return false
	}
	for _, name := range m.ownerNames() {
		if ownerMayManage(device.User, name) {
			return true
		}
	}
	return false
}

func (m *Module) gate(handler rpc.Handler) rpc.Handler {
	return func(ctx context.Context, req *rpc.Request) (any, error) {
		if req.Peer != nil && rpc.Remote(req.Peer) && !m.isOwner(req.Peer.Device()) {
			return nil, rpc.Fail("not_owner")
		}
		return handler(ctx, req)
	}
}

func (m *Module) handleWatch(ctx context.Context, req *rpc.Request) (any, error) {
	m.mu.Lock()
	out := historyOf(m.samples)
	m.mu.Unlock()
	if req.Peer != nil {
		m.addWatcher(req.Peer, watchOptions{})
	}
	return out, nil
}

func (m *Module) handleUnwatch(ctx context.Context, req *rpc.Request) (any, error) {
	if req.Peer != nil {
		m.mu.Lock()
		w := m.watchers[req.Peer.ID()]
		m.mu.Unlock()
		if w != nil && w.peer == req.Peer {
			m.removeWatcher(w)
		}
	}
	return map[string]any{}, nil
}

func (m *Module) addWatcher(peer rpc.Peer, opts watchOptions) {
	m.mu.Lock()
	if current, ok := m.watchers[peer.ID()]; ok && current.peer == peer {
		current.opts = opts
		m.mu.Unlock()
		return
	}
	if current, ok := m.watchers[peer.ID()]; ok {
		close(current.stop)
	}
	w := &watcher{peer: peer, stop: make(chan struct{}), opts: opts}
	m.watchers[peer.ID()] = w
	m.mu.Unlock()
	go func() {
		select {
		case <-peer.Done():
			m.removeWatcher(w)
		case <-w.stop:
		case <-m.ctx.Done():
		}
	}()
}

func (m *Module) removeWatcher(w *watcher) {
	m.mu.Lock()
	defer m.mu.Unlock()
	if current, ok := m.watchers[w.peer.ID()]; ok && current == w {
		delete(m.watchers, w.peer.ID())
		close(w.stop)
	}
}

func (m *Module) watching() int {
	m.mu.Lock()
	defer m.mu.Unlock()
	return len(m.watchers)
}

func jsonInt(value any, fallback int) int {
	number, ok := value.(float64)
	if !ok || math.IsNaN(number) || math.IsInf(number, 0) || number != math.Trunc(number) ||
		number < math.MinInt32 || number > math.MaxInt32 {
		return fallback
	}
	return int(number)
}

func jsonString(value any) string {
	text, _ := value.(string)
	return text
}

func (m *Module) logPath() string {
	return filepath.Join(m.core.Layout.Data, "network_log.txt")
}

func (m *Module) handleLog(ctx context.Context, req *rpc.Request) (any, error) {
	params := req.Object()
	before := int64(-1)
	if value, ok := params["before"]; ok {
		if number, ok := value.(float64); ok {
			before = int64(math.Max(-1, math.Min(number, 1<<62)))
		}
	}
	return logPage(m.logPath(), before, jsonInt(params["lines"], 200), jsonString(params["query"]), jsonString(params["severity"])), nil
}

func (m *Module) handleBuildMac(ctx context.Context, req *rpc.Request) (any, error) {
	variables := func() string {
		raw, _ := json.Marshal(map[string]string{"MAC_BUILD_HOST": vault.Variable(m.core, "MAC_BUILD_HOST")})
		return string(raw)
	}
	return m.mac.status(ctx, m.core.Layout.SSH, variables)
}

func (m *Module) handleCoreVitals(ctx context.Context, req *rpc.Request) (any, error) {
	if req.Peer == nil || rpc.Remote(req.Peer) {
		return nil, rpc.Fail("forbidden")
	}
	raw := req.Object()
	pid := jsonInt(raw["pid"], 0)
	m.mu.Lock()
	hist := historyOf(m.samples)
	r := m.latest
	cpu := m.cpu
	latest := sampleOf(r, cpu)
	if len(m.samples) > 0 {
		latest = m.samples[len(m.samples)-1]
	}
	m.mu.Unlock()
	detail, _ := raw["detail"].(bool)
	opts := watchOptions{detail: detail, pid: pid, disks: detailDisks(raw["disks"])}
	if watch, ok := raw["watch"].(bool); ok {
		if watch {
			m.addWatcher(req.Peer, opts)
		} else {
			m.mu.Lock()
			w := m.watchers[req.Peer.ID()]
			m.mu.Unlock()
			if w != nil && w.peer == req.Peer {
				m.removeWatcher(w)
			}
		}
	}
	proc := unknownProcess()
	if pid > 0 {
		proc = m.readProc(pid)
	}
	out := map[string]any{
		"memory":      hist.Memory,
		"cpu":         hist.CPU,
		"disk":        hist.Disk,
		"swap":        hist.Swap,
		"latest":      latest,
		"system":      systemSummary(r, cpu, proc),
		"seconds":     vitalsSeconds,
		"sampledAtMs": time.Now().UnixMilli(),
	}
	if detail {
		out["detail"] = m.detailSample(r, latest, opts, time.Now())
	}
	return out, nil
}
