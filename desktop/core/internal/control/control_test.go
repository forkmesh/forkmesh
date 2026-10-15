package control

import (
	"bytes"
	"context"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
	"time"

	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/ipc"
	"forkmesh.com/desktop-core/internal/rpc"
)

func intOf(t *testing.T, value any) int {
	t.Helper()
	n, ok := jsonInt(value)
	if !ok {
		t.Fatalf("not an int: %#v", value)
	}
	return n
}

func eventually(t *testing.T, what string, check func() bool) {
	t.Helper()
	deadline := time.Now().Add(waitTimeout)
	for !check() {
		if time.Now().After(deadline) {
			t.Fatalf("timed out waiting for %s", what)
		}
		time.Sleep(5 * time.Millisecond)
	}
}

func registerTestHandlers(core *daemon.Core) {
	core.Registry.Handle("test.echo", "test", func(ctx context.Context, req *rpc.Request) (any, error) {
		params := req.Object()
		return map[string]any{"echo": params["x"], "peer": req.Peer.ID(), "transport": string(req.Peer.Transport()),
			"device": req.Peer.Device().Key, "remote": rpc.Remote(req.Peer)}, nil
	})
	core.Registry.Handle("test.fail", "test", func(ctx context.Context, req *rpc.Request) (any, error) {
		return nil, rpc.Fail("nope")
	})
	core.Registry.Handle("test.big", "test", func(ctx context.Context, req *rpc.Request) (any, error) {
		return map[string]any{"blob": strings.Repeat("b", 30000)}, nil
	})
}

