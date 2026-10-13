package control

import (
	"bufio"
	"context"
	"crypto/ed25519"
	"crypto/rand"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"os"
	"path/filepath"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"forkmesh.com/desktop-core/internal/coretest"
	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/identity"
	"forkmesh.com/desktop-core/internal/ipc"
	"forkmesh.com/desktop-core/internal/rpc"
)

const waitTimeout = 5 * time.Second

func writeFile(t *testing.T, path, content string) {
	t.Helper()
	if err := os.WriteFile(path, []byte(content), 0o600); err != nil {
		t.Fatal(err)
	}
}

type signer struct {
	private ed25519.PrivateKey
	public  string
}

func newSigner(t *testing.T) *signer {
	t.Helper()
	public, private, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	return &signer{private: private, public: ToBase64Url(public)}
}

func (s *signer) sign(data []byte) string { return ToBase64Url(ed25519.Sign(s.private, data)) }

type harness struct {
	t      *testing.T
	core   *daemon.Core
	module *Module
	ctx    context.Context
	ui     *testUI
	mesh   net.Listener
}

func newHarness(t *testing.T, prepare func(m *Module, core *daemon.Core)) *harness {
	t.Helper()
	core := coretest.New(t)
	seed := make([]byte, 32)
	_, _ = rand.Read(seed)
	id, err := identity.FromSeed(seed)
	if err != nil {
		t.Fatal(err)
	}
	core.Identity = id
	module := New()
	if prepare != nil {
		prepare(module, core)
	}
	ctx := coretest.Start(t, core, module)
	h := &harness{t: t, core: core, module: module, ctx: ctx}
	return h
}

func (h *harness) listenIPC() ipc.Endpoint {
	h.t.Helper()
	dir, err := os.MkdirTemp("", "fmc")
	if err != nil {
		h.t.Fatal(err)
	}
	h.t.Cleanup(func() {
		_ = os.Remove(filepath.Join(dir, "core.sock"))
		_ = os.Remove(filepath.Join(dir, "endpoint.json"))
		_ = os.Remove(dir)
	})
	endpoint, err := h.core.IPC.Listen(dir)
	if err != nil {
		h.t.Fatal(err)
	}
	ctx, cancel := context.WithCancel(h.ctx)
	done := make(chan struct{})
	go func() {
		h.core.IPC.Serve(ctx)
		close(done)
	}()
	h.t.Cleanup(func() {
		cancel()
		<-done
	})
	return endpoint
}

func (h *harness) attachUI() *testUI {
	h.t.Helper()
	endpoint := h.listenIPC()
	ui := dialUI(h.t, endpoint)
	h.ui = ui
	deadline := time.Now().Add(waitTimeout)
	for h.core.UI() == nil {
		if time.Now().After(deadline) {
			h.t.Fatal("ui never attached")
		}
		time.Sleep(5 * time.Millisecond)
	}
	return ui
}

func (h *harness) startMesh() string {
	h.t.Helper()
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		h.t.Fatal(err)
	}
	h.mesh = listener
	h.t.Cleanup(func() { listener.Close() })
	go func() {
		for {
			conn, err := listener.Accept()
			if err != nil {
				return
			}
			go func() {
				_, _ = conn.Write(Frame([]byte(`{"iv":"mesh","ct":"key"}`)))
				var buffer []byte
				chunk := make([]byte, 4096)
				for {
					payload, taken := takeFrame(&buffer)
					if taken == 1 {
						if !IsControlHello(payload) {
							conn.Close()
							return
						}
						h.module.Adopt(conn, payload, buffer)
						return
					}
					if taken < 0 {
						conn.Close()
						return
					}
					n, err := conn.Read(chunk)
					buffer = append(buffer, chunk[:n]...)
					if err != nil {
						conn.Close()
						return
					}
				}
			}()
		}
	}()
	return listener.Addr().String()
}

func (h *harness) do(task func()) {
	h.module.call(func() any {
		task()
		return nil
	})
}

type uiNote struct {
	Method string
	Params json.RawMessage
}

type testUI struct {
	t       *testing.T
	conn    net.Conn
	writeMu sync.Mutex
	nextID  atomic.Int64
	mu      sync.Mutex
	calls   map[int64]chan rpc.Message
	notes   chan uiNote
	seen    []uiNote
	verdict func(key string) string
	relayUp atomic.Bool
	relayed map[string]*relayPhone
	frames  []map[string]any
	asked   []string
}

