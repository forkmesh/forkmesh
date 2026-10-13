//go:build !windows

package agentrun

import (
	"bufio"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
	"time"

	"forkmesh.com/desktop-core/internal/agentproc"
	"forkmesh.com/desktop-core/internal/agents"
	"forkmesh.com/desktop-core/internal/coretest"
	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/qtjson"
	"forkmesh.com/desktop-core/internal/rpc"
)

const shimMarker = "FORKMESH_AGENTRUN_TEST_SHIM"

func TestMain(m *testing.M) {
	if os.Getenv(shimMarker) == "1" && len(os.Args) > 2 && os.Args[1] == "agentproc-shim" {
		os.Exit(agentproc.ShimMain(os.Args[2:], os.Stdin, os.Stdout))
	}
	if os.Getenv("FAKE_CLAUDE") == "1" {
		os.Exit(fakeClaude())
	}
	if os.Getenv("FAKE_CODEX") == "1" {
		os.Exit(fakeCodex())
	}
	os.Exit(m.Run())
}

func emit(value any) {
	raw, _ := json.Marshal(value)
	fmt.Println(string(raw))
}

func fakeClaude() int {
	emit(map[string]any{"type": "system", "subtype": "init", "session_id": "fake-session", "tools": []any{}})
	scanner := bufio.NewScanner(os.Stdin)
	scanner.Buffer(make([]byte, 1<<20), 1<<20)
	for scanner.Scan() {
		var message map[string]any
		if json.Unmarshal(scanner.Bytes(), &message) != nil || message["type"] != "user" {
			continue
		}
		body, _ := message["message"].(map[string]any)
		text, _ := body["content"].(string)
		count := 1
		if strings.HasPrefix(text, "slow ") {
			count, _ = strconv.Atoi(strings.TrimPrefix(text, "slow "))
		}
		if strings.HasPrefix(text, "err ") {
			os.Stderr.WriteString(strings.TrimPrefix(text, "err "))
		}
		for i := 1; i <= count; i++ {
			if count > 1 {
				time.Sleep(150 * time.Millisecond)
			}
			emit(map[string]any{"type": "assistant", "session_id": "fake-session", "message": map[string]any{
				"id": "msg-" + strconv.Itoa(i), "content": []any{map[string]any{"type": "text", "text": fmt.Sprintf("tick %d of %s", i, text)}},
				"usage": map[string]any{"output_tokens": 1}}})
		}
		emit(map[string]any{"type": "result", "subtype": "success", "is_error": false, "num_turns": 1, "duration_ms": 5, "session_id": "fake-session"})
	}
	return 0
}

type runHarness struct {
	t     *testing.T
	core  *daemon.Core
	procs *agentproc.Module
	run   *Module
	store *agents.Module
}

func newRunHarness(t *testing.T, layoutFrom *daemon.Core, procRoot string) *runHarness {
	t.Helper()
	core := coretest.New(t)
	if layoutFrom != nil {
		core.Layout = layoutFrom.Layout
	}
	store := agents.New()
	procs := agentproc.New()
	if procRoot != "" {
		procs.SetRoot(procRoot)
	}
	procs.SetLauncher(func() (string, []string) { return os.Args[0], []string{"agentproc-shim"} },
		func() []string { return append(os.Environ(), shimMarker+"=1") })
	run := New()
	coretest.Start(t, core, store, procs, run)
	t.Cleanup(func() { core.Set("core.shutdown.hard", true) })
	return &runHarness{t: t, core: core, procs: procs, run: run, store: store}
}

func (h *runHarness) call(peer rpc.Peer, method string, params any, into any) {
	h.t.Helper()
	coretest.MustCall(h.t, h.core, peer, method, params, into)
}

func userLine(text string) string {
	return string(qtjson.ObjectOf("type", "user", "message", qtjson.ObjectOf("role", "user", "content", text)).Compact()) + "\n"
}

func collect(t *testing.T, peer *coretest.Peer, until func(lines []string) bool) []string {
	t.Helper()
	lines := []string{}
	deadline := time.After(20 * time.Second)
	for !until(lines) {
		select {
		case event := <-peer.Events:
			if event.Name != "core.agentrun.event" {
				continue
			}
			var payload struct {
				Line string `json:"line"`
			}
			_ = json.Unmarshal(event.Data, &payload)
			if payload.Line != "" {
				lines = append(lines, payload.Line)
			}
		case <-deadline:
			t.Fatalf("timed out with %d lines: %v", len(lines), lines)
		}
	}
	return lines
}

func countLines(lines []string, needle string) int {
	n := 0
	for _, line := range lines {
		if strings.Contains(line, needle) {
			n++
		}
	}
	return n
}

