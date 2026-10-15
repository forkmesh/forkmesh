package mesh

import (
	"context"
	"crypto/ecdh"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"net"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"time"

	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/ipc"
	"forkmesh.com/desktop-core/internal/rpc"
	"forkmesh.com/desktop-core/internal/vault"
)

type Options struct {
	DiscoveryPort    int
	BeaconInterval   time.Duration
	PeerTimeout      time.Duration
	LinkReplaceAfter time.Duration
	DisableMulticast bool
	DisableSweep     bool
	Interfaces       func() []Interface
	Group            string
	BindHost         string
}

type Module struct {
	Options Options

	core   *daemon.Core
	ctx    context.Context
	mu     sync.Mutex
	node   *Node
	events *pump
}

func New() *Module { return &Module{} }

func (m *Module) Name() string { return "mesh" }

func (m *Module) Start(ctx context.Context, core *daemon.Core) error {
	m.core = core
	m.ctx = ctx
	m.events = newPump()
	go m.events.run()
	m.register()
	previous := core.IPC.OnUI
	core.IPC.OnUI = func(conn *ipc.Conn, attached bool) {
		if previous != nil {
			previous(conn, attached)
		}
		if node := m.current(); node != nil {
			node.DetachBridges()
		}
	}
	return nil
}

func (m *Module) Stop() {
	m.mu.Lock()
	node := m.node
	m.node = nil
	m.mu.Unlock()
	if node != nil {
		node.Close()
	}
	if m.events != nil {
		m.events.close(2 * time.Second)
	}
}

func (m *Module) current() *Node {
	m.mu.Lock()
	defer m.mu.Unlock()
	return m.node
}

func (m *Module) Node() *Node { return m.current() }

func (m *Module) emit(event string, data any) {
	if m.core == nil {
		return
	}
	core := m.core
	m.events.push(func() { core.BroadcastUI(event, data) })
}

type startParams struct {
	NodeID      string `json:"nodeId"`
	UserName    string `json:"userName"`
	NodeName    string `json:"nodeName"`
	OwnerUser   string `json:"ownerUser"`
	AccountKind string `json:"accountKind"`
	Version     string `json:"version"`
	Platform    string `json:"platform"`
	SealKey     string `json:"sealKey"`
	HistoryFile string `json:"historyFile"`
	Control     bool   `json:"control"`
}

func (m *Module) localDir() string {
	return filepath.Join(m.core.Layout.Data, "local_mesh")
}

func idHash(nodeID string) string {
	sum := sha256.Sum256([]byte(nodeID))
	return hex.EncodeToString(sum[:])[:16]
}

func (m *Module) defaultHistoryFile(nodeID string) string {
	return filepath.Join(m.localDir(), "history-"+idHash(nodeID)+".json")
}

func (m *Module) sealPeersFile(nodeID string) string {
	return filepath.Join(m.localDir(), "seal-peers-"+idHash(nodeID)+".json")
}

func SealScope(nodeID string) string {
	return RoomName + "\n" + nodeID
}

