package control

import (
	"context"
	"encoding/base64"
	"encoding/json"
	"net"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/ipc"
	"forkmesh.com/desktop-core/internal/paths"
	"forkmesh.com/desktop-core/internal/rpc"
)

const (
	maxConnections        = 8
	handshakeTimeout      = 15 * time.Second
	maintenanceInterval   = 60 * time.Second
	relayIdleMs           = 15 * 60 * 1000
	relayEvictIdleMs      = 60 * 1000
	relayOpeningTimeoutMs = 30 * 1000
	maxRelayOpenings      = 16
	maxRelayNotes         = 64
	accountRecheckMs      = 10 * 60 * 1000
	accountRetryMs        = 2 * 60 * 1000
	accountGraceMs        = 24 * 60 * 60 * 1000
	accountCheckTimeout   = 10 * time.Second
	maxFileBytes          = 2 * 1024 * 1024 * 1024
	outgoingHighWater     = 1024 * 1024
	maxProxiedPerConn     = 4096

	approvalAccount = "account"
	approvalCode    = "code"

	VerdictOwned       = "owned"
	VerdictNotOwned    = "not_owned"
	VerdictUnreachable = "unreachable"
)

type Module struct {
	core   *daemon.Core
	ctx    context.Context
	cancel context.CancelFunc

	queueMu sync.Mutex
	queue   []func()
	wake    chan struct{}
	exited  chan struct{}
	running atomic.Bool
	open    atomic.Int64

	StorePath  string
	ReceiveDir string

	accountHook func(device Device) string
	relayHook   func(frame map[string]any) bool
	relaySender atomic.Pointer[func(frame map[string]any) bool]
	now         func() int64

	publicKey string
	sign      func([]byte) string
	name      string
	user      string
	node      string
	stopping  bool

	devices           map[string]Device
	conns             []*connection
	nextID            int
	relayOpening      map[string][]byte
	relayOpeningSeq   map[string]uint32
	relayOpeningSince map[string]int64
	relayUndelivered  []string
	relayToldGone     []string

	relay    *relayWriter
	notifier *notifier
	peers    sync.Map
}

func New() *Module {
	return &Module{wake: make(chan struct{}, 1), exited: make(chan struct{}), nextID: 1,
		devices: map[string]Device{}, relayOpening: map[string][]byte{}, relayOpeningSeq: map[string]uint32{},
		relayOpeningSince: map[string]int64{}}
}

func (m *Module) Name() string { return "control" }

func (m *Module) Busy() bool { return m.open.Load() > 0 }

func (m *Module) Peer(connection int) rpc.Peer {
	value, ok := m.peers.Load(connection)
	if !ok {
		return nil
	}
	peer := value.(*phonePeer)
	if !peer.paired.Load() {
		return nil
	}
	return peer
}

func (m *Module) Start(ctx context.Context, core *daemon.Core) error {
	m.core = core
	m.ctx, m.cancel = context.WithCancel(ctx)
	if m.now == nil {
		m.now = func() int64 { return time.Now().UnixMilli() }
	}
	if m.StorePath == "" {
		m.StorePath = filepath.Join(core.Layout.Data, "local_control", "paired-devices.json")
	}
	if m.ReceiveDir == "" {
		m.ReceiveDir = ReceivedFilesDirectory(core.Layout)
	}
	m.devices = LoadDevices(m.StorePath)
	m.recallInfo()
	m.relay = newRelayWriter(m)
	m.notifier = newNotifier(m)
	m.registerMethods()
	core.Set(RelaySinkKey, m)
	core.IPC.OnAdopt = m.onAdopt
	previous := core.IPC.OnUI
	core.IPC.OnUI = func(conn *ipc.Conn, attached bool) {
		if previous != nil {
			previous(conn, attached)
		}
		m.post(func() { m.uiChanged(attached) })
	}
	m.running.Store(true)
	go m.loop()
	go m.relay.run(m.ctx)
	go m.notifier.run(m.ctx)
	go m.maintenanceTicker()
	return nil
}

