//go:build !windows

package agentproc

import (
	"bufio"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"os"
	"os/signal"
	"path/filepath"
	"reflect"
	"strconv"
	"strings"
	"syscall"
	"testing"
	"time"

	"forkmesh.com/desktop-core/internal/coretest"
	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/rpc"
)

const shimMarker = "FORKMESH_AGENTPROC_TEST_SHIM"

func TestMain(m *testing.M) {
	if os.Getenv(shimMarker) == "1" && len(os.Args) > 2 && os.Args[1] == "agentproc-shim" {
		os.Exit(ShimMain(os.Args[2:], os.Stdin, os.Stdout))
	}
	if mode := os.Getenv("FAKE_AGENT"); mode != "" {
		os.Exit(fakeAgent(mode))
	}
	os.Exit(m.Run())
}

func fakeAgent(mode string) int {
	if os.Getenv("FAKE_TRAP") == "1" {
		terms := make(chan os.Signal, 1)
		signal.Notify(terms, syscall.SIGTERM)
		go func() {
			<-terms
			fmt.Println("term")
			os.Exit(0)
		}()
	}
	if mode == "hang" {
		signal.Ignore(syscall.SIGTERM)
	}
	scanner := bufio.NewScanner(os.Stdin)
	scanner.Buffer(make([]byte, 1<<20), 1<<20)
	for scanner.Scan() {
		line := scanner.Text()
		switch {
		case strings.HasPrefix(line, "exit "):
			code, _ := strconv.Atoi(strings.TrimPrefix(line, "exit "))
			return code
		case strings.HasPrefix(line, "err "):
			fmt.Fprintln(os.Stderr, strings.TrimPrefix(line, "err "))
		case strings.HasPrefix(line, "env "):
			if value, ok := os.LookupEnv(strings.TrimPrefix(line, "env ")); ok {
				fmt.Println(value)
			} else {
				fmt.Println("<unset>")
			}
		case strings.HasPrefix(line, "big "):
			count, _ := strconv.Atoi(strings.TrimPrefix(line, "big "))
			for i := 0; i < count; i++ {
				fmt.Printf("line-%05d\n", i)
			}
		case line == "pwd":
			dir, _ := os.Getwd()
			fmt.Println(dir)
		default:
			fmt.Println(line)
		}
	}
	if mode == "hang" {
		select {}
	}
	return 0
}

type harness struct {
	t    *testing.T
	core *daemon.Core
	mod  *Module
	ui   *coretest.Peer
}

func newHarness(t *testing.T, root string, tune func(*Module)) *harness {
	t.Helper()
	core := coretest.New(t)
	mod := New()
	if root != "" {
		mod.root = root
	}
	mod.shimCommand = func() (string, []string) { return os.Args[0], []string{"agentproc-shim"} }
	mod.environ = func() []string { return append(os.Environ(), shimMarker+"=1") }
	if tune != nil {
		tune(mod)
	}
	coretest.Start(t, core, mod)
	t.Cleanup(func() { core.Set(shutdownHardKey, true) })
	return &harness{t: t, core: core, mod: mod, ui: coretest.NewPeer("ui:1", rpc.UI)}
}

func agentEnv(mode string, extra ...string) []string {
	env := []string{"FAKE_AGENT=" + mode, "PATH=" + os.Getenv("PATH"), "HOME=" + os.TempDir(),
		"FORKMESH_DESKTOP_INSTANCE=deadbeef"}
	return append(env, extra...)
}

func (h *harness) call(peer rpc.Peer, method string, params any, into any) {
	h.t.Helper()
	coretest.MustCall(h.t, h.core, peer, method, params, into)
}

func (h *harness) start(key, mode string, extra map[string]any, env ...string) map[string]any {
	h.t.Helper()
	params := map[string]any{"key": key, "program": os.Args[0], "args": []string{}, "env": agentEnv(mode, env...),
		"cwd": h.t.TempDir(), "label": "claude 7", "detachable": true}
	for k, v := range extra {
		params[k] = v
	}
	var reply map[string]any
	h.call(h.ui, "core.agentproc.start", params, &reply)
	if reply["pid"].(float64) <= 0 {
		h.t.Fatalf("start reply %v", reply)
	}
	return reply
}

