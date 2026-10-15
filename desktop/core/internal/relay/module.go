package relay

import (
	"context"
	"encoding/json"
	"errors"
	"net/url"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"time"
	"unicode/utf16"

	"forkmesh.com/desktop-core/internal/control"
	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/identity"
	"forkmesh.com/desktop-core/internal/ipc"
	"forkmesh.com/desktop-core/internal/rpc"
)

const (
	EventState = "core.relay.state"
	EventFrame = "core.relay.event"
	maxUIQueue = 4096
	savedFile  = "relay.json"
)

var forwarded = map[string]bool{
	"event":               true,
	"terminal":            true,
	"control":             true,
	"agent-status-result": true,
	"heartbeat-result":    true,
}

type ControlSink interface {
	SetRelaySender(send func(frame map[string]any) bool)
	RelayIn(frame map[string]any)
	RelayRestored()
}

type Config struct {
	Endpoint string
	Owner    string
	Account  string
	Node     string
}

type saved struct {
	Endpoint string `json:"endpoint"`
	Owner    string `json:"owner"`
	Account  string `json:"account,omitempty"`
	Node     string `json:"node,omitempty"`
	Identity string `json:"identity"`
}

type Module struct {
	core *daemon.Core
	ctx  context.Context

	mu       sync.Mutex
	client   *Client
	config   Config
	identity *identity.Identity
	sink     ControlSink

	uiMu     sync.Mutex
	uiQueue  []uiEvent
	uiSignal chan struct{}

	Timings        Timings
	NewClient      func() *Client
	Now            func() time.Time
	ReclaimWindow  time.Duration
	uiEpoch        int64
	configureEpoch int64
}

type uiEvent struct {
	name string
	data any
}

func New() *Module {
	return &Module{Timings: DefaultTimings(), NewClient: NewClient, Now: time.Now, uiSignal: make(chan struct{}, 1),
		ReclaimWindow: 2 * time.Minute}
}

func (m *Module) Name() string { return "relay" }

func (m *Module) Start(ctx context.Context, core *daemon.Core) error {
	m.core = core
	m.ctx = ctx
	m.register()
	previous := core.IPC.OnUI
	core.IPC.OnUI = func(conn *ipc.Conn, attached bool) {
		if previous != nil {
			previous(conn, attached)
		}
		if attached {
			m.uiAttached()
		}
	}
	go m.deliver(ctx)
	m.resume()
	return nil
}

func (m *Module) savedPath() string {
	if m.core == nil || m.core.Layout.State == "" {
		return ""
	}
	return filepath.Join(m.core.Layout.CoreState(), savedFile)
}

func (m *Module) save(config Config, id *identity.Identity) {
	path := m.savedPath()
	if path == "" || id == nil {
		return
	}
	raw, err := json.Marshal(saved{Endpoint: config.Endpoint, Owner: config.Owner, Account: config.Account,
		Node: config.Node, Identity: id.PublicKey()})
	if err != nil {
		return
	}
	if err := os.MkdirAll(filepath.Dir(path), 0o700); err != nil {
		m.core.Logger.Printf("relay: could not save the relay settings: %v", err)
		return
	}
	temp := path + ".tmp"
	if err := os.WriteFile(temp, raw, 0o600); err != nil {
		m.core.Logger.Printf("relay: could not save the relay settings: %v", err)
		return
	}
	if err := os.Rename(temp, path); err != nil {
		m.core.Logger.Printf("relay: could not save the relay settings: %v", err)
	}
}

func (m *Module) forget() {
	path := m.savedPath()
	if path == "" {
		return
	}
	if err := os.Remove(path); err != nil && !errors.Is(err, os.ErrNotExist) {
		m.core.Logger.Printf("relay: could not remove the saved relay settings: %v", err)
	}
}