func TestClaudeRunSurvivesCoreRestartExactlyOnce(t *testing.T) {
	first := newRunHarness(t, nil, "")
	ui := coretest.NewPeer("ui:1", rpc.UI)
	created, err := first.store.Writer().CreateRecord(qtjson.ObjectOf("schemaVersion", 2, "owner", "alice", "name", "demo",
		"provider", "claude-code", "status", "running"))
	if err != nil {
		t.Fatal(err)
	}
	sid := created.Get("id").Int()
	dir, _ := first.store.Writer().DirFor("alice-demo", sid)
	first.call(ui, "core.agentrun.credentials", map[string]any{"values": []string{"topsecretvalue"}}, nil)
	key := "agent-" + strconv.Itoa(sid) + "-1"
	meta := map[string]any{"parse": "claude", "sessionId": strconv.Itoa(sid), "storeKey": "alice-demo", "accountId": "acct"}
	first.call(ui, "core.agentproc.start", map[string]any{"key": key, "program": os.Args[0], "args": []string{},
		"env": append(os.Environ(), "FAKE_CLAUDE=1"), "cwd": t.TempDir(), "label": "claude", "detachable": true,
		"meta": meta, "skipStdout": true}, nil)
	var state map[string]any
	first.call(ui, "core.agentrun.attach", map[string]any{"key": key, "after": 0}, &state)
	write := func(h *runHarness, text string) {
		h.call(ui, "core.agentproc.write", map[string]any{"key": key, "data": base64.StdEncoding.EncodeToString([]byte(userLine(text)))}, nil)
	}
	write(first, "hello topsecretvalue")
	lines := collect(t, ui, func(lines []string) bool { return countLines(lines, `"type":"result"`) == 1 })
	if countLines(lines, "topsecretvalue") != 0 || countLines(lines, `"_forkmesh_account":"acct"`) < 2 {
		t.Fatalf("lines not normalised: %v", lines)
	}
	write(first, "slow 6")
	collect(t, ui, func(lines []string) bool { return countLines(lines, "tick 1 of slow") == 1 })
	first.procs.Stop()
	first.run.Stop()
	time.Sleep(500 * time.Millisecond)

	second := newRunHarness(t, first.core, filepath.Join(first.core.Layout.State, "core", "agentproc"))
	ui2 := coretest.NewPeer("ui:2", rpc.UI)
	second.call(ui2, "core.agentrun.credentials", map[string]any{"values": []string{"topsecretvalue"}}, nil)
	waitUntil(t, "the second core parses the run", func() bool { return second.run.lookup(key) != nil })
	loaded := lastStoredOffset(filepath.Join(dir, "events.jsonl"), key)
	second.call(ui2, "core.agentrun.attach", map[string]any{"key": key, "after": loaded}, nil)
	rest := collect(t, ui2, func(lines []string) bool { return countLines(lines, `"type":"result"`) == 1 })
	if countLines(rest, "tick 6 of slow") != 1 {
		t.Fatalf("rest %v", rest)
	}
	second.call(ui2, "core.agentproc.closeStdin", map[string]any{"key": key}, nil)
	ui2.Next(t, "core.agentrun.exit", 20*time.Second)
	second.store.Writer().Flush()
	events, _ := os.ReadFile(filepath.Join(dir, "events.jsonl"))
	transcript, _ := os.ReadFile(filepath.Join(dir, "transcript.txt"))
	for i := 1; i <= 6; i++ {
		needle := fmt.Sprintf("tick %d of slow 6", i)
		if strings.Count(string(events), needle) != 1 || strings.Count(string(transcript), needle) != 1 {
			t.Fatalf("%q not exactly once:\n%s\n%s", needle, events, transcript)
		}
	}
	if strings.Count(string(events), `"subtype":"init"`) != 1 || strings.Contains(string(events), "topsecretvalue") {
		t.Fatalf("events %s", events)
	}
}

func waitUntil(t *testing.T, what string, check func() bool) {
	t.Helper()
	deadline := time.Now().Add(20 * time.Second)
	for !check() {
		if time.Now().After(deadline) {
			t.Fatalf("timed out waiting for %s", what)
		}
		time.Sleep(20 * time.Millisecond)
	}
}

