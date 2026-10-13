//go:build linux || darwin

package terminal

import (
	"encoding/base64"
	"encoding/json"
	"errors"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"testing"
	"time"

	"forkmesh.com/desktop-core/internal/coretest"
	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/rpc"
)

type started struct {
	Terminal string `json:"terminal"`
	Pid      int    `json:"pid"`
	Running  bool   `json:"running"`
	Cols     int    `json:"cols"`
	Rows     int    `json:"rows"`
	Cwd      string `json:"cwd"`
	Replay   string `json:"replay"`
	Device   string `json:"device"`
	Provider string `json:"provider"`
	Label    string `json:"label"`
	Session  int    `json:"session"`
}

type exitEvent struct {
	Terminal string `json:"terminal"`
	Code     int    `json:"code"`
}

func setup(t *testing.T) (*daemon.Core, *Module, *coretest.Peer) {
	t.Helper()
	core := coretest.New(t)
	module := New()
	coretest.Start(t, core, module)
	return core, module, coretest.NewPeer("ui", rpc.UI)
}

func startShell(t *testing.T, core *daemon.Core, peer rpc.Peer, script string, extra map[string]any) started {
	t.Helper()
	params := map[string]any{"program": "/bin/sh", "args": []string{"-c", script}, "cwd": t.TempDir()}
	for k, v := range extra {
		params[k] = v
	}
	var out started
	coretest.MustCall(t, core, peer, "core.terminal.start", params, &out)
	if !validTerminalID(out.Terminal) || out.Pid <= 0 || !out.Running {
		t.Fatalf("bad start answer %+v", out)
	}
	return out
}

func send(t *testing.T, core *daemon.Core, peer rpc.Peer, id, text string) {
	t.Helper()
	coretest.MustCall(t, core, peer, "core.terminal.input",
		map[string]any{"terminal": id, "data": base64.StdEncoding.EncodeToString([]byte(text))}, nil)
}

type stream struct {
	text string
	exit *exitEvent
}

func (s *stream) until(t *testing.T, peer *coretest.Peer, id string, done func(*stream) bool) {
	t.Helper()
	deadline := time.After(10 * time.Second)
	for !done(s) {
		select {
		case ev := <-peer.Events:
			switch ev.Name {
			case "core.terminal.output":
				var out struct {
					Terminal string `json:"terminal"`
					Data     string `json:"data"`
					Reset    bool   `json:"reset"`
				}
				if err := json.Unmarshal(ev.Data, &out); err != nil || out.Terminal != id {
					t.Fatalf("bad output event %s", ev.Data)
				}
				raw, err := base64.StdEncoding.DecodeString(out.Data)
				if err != nil {
					t.Fatalf("output not base64: %v", err)
				}
				if out.Reset {
					s.text = ""
				}
				s.text += string(raw)
			case "core.terminal.exit":
				var out exitEvent
				if err := json.Unmarshal(ev.Data, &out); err != nil || out.Terminal != id {
					t.Fatalf("bad exit event %s", ev.Data)
				}
				s.exit = &out
			}
		case <-deadline:
			t.Fatalf("timed out; output so far %q exit %v", s.text, s.exit)
		}
	}
}

func contains(text string) func(*stream) bool {
	return func(s *stream) bool { return strings.Contains(s.text, text) }
}

func exited(s *stream) bool { return s.exit != nil }

func waitGone(t *testing.T, pid int) {
	t.Helper()
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		if err := syscall.Kill(pid, 0); errors.Is(err, syscall.ESRCH) {
			return
		}
		time.Sleep(20 * time.Millisecond)
	}
	t.Fatalf("pid %d still alive", pid)
}

func waitIdle(t *testing.T, module *Module) {
	t.Helper()
	deadline := time.Now().Add(5 * time.Second)
	for module.Busy() {
		if time.Now().After(deadline) {
			t.Fatal("module still busy")
		}
		time.Sleep(10 * time.Millisecond)
	}
}

func TestEchoInputExit(t *testing.T) {
	core, module, ui := setup(t)
	out := startShell(t, core, ui, `echo hi; read x; echo got $x`, nil)
	if out.Device != "ui" || out.Cols != 80 || out.Rows != 24 || out.Replay != "" {
		t.Fatalf("start answer %+v", out)
	}
	if !module.Busy() {
		t.Fatal("not busy with a running session")
	}
	var s stream
	s.until(t, ui, out.Terminal, contains("hi"))
	send(t, core, ui, out.Terminal, "abc\n")
	s.until(t, ui, out.Terminal, exited)
	if !strings.Contains(s.text, "got abc") || s.exit.Code != 0 {
		t.Fatalf("output %q exit %+v", s.text, s.exit)
	}
	waitIdle(t, module)
	var list struct {
		Terminals []started `json:"terminals"`
	}
	coretest.MustCall(t, core, ui, "core.terminal.list", nil, &list)
	if len(list.Terminals) != 0 {
		t.Fatalf("ended session still listed: %+v", list.Terminals)
	}
}

