package agentmodel

import (
	"encoding/json"
	"path/filepath"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	"forkmesh.com/desktop-core/internal/coretest"
	"forkmesh.com/desktop-core/internal/qtjson"
	"forkmesh.com/desktop-core/internal/rpc"
)

func nextOutput(t *testing.T, peer *coretest.Peer) qtjson.Value {
	t.Helper()
	return decode(t, peer.Next(t, "agents.output", 3*time.Second))
}

func TestFollowStreamsTranscriptGrowth(t *testing.T) {
	h := newHarness(t)
	h.credentials("tok-secret-1")
	session := h.create("owner", "alice", "name", "demo", "status", "running", "prompt", "hi")
	h.push(qtjson.ObjectOf("seq", 1))
	dir, _ := h.store.Writer().DirFor("alice-demo", session.ID)
	h.store.Writer().AppendLog(dir, session.ID, "first line\r")
	first := h.value(h.phone, "agents.follow", map[string]any{"id": session.ID})
	if first.Get("transcript").Str() != "first line\n" || first.Get("from").Int() != 0 || first.Get("next").Int() != 11 ||
		!first.Get("streaming").BoolValue() || !first.Get("active").BoolValue() || first.Get("status").Str() != "running" ||
		!first.Get("attachments").IsArray() {
		t.Fatalf("first %s", first.Compact())
	}
	h.store.Writer().AppendLog(dir, session.ID, "tok-secret-1 é")
	out := nextOutput(t, h.phone)
	if out.Get("transcript").Str() != "*** é\n" || out.Get("from").Int() != 11 || out.Get("next").Int() != 26 ||
		out.Get("reset").BoolValue() || out.Has("attachments") || out.Has("streaming") {
		t.Fatalf("growth %s", out.Compact())
	}
	h.save(session, func(record *qtjson.Value) { record.Set("status", qtjson.NewString("success")) })
	h.push(qtjson.ObjectOf("seq", 2))
	out = nextOutput(t, h.phone)
	if out.Get("status").Str() != "success" || out.Get("active").BoolValue() || out.Get("transcript").Str() != "" {
		t.Fatalf("status change %s", out.Compact())
	}
	h.call(h.phone, "agents.unfollow", map[string]any{"id": session.ID}, nil)
	if h.model.follows.count() != 0 {
		t.Fatalf("unfollow kept %d", h.model.follows.count())
	}
}

func TestFollowLimitAndClose(t *testing.T) {
	h := newHarness(t)
	h.credentials()
	h.push(qtjson.ObjectOf("seq", 1))
	ids := []int{}
	for i := 0; i < 9; i++ {
		ids = append(ids, h.create("owner", "alice", "name", "demo", "status", "success").ID)
	}
	for _, id := range ids {
		h.value(h.phone, "agents.follow", map[string]any{"id": id})
	}
	if got := h.model.follows.count(); got != maxFollowsPerConnection {
		t.Fatalf("follows %d", got)
	}
	h.model.follows.mu.Lock()
	first := h.model.follows.list[0].session
	h.model.follows.mu.Unlock()
	if first != ids[1] {
		t.Fatalf("the oldest idle follow should go, first is %d", first)
	}
	h.fails(h.phone, "agents.follow", map[string]any{"id": 0}, "unknown_session")
	h.fails(h.phone, "agents.follow", map[string]any{"id": 4242}, "unknown_session")
	h.phone.Close()
	waitFor(t, "follows dropped", func() bool { return h.model.follows.count() == 0 })
}

func TestFollowOutsideTranscript(t *testing.T) {
	h := newHarness(t)
	h.credentials()
	path := filepath.Join(t.TempDir(), "s.jsonl")
	line := func(value qtjson.Value) string { return string(value.Compact()) + "\n" }
	writeFile(t, path, line(qtjson.ObjectOf("type", "user", "message", qtjson.ObjectOf("content", "build it")))+
		line(qtjson.ObjectOf("type", "assistant", "message", qtjson.ObjectOf("content", qtjson.NewArray(
			qtjson.ObjectOf("type", "text", "text", "Sure"),
			qtjson.ObjectOf("type", "tool_use", "name", "Bash", "input", qtjson.ObjectOf("command", "make")))))))
	h.push(qtjson.ObjectOf("seq", 1, "outsideSources", qtjson.NewArray(
		qtjson.ObjectOf("id", -1005, "label", "Claude Code", "cwd", "/w", "transcript",
			qtjson.ObjectOf("path", path, "kind", "claude", "cwd", "/w")),
		qtjson.ObjectOf("id", -1006, "terminal", true))))
	first := h.value(h.phone, "agents.follow", map[string]any{"id": -1005})
	want := "==> Claude Code started outside ForkMesh, in /w\n\n> build it\n\nSure\n\n[Bash make]\n"
	if first.Get("transcript").Str() != want || !first.Get("outside").BoolValue() || first.Get("status").Str() != "running" {
		t.Fatalf("outside %s", first.Compact())
	}
	appendLine := line(qtjson.ObjectOf("type", "assistant", "message", qtjson.ObjectOf("content", qtjson.NewArray(
		qtjson.ObjectOf("type", "text", "text", "Done")))))
	writeFile(t, path, line(qtjson.ObjectOf("type", "user", "message", qtjson.ObjectOf("content", "build it")))+
		line(qtjson.ObjectOf("type", "assistant", "message", qtjson.ObjectOf("content", qtjson.NewArray(
			qtjson.ObjectOf("type", "text", "text", "Sure"),
			qtjson.ObjectOf("type", "tool_use", "name", "Bash", "input", qtjson.ObjectOf("command", "make"))))))+appendLine)
	out := nextOutput(t, h.phone)
	if out.Get("transcript").Str() != "\nDone\n" || out.Get("from").Int() != len16(want) {
		t.Fatalf("outside growth %s", out.Compact())
	}
	h.fails(h.phone, "agents.follow", map[string]any{"id": -1006}, "no_transcript")
	h.fails(h.phone, "agents.follow", map[string]any{"id": -1007}, "unknown_session")
}

