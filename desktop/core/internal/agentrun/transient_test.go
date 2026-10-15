//go:build !windows

package agentrun

import (
	"encoding/base64"
	"encoding/json"
	"os"
	"path/filepath"
	"testing"
	"time"

	"forkmesh.com/desktop-core/internal/coretest"
	"forkmesh.com/desktop-core/internal/rpc"
)

func storeFiles(t *testing.T, root string) []string {
	t.Helper()
	files := []string{}
	_ = filepath.WalkDir(root, func(path string, entry os.DirEntry, err error) error {
		if err == nil && !entry.IsDir() {
			files = append(files, path)
		}
		return nil
	})
	return files
}

func TestTransientClaudeRunIsParsedWithoutTheStore(t *testing.T) {
	h := newRunHarness(t, nil, "")
	ui := coretest.NewPeer("ui:1", rpc.UI)
	root := h.store.Writer().Store().SessionsDir()
	before := storeFiles(t, root)
	h.call(ui, "core.agentrun.credentials", map[string]any{"values": []string{"dialogsecretvalue"}}, nil)
	key := "dialog-claude-1"
	var started map[string]any
	h.call(ui, "core.agentproc.start", map[string]any{"key": key, "program": os.Args[0], "args": []string{},
		"env": append(os.Environ(), "FAKE_CLAUDE=1"), "cwd": t.TempDir(), "label": "claude",
		"meta":       map[string]any{"parse": "claude", "transient": true, "runtime": "claude-stream-json"},
		"skipStdout": true, "skipStderr": true}, &started)
	if started["parsed"] != true {
		t.Fatalf("start %v", started)
	}
	h.call(ui, "core.agentrun.attach", map[string]any{"key": key, "after": 0, "stderr": true}, nil)
	h.call(ui, "core.agentproc.write", map[string]any{"key": key,
		"data": base64.StdEncoding.EncodeToString([]byte(userLine("fill dialogsecretvalue")))}, nil)
	lines := collect(t, ui, func(lines []string) bool { return countLines(lines, `"type":"result"`) == 1 })
	if countLines(lines, "tick 1 of fill ***") != 1 || countLines(lines, "dialogsecretvalue") != 0 {
		t.Fatalf("lines %v", lines)
	}
	h.call(ui, "core.agentrun.detach", map[string]any{"key": key}, nil)

	again := coretest.NewPeer("ui:2", rpc.UI)
	var state map[string]any
	h.call(again, "core.agentrun.attach", map[string]any{"key": key, "after": 0}, &state)
	if state["replayed"] != 3.0 {
		t.Fatalf("replayed %v", state)
	}
	var firstSo float64
	for i := 0; i < 3; i++ {
		raw := again.Next(t, "core.agentrun.event", 5*time.Second)
		var payload map[string]any
		_ = json.Unmarshal(raw, &payload)
		if payload["replay"] != true {
			t.Fatalf("replay %v", payload)
		}
		if i == 0 {
			firstSo = payload["so"].(float64)
		}
	}
	tail := coretest.NewPeer("ui:3", rpc.UI)
	h.call(tail, "core.agentrun.attach", map[string]any{"key": key, "after": firstSo}, &state)
	if state["replayed"] != 2.0 {
		t.Fatalf("tail replayed %v", state)
	}
	h.call(tail, "core.agentproc.closeStdin", map[string]any{"key": key}, nil)
	tail.Next(t, "core.agentrun.exit", 20*time.Second)
	h.store.Writer().Flush()
	if after := storeFiles(t, root); len(after) != len(before) {
		t.Fatalf("a transient run wrote to the agent store: %v", after)
	}
}

func TestTransientRunNeedsTheFlag(t *testing.T) {
	h := newRunHarness(t, nil, "")
	ui := coretest.NewPeer("ui:1", rpc.UI)
	h.call(ui, "core.agentrun.credentials", map[string]any{"values": []string{}}, nil)
	var started map[string]any
	h.call(ui, "core.agentproc.start", map[string]any{"key": "nosession-1", "program": os.Args[0], "args": []string{},
		"env": append(os.Environ(), "FAKE_CLAUDE=1"), "cwd": t.TempDir(), "label": "claude",
		"meta": map[string]any{"parse": "claude"}, "skipStdout": true}, &started)
	if started["parsed"] != false {
		t.Fatalf("a run without a session or the transient flag was parsed: %v", started)
	}
	h.call(ui, "core.agentproc.stop", map[string]any{"key": "nosession-1", "forget": true}, nil)
}

func TestTransientCodexRunAnswersADialog(t *testing.T) {
	h := newRunHarness(t, nil, "")
	ui := coretest.NewPeer("ui:1", rpc.UI)
	h.call(ui, "core.agentrun.credentials", map[string]any{"values": []string{}}, nil)
	key := "dialog-codex-1"
	var started map[string]any
	h.call(ui, "core.agentproc.start", map[string]any{"key": key, "program": os.Args[0], "args": []string{},
		"env": append(os.Environ(), "FAKE_CODEX=1"), "cwd": t.TempDir(), "label": "codex",
		"skipStdout": true, "meta": map[string]any{"parse": "codex", "transient": true, "initialPrompt": "fill",
			"mode": "Plan", "runtime": "codex-app-server"}}, &started)
	if started["parsed"] != true {
		t.Fatalf("start %v", started)
	}
	h.call(ui, "core.agentrun.attach", map[string]any{"key": key, "after": 0}, nil)
	lines := collect(t, ui, func(lines []string) bool { return countLines(lines, `"type":"result"`) == 1 })
	if countLines(lines, "tick 1 of fill") != 2 {
		t.Fatalf("lines %v", lines)
	}
	h.call(ui, "core.agentproc.closeStdin", map[string]any{"key": key}, nil)
	ui.Next(t, "core.agentrun.exit", 20*time.Second)
}
