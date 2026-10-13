package hosts

import (
	"context"
	"encoding/json"
	"net/http"
	"sort"
	"strings"
	"sync"
	"time"

	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/phonefamily"
	"forkmesh.com/desktop-core/internal/qsettings"
	"forkmesh.com/desktop-core/internal/rpc"
)

const (
	contextTimeout       = 3 * time.Second
	logFlushInterval     = 400 * time.Millisecond
	serviceTimeout       = 45 * time.Second
	diskUsageTimeout     = 60 * time.Second
	execMaxOutput        = 64 * 1024
	maxFinishedOps       = 50
	maxLinesPerNotify    = 500
	broadcastAfterAnswer = 50 * time.Millisecond
)

type rosterEntry struct {
	Name    string `json:"name"`
	Version string `json:"version"`
	Online  bool   `json:"online"`
}

type hostsContext struct {
	Desktop      string            `json:"desktop"`
	Roster       []rosterEntry     `json:"roster"`
	CanCreate    bool              `json:"canCreate"`
	EditKey      bool              `json:"editKey"`
	Busy         bool              `json:"busy"`
	Provisioning string            `json:"provisioning"`
	InstallText  string            `json:"installStatus"`
	LinkedNodes  []string          `json:"linkedNodes"`
	Probes       map[string]string `json:"probes"`
	Provision    json.RawMessage   `json:"provision"`
	MirrorClient struct {
		Status string `json:"status"`
		Health string `json:"health"`
	} `json:"mirrorClient"`
	MirrorPush struct {
		Pushing int `json:"pushing"`
		Pending int `json:"pending"`
	} `json:"mirrorPush"`
	Deploying  int  `json:"deploying"`
	Admin      bool `json:"admin"`
	NodeDelete struct {
		InProgress bool   `json:"inProgress"`
		Target     string `json:"target"`
	} `json:"nodeDelete"`
	FleetRunning []string          `json:"fleetRunning"`
	Gateways     []string          `json:"gateways"`
	ProvisionLog string            `json:"provisionLog"`
	Repos        []json.RawMessage `json:"repos"`

	valid     bool
	fetchedAt int64
}

type subscription struct {
	id     string
	peer   rpc.Peer
	host   string
	stream *logStream
}

type operation struct {
	ID       string `json:"id"`
	Kind     string `json:"kind"`
	Action   string `json:"action,omitempty"`
	Host     string `json:"host"`
	Origin   string `json:"origin"`
	Started  int64  `json:"startedAt"`
	Finished int64  `json:"finishedAt,omitempty"`
	Running  bool   `json:"running"`
	OK       bool   `json:"ok"`
	Output   string `json:"output,omitempty"`
}

type Module struct {
	Owners         func() []string
	Now            func() time.Time
	HTTPClient     *http.Client
	VultrBase      string
	CloudflareBase string
	SSHDir         string
	CallUI         func(ctx context.Context, method string, params any) ([]byte, error)
	WriteSettings  func(ctx context.Context, writes []daemon.SettingWrite) error
	Attached       func() bool
	Sleep          func(ctx context.Context, d time.Duration) bool
	Variables      func() map[string]string

	core    *daemon.Core
	ctx     context.Context
	gate    *phonefamily.Gate
	hostOK  *phonefamily.Confirmations
	fleetOK *phonefamily.Confirmations

	mu           sync.Mutex
	probes       map[string]probeOutcome
	passwords    map[string]string
	recent       map[string][]string
	streams      map[string]*subscription
	watched      map[string]bool
	running      map[string]string
	operations   map[string]*operation
	finished     []string
	last         hostsContext
	probingAll   bool
	lastProbeAll int64
	wake         chan struct{}
	active       int
	jobs         *jobTable
	writes       *settingsQueue
	env          hostEnv
	savedMu      sync.Mutex
	fleetRunning map[string]string
	prov         *provisioner
	deploy       *deployState
	mirrors      *mirrorEngine
	vultrKept    string

	lastInstallFailure string
}

func New() *Module {
	return &Module{hostOK: phonefamily.NewConfirmations(), fleetOK: phonefamily.NewConfirmations()}
}

func (m *Module) Name() string { return "hosts" }