func (h *harness) write(key, text string) {
	h.t.Helper()
	h.call(h.ui, "core.agentproc.write", map[string]any{"key": key, "data": base64.StdEncoding.EncodeToString([]byte(text))}, nil)
}

type stream struct {
	out, err   strings.Builder
	outEnd     int64
	errEnd     int64
	exit       map[string]any
	gaps       int64
	outOffsets []int64
}

func (s *stream) take(t *testing.T, event coretest.Event) {
	t.Helper()
	var data struct {
		Key    string `json:"key"`
		Data   string `json:"data"`
		Offset int64  `json:"offset"`
		End    int64  `json:"end"`
		Gap    int64  `json:"gap"`
	}
	switch event.Name {
	case "core.agentproc.stdout", "core.agentproc.stderr":
		if err := json.Unmarshal(event.Data, &data); err != nil {
			t.Fatalf("event %s", event.Data)
		}
		raw, _ := base64.StdEncoding.DecodeString(data.Data)
		if data.End != data.Offset+int64(len(raw)) {
			t.Fatalf("bad end %+v", data)
		}
		s.gaps += data.Gap
		if event.Name == "core.agentproc.stdout" {
			if s.outEnd != 0 && data.Offset != s.outEnd && data.Gap == 0 {
				t.Fatalf("stdout offset %d, expected %d", data.Offset, s.outEnd)
			}
			s.outOffsets = append(s.outOffsets, data.Offset)
			s.out.Write(raw)
			s.outEnd = data.End
		} else {
			s.err.Write(raw)
			s.errEnd = data.End
		}
	case "core.agentproc.exit":
		_ = json.Unmarshal(event.Data, &s.exit)
	}
}

func (h *harness) until(peer *coretest.Peer, s *stream, done func(*stream) bool) {
	h.t.Helper()
	deadline := time.After(15 * time.Second)
	for !done(s) {
		select {
		case event := <-peer.Events:
			s.take(h.t, event)
		case <-deadline:
			h.t.Fatalf("timed out; stdout %q stderr %q exit %v", s.out.String(), s.err.String(), s.exit)
		}
	}
}

func outHas(text string) func(*stream) bool {
	return func(s *stream) bool { return strings.Contains(s.out.String(), text) }
}

func exited(s *stream) bool { return s.exit != nil }

func (h *harness) list() map[string]map[string]any {
	h.t.Helper()
	var reply struct {
		Processes []map[string]any `json:"processes"`
	}
	h.call(h.ui, "core.agentproc.list", nil, &reply)
	out := map[string]map[string]any{}
	for _, p := range reply.Processes {
		out[p["key"].(string)] = p
	}
	return out
}

func waitFor(t *testing.T, what string, check func() bool) {
	t.Helper()
	deadline := time.Now().Add(15 * time.Second)
	for !check() {
		if time.Now().After(deadline) {
			t.Fatalf("timed out waiting for %s", what)
		}
		time.Sleep(20 * time.Millisecond)
	}
}

func TestStartWriteStdoutStderrExit(t *testing.T) {
	h := newHarness(t, "", nil)
	h.start("agent-1", "echo", nil)
	if !h.mod.Busy() {
		t.Fatal("not busy while an agent runs")
	}
	s := &stream{}
	h.write("agent-1", "hello\n")
	h.until(h.ui, s, outHas("hello\n"))
	h.write("agent-1", "err oops\n")
	h.until(h.ui, s, func(s *stream) bool { return s.err.String() == "oops\n" })
	h.write("agent-1", "exit 3\n")
	h.until(h.ui, s, exited)
	if s.exit["code"].(float64) != 3 || s.exit["crashed"].(bool) {
		t.Fatalf("exit %v", s.exit)
	}
	waitFor(t, "not busy", func() bool { return !h.mod.Busy() })
	entry := h.list()["agent-1"]
	if entry["running"].(bool) || entry["exit"] == nil || entry["stdout"].(float64) != 6 {
		t.Fatalf("list %v", entry)
	}
	var exit exitInfo
	if !readJSONFile(filepath.Join(h.mod.root, "agent-1", "exit.json"), &exit) || exit.Code != 3 {
		t.Fatalf("exit.json %+v", exit)
	}
}

