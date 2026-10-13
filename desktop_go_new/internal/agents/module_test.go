package agents

import (
	"os"
	"path/filepath"
	"strings"
	"testing"

	"forkmesh.com/desktop-core/internal/coretest"
	"forkmesh.com/desktop-core/internal/rpc"
)

func startModule(t *testing.T) (*Module, string, func(string, any, any), func(string, any) error) {
	t.Helper()
	core := coretest.New(t)
	if !strings.HasPrefix(core.Layout.Data, os.TempDir()) {
		t.Fatalf("data dir %s is outside the temp dir", core.Layout.Data)
	}
	module := New()
	coretest.Start(t, core, module)
	ui := coretest.NewPeer("ui", rpc.UI)
	call := func(method string, params any, into any) {
		t.Helper()
		coretest.MustCall(t, core, ui, method, params, into)
	}
	phone := coretest.NewPeer("phone", rpc.LAN)
	for _, method := range []string{"core.agents.sessions", "core.agents.transcript", "core.agents.events", "core.agents.search"} {
		if _, err := coretest.Call(t, core, phone, method, map[string]any{"id": 1, "needle": "x"}); rpc.Code(err) != "forbidden" {
			t.Fatalf("%s from a phone: %v", method, err)
		}
	}
	fail := func(method string, params any) error {
		t.Helper()
		_, err := coretest.Call(t, core, ui, method, params)
		return err
	}
	return module, filepath.Join(core.Layout.Data, "agents"), call, fail
}

func TestModuleSessions(t *testing.T) {
	module, root, call, _ := startModule(t)
	if module.Store().Root != root {
		t.Fatalf("root %s", module.Store().Root)
	}
	writeMeta(t, filepath.Join(root, "sessions"), cppMeta(map[string]any{"id": 1, "status": "queued", "extraKey": "kept"}))
	writeMeta(t, filepath.Join(root, "sessions"), cppMeta(map[string]any{"id": 2, "createdAtMs": 5000}))
	bin := writeMeta(t, filepath.Join(root, "recycle-bin"), cppMeta(map[string]any{"id": 3}))
	writeFile(t, filepath.Join(bin, "deleted-at.json"), []byte(`{"deletedAtMs":777}`))
	writeFile(t, filepath.Join(root, "model-outcomes.json"), []byte(`{"version":1,"models":[{"provider":"codex","model":"m","runs":2,"merged":1}]}`))

	var plain struct {
		Sessions []map[string]any `json:"sessions"`
		Queued   []int            `json:"queued"`
		Recycled []map[string]any `json:"recycled"`
	}
	call("core.agents.sessions", map[string]any{}, &plain)
	if len(plain.Sessions) != 2 || plain.Sessions[0]["id"] != 2.0 || plain.Sessions[1]["extraKey"] != "kept" {
		t.Fatalf("sessions %v", plain.Sessions)
	}
	if len(plain.Queued) != 1 || plain.Queued[0] != 1 || plain.Recycled != nil {
		t.Fatalf("queued %v recycled %v", plain.Queued, plain.Recycled)
	}
	var full struct {
		Recycled []map[string]any `json:"recycled"`
		Outcomes []ModelOutcome   `json:"outcomes"`
	}
	call("core.agents.sessions", map[string]any{"recycled": true, "outcomes": true}, &full)
	if len(full.Recycled) != 1 || full.Recycled[0]["deletedAtMs"] != 777.0 {
		t.Fatalf("recycled %v", full.Recycled)
	}
	if len(full.Outcomes) != 1 || full.Outcomes[0].Runs != 2 {
		t.Fatalf("outcomes %v", full.Outcomes)
	}
}

type transcriptReply struct {
	ID    int    `json:"id"`
	Text  string `json:"text"`
	From  int64  `json:"from"`
	Next  int64  `json:"next"`
	Size  int64  `json:"size"`
	More  bool   `json:"more"`
	Reset bool   `json:"reset"`
	Stamp string `json:"stamp"`
}