func (m *Module) resume() {
	path := m.savedPath()
	if path == "" {
		return
	}
	raw, err := os.ReadFile(path)
	if err != nil {
		return
	}
	var last saved
	if json.Unmarshal(raw, &last) != nil {
		m.core.Logger.Printf("relay: ignoring unreadable %s", path)
		return
	}
	target, err := url.Parse(last.Endpoint)
	if err != nil || target.Hostname() == "" || (target.Scheme != "ws" && target.Scheme != "wss") || last.Owner == "" {
		m.core.Logger.Printf("relay: ignoring invalid saved relay settings in %s", path)
		return
	}
	id, err := m.core.RequireIdentity()
	if err != nil {
		m.core.Logger.Printf("relay: not reconnecting at start: %v", err)
		return
	}
	if id.PublicKey() != last.Identity {
		m.core.Logger.Printf("relay: not reconnecting at start: the node key changed since the relay was configured")
		return
	}
	m.mu.Lock()
	if m.client != nil {
		m.mu.Unlock()
		return
	}
	m.identity = id
	m.config = Config{Endpoint: last.Endpoint, Owner: last.Owner, Account: last.Account, Node: last.Node}
	client := m.newClient()
	m.client = client
	m.mu.Unlock()
	m.own(client)
	m.core.Logger.Printf("relay: connecting %s for %s with the settings the desktop last gave, without waiting for it", last.Endpoint, last.Owner)
	client.Start(m.ctx)
}

func (m *Module) uiAttached() {
	m.mu.Lock()
	m.uiEpoch++
	epoch := m.uiEpoch
	window := m.ReclaimWindow
	m.mu.Unlock()
	if window <= 0 {
		return
	}
	time.AfterFunc(window, func() {
		m.mu.Lock()
		unclaimed := m.uiEpoch == epoch && m.configureEpoch < epoch && m.client != nil
		m.mu.Unlock()
		if unclaimed && m.ctx.Err() == nil {
			m.core.Logger.Printf("relay: the desktop did not reclaim the relay socket within %s; closing it", window)
			m.stop()
		}
	})
}

func (m *Module) Stop() {
	m.mu.Lock()
	client := m.client
	m.client = nil
	sink := m.sink
	m.mu.Unlock()
	if sink != nil {
		sink.SetRelaySender(nil)
	}
	if client != nil {
		client.Stop()
	}
}

func (m *Module) controlSink() ControlSink {
	if m.core == nil {
		return nil
	}
	sink, _ := m.core.Get(control.RelaySinkKey).(ControlSink)
	return sink
}

func (m *Module) post(name string, data any) {
	m.uiMu.Lock()
	if len(m.uiQueue) >= maxUIQueue {
		m.uiQueue = m.uiQueue[1:]
	}
	m.uiQueue = append(m.uiQueue, uiEvent{name: name, data: data})
	m.uiMu.Unlock()
	select {
	case m.uiSignal <- struct{}{}:
	default:
	}
}

func (m *Module) deliver(ctx context.Context) {
	for {
		m.uiMu.Lock()
		batch := m.uiQueue
		m.uiQueue = nil
		m.uiMu.Unlock()
		if len(batch) == 0 {
			select {
			case <-ctx.Done():
				return
			case <-m.uiSignal:
			}
			continue
		}
		for _, event := range batch {
			m.core.BroadcastUI(event.name, event.data)
		}
	}
}

func diagnosticToken(value string) string {
	units := utf16.Encode([]rune(strings.TrimSpace(value)))
	if len(units) > 64 {
		units = units[:64]
	}
	text := string(utf16.Decode(units))
	var out strings.Builder
	for _, b := range []byte(text) {
		switch {
		case b >= 'a' && b <= 'z', b >= 'A' && b <= 'Z', b >= '0' && b <= '9', b == '-', b == '.', b == '_', b == '~':
			out.WriteByte(b)
		default:
			out.WriteString("%" + strings.ToUpper(strconv.FormatInt(int64(b)>>4, 16)+strconv.FormatInt(int64(b)&15, 16)))
		}
	}
	return out.String()
}

