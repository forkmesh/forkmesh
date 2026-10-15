package ipc

import (
	"net"
	"os"
	"path/filepath"
	"runtime"
	"testing"
)

func TestPeerPIDOnUnixSocket(t *testing.T) {
	if runtime.GOOS != "linux" && runtime.GOOS != "darwin" {
		t.Skip("peer credentials are read on Linux and macOS only")
	}
	dir, err := os.MkdirTemp("", "fmpc")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = os.Remove(filepath.Join(dir, "s")); _ = os.Remove(dir) })
	listener, err := net.Listen("unix", filepath.Join(dir, "s"))
	if err != nil {
		t.Fatal(err)
	}
	defer listener.Close()
	accepted := make(chan net.Conn, 1)
	go func() {
		conn, _ := listener.Accept()
		accepted <- conn
	}()
	client, err := net.Dial("unix", filepath.Join(dir, "s"))
	if err != nil {
		t.Fatal(err)
	}
	defer client.Close()
	server := <-accepted
	defer server.Close()
	conn := &Conn{raw: server}
	pid, ok := conn.PeerPID()
	if !ok || pid != os.Getpid() {
		t.Fatalf("PeerPID = %d %v, want %d", pid, ok, os.Getpid())
	}
	tcp := &Conn{raw: &net.TCPConn{}}
	if _, ok := tcp.PeerPID(); ok {
		t.Fatal("a TCP connection has no verified peer pid")
	}
}