func TestLANPairingAndDispatch(t *testing.T) {
	h := newHarness(t, func(m *Module, core *daemon.Core) { registerTestHandlers(core) })
	ui := h.attachUI()
	ui.mustCall("control.info", map[string]any{"name": "desk", "user": "alice", "node": "desk"})
	addr := h.startMesh()
	phoneKey := newSigner(t)
	phone := dialPhone(t, addr, phoneKey, false)

	session := phone.event("session")
	if session["paired"] != false || jsonString(session["code"]) != phone.hs.code || len(phone.hs.code) != 6 {
		t.Fatalf("pending session %v (code %s)", session, phone.hs.code)
	}
	if phone.hs.serverKey != h.core.Identity.PublicKey() || session["key"] != h.core.Identity.PublicKey() {
		t.Fatal("the desktop key is the core identity")
	}
	if session["transport"] != "lan" || intOf(t, session["protocol"]) != 1 || session["name"] != "desk" ||
		session["user"] != "alice" {
		t.Fatalf("session info %v", session)
	}
	pairing := ui.waitNote("ui.control.pairing", nil)
	if pairing["code"] != phone.hs.code {
		t.Fatalf("the desktop shows the same code: %v", pairing)
	}
	device := jsonObject(pairing["device"])
	if device["name"] != "Test phone" || device["user"] != "alice" || device["key"] != phoneKey.public {
		t.Fatalf("pairing device %v", device)
	}
	connID := intOf(t, pairing["connection"])

	phone.send(map[string]any{"id": 1, "method": "agents.list"})
	if phone.response(1)["error"] != "not_paired" {
		t.Fatal("requests refused before approval")
	}
	phone.send(map[string]any{"id": 2, "method": "ping"})
	if phone.response(2)["ok"] != true {
		t.Fatal("ping answers before approval")
	}
	if ui.countNotes("ui.control.request", nil) != 0 {
		t.Fatal("the UI never saw a request before approval")
	}

	ui.mustCall("control.approve", map[string]any{"connection": connID})
	phone.wait(func(m map[string]any) bool {
		return m["event"] == "session" && jsonObject(m["data"])["paired"] == true
	})
	opened := ui.waitNote("ui.control.opened", nil)
	if intOf(t, opened["connection"]) != connID || opened["relayed"] != false ||
		jsonObject(opened["device"])["approval"] != "code" {
		t.Fatalf("opened %v", opened)
	}
	ui.waitNote("ui.control.pairingClosed", func(p map[string]any) bool { return intOf(t, p["connection"]) == connID })
	stored := LoadDevices(h.module.StorePath)
	if stored[phoneKey.public].Approval != "code" || stored[phoneKey.public].PairedAtMs == 0 {
		t.Fatalf("approval remembered: %#v", stored)
	}
	if h.module.StorePath != filepath.Join(h.core.Layout.Data, "local_control", "paired-devices.json") {
		t.Fatal(h.module.StorePath)
	}

	phone.send(map[string]any{"id": 3, "method": "test.echo", "params": map[string]any{"x": 5}})
	result := jsonObject(phone.response(3)["result"])
	if intOf(t, result["echo"]) != 5 || result["peer"] != fmt.Sprintf("lan:%d", connID) || result["transport"] != "lan" ||
		result["device"] != phoneKey.public || result["remote"] != true {
		t.Fatalf("native result %v", result)
	}
	phone.send(map[string]any{"id": 4, "method": "test.fail"})
	if response := phone.response(4); response["ok"] != false || response["error"] != "nope" {
		t.Fatalf("native error %v", response)
	}
	phone.send(map[string]any{"id": 5, "method": "control.devices"})
	if phone.response(5)["error"] != "forbidden" {
		t.Fatal("UI-only methods are refused to phones")
	}

	phone.send(map[string]any{"id": 6, "method": "agents.list", "params": map[string]any{"y": 1}})
	request := ui.waitNote("ui.control.request", func(p map[string]any) bool { return p["method"] == "agents.list" })
	if intOf(t, request["id"]) != 6 || intOf(t, request["connection"]) != connID || request["relayed"] != false ||
		intOf(t, jsonObject(request["params"])["y"]) != 1 || jsonObject(request["device"])["key"] != phoneKey.public {
		t.Fatalf("proxied request %v", request)
	}
	ui.notify("control.reply", map[string]any{"connection": connID,
		"message": map[string]any{"id": 6, "ok": true, "result": map[string]any{"agents": []any{"a"}}}})
	if agents, _ := jsonObject(phone.response(6)["result"])["agents"].([]any); len(agents) != 1 {
		t.Fatal("proxied answer reaches the phone")
	}
	ui.notify("control.reply", map[string]any{"connection": connID,
		"message": map[string]any{"event": "agents.changed", "data": map[string]any{"n": 1}}})
	if intOf(t, phone.event("agents.changed")["n"]) != 1 {
		t.Fatal("proxied event")
	}
	ui.notify("control.broadcast", map[string]any{"event": "hello.all", "data": map[string]any{"a": 1}})
	if intOf(t, phone.event("hello.all")["a"]) != 1 {
		t.Fatal("broadcast")
	}
	if sent := h.core.Hub.Broadcast("hub.event", map[string]any{"b": 2}, rpc.Remote); sent != 1 {
		t.Fatalf("hub broadcast reached %d phones", sent)
	}
	if intOf(t, phone.event("hub.event")["b"]) != 2 {
		t.Fatal("hub broadcast event")
	}
	peer := h.core.Hub.Get(fmt.Sprintf("lan:%d", connID))
	if peer == nil || peer.Transport() != rpc.LAN || peer.Device().Key != phoneKey.public || !peer.Send("one.peer", nil) {
		t.Fatal("the paired phone is a hub peer")
	}
	phone.event("one.peer")

	var devices struct {
		Paired    []map[string]any `json:"paired"`
		Connected []map[string]any `json:"connected"`
	}
	_ = json.Unmarshal(ui.mustCall("control.devices", map[string]any{}), &devices)
	if len(devices.Paired) != 1 || len(devices.Connected) != 1 || intOf(t, devices.Connected[0]["connection"]) != connID ||
		devices.Connected[0]["relayed"] != false || devices.Paired[0]["key"] != phoneKey.public {
		t.Fatalf("devices %+v", devices)
	}
	var status map[string]any
	_ = json.Unmarshal(ui.mustCall("control.status", map[string]any{}), &status)
	if intOf(t, status["connections"]) != 1 || intOf(t, status["paired"]) != 1 || intOf(t, status["relayed"]) != 0 {
		t.Fatalf("status %v", status)
	}
	if !h.module.Busy() {
		t.Fatal("busy while a phone is connected")
	}

	content := append(bytes.Repeat([]byte("x"), 300000), "tail"...)
	phone.send(map[string]any{"id": 10, "method": "files.begin", "params": map[string]any{"name": "../photo.jpg",
		"size": len(content)}})
	transfer := jsonString(jsonObject(phone.response(10)["result"])["transfer"])
	if len(transfer) != 36 {
		t.Fatalf("transfer id %q", transfer)
	}
	next := 11
	for offset := 0; offset < len(content); offset += FileChunkBytes {
		end := min(offset+FileChunkBytes, len(content))
		phone.send(map[string]any{"id": next, "method": "files.chunk", "params": map[string]any{"transfer": transfer,
			"data": base64.StdEncoding.EncodeToString(content[offset:end])}})
		next++
	}
	if intOf(t, jsonObject(phone.response(next - 1)["result"])["received"]) != len(content) {
		t.Fatal("received count")
	}
	phone.send(map[string]any{"id": next, "method": "files.end", "params": map[string]any{"transfer": transfer}})
	end := phone.response(next)
	want := filepath.Join(h.module.ReceiveDir, "photo.jpg")
	if end["ok"] != true || jsonObject(end["result"])["path"] != want || jsonObject(end["result"])["name"] != "photo.jpg" {
		t.Fatalf("upload end %v", end)
	}
	if h.module.ReceiveDir != filepath.Join(filepath.Dir(h.core.Layout.LegacyState), "Downloads", "ForkMesh from phone") {
		t.Fatalf("receive dir %s", h.module.ReceiveDir)
	}
	saved, err := os.ReadFile(want)
	if err != nil || !bytes.Equal(saved, content) {
		t.Fatal("content intact")
	}
	received := ui.waitNote("ui.control.fileReceived", nil)
	if received["path"] != want || jsonObject(received["device"])["key"] != phoneKey.public {
		t.Fatalf("file received note %v", received)
	}
	phone.send(map[string]any{"id": next + 1, "method": "files.chunk", "params": map[string]any{"transfer": transfer, "data": "eA=="}})
	if phone.response(next + 1)["error"] != "unknown_transfer" {
		t.Fatal("a finished transfer is gone")
	}
	phone.send(map[string]any{"id": next + 2, "method": "files.begin", "params": map[string]any{"name": "a.txt", "size": 3}})
	second := jsonString(jsonObject(phone.response(next + 2)["result"])["transfer"])
	phone.send(map[string]any{"id": next + 3, "method": "files.end", "params": map[string]any{"transfer": second}})
	if phone.response(next + 3)["error"] != "incomplete" {
		t.Fatal("an incomplete upload is refused")
	}
	if _, err := os.Stat(filepath.Join(h.module.ReceiveDir, "a.txt.part")); !os.IsNotExist(err) {
		t.Fatal("the partial file is removed")
	}

	report := filepath.Join(t.TempDir(), "report.txt")
	writeFile(t, report, "hello phone")
	ui.mustCall("control.sendFile", map[string]any{"key": phoneKey.public, "path": report})
	begin := phone.event("file.begin")
	if begin["name"] != "report.txt" || intOf(t, begin["size"]) != 11 {
		t.Fatalf("file begin %v", begin)
	}
	chunk, _ := base64.StdEncoding.DecodeString(jsonString(phone.event("file.chunk")["data"]))
	if string(chunk) != "hello phone" || phone.event("file.end")["transfer"] != begin["transfer"] {
		t.Fatal("file bytes arrive")
	}
	big := filepath.Join(t.TempDir(), "big.bin")
	bigContent := bytes.Repeat([]byte{1, 2, 3, 4, 5}, 500000)
	if err := os.WriteFile(big, bigContent, 0o600); err != nil {
		t.Fatal(err)
	}
	ui.mustCall("control.sendFile", map[string]any{"key": phoneKey.public, "path": big})
	bigBegin := phone.wait(func(m map[string]any) bool {
		return m["event"] == "file.begin" && jsonObject(m["data"])["name"] == "big.bin"
	})
	bigID := jsonObject(bigBegin["data"])["transfer"]
	phone.wait(func(m map[string]any) bool {
		return m["event"] == "file.end" && jsonObject(m["data"])["transfer"] == bigID
	})
	var streamed []byte
	phone.count(func(m map[string]any) bool {
		if m["event"] == "file.chunk" && jsonObject(m["data"])["transfer"] == bigID {
			part, _ := base64.StdEncoding.DecodeString(jsonString(jsonObject(m["data"])["data"]))
			streamed = append(streamed, part...)
		}
		return false
	})
	if !bytes.Equal(streamed, bigContent) {
		t.Fatalf("a large file streams intact (%d of %d bytes)", len(streamed), len(bigContent))
	}
	if _, err := ui.call("control.sendFile", map[string]any{"key": "nobody", "path": report}); err == nil ||
		err.Error() != "That phone is not connected." {
		t.Fatalf("unknown phone: %v", err)
	}
	if _, err := ui.call("control.sendFile", map[string]any{"key": phoneKey.public, "path": report + ".missing"}); err == nil ||
		err.Error() != "No such file or directory" {
		t.Fatalf("missing file: %v", err)
	}
	if phone.count(func(m map[string]any) bool { return m["badFrame"] == true }) != 0 {
		t.Fatal("every frame authenticated in order")
	}

	phone.conn.Close()
	closed := ui.waitNote("ui.control.closed", func(p map[string]any) bool { return intOf(t, p["connection"]) == connID })
	if closed["reason"] == nil {
		t.Fatal("close reason")
	}
	eventually(t, "idle", func() bool { return !h.module.Busy() })
	if h.core.Hub.Get(fmt.Sprintf("lan:%d", connID)) != nil {
		t.Fatal("a closed phone leaves the hub")
	}
	if peer.Send("late", nil) {
		t.Fatal("a closed peer refuses events")
	}

	again := dialPhone(t, addr, phoneKey, false)
	if again.event("session")["paired"] != true {
		t.Fatal("a paired phone reconnects straight into control")
	}
	if ui.countNotes("ui.control.pairing", nil) != 1 {
		t.Fatal("no second prompt")
	}

	impostor := dialPhone(t, addr, newSigner(t), true)
	impostor.waitClosed()
	if impostor.count(func(map[string]any) bool { return true }) != 0 {
		t.Fatal("an impostor gets nothing")
	}

	strangerKey := newSigner(t)
	stranger := dialPhone(t, addr, strangerKey, false)
	stranger.event("session")
	strangerPairing := ui.waitNote("ui.control.pairing", func(p map[string]any) bool {
		return jsonObject(p["device"])["key"] == strangerKey.public
	})
	ui.mustCall("control.deny", map[string]any{"connection": strangerPairing["connection"]})
	stranger.event("pairing-denied")
	stranger.waitClosed()
	ui.waitNote("ui.control.pairingClosed", func(p map[string]any) bool {
		return intOf(t, p["connection"]) == intOf(t, strangerPairing["connection"])
	})
	if _, known := LoadDevices(h.module.StorePath)[strangerKey.public]; known {
		t.Fatal("a denied phone is not remembered")
	}

	ui.mustCall("control.forget", map[string]any{"key": phoneKey.public})
	again.waitClosed()
	ui.waitNote("ui.control.closed", func(p map[string]any) bool { return p["reason"] == "forgotten" })
	if _, known := LoadDevices(h.module.StorePath)[phoneKey.public]; known {
		t.Fatal("forgotten")
	}
}