func (m *Module) Stop() {
	if !m.running.Load() {
		return
	}
	m.call(func() any {
		m.stopping = true
		for _, conn := range append([]*connection(nil), m.conns...) {
			m.closeConnection(conn, "")
		}
		return nil
	})
	m.cancel()
	<-m.exited
}

func (m *Module) post(task func()) {
	m.queueMu.Lock()
	m.queue = append(m.queue, task)
	m.queueMu.Unlock()
	select {
	case m.wake <- struct{}{}:
	default:
	}
}

func (m *Module) call(task func() any) any {
	reply := make(chan any, 1)
	m.post(func() { reply <- task() })
	select {
	case value := <-reply:
		return value
	case <-m.exited:
		return nil
	}
}

func (m *Module) loop() {
	defer close(m.exited)
	for {
		select {
		case <-m.ctx.Done():
			m.running.Store(false)
			m.stopping = true
			for _, conn := range append([]*connection(nil), m.conns...) {
				m.closeConnection(conn, "")
			}
			return
		case <-m.wake:
		}
		for {
			m.queueMu.Lock()
			tasks := m.queue
			m.queue = nil
			m.queueMu.Unlock()
			if len(tasks) == 0 {
				break
			}
			for _, task := range tasks {
				m.runTask(task)
			}
		}
	}
}

func (m *Module) runTask(task func()) {
	defer func() {
		if recovered := recover(); recovered != nil {
			m.logf("control: task panicked: %v", recovered)
		}
	}()
	task()
}

func (m *Module) maintenanceTicker() {
	ticker := time.NewTicker(maintenanceInterval)
	defer ticker.Stop()
	for {
		select {
		case <-m.ctx.Done():
			return
		case <-ticker.C:
			m.post(func() { m.maintain(m.now()) })
		}
	}
}

func (m *Module) logf(format string, args ...any) {
	if m.core != nil && m.core.Logger != nil {
		m.core.Logger.Printf(format, args...)
	}
}

func (m *Module) ensureIdentity() bool {
	if m.publicKey != "" && m.sign != nil {
		return true
	}
	id, err := m.core.RequireIdentity()
	if err != nil || id == nil {
		return false
	}
	m.publicKey = id.PublicKey()
	m.sign = id.Sign
	return m.publicKey != ""
}

func (m *Module) notifyUI(method string, params any, onFail func()) {
	if m.stopping {
		return
	}
	m.notifier.push(method, params, onFail)
}

func (m *Module) onAdopt(stream net.Conn, params ipc.AdoptParams) {
	hello := decodeStd(params.Hello)
	rest := decodeStd(params.Rest)
	if len(hello) == 0 {
		stream.Close()
		return
	}
	m.Adopt(stream, hello, rest)
}

func decodeStd(text string) []byte {
	if text == "" {
		return nil
	}
	if raw, err := base64.StdEncoding.DecodeString(text); err == nil {
		return raw
	}
	if raw, err := base64.RawStdEncoding.DecodeString(strings.TrimRight(text, "=")); err == nil {
		return raw
	}
	return nil
}

func (m *Module) Adopt(stream net.Conn, hello, rest []byte) {
	hello = append([]byte(nil), hello...)
	rest = append([]byte(nil), rest...)
	if !m.running.Load() {
		stream.Close()
		return
	}
	m.post(func() { m.adopt(stream, hello, rest) })
}

func ReceivedFilesDirectory(layout paths.Layout) string {
	home := ""
	if layout.LegacyState != "" {
		home = filepath.Dir(layout.LegacyState)
	}
	if home == "" || home == "." {
		return filepath.Join(layout.Data, "phone-files")
	}
	downloads := ""
	if runtime.GOOS != "windows" && runtime.GOOS != "darwin" {
		configHome := filepath.Join(home, ".config")
		if layout.LegacyConfig != "" {
			configHome = filepath.Dir(layout.LegacyConfig)
		}
		downloads = xdgUserDir(filepath.Join(configHome, "user-dirs.dirs"), "DOWNLOAD", home)
	}
	if downloads == "" {
		downloads = filepath.Join(home, "Downloads")
	}
	return filepath.Join(downloads, "ForkMesh from phone")
}