func dialUI(t *testing.T, endpoint ipc.Endpoint) *testUI {
	t.Helper()
	conn, err := ipc.Dial(endpoint, 2*time.Second)
	if err != nil {
		t.Fatal(err)
	}
	ui := &testUI{t: t, conn: conn, calls: map[int64]chan rpc.Message{}, notes: make(chan uiNote, 8192),
		relayed: map[string]*relayPhone{}}
	ui.relayUp.Store(true)
	ui.verdict = func(string) string { return VerdictNotOwned }
	t.Cleanup(func() { conn.Close() })
	reader := bufio.NewReaderSize(conn, 1<<20)
	ui.writeLine(map[string]any{"id": 1, "method": "core.hello", "params": map[string]any{"token": endpoint.Token,
		"role": "ui", "name": "test-ui", "pid": os.Getpid()}})
	line, err := reader.ReadBytes('\n')
	if err != nil {
		t.Fatal(err)
	}
	var hello rpc.Message
	if json.Unmarshal(line, &hello) != nil || hello.OK == nil || !*hello.OK {
		t.Fatalf("hello refused: %s", line)
	}
	ui.nextID.Store(100)
	go ui.readLoop(reader)
	return ui
}

func (u *testUI) writeLine(value any) {
	raw, _ := json.Marshal(value)
	u.writeMu.Lock()
	defer u.writeMu.Unlock()
	_, _ = u.conn.Write(append(raw, '\n'))
}

func (u *testUI) readLoop(reader *bufio.Reader) {
	for {
		line, err := reader.ReadBytes('\n')
		if len(line) > 0 {
			var message rpc.Message
			if json.Unmarshal(line, &message) == nil {
				u.handle(message)
			}
		}
		if err != nil {
			return
		}
	}
}

func (u *testUI) handle(message rpc.Message) {
	switch {
	case message.Method != "" && message.ID > 0:
		u.writeLine(map[string]any{"id": message.ID, "ok": true, "result": u.answer(message.Method, message.Params)})
	case message.Method != "":
		u.notes <- uiNote{Method: message.Method, Params: message.Params}
	case message.ID > 0:
		u.mu.Lock()
		reply := u.calls[message.ID]
		delete(u.calls, message.ID)
		u.mu.Unlock()
		if reply != nil {
			reply <- message
		}
	}
}

func (u *testUI) answer(method string, params json.RawMessage) any {
	switch method {
	case "ui.control.accountCheck":
		var request struct {
			Device struct {
				Key string `json:"key"`
			} `json:"device"`
		}
		_ = json.Unmarshal(params, &request)
		u.mu.Lock()
		u.asked = append(u.asked, request.Device.Key)
		verdict := u.verdict
		u.mu.Unlock()
		return map[string]any{"verdict": verdict(request.Device.Key)}
	case "ui.relay.send":
		var request struct {
			Frame map[string]any `json:"frame"`
		}
		_ = json.Unmarshal(params, &request)
		if !u.relayUp.Load() {
			return map[string]any{"sent": false}
		}
		u.mu.Lock()
		u.frames = append(u.frames, request.Frame)
		phone := u.relayed[jsonString(request.Frame["session"])]
		u.mu.Unlock()
		if phone != nil {
			phone.frames <- request.Frame
		}
		return map[string]any{"sent": true}
	}
	return map[string]any{}
}

func (u *testUI) relayFrames(session, action string) []map[string]any {
	u.mu.Lock()
	defer u.mu.Unlock()
	var out []map[string]any
	for _, frame := range u.frames {
		if jsonString(frame["session"]) == session && (action == "" || jsonString(frame["action"]) == action) {
			out = append(out, frame)
		}
	}
	return out
}

func (u *testUI) askedKeys() []string {
	u.mu.Lock()
	defer u.mu.Unlock()
	return append([]string(nil), u.asked...)
}

func (u *testUI) clearAsked() {
	u.mu.Lock()
	u.asked = nil
	u.mu.Unlock()
}

func (u *testUI) setVerdicts(verdicts map[string]string) {
	copied := map[string]string{}
	for key, value := range verdicts {
		copied[key] = value
	}
	u.mu.Lock()
	u.verdict = func(key string) string {
		if value, ok := copied[key]; ok {
			return value
		}
		return VerdictNotOwned
	}
	u.mu.Unlock()
}