func TestChildEnvironmentAndDirectory(t *testing.T) {
	h := newHarness(t, "", nil)
	cwd := t.TempDir()
	h.start("agent-env", "echo", map[string]any{"cwd": cwd}, "FAKE_MARK=present")
	s := &stream{}
	h.write("agent-env", "env FORKMESH_DESKTOP_INSTANCE\nenv FAKE_MARK\npwd\n")
	resolved, _ := filepath.EvalSymlinks(cwd)
	h.until(h.ui, s, outHas(resolved))
	if !strings.HasPrefix(s.out.String(), "<unset>\npresent\n") {
		t.Fatalf("env output %q", s.out.String())
	}
}

func TestMergedChannels(t *testing.T) {
	h := newHarness(t, "", nil)
	h.start("agent-merge", "echo", map[string]any{"merge": true})
	s := &stream{}
	h.write("agent-merge", "err to-stdout\n")
	h.until(h.ui, s, outHas("to-stdout\n"))
	if s.err.Len() != 0 {
		t.Fatalf("stderr %q", s.err.String())
	}
}

func TestReplayFromOffsetsAcrossDetachAttach(t *testing.T) {
	h := newHarness(t, "", nil)
	h.start("agent-replay", "echo", nil)
	s := &stream{}
	h.write("agent-replay", "a\nb\n")
	h.until(h.ui, s, outHas("a\nb\n"))
	h.call(h.ui, "core.agentproc.ack", map[string]any{"key": "agent-replay", "stdoutOffset": 2}, nil)
	var ack ackState
	if !readJSONFile(filepath.Join(h.mod.root, "agent-replay", "ack.json"), &ack) || ack.Stdout != 2 {
		t.Fatalf("ack.json %+v", ack)
	}
	h.call(h.ui, "core.agentproc.detach", map[string]any{"key": "agent-replay"}, nil)
	h.write("agent-replay", "c\n")
	waitFor(t, "c spooled", func() bool { return spoolLength(filepath.Join(h.mod.root, "agent-replay"), streamOut) == 6 })
	select {
	case event := <-h.ui.Events:
		t.Fatalf("event while detached: %s", event.Name)
	case <-time.After(100 * time.Millisecond):
	}
	second := coretest.NewPeer("ui:2", rpc.UI)
	var info map[string]any
	h.call(second, "core.agentproc.attach", map[string]any{"key": "agent-replay", "stdoutOffset": -1}, &info)
	replay := &stream{}
	h.until(second, replay, outHas("b\nc\n"))
	if replay.out.String() != "b\nc\n" || replay.outOffsets[0] != 2 {
		t.Fatalf("replay %q offsets %v", replay.out.String(), replay.outOffsets)
	}
	third := coretest.NewPeer("ui:3", rpc.UI)
	h.call(third, "core.agentproc.attach", map[string]any{"key": "agent-replay", "stdoutOffset": 0}, nil)
	full := &stream{}
	h.until(third, full, outHas("a\nb\nc\n"))
	h.write("agent-replay", "d\n")
	h.until(second, replay, outHas("d\n"))
	h.until(third, full, outHas("d\n"))
}

func TestCoreRestartAdoptsRunningAgent(t *testing.T) {
	first := newHarness(t, "", nil)
	first.start("agent-live", "echo", map[string]any{"meta": map[string]any{"sessionId": 42.0}})
	s := &stream{}
	first.write("agent-live", "one\n")
	first.until(first.ui, s, outHas("one\n"))
	first.call(first.ui, "core.agentproc.ack", map[string]any{"key": "agent-live", "stdoutOffset": 4}, nil)
	first.call(first.ui, "core.agentproc.setMeta", map[string]any{"key": "agent-live", "meta": map[string]any{"opened": true}}, nil)
	pid := int(first.list()["agent-live"]["pid"].(float64))
	first.mod.Stop()
	if !pidAlive(pid) {
		t.Fatal("a soft core stop killed the agent")
	}
	second := newHarness(t, first.mod.root, nil)
	entry := second.list()["agent-live"]
	if entry == nil || !entry["running"].(bool) || int(entry["pid"].(float64)) != pid {
		t.Fatalf("adopted %v", entry)
	}
	meta := entry["meta"].(map[string]any)
	if meta["sessionId"] != 42.0 || meta["opened"] != true {
		t.Fatalf("meta %v", meta)
	}
	if !second.mod.Busy() {
		t.Fatal("adopted agent does not keep the core busy")
	}
	second.call(second.ui, "core.agentproc.attach", map[string]any{"key": "agent-live", "stdoutOffset": -1}, nil)
	second.write("agent-live", "two\n")
	resumed := &stream{}
	second.until(second.ui, resumed, outHas("two\n"))
	if resumed.out.String() != "two\n" || resumed.outOffsets[0] != 4 {
		t.Fatalf("resumed %q %v", resumed.out.String(), resumed.outOffsets)
	}
	second.write("agent-live", "exit 0\n")
	second.until(second.ui, resumed, exited)
}

