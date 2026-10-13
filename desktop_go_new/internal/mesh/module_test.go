package mesh

import (
	"bytes"
	"context"
	"encoding/base64"
	"encoding/json"
	"net"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"forkmesh.com/desktop-core/internal/coretest"
	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/identity"
	"forkmesh.com/desktop-core/internal/rpc"
	"forkmesh.com/desktop-core/internal/vault"
)

type fakeControl struct {
	adopted chan []byte
}

func (f *fakeControl) Name() string                                       { return "control" }
func (f *fakeControl) Start(ctx context.Context, core *daemon.Core) error { return nil }
func (f *fakeControl) Adopt(conn net.Conn, hello, rest []byte) {
	conn.Close()
	f.adopted <- hello
}

type meshCore struct {
	core   *daemon.Core
	module *Module
	ui     *coretest.Peer
}

func newMeshCore(t *testing.T, extra ...daemon.Module) *meshCore {
	t.Helper()
	core := coretest.New(t)
	module := New()
	module.Options = Options{DiscoveryPort: freePort(t), DisableMulticast: true, DisableSweep: true,
		Interfaces: loopbackInterfaces, Group: "127.0.0.1", BindHost: "127.0.0.1"}
	coretest.Start(t, core, append(extra, module)...)
	ui := coretest.NewPeer("ui:1", rpc.UI)
	core.Hub.Add(ui)
	return &meshCore{core: core, module: module, ui: ui}
}

func (m *meshCore) call(t *testing.T, method string, params any) map[string]any {
	t.Helper()
	var out map[string]any
	coretest.MustCall(t, m.core, m.ui, method, params, &out)
	return out
}

func (m *meshCore) state(t *testing.T) State {
	t.Helper()
	var out struct {
		State State `json:"state"`
	}
	coretest.MustCall(t, m.core, m.ui, "core.mesh.state", nil, &out)
	return out.State
}

func nextEvent(t *testing.T, peer *coretest.Peer, name string, match func(json.RawMessage) bool) json.RawMessage {
	t.Helper()
	deadline := time.After(5 * time.Second)
	for {
		select {
		case event := <-peer.Events:
			if event.Name == name && (match == nil || match(event.Data)) {
				return event.Data
			}
		case <-deadline:
			t.Fatalf("no %s event", name)
			return nil
		}
	}
}