func UserAgent(version, account, node string) string {
	if version == "" {
		version = "dev"
	}
	return "ForkMeshDesktop/" + version + " (account=" + diagnosticToken(account) + "; node=" + diagnosticToken(node) + ")"
}

func Canonical(owner, ts string) []byte {
	return []byte("forkmesh-issues-pull-v1\n" + owner + "\n" + ts)
}

func SignedURL(endpoint, owner string, id *identity.Identity, now time.Time) string {
	ts := strconv.FormatInt(now.UnixMilli(), 10)
	return endpoint + "?owner=" + url.QueryEscape(owner) + "&ts=" + ts + "&sig=" + url.QueryEscape(id.Sign(Canonical(owner, ts)))
}

func parseConfig(params map[string]any) (Config, string, string, error) {
	raw, _ := params["url"].(string)
	target, err := url.Parse(strings.TrimSpace(raw))
	if err != nil || target.Hostname() == "" || (target.Scheme != "ws" && target.Scheme != "wss") {
		return Config{}, "", "", rpc.Fail("invalid_url")
	}
	query := target.Query()
	owner, _ := params["owner"].(string)
	owner = strings.TrimSpace(owner)
	if owner == "" {
		owner = query.Get("owner")
	}
	if owner == "" {
		return Config{}, "", "", rpc.Fail("invalid_owner")
	}
	target.RawQuery = ""
	target.Fragment = ""
	account, _ := params["account"].(string)
	node, _ := params["node"].(string)
	return Config{Endpoint: target.String(), Owner: owner, Account: account, Node: node}, query.Get("ts"), query.Get("sig"), nil
}

func (m *Module) loadIdentity(owner, ts, sig, wantKey string) (*identity.Identity, error) {
	matches := func(id *identity.Identity) bool {
		if id == nil {
			return false
		}
		if wantKey != "" && id.PublicKey() != wantKey {
			return false
		}
		if sig != "" && !identity.Verify(id.PublicKey(), sig, Canonical(owner, ts)) {
			return false
		}
		return true
	}
	m.mu.Lock()
	current := m.identity
	m.mu.Unlock()
	if matches(current) {
		return current, nil
	}
	if current == nil {
		if loaded, err := m.core.RequireIdentity(); err == nil && matches(loaded) {
			return loaded, nil
		}
	}
	loaded, err := identity.Load(m.core.Layout.IdentityKey())
	if err != nil {
		return nil, rpc.Fail("no_identity")
	}
	if !matches(loaded) {
		return nil, rpc.Fail("identity_mismatch")
	}
	return loaded, nil
}

func (m *Module) configure(params map[string]any) (any, error) {
	config, ts, sig, err := parseConfig(params)
	if err != nil {
		return nil, err
	}
	wantKey, _ := params["identity"].(string)
	id, err := m.loadIdentity(config.Owner, ts, sig, strings.TrimSpace(wantKey))
	if err != nil {
		return nil, err
	}
	m.save(config, id)
	m.mu.Lock()
	m.identity = id
	m.configureEpoch = m.uiEpoch
	same := m.client != nil && m.config.Endpoint == config.Endpoint && m.config.Owner == config.Owner
	m.config = config
	if same {
		client := m.client
		m.mu.Unlock()
		m.own(client)
		return m.stateOf(client), nil
	}
	previous := m.client
	client := m.newClient()
	m.client = client
	m.mu.Unlock()
	if previous != nil {
		previous.Stop()
	}
	m.own(client)
	m.core.Logger.Printf("relay: connecting %s for %s", config.Endpoint, config.Owner)
	client.Start(m.ctx)
	return m.stateOf(client), nil
}

func (m *Module) own(client *Client) {
	sink := m.controlSink()
	m.mu.Lock()
	m.sink = sink
	m.mu.Unlock()
	if sink != nil {
		sink.SetRelaySender(client.SendOwnerControl)
	}
}