func TestModuleTranscript(t *testing.T) {
	_, root, call, fail := startModule(t)
	dir := writeMeta(t, filepath.Join(root, "sessions"), cppMeta(map[string]any{"id": 4}))
	content := "héllo\r\nwörld\n"
	writeFile(t, filepath.Join(dir, "transcript.txt"), []byte(content))

	var reply transcriptReply
	call("core.agents.transcript", map[string]any{"id": 4}, &reply)
	if reply.Text != "héllo\nwörld\n" || reply.From != 0 || reply.Next != int64(len(content)) || reply.More {
		t.Fatalf("full %+v", reply)
	}
	call("core.agents.transcript", map[string]any{"id": 4, "offset": 0, "limit": 2}, &reply)
	if reply.Text != "h" || reply.Next != 1 || !reply.More {
		t.Fatalf("split rune %+v", reply)
	}
	call("core.agents.transcript", map[string]any{"id": 4, "offset": 2, "limit": 100}, &reply)
	if reply.From != 3 || reply.Text != "llo\nwörld\n" {
		t.Fatalf("mid rune offset %+v", reply)
	}
	call("core.agents.transcript", map[string]any{"id": 4, "offset": -1, "limit": 6}, &reply)
	if reply.Text != "rld\n" && reply.Text != "örld\n" {
		t.Fatalf("tail %+v", reply)
	}
	call("core.agents.transcript", map[string]any{"id": 4, "offset": 9999}, &reply)
	if !reply.Reset || reply.From != 0 || reply.Next != int64(len(content)) {
		t.Fatalf("reset %+v", reply)
	}
	if !strings.HasPrefix(reply.Stamp, "15:") {
		t.Fatalf("stamp %q", reply.Stamp)
	}
	writeMeta(t, filepath.Join(root, "sessions"), cppMeta(map[string]any{"id": 5}))
	call("core.agents.transcript", map[string]any{"id": 5}, &reply)
	if reply.Text != "" || reply.Next != 0 || reply.Stamp != "-|-" {
		t.Fatalf("empty %+v", reply)
	}
	for _, method := range []string{"core.agents.transcript", "core.agents.events"} {
		if err := fail(method, map[string]any{"id": 99}); rpc.Code(err) != "unknown_session" {
			t.Fatalf("%s unknown session: %v", method, err)
		}
	}
	if err := fail("core.agents.transcript", map[string]any{"id": "x"}); rpc.Code(err) != "bad_params" {
		t.Fatalf("bad params: %v", err)
	}
}

func TestModuleEvents(t *testing.T) {
	_, root, call, _ := startModule(t)
	dir := writeMeta(t, filepath.Join(root, "sessions"), cppMeta(map[string]any{"id": 6}))
	writeFile(t, filepath.Join(dir, "events.jsonl"), []byte("{\"type\":\"a\"}\n{\"type\":\"b\"}\n"))
	var reply struct {
		Events []map[string]any `json:"events"`
		Next   int64            `json:"next"`
		More   bool             `json:"more"`
		Reset  bool             `json:"reset"`
	}
	call("core.agents.events", map[string]any{"id": 6}, &reply)
	if len(reply.Events) != 2 || reply.Events[1]["type"] != "b" || reply.Next != 26 || reply.More {
		t.Fatalf("events %+v", reply)
	}
	call("core.agents.events", map[string]any{"id": 6, "offset": 13}, &reply)
	if len(reply.Events) != 1 || reply.Events[0]["type"] != "b" {
		t.Fatalf("offset %+v", reply)
	}
	call("core.agents.events", map[string]any{"id": 6, "offset": 500}, &reply)
	if !reply.Reset || len(reply.Events) != 2 {
		t.Fatalf("reset %+v", reply)
	}
}

func TestModuleSearch(t *testing.T) {
	_, root, call, _ := startModule(t)
	for id := 1; id <= 30; id++ {
		dir := writeMeta(t, filepath.Join(root, "sessions"), cppMeta(map[string]any{"id": id, "createdAtMs": id}))
		text := "nothing"
		if id%3 == 0 {
			text = strings.Repeat("Deploy failed. ", id/3)
		}
		writeFile(t, filepath.Join(dir, "transcript.txt"), []byte(text))
	}
	var reply struct {
		Hits     []SearchHit `json:"hits"`
		Searched int         `json:"searched"`
	}
	call("core.agents.search", map[string]any{"needle": "deploy FAILED"}, &reply)
	if len(reply.Hits) != 10 || reply.Searched != 30 {
		t.Fatalf("hits %+v", reply)
	}
	if reply.Hits[0].ID != 30 || reply.Hits[0].Count != 10 || !strings.Contains(reply.Hits[0].Snippet, "Deploy failed.") {
		t.Fatalf("first hit %+v", reply.Hits[0])
	}
	call("core.agents.search", map[string]any{"needle": "deploy", "ids": []int{3, 4, 6}}, &reply)
	if len(reply.Hits) != 2 || reply.Hits[0].ID != 6 || reply.Hits[1].ID != 3 || reply.Searched != 3 {
		t.Fatalf("filtered %+v", reply)
	}
	call("core.agents.search", map[string]any{"needle": "  "}, &reply)
	if len(reply.Hits) != 0 {
		t.Fatalf("blank %+v", reply)
	}
}