func TestAccountOwnedPhonePairsWithoutCode(t *testing.T) {
	h := newHarness(t, nil)
	ui := h.attachUI()
	ui.mustCall("control.info", map[string]any{"name": "desk", "user": "alice", "node": "desk"})
	addr := h.startMesh()
	accountKey := newSigner(t)
	ui.setVerdicts(map[string]string{accountKey.public: VerdictOwned})

	phone := dialPhone(t, addr, accountKey, false)
	session := phone.event("session")
	if session["paired"] != true || session["code"] != nil {
		t.Fatalf("same-account phone paired without a code: %v", session)
	}
	ui.waitNote("ui.control.opened", nil)
	if ui.countNotes("ui.control.pairing", nil) != 0 {
		t.Fatal("no prompt for a same-account phone")
	}
	stored := LoadDevices(h.module.StorePath)[accountKey.public]
	if stored.Approval != "account" || stored.AccountCheckedMs == 0 {
		t.Fatalf("held by the account: %#v", stored)
	}
	phone.conn.Close()
	ui.waitNote("ui.control.closed", nil)

	ui.clearAsked()
	again := dialPhone(t, addr, accountKey, false)
	if again.event("session")["paired"] != true {
		t.Fatal("reconnect paired")
	}
	if asked := ui.askedKeys(); len(asked) != 1 || asked[0] != accountKey.public {
		t.Fatalf("a phone approved through the account is checked again: %v", asked)
	}
}

func TestPhonesWithoutDesktopUI(t *testing.T) {
	h := newHarness(t, func(m *Module, core *daemon.Core) { registerTestHandlers(core) })
	addr := h.startMesh()
	phoneKey := newSigner(t)
	phone := dialPhone(t, addr, phoneKey, false)
	if phone.event("session")["paired"] != false {
		t.Fatal("an unreachable account check leaves a new phone pending")
	}
	var connID int
	h.do(func() { connID = h.module.conns[0].id })
	h.do(func() { h.module.approvePairing(connID) })
	phone.wait(func(m map[string]any) bool {
		return m["event"] == "session" && jsonObject(m["data"])["paired"] == true
	})
	phone.send(map[string]any{"id": 1, "method": "agents.list"})
	if phone.response(1)["error"] != "desktop_unavailable" {
		t.Fatal("a proxied method answers at once with no desktop attached")
	}
	phone.send(map[string]any{"id": 2, "method": "test.echo", "params": map[string]any{"x": 1}})
	if phone.response(2)["ok"] != true {
		t.Fatal("native methods work without the desktop")
	}
}