func percentEncode(text string) string {
	var out strings.Builder
	for i := 0; i < len(text); i++ {
		c := text[i]
		if (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || strings.IndexByte("-._~", c) >= 0 {
			out.WriteByte(c)
			continue
		}
		out.WriteString("%" + strings.ToUpper(hex.EncodeToString([]byte{c})))
	}
	return out.String()
}

func SealPeersSettingKey(nodeID string) string {
	return sealPeersSetting + percentEncode(trimmed(SealScope(nodeID)))
}

func parsePeerKeys(raw []byte) []PeerKey {
	var values []any
	if err := json.Unmarshal(raw, &values); err != nil {
		return nil
	}
	out := []PeerKey{}
	for _, value := range values {
		entry, _ := value.(map[string]any)
		out = append(out, PeerKey{KeyID: str(entry["kid"]), Public: str(entry["pub"])})
	}
	return out
}

func (m *Module) loadSealPeers(nodeID string) []PeerKey {
	if raw, err := os.ReadFile(m.sealPeersFile(nodeID)); err == nil {
		return parsePeerKeys(raw)
	}
	if m.core.Settings == nil {
		return nil
	}
	value, ok := m.core.Settings.Get().Value(SealPeersSettingKey(nodeID))
	if !ok {
		return nil
	}
	raw := value.Bytes
	if value.Kind != "bytes" {
		raw = []byte(value.String())
	}
	return parsePeerKeys(raw)
}

func (m *Module) sealKey(param string) []byte {
	if key := qtBase64(param); len(key) == keyBytes {
		return key
	}
	if key := qtBase64(vault.Secret(m.core, vault.DesktopNamespace, SealKeySetting)); len(key) == keyBytes {
		return key
	}
	if m.core.Settings == nil {
		return nil
	}
	value, ok := m.core.Settings.Get().Value(SealKeySetting)
	if !ok {
		return nil
	}
	text := value.String()
	if key := qtBase64(text); len(key) == keyBytes {
		return key
	}
	return nil
}

func (m *Module) hooks(node **Node, nodeID string) Hooks {
	state := func() State { return (*node).state() }
	peersFile := m.sealPeersFile(nodeID)
	return Hooks{
		Message: func(message ChatMessage) {
			if message.Historical {
				return
			}
			m.emit("core.mesh.message", map[string]any{"message": message, "state": state()})
		},
		Queued: func(id string, queued bool) {
			m.emit("core.mesh.queued", map[string]any{"id": id, "queued": queued, "state": state()})
		},
		Wake: func(tokens []string) {
			m.emit("core.mesh.wake", map[string]any{"tokens": tokens})
		},
		Typing: func(peerID, peerName string, active bool) {
			m.emit("core.mesh.typing", map[string]any{"conversation": Conversation, "peerId": peerID, "peerName": peerName, "active": active})
		},
		Peers: func() {
			m.emit("core.mesh.peers", map[string]any{"state": state()})
		},
		System: func(text string) {
			if m.core.Logger != nil {
				m.core.Logger.Print(text)
			}
			m.emit("core.mesh.system", map[string]any{"text": text})
		},
		Blocked: func() {
			m.emit("core.mesh.blocked", map[string]any{"state": state()})
		},
		Inbound: func() {
			m.emit("core.mesh.inbound", map[string]any{})
		},
		SealPeers: func(peers []PeerKey) {
			data := marshal(peers)
			m.events.push(func() { _ = writeFileAtomic(peersFile, data, 0o600) })
		},
		BridgeWrite: func(link int, data []byte) {
			m.emit("core.mesh.link.write", map[string]any{"link": link, "data": stdBase64(data)})
		},
		BridgeClose: func(link int, reason string) {
			m.emit("core.mesh.link.close", map[string]any{"link": link, "reason": reason})
		},
		LinkDropped: func(info DropInfo) {
			m.emit("core.mesh.link.dropped", info)
		},
		Control: func(conn net.Conn, hello, rest []byte) bool {
			adopter, ok := m.core.Module("control").(interface {
				Adopt(net.Conn, []byte, []byte)
			})
			if !ok {
				return false
			}
			adopter.Adopt(conn, hello, rest)
			return true
		},
	}
}

func publicKeyID(private []byte) string {
	key, err := ecdh.X25519().NewPrivateKey(private)
	if err != nil {
		return ""
	}
	return KeyIDFor(key.PublicKey().Bytes())
}

func (m *Module) startMesh(params startParams) (any, error) {
	key := m.sealKey(params.SealKey)
	if len(key) != keyBytes {
		return nil, rpc.Fail("seal_key_unavailable")
	}
	nodeID := left16(trimmed(params.NodeID), maxIDChars)
	if nodeID == "" {
		return nil, rpc.Fail("bad_params")
	}
	historyFile := params.HistoryFile
	if historyFile == "" {
		historyFile = m.defaultHistoryFile(nodeID)
	}
	m.mu.Lock()
	defer m.mu.Unlock()
	if m.node != nil && m.node.Running() && m.node.Matches(nodeID, historyFile, publicKeyID(key)) {
		m.node.SetIdentity(params.UserName, params.NodeName, params.OwnerUser, params.AccountKind)
		m.node.SetControl(params.Control)
		return m.startReply(m.node, true), nil
	}
	if m.node != nil {
		m.node.Close()
		m.node = nil
	}
	options := m.Options
	cfg := Config{UserName: params.UserName, NodeName: params.NodeName, OwnerUser: params.OwnerUser, NodeID: nodeID,
		AccountKind: params.AccountKind, Platform: params.Platform, Version: params.Version, SealKey: key,
		SealPeers: m.loadSealPeers(nodeID), HistoryFile: historyFile, DiscoveryPort: options.DiscoveryPort,
		BeaconInterval: options.BeaconInterval, PeerTimeout: options.PeerTimeout, LinkReplaceAfter: options.LinkReplaceAfter,
		DisableMulticast: options.DisableMulticast, DisableSweep: options.DisableSweep, Interfaces: options.Interfaces,
		Group: options.Group, BindHost: options.BindHost, Control: params.Control}
	var node *Node
	created, err := NewNode(cfg, m.hooks(&node, nodeID))
	if err != nil {
		return nil, rpc.Fail("encryption_unavailable")
	}
	node = created
	m.node = node
	node.Start()
	return m.startReply(node, false), nil
}

func (m *Module) startReply(node *Node, attached bool) map[string]any {
	reply := map[string]any{"attached": attached, "state": node.StateSnapshot(), "history": node.History()}
	if attached {
		reply["note"] = node.OpenNote()
	}
	return reply
}

func (m *Module) register() {
	handle := func(method string, handler func(req *rpc.Request) (any, error)) {
		m.core.Registry.Handle(method, "mesh", func(ctx context.Context, req *rpc.Request) (any, error) {
			if req.Peer == nil || rpc.Remote(req.Peer) {
				return nil, rpc.Fail("forbidden")
			}
			return handler(req)
		})
	}
	withNode := func(handler func(node *Node, req *rpc.Request) (any, error)) func(req *rpc.Request) (any, error) {
		return func(req *rpc.Request) (any, error) {
			node := m.current()
			if node == nil {
				return nil, rpc.Fail("mesh_not_running")
			}
			return handler(node, req)
		}
	}
	handle("core.mesh.start", func(req *rpc.Request) (any, error) {
		var params startParams
		if err := req.Bind(&params); err != nil {
			return nil, err
		}
		return m.startMesh(params)
	})
	handle("core.mesh.stop", func(req *rpc.Request) (any, error) {
		m.mu.Lock()
		node := m.node
		m.node = nil
		m.mu.Unlock()
		if node != nil {
			node.Close()
		}
		return map[string]any{"stopped": node != nil}, nil
	})
	handle("core.mesh.state", func(req *rpc.Request) (any, error) {
		node := m.current()
		if node == nil {
			return map[string]any{"state": State{Queued: []string{}, WakeTargets: []string{}, Members: []Member{}}}, nil
		}
		return map[string]any{"state": node.StateSnapshot()}, nil
	})
	handle("core.mesh.history", withNode(func(node *Node, req *rpc.Request) (any, error) {
		return map[string]any{"history": node.History()}, nil
	}))
	handle("core.mesh.identity", withNode(func(node *Node, req *rpc.Request) (any, error) {
		var params startParams
		if err := req.Bind(&params); err != nil {
			return nil, err
		}
		node.SetIdentity(params.UserName, params.NodeName, params.OwnerUser, params.AccountKind)
		return map[string]any{}, nil
	}))
	handle("core.mesh.sendChat", withNode(func(node *Node, req *rpc.Request) (any, error) {
		var params struct {
			ID   string `json:"id"`
			Text string `json:"text"`
		}
		if err := req.Bind(&params); err != nil {
			return nil, err
		}
		return map[string]any{"id": node.SendChat(params.ID, params.Text)}, nil
	}))
	handle("core.mesh.sendFile", withNode(func(node *Node, req *rpc.Request) (any, error) {
		var params struct {
			ID       string `json:"id"`
			FileName string `json:"fileName"`
			MimeType string `json:"mimeType"`
			Data     string `json:"data"`
		}
		if err := req.Bind(&params); err != nil {
			return nil, err
		}
		return map[string]any{"id": node.SendFile(params.ID, params.FileName, params.MimeType, qtBase64(params.Data))}, nil
	}))
	handle("core.mesh.typing", withNode(func(node *Node, req *rpc.Request) (any, error) {
		var params struct {
			Active bool `json:"active"`
		}
		if err := req.Bind(&params); err != nil {
			return nil, err
		}
		node.SendTyping(params.Active)
		return map[string]any{}, nil
	}))
	handle("core.mesh.control", withNode(func(node *Node, req *rpc.Request) (any, error) {
		var params struct {
			Enabled bool `json:"enabled"`
		}
		if err := req.Bind(&params); err != nil {
			return nil, err
		}
		node.SetControl(params.Enabled)
		return map[string]any{}, nil
	}))
	handle("core.mesh.connect", withNode(func(node *Node, req *rpc.Request) (any, error) {
		var params struct {
			Address string `json:"address"`
			Port    int    `json:"port"`
		}
		if err := req.Bind(&params); err != nil {
			return nil, err
		}
		ip := net.ParseIP(strings.TrimSpace(params.Address))
		return map[string]any{"ok": ip != nil && node.ConnectToPeer(ip, params.Port)}, nil
	}))
	handle("core.mesh.announce", withNode(func(node *Node, req *rpc.Request) (any, error) {
		node.AnnounceNow()
		return map[string]any{}, nil
	}))
	handle("core.mesh.diagnostics", func(req *rpc.Request) (any, error) {
		var params struct {
			Bluetooth Bluetooth `json:"bluetooth"`
		}
		if err := req.Bind(&params); err != nil {
			return nil, err
		}
		node := m.current()
		if node == nil {
			return map[string]any{"line": "mesh stopped", "detail": ""}, nil
		}
		node.SetBluetooth(params.Bluetooth)
		line, detail := node.Diagnostics()
		return map[string]any{"line": line, "detail": detail}, nil
	})
	handle("core.mesh.link.open", withNode(func(node *Node, req *rpc.Request) (any, error) {
		var params struct {
			Link     int  `json:"link"`
			Outbound bool `json:"outbound"`
		}
		if err := req.Bind(&params); err != nil {
			return nil, err
		}
		if !node.BridgeOpen(params.Link, params.Outbound) {
			m.emit("core.mesh.link.close", map[string]any{"link": params.Link, "reason": "refused"})
			return map[string]any{"ok": false}, nil
		}
		return map[string]any{"ok": true}, nil
	}))
	handle("core.mesh.link.data", withNode(func(node *Node, req *rpc.Request) (any, error) {
		var params struct {
			Link int    `json:"link"`
			Data string `json:"data"`
		}
		if err := req.Bind(&params); err != nil {
			return nil, err
		}
		node.BridgeData(params.Link, qtBase64(params.Data))
		return map[string]any{}, nil
	}))
	handle("core.mesh.link.closed", withNode(func(node *Node, req *rpc.Request) (any, error) {
		var params struct {
			Link   int    `json:"link"`
			Reason string `json:"reason"`
		}
		if err := req.Bind(&params); err != nil {
			return nil, err
		}
		node.BridgeClosed(params.Link, params.Reason)
		return map[string]any{}, nil
	}))
}

type pump struct {
	mu      sync.Mutex
	queue   []func()
	wake    chan struct{}
	closed  bool
	stopped chan struct{}
}

func newPump() *pump {
	return &pump{wake: make(chan struct{}, 1), stopped: make(chan struct{})}
}

func (p *pump) push(task func()) {
	p.mu.Lock()
	if p.closed {
		p.mu.Unlock()
		return
	}
	p.queue = append(p.queue, task)
	p.mu.Unlock()
	select {
	case p.wake <- struct{}{}:
	default:
	}
}

func (p *pump) run() {
	defer close(p.stopped)
	for {
		p.mu.Lock()
		tasks := p.queue
		p.queue = nil
		closed := p.closed
		p.mu.Unlock()
		for _, task := range tasks {
			func() {
				defer func() { _ = recover() }()
				task()
			}()
		}
		if len(tasks) > 0 {
			continue
		}
		if closed {
			return
		}
		<-p.wake
	}
}

func (p *pump) close(timeout time.Duration) {
	p.mu.Lock()
	p.closed = true
	p.mu.Unlock()
	select {
	case p.wake <- struct{}{}:
	default:
	}
	select {
	case <-p.stopped:
	case <-time.After(timeout):
	}
}