func (m *Module) newClient() *Client {
	client := m.NewClient()
	client.Timings = m.Timings
	client.SignedURL = func() (string, bool) {
		m.mu.Lock()
		config, id, current := m.config, m.identity, m.client
		m.mu.Unlock()
		if current != client || id == nil || config.Owner == "" {
			return "", false
		}
		return SignedURL(config.Endpoint, config.Owner, id, m.Now()), true
	}
	client.UserAgent = func() string {
		m.mu.Lock()
		config := m.config
		m.mu.Unlock()
		return UserAgent(m.core.Version, config.Account, config.Node)
	}
	client.OnMessage = func(text string) { m.core.LogUI("%s", text) }
	client.OnConnected = func(connected bool) {
		if !m.current(client) {
			return
		}
		if connected {
			m.core.Logger.Printf("relay: connected")
			if sink := m.controlSink(); sink != nil {
				sink.RelayRestored()
			}
		} else {
			m.core.Logger.Printf("relay: link lost")
		}
	}
	client.OnState = func(state State) {
		if m.current(client) {
			m.post(EventState, state)
		}
	}
	client.OnFrame = func(kind string, frame map[string]any, raw []byte) {
		if !m.current(client) || !forwarded[kind] {
			return
		}
		if kind == "control" {
			if from, _ := frame["from"].(string); from != "browser" {
				return
			}
			if sink := m.controlSink(); sink != nil {
				sink.RelayIn(frame)
				return
			}
		}
		m.post(EventFrame, map[string]any{"frame": json.RawMessage(append([]byte(nil), raw...))})
	}
	return client
}

func (m *Module) current(client *Client) bool {
	m.mu.Lock()
	defer m.mu.Unlock()
	return m.client == client
}

func (m *Module) stateOf(client *Client) State {
	if client == nil {
		return State{Accepts: []string{}, Relays: []string{}, BudgetWindowMs: 10000, BudgetLimit: 180, Seq: stateSeq.Add(1)}
	}
	return client.State()
}

func (m *Module) stop() State {
	m.mu.Lock()
	client := m.client
	m.client = nil
	m.config = Config{}
	m.mu.Unlock()
	if sink := m.controlSink(); sink != nil {
		sink.SetRelaySender(nil)
	}
	if client != nil {
		client.Stop()
		m.core.Logger.Printf("relay: stopped by the desktop")
	}
	state := m.stateOf(nil)
	m.post(EventState, state)
	return state
}

func (m *Module) active() *Client {
	m.mu.Lock()
	defer m.mu.Unlock()
	return m.client
}

func (m *Module) Client() *Client { return m.active() }

func (m *Module) Busy() bool { return false }

func (m *Module) register() {
	handle := func(method string, handler func(params map[string]any) (any, error)) {
		m.core.Registry.Handle(method, "relay", func(ctx context.Context, req *rpc.Request) (any, error) {
			if req.Peer == nil || rpc.Remote(req.Peer) {
				return nil, rpc.Fail("forbidden")
			}
			return handler(req.Object())
		})
	}
	handle("core.relay.configure", m.configure)
	handle("core.relay.stop", func(map[string]any) (any, error) {
		m.forget()
		return m.stop(), nil
	})
	handle("core.relay.state", func(map[string]any) (any, error) { return m.stateOf(m.active()), nil })
	handle("core.relay.reconnect", func(map[string]any) (any, error) {
		client := m.active()
		kicked := client != nil && client.ReconnectNow()
		return map[string]any{"reconnecting": kicked}, nil
	})
	handle("core.relay.send", func(params map[string]any) (any, error) {
		frame, _ := params["frame"].(map[string]any)
		kind, _ := params["kind"].(string)
		client := m.active()
		if client == nil || frame == nil {
			return map[string]any{"sent": false}, nil
		}
		sent := false
		switch kind {
		case "signed":
			sent = client.SendSigned(frame)
		case "terminal":
			sent = client.SendTerminal(frame)
		case "control":
			sent = client.SendOwnerControl(frame)
		default:
			return nil, rpc.Fail("invalid_kind")
		}
		return map[string]any{"sent": sent}, nil
	})
}
