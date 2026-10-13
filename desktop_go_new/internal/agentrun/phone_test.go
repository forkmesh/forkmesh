//go:build !windows

package agentrun

import (
	"encoding/json"
	"os"
	"path/filepath"
	"testing"
	"time"

	"forkmesh.com/desktop-core/internal/coretest"
	"forkmesh.com/desktop-core/internal/qtjson"
	"forkmesh.com/desktop-core/internal/rpc"
)

func TestPhoneTranscriptMatchesDesktopRules(t *testing.T) {
	h := newRunHarness(t, nil, "")
	ui := coretest.NewPeer("ui:1", rpc.UI)
	phone := coretest.NewPeer("lan:1", rpc.LAN)
	phone.Dev = rpc.Device{Key: "k1", Name: "Phone", User: "alice"}
	created, _ := h.store.Writer().CreateRecord(qtjson.ObjectOf("schemaVersion", 2, "owner", "alice", "name", "demo",
		"provider", "claude-code", "status", "running", "prompt", "Look\nAttached image: /nowhere/x.png"))
	sid := created.Get("id").Int()
	dir, _ := h.store.Writer().DirFor("alice-demo", sid)
	image := filepath.Join(h.core.Layout.Data, "agent-images", "x.png")
	if err := os.MkdirAll(filepath.Dir(image), 0o700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(image, []byte("\x89PNG\r\n\x1a\nxx"), 0o600); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "transcript.txt"), []byte("a\r\nkey supersecret99 \U0001F600\xff\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	if _, err := coretest.Call(t, h.core, phone, "agents.transcript", map[string]any{"id": sid}); rpc.Code(err) != "unsupported" {
		t.Fatalf("without credentials the desktop answers: %v", err)
	}
	h.call(ui, "core.agentrun.credentials", map[string]any{"values": []string{"supersecret99"}}, nil)
	var reply struct {
		ID          int              `json:"id"`
		Transcript  string           `json:"transcript"`
		From        int64            `json:"from"`
		Next        int64            `json:"next"`
		Reset       bool             `json:"reset"`
		Active      bool             `json:"active"`
		Status      string           `json:"status"`
		Attachments []map[string]any `json:"attachments"`
	}
	h.call(phone, "agents.transcript", map[string]any{"id": sid}, &reply)
	if reply.Transcript != "a\nkey *** \U0001F600�\n" || reply.Next != 24 || reply.From != 0 || !reply.Active || reply.Status != "running" {
		t.Fatalf("reply %+v", reply)
	}
	if len(reply.Attachments) != 1 || reply.Attachments[0]["name"] != "x.png" || reply.Attachments[0]["mediaType"] != "image/png" {
		t.Fatalf("attachments %v", reply.Attachments)
	}
	canonical, _ := filepath.EvalSymlinks(image)
	if reply.Attachments[0]["id"] != AttachmentID(h.run.salt, sid, canonical) {
		t.Fatalf("attachment id %v", reply.Attachments[0])
	}
	h.call(phone, "agents.transcript", map[string]any{"id": sid, "since": 20}, &reply)
	if reply.From != 20 || reply.Transcript != "\U0001F600�\n" || reply.Reset {
		t.Fatalf("since %+v", reply)
	}
	h.call(phone, "agents.transcript", map[string]any{"id": sid, "since": 99}, &reply)
	if !reply.Reset || reply.From != 0 {
		t.Fatalf("reset %+v", reply)
	}
	if _, err := coretest.Call(t, h.core, phone, "agents.transcript", map[string]any{"id": 0}); rpc.Code(err) != "unknown_session" {
		t.Fatalf("id 0: %v", err)
	}
	for _, id := range []int{-3, sid + 50} {
		if _, err := coretest.Call(t, h.core, phone, "agents.transcript", map[string]any{"id": id}); rpc.Code(err) != "unsupported" {
			t.Fatalf("id %d: %v", id, err)
		}
	}
	var salt struct{ Salt string }
	h.call(ui, "core.agentrun.salt", nil, &salt)
	if len(salt.Salt) != 64 {
		t.Fatalf("salt %q", salt.Salt)
	}
	if _, err := coretest.Call(t, h.core, phone, "core.agentrun.salt", nil); rpc.Code(err) != "forbidden" {
		t.Fatalf("salt for a phone: %v", err)
	}
}

func TestPhoneBinIsOwnerGated(t *testing.T) {
	h := newRunHarness(t, nil, "")
	h.run.now = func() time.Time { return time.UnixMilli(1_000_000_000) }
	w := h.store.Writer()
	for i, title := range []string{"first line\nsecond", "x"} {
		record, _ := w.CreateRecord(qtjson.ObjectOf("schemaVersion", 2, "owner", "o", "name", "n", "issueNumber", i,
			"issueTitle", title, "prompt", "  spaced   prompt  \nmore", "branchName", "b"))
		w.SetClock(func() time.Time { return time.UnixMilli(999_000_000) })
		if ok, err := w.RecycleRecord(record); !ok {
			t.Fatal(err)
		}
	}
	stranger := coretest.NewPeer("lan:2", rpc.LAN)
	stranger.Dev = rpc.Device{Key: "k2", User: "mallory"}
	if _, err := coretest.Call(t, h.core, stranger, "agentops.bin", nil); rpc.Code(err) != "not_owner" {
		t.Fatalf("stranger: %v", err)
	}
	h.core.Set("control.owners", []string{"alice"})
	owner := coretest.NewPeer("lan:3", rpc.LAN)
	owner.Dev = rpc.Device{Key: "k3", User: "Alice"}
	raw, err := coretest.Call(t, h.core, owner, "agentops.bin", nil)
	if err != nil {
		t.Fatal(err)
	}
	var bin struct {
		Agents         []map[string]any `json:"agents"`
		Count          int              `json:"count"`
		RetentionHours int              `json:"retentionHours"`
		MinHours       int              `json:"minHours"`
		MaxHours       int              `json:"maxHours"`
	}
	_ = json.Unmarshal(raw, &bin)
	if bin.Count != 2 || bin.RetentionHours != 24 || bin.MinHours != 1 || bin.MaxHours != 720 {
		t.Fatalf("bin %+v", bin)
	}
	titles := map[string]bool{}
	for _, row := range bin.Agents {
		titles[row["title"].(string)] = true
		if row["removeInMs"].(float64) != float64(24*3600*1000-1_000_000) {
			t.Fatalf("row %v", row)
		}
	}
	if !titles["x"] || !titles["spaced prompt"] {
		t.Fatalf("titles %v", titles)
	}
}