func (u *testUI) call(method string, params any) (json.RawMessage, error) {
	id := u.nextID.Add(1)
	reply := make(chan rpc.Message, 1)
	u.mu.Lock()
	u.calls[id] = reply
	u.mu.Unlock()
	u.writeLine(map[string]any{"id": id, "method": method, "params": params})
	select {
	case message := <-reply:
		if message.OK != nil && !*message.OK {
			return nil, errors.New(message.Error)
		}
		return message.Result, nil
	case <-time.After(waitTimeout):
		return nil, fmt.Errorf("%s timed out", method)
	}
}

func (u *testUI) mustCall(method string, params any) json.RawMessage {
	u.t.Helper()
	raw, err := u.call(method, params)
	if err != nil {
		u.t.Fatalf("%s: %v", method, err)
	}
	return raw
}

func (u *testUI) notify(method string, params any) {
	u.writeLine(map[string]any{"method": method, "params": params})
}

func (u *testUI) waitNote(method string, match func(params map[string]any) bool) map[string]any {
	u.t.Helper()
	u.mu.Lock()
	for _, note := range u.seen {
		if note.Method == method {
			params := parseObject(note.Params)
			if match == nil || match(params) {
				u.mu.Unlock()
				return params
			}
		}
	}
	u.mu.Unlock()
	deadline := time.After(waitTimeout)
	for {
		select {
		case note := <-u.notes:
			u.mu.Lock()
			u.seen = append(u.seen, note)
			u.mu.Unlock()
			if note.Method == method {
				params := parseObject(note.Params)
				if match == nil || match(params) {
					return params
				}
			}
		case <-deadline:
			u.t.Fatalf("no %s notification", method)
			return nil
		}
	}
}

func (u *testUI) drainNotes() {
	for {
		select {
		case note := <-u.notes:
			u.mu.Lock()
			u.seen = append(u.seen, note)
			u.mu.Unlock()
		default:
			return
		}
	}
}

func (u *testUI) countNotes(method string, match func(params map[string]any) bool) int {
	u.drainNotes()
	u.mu.Lock()
	defer u.mu.Unlock()
	n := 0
	for _, note := range u.seen {
		if note.Method == method && (match == nil || match(parseObject(note.Params))) {
			n++
		}
	}
	return n
}

type messageLog struct {
	t      *testing.T
	in     chan map[string]any
	closed chan struct{}
	mu     sync.Mutex
	seen   []map[string]any
}

func newMessageLog(t *testing.T) *messageLog {
	return &messageLog{t: t, in: make(chan map[string]any, 4096), closed: make(chan struct{})}
}

func (l *messageLog) wait(match func(map[string]any) bool) map[string]any {
	l.t.Helper()
	l.mu.Lock()
	for _, message := range l.seen {
		if match(message) {
			l.mu.Unlock()
			return message
		}
	}
	l.mu.Unlock()
	deadline := time.After(waitTimeout)
	for {
		select {
		case message := <-l.in:
			l.mu.Lock()
			l.seen = append(l.seen, message)
			l.mu.Unlock()
			if match(message) {
				return message
			}
		case <-deadline:
			l.t.Fatal("expected message never arrived")
			return nil
		}
	}
}

func (l *messageLog) event(name string) map[string]any {
	l.t.Helper()
	message := l.wait(func(m map[string]any) bool { return jsonString(m["event"]) == name })
	return jsonObject(message["data"])
}

func (l *messageLog) response(id int) map[string]any {
	l.t.Helper()
	return l.wait(func(m map[string]any) bool {
		got, ok := jsonInt(m["id"])
		return ok && got == id && m["ok"] != nil
	})
}

func (l *messageLog) waitClosed() {
	l.t.Helper()
	select {
	case <-l.closed:
	case <-time.After(waitTimeout):
		l.t.Fatal("channel never closed")
	}
}

func (l *messageLog) isClosed() bool {
	select {
	case <-l.closed:
		return true
	default:
		return false
	}
}

func (l *messageLog) count(match func(map[string]any) bool) int {
	for {
		select {
		case message := <-l.in:
			l.mu.Lock()
			l.seen = append(l.seen, message)
			l.mu.Unlock()
			continue
		default:
		}
		break
	}
	l.mu.Lock()
	defer l.mu.Unlock()
	n := 0
	for _, message := range l.seen {
		if match(message) {
			n++
		}
	}
	return n
}