func TestExitWhileNoCoreIsReplayedByTheNextCore(t *testing.T) {
	first := newHarness(t, "", nil)
	first.start("agent-gone", "echo", nil)
	s := &stream{}
	first.write("agent-gone", "before\n")
	first.until(first.ui, s, outHas("before\n"))
	first.write("agent-gone", "after\nexit 5\n")
	first.mod.Stop()
	dir := filepath.Join(first.mod.root, "agent-gone")
	waitFor(t, "exit.json", func() bool { return readJSONFile(filepath.Join(dir, "exit.json"), &exitInfo{}) })
	second := newHarness(t, first.mod.root, nil)
	second.call(second.ui, "core.agentproc.attach", map[string]any{"key": "agent-gone", "stdoutOffset": 7}, nil)
	replay := &stream{}
	second.until(second.ui, replay, exited)
	if replay.out.String() != "after\n" || replay.exit["code"].(float64) != 5 {
		t.Fatalf("replay %q exit %v", replay.out.String(), replay.exit)
	}
	second.call(second.ui, "core.agentproc.forget", map[string]any{"key": "agent-gone"}, nil)
	if _, err := os.Stat(dir); !os.IsNotExist(err) {
		t.Fatalf("forget left %s: %v", dir, err)
	}
}

func TestSpoolSegmentsTrimOnAck(t *testing.T) {
	h := newHarness(t, "", func(m *Module) { m.segmentBytes = 100 })
	h.start("agent-big", "echo", nil)
	s := &stream{}
	h.write("agent-big", "big 200\n")
	h.until(h.ui, s, outHas("line-00199\n"))
	dir := filepath.Join(h.mod.root, "agent-big")
	total := int64(200 * 11)
	if got := spoolLength(dir, streamOut); got != total {
		t.Fatalf("spool length %d", got)
	}
	before := len(listSegments(dir, streamOut))
	h.call(h.ui, "core.agentproc.ack", map[string]any{"key": "agent-big", "stdoutOffset": 1100}, nil)
	after := listSegments(dir, streamOut)
	if len(after) >= before || after[0] > 1100 {
		t.Fatalf("segments %d -> %v", before, after)
	}
	late := coretest.NewPeer("ui:late", rpc.UI)
	h.call(late, "core.agentproc.attach", map[string]any{"key": "agent-big", "stdoutOffset": -1}, nil)
	replay := &stream{}
	h.until(late, replay, outHas("line-00199\n"))
	if !strings.HasPrefix(replay.out.String(), "line-00100\n") {
		t.Fatalf("replay starts %q", replay.out.String()[:22])
	}
	early := coretest.NewPeer("ui:early", rpc.UI)
	h.call(early, "core.agentproc.attach", map[string]any{"key": "agent-big", "stdoutOffset": 0}, nil)
	gapped := &stream{}
	h.until(early, gapped, outHas("line-00199\n"))
	if gapped.gaps != after[0] {
		t.Fatalf("gap %d, first segment %d", gapped.gaps, after[0])
	}
}

func TestSpoolCapDropsOldestSegments(t *testing.T) {
	dir := t.TempDir()
	writer := newSpoolWriter(dir, streamOut, 10, 20)
	for i := 0; i < 10; i++ {
		if err := writer.Write([]byte("0123456789")); err != nil {
			t.Fatal(err)
		}
	}
	writer.Close()
	segments := listSegments(dir, streamOut)
	if len(segments) > 3 || segments[len(segments)-1] != 90 {
		t.Fatalf("segments %v", segments)
	}
	data, from, err := readSpool(dir, streamOut, 0, 100)
	if err != nil || from != segments[0] || string(data) != "0123456789" {
		t.Fatalf("read %q from %d: %v", data, from, err)
	}
}

