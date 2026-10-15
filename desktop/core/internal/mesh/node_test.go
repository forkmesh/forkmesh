package mesh

import (
	"net"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"
)

var loopback = net.IPv4(127, 0, 0, 1).To4()

func loopbackInterfaces() []Interface {
	return []Interface{{Name: "lo-test", Up: true, Running: true, Multicast: true, Loopback: true,
		Addrs: []IfAddr{{IP: loopback, Prefix: 8}}}}
}

type recorder struct {
	mu       sync.Mutex
	messages []ChatMessage
	queued   map[string]bool
	typing   []string
	wakes    [][]string
	blocked  int
	inbound  int
	system   []string
	drops    []DropInfo
	control  chan [3]any
}

func (r *recorder) hooks() Hooks {
	return Hooks{
		Message: func(m ChatMessage) { r.mu.Lock(); r.messages = append(r.messages, m); r.mu.Unlock() },
		Queued:  func(id string, q bool) { r.mu.Lock(); r.queued[id] = q; r.mu.Unlock() },
		Wake:    func(tokens []string) { r.mu.Lock(); r.wakes = append(r.wakes, tokens); r.mu.Unlock() },
		Typing: func(peerID, name string, active bool) {
			r.mu.Lock()
			if active {
				r.typing = append(r.typing, peerID)
			}
			r.mu.Unlock()
		},
		System:      func(text string) { r.mu.Lock(); r.system = append(r.system, text); r.mu.Unlock() },
		Blocked:     func() { r.mu.Lock(); r.blocked++; r.mu.Unlock() },
		Inbound:     func() { r.mu.Lock(); r.inbound++; r.mu.Unlock() },
		LinkDropped: func(d DropInfo) { r.mu.Lock(); r.drops = append(r.drops, d); r.mu.Unlock() },
	}
}

func (r *recorder) live() []ChatMessage {
	r.mu.Lock()
	defer r.mu.Unlock()
	out := []ChatMessage{}
	for _, m := range r.messages {
		if !m.Historical {
			out = append(out, m)
		}
	}
	return out
}

func (r *recorder) has(id string) bool {
	for _, m := range r.live() {
		if m.ID == id {
			return true
		}
	}
	return false
}

func freePort(t *testing.T) int {
	t.Helper()
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	port := listener.Addr().(*net.TCPAddr).Port
	listener.Close()
	return port
}

type testNode struct {
	*Node
	rec *recorder
	cfg Config
}

func quietConfig(t *testing.T, name, id string) Config {
	return Config{UserName: name, NodeName: name + "-desk", OwnerUser: name, NodeID: id, SealKey: randomBytes(32),
		Version: "9.9.9", DiscoveryPort: freePort(t), DisableMulticast: true, DisableSweep: true,
		Interfaces: loopbackInterfaces, Group: "127.0.0.1", BindHost: "127.0.0.1", BeaconInterval: time.Second}
}

func startNode(t *testing.T, cfg Config, extra func(*Hooks)) *testNode {
	t.Helper()
	rec := &recorder{queued: map[string]bool{}, control: make(chan [3]any, 4)}
	hooks := rec.hooks()
	if extra != nil {
		extra(&hooks)
	}
	node, err := NewNode(cfg, hooks)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(node.Close)
	if !node.Start() {
		t.Fatal("start")
	}
	return &testNode{Node: node, rec: rec, cfg: cfg}
}

func waitUntil(timeout time.Duration, cond func() bool) bool {
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		if cond() {
			return true
		}
		time.Sleep(10 * time.Millisecond)
	}
	return cond()
}

func (n *testNode) peers() int { return n.StateSnapshot().PeerCount }

func (n *testNode) port() int { return n.StateSnapshot().ListenPort }

func (n *testNode) line() string {
	line, _ := n.Diagnostics()
	return line
}

func (n *testNode) detail() string {
	_, detail := n.Diagnostics()
	return detail
}