func TestTwoCoresShareTheRoom(t *testing.T) {
	control := &fakeControl{adopted: make(chan []byte, 1)}
	a := newMeshCore(t, control)
	b := newMeshCore(t)
	keyA := randomBytes(32)
	keyB := randomBytes(32)
	settings := "[chat]\ne2ee\\x25519PrivateV2=@ByteArray(" + base64.StdEncoding.EncodeToString(keyB) + ")\n"
	if err := os.MkdirAll(filepath.Dir(b.core.Layout.SettingsFile()), 0o700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(b.core.Layout.SettingsFile(), []byte(settings), 0o600); err != nil {
		t.Fatal(err)
	}

	phone := coretest.NewPeer("lan:1", rpc.LAN)
	if _, err := coretest.Call(t, a.core, phone, "core.mesh.start", map[string]any{"nodeId": "x"}); rpc.Code(err) != "forbidden" {
		t.Fatalf("a phone cannot drive the mesh: %v", err)
	}
	if _, err := coretest.Call(t, a.core, a.ui, "core.mesh.sendChat", map[string]any{"text": "x"}); rpc.Code(err) != "mesh_not_running" {
		t.Fatalf("no mesh yet: %v", err)
	}

	startA := a.call(t, "core.mesh.start", map[string]any{"nodeId": "core-node-a", "userName": "ada", "nodeName": "workshop",
		"ownerUser": "ada", "version": "9.9.9", "sealKey": base64.StdEncoding.EncodeToString(keyA), "control": true})
	if startA["attached"] != false {
		t.Fatalf("fresh start %v", startA)
	}
	nextEvent(t, a.ui, "core.mesh.system", func(raw json.RawMessage) bool { return strings.Contains(string(raw), "is open over this network") })
	b.call(t, "core.mesh.start", map[string]any{"nodeId": "core-node-b", "userName": "grace", "nodeName": "studio"})
	stateB := b.state(t)
	if stateB.SealKeyID != KeyIDFor(mustSeal(t, RoomName, keyB).public) || !stateB.Running || stateB.ListenPort == 0 {
		t.Fatalf("b took its key from the desktop's settings: %+v", stateB)
	}

	connect := a.call(t, "core.mesh.connect", map[string]any{"address": "127.0.0.1", "port": stateB.ListenPort})
	if connect["ok"] != true {
		t.Fatalf("connect %v", connect)
	}
	nextEvent(t, b.ui, "core.mesh.peers", func(raw json.RawMessage) bool { return strings.Contains(string(raw), `"peerCount":1`) })
	if !waitUntil(4*time.Second, func() bool { return a.state(t).PeerCount == 1 }) {
		t.Fatal("a links")
	}

	sent := a.call(t, "core.mesh.sendChat", map[string]any{"id": "desktop-id-1", "text": "  hello from a core  "})
	if sent["id"] != "desktop-id-1" {
		t.Fatalf("the desktop's id is kept: %v", sent)
	}
	var arrived struct {
		Message ChatMessage `json:"message"`
		State   State       `json:"state"`
	}
	raw := nextEvent(t, b.ui, "core.mesh.message", nil)
	if err := json.Unmarshal(raw, &arrived); err != nil || arrived.Message.Text != "hello from a core" ||
		arrived.Message.SenderName != "ada" || arrived.Message.Self || arrived.Message.ID != "desktop-id-1" {
		t.Fatalf("b received %s", raw)
	}
	own := nextEvent(t, a.ui, "core.mesh.message", nil)
	if !strings.Contains(string(own), `"self":true`) {
		t.Fatalf("a sees its own message: %s", own)
	}
	file := a.call(t, "core.mesh.sendFile", map[string]any{"fileName": "../notes.txt", "mimeType": "text/plain",
		"data": base64.StdEncoding.EncodeToString([]byte("file body"))})
	raw = nextEvent(t, b.ui, "core.mesh.message", func(raw json.RawMessage) bool { return strings.Contains(string(raw), "notes.txt") })
	arrived = struct {
		Message ChatMessage `json:"message"`
		State   State       `json:"state"`
	}{}
	if json.Unmarshal(raw, &arrived) != nil || string(arrived.Message.FileData) != "file body" || arrived.Message.ID != file["id"] {
		t.Fatalf("file %s", raw)
	}

	a.call(t, "core.mesh.typing", map[string]any{"active": true})
	nextEvent(t, b.ui, "core.mesh.typing", func(raw json.RawMessage) bool {
		return strings.Contains(string(raw), `"peerId":"core-node-a"`) && strings.Contains(string(raw), `"active":true`)
	})

	again := a.call(t, "core.mesh.start", map[string]any{"nodeId": "core-node-a", "userName": "ada", "nodeName": "workshop",
		"ownerUser": "ada", "sealKey": base64.StdEncoding.EncodeToString(keyA), "control": true})
	history, _ := json.Marshal(again["history"])
	if again["attached"] != true || !strings.Contains(string(history), "desktop-id-1") || !strings.Contains(string(history), `"historical":true`) ||
		!strings.Contains(again["note"].(string), "open over this network") {
		t.Fatalf("a restarted desktop re-attaches to the running room: %v", again)
	}

	client, err := net.Dial("tcp", net.JoinHostPort("127.0.0.1", itoa(int64(a.state(t).ListenPort))))
	if err != nil {
		t.Fatal(err)
	}
	defer client.Close()
	hello := []byte(`{"t":"fm-control-hello","v":1}`)
	client.Write(FrameBytes(hello))
	select {
	case got := <-control.adopted:
		if string(got) != string(hello) {
			t.Fatalf("adopted %q", got)
		}
	case <-time.After(4 * time.Second):
		t.Fatal("a control hello goes straight to the control module")
	}

	diagnostics := a.call(t, "core.mesh.diagnostics", map[string]any{"bluetooth": map[string]any{"active": true, "seen": 2, "dials": 1, "opened": 1}})
	if !strings.Contains(diagnostics["line"].(string), "bt on") || !strings.Contains(diagnostics["detail"].(string), "ble: seen 2 · dial 1 · open 1") ||
		!strings.Contains(diagnostics["line"].(string), "links 1 keyed 1") {
		t.Fatalf("diagnostics %v", diagnostics)
	}

	open := a.call(t, "core.mesh.link.open", map[string]any{"link": 7, "outbound": true})
	if open["ok"] != true {
		t.Fatalf("bridge %v", open)
	}
	raw = nextEvent(t, a.ui, "core.mesh.link.write", func(raw json.RawMessage) bool { return strings.Contains(string(raw), `"link":7`) })
	var write struct {
		Data string `json:"data"`
	}
	json.Unmarshal(raw, &write)
	frame := qtBase64(write.Data)
	payload, status := TakeFrame(&frame)
	envelope, _ := parseObject(payload)
	crypto, _ := NewRoomCrypto(RoomName)
	if status != 1 || !IsKeyFrame(crypto.Decrypt(envelope)) {
		t.Fatal("a Bluetooth link opens with the key frame, written through the desktop")
	}
	a.call(t, "core.mesh.link.closed", map[string]any{"link": 7, "reason": "Bluetooth off"})
	nextEvent(t, a.ui, "core.mesh.link.dropped", func(raw json.RawMessage) bool { return strings.Contains(string(raw), "Bluetooth off") })

	peersFile := a.module.sealPeersFile("core-node-a")
	if !waitUntil(4*time.Second, func() bool {
		raw, err := os.ReadFile(peersFile)
		return err == nil && strings.Contains(string(raw), stateB.SealKeyID)
	}) {
		t.Fatal("learned seal keys are kept beside the history")
	}
	a.call(t, "core.mesh.stop", nil)
	historyFile := a.module.defaultHistoryFile("core-node-a")
	if raw, err := os.ReadFile(historyFile); err != nil || !strings.Contains(string(raw), "desktop-id-1") {
		t.Fatalf("history saved at stop: %v", err)
	}
	if !strings.HasPrefix(historyFile, filepath.Join(a.core.Layout.Data, "local_mesh", "history-")) {
		t.Fatalf("history path %s", historyFile)
	}
	nextEvent(t, b.ui, "core.mesh.peers", func(raw json.RawMessage) bool { return strings.Contains(string(raw), `"peerCount":0`) })
}

func TestSealKeyRequired(t *testing.T) {
	a := newMeshCore(t)
	if _, err := coretest.Call(t, a.core, a.ui, "core.mesh.start", map[string]any{"nodeId": "lonely"}); rpc.Code(err) != "seal_key_unavailable" {
		t.Fatalf("no key, no room: %v", err)
	}
	if _, err := coretest.Call(t, a.core, a.ui, "core.mesh.start", map[string]any{"sealKey": base64.StdEncoding.EncodeToString(randomBytes(32))}); rpc.Code(err) != "bad_params" {
		t.Fatalf("no node id: %v", err)
	}
}

func TestSealKeyComesFromTheVault(t *testing.T) {
	id, err := identity.FromSeed(bytes.Repeat([]byte{4}, 32))
	if err != nil {
		t.Fatal(err)
	}
	store := &vault.Module{Identities: func() []vault.Signer { return []vault.Signer{id} }, Interval: time.Hour}
	c := newMeshCore(t, store)
	key := randomBytes(32)
	settings := "[chat]\ne2ee\\x25519PrivateV2=" + vault.Marker + "\n"
	if err := os.MkdirAll(filepath.Dir(c.core.Layout.SettingsFile()), 0o700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(c.core.Layout.SettingsFile(), []byte(settings), 0o600); err != nil {
		t.Fatal(err)
	}
	if err := os.Chtimes(c.core.Layout.SettingsFile(), time.Now().Add(-time.Hour), time.Now().Add(-time.Hour)); err != nil {
		t.Fatal(err)
	}
	if _, err := store.Apply(vault.DesktopNamespace, map[string]string{SealKeySetting: base64.StdEncoding.EncodeToString(key)}, nil, false); err != nil {
		t.Fatal(err)
	}
	c.call(t, "core.mesh.start", map[string]any{"nodeId": "core-node-v", "userName": "ada", "sealKey": vault.Marker})
	if got := c.state(t).SealKeyID; got != KeyIDFor(mustSeal(t, RoomName, key).public) {
		t.Fatalf("seal key id %q: the marker in ForkMesh.conf must send the core to the vault", got)
	}
}