func TestFlowWindowPacesReplay(t *testing.T) {
	h := newHarness(t, "", nil)
	h.start("agent-flow", "echo", map[string]any{"attach": false})
	h.write("agent-flow", "big 100\n")
	dir := filepath.Join(h.mod.root, "agent-flow")
	waitFor(t, "spooled", func() bool { return spoolLength(dir, streamOut) == 1100 })
	viewer := coretest.NewPeer("ui:flow", rpc.UI)
	h.call(viewer, "core.agentproc.attach", map[string]any{"key": "agent-flow", "stdoutOffset": 0, "window": 300}, nil)
	s := &stream{}
	h.until(viewer, s, func(s *stream) bool { return s.outEnd >= 300 })
	select {
	case event := <-viewer.Events:
		t.Fatalf("sent past the window: %s", event.Data)
	case <-time.After(150 * time.Millisecond):
	}
	if s.outEnd != 300 {
		t.Fatalf("delivered %d", s.outEnd)
	}
	h.call(viewer, "core.agentproc.flow", map[string]any{"key": "agent-flow", "stdoutOffset": 300}, nil)
	h.until(viewer, s, func(s *stream) bool { return s.outEnd >= 600 })
	h.call(viewer, "core.agentproc.ack", map[string]any{"key": "agent-flow", "stdoutOffset": 1100}, nil)
	h.until(viewer, s, func(s *stream) bool { return s.outEnd == 1100 })
}

func TestSignalKillAndStop(t *testing.T) {
	h := newHarness(t, "", nil)
	h.start("agent-trap", "echo", nil, "FAKE_TRAP=1")
	s := &stream{}
	h.write("agent-trap", "ready\n")
	h.until(h.ui, s, outHas("ready\n"))
	h.call(h.ui, "core.agentproc.signal", map[string]any{"key": "agent-trap", "signal": "TERM"}, nil)
	h.until(h.ui, s, exited)
	if !strings.Contains(s.out.String(), "term\n") || s.exit["code"].(float64) != 0 {
		t.Fatalf("trap %q %v", s.out.String(), s.exit)
	}

	h.start("agent-hang", "hang", nil)
	hung := &stream{}
	h.write("agent-hang", "ready\n")
	h.until(h.ui, hung, outHas("ready\n"))
	h.call(h.ui, "core.agentproc.signal", map[string]any{"key": "agent-hang", "signal": 15}, nil)
	h.call(h.ui, "core.agentproc.kill", map[string]any{"key": "agent-hang"}, nil)
	h.until(h.ui, hung, exited)
	if !hung.exit["crashed"].(bool) || hung.exit["signal"].(float64) != float64(syscall.SIGKILL) {
		t.Fatalf("kill exit %v", hung.exit)
	}

	h.start("agent-stop", "hang", nil)
	stopped := &stream{}
	h.write("agent-stop", "ready\n")
	h.until(h.ui, stopped, outHas("ready\n"))
	h.call(h.ui, "core.agentproc.stop", map[string]any{"key": "agent-stop", "graceMs": 100, "forget": true}, nil)
	h.until(h.ui, stopped, exited)
	waitFor(t, "forgotten", func() bool { return h.list()["agent-stop"] == nil })
	waitFor(t, "directory removed", func() bool {
		_, err := os.Stat(filepath.Join(h.mod.root, "agent-stop"))
		return os.IsNotExist(err)
	})
	if _, err := coretest.Call(t, h.core, h.ui, "core.agentproc.signal", map[string]any{"key": "agent-trap", "signal": "BOGUS"}); rpc.Code(err) != "bad_signal" {
		t.Fatalf("bad signal: %v", err)
	}
}

func TestNonDetachableStopsWhenItsDesktopLeaves(t *testing.T) {
	h := newHarness(t, "", nil)
	owner := coretest.NewPeer("ui:owner", rpc.UI)
	var reply map[string]any
	h.call(owner, "core.agentproc.start", map[string]any{"key": "dialog-1", "program": os.Args[0],
		"env": agentEnv("hang"), "label": "codex", "detachable": false}, &reply)
	pid := int(reply["pid"].(float64))
	owner.Close()
	waitFor(t, "owner-bound process stopped", func() bool { return !pidAlive(pid) })
	waitFor(t, "forgotten", func() bool { return h.list()["dialog-1"] == nil })
}

