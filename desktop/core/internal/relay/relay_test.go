package relay

import (
	"bufio"
	"context"
	"crypto/ed25519"
	"crypto/rand"
	"crypto/sha1"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/base64"
	"encoding/binary"
	"encoding/json"
	"encoding/pem"
	"errors"
	"io"
	"log"
	"math/big"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"forkmesh.com/desktop-core/internal/control"
	"forkmesh.com/desktop-core/internal/coretest"
	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/identity"
	"forkmesh.com/desktop-core/internal/rpc"
)

const wait = 5 * time.Second

func fastTimings() Timings {
	return Timings{Ping: 100 * time.Millisecond, AppPing: 250 * time.Millisecond, StaleRx: 3 * time.Second,
		Connect: 2 * time.Second, ReconnectBase: 20 * time.Millisecond, ReconnectMax: 200 * time.Millisecond,
		Jitter: time.Millisecond}
}

type received struct {
	opcode  int
	payload []byte
	masked  bool
	at      time.Time
}

type serverConn struct {
	conn    net.Conn
	request *http.Request
	frames  chan received
	writeMu sync.Mutex
}

func (s *serverConn) send(opcode int, payload []byte) error {
	s.writeMu.Lock()
	defer s.writeMu.Unlock()
	header := []byte{byte(0x80 | opcode)}
	switch {
	case len(payload) < 126:
		header = append(header, byte(len(payload)))
	case len(payload) < 65536:
		header = append(header, 126, byte(len(payload)>>8), byte(len(payload)))
	default:
		extended := make([]byte, 8)
		binary.BigEndian.PutUint64(extended, uint64(len(payload)))
		header = append(append(header, 127), extended...)
	}
	_, err := s.conn.Write(append(header, payload...))
	return err
}

func (s *serverConn) sendJSON(t *testing.T, value any) {
	t.Helper()
	raw, _ := json.Marshal(value)
	if err := s.send(opText, raw); err != nil {
		t.Fatalf("server write: %v", err)
	}
}

func (s *serverConn) next(t *testing.T, opcode int) received {
	t.Helper()
	deadline := time.After(wait)
	for {
		select {
		case frame, ok := <-s.frames:
			if !ok {
				t.Fatalf("connection closed while waiting for opcode %d", opcode)
			}
			if frame.opcode == opcode {
				if !frame.masked {
					t.Fatalf("client frame not masked")
				}
				return frame
			}
		case <-deadline:
			t.Fatalf("no frame with opcode %d", opcode)
		}
	}
}

func (s *serverConn) nextText(t *testing.T, kind string) (map[string]any, received) {
	t.Helper()
	for {
		frame := s.next(t, opText)
		var value map[string]any
		if json.Unmarshal(frame.payload, &value) != nil {
			t.Fatalf("bad client JSON %q", frame.payload)
		}
		if value["type"] == kind {
			return value, frame
		}
	}
}

type testRelay struct {
	t         *testing.T
	listener  net.Listener
	publicKey string
	hello     map[string]any
	refuse    atomic.Int32
	accepted  chan *serverConn
	count     atomic.Int32
	scheme    string
}

func newRelay(t *testing.T, publicKey string, hello map[string]any) *testRelay {
	t.Helper()
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	r := &testRelay{t: t, listener: listener, publicKey: publicKey, hello: hello, accepted: make(chan *serverConn, 16), scheme: "ws"}
	t.Cleanup(func() { _ = listener.Close() })
	go r.serve()
	return r
}

func (r *testRelay) endpoint() string {
	return r.scheme + "://" + r.listener.Addr().String() + "/api/nodes/events"
}

func newTLSRelay(t *testing.T, hello map[string]any) (*testRelay, *x509.CertPool) {
	t.Helper()
	public, private, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	template := &x509.Certificate{SerialNumber: big.NewInt(1), Subject: pkix.Name{CommonName: "relay"},
		NotBefore: time.Now().Add(-time.Hour), NotAfter: time.Now().Add(time.Hour),
		IPAddresses: []net.IP{net.ParseIP("127.0.0.1")}, KeyUsage: x509.KeyUsageDigitalSignature,
		ExtKeyUsage: []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth}}
	der, err := x509.CreateCertificate(rand.Reader, template, template, public, private)
	if err != nil {
		t.Fatal(err)
	}
	cert, err := x509.ParseCertificate(der)
	if err != nil {
		t.Fatal(err)
	}
	pool := x509.NewCertPool()
	pool.AddCert(cert)
	inner, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	listener := tls.NewListener(inner, &tls.Config{Certificates: []tls.Certificate{{Certificate: [][]byte{der}, PrivateKey: private}}})
	r := &testRelay{t: t, listener: listener, hello: hello, accepted: make(chan *serverConn, 16), scheme: "wss"}
	t.Cleanup(func() { _ = listener.Close() })
	go r.serve()
	return r, pool
}