func TestRoomOverLoopback(t *testing.T) {
	ada := startNode(t, quietConfig(t, "ada", "mesh-node-ada"), nil)
	grace := startNode(t, quietConfig(t, "grace", "mesh-node-grace"), nil)
	if ada.port() == 0 || grace.port() == 0 {
		t.Fatal("each device listens")
	}
	if !ada.ConnectToPeer(loopback, grace.port()) {
		t.Fatal("dial")
	}
	if !waitUntil(4*time.Second, func() bool { return ada.peers() == 1 && grace.peers() == 1 }) {
		t.Fatalf("not linked\n%s\n%s", ada.detail(), grace.detail())
	}
	roster := grace.StateSnapshot().Members
	if len(roster) != 2 || !roster[0].Self || roster[1].Name != "ada" || roster[1].NodeName != "ada-desk" || !roster[1].Online ||
		roster[1].Note != "On this network" || roster[1].Version != "9.9.9" {
		t.Fatalf("roster %+v", roster)
	}
	sent := ada.SendChat("", "anyone here?")
	if sent == "" {
		t.Fatal("send")
	}
	if !waitUntil(4*time.Second, func() bool { return len(grace.rec.live()) > 0 }) {
		t.Fatal("message did not cross")
	}
	got := grace.rec.live()[0]
	if got.Text != "anyone here?" || got.Conversation != "#mesh" || got.SenderName != "ada" || got.Self || got.ID != sent {
		t.Fatalf("received %+v", got)
	}
	if mine := ada.rec.live(); len(mine) != 1 || !mine[0].Self || mine[0].ID != sent {
		t.Fatalf("own echo %+v", mine)
	}
	if !waitUntil(2*time.Second, func() bool { return !ada.rec.queued[sent] && len(ada.StateSnapshot().Queued) == 0 }) {
		t.Fatal("the message left the queue")
	}
	for _, want := range []string{"dial 1 ok 1 fail 0", "links 1 keyed 1", "chat in 0 out 1 queued 0", "tcp ", "lo-test"} {
		if !strings.Contains(ada.line(), want) && want != "lo-test" {
			t.Fatalf("ada line lacks %q: %s", want, ada.line())
		}
	}
	for _, want := range []string{"fail 0 · in 1", "chat in 1 out 0"} {
		if !strings.Contains(grace.line(), want) {
			t.Fatalf("grace line lacks %q: %s", want, grace.line())
		}
	}
	if !strings.Contains(grace.detail(), "peer ada") {
		t.Fatalf("detail %s", grace.detail())
	}

	ada.SendTyping(true)
	if !waitUntil(4*time.Second, func() bool {
		grace.rec.mu.Lock()
		defer grace.rec.mu.Unlock()
		return len(grace.rec.typing) > 0 && grace.rec.typing[0] == "mesh-node-ada"
	}) {
		t.Fatal("typing did not show")
	}

	linus := startNode(t, quietConfig(t, "linus", "mesh-node-linus"), nil)
	if !linus.ConnectToPeer(loopback, grace.port()) {
		t.Fatal("linus dial")
	}
	if !waitUntil(4*time.Second, func() bool { return len(linus.rec.live()) > 0 }) || linus.rec.live()[0].Text != "anyone here?" {
		t.Fatal("a late joiner is backfilled")
	}
	second := ada.SendChat("", "second")
	if !waitUntil(4*time.Second, func() bool { return linus.rec.has(second) }) {
		t.Fatal("relayed to a device with no direct link")
	}
	if !waitUntil(time.Second, func() bool { return len(grace.rec.live()) == 2 }) || len(ada.rec.live()) != 2 {
		t.Fatal("exactly once everywhere")
	}
	ada.Close()
	if !waitUntil(4*time.Second, func() bool {
		for _, member := range grace.StateSnapshot().Members {
			if member.ID == "mesh-node-ada" {
				return false
			}
		}
		return grace.peers() == 1
	}) {
		t.Fatal("a device that leaves drops out")
	}
}