func TestOrphanedNonDetachableIsClaimedOrStopped(t *testing.T) {
	first := newHarness(t, "", nil)
	var reply map[string]any
	first.call(first.ui, "core.agentproc.start", map[string]any{"key": "codex-1", "program": os.Args[0],
		"env": agentEnv("echo"), "detachable": false}, &reply)
	first.call(first.ui, "core.agentproc.start", map[string]any{"key": "codex-2", "program": os.Args[0],
		"env": agentEnv("echo"), "detachable": false}, nil)
	first.mod.Stop()
	second := newHarness(t, first.mod.root, func(m *Module) { m.orphanGrace = 300 * time.Millisecond })
	second.call(second.ui, "core.agentproc.attach", map[string]any{"key": "codex-1", "stdoutOffset": 0}, nil)
	waitFor(t, "unclaimed orphan stopped", func() bool { return second.list()["codex-2"] == nil })
	time.Sleep(400 * time.Millisecond)
	if entry := second.list()["codex-1"]; entry == nil || !entry["running"].(bool) {
		t.Fatalf("claimed orphan %v", entry)
	}
}

func TestLostShimReportsLostExit(t *testing.T) {
	h := newHarness(t, "", nil)
	reply := h.start("agent-lost", "echo", nil)
	s := &stream{}
	h.write("agent-lost", "x\n")
	h.until(h.ui, s, outHas("x\n"))
	_ = syscall.Kill(int(reply["shimPid"].(float64)), syscall.SIGKILL)
	h.until(h.ui, s, exited)
	if !s.exit["lost"].(bool) || !s.exit["crashed"].(bool) {
		t.Fatalf("lost exit %v", s.exit)
	}
}

func TestStartFailuresAndScopeFallback(t *testing.T) {
	h := newHarness(t, "", nil)
	_, err := coretest.Call(t, h.core, h.ui, "core.agentproc.start", map[string]any{"key": "nope",
		"program": "/nonexistent/agent", "env": agentEnv("echo")})
	if !strings.HasPrefix(rpc.Code(err), "failed_to_start") {
		t.Fatalf("missing program: %v", err)
	}
	if _, err := os.Stat(filepath.Join(h.mod.root, "nope")); !os.IsNotExist(err) {
		t.Fatalf("failed start left a directory: %v", err)
	}
	if _, err := coretest.Call(t, h.core, h.ui, "core.agentproc.start", map[string]any{"key": "../x", "program": "sh"}); rpc.Code(err) != "bad_key" {
		t.Fatalf("bad key: %v", err)
	}
	reply := h.start("agent-scope", "echo", map[string]any{"scope": map[string]any{
		"systemdRun": "/nonexistent/systemd-run", "contain": true, "memoryHigh": 1 << 30, "memoryMax": 2 << 30}})
	if reply["contained"].(bool) || reply["unit"].(string) != "" {
		t.Fatalf("fallback reply %v", reply)
	}
	if _, err := coretest.Call(t, h.core, h.ui, "core.agentproc.start", map[string]any{"key": "agent-scope",
		"program": os.Args[0], "env": agentEnv("echo")}); rpc.Code(err) != "key_busy" {
		t.Fatalf("second start: %v", err)
	}
	if _, err := coretest.Call(t, h.core, h.ui, "core.agentproc.forget", map[string]any{"key": "agent-scope"}); rpc.Code(err) != "running" {
		t.Fatalf("forget running: %v", err)
	}
}

func TestRemotePeersAreRefused(t *testing.T) {
	h := newHarness(t, "", nil)
	phone := coretest.NewPeer("lan:1", rpc.LAN)
	for _, method := range []string{"core.agentproc.start", "core.agentproc.list", "core.agentproc.attach",
		"core.agentproc.write", "core.agentproc.kill", "core.agentproc.ack", "core.agentproc.forget"} {
		if _, err := coretest.Call(t, h.core, phone, method, map[string]any{"key": "x"}); rpc.Code(err) != errForbidden {
			t.Fatalf("%s from a phone: %v", method, err)
		}
	}
}

func TestHardStopKillsAgents(t *testing.T) {
	h := newHarness(t, "", nil)
	reply := h.start("agent-hard", "hang", nil)
	pid := int(reply["pid"].(float64))
	h.core.Set(shutdownHardKey, true)
	h.mod.Stop()
	waitFor(t, "hard stop", func() bool { return !pidAlive(pid) })
}

