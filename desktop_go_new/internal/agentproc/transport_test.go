//go:build !windows

package agentproc

import (
	"net"
	"os"
	"path/filepath"
	"testing"
	"time"
)

func tcpShims(m *Module) { m.useTCP = true }

func TestTCPShimTransportSurvivesACoreRestart(t *testing.T) {
	first := newHarness(t, "", tcpShims)
	first.start("agent-tcp", "echo", nil)
	dir := filepath.Join(first.mod.root, "agent-tcp")
	var sp spec
	if !readJSONFile(filepath.Join(dir, "spec.json"), &sp) || sp.Transport != transportTCP || sp.Socket != "" {
		t.Fatalf("spec %+v", sp)
	}
	if _, err := os.Stat(filepath.Join(dir, "s")); !os.IsNotExist(err) {
		t.Fatalf("a TCP shim made a Unix socket: %v", err)
	}
	var endpoint tcpEndpoint
	if !readJSONFile(filepath.Join(dir, tcpFile), &endpoint) || len(endpoint.Token) != 48 {
		t.Fatalf("endpoint %+v", endpoint)
	}
	s := &stream{}
	first.write("agent-tcp", "one\nerr warn\n")
	first.until(first.ui, s, func(s *stream) bool { return s.out.String() == "one\n" && s.err.String() == "warn\n" })
	first.call(first.ui, "core.agentproc.ack", map[string]any{"key": "agent-tcp", "stdoutOffset": 4}, nil)
	pid := int(first.list()["agent-tcp"]["pid"].(float64))
	first.mod.Stop()
	if !pidAlive(pid) {
		t.Fatal("a soft core stop killed the agent")
	}
	second := newHarness(t, first.mod.root, tcpShims)
	entry := second.list()["agent-tcp"]
	if entry == nil || !entry["running"].(bool) || int(entry["pid"].(float64)) != pid {
		t.Fatalf("adopted %v", entry)
	}
	second.call(second.ui, "core.agentproc.attach", map[string]any{"key": "agent-tcp", "stdoutOffset": -1}, nil)
	second.write("agent-tcp", "two\nexit 4\n")
	resumed := &stream{}
	second.until(second.ui, resumed, exited)
	if resumed.out.String() != "two\n" || resumed.exit["code"].(float64) != 4 {
		t.Fatalf("resumed %q exit %v", resumed.out.String(), resumed.exit)
	}
	waitFor(t, "the shim removes its port file", func() bool {
		_, err := os.Stat(filepath.Join(dir, tcpFile))
		return os.IsNotExist(err)
	})
}

func TestTCPShimRefusesAConnectionWithoutTheToken(t *testing.T) {
	h := newHarness(t, "", tcpShims)
	h.start("agent-auth", "echo", nil)
	var endpoint tcpEndpoint
	if !readJSONFile(filepath.Join(h.mod.root, "agent-auth", tcpFile), &endpoint) {
		t.Fatal("no endpoint")
	}
	for _, token := range []string{"", "0123456789abcdef0123456789abcdef0123456789abcdef"} {
		conn, err := net.DialTimeout("tcp", endpoint.Addr, time.Second)
		if err != nil {
			t.Fatal(err)
		}
		if token != "" {
			_ = writeFrame(conn, frameAuth, []byte(token))
		} else {
			_ = writeFrame(conn, frameKill, nil)
		}
		_ = conn.SetReadDeadline(time.Now().Add(3 * time.Second))
		buffer := make([]byte, 64)
		if n, err := conn.Read(buffer); err == nil || n > 0 {
			t.Fatalf("token %q: the shim answered %d bytes", token, n)
		}
		conn.Close()
	}
	s := &stream{}
	h.write("agent-auth", "still\n")
	h.until(h.ui, s, outHas("still\n"))
	h.write("agent-auth", "exit 0\n")
	h.until(h.ui, s, exited)
}