func TestRefusesOffNetworkDials(t *testing.T) {
	node := startNode(t, quietConfig(t, "ada", "mesh-policy"), nil)
	if node.ConnectToPeer(net.ParseIP("93.184.216.34"), 48732) || node.ConnectToPeer(loopback, 0) {
		t.Fatal("dial refused")
	}
}

func TestQueuedUntilJoinAndRestart(t *testing.T) {
	dir := t.TempDir()
	history := filepath.Join(dir, "mesh.json")
	cfg := quietConfig(t, "ada", "mesh-queue-ada")
	cfg.HistoryFile = history
	alone := startNode(t, cfg, nil)
	sent := alone.SendChat("", "anyone out there?")
	if sent == "" || !alone.rec.queued[sent] {
		t.Fatal("queued with nobody linked")
	}
	alone.Close()
	raw, err := os.ReadFile(history)
	if err != nil || !strings.Contains(string(raw), sent) || !strings.Contains(string(raw), `"queued":["`+sent+`"]`) {
		t.Fatalf("history file %s %v", raw, err)
	}
	info, _ := os.Stat(history)
	if info.Mode().Perm() != 0o600 {
		t.Fatalf("history mode %v", info.Mode())
	}
	cfg.DiscoveryPort = freePort(t)
	ada := startNode(t, cfg, nil)
	state := ada.StateSnapshot()
	if len(state.Queued) != 1 || state.Queued[0] != sent {
		t.Fatalf("still queued after restart: %v", state.Queued)
	}
	if past := ada.History(); len(past) != 1 || past[0].ID != sent || !past[0].Historical || !past[0].Self {
		t.Fatalf("history %+v", past)
	}
	grace := startNode(t, quietConfig(t, "grace", "mesh-queue-grace"), nil)
	carol := startNode(t, quietConfig(t, "carol", "mesh-queue-carol"), nil)
	ada.ConnectToPeer(loopback, grace.port())
	if !waitUntil(4*time.Second, func() bool { return len(ada.StateSnapshot().Queued) == 0 && grace.rec.has(sent) }) {
		t.Fatal("the first device to join takes the queued message")
	}
	carol.ConnectToPeer(loopback, grace.port())
	if !waitUntil(4*time.Second, func() bool { return carol.rec.has(sent) }) {
		t.Fatal("a joiner gets it from whichever device it links to")
	}
}

func TestWakeTargetsFromHistory(t *testing.T) {
	dir := t.TempDir()
	history := filepath.Join(dir, "mesh.json")
	now := nowMs()
	phone := "mw1." + strings.Repeat("a", 64) + "." + itoa(now+3600000) + ".sig"
	stale := "mw1." + strings.Repeat("c", 64) + "." + itoa(now-1000) + ".sig"
	entry := func(id, sender, wake string) Object {
		return Object{"type": "chat", "id": id, "senderId": sender, "sender": sender, "conversation": Conversation,
			"ts": float64(now - 60000), "text": "hi", "wake": wake}
	}
	if err := writeHistory(history, []Object{entry("p-1", "phone", phone), entry("o-1", "old", stale)}, nil); err != nil {
		t.Fatal(err)
	}
	cfg := quietConfig(t, "desk", "mesh-wake-desk")
	cfg.HistoryFile = history
	desk := startNode(t, cfg, nil)
	if targets := desk.StateSnapshot().WakeTargets; len(targets) != 1 || targets[0] != phone {
		t.Fatalf("targets %v", targets)
	}
	if desk.SendChat("", "are you there?") == "" {
		t.Fatal("send")
	}
	desk.rec.mu.Lock()
	defer desk.rec.mu.Unlock()
	if len(desk.rec.wakes) != 1 || len(desk.rec.wakes[0]) != 1 || desk.rec.wakes[0][0] != phone {
		t.Fatalf("wakes %v", desk.rec.wakes)
	}
}