func TestScopeArguments(t *testing.T) {
	scope := Scope{SystemdRun: "/usr/bin/systemd-run", Contain: true, MemoryHigh: 100, MemoryMax: 200, OOMPolicy: true}
	unit := unitName("claude 12", 4242, 7)
	if unit != "forkmesh-agent-core-claude-12-4242-7.scope" {
		t.Fatalf("unit %s", unit)
	}
	program, args := scopeCommand(scope, unit, "claude 12", "/bin/shim", []string{"agentproc-shim", "/d"})
	want := []string{"--user", "--scope", "--quiet", "--collect", "--slice=forkmesh.slice",
		"--unit=" + unit, "--description=ForkMesh agent claude 12",
		"--property=MemoryHigh=100", "--property=MemoryMax=200", "--property=OOMPolicy=continue",
		"--", "/bin/shim", "agentproc-shim", "/d"}
	if program != "/usr/bin/systemd-run" || !reflect.DeepEqual(args, want) {
		t.Fatalf("contained %s %v", program, args)
	}
	_, plain := scopeCommand(Scope{SystemdRun: "systemd-run"}, unit, "codex", "/bin/shim", nil)
	wantPlain := []string{"--user", "--scope", "--quiet", "--collect", "--unit=" + unit,
		"--description=ForkMesh agent codex", "--", "/bin/shim"}
	if !reflect.DeepEqual(plain, wantPlain) {
		t.Fatalf("escape scope %v", plain)
	}
	if got := unitName("!!!", 1, 2); got != "forkmesh-agent-core-agent-1-2.scope" {
		t.Fatalf("empty label unit %s", got)
	}
	if (Scope{}).usable() {
		t.Fatal("an empty scope is usable")
	}
}

func TestReleasedEnvironment(t *testing.T) {
	got := releasedEnv([]string{"A=1", "FORKMESH_DESKTOP_INSTANCE=abc", "FORKMESH_DESKTOP_INSTANCE", "B=FORKMESH_DESKTOP_INSTANCE=x"})
	if !reflect.DeepEqual(got, []string{"A=1", "B=FORKMESH_DESKTOP_INSTANCE=x"}) {
		t.Fatalf("released %v", got)
	}
}

func TestFrameRoundTrip(t *testing.T) {
	var buffer strings.Builder
	if err := writeFrame(&buffer, frameData, lengthsPayload(5, 9)); err != nil {
		t.Fatal(err)
	}
	kind, payload, err := readFrame(bufio.NewReader(strings.NewReader(buffer.String())))
	out, errLen := payloadLengths(payload)
	if err != nil || kind != frameData || out != 5 || errLen != 9 {
		t.Fatalf("frame %c %d %d %v", kind, out, errLen, err)
	}
	if payloadInt32(int32Payload(-3)) != -3 {
		t.Fatal("int32 payload")
	}
	if sig, ok := parseSignal(json.RawMessage(`"SIGKILL"`)); !ok || sig != int(syscall.SIGKILL) {
		t.Fatalf("parse %d", sig)
	}
}

func TestSetDetachableBindsAndReleasesTheDesktop(t *testing.T) {
	h := newHarness(t, "", nil)
	first := coretest.NewPeer("ui:first", rpc.UI)
	var reply map[string]any
	h.call(first, "core.agentproc.start", map[string]any{"key": "claude-idle", "program": os.Args[0],
		"env": agentEnv("hang"), "label": "claude 1", "detachable": true}, &reply)
	pid := int(reply["pid"].(float64))
	h.call(first, "core.agentproc.setDetachable", map[string]any{"key": "claude-idle", "detachable": false}, nil)
	h.call(first, "core.agentproc.setDetachable", map[string]any{"key": "claude-idle", "detachable": true}, nil)
	first.Close()
	time.Sleep(300 * time.Millisecond)
	if !pidAlive(pid) {
		t.Fatal("a detachable agent died with its desktop")
	}
	second := coretest.NewPeer("ui:second", rpc.UI)
	h.call(second, "core.agentproc.setDetachable", map[string]any{"key": "claude-idle", "detachable": false}, nil)
	second.Close()
	waitFor(t, "idle agent stopped with its desktop", func() bool { return !pidAlive(pid) })
}