func TestFollowDelegatesBeforeContext(t *testing.T) {
	h := newHarness(t)
	session := h.create("owner", "alice", "name", "demo", "status", "running")
	params := h.delegatedParams(h.phone, "agents.follow", map[string]any{"id": session.ID})
	if params.Get("id").Int() != session.ID {
		t.Fatalf("params %s", params.Compact())
	}
}

func TestFollowRelayPacingWaitsForBacklog(t *testing.T) {
	h := newHarness(t)
	h.credentials()
	var backlog atomic.Int32
	backlog.Store(1)
	h.model.backlog = func() int { return int(backlog.Load()) }
	relay := coretest.NewPeer("relay:1", rpc.Relay)
	session := h.create("owner", "alice", "name", "demo", "status", "running")
	h.push(qtjson.ObjectOf("seq", 1))
	h.value(relay, "agents.follow", map[string]any{"id": session.ID})
	dir, _ := h.store.Writer().DirFor("alice-demo", session.ID)
	h.store.Writer().AppendLog(dir, session.ID, "x")
	select {
	case event := <-relay.Events:
		t.Fatalf("sent with a relay backlog: %s %s", event.Name, event.Data)
	case <-time.After(250 * time.Millisecond):
	}
	backlog.Store(0)
	out := nextOutput(t, relay)
	if !strings.Contains(out.Get("transcript").Str(), "x") {
		t.Fatalf("relay output %s", out.Compact())
	}
	_ = json.RawMessage{}
}

func TestTranscriptCoversOutsideSessionsAndKnownImages(t *testing.T) {
	h := newHarness(t)
	h.credentials()
	path := filepath.Join(t.TempDir(), "c.jsonl")
	writeFile(t, path, string(qtjson.ObjectOf("type", "event_msg", "payload",
		qtjson.ObjectOf("type", "user_message", "message", "hello codex")).Compact())+"\n")
	image := filepath.Join(t.TempDir(), "pasted.png")
	writeFile(t, image, "\x89PNG\r\n\x1a\n")
	session := h.create("owner", "alice", "name", "demo", "status", "running")
	h.push(qtjson.ObjectOf("seq", 1,
		"sessions", qtjson.NewArray(qtjson.ObjectOf("id", session.ID, "images", []string{image})),
		"outsideSources", qtjson.NewArray(qtjson.ObjectOf("id", -1010, "label", "Codex", "cwd", "/c",
			"transcript", qtjson.ObjectOf("path", path, "kind", "codex", "cwd", "/c")))))
	outside := h.value(h.phone, "agents.transcript", map[string]any{"id": -1010, "since": 0})
	if outside.Get("transcript").Str() != "==> Codex started outside ForkMesh, in /c\n\n> hello codex\n" ||
		!outside.Get("outside").BoolValue() || outside.Get("reset").BoolValue() {
		t.Fatalf("outside %s", outside.Compact())
	}
	own := h.value(h.phone, "agents.transcript", map[string]any{"id": session.ID})
	if own.Get("attachments").Len() != 1 || own.Get("attachments").At(0).Get("name").Str() != "pasted.png" {
		t.Fatalf("own %s", own.Compact())
	}
	h.fails(h.phone, "agents.transcript", map[string]any{"id": -1011}, "unknown_session")
}

func TestOutsidePhoneSteerHidesLead(t *testing.T) {
	entry := qtjson.ObjectOf("type", "user", "isMeta", true, "message", qtjson.ObjectOf("content", "x"),
		"origin", qtjson.ObjectOf("kind", "peer", "name", "ForkMesh (phone)", "body", peerSteerLead+"\n\nYes"))
	if got := claudeEntry(entry); got != "\n> ⟵ ForkMesh (phone): Yes\n" {
		t.Fatalf("steer %q", got)
	}
	other := qtjson.ObjectOf("type", "user", "isMeta", true, "message", qtjson.ObjectOf("content", "x"),
		"origin", qtjson.ObjectOf("kind", "peer", "name", "a-13", "body", "ping from a"))
	if got := claudeEntry(other); got != "\n> ⟵ a-13: ping from a\n" {
		t.Fatalf("peer %q", got)
	}
}