func itoa(v int64) string { return strconv.FormatInt(v, 10) }

func TestReturningDeviceReplacesStaleLink(t *testing.T) {
	graceCfg := quietConfig(t, "grace", "mesh-return-grace")
	graceCfg.LinkReplaceAfter = 200 * time.Millisecond
	grace := startNode(t, graceCfg, nil)
	adaCfg := quietConfig(t, "ada", "mesh-return-ada")
	ada := startNode(t, adaCfg, nil)
	grace.ConnectToPeer(loopback, ada.port())
	if !waitUntil(4*time.Second, func() bool { return grace.peers() == 1 && ada.peers() == 1 }) {
		t.Fatal("first link")
	}
	time.Sleep(300 * time.Millisecond)
	backCfg := adaCfg
	backCfg.DiscoveryPort = freePort(t)
	back := startNode(t, backCfg, nil)
	back.ConnectToPeer(loopback, grace.port())
	if !waitUntil(4*time.Second, func() bool { return back.peers() == 1 && ada.peers() == 0 }) {
		t.Fatalf("the room keeps the new link\n%s\n%s", grace.detail(), ada.detail())
	}
	if !strings.Contains(grace.detail(), "replaced") {
		t.Fatalf("grace replaced the stale link: %s", grace.detail())
	}
	ada.Close()
	said := grace.SendChat("", "welcome back")
	if !waitUntil(4*time.Second, func() bool { return back.rec.has(said) }) {
		t.Fatal("messages reach the returning device")
	}
}

func TestCrossingDialKeepsOneLink(t *testing.T) {
	high := startNode(t, quietConfig(t, "high", "mesh-cross-z"), nil)
	low := startNode(t, quietConfig(t, "low", "mesh-cross-a"), nil)
	high.ConnectToPeer(loopback, low.port())
	if !waitUntil(4*time.Second, func() bool { return high.peers() == 1 && low.peers() == 1 }) {
		t.Fatal("linked")
	}
	low.ConnectToPeer(loopback, high.port())
	links := func(n *testNode) (int, bool) {
		result := n.Call(func() any {
			out := []bool{}
			for _, l := range n.links {
				out = append(out, l.outbound)
			}
			return out
		}).([]bool)
		return len(result), len(result) == 1 && result[0]
	}
	if !waitUntil(4*time.Second, func() bool {
		hc, _ := links(high)
		lc, _ := links(low)
		return hc == 1 && lc == 1 && (strings.Contains(high.detail(), "duplicate link") || strings.Contains(low.detail(), "duplicate link"))
	}) {
		t.Fatalf("one link survives\n%s\n%s", high.detail(), low.detail())
	}
	if _, outbound := links(high); !outbound {
		t.Fatal("the link the higher id opened survives")
	}
	if _, outbound := links(low); outbound {
		t.Fatal("low keeps the inbound one")
	}
}

func beaconDatagramFor(t *testing.T, beacon Beacon) []byte {
	t.Helper()
	crypto, err := NewRoomCrypto(RoomName)
	if err != nil {
		t.Fatal(err)
	}
	return marshal(crypto.Encrypt(BeaconObject(beacon)))
}