func TestProxiedRequestsFailWhenTheDesktopLeaves(t *testing.T) {
	h := newHarness(t, nil)
	ui := h.attachUI()
	addr := h.startMesh()
	phoneKey := newSigner(t)
	ui.setVerdicts(map[string]string{phoneKey.public: VerdictOwned})
	phone := dialPhone(t, addr, phoneKey, false)
	phone.event("session")
	phone.send(map[string]any{"id": 7, "method": "agents.list"})
	ui.waitNote("ui.control.request", nil)
	ui.conn.Close()
	if phone.response(7)["error"] != "desktop_unavailable" {
		t.Fatal("an unanswered proxied request fails when the desktop detaches")
	}
}

func TestUIResyncOnAttach(t *testing.T) {
	h := newHarness(t, nil)
	first := h.attachUI()
	addr := h.startMesh()
	phoneKey := newSigner(t)
	first.setVerdicts(map[string]string{phoneKey.public: VerdictOwned})
	phone := dialPhone(t, addr, phoneKey, false)
	phone.event("session")
	opened := first.waitNote("ui.control.opened", nil)
	second := dialUI(t, h.core.IPC.Endpoint())
	reopened := second.waitNote("ui.control.opened", nil)
	if intOf(t, reopened["connection"]) != intOf(t, opened["connection"]) {
		t.Fatal("a new desktop UI learns the open connections")
	}
	devices := second.waitNote("ui.control.devices", nil)
	if connected, _ := devices["connected"].([]any); len(connected) != 1 {
		t.Fatalf("and the device lists: %v", devices)
	}
}

func TestOnUIChainsExistingCallback(t *testing.T) {
	calls := make(chan bool, 4)
	h := newHarness(t, func(m *Module, core *daemon.Core) {
		core.IPC.OnUI = func(conn *ipc.Conn, attached bool) { calls <- attached }
	})
	h.attachUI()
	select {
	case attached := <-calls:
		if !attached {
			t.Fatal("attach reported")
		}
	case <-time.After(waitTimeout):
		t.Fatal("the existing OnUI callback still runs")
	}
}

func TestDelegatedNativeHandlerIsProxied(t *testing.T) {
	h := newHarness(t, func(m *Module, core *daemon.Core) {
		core.Registry.Handle("agents.maybe", "test", func(ctx context.Context, req *rpc.Request) (any, error) {
			if req.Object()["native"] == true {
				return map[string]any{"native": true}, nil
			}
			if req.Object()["rewrite"] == true {
				return nil, rpc.DelegateWith([]byte(`{"rewritten":true}`))
			}
			return nil, rpc.ErrDelegate
		})
	})
	ui := h.attachUI()
	addr := h.startMesh()
	phoneKey := newSigner(t)
	ui.setVerdicts(map[string]string{phoneKey.public: VerdictOwned})
	phone := dialPhone(t, addr, phoneKey, false)
	phone.event("session")
	opened := ui.waitNote("ui.control.opened", nil)
	connID := intOf(t, opened["connection"])
	peer := h.module.Peer(connID)
	if peer == nil || peer.ID() != fmt.Sprintf("lan:%d", connID) || h.module.Peer(connID+100) != nil {
		t.Fatal("Peer looks a paired connection up by id")
	}
	if !peer.Send("terminal.output", map[string]any{"text": "hi"}) || phone.event("terminal.output")["text"] != "hi" {
		t.Fatal("an event pushed to a connection by id")
	}
	phone.send(map[string]any{"id": 1, "method": "agents.maybe", "params": map[string]any{"native": true}})
	if jsonObject(phone.response(1)["result"])["native"] != true {
		t.Fatal("native answer")
	}
	phone.send(map[string]any{"id": 2, "method": "agents.maybe"})
	request := ui.waitNote("ui.control.request", func(p map[string]any) bool { return p["method"] == "agents.maybe" })
	if intOf(t, request["id"]) != 2 {
		t.Fatalf("a delegated request goes to the desktop: %v", request)
	}
	ui.notify("control.reply", map[string]any{"connection": connID,
		"message": map[string]any{"id": 2, "ok": true, "result": map[string]any{"desktop": true}}})
	if jsonObject(phone.response(2)["result"])["desktop"] != true {
		t.Fatal("the desktop answers a delegated request")
	}
	phone.send(map[string]any{"id": 3, "method": "agents.maybe", "params": map[string]any{"rewrite": true}})
	rewritten := ui.waitNote("ui.control.request", func(p map[string]any) bool { return intOf(t, p["id"]) == 3 })
	if params := jsonObject(rewritten["params"]); params["rewritten"] != true || params["rewrite"] != nil {
		t.Fatalf("a delegation with params forwards those params: %v", rewritten)
	}
	phone.conn.Close()
	eventually(t, "peer gone", func() bool { return h.module.Peer(connID) == nil })
}