func TestExitCodeCwdAndEnv(t *testing.T) {
	core, _, ui := setup(t)
	dir := t.TempDir()
	out := startShell(t, core, ui, `pwd; echo "v=$FOO t=$TERM k=${ANTHROPIC_API_KEY-none}"; exit 3`, map[string]any{
		"cwd": dir, "env": map[string]string{"FOO": "bar"}, "stripCredentials": true,
		"provider": "codex", "session": 7,
	})
	if out.Provider != "codex" || out.Label != "Codex" || out.Session != 7 {
		t.Fatalf("meta %+v", out)
	}
	var s stream
	s.until(t, ui, out.Terminal, exited)
	real, _ := filepath.EvalSymlinks(dir)
	if !strings.Contains(s.text, real) && !strings.Contains(s.text, dir) {
		t.Fatalf("cwd not used: %q", s.text)
	}
	if !strings.Contains(s.text, "v=bar t=xterm-256color k=none") || s.exit.Code != 3 {
		t.Fatalf("output %q exit %+v", s.text, s.exit)
	}
}

func TestResize(t *testing.T) {
	core, _, ui := setup(t)
	out := startShell(t, core, ui, `stty size; read x; stty size; read y`, map[string]any{"cols": 100.0, "rows": 30.0})
	if out.Cols != 100 || out.Rows != 30 {
		t.Fatalf("size %+v", out)
	}
	var s stream
	s.until(t, ui, out.Terminal, contains("30 100"))
	coretest.MustCall(t, core, ui, "core.terminal.resize", map[string]any{"terminal": out.Terminal, "cols": 1000, "rows": 41}, nil)
	send(t, core, ui, out.Terminal, "\n")
	s.until(t, ui, out.Terminal, contains("41 500"))
	if _, err := coretest.Call(t, core, ui, "core.terminal.resize", map[string]any{"terminal": out.Terminal, "cols": 0, "rows": 41}); rpc.Code(err) != "bad_request" {
		t.Fatalf("zero size: %v", err)
	}
	var list struct {
		Terminals []started `json:"terminals"`
	}
	coretest.MustCall(t, core, ui, "core.terminal.list", nil, &list)
	if len(list.Terminals) != 1 || list.Terminals[0].Cols != 500 || list.Terminals[0].Rows != 41 {
		t.Fatalf("list %+v", list.Terminals)
	}
}

func TestCloseKillsGroup(t *testing.T) {
	core, module, ui := setup(t)
	out := startShell(t, core, ui, `trap '' HUP; sleep 300 & echo bg=$!; wait`, nil)
	var s stream
	s.until(t, ui, out.Terminal, contains("\n"))
	line := s.text[strings.Index(s.text, "bg=")+3:]
	bg, err := strconv.Atoi(strings.TrimSpace(strings.SplitN(line, "\n", 2)[0]))
	if err != nil {
		t.Fatalf("no bg pid in %q", s.text)
	}
	coretest.MustCall(t, core, ui, "core.terminal.close", map[string]any{"terminal": out.Terminal}, nil)
	s.until(t, ui, out.Terminal, exited)
	if s.exit.Code != -1 {
		t.Fatalf("close exit code %d", s.exit.Code)
	}
	waitGone(t, out.Pid)
	waitGone(t, bg)
	waitIdle(t, module)
	if _, err := coretest.Call(t, core, ui, "core.terminal.input", map[string]any{"terminal": out.Terminal, "data": "YQ=="}); rpc.Code(err) != "unknown_terminal" {
		t.Fatalf("input after close: %v", err)
	}
}