func TestDiscoveryByBeacon(t *testing.T) {
	firstCfg := quietConfig(t, "first", "mesh-discover-1")
	firstCfg.DisableMulticast = false
	firstCfg.BeaconInterval = 200 * time.Millisecond
	first := startNode(t, firstCfg, nil)
	second := startNode(t, quietConfig(t, "second", "mesh-discover-2"), nil)
	if !strings.Contains(first.line(), "udp "+itoa(int64(firstCfg.DiscoveryPort))+" bound") {
		t.Fatalf("discovery bound: %s", first.line())
	}
	wire, err := net.DialUDP("udp4", nil, &net.UDPAddr{IP: loopback, Port: firstCfg.DiscoveryPort})
	if err != nil {
		t.Fatal(err)
	}
	defer wire.Close()
	wire.Write([]byte("not-a-beacon"))
	otherKey, _ := RoomKey("some-other-room", appRoomKey)
	foreign := (&RoomCrypto{key: otherKey}).Encrypt(BeaconObject(Beacon{NodeID: "outsider", Port: second.port(), TimestampMs: nowMs()}))
	wire.Write(marshal(foreign))
	time.Sleep(100 * time.Millisecond)
	if first.peers() != 0 || len(first.StateSnapshot().Members) != 1 {
		t.Fatal("an undecryptable datagram is not a peer")
	}
	if !waitUntil(2*time.Second, func() bool { return strings.Contains(first.line(), "bad 2") }) {
		t.Fatalf("counted as bad: %s", first.line())
	}
	if !waitUntil(2*time.Second, func() bool { return !strings.Contains(first.line(), "own 0 ") }) {
		t.Fatalf("own beacons loop back: %s", first.line())
	}
	wire.Write(beaconDatagramFor(t, Beacon{NodeID: "mesh-discover-2", Name: "second", NodeName: "two", Port: second.port(),
		Platform: "linux", TimestampMs: nowMs()}))
	if !waitUntil(4*time.Second, func() bool { return first.peers() == 1 && second.peers() == 1 }) {
		t.Fatalf("a beacon is all it takes\n%s", first.detail())
	}
	if roster := first.StateSnapshot().Members; len(roster) != 2 || roster[1].Name != "second" {
		t.Fatalf("roster %+v", roster)
	}
}

func TestUnicastAnswerAndHigherIDDials(t *testing.T) {
	highCfg := quietConfig(t, "high", "mesh-unicast-z")
	highCfg.DisableMulticast = false
	highCfg.BeaconInterval = 200 * time.Millisecond
	high := startNode(t, highCfg, nil)
	low := startNode(t, quietConfig(t, "low", "mesh-unicast-a"), nil)
	wire, err := net.ListenUDP("udp4", &net.UDPAddr{IP: loopback})
	if err != nil {
		t.Fatal(err)
	}
	defer wire.Close()
	crypto, _ := NewRoomCrypto(RoomName)
	send := func(reply bool) {
		datagram := beaconDatagramFor(t, Beacon{NodeID: "mesh-unicast-a", Name: "low", Port: low.port(), TimestampMs: nowMs(), Reply: reply})
		wire.WriteToUDP(datagram, &net.UDPAddr{IP: loopback, Port: highCfg.DiscoveryPort})
	}
	answer := func(timeout time.Duration) (Beacon, bool) {
		buffer := make([]byte, 65535)
		wire.SetReadDeadline(time.Now().Add(timeout))
		for {
			size, _, err := wire.ReadFromUDP(buffer)
			if err != nil {
				return Beacon{}, false
			}
			envelope, ok := parseObject(buffer[:size])
			if !ok {
				continue
			}
			if beacon, ok := ParseBeacon(crypto.Decrypt(envelope)); ok {
				return beacon, true
			}
		}
	}
	send(false)
	got, ok := answer(2 * time.Second)
	if !ok || !got.Reply || got.NodeID != "mesh-unicast-z" || got.Port != high.port() {
		t.Fatalf("a beacon from an unlinked device is answered by unicast: %+v %v", got, ok)
	}
	time.Sleep(50 * time.Millisecond)
	if high.peers() != 0 || low.peers() != 0 {
		t.Fatal("the higher id leaves the first dial to the lower id")
	}
	time.Sleep(100 * time.Millisecond)
	send(true)
	if _, ok := answer(150 * time.Millisecond); ok {
		t.Fatal("a reply beacon is never answered")
	}
	if !waitUntil(4*time.Second, func() bool { return high.peers() == 1 && low.peers() == 1 }) {
		t.Fatalf("the higher id dials a device that stays unlinked\n%s", high.detail())
	}
}

