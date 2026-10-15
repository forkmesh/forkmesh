//go:build !windows

package control

import (
	"encoding/base64"
	"encoding/json"
	"net"
	"syscall"
	"testing"
	"time"
)

func TestAdoptOverIPCDescriptor(t *testing.T) {
	h := newHarness(t, nil)
	endpoint := h.listenIPC()
	if endpoint.Unix == "" {
		t.Skip("no unix socket")
	}
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer listener.Close()
	phoneSide, err := net.Dial("tcp", listener.Addr().String())
	if err != nil {
		t.Fatal(err)
	}
	defer phoneSide.Close()
	meshSide, err := listener.Accept()
	if err != nil {
		t.Fatal(err)
	}
	file, err := meshSide.(*net.TCPConn).File()
	if err != nil {
		t.Fatal(err)
	}
	meshSide.Close()
	defer file.Close()

	phoneKey := newSigner(t)
	hs := newHandshake(phoneKey, "ios", "Phone")
	ipcConn, err := net.DialTimeout("unix", endpoint.Unix, 2*time.Second)
	if err != nil {
		t.Fatal(err)
	}
	defer ipcConn.Close()
	line, _ := json.Marshal(map[string]any{"id": 1, "method": "control.adopt", "params": map[string]any{
		"token": endpoint.Token, "peer": phoneSide.LocalAddr().String(), "pass": "fd",
		"hello": base64.StdEncoding.EncodeToString(hs.hello())}})
	rights := syscall.UnixRights(int(file.Fd()))
	if _, _, err := ipcConn.(*net.UnixConn).WriteMsgUnix(append(line, '\n'), rights, nil); err != nil {
		t.Fatal(err)
	}
	file.Close()
	phone := startPhoneWith(t, phoneSide, nil, hs, false, false)
	if phone.event("session")["code"] != hs.code {
		t.Fatal("handshake over the passed descriptor")
	}
	phone.send(map[string]any{"id": 1, "method": "ping"})
	if phone.response(1)["ok"] != true {
		t.Fatal("ping over the passed descriptor")
	}
}