func TestRelayedChannel(t *testing.T) {
	pairedKey := newSigner(t)
	strangerKey := newSigner(t)
	store := filepath.Join(t.TempDir(), "paired.json")
	if err := os.WriteFile(store, storeSample(map[string]any{"key": pairedKey.public, "name": "Away phone",
		"user": "alice", "pairedAtMs": 1, "approval": "code", "platform": "ios", "lastSeenMs": 2}), 0o600); err != nil {
		t.Fatal(err)
	}
	h := newHarness(t, func(m *Module, core *daemon.Core) {
		m.StorePath = store
		registerTestHandlers(core)
	})
	ui := h.attachUI()
	ui.mustCall("control.info", map[string]any{"name": "desk", "user": "alice", "node": " Desk "})

	stranger := ui.relayPhone(strangerKey, "relay-session-0123456789", "ios")
	stranger.open()
	if stranger.event("pairing-denied")["reason"] != "pair_on_lan" {
		t.Fatal("an unpaired relay phone is told to pair on the LAN")
	}
	stranger.waitClosed()
	if ui.countNotes("ui.control.pairing", nil) != 0 {
		t.Fatal("no pairing prompt over the relay")
	}

	ui.clearAsked()
	phone := ui.relayPhone(pairedKey, "relay-session-paired-0001", "ios")
	phone.open()
	session := phone.event("session")
	if session["paired"] != true || session["transport"] != "relay" {
		t.Fatalf("paired phone controls over the relay: %v", session)
	}
	opened := ui.waitNote("ui.control.opened", func(p map[string]any) bool { return p["relayed"] == true })
	connID := intOf(t, opened["connection"])
	if peer := h.core.Hub.Get(fmt.Sprintf("relay:%d", connID)); peer == nil || peer.Transport() != rpc.Relay {
		t.Fatal("a relayed phone is a relay hub peer")
	}
	if len(ui.askedKeys()) != 0 {
		t.Fatal("a phone approved in person is not asked about")
	}

	phone.send(map[string]any{"id": 1, "method": "test.echo", "params": map[string]any{"x": 3}})
	if jsonObject(phone.response(1)["result"])["transport"] != "relay" {
		t.Fatal("native request over the relay")
	}
	phone.send(map[string]any{"id": 2, "method": "test.big"})
	if len(jsonString(jsonObject(phone.response(2)["result"])["blob"])) != 30000 {
		t.Fatal("a large response is reassembled")
	}
	chunked := 0
	for _, frame := range ui.relayFrames(phone.session, "data") {
		if frame["more"] == true {
			chunked++
		}
		if len(jsonString(frame["data"])) > RelayChunkChars || frame["node"] != "desk" {
			t.Fatalf("relay chunk %v", frame["node"])
		}
	}
	if chunked < 4 {
		t.Fatalf("a 30000-character response spans several chunks (%d)", chunked)
	}
	phone.send(map[string]any{"id": 3, "method": "agents.list"})
	request := ui.waitNote("ui.control.request", nil)
	if request["relayed"] != true || intOf(t, request["connection"]) != connID {
		t.Fatalf("proxied relay request %v", request)
	}
	ui.notify("control.reply", map[string]any{"connection": connID,
		"message": map[string]any{"id": 3, "ok": false, "error": "not_owner"}})
	if phone.response(3)["error"] != "not_owner" {
		t.Fatal("proxied error over the relay")
	}
	phone.send(map[string]any{"id": 4, "method": "files.begin", "params": map[string]any{"name": "a.txt", "size": 3}})
	if phone.response(4)["error"] != "lan_only" {
		t.Fatal("files stay on the LAN")
	}
	phone.send(map[string]any{"id": 5, "method": "files.browse.list"})
	if ui.waitNote("ui.control.request", func(p map[string]any) bool { return p["method"] == "files.browse.list" }) == nil {
		t.Fatal("files.browse. is a desktop family, not the transfer")
	}

	other := phone.chunkFrame("data", []byte("x"), false)
	other["node"] = "someone-else"
	ui.notify("control.relay.in", map[string]any{"frame": other})
	echoed := phone.chunkFrame("data", []byte("x"), false)
	echoed["from"] = "node"
	ui.notify("control.relay.in", map[string]any{"frame": echoed})
	phone.sendMu.Lock()
	phone.sendSeq -= 2
	phone.sendMu.Unlock()
	phone.send(map[string]any{"id": 6, "method": "ping"})
	phone.response(6)
	if phone.isClosed() {
		t.Fatal("misaddressed frames are ignored")
	}

	phone.sendMu.Lock()
	phone.sendSeq++
	phone.sendMu.Unlock()
	phone.send(map[string]any{"id": 7, "method": "ping"})
	phone.waitClosed()
	if phone.reason() != "relay chunk out of order" {
		t.Fatalf("out-of-order chunk closes the channel: %q", phone.reason())
	}
	ui.waitNote("ui.control.closed", func(p map[string]any) bool { return intOf(t, p["connection"]) == connID })
	if phone.outOfOrder.Load() {
		t.Fatal("desktop chunks arrive in sequence")
	}
}