func (m *Module) Start(ctx context.Context, core *daemon.Core) error {
	m.core = core
	m.ctx = ctx
	if m.hostOK == nil {
		m.hostOK = phonefamily.NewConfirmations()
	}
	if m.fleetOK == nil {
		m.fleetOK = phonefamily.NewConfirmations()
	}
	m.gate = &phonefamily.Gate{Core: core, Owners: m.Owners}
	m.probes = map[string]probeOutcome{}
	m.passwords = map[string]string{}
	m.recent = map[string][]string{}
	m.streams = map[string]*subscription{}
	m.watched = map[string]bool{}
	m.running = map[string]string{}
	m.operations = map[string]*operation{}
	m.wake = make(chan struct{}, 1)
	m.fleetRunning = map[string]string{}
	m.table()
	m.queue()
	core.Registry.HandleFamily("hosts.", m.Name(), m.wrap(m.handleHosts))
	core.Registry.HandleFamily("fleet.", m.Name(), m.wrap(m.handleFleet))
	desktop := func(method string, handler rpc.Handler) {
		core.Registry.Handle(method, m.Name(), func(ctx context.Context, req *rpc.Request) (any, error) {
			if req.Peer == nil || rpc.Remote(req.Peer) {
				return nil, rpc.Fail("forbidden")
			}
			return handler(ctx, req)
		})
	}
	desktop("core.hosts.state", m.handleState)
	desktop("core.hosts.probe", m.handleProbe)
	desktop("core.hosts.probeAll", m.handleProbeAll)
	desktop("core.hosts.exec", m.handleExec)
	desktop("core.hosts.operations", m.handleOperations)
	desktop("core.hosts.lines", m.handleLines)
	desktop("core.hosts.context", m.handleContextPush)
	desktop("core.hosts.environment", m.handleEnvironmentPush)
	desktop("core.hosts.jobs", m.handleJobs)
	desktop("core.hosts.transcript", m.handleTranscript)
	desktop("core.hosts.cancel", m.handleJobCancel)
	desktop("core.hosts.fleetFinished", m.handleFleetFinished)
	m.registerJobMethods(desktop)
	m.registerMirrorMethods(desktop)
	go m.flushLoop(ctx)
	go m.settingsLoop(ctx)
	m.startExtensions(ctx)
	return nil
}

func (m *Module) Busy() bool {
	m.mu.Lock()
	defer m.mu.Unlock()
	return m.active > 0
}

func (m *Module) sleep(ctx context.Context, d time.Duration) bool {
	if m.Sleep != nil {
		return m.Sleep(ctx, d)
	}
	timer := time.NewTimer(d)
	defer timer.Stop()
	select {
	case <-ctx.Done():
		return false
	case <-timer.C:
		return true
	}
}

func (m *Module) now() time.Time {
	if m.Now != nil {
		return m.Now()
	}
	return time.Now()
}

func (m *Module) nowMs() int64 { return m.now().UnixMilli() }

func (m *Module) httpClient() *http.Client {
	if m.HTTPClient != nil {
		return m.HTTPClient
	}
	return http.DefaultClient
}

func (m *Module) vultrBase() string {
	if m.VultrBase != "" {
		return m.VultrBase
	}
	return vultrAPIBase
}

func (m *Module) sshDir() string {
	if m.SSHDir != "" {
		return m.SSHDir
	}
	if m.core != nil {
		return m.core.Layout.SSH
	}
	return ""
}

func (m *Module) settings() *qsettings.Settings {
	if m.core == nil || m.core.Settings == nil {
		return qsettings.Parse(nil)
	}
	return m.core.Settings.Get()
}

func (m *Module) savedHosts() []map[string]any {
	hosts, migrated := parseSavedHosts(m.settingString(hostsSetting))
	if len(migrated) > 0 {
		m.mu.Lock()
		for key, password := range migrated {
			m.passwords[key] = password
		}
		m.mu.Unlock()
	}
	return hosts
}

func (m *Module) password(key string) string {
	m.mu.Lock()
	defer m.mu.Unlock()
	return m.passwords[key]
}

func (m *Module) logUI(format string, args ...any) {
	if m.core != nil {
		m.core.LogUI(format, args...)
	}
}

func (m *Module) wrap(handler rpc.Handler) rpc.Handler { return m.gate.Wrap(handler) }

func deviceLabel(peer rpc.Peer) string {
	if peer == nil {
		return "a phone"
	}
	device := peer.Device()
	label := device.Name
	if label == "" {
		label = "Phone"
	}
	if device.User != "" {
		label += " (" + device.User + ")"
	}
	if peer.Transport() == rpc.Relay {
		label += ", through the relay"
	} else {
		label += ", on the local network"
	}
	return label
}

func (m *Module) broadcastPhones(event string, data map[string]any) {
	if m.core == nil || m.core.Hub == nil {
		return
	}
	if data == nil {
		data = map[string]any{}
	}
	m.core.Hub.Broadcast(event, data, rpc.Remote)
}

func (m *Module) broadcastUI(event string, data any) {
	if m.core != nil && m.core.Hub != nil {
		m.core.BroadcastUI(event, data)
	}
}

func (m *Module) desktopAttached() bool {
	if m.Attached != nil {
		return m.Attached()
	}
	return m.core != nil && m.core.IPC != nil && m.core.UI() != nil
}

func (m *Module) desktopContext(ctx context.Context, fetch bool) hostsContext {
	if fetch && m.desktopAttached() {
		callCtx, cancel := context.WithTimeout(ctx, contextTimeout)
		raw, err := m.callUI(callCtx, "ui.hosts.context", map[string]any{})
		cancel()
		if err == nil {
			var fresh hostsContext
			if json.Unmarshal(raw, &fresh) == nil {
				fresh.valid = true
				fresh.fetchedAt = m.nowMs()
				m.mu.Lock()
				m.last = fresh
				m.mu.Unlock()
				return fresh
			}
		}
	}
	m.mu.Lock()
	defer m.mu.Unlock()
	return m.last
}