type handshake struct {
	identity  *signer
	eph       Ephemeral
	ephB64    string
	nonceB64  string
	platform  string
	name      string
	keys      SessionKeys
	code      string
	serverKey string
	forge     bool
}

func newHandshake(identity *signer, platform, name string) *handshake {
	nonce := make([]byte, 16)
	_, _ = rand.Read(nonce)
	eph := NewEphemeral()
	return &handshake{identity: identity, eph: eph, ephB64: ToBase64Url(eph.PublicKey),
		nonceB64: ToBase64Url(nonce), platform: platform, name: name}
}

func (h *handshake) hello() []byte {
	return encodeJSON(map[string]any{"t": HelloTag, "v": ProtocolVersion, "key": h.identity.public,
		"eph": h.ephB64, "nonce": h.nonceB64, "name": h.name, "user": "alice", "platform": h.platform})
}

func (h *handshake) welcome(payload []byte) ([]byte, error) {
	w := parseObject(payload)
	if jsonString(w["t"]) != WelcomeTag {
		return nil, nil
	}
	h.serverKey = jsonString(w["key"])
	serverEph := jsonString(w["eph"])
	transcript := Transcript(h.identity.public, h.ephB64, h.nonceB64, h.serverKey, serverEph)
	if !VerifyEd25519(h.serverKey, jsonString(w["sig"]), ServerSignedBytes(transcript)) {
		return nil, errors.New("welcome signature did not verify")
	}
	h.keys = DeriveKeys(X25519(h.eph.PrivateKey, FromBase64Url(serverEph, 32)), transcript)
	h.code = PairingCode(transcript)
	signed := ClientSignedBytes(transcript)
	if h.forge {
		signed = []byte("forged")
	}
	return encodeJSON(map[string]any{"t": AuthTag, "sig": h.identity.sign(signed)}), nil
}

type lanPhone struct {
	*messageLog
	hs          *handshake
	conn        net.Conn
	writeMu     sync.Mutex
	sendCounter uint64
	welcomed    chan struct{}
}

func dialPhone(t *testing.T, addr string, identity *signer, forge bool) *lanPhone {
	t.Helper()
	conn, err := net.Dial("tcp", addr)
	if err != nil {
		t.Fatal(err)
	}
	return startPhone(t, conn, nil, identity, forge)
}

func startPhone(t *testing.T, conn net.Conn, reader io.Reader, identity *signer, forge bool) *lanPhone {
	return startPhoneWith(t, conn, reader, newHandshake(identity, "ios", "Test phone"), forge, true)
}

func startPhoneWith(t *testing.T, conn net.Conn, reader io.Reader, hs *handshake, forge, sendHello bool) *lanPhone {
	t.Helper()
	phone := &lanPhone{messageLog: newMessageLog(t), hs: hs, conn: conn,
		welcomed: make(chan struct{})}
	phone.hs.forge = forge
	t.Cleanup(func() { conn.Close() })
	if reader == nil {
		reader = conn
	}
	if sendHello {
		phone.write(Frame(phone.hs.hello()))
	}
	go phone.readLoop(reader)
	return phone
}

func (p *lanPhone) write(raw []byte) {
	p.writeMu.Lock()
	defer p.writeMu.Unlock()
	_, _ = p.conn.Write(raw)
}

func (p *lanPhone) readLoop(reader io.Reader) {
	defer close(p.closed)
	var buffer []byte
	chunk := make([]byte, 1<<16)
	welcomed := false
	var receiveCounter uint64
	for {
		for {
			payload, taken := takeFrame(&buffer)
			if taken != 1 {
				break
			}
			if !welcomed {
				auth, err := p.hs.welcome(payload)
				if err != nil {
					return
				}
				if auth == nil {
					continue
				}
				welcomed = true
				close(p.welcomed)
				p.write(Frame(auth))
				continue
			}
			plain := OpenFrame(p.hs.keys.ServerToClient, receiveCounter, payload)
			receiveCounter++
			if plain == nil {
				p.in <- map[string]any{"badFrame": true}
				continue
			}
			p.in <- parseObject(plain)
		}
		n, err := reader.Read(chunk)
		buffer = append(buffer, chunk[:n]...)
		if err != nil {
			return
		}
	}
}