func TestRelayResilience(t *testing.T) {
	phoneKey := newSigner(t)
	store := filepath.Join(t.TempDir(), "paired.json")
	if err := os.WriteFile(store, storeSample(map[string]any{"key": phoneKey.public, "name": "Away phone",
		"user": "alice", "pairedAtMs": 1, "approval": "code"}), 0o600); err != nil {
		t.Fatal(err)
	}
	h := newHarness(t, func(m *Module, core *daemon.Core) { m.StorePath = store })
	ui := h.attachUI()
	ui.mustCall("control.info", map[string]any{"name": "desk", "user": "alice", "node": "desk"})

	stale := ui.relayPhone(phoneKey, "relay-session-stale-00001", "ios")
	ui.notify("control.relay.in", map[string]any{"frame": stale.chunkFrame("data", []byte("x"), false)})
	ui.notify("control.relay.in", map[string]any{"frame": stale.chunkFrame("data", []byte("y"), false)})
	stale.waitClosed()
	if stale.reason() != "channel_gone" {
		t.Fatalf("a frame for a gone channel is answered: %q", stale.reason())
	}
	h.do(func() {})
	time.Sleep(50 * time.Millisecond)
	if n := len(ui.relayFrames(stale.session, "close")); n != 1 {
		t.Fatalf("a dead channel is told only once (%d)", n)
	}

	phone := ui.relayPhone(phoneKey, "relay-session-resilient-01", "ios")
	phone.open()
	if phone.event("session")["paired"] != true {
		t.Fatal("code-paired phone over the relay")
	}
	opened := ui.waitNote("ui.control.opened", nil)
	ui.relayUp.Store(false)
	ui.notify("control.broadcast", map[string]any{"event": "agents.changed"})
	ui.waitNote("ui.control.closed", func(p map[string]any) bool {
		return intOf(t, p["connection"]) == intOf(t, opened["connection"]) && p["reason"] == "relay unavailable"
	})
	h.do(func() {})
	eventually(t, "undelivered close noted", func() bool {
		noted := false
		h.do(func() { noted = containsString(h.module.relayUndelivered, phone.session) })
		return noted
	})
	ui.relayUp.Store(true)
	if len(ui.relayFrames(phone.session, "close")) != 0 {
		t.Fatal("no close while the relay is down")
	}
	ui.notify("control.relay.restored", map[string]any{})
	phone.waitClosed()
	if phone.reason() != "relay_reconnected" {
		t.Fatalf("the phone is told once the relay is back: %q", phone.reason())
	}
	ui.notify("control.relay.restored", map[string]any{})
	h.do(func() {})
	time.Sleep(50 * time.Millisecond)
	if len(ui.relayFrames(phone.session, "close")) != 1 {
		t.Fatal("told once")
	}

	idle := ui.relayPhone(phoneKey, "relay-session-idle-000001", "ios")
	idle.open()
	idle.event("session")
	now := time.Now().UnixMilli()
	connected := func() bool {
		found := false
		h.do(func() {
			for _, conn := range h.module.conns {
				if conn.state == statePaired && conn.device.Key == phoneKey.public {
					found = true
				}
			}
		})
		return found
	}
	h.do(func() { h.module.maintain(now + 60*1000) })
	if !connected() {
		t.Fatal("a quiet minute keeps the channel")
	}
	h.do(func() { h.module.maintain(now + 16*60*1000) })
	idle.waitClosed()
	if idle.reason() != "idle" || connected() {
		t.Fatalf("a relayed channel silent for 15 minutes is closed: %q", idle.reason())
	}

	for i := 0; i < 16; i++ {
		opener := ui.relayPhone(phoneKey, fmt.Sprintf("relay-session-half-open-%02d", i), "ios")
		ui.notify("control.relay.in", map[string]any{"frame": opener.chunkFrame("open", []byte("{"), true)})
	}
	extra := ui.relayPhone(phoneKey, "relay-session-half-open-xx", "ios")
	ui.notify("control.relay.in", map[string]any{"frame": extra.chunkFrame("open", []byte("{"), true)})
	extra.waitClosed()
	if errors := ui.relayFrames(extra.session, "error"); len(errors) != 1 ||
		errors[0]["reason"] != "This desktop has too many channels opening." {
		t.Fatalf("too many half-open channels are refused: %v", errors)
	}
	for i := 0; i < 16; i++ {
		if len(ui.relayFrames(fmt.Sprintf("relay-session-half-open-%02d", i), "")) != 0 {
			t.Fatal("half-open channels wait for the rest of the hello")
		}
	}
	h.do(func() { h.module.maintain(time.Now().UnixMilli() + 31*1000) })
	after := ui.relayPhone(phoneKey, "relay-session-after-expiry", "ios")
	after.open()
	if after.event("session")["paired"] != true {
		t.Fatal("half-open channels expire and make room")
	}
}

