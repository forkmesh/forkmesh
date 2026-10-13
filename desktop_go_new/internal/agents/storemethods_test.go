package agents

import (
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"forkmesh.com/desktop-core/internal/coretest"
	"forkmesh.com/desktop-core/internal/rpc"
)

func TestAgentStoreMethods(t *testing.T) {
	module, root, call, fail := startModule(t)
	core := module.core
	phone := coretest.NewPeer("phone", rpc.LAN)
	for _, method := range []string{"core.agentstore.create", "core.agentstore.appendLog", "core.agentstore.sync"} {
		if _, err := coretest.Call(t, core, phone, method, map[string]any{}); rpc.Code(err) != "forbidden" {
			t.Fatalf("%s from a phone: %v", method, err)
		}
	}
	input := `{"schemaVersion":2,"id":0,"owner":"alice","name":"demo","provider":"codex","model":"GPT-5 ","merged":true,"costUsd":1e+16,"createdAtMs":0,"status":"running"}`
	var created struct {
		Session     map[string]any `json:"session"`
		ID          int            `json:"id"`
		CreatedAtMs int64          `json:"createdAtMs"`
	}
	call("core.agentstore.create", map[string]any{"session": json.RawMessage(input)}, &created)
	if created.ID != 1 || created.CreatedAtMs <= 0 || created.Session["id"] != 1.0 {
		t.Fatalf("created %+v", created)
	}
	dir := filepath.Join(root, "sessions", "alice-demo", "1")
	meta, err := os.ReadFile(filepath.Join(dir, "meta.json"))
	if err != nil || !strings.Contains(string(meta), "\"costUsd\": 1e+16,\n") || !strings.HasSuffix(string(meta), "}\n") {
		t.Fatalf("meta %s %v", meta, err)
	}
	ui := coretest.NewPeer("ui2", rpc.UI)
	notify := func(method string, params any) {
		raw, _ := json.Marshal(params)
		if _, err := core.Registry.Dispatch(t.Context(), &rpc.Request{Peer: ui, Method: method, Params: raw}); err != nil {
			t.Fatalf("%s: %v", method, err)
		}
	}
	notify("core.agentstore.appendLog", map[string]any{"path": "sessions/alice-demo/1/transcript.txt", "text": "hello"})
	notify("core.agentstore.appendEvent", map[string]any{"key": "alice-demo", "id": 1, "event": json.RawMessage(`{"type":"x","_so":1e+05}`)})
	call("core.agentstore.sync", map[string]any{}, nil)
	if data, _ := os.ReadFile(filepath.Join(dir, "transcript.txt")); string(data) != "hello\n" {
		t.Fatalf("transcript %q", data)
	}
	if data, _ := os.ReadFile(filepath.Join(dir, "events.jsonl")); string(data) != "{\"type\":\"x\",\"_so\":1e+05}\n" {
		t.Fatalf("events %q", data)
	}
	if err := fail("core.agentstore.appendLog", map[string]any{"path": "sessions/../x/1/transcript.txt", "text": "no"}); rpc.Code(err) != "bad_session" {
		t.Fatalf("escape: %v", err)
	}
	if err := fail("core.agentstore.appendLog", map[string]any{"key": "..", "id": 1, "text": "no"}); rpc.Code(err) != "bad_session" {
		t.Fatalf("escape: %v", err)
	}
	var patched struct{ OK bool }
	call("core.agentstore.writePatch", map[string]any{"key": "alice-demo", "id": 1, "patch": "diff\n"}, &patched)
	if !patched.OK {
		t.Fatal("patch")
	}
	var transcript transcriptReply
	call("core.agents.transcript", map[string]any{"id": 1}, &transcript)
	if transcript.Text != "hello\n" {
		t.Fatalf("read back %q", transcript.Text)
	}
	session := created.Session
	raw, _ := json.Marshal(session)
	var ok struct{ OK bool }
	call("core.agentstore.recycle", map[string]any{"session": json.RawMessage(raw)}, &ok)
	if !ok.OK {
		t.Fatal("recycle")
	}
	notify("core.agentstore.appendLog", map[string]any{"key": "alice-demo", "id": 1, "text": "late"})
	var saved struct{ Saved bool }
	call("core.agentstore.save", map[string]any{"session": json.RawMessage(raw)}, &saved)
	call("core.agentstore.sync", map[string]any{}, nil)
	if saved.Saved {
		t.Fatal("a recycled session was saved")
	}
	if _, err := os.Stat(dir); !os.IsNotExist(err) {
		t.Fatalf("recycled dir came back: %v", err)
	}
	call("core.agentstore.restore", map[string]any{"id": 1}, &ok)
	if !ok.OK {
		t.Fatal("restore")
	}
	restored, _ := os.ReadFile(filepath.Join(dir, "meta.json"))
	if !strings.Contains(string(restored), `"status": "stopped"`) {
		t.Fatalf("restored %s", restored)
	}
	var outcomes struct{ Outcomes []ModelOutcome }
	call("core.agentstore.outcomes", map[string]any{}, &outcomes)
	if len(outcomes.Outcomes) != 1 || outcomes.Outcomes[0].Model != "gpt-5" || outcomes.Outcomes[0].Merged != 1 {
		t.Fatalf("outcomes %+v", outcomes.Outcomes)
	}
	call("core.agentstore.delete", map[string]any{"session": json.RawMessage(raw)}, &ok)
	if _, err := os.Stat(dir); !ok.OK || !os.IsNotExist(err) {
		t.Fatalf("delete %v %v", ok.OK, err)
	}
	call("core.agentstore.create", map[string]any{"session": json.RawMessage(input)}, &created)
	if created.ID != 1 {
		t.Fatalf("reused id %d", created.ID)
	}
	if _, err := os.Stat(filepath.Join(dir, "meta.json")); err != nil {
		t.Fatalf("a re-created id must be writable again: %v", err)
	}
	var unsupported struct{ Saved bool }
	call("core.agentstore.save", map[string]any{"session": json.RawMessage(`{"schemaVersion":3,"id":9,"owner":"a","name":"b"}`)}, &unsupported)
	if unsupported.Saved {
		t.Fatal("a newer schema was rewritten")
	}
}