func xdgUserDir(file, key, home string) string {
	raw, err := os.ReadFile(file)
	if err != nil {
		return ""
	}
	result := ""
	found := false
	for _, line := range strings.Split(string(raw), "\n") {
		line = strings.TrimRight(line, "\r")
		if !strings.HasPrefix(line, "XDG_") {
			continue
		}
		eq := strings.LastIndex(line, "_DIR=")
		if eq < 4 || line[4:eq] != key {
			continue
		}
		value := line[eq+5:]
		if len(value) > 2 && strings.HasPrefix(value, "\"") && strings.HasSuffix(value, "\"") {
			value = value[1 : len(value)-1]
		}
		if strings.HasPrefix(value, "$HOME") {
			result = home + value[5:]
		} else {
			result = value
		}
		if len(result) > 1 && strings.HasSuffix(result, "/") {
			result = result[:len(result)-1]
		}
		found = true
	}
	if !found {
		return ""
	}
	return result
}

func (m *Module) uiChanged(attached bool) {
	if !attached {
		for _, conn := range m.conns {
			ids := make([]int, 0, len(conn.proxied))
			for id := range conn.proxied {
				ids = append(ids, id)
			}
			conn.proxied = map[int]struct{}{}
			for _, id := range ids {
				m.respondError(conn, id, "desktop_unavailable")
			}
		}
		return
	}
	for _, conn := range m.conns {
		if conn.state == statePaired {
			m.notifyUI("ui.control.opened", m.openedParams(conn), nil)
		}
	}
	m.notifyUI("ui.control.devices", m.devicesPayload(), nil)
}

func (m *Module) openedParams(conn *connection) map[string]any {
	return map[string]any{"connection": conn.id, "device": conn.device.record(), "relayed": conn.relayed()}
}

func (m *Module) devicesPayload() map[string]any {
	paired := []any{}
	for _, device := range sortedDevices(m.devices) {
		paired = append(paired, device.record())
	}
	connected := []any{}
	for _, conn := range m.conns {
		if conn.state == stateAwaitAuth {
			continue
		}
		record := conn.device.record()
		record["connection"] = conn.id
		record["relayed"] = conn.relayed()
		connected = append(connected, record)
	}
	return map[string]any{"paired": paired, "connected": connected}
}

func (m *Module) devicesChanged() {
	m.notifyUI("ui.control.devices", m.devicesPayload(), nil)
}

func forbidden(req *rpc.Request) bool {
	return req.Peer != nil && rpc.Remote(req.Peer)
}