func (m *Module) handleContextPush(ctx context.Context, req *rpc.Request) (any, error) {
	var fresh hostsContext
	if err := req.Bind(&fresh); err != nil {
		return nil, err
	}
	fresh.valid = true
	fresh.fetchedAt = m.nowMs()
	m.mu.Lock()
	m.last = fresh
	m.mu.Unlock()
	return map[string]any{}, nil
}

func (m *Module) probeLabel(hc hostsContext, key string) string {
	m.mu.Lock()
	outcome, own := m.probes[key]
	m.mu.Unlock()
	if hc.valid && hc.Probes != nil {
		if label, ok := hc.Probes[key]; ok && (!own || outcome.At <= hc.fetchedAt) {
			return label
		}
	}
	if own {
		return outcome.Status
	}
	return ""
}

func lowerHost(host string) string { return strings.ToLower(strings.TrimSpace(host)) }

func (m *Module) hostsPayload(hc hostsContext) map[string]any {
	hosts := m.savedHosts()
	roster := map[string]rosterEntry{}
	for _, member := range hc.Roster {
		if member.Name != "" {
			roster[lowerHost(member.Name)] = member
		}
	}
	rows := make([]map[string]any, 0, len(hosts))
	online, attention := 0, 0
	for _, host := range hosts {
		name := jsonString(host["name"])
		probe := m.probeLabel(hc, credentialKey(name, jsonString(host["ip"]), jsonString(host["user"])))
		member, known := roster[lowerHost(name)]
		row := hostRow(host, probe, member.Version, known && member.Online)
		switch row["health"] {
		case "online":
			online++
		case "attention", "offline":
			attention++
		}
		rows = append(rows, row)
	}
	return map[string]any{
		"hosts":        rows,
		"online":       online,
		"attention":    attention,
		"canCreate":    hc.CanCreate || vultrAPIKeyFromVariables(m.actionVariables()) != "",
		"busy":         m.busyForPhones(hc),
		"provisioning": m.provisioningNode(hc),
		"desktop":      hc.Desktop,
	}
}

func (m *Module) hostDetail(hc hostsContext, host string) map[string]any {
	payload := m.hostsPayload(hc)
	rows, _ := payload["hosts"].([]map[string]any)
	var row map[string]any
	for _, candidate := range rows {
		if lowerHost(jsonString(candidate["name"])) == lowerHost(host) {
			row = candidate
			break
		}
	}
	if row == nil {
		return nil
	}
	m.mu.Lock()
	recent := m.recent[lowerHost(host)]
	if len(recent) > 60 {
		recent = recent[len(recent)-60:]
	}
	row["recentLog"] = append([]string{}, recent...)
	m.mu.Unlock()
	row["lastResult"] = leftUTF16(hc.InstallText, 600)
	return row
}

func (m *Module) track() func() {
	m.mu.Lock()
	m.active++
	m.mu.Unlock()
	return func() {
		m.mu.Lock()
		m.active--
		m.mu.Unlock()
	}
}

func (m *Module) beginOperation(kind, action, host, origin string) *operation {
	op := &operation{ID: newStreamID(), Kind: kind, Action: action, Host: host, Origin: origin, Started: m.nowMs(), Running: true}
	m.mu.Lock()
	m.operations[op.ID] = op
	m.active++
	snapshot := *op
	m.mu.Unlock()
	m.broadcastUI("core.hosts.operation", snapshot)
	return op
}

func (m *Module) endOperation(op *operation, ok bool, output string) {
	m.mu.Lock()
	op.Running = false
	op.OK = ok
	op.Output = output
	op.Finished = m.nowMs()
	m.active--
	m.finished = append(m.finished, op.ID)
	for len(m.finished) > maxFinishedOps {
		delete(m.operations, m.finished[0])
		m.finished = m.finished[1:]
	}
	snapshot := *op
	m.mu.Unlock()
	m.broadcastUI("core.hosts.operation", snapshot)
}

func (m *Module) operationList() []operation {
	m.mu.Lock()
	defer m.mu.Unlock()
	out := make([]operation, 0, len(m.operations))
	for _, op := range m.operations {
		out = append(out, *op)
	}
	sort.Slice(out, func(i, j int) bool {
		if out[i].Started != out[j].Started {
			return out[i].Started < out[j].Started
		}
		return out[i].ID < out[j].ID
	})
	return out
}

func (m *Module) runMirrorNodeSSH(ctx context.Context, node, remote string, timeout time.Duration, provisioningIP, provisioningIdentity string) (bool, string) {
	hosts := m.savedHosts()
	ip := strings.TrimSpace(provisioningIP)
	if ip == "" {
		ip = savedHostAddressNamed(hosts, node)
	}
	if ip == "" {
		return false, ""
	}
	identity := strings.TrimSpace(provisioningIdentity)
	if identity == "" {
		identity = savedHostIdentityFile(hosts, m.sshDir(), node, ip, "root")
	}
	command, err := buildHostSSHCommand(m.sshDir(), ip, "root", "", remote, identity)
	if err != nil {
		return false, err.Error()
	}
	result := runSSH(ctx, command, timeout, execMaxOutput, false)
	return result.OK, reportOutput(result.Output)
}