func TestLinksBlockedIsDetected(t *testing.T) {
	cfg := quietConfig(t, "first", "mesh-firewall-1")
	cfg.DisableMulticast = false
	cfg.BeaconInterval = 100 * time.Millisecond
	first := startNode(t, cfg, nil)
	dead := freePort(t)
	wire, err := net.DialUDP("udp4", nil, &net.UDPAddr{IP: loopback, Port: cfg.DiscoveryPort})
	if err != nil {
		t.Fatal(err)
	}
	defer wire.Close()
	announce := func() {
		wire.Write(beaconDatagramFor(t, Beacon{NodeID: "mesh-firewall-ghost", Name: "ghost", Port: dead, TimestampMs: nowMs()}))
	}
	announce()
	time.Sleep(120 * time.Millisecond)
	if first.StateSnapshot().LinksBlocked {
		t.Fatal("one unanswered dial is not yet a firewall")
	}
	if !waitUntil(4*time.Second, func() bool {
		announce()
		return first.StateSnapshot().LinksBlocked
	}) {
		t.Fatalf("a device heard but never linked is a blocked link\n%s", first.detail())
	}
	first.rec.mu.Lock()
	blocked := first.rec.blocked
	first.rec.mu.Unlock()
	if blocked != 1 || !strings.Contains(first.line(), "LINKS BLOCKED") {
		t.Fatalf("signalled once: %d %s", blocked, first.line())
	}
	if !waitUntil(4*time.Second, func() bool { return !first.StateSnapshot().LinksBlocked }) {
		t.Fatal("the episode ends once the device goes quiet")
	}
}

func TestControlHelloIsHandedOff(t *testing.T) {
	cfg := quietConfig(t, "desk", "mesh-control-desk")
	cfg.Control = true
	adopted := make(chan [3]any, 1)
	node := startNode(t, cfg, func(h *Hooks) {
		h.Control = func(conn net.Conn, hello, rest []byte) bool {
			adopted <- [3]any{conn, hello, rest}
			return true
		}
	})
	if advertised, _ := node.Call(func() any { return node.advertisesControl() }).(bool); !advertised {
		t.Fatal("the beacon offers the control channel")
	}
	client, err := net.Dial("tcp", net.JoinHostPort("127.0.0.1", itoa(int64(node.port()))))
	if err != nil {
		t.Fatal(err)
	}
	defer client.Close()
	hello := []byte(`{"t":"fm-control-hello","v":1,"key":"phone"}`)
	client.Write(append(FrameBytes(hello), []byte("tail")...))
	var got [3]any
	select {
	case got = <-adopted:
	case <-time.After(4 * time.Second):
		t.Fatal("not handed off")
	}
	conn := got[0].(net.Conn)
	defer conn.Close()
	if string(got[1].([]byte)) != string(hello) || string(got[2].([]byte)) != "tail" {
		t.Fatalf("hello %q rest %q", got[1], got[2])
	}
	client.SetReadDeadline(time.Now().Add(2 * time.Second))
	var buffer []byte
	chunk := make([]byte, 4096)
	for {
		count, err := client.Read(chunk)
		buffer = append(buffer, chunk[:count]...)
		if payload, status := TakeFrame(&buffer); status == 1 {
			envelope, _ := parseObject(payload)
			crypto, _ := NewRoomCrypto(RoomName)
			if !IsKeyFrame(crypto.Decrypt(envelope)) {
				t.Fatal("the mesh key frame went out before the hand-off, as on the desktop")
			}
			break
		}
		if err != nil {
			t.Fatalf("no key frame: %v", err)
		}
	}
	client.Write([]byte("after"))
	conn.SetReadDeadline(time.Now().Add(2 * time.Second))
	after := make([]byte, 5)
	if _, err := conn.Read(after); err != nil || string(after) != "after" {
		t.Fatalf("the mesh stopped reading the adopted socket: %q %v", after, err)
	}
	if node.StateSnapshot().PeerCount != 0 || !strings.Contains(node.line(), "links 0") {
		t.Fatalf("the room never saw it: %s", node.line())
	}

	node.SetControl(false)
	refused, err := net.Dial("tcp", net.JoinHostPort("127.0.0.1", itoa(int64(node.port()))))
	if err != nil {
		t.Fatal(err)
	}
	defer refused.Close()
	refused.Write(FrameBytes(hello))
	refused.SetReadDeadline(time.Now().Add(2 * time.Second))
	for {
		if _, err := refused.Read(chunk); err != nil {
			break
		}
	}
	select {
	case <-adopted:
		t.Fatal("no control channel without a handler")
	default:
	}
}