func fakeCodex() int {
	scanner := bufio.NewScanner(os.Stdin)
	scanner.Buffer(make([]byte, 1<<20), 1<<20)
	turns := 0
	for scanner.Scan() {
		var message map[string]any
		if json.Unmarshal(scanner.Bytes(), &message) != nil {
			continue
		}
		id := message["id"]
		switch message["method"] {
		case "initialize":
			emit(map[string]any{"id": id, "result": map[string]any{}})
		case "account/rateLimits/read":
			emit(map[string]any{"id": id, "result": map[string]any{"rateLimits": map[string]any{"limitId": "codex"}}})
		case "model/list":
			emit(map[string]any{"id": id, "result": map[string]any{"data": []any{map[string]any{"id": "m"}}}})
		case "thread/start":
			emit(map[string]any{"id": id, "result": map[string]any{"thread": map[string]any{"id": "th-9"}, "model": "m"}})
		case "turn/start":
			turns++
			turn := "turn-" + strconv.Itoa(turns)
			emit(map[string]any{"id": id, "result": map[string]any{"turn": map[string]any{"id": turn}}})
			params, _ := message["params"].(map[string]any)
			inputs, _ := params["input"].([]any)
			text := ""
			if len(inputs) > 0 {
				text, _ = inputs[0].(map[string]any)["text"].(string)
			}
			count := 1
			if strings.HasPrefix(text, "slow ") {
				count, _ = strconv.Atoi(strings.TrimPrefix(text, "slow "))
			}
			for i := 1; i <= count; i++ {
				if count > 1 {
					time.Sleep(150 * time.Millisecond)
				}
				emit(map[string]any{"method": "item/completed", "params": map[string]any{"item": map[string]any{
					"type": "agentMessage", "id": fmt.Sprintf("%s-%d", turn, i), "text": fmt.Sprintf("tick %d of %s", i, text)}}})
			}
			emit(map[string]any{"method": "turn/completed", "params": map[string]any{"turn": map[string]any{"id": turn, "status": "completed"}}})
		}
	}
	return 0
}

func TestCodexRunReattachesWithItsProtocolState(t *testing.T) {
	h := newRunHarness(t, nil, "")
	ui := coretest.NewPeer("ui:1", rpc.UI)
	created, _ := h.store.Writer().CreateRecord(qtjson.ObjectOf("schemaVersion", 2, "owner", "bob", "name", "x",
		"provider", "codex", "status", "running"))
	sid := created.Get("id").Int()
	dir, _ := h.store.Writer().DirFor("bob-x", sid)
	h.call(ui, "core.agentrun.credentials", map[string]any{"values": []string{}}, nil)
	key := "agent-" + strconv.Itoa(sid) + "-2"
	var started map[string]any
	h.call(ui, "core.agentproc.start", map[string]any{"key": key, "program": os.Args[0], "args": []string{},
		"env": append(os.Environ(), "FAKE_CODEX=1"), "cwd": t.TempDir(), "label": "codex", "detachable": true,
		"skipStdout": true, "meta": map[string]any{"parse": "codex", "sessionId": strconv.Itoa(sid), "storeKey": "bob-x",
			"initialPrompt": "first", "mode": "Auto", "runtime": "codex-app-server"}}, &started)
	if started["parsed"] != true {
		t.Fatalf("start %v", started)
	}
	h.call(ui, "core.agentrun.attach", map[string]any{"key": key, "after": 0}, nil)
	lines := collect(t, ui, func(lines []string) bool { return countLines(lines, `"type":"result"`) == 1 })
	if countLines(lines, "tick 1 of first") != 2 || countLines(lines, `"subtype":"init"`) != 1 {
		t.Fatalf("first turn %v", lines)
	}
	var state map[string]any
	h.call(ui, "core.agentrun.codex", map[string]any{"key": key, "op": "send", "text": "slow 5"}, &state)
	collect(t, ui, func(lines []string) bool { return countLines(lines, "tick 1 of slow") == 1 })
	h.call(ui, "core.agentrun.detach", map[string]any{"key": key}, nil)
	time.Sleep(300 * time.Millisecond)
	h.store.Writer().Flush()
	after := lastStoredOffset(filepath.Join(dir, "events.jsonl"), key)
	ui2 := coretest.NewPeer("ui:2", rpc.UI)
	h.call(ui2, "core.agentrun.attach", map[string]any{"key": key, "after": after}, &state)
	if state["threadId"] != "th-9" {
		t.Fatalf("state %v", state)
	}
	rest := collect(t, ui2, func(lines []string) bool { return countLines(lines, `"type":"result"`) == 1 })
	if countLines(rest, "tick 5 of slow 5") != 2 {
		t.Fatalf("rest %v", rest)
	}
	h.call(ui2, "core.agentproc.closeStdin", map[string]any{"key": key}, nil)
	ui2.Next(t, "core.agentrun.exit", 20*time.Second)
	h.store.Writer().Flush()
	events, _ := os.ReadFile(filepath.Join(dir, "events.jsonl"))
	transcript, _ := os.ReadFile(filepath.Join(dir, "transcript.txt"))
	for i := 1; i <= 5; i++ {
		needle := fmt.Sprintf("tick %d of slow 5", i)
		want := 1
		if i == 5 {
			want = 2
		}
		if strings.Count(string(events), needle) != want || strings.Count(string(transcript), needle) != 1 {
			t.Fatalf("%q:\n%s\n%s", needle, events, transcript)
		}
	}
}