func (p *lanPhone) send(message map[string]any) {
	<-p.welcomed
	p.writeMu.Lock()
	counter := p.sendCounter
	p.sendCounter++
	sealed := SealFrame(p.hs.keys.ClientToServer, counter, encodeJSON(message))
	_, _ = p.conn.Write(Frame(sealed))
	p.writeMu.Unlock()
}

type relayPhone struct {
	*messageLog
	t           *testing.T
	hs          *handshake
	session     string
	node        string
	deliver     func(frame map[string]any)
	frames      chan map[string]any
	sendMu      sync.Mutex
	sendSeq     uint32
	sendCounter uint64
	welcomed    chan struct{}
	closeReason atomic.Value
	outOfOrder  atomic.Bool
}

func newRelayPhone(t *testing.T, identity *signer, session, platform string, deliver func(map[string]any)) *relayPhone {
	phone := &relayPhone{messageLog: newMessageLog(t), t: t, hs: newHandshake(identity, platform, "Away phone"),
		session: session, node: "desk", deliver: deliver, frames: make(chan map[string]any, 4096),
		welcomed: make(chan struct{})}
	go phone.pump()
	return phone
}

func (p *relayPhone) chunkFrame(action string, raw []byte, more bool) map[string]any {
	frame := map[string]any{"type": "control", "action": action, "session": p.session, "node": p.node,
		"from": "browser", "seq": p.sendSeq, "data": base64.StdEncoding.EncodeToString(raw), "more": more}
	p.sendSeq++
	return frame
}

func (p *relayPhone) open() {
	hello := p.hs.hello()
	p.sendMu.Lock()
	first := p.chunkFrame("open", hello[:40], true)
	second := p.chunkFrame("open", hello[40:], false)
	p.sendMu.Unlock()
	p.deliver(first)
	p.deliver(second)
}

func (p *relayPhone) pump() {
	var pending []byte
	var receiveSeq uint32
	var receiveCounter uint64
	welcomed := false
	for frame := range p.frames {
		action := jsonString(frame["action"])
		if action == "close" || action == "error" {
			p.closeReason.Store(jsonString(frame["reason"]))
			select {
			case <-p.closed:
			default:
				close(p.closed)
			}
			continue
		}
		seq, _ := jsonInt(frame["seq"])
		if uint32(seq) != receiveSeq {
			p.outOfOrder.Store(true)
		}
		receiveSeq++
		pending = append(pending, lenientBase64(jsonString(frame["data"]))...)
		if jsonBool(frame["more"]) {
			continue
		}
		payload := pending
		pending = nil
		if !welcomed {
			auth, err := p.hs.welcome(payload)
			if err != nil || auth == nil {
				continue
			}
			welcomed = true
			p.sendMu.Lock()
			chunk := p.chunkFrame("data", auth, false)
			p.sendMu.Unlock()
			p.deliver(chunk)
			close(p.welcomed)
			continue
		}
		plain := OpenFrame(p.hs.keys.ServerToClient, receiveCounter, payload)
		receiveCounter++
		p.in2(parseObject(plain))
	}
}

func (p *relayPhone) in2(message map[string]any) { p.messageLog.in <- message }

func (p *relayPhone) send(message map[string]any) {
	<-p.welcomed
	p.sendMu.Lock()
	sealed := SealFrame(p.hs.keys.ClientToServer, p.sendCounter, encodeJSON(message))
	p.sendCounter++
	frame := p.chunkFrame("data", sealed, false)
	p.sendMu.Unlock()
	p.deliver(frame)
}

func (p *relayPhone) reason() string {
	value, _ := p.closeReason.Load().(string)
	return value
}

func (u *testUI) relayPhone(identity *signer, session, platform string) *relayPhone {
	phone := newRelayPhone(u.t, identity, session, platform, func(frame map[string]any) {
		u.notify("control.relay.in", map[string]any{"frame": frame})
	})
	u.mu.Lock()
	u.relayed[session] = phone
	u.mu.Unlock()
	return phone
}

func storeSample(devices ...map[string]any) []byte {
	var out []byte
	out = append(out, "{\n    \"devices\": [\n"...)
	for i, device := range devices {
		raw, _ := json.MarshalIndent(device, "        ", "    ")
		out = append(out, "        "...)
		out = append(out, raw...)
		if i < len(devices)-1 {
			out = append(out, ',')
		}
		out = append(out, '\n')
	}
	out = append(out, "    ]\n}\n"...)
	return out
}