func TestAttachAfterDetachAndReconnect(t *testing.T) {
	core, _, ui := setup(t)
	out := startShell(t, core, ui, `read a; echo first $a; read b; echo second $b; read c`, nil)
	coretest.MustCall(t, core, ui, "core.terminal.detach", map[string]any{"terminal": out.Terminal}, nil)
	send(t, core, ui, out.Terminal, "one\n")
	time.Sleep(300 * time.Millisecond)
	ui.Close()
	for len(ui.Events) > 0 {
		ev := <-ui.Events
		if ev.Name == "core.terminal.output" {
			t.Fatalf("detached viewer got output %s", ev.Data)
		}
	}
	next := coretest.NewPeer("ui2", rpc.UI)
	var attached started
	coretest.MustCall(t, core, next, "core.terminal.attach", map[string]any{"terminal": out.Terminal, "cols": 90, "rows": 20}, &attached)
	replay, _ := base64.StdEncoding.DecodeString(attached.Replay)
	if !strings.Contains(string(replay), "first one") || attached.Cols != 90 || attached.Rows != 20 || !attached.Running {
		t.Fatalf("attach %+v replay %q", attached, replay)
	}
	send(t, core, next, out.Terminal, "two\n")
	var s stream
	s.until(t, next, out.Terminal, contains("second two"))
	coretest.MustCall(t, core, next, "core.terminal.close", map[string]any{"terminal": out.Terminal}, nil)
	s.until(t, next, out.Terminal, exited)
}

func TestViewerDroppedWhenPeerDisconnects(t *testing.T) {
	core, module, ui := setup(t)
	out := startShell(t, core, ui, `while read line; do echo "x$line"; done`, nil)
	ui.Close()
	deadline := time.Now().Add(2 * time.Second)
	for {
		module.mu.Lock()
		n := len(module.sessions[out.Terminal].viewers)
		module.mu.Unlock()
		if n == 0 {
			break
		}
		if time.Now().After(deadline) {
			t.Fatal("viewer not dropped")
		}
		time.Sleep(10 * time.Millisecond)
	}
	if !module.Busy() {
		t.Fatal("session should outlive its viewer")
	}
}

func TestStopKillsSessions(t *testing.T) {
	core := coretest.New(t)
	module := New()
	coretest.Start(t, core, module)
	ui := coretest.NewPeer("ui", rpc.UI)
	out := startShell(t, core, ui, `trap '' HUP TERM; sleep 300`, nil)
	module.Stop()
	waitGone(t, out.Pid)
	if module.Busy() {
		t.Fatal("busy after stop")
	}
}

func TestRequestErrors(t *testing.T) {
	core, _, ui := setup(t)
	phone := coretest.NewPeer("phone", rpc.LAN)
	relay := coretest.NewPeer("relay", rpc.Relay)
	for _, method := range []string{"core.terminal.start", "core.terminal.list", "core.terminal.input", "core.terminal.attach"} {
		for _, peer := range []rpc.Peer{phone, relay} {
			if _, err := coretest.Call(t, core, peer, method, map[string]any{"program": "/bin/sh"}); rpc.Code(err) != "forbidden" {
				t.Fatalf("%s from %s: %v", method, peer.Transport(), err)
			}
		}
	}
	checks := []struct {
		method string
		params map[string]any
		code   string
	}{
		{"core.terminal.start", map[string]any{}, "bad_request"},
		{"core.terminal.start", map[string]any{"program": "/bin/sh", "cwd": "relative"}, "bad_request"},
		{"core.terminal.start", map[string]any{"program": "/bin/sh", "args": "x"}, "bad_request"},
		{"core.terminal.start", map[string]any{"program": "/bin/sh", "terminal": "XYZ"}, "bad_request"},
		{"core.terminal.start", map[string]any{"program": "forkmesh-no-such-program-xyz"}, "cli_missing"},
		{"core.terminal.start", map[string]any{"program": "/bin/sh", "cwd": filepath.Join(t.TempDir(), "missing")}, "terminal_failed"},
		{"core.terminal.input", map[string]any{"terminal": strings.Repeat("a", 32), "data": "YQ=="}, "unknown_terminal"},
		{"core.terminal.close", map[string]any{"terminal": 5}, "unknown_terminal"},
	}
	for _, c := range checks {
		if _, err := coretest.Call(t, core, ui, c.method, c.params); rpc.Code(err) != c.code {
			t.Fatalf("%s %v: got %v want %s", c.method, c.params, err, c.code)
		}
	}
	id := newTerminalID()
	out := startShell(t, core, ui, `read x`, map[string]any{"terminal": id})
	if out.Terminal != id {
		t.Fatalf("caller id ignored: %s", out.Terminal)
	}
	if _, err := coretest.Call(t, core, ui, "core.terminal.start", map[string]any{"program": "/bin/sh", "terminal": id}); rpc.Code(err) != "bad_request" {
		t.Fatalf("duplicate id: %v", err)
	}
	if _, err := coretest.Call(t, core, ui, "core.terminal.input", map[string]any{"terminal": id, "data": "!!"}); rpc.Code(err) != "bad_input" {
		t.Fatalf("bad input: %v", err)
	}
}