func TestAccountRevocation(t *testing.T) {
	accountPhone := newSigner(t)
	legacyPhone := newSigner(t)
	codePhone := newSigner(t)
	now := time.Now().UnixMilli()
	store := filepath.Join(t.TempDir(), "paired.json")
	if err := os.WriteFile(store, storeSample(
		map[string]any{"key": accountPhone.public, "name": "Account phone", "pairedAtMs": 1, "approval": "account",
			"accountCheckedMs": now - 60*60*1000},
		map[string]any{"key": legacyPhone.public, "name": "Legacy phone", "pairedAtMs": 2},
		map[string]any{"key": codePhone.public, "name": "Code phone", "pairedAtMs": 3, "approval": "code"},
	), 0o600); err != nil {
		t.Fatal(err)
	}
	h := newHarness(t, func(m *Module, core *daemon.Core) { m.StorePath = store })
	ui := h.attachUI()
	ui.mustCall("control.info", map[string]any{"name": "desk", "user": "alice", "node": "desk"})
	verdicts := map[string]string{}
	connected := func(key string) bool {
		found := false
		h.do(func() {
			for _, conn := range h.module.conns {
				if conn.state == statePaired && conn.device.Key == key {
					found = true
				}
			}
		})
		return found
	}
	paired := func(key string) bool {
		found := false
		h.do(func() { _, found = h.module.devices[key] })
		return found
	}

	verdicts[accountPhone.public] = VerdictUnreachable
	ui.setVerdicts(verdicts)
	phone := ui.relayPhone(accountPhone, "relay-session-grace-000001", "ios")
	phone.open()
	if phone.event("session")["paired"] != true || !connected(accountPhone.public) {
		t.Fatal("an account check that cannot be made admits a phone confirmed today")
	}

	verdicts[legacyPhone.public] = VerdictUnreachable
	ui.setVerdicts(verdicts)
	legacy := ui.relayPhone(legacyPhone, "relay-session-legacy-00001", "ios")
	legacy.open()
	if legacy.event("pairing-denied")["reason"] != "account_check_failed" {
		t.Fatal("a never-confirmed phone is refused over the relay when the account cannot be asked")
	}
	legacy.waitClosed()
	if !paired(legacyPhone.public) {
		t.Fatal("and is not forgotten")
	}

	verdicts[legacyPhone.public] = VerdictOwned
	ui.setVerdicts(verdicts)
	legacy = ui.relayPhone(legacyPhone, "relay-session-legacy-00002", "ios")
	legacy.open()
	legacy.event("session")
	if !connected(legacyPhone.public) || LoadDevices(store)[legacyPhone.public].Approval != "account" {
		t.Fatal("a confirmed legacy phone connects and is now held by the account")
	}

	ui.clearAsked()
	code := ui.relayPhone(codePhone, "relay-session-code-0000001", "ios")
	code.open()
	code.event("session")
	if !connected(codePhone.public) || len(ui.askedKeys()) != 0 {
		t.Fatal("a phone approved in person is not asked about")
	}

	verdicts[accountPhone.public] = VerdictOwned
	verdicts[legacyPhone.public] = VerdictNotOwned
	ui.setVerdicts(verdicts)
	ui.clearAsked()
	h.do(func() { h.module.maintain(time.Now().UnixMilli()) })
	eventually(t, "grace recheck", func() bool { return len(ui.askedKeys()) == 1 })
	time.Sleep(50 * time.Millisecond)
	if asked := ui.askedKeys(); len(asked) != 1 || asked[0] != accountPhone.public {
		t.Fatalf("a phone admitted on grace is rechecked at the next sweep, a confirmed one only later: %v", asked)
	}
	eventually(t, "recheck settled", func() bool {
		settled := false
		h.do(func() {
			for _, conn := range h.module.conns {
				if conn.device.Key == accountPhone.public && !conn.accountCheckInFlight {
					settled = true
				}
			}
		})
		return settled
	})
	ui.clearAsked()
	phone.send(map[string]any{"id": 1, "method": "ping"})
	legacy.send(map[string]any{"id": 1, "method": "ping"})
	code.send(map[string]any{"id": 1, "method": "ping"})
	phone.response(1)
	legacy.response(1)
	code.response(1)
	h.do(func() { h.module.maintain(time.Now().UnixMilli() + 11*60*1000) })
	legacy.waitClosed()
	asked := strings.Join(ui.askedKeys(), ",")
	if !strings.Contains(asked, accountPhone.public) || !strings.Contains(asked, legacyPhone.public) ||
		strings.Contains(asked, codePhone.public) {
		t.Fatalf("account-held channels are rechecked every ten minutes: %v", asked)
	}
	if legacy.reason() != "revoked" {
		t.Fatalf("the phone is told it was revoked: %q", legacy.reason())
	}
	if connected(legacyPhone.public) || paired(legacyPhone.public) {
		t.Fatal("a phone removed from the account is cut off and forgotten")
	}
	if !connected(accountPhone.public) || !connected(codePhone.public) {
		t.Fatal("a phone still on the account stays; the in-person phone is untouched")
	}

	verdicts[accountPhone.public] = VerdictNotOwned
	ui.setVerdicts(verdicts)
	again := ui.relayPhone(accountPhone, "relay-session-revoked-0001", "ios")
	again.open()
	if again.event("pairing-denied")["reason"] != "pair_on_lan" {
		t.Fatal("a revoked phone cannot reconnect over the relay")
	}
	again.waitClosed()
	if paired(accountPhone.public) {
		t.Fatal("the revoked phone is forgotten")
	}
}

func TestBrowserIsCheckedEveryTimeAndNeverRemembered(t *testing.T) {
	h := newHarness(t, nil)
	ui := h.attachUI()
	ui.mustCall("control.info", map[string]any{"name": "desk", "user": "alice", "node": "desk"})
	browserKey := newSigner(t)
	ui.setVerdicts(map[string]string{browserKey.public: VerdictOwned})
	for round := 0; round < 2; round++ {
		ui.clearAsked()
		browser := ui.relayPhone(browserKey, fmt.Sprintf("relay-session-browser-000%d", round), "web")
		browser.open()
		if browser.event("session")["paired"] != true {
			t.Fatal("same-account browser paired over the relay")
		}
		if asked := ui.askedKeys(); len(asked) != 1 || asked[0] != browserKey.public {
			t.Fatalf("a browser is checked on every connect: %v", asked)
		}
		if _, known := LoadDevices(h.module.StorePath)[browserKey.public]; known {
			t.Fatal("a browser is never remembered")
		}
		ui.notify("control.relay.in", map[string]any{"frame": map[string]any{"type": "control", "action": "close",
			"session": browser.session, "node": "desk", "from": "browser"}})
		ui.waitNote("ui.control.closed", func(p map[string]any) bool { return p["reason"] == "" })
		ui.mu.Lock()
		ui.seen = nil
		ui.mu.Unlock()
	}
	ui.setVerdicts(map[string]string{})
	revoked := ui.relayPhone(browserKey, "relay-session-browser-revoked", "web")
	revoked.open()
	revoked.waitClosed()
}