func TestBluetoothBridgeCarriesTheRoom(t *testing.T) {
	var phone, laptop *testNode
	var mu sync.Mutex
	closed := map[string]string{}
	pipe := func(name string, other func() *testNode) func(h *Hooks) {
		return func(h *Hooks) {
			h.BridgeWrite = func(link int, data []byte) { other().BridgeData(link, data) }
			h.BridgeClose = func(link int, reason string) {
				mu.Lock()
				closed[name] = reason
				mu.Unlock()
				other().BridgeClosed(link, "")
			}
		}
	}
	phoneCfg := quietConfig(t, "phone", "mesh-bt-a")
	laptopCfg := quietConfig(t, "laptop", "mesh-bt-z")
	phone = startNode(t, phoneCfg, pipe("phone", func() *testNode { return laptop }))
	laptop = startNode(t, laptopCfg, pipe("laptop", func() *testNode { return phone }))
	if !phone.BridgeOpen(1, true) || !laptop.BridgeOpen(1, false) {
		t.Fatal("bridge open")
	}
	if !waitUntil(4*time.Second, func() bool { return phone.peers() == 1 && laptop.peers() == 1 }) {
		t.Fatalf("a Bluetooth link puts both in the room\n%s", phone.detail())
	}
	if members := laptop.StateSnapshot().Members; members[1].Note != "Over Bluetooth" {
		t.Fatalf("note %+v", members)
	}
	said := phone.SendChat("", "no wifi here")
	if !waitUntil(4*time.Second, func() bool { return laptop.rec.has(said) }) {
		t.Fatal("the message crosses Bluetooth")
	}
	laptop.ConnectToPeer(loopback, phone.port())
	if !waitUntil(4*time.Second, func() bool {
		mu.Lock()
		defer mu.Unlock()
		return closed["phone"] == reasonDuplicate || closed["laptop"] == reasonDuplicate
	}) {
		t.Fatalf("the Bluetooth duplicate gives way to TCP\n%s\n%s", phone.detail(), laptop.detail())
	}
	if !waitUntil(4*time.Second, func() bool {
		return phone.peers() == 1 && laptop.peers() == 1 && laptop.StateSnapshot().Members[1].Note == "On this network"
	}) {
		t.Fatal("still one room over TCP")
	}
	phone.rec.mu.Lock()
	var duplicate *DropInfo
	for i := range phone.rec.drops {
		if phone.rec.drops[i].Reason == reasonDuplicate {
			duplicate = &phone.rec.drops[i]
		}
	}
	phone.rec.mu.Unlock()
	laptop.rec.mu.Lock()
	for i := range laptop.rec.drops {
		if laptop.rec.drops[i].Reason == reasonDuplicate {
			duplicate = &laptop.rec.drops[i]
		}
	}
	laptop.rec.mu.Unlock()
	if duplicate == nil || !duplicate.Bluetooth || !duplicate.Superseded || duplicate.Bridge != 1 {
		t.Fatalf("drop info %+v", duplicate)
	}
}