func (r *testRelay) serve() {
	for {
		conn, err := r.listener.Accept()
		if err != nil {
			return
		}
		go r.handle(conn)
	}
}

func (r *testRelay) handle(conn net.Conn) {
	reader := bufio.NewReader(conn)
	request, err := http.ReadRequest(reader)
	if err != nil {
		_ = conn.Close()
		return
	}
	query := request.URL.Query()
	authorized := request.URL.Path == "/api/nodes/events" &&
		(r.publicKey == "" || identity.Verify(r.publicKey, query.Get("sig"), Canonical(query.Get("owner"), query.Get("ts"))))
	if r.refuse.Load() > 0 {
		r.refuse.Add(-1)
		authorized = false
	}
	if !authorized || !strings.EqualFold(request.Header.Get("Upgrade"), "websocket") {
		_, _ = io.WriteString(conn, "HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\n\r\n")
		_ = conn.Close()
		return
	}
	digest := sha1.Sum([]byte(request.Header.Get("Sec-WebSocket-Key") + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"))
	_, _ = io.WriteString(conn, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"+
		"Sec-WebSocket-Accept: "+base64.StdEncoding.EncodeToString(digest[:])+"\r\n\r\n")
	server := &serverConn{conn: conn, request: request, frames: make(chan received, 1024)}
	r.count.Add(1)
	if r.hello != nil {
		raw, _ := json.Marshal(r.hello)
		_ = server.send(opText, raw)
	}
	r.accepted <- server
	defer close(server.frames)
	for {
		var head [2]byte
		if _, err := io.ReadFull(reader, head[:]); err != nil {
			return
		}
		length := int(head[1] & 0x7f)
		if length == 126 {
			var extended [2]byte
			if _, err := io.ReadFull(reader, extended[:]); err != nil {
				return
			}
			length = int(binary.BigEndian.Uint16(extended[:]))
		}
		masked := head[1]&0x80 != 0
		var mask [4]byte
		if masked {
			if _, err := io.ReadFull(reader, mask[:]); err != nil {
				return
			}
		}
		payload := make([]byte, length)
		if _, err := io.ReadFull(reader, payload); err != nil {
			return
		}
		if masked {
			for i := range payload {
				payload[i] ^= mask[i%4]
			}
		}
		server.frames <- received{opcode: int(head[0] & 0x0f), payload: payload, masked: masked, at: time.Now()}
	}
}

func (r *testRelay) next(t *testing.T) *serverConn {
	t.Helper()
	select {
	case conn := <-r.accepted:
		return conn
	case <-time.After(wait):
		t.Fatal("relay never saw a connection")
		return nil
	}
}

func writeIdentity(t *testing.T, core *daemon.Core) *identity.Identity {
	t.Helper()
	seed := make([]byte, ed25519.SeedSize)
	_, _ = rand.Read(seed)
	der, err := x509.MarshalPKCS8PrivateKey(ed25519.NewKeyFromSeed(seed))
	if err != nil {
		t.Fatal(err)
	}
	path := core.Layout.IdentityKey()
	if err := os.MkdirAll(filepath.Dir(path), 0o700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, pem.EncodeToMemory(&pem.Block{Type: "PRIVATE KEY", Bytes: der}), 0o600); err != nil {
		t.Fatal(err)
	}
	id, err := identity.FromSeed(seed)
	if err != nil {
		t.Fatal(err)
	}
	return id
}

type fakeSink struct {
	mu       sync.Mutex
	sender   func(map[string]any) bool
	in       chan map[string]any
	restored atomic.Int32
}

func newFakeSink() *fakeSink { return &fakeSink{in: make(chan map[string]any, 64)} }

func (f *fakeSink) SetRelaySender(send func(map[string]any) bool) {
	f.mu.Lock()
	f.sender = send
	f.mu.Unlock()
}

func (f *fakeSink) current() func(map[string]any) bool {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.sender
}

func (f *fakeSink) RelayIn(frame map[string]any) { f.in <- frame }

func (f *fakeSink) RelayRestored() { f.restored.Add(1) }

type fixture struct {
	core   *daemon.Core
	module *Module
	id     *identity.Identity
	ui     *coretest.Peer
	relay  *testRelay
}

func defaultHello() map[string]any {
	return map[string]any{"type": "hello", "accepts": []string{"heartbeat", "agent-status"},
		"relays": []string{"terminal", "control"}, "controlBudget": map[string]any{"windowMs": 10000, "max": 200}}
}

func newFixture(t *testing.T, hello map[string]any, prepare func(core *daemon.Core) []daemon.Module) *fixture {
	t.Helper()
	core := coretest.New(t)
	id := writeIdentity(t, core)
	module := New()
	module.Timings = fastTimings()
	var modules []daemon.Module
	if prepare != nil {
		modules = prepare(core)
	}
	modules = append(modules, module)
	coretest.Start(t, core, modules...)
	ui := coretest.NewPeer("ui", rpc.UI)
	core.Hub.Add(ui)
	return &fixture{core: core, module: module, id: id, ui: ui, relay: newRelay(t, id.PublicKey(), hello)}
}

func (f *fixture) configure(t *testing.T, owner, account, node string) State {
	t.Helper()
	var state State
	coretest.MustCall(t, f.core, f.ui, "core.relay.configure", map[string]any{
		"url":     SignedURL(f.relay.endpoint(), owner, f.id, time.Now()),
		"account": account, "node": node,
	}, &state)
	return state
}

func (f *fixture) waitState(t *testing.T, match func(State) bool) State {
	t.Helper()
	deadline := time.After(wait)
	for {
		select {
		case event := <-f.ui.Events:
			if event.Name != EventState {
				continue
			}
			var state State
			_ = json.Unmarshal(event.Data, &state)
			if match(state) {
				return state
			}
		case <-deadline:
			t.Fatal("state never matched")
		}
	}
}

func (f *fixture) nextFrame(t *testing.T) map[string]any {
	t.Helper()
	deadline := time.After(wait)
	for {
		select {
		case event := <-f.ui.Events:
			if event.Name != EventFrame {
				continue
			}
			var payload struct {
				Frame map[string]any `json:"frame"`
			}
			_ = json.Unmarshal(event.Data, &payload)
			return payload.Frame
		case <-deadline:
			t.Fatal("no relay frame reached the desktop")
			return nil
		}
	}
}

func (f *fixture) send(t *testing.T, kind string, frame map[string]any) bool {
	t.Helper()
	var result struct {
		Sent bool `json:"sent"`
	}
	coretest.MustCall(t, f.core, f.ui, "core.relay.send", map[string]any{"kind": kind, "frame": frame}, &result)
	return result.Sent
}

func TestHandshakeAuthAndFramesBothWays(t *testing.T) {
	f := newFixture(t, defaultHello(), nil)
	f.configure(t, "alice", "al ice", "Box-1")
	server := f.relay.next(t)
	if got := server.request.Header.Get("User-Agent"); got != "ForkMeshDesktop/test (account=al%20ice; node=Box-1)" {
		t.Fatalf("user agent %q", got)
	}
	if server.request.Host != f.relay.listener.Addr().String() {
		t.Fatalf("host %q", server.request.Host)
	}
	if server.request.URL.Query().Get("owner") != "alice" {
		t.Fatalf("owner %q", server.request.URL.RawQuery)
	}
	if !strings.HasPrefix(server.request.URL.RawQuery, "owner=alice&ts=") {
		t.Fatalf("query order %q", server.request.URL.RawQuery)
	}
	state := f.waitState(t, func(s State) bool { return s.Connected && len(s.Accepts) == 2 })
	if strings.Join(state.Relays, ",") != "control,terminal" || state.BudgetLimit != 180 || state.BudgetWindowMs != 10000 {
		t.Fatalf("state %+v", state)
	}

	server.sendJSON(t, map[string]any{"type": "weird"})
	server.sendJSON(t, map[string]any{"type": "event", "topic": "commits", "repo": "alice/x", "state": "abc"})
	frame := f.nextFrame(t)
	if frame["type"] != "event" || frame["topic"] != "commits" || frame["repo"] != "alice/x" || frame["state"] != "abc" {
		t.Fatalf("event frame %v", frame)
	}
	for _, kind := range []string{"heartbeat-result", "agent-status-result", "terminal"} {
		server.sendJSON(t, map[string]any{"type": kind, "from": "browser", "requestId": "1"})
		if got := f.nextFrame(t); got["type"] != kind {
			t.Fatalf("forwarded %v, want %s", got, kind)
		}
	}
	server.sendJSON(t, map[string]any{"type": "control", "from": "node", "session": "x"})
	server.sendJSON(t, map[string]any{"type": "control", "from": "browser", "session": "y"})
	if got := f.nextFrame(t); got["type"] != "control" || got["session"] != "y" {
		t.Fatalf("control without a sink %v", got)
	}

	if !f.send(t, "signed", map[string]any{"type": "heartbeat", "node": "box-1", "ts": "1", "sig": "s"}) {
		t.Fatal("heartbeat refused")
	}
	if got, _ := server.nextText(t, "heartbeat"); got["node"] != "box-1" {
		t.Fatalf("heartbeat %v", got)
	}
	if f.send(t, "signed", map[string]any{"type": "endpoint"}) {
		t.Fatal("unadvertised kind sent")
	}
	if f.send(t, "signed", map[string]any{"type": "heartbeat", "pad": strings.Repeat("x", MaxOutboundPayload)}) {
		t.Fatal("oversize frame sent")
	}
	if !f.send(t, "terminal", map[string]any{"type": "terminal", "action": "output", "data": "<&>"}) {
		t.Fatal("terminal refused")
	}
	got, raw := server.nextText(t, "terminal")
	if got["data"] != "<&>" || !strings.Contains(string(raw.payload), `"<&>"`) {
		t.Fatalf("terminal %s", raw.payload)
	}
	if !f.send(t, "control", map[string]any{"type": "control", "action": "data", "session": "s"}) {
		t.Fatal("control refused")
	}
	server.nextText(t, "control")

	server.sendJSON(t, map[string]any{"type": "hello", "accepts": []string{}})
	f.waitState(t, func(s State) bool { return s.Connected && len(s.Accepts) == 0 && len(s.Relays) == 0 })
	if f.send(t, "control", map[string]any{"type": "control"}) {
		t.Fatal("control sent although the relay stopped carrying it")
	}
	if _, err := coretest.Call(t, f.core, f.ui, "core.relay.send", map[string]any{"kind": "bogus", "frame": map[string]any{"type": "x"}}); rpc.Code(err) != "invalid_kind" {
		t.Fatalf("bogus kind: %v", err)
	}
}

func TestControlFramesGoToTheControlSink(t *testing.T) {
	sink := newFakeSink()
	f := newFixture(t, defaultHello(), func(core *daemon.Core) []daemon.Module {
		core.Set(control.RelaySinkKey, sink)
		return nil
	})
	f.configure(t, "alice", "alice", "box-1")
	server := f.relay.next(t)
	f.waitState(t, func(s State) bool { return s.Connected && len(s.Relays) == 2 })
	if sink.restored.Load() != 1 {
		t.Fatalf("restored %d", sink.restored.Load())
	}
	server.sendJSON(t, map[string]any{"type": "control", "from": "node", "session": "echo"})
	server.sendJSON(t, map[string]any{"type": "control", "from": "browser", "session": "phone", "seq": 3})
	select {
	case frame := <-sink.in:
		if frame["session"] != "phone" || frame["seq"] != float64(3) {
			t.Fatalf("sink got %v", frame)
		}
	case <-time.After(wait):
		t.Fatal("control frame never reached the sink")
	}
	send := sink.current()
	if send == nil || !send(map[string]any{"type": "control", "action": "data", "session": "phone"}) {
		t.Fatal("no in-core sender")
	}
	if got, _ := server.nextText(t, "control"); got["session"] != "phone" {
		t.Fatalf("server got %v", got)
	}
	coretest.MustCall(t, f.core, f.ui, "core.relay.stop", nil, nil)
	if sink.current() != nil {
		t.Fatal("sender kept after stop")
	}
}

func TestRealControlModuleAnswersOverTheCoreRelay(t *testing.T) {
	controlModule := control.New()
	f := newFixture(t, defaultHello(), func(core *daemon.Core) []daemon.Module {
		return []daemon.Module{controlModule}
	})
	coretest.MustCall(t, f.core, f.ui, "control.info", map[string]any{"name": "Box", "user": "alice", "node": "box-1"}, nil)
	f.configure(t, "alice", "alice", "box-1")
	server := f.relay.next(t)
	f.waitState(t, func(s State) bool { return s.Connected && len(s.Relays) == 2 })
	if !controlModule.RelayOwned() {
		t.Fatal("control still sends through the desktop")
	}
	session := "abcdefghijklmnopqrstu"
	server.sendJSON(t, map[string]any{"type": "control", "from": "browser", "node": "box-1", "session": session,
		"action": "data", "seq": 0, "data": "aGVsbG8="})
	got, _ := server.nextText(t, "control")
	if got["action"] != "close" || got["reason"] != "channel_gone" || got["session"] != session {
		t.Fatalf("control answer %v", got)
	}
}

func TestReconnectAfterServerCloseAndRestoredEachTime(t *testing.T) {
	sink := newFakeSink()
	f := newFixture(t, defaultHello(), func(core *daemon.Core) []daemon.Module {
		core.Set(control.RelaySinkKey, sink)
		return nil
	})
	f.configure(t, "alice", "alice", "box-1")
	first := f.relay.next(t)
	f.waitState(t, func(s State) bool { return s.Connected })
	if err := first.send(opClose, []byte{0x03, 0xE8}); err != nil {
		t.Fatal(err)
	}
	echo := first.next(t, opClose)
	if string(echo.payload) != "\x03\xe8" {
		t.Fatalf("close echo %x", echo.payload)
	}
	f.waitState(t, func(s State) bool { return !s.Connected && s.Running })
	second := f.relay.next(t)
	f.waitState(t, func(s State) bool { return s.Connected })
	if sink.restored.Load() != 2 {
		t.Fatalf("restored %d", sink.restored.Load())
	}
	second.sendJSON(t, map[string]any{"type": "event", "topic": "pings"})
	if frame := f.nextFrame(t); frame["topic"] != "pings" {
		t.Fatalf("frame %v", frame)
	}
}

func TestRefusedUpgradeIsReportedAndRetried(t *testing.T) {
	relay := newRelay(t, "", defaultHello())
	relay.refuse.Store(1)
	client := NewClient()
	client.Timings = fastTimings()
	messages := make(chan string, 4)
	client.OnMessage = func(text string) { messages <- text }
	client.SignedURL = func() (string, bool) { return relay.endpoint() + "?owner=a&ts=1&sig=x", true }
	client.Start(context.Background())
	defer client.Stop()
	select {
	case text := <-messages:
		if text != "Relay event socket upgrade refused (HTTP/1.1 401 Unauthorized); falling back to the sync poll until it reconnects." {
			t.Fatalf("message %q", text)
		}
	case <-time.After(wait):
		t.Fatal("no refusal message")
	}
	relay.next(t)
}

func TestNotSignedInRetriesQuietly(t *testing.T) {
	relay := newRelay(t, "", defaultHello())
	client := NewClient()
	client.Timings = fastTimings()
	var asked atomic.Int32
	client.SignedURL = func() (string, bool) {
		if asked.Add(1) < 3 {
			return "", false
		}
		return relay.endpoint(), true
	}
	client.OnMessage = func(text string) { t.Errorf("unexpected message %q", text) }
	client.Start(context.Background())
	defer client.Stop()
	relay.next(t)
	if asked.Load() < 3 {
		t.Fatalf("asked %d", asked.Load())
	}
}

func TestKeepalivePingsAndStaleLinkReconnects(t *testing.T) {
	relay := newRelay(t, "", defaultHello())
	client := NewClient()
	client.Timings = fastTimings()
	client.Timings.StaleRx = 400 * time.Millisecond
	client.SignedURL = func() (string, bool) { return relay.endpoint(), true }
	client.Start(context.Background())
	defer client.Stop()
	server := relay.next(t)
	if err := server.send(opPing, []byte("abc")); err != nil {
		t.Fatal(err)
	}
	if pong := server.next(t, opPong); string(pong.payload) != "abc" {
		t.Fatalf("pong %q", pong.payload)
	}
	server.next(t, opPing)
	server.nextText(t, "ping")
	relay.next(t)
	if relay.count.Load() < 2 {
		t.Fatal("stale link was not replaced")
	}
}

func TestOversizeInboundFrameDropsTheLink(t *testing.T) {
	relay := newRelay(t, "", defaultHello())
	client := NewClient()
	client.Timings = fastTimings()
	client.Timings.StaleRx = time.Minute
	client.SignedURL = func() (string, bool) { return relay.endpoint(), true }
	client.Start(context.Background())
	defer client.Stop()
	server := relay.next(t)
	_ = server.send(opText, []byte(`{"type":"event","pad":"`+strings.Repeat("x", MaxEventPayload)+`"}`))
	relay.next(t)
}

func TestControlBudgetPacing(t *testing.T) {
	hello := map[string]any{"type": "hello", "relays": []string{"control"}, "controlBudget": map[string]any{"windowMs": 1000, "max": 10}}
	relay := newRelay(t, "", hello)
	client := NewClient()
	client.Timings = fastTimings()
	client.Timings.StaleRx = time.Minute
	states := make(chan State, 256)
	client.OnState = func(s State) { states <- s }
	client.SignedURL = func() (string, bool) { return relay.endpoint(), true }
	client.Start(context.Background())
	defer client.Stop()
	server := relay.next(t)
	deadline := time.Now().Add(wait)
	for client.State().BudgetLimit != 9 {
		if time.Now().After(deadline) {
			t.Fatalf("budget %+v", client.State())
		}
		time.Sleep(5 * time.Millisecond)
	}
	start := time.Now()
	for i := 0; i < 12; i++ {
		if !client.SendOwnerControl(map[string]any{"type": "control", "seq": i}) {
			t.Fatalf("frame %d refused", i)
		}
	}
	var times []time.Duration
	for i := 0; i < 12; i++ {
		frame, raw := server.nextText(t, "control")
		if int(frame["seq"].(float64)) != i {
			t.Fatalf("frame %d out of order: %v", i, frame)
		}
		times = append(times, raw.at.Sub(start))
	}
	if times[8] > 500*time.Millisecond {
		t.Fatalf("first nine frames paced: %v", times[8])
	}
	if times[9] < 900*time.Millisecond {
		t.Fatalf("tenth frame not held for the window: %v", times[9])
	}
	sawBacklog, sawClear := false, false
	edges := time.After(wait)
	for !sawClear {
		select {
		case s := <-states:
			if s.ControlBacklog > 0 {
				sawBacklog = true
			} else if sawBacklog {
				sawClear = true
			}
		case <-edges:
			t.Fatalf("backlog edges not reported (%v, %v)", sawBacklog, sawClear)
		}
	}
	if client.ControlBacklog() != 0 {
		t.Fatal("queue not empty")
	}
}

func TestDefaultBudgetWithoutAdvertisedOne(t *testing.T) {
	relay := newRelay(t, "", map[string]any{"type": "hello", "relays": []string{"control"}})
	client := NewClient()
	client.Timings = fastTimings()
	client.SignedURL = func() (string, bool) { return relay.endpoint(), true }
	client.Start(context.Background())
	defer client.Stop()
	relay.next(t)
	deadline := time.Now().Add(wait)
	for {
		s := client.State()
		if len(s.Relays) == 1 && s.BudgetLimit == 180 && s.BudgetWindowMs == 10000 {
			return
		}
		if time.Now().After(deadline) {
			t.Fatalf("state %+v", s)
		}
		time.Sleep(5 * time.Millisecond)
	}
}

func TestReconnectNowSkipsTheBackoff(t *testing.T) {
	relay := newRelay(t, "", defaultHello())
	client := NewClient()
	client.Timings = fastTimings()
	client.Timings.ReconnectBase = time.Minute
	client.Timings.ReconnectMax = time.Minute
	client.Timings.StaleRx = time.Minute
	client.SignedURL = func() (string, bool) { return relay.endpoint(), true }
	client.Start(context.Background())
	defer client.Stop()
	first := relay.next(t)
	if client.ReconnectNow() {
		t.Fatal("reconnectNow acted on a live link")
	}
	_ = first.conn.Close()
	deadline := time.Now().Add(wait)
	for !client.ReconnectNow() {
		if time.Now().After(deadline) {
			t.Fatal("client never waited to reconnect")
		}
		time.Sleep(5 * time.Millisecond)
	}
	select {
	case <-relay.accepted:
	case <-time.After(time.Second):
		t.Fatal("reconnectNow did not skip the backoff")
	}
}

func TestBackoffDoublesAndCaps(t *testing.T) {
	client := NewClient()
	client.Timings.Jitter = 0
	want := []time.Duration{2, 4, 8, 16, 32, 64, 120, 120, 120, 120, 120}
	for i, seconds := range want {
		got := client.nextDelay()
		low := seconds * time.Second
		if got < low || got > low+low/4 {
			t.Fatalf("attempt %d: %v not in [%v, %v]", i, got, low, low+low/4)
		}
	}
}

func TestReconfigureKeepsTheLinkAndStopClosesNormally(t *testing.T) {
	f := newFixture(t, defaultHello(), nil)
	f.configure(t, "alice", "alice", "box-1")
	server := f.relay.next(t)
	f.waitState(t, func(s State) bool { return s.Connected && len(s.Accepts) == 2 })
	again := f.configure(t, "alice", "alice", "box-1")
	if !again.Connected || !again.Running || len(again.Accepts) != 2 {
		t.Fatalf("reattach state %+v", again)
	}
	select {
	case <-f.relay.accepted:
		t.Fatal("same configuration reconnected")
	case <-time.After(150 * time.Millisecond):
	}
	moving := f.configure(t, "bob", "bob", "box-1")
	if closing := server.next(t, opClose); string(closing.payload) != "\x03\xe8" {
		t.Fatalf("close %x", closing.payload)
	}
	moved := f.relay.next(t)
	f.waitState(t, func(s State) bool { return s.Seq > moving.Seq && s.Connected })
	if moved.request.URL.Query().Get("owner") != "bob" {
		t.Fatalf("owner %q", moved.request.URL.RawQuery)
	}
	var stopped State
	coretest.MustCall(t, f.core, f.ui, "core.relay.stop", nil, &stopped)
	if stopped.Running || stopped.Connected {
		t.Fatalf("stopped state %+v", stopped)
	}
	if closing := moved.next(t, opClose); string(closing.payload) != "\x03\xe8" {
		t.Fatalf("close %x", closing.payload)
	}
	var reconnect struct {
		Reconnecting bool `json:"reconnecting"`
	}
	coretest.MustCall(t, f.core, f.ui, "core.relay.reconnect", nil, &reconnect)
	if reconnect.Reconnecting {
		t.Fatal("reconnect after stop")
	}
}

func TestSurvivesDesktopDetach(t *testing.T) {
	f := newFixture(t, defaultHello(), nil)
	f.configure(t, "alice", "alice", "box-1")
	server := f.relay.next(t)
	f.waitState(t, func(s State) bool { return s.Connected })
	f.core.Hub.Remove(f.ui)
	f.ui.Close()
	server.sendJSON(t, map[string]any{"type": "event", "topic": "pings"})
	time.Sleep(50 * time.Millisecond)
	if !f.module.Client().State().Connected {
		t.Fatal("link dropped with the desktop")
	}
	ui := coretest.NewPeer("ui2", rpc.UI)
	f.core.Hub.Add(ui)
	var state State
	coretest.MustCall(t, f.core, ui, "core.relay.configure", map[string]any{
		"url": SignedURL(f.relay.endpoint(), "alice", f.id, time.Now()), "account": "alice", "node": "box-1"}, &state)
	if !state.Connected {
		t.Fatalf("reattached state %+v", state)
	}
	if f.relay.count.Load() != 1 {
		t.Fatalf("connections %d", f.relay.count.Load())
	}
}

func TestRefusals(t *testing.T) {
	f := newFixture(t, defaultHello(), nil)
	phone := coretest.NewPeer("phone", rpc.Relay)
	for _, method := range []string{"core.relay.configure", "core.relay.stop", "core.relay.state", "core.relay.send", "core.relay.reconnect"} {
		if _, err := coretest.Call(t, f.core, phone, method, nil); rpc.Code(err) != "forbidden" {
			t.Fatalf("%s from a phone: %v", method, err)
		}
	}
	other, _ := identity.FromSeed(make([]byte, 32))
	_, err := coretest.Call(t, f.core, f.ui, "core.relay.configure", map[string]any{"url": SignedURL(f.relay.endpoint(), "alice", other, time.Now())})
	if rpc.Code(err) != "identity_mismatch" {
		t.Fatalf("foreign signature: %v", err)
	}
	_, err = coretest.Call(t, f.core, f.ui, "core.relay.configure", map[string]any{"url": "https://example.invalid/x?owner=a"})
	if rpc.Code(err) != "invalid_url" {
		t.Fatalf("https url: %v", err)
	}
	_, err = coretest.Call(t, f.core, f.ui, "core.relay.configure", map[string]any{"url": f.relay.endpoint()})
	if rpc.Code(err) != "invalid_owner" {
		t.Fatalf("no owner: %v", err)
	}
	_, err = coretest.Call(t, f.core, f.ui, "core.relay.send", map[string]any{"kind": "x", "frame": map[string]any{}})
	if err != nil {
		t.Fatalf("send with no relay: %v", err)
	}
}

func TestUserAgentEncoding(t *testing.T) {
	if got := UserAgent("1.2", "  al ice/é ", "box_1~"); got != "ForkMeshDesktop/1.2 (account=al%20ice%2F%C3%A9; node=box_1~)" {
		t.Fatalf("user agent %q", got)
	}
	if got := UserAgent("", strings.Repeat("a", 80), ""); got != "ForkMeshDesktop/dev (account="+strings.Repeat("a", 64)+"; node=)" {
		t.Fatalf("long account %q", got)
	}
}

func TestFrameCodec(t *testing.T) {
	for _, size := range []int{0, 125, 126, 4000, MaxOutboundPayload} {
		payload := []byte(strings.Repeat("z", size))
		reader := bufio.NewReader(strings.NewReader(string(encodeFrame(opText, payload))))
		final, opcode, got, err := readFrame(reader)
		if err != nil || !final || opcode != opText || string(got) != string(payload) {
			t.Fatalf("size %d: %v %v %d", size, err, final, opcode)
		}
	}
	fragmented := string([]byte{0x01, 3}) + "abc" + string([]byte{0x80, 2}) + "de"
	opcode, message, err := readMessage(bufio.NewReader(strings.NewReader(fragmented)))
	if err != nil || opcode != opText || string(message) != "abcde" {
		t.Fatalf("fragmented: %v %d %q", err, opcode, message)
	}
	huge := string([]byte{0x81, 127, 0, 0, 0, 0, 0, 2, 0, 0})
	if _, _, err := readMessage(bufio.NewReader(strings.NewReader(huge))); !errors.Is(err, errFrameTooLarge) {
		t.Fatalf("huge frame: %v", err)
	}
	server, client := net.Pipe()
	defer server.Close()
	defer client.Close()
	if err := writeFrame(client, opText, make([]byte, MaxOutboundPayload+1)); !errors.Is(err, errFrameTooLarge) {
		t.Fatalf("oversize write: %v", err)
	}
}

func TestRestartedDesktopMustReclaimTheSocket(t *testing.T) {
	f := newFixture(t, defaultHello(), nil)
	f.module.ReclaimWindow = 150 * time.Millisecond
	f.module.Timings.StaleRx = time.Minute
	f.core.IPC.OnUI(nil, true)
	f.configure(t, "alice", "alice", "box-1")
	server := f.relay.next(t)
	f.waitState(t, func(s State) bool { return s.Connected })
	time.Sleep(250 * time.Millisecond)
	if f.module.Client() == nil {
		t.Fatal("a configured socket was closed")
	}
	f.core.IPC.OnUI(nil, false)
	f.core.IPC.OnUI(nil, true)
	time.Sleep(50 * time.Millisecond)
	f.configure(t, "alice", "alice", "box-1")
	time.Sleep(200 * time.Millisecond)
	if f.module.Client() == nil || f.relay.count.Load() != 1 {
		t.Fatal("a reclaimed socket was closed or reopened")
	}
	f.core.IPC.OnUI(nil, false)
	f.core.IPC.OnUI(nil, true)
	if closing := server.next(t, opClose); string(closing.payload) != "\x03\xe8" {
		t.Fatalf("close %x", closing.payload)
	}
	if f.module.Client() != nil {
		t.Fatal("unclaimed socket kept")
	}
}

func restartCore(t *testing.T, previous *daemon.Core) (*daemon.Core, *Module) {
	t.Helper()
	core := daemon.NewCore(previous.Layout, "test", log.New(io.Discard, "", 0))
	module := New()
	module.Timings = fastTimings()
	coretest.Start(t, core, module)
	return core, module
}

func TestStartReconnectsWithTheLastSettingsUntilStopped(t *testing.T) {
	f := newFixture(t, defaultHello(), nil)
	f.configure(t, "alice", "al ice", "box-1")
	f.relay.next(t)
	f.waitState(t, func(s State) bool { return s.Connected })
	path := filepath.Join(f.core.Layout.CoreState(), savedFile)
	raw, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	var last saved
	if err := json.Unmarshal(raw, &last); err != nil {
		t.Fatal(err)
	}
	if last != (saved{Endpoint: f.relay.endpoint(), Owner: "alice", Account: "al ice", Node: "box-1", Identity: f.id.PublicKey()}) {
		t.Fatalf("saved %+v", last)
	}
	if info, _ := os.Stat(path); info.Mode().Perm() != 0o600 {
		t.Fatalf("mode %v", info.Mode())
	}
	f.module.Stop()

	core, module := restartCore(t, f.core)
	server := f.relay.next(t)
	if owner := server.request.URL.Query().Get("owner"); owner != "alice" {
		t.Fatalf("owner %q", owner)
	}
	if agent := server.request.Header.Get("User-Agent"); agent != "ForkMeshDesktop/test (account=al%20ice; node=box-1)" {
		t.Fatalf("user agent %q", agent)
	}
	deadline := time.Now().Add(wait)
	for module.Client() == nil || !module.Client().State().Connected {
		if time.Now().After(deadline) {
			t.Fatal("restarted core never connected")
		}
		time.Sleep(10 * time.Millisecond)
	}
	ui := coretest.NewPeer("ui", rpc.UI)
	core.Hub.Add(ui)
	var state State
	coretest.MustCall(t, core, ui, "core.relay.configure", map[string]any{
		"url": SignedURL(f.relay.endpoint(), "alice", f.id, time.Now()), "account": "al ice", "node": "box-1"}, &state)
	if !state.Connected || f.relay.count.Load() != 2 {
		t.Fatalf("reclaimed state %+v after %d connections", state, f.relay.count.Load())
	}
	coretest.MustCall(t, core, ui, "core.relay.stop", nil, nil)
	if _, err := os.Stat(path); !errors.Is(err, os.ErrNotExist) {
		t.Fatalf("stop kept the saved settings: %v", err)
	}
	module.Stop()

	_, again := restartCore(t, core)
	time.Sleep(150 * time.Millisecond)
	if again.Client() != nil || f.relay.count.Load() != 2 {
		t.Fatal("a stopped relay reconnected at start")
	}
}

func TestStartSkipsSavedSettingsForAnotherNodeKey(t *testing.T) {
	f := newFixture(t, defaultHello(), nil)
	f.configure(t, "alice", "alice", "box-1")
	f.relay.next(t)
	f.module.Stop()
	writeIdentity(t, f.core)
	_, module := restartCore(t, f.core)
	time.Sleep(150 * time.Millisecond)
	if module.Client() != nil || f.relay.count.Load() != 1 {
		t.Fatal("reconnected with a different node key")
	}
}

func TestSecureRelayOverTLS(t *testing.T) {
	relay, pool := newTLSRelay(t, defaultHello())
	client := NewClient()
	client.Timings = fastTimings()
	client.TLSConfig = &tls.Config{RootCAs: pool}
	client.SignedURL = func() (string, bool) { return relay.endpoint() + "?owner=a", true }
	client.Start(context.Background())
	defer client.Stop()
	server := relay.next(t)
	deadline := time.Now().Add(wait)
	for !client.Accepts("heartbeat") {
		if time.Now().After(deadline) {
			t.Fatal("no hello over TLS")
		}
		time.Sleep(5 * time.Millisecond)
	}
	if !client.SendSigned(map[string]any{"type": "heartbeat"}) {
		t.Fatal("send over TLS")
	}
	server.nextText(t, "heartbeat")
}

func TestUntrustedRelayCertificateIsRefused(t *testing.T) {
	relay, _ := newTLSRelay(t, defaultHello())
	client := NewClient()
	client.Timings = fastTimings()
	client.SignedURL = func() (string, bool) { return relay.endpoint() + "?owner=a", true }
	client.Start(context.Background())
	defer client.Stop()
	select {
	case <-relay.accepted:
		t.Fatal("connected to an untrusted relay")
	case <-time.After(300 * time.Millisecond):
	}
	if client.State().Connected {
		t.Fatal("connected state with a bad certificate")
	}
}
