package control

import (
	"bufio"
	"encoding/base64"
	"encoding/json"
	"testing"
	"time"

	"forkmesh.com/desktop-core/internal/ipc"
	"forkmesh.com/desktop-core/internal/rpc"
)

func TestAdoptOverIPCPipe(t *testing.T) {
	h := newHarness(t, nil)
	endpoint := h.listenIPC()
	conn, err := ipc.Dial(endpoint, 2*time.Second)
	if err != nil {
		t.Fatal(err)
	}
	phoneKey := newSigner(t)
	hs := newHandshake(phoneKey, "android", "Pixel")
	line, _ := json.Marshal(map[string]any{"id": 1, "method": "control.adopt", "params": map[string]any{
		"token": endpoint.Token, "peer": "192.168.1.20:50123", "pass": "pipe",
		"hello": base64.StdEncoding.EncodeToString(hs.hello()), "rest": ""}})
	if _, err := conn.Write(append(line, '\n')); err != nil {
		t.Fatal(err)
	}
	reader := bufio.NewReader(conn)
	reply, err := reader.ReadBytes('\n')
	if err != nil {
		t.Fatal(err)
	}
	var message rpc.Message
	if json.Unmarshal(reply, &message) != nil || message.OK == nil || !*message.OK {
		t.Fatalf("adopt refused: %s", reply)
	}
	phone := startPhoneWith(t, conn, reader, hs, false, false)
	session := phone.event("session")
	if session["paired"] != false || session["code"] != hs.code || hs.serverKey != h.core.Identity.PublicKey() {
		t.Fatalf("handshake through the pipe: %v", session)
	}
	phone.send(map[string]any{"id": 1, "method": "ping"})
	if phone.response(1)["ok"] != true {
		t.Fatal("ping through the pipe")
	}
	var device Device
	h.do(func() {
		if len(h.module.conns) == 1 {
			device = h.module.conns[0].device
		}
	})
	if device.Key != phoneKey.public || device.Platform != "android" || device.Name != "Pixel" {
		t.Fatalf("device %#v", device)
	}
}

func TestAdoptRestBytesAndBadHello(t *testing.T) {
	h := newHarness(t, nil)
	endpoint := h.listenIPC()
	conn, err := ipc.Dial(endpoint, 2*time.Second)
	if err != nil {
		t.Fatal(err)
	}
	line, _ := json.Marshal(map[string]any{"id": 1, "method": "control.adopt", "params": map[string]any{
		"token": endpoint.Token, "peer": "x", "pass": "pipe", "hello": base64.StdEncoding.EncodeToString([]byte(`{"t":"nope"}`))}})
	_, _ = conn.Write(append(line, '\n'))
	reader := bufio.NewReader(conn)
	if _, err := reader.ReadBytes('\n'); err != nil {
		t.Fatal(err)
	}
	_ = conn.SetReadDeadline(time.Now().Add(waitTimeout))
	if _, err := reader.ReadByte(); err == nil {
		t.Fatal("a bad hello closes the stream")
	}
	if h.module.Busy() {
		t.Fatal("nothing stays open")
	}
}