func TestPairedDevicesStoreCompat(t *testing.T) {
	keyA := ToBase64Url(bytes.Repeat([]byte{1}, 32))
	keyB := ToBase64Url(bytes.Repeat([]byte{2}, 32))
	sample := "{\n" +
		"    \"devices\": [\n" +
		"        {\n" +
		"            \"approval\": \"code\",\n" +
		"            \"key\": \"" + keyB + "\",\n" +
		"            \"lastSeenMs\": 1759400000000,\n" +
		"            \"name\": \"Pixel\",\n" +
		"            \"pairedAtMs\": 1759300000000,\n" +
		"            \"platform\": \"android\",\n" +
		"            \"user\": \"alice\"\n" +
		"        },\n" +
		"        {\n" +
		"            \"accountCheckedMs\": 1759400000123,\n" +
		"            \"approval\": \"account\",\n" +
		"            \"key\": \"" + keyA + "\",\n" +
		"            \"lastSeenMs\": 1759400000000,\n" +
		"            \"name\": \"  iPhone  \",\n" +
		"            \"pairedAtMs\": 1759200000000,\n" +
		"            \"platform\": \"ios\",\n" +
		"            \"user\": \"alice\"\n" +
		"        },\n" +
		"        {\n" +
		"            \"key\": \"not-a-key\",\n" +
		"            \"name\": \"broken\",\n" +
		"            \"pairedAtMs\": 1\n" +
		"        }\n" +
		"    ]\n" +
		"}\n"
	path := filepath.Join(t.TempDir(), "local_control", "paired-devices.json")
	if err := os.MkdirAll(filepath.Dir(path), 0o700); err != nil {
		t.Fatal(err)
	}
	writeFile(t, path, sample)
	devices := LoadDevices(path)
	if len(devices) != 2 {
		t.Fatalf("an invalid key is skipped: %v", devices)
	}
	a := devices[keyA]
	if a.Name != "iPhone" || a.Approval != "account" || a.AccountCheckedMs != 1759400000123 || a.PairedAtMs != 1759200000000 ||
		a.Platform != "ios" || a.User != "alice" || a.LastSeenMs != 1759400000000 {
		t.Fatalf("device A %#v", a)
	}
	if b := devices[keyB]; b.Approval != "code" || b.AccountCheckedMs != 0 || b.Platform != "android" {
		t.Fatalf("device B %#v", b)
	}
	if err := SaveDevices(path, devices); err != nil {
		t.Fatal(err)
	}
	raw, _ := os.ReadFile(path)
	text := string(raw)
	if !strings.HasPrefix(text, "{\n    \"devices\": [\n        {\n            \"accountCheckedMs\": 1759400000123,") ||
		!strings.HasSuffix(text, "}\n") {
		t.Fatalf("Qt indented layout, sorted keys, oldest pairing first:\n%s", text)
	}
	if strings.Count(text, "\"approval\"") != 2 || strings.Count(text, "\"accountCheckedMs\"") != 1 {
		t.Fatalf("empty approval and zero accountCheckedMs are omitted:\n%s", text)
	}
	if strings.Index(text, keyA) > strings.Index(text, keyB) {
		t.Fatal("sorted by pairedAtMs")
	}
	reloaded := LoadDevices(path)
	if len(reloaded) != 2 || reloaded[keyA] != a || reloaded[keyB] != devices[keyB] {
		t.Fatal("round trip")
	}
	entries, _ := os.ReadDir(filepath.Dir(path))
	if len(entries) != 1 {
		t.Fatalf("no temp file left behind: %v", entries)
	}
	if err := SaveDevices(path, map[string]Device{}); err != nil {
		t.Fatal(err)
	}
	if empty := LoadDevices(path); len(empty) != 0 {
		t.Fatal("an empty store")
	}
	var parsed map[string][]any
	raw, _ = os.ReadFile(path)
	if json.Unmarshal(raw, &parsed) != nil || parsed["devices"] == nil {
		t.Fatalf("an empty store still lists devices: %s", raw)
	}

	writeFile(t, path, sample)
	h := newHarness(t, func(m *Module, core *daemon.Core) { m.StorePath = path })
	ui := h.attachUI()
	var listed struct {
		Paired []map[string]any `json:"paired"`
	}
	_ = json.Unmarshal(ui.mustCall("control.devices", nil), &listed)
	if len(listed.Paired) != 2 || listed.Paired[0]["key"] != keyA || listed.Paired[1]["approval"] != "code" {
		t.Fatalf("the core lists the C++ store: %v", listed.Paired)
	}
	ui.mustCall("control.forget", map[string]any{"key": keyB})
	if _, still := LoadDevices(path)[keyB]; still || len(LoadDevices(path)) != 1 {
		t.Fatal("forget rewrites the store")
	}
}

func TestUIMethodsRefusePhones(t *testing.T) {
	h := newHarness(t, nil)
	phone := &phonePeer{id: "lan:99", transport: rpc.LAN, done: make(chan struct{})}
	for _, method := range []string{"control.info", "control.relay.in", "control.reply", "control.broadcast",
		"control.devices", "control.forget", "control.approve", "control.deny", "control.sendFile", "control.status",
		"control.relay.restored"} {
		_, err := h.core.Registry.Dispatch(context.Background(), &rpc.Request{Peer: phone, ID: 1, Method: method,
			Params: json.RawMessage("{}")})
		if rpc.Code(err) != "forbidden" {
			t.Fatalf("%s: %v", method, err)
		}
	}
}

func TestReceivedFilesDirectoryHonoursXDG(t *testing.T) {
	h := newHarness(t, nil)
	layout := h.core.Layout
	home := filepath.Dir(layout.LegacyState)
	configHome := filepath.Dir(layout.LegacyConfig)
	if err := os.MkdirAll(configHome, 0o700); err != nil {
		t.Fatal(err)
	}
	writeFile(t, filepath.Join(configHome, "user-dirs.dirs"),
		"# comment\nXDG_DESKTOP_DIR=\"$HOME/Desktop\"\nXDG_DOWNLOAD_DIR=\"$HOME/Fetched/\"\n")
	got := ReceivedFilesDirectory(layout)
	want := filepath.Join(home, "Fetched", "ForkMesh from phone")
	if runtime.GOOS == "windows" || runtime.GOOS == "darwin" {
		want = filepath.Join(home, "Downloads", "ForkMesh from phone")
	}
	if got != want {
		t.Fatalf("receive dir %s, want %s", got, want)
	}
	writeFile(t, filepath.Join(configHome, "user-dirs.dirs"), "XDG_DOWNLOAD_DIR=\"/srv/drop\"\n")
	if runtime.GOOS == "linux" && ReceivedFilesDirectory(layout) != filepath.Join("/srv/drop", "ForkMesh from phone") {
		t.Fatal("an absolute XDG download dir")
	}
}

func TestStopClosesPhoneSockets(t *testing.T) {
	h := newHarness(t, nil)
	addr := h.startMesh()
	phone := dialPhone(t, addr, newSigner(t), false)
	phone.event("session")
	h.module.cancel()
	phone.waitClosed()
	eventually(t, "idle", func() bool { return !h.module.Busy() })
}