func (m *Module) registerMethods() {
	registry := m.core.Registry
	handle := func(method string, handler rpc.Handler) {
		registry.Handle(method, "control", func(ctx context.Context, req *rpc.Request) (any, error) {
			if forbidden(req) {
				return nil, rpc.Fail("forbidden")
			}
			return handler(ctx, req)
		})
	}
	handle("control.info", func(ctx context.Context, req *rpc.Request) (any, error) {
		params := req.Object()
		m.call(func() any {
			m.applyInfo(savedInfo{Name: jsonString(params["name"]), User: jsonString(params["user"]),
				Node: jsonString(params["node"])})
			m.relay.configureFrom(params)
			return nil
		})
		m.rememberInfo()
		return map[string]any{}, nil
	})
	handle("control.relay.in", func(ctx context.Context, req *rpc.Request) (any, error) {
		frame := jsonObject(req.Object()["frame"])
		m.post(func() { m.relayFrame(frame) })
		return map[string]any{}, nil
	})
	handle("control.relay.restored", func(ctx context.Context, req *rpc.Request) (any, error) {
		params := req.Object()
		m.post(func() {
			m.relay.restored(params)
			m.relayRestored()
		})
		return map[string]any{}, nil
	})
	handle("control.reply", func(ctx context.Context, req *rpc.Request) (any, error) {
		var params struct {
			Connection json.RawMessage `json:"connection"`
			Message    json.RawMessage `json:"message"`
		}
		if err := req.Bind(&params); err != nil {
			return nil, err
		}
		var connection any
		_ = json.Unmarshal(params.Connection, &connection)
		id, _ := jsonInt(connection)
		message := params.Message
		m.post(func() { m.reply(id, message) })
		return map[string]any{}, nil
	})
	handle("control.broadcast", func(ctx context.Context, req *rpc.Request) (any, error) {
		var params struct {
			Event string          `json:"event"`
			Data  json.RawMessage `json:"data"`
		}
		if err := req.Bind(&params); err != nil {
			return nil, err
		}
		if params.Event == "" {
			return map[string]any{}, nil
		}
		plain := eventBytes(params.Event, objectOrEmpty(params.Data))
		m.post(func() { m.broadcast(plain) })
		return map[string]any{}, nil
	})
	handle("control.devices", func(ctx context.Context, req *rpc.Request) (any, error) {
		result := m.call(func() any { return m.devicesPayload() })
		if result == nil {
			return nil, rpc.Fail("unavailable")
		}
		return result, nil
	})
	handle("control.forget", func(ctx context.Context, req *rpc.Request) (any, error) {
		key := jsonString(req.Object()["key"])
		m.call(func() any {
			m.forget(key)
			return nil
		})
		return map[string]any{}, nil
	})
	handle("control.approve", func(ctx context.Context, req *rpc.Request) (any, error) {
		id, _ := jsonInt(req.Object()["connection"])
		m.call(func() any {
			m.approvePairing(id)
			return nil
		})
		return map[string]any{}, nil
	})
	handle("control.deny", func(ctx context.Context, req *rpc.Request) (any, error) {
		id, _ := jsonInt(req.Object()["connection"])
		m.call(func() any {
			m.denyPairing(id)
			return nil
		})
		return map[string]any{}, nil
	})
	handle("control.sendFile", func(ctx context.Context, req *rpc.Request) (any, error) {
		params := req.Object()
		key := jsonString(params["key"])
		path := jsonString(params["path"])
		result := m.call(func() any { return m.sendFile(key, path) })
		if result == nil {
			return nil, rpc.Fail("unavailable")
		}
		if message, _ := result.(string); message != "" {
			return nil, rpc.Fail(message)
		}
		return map[string]any{}, nil
	})
	handle("control.status", func(ctx context.Context, req *rpc.Request) (any, error) {
		result := m.call(func() any { return m.status() })
		if result == nil {
			return nil, rpc.Fail("unavailable")
		}
		return result, nil
	})
}

func (m *Module) status() map[string]any {
	paired, relayed, pending := 0, 0, 0
	for _, conn := range m.conns {
		if conn.state == statePaired {
			paired++
		}
		if conn.state == statePending {
			pending++
		}
		if conn.relayed() {
			relayed++
		}
	}
	return map[string]any{"connections": len(m.conns), "paired": paired, "relayed": relayed, "pending": pending,
		"devices": len(m.devices)}
}

func objectOrEmpty(raw json.RawMessage) json.RawMessage {
	trimmed := strings.TrimSpace(string(raw))
	if !strings.HasPrefix(trimmed, "{") {
		return json.RawMessage("{}")
	}
	var probe map[string]json.RawMessage
	if json.Unmarshal(raw, &probe) != nil {
		return json.RawMessage("{}")
	}
	return raw
}

func eventBytes(event string, data json.RawMessage) []byte {
	compact := encodeJSON(data)
	if compact == nil {
		compact = []byte("{}")
	}
	return compactEventBytes(event, compact)
}

func compactEventBytes(event string, compact []byte) []byte {
	out := make([]byte, 0, len(compact)+len(event)+24)
	out = append(out, `{"data":`...)
	out = append(out, compact...)
	out = append(out, `,"event":`...)
	out = append(out, encodeJSON(event)...)
	out = append(out, '}')
	return out
}

func responseBytes(id int, result json.RawMessage) []byte {
	compact := encodeJSON(result)
	if compact == nil || string(compact) == "null" {
		compact = []byte("{}")
	}
	out := make([]byte, 0, len(compact)+40)
	out = append(out, `{"id":`...)
	out = append(out, encodeJSON(id)...)
	out = append(out, `,"ok":true,"result":`...)
	out = append(out, compact...)
	out = append(out, '}')
	return out
}

func errorBytes(id int, code string) []byte {
	return encodeJSON(map[string]any{"id": id, "ok": false, "error": code})
}
