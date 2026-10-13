package agentmodel

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"forkmesh.com/desktop-core/internal/agentrun"
	"forkmesh.com/desktop-core/internal/qtjson"
)

func TestAgentsListNeedsContextAndCredentials(t *testing.T) {
	h := newHarness(t)
	h.create("owner", "alice", "name", "demo", "status", "queued")
	h.delegatedParams(h.phone, "agents.list", map[string]any{})
	h.push(qtjson.ObjectOf("seq", 1))
	h.delegatedParams(h.phone, "agents.list", map[string]any{})
	h.credentials()
	list := h.value(h.phone, "agents.list", map[string]any{})
	if list.Get("sessions").Len() != 1 {
		t.Fatalf("list %s", list.Compact())
	}
}

func TestAgentsListRowsMatchDesktopPayload(t *testing.T) {
	h := newHarness(t)
	h.attach(true)
	h.credentials("supersecret-value")
	old := h.create("owner", "alice", "name", "demo", "status", "success", "prompt", "old run supersecret-value",
		"finishedAtMs", 1700000000500, "costUsd", 0.125, "numTurns", 3, "durationMs", 4200, "totalTokens", 900,
		"branchName", "agent/old", "baseBranch", "main", "prNumber", 7)
	time.Sleep(2 * time.Millisecond)
	queued := h.create("owner", "alice", "name", "demo", "status", "queued", "prompt", strings.Repeat("é", 700),
		"priority", 10, "held", true)
	time.Sleep(2 * time.Millisecond)
	h.create("owner", "alice", "name", "demo", "status", "running", "associationOnly", true)
	time.Sleep(2 * time.Millisecond)
	running := h.create("owner", "alice", "name", "demo", "status", "running", "prompt", "go", "mode", "Auto",
		"model", "opus", "lastError", "", "issueNumber", 4, "issueTitle", "Fix it")
	image := filepath.Join(t.TempDir(), "shot.png")
	writeFile(t, image, "\x89PNG\r\n\x1a\nxx")
	h.push(qtjson.ObjectOf(
		"seq", 3,
		"sessions", qtjson.NewArray(
			qtjson.ObjectOf("id", old.ID, "unread", true, "diffStats", qtjson.ObjectOf("files", 2, "ahead", 1,
				"behind", 0, "added", 5, "removed", 1, "dirty", 0, "conflicted", false), "worktree", true),
			qtjson.ObjectOf("id", running.ID, "terminal", "t-1", "images", []string{image})),
		"outside", qtjson.NewArray(qtjson.ObjectOf("id", -1001, "kind", "claude", "title", "x supersecret-value")),
		"repositories", qtjson.NewArray(
			qtjson.ObjectOf("owner", "alice", "name", "demo", "localPath", "/tmp/demo", "active", true),
			qtjson.ObjectOf("owner", "Alice", "name", "Demo", "localPath", "/tmp/demo2"),
			qtjson.ObjectOf("owner", "bob", "name", "x", "localPath", "")),
		"folders", qtjson.NewArray(qtjson.ObjectOf("repositoryOwner", "alice", "repositoryName", "demo",
			"id", "f1", "name", "Release", "collapsed", false, "members", []string{"alice/demo#7"})),
		"providers", qtjson.NewArray(qtjson.ObjectOf("id", "claude-code", "label", "Claude Code")),
		"defaultProvider", "claude-code",
		"defaultMode", "Auto",
		"running", 1,
		"queue", []any{queued.ID, 999, queued.ID},
		"limit", 3,
	))
	list := h.value(h.phone, "agents.list", map[string]any{})
	sessions := list.Get("sessions").Items()
	if len(sessions) != 3 {
		t.Fatalf("sessions %s", list.Get("sessions").Compact())
	}
	if sessions[0].Get("id").Int() != running.ID || sessions[1].Get("id").Int() != queued.ID ||
		sessions[2].Get("id").Int() != old.ID {
		t.Fatalf("order %s", list.Get("sessions").Compact())
	}
	if !sessions[1].Get("held").BoolValue() || len16(sessions[1].Get("prompt").Str()) != 600 {
		t.Fatalf("held row %s", sessions[1].Compact())
	}
	attachment := sessions[0].Get("attachments").At(0)
	canonical, _ := filepath.EvalSymlinks(image)
	if attachment.Get("id").Str() != agentrun.AttachmentID(h.run.Salt(), running.ID, canonical) ||
		attachment.Get("name").Str() != "shot.png" || sessions[0].Get("terminal").Str() != "t-1" {
		t.Fatalf("running row %s", sessions[0].Compact())
	}
	want := `{"baseBranch":"main","branchName":"agent/old","costUsd":0.125,"createdAtMs":` +
		qtjson.FormatDouble(float64(old.CreatedAtMs)) + `,"diffStats":{"added":5,"ahead":1,"behind":0,"conflicted":false,"dirty":0,"files":2,"removed":1},"durationMs":4200,"finishedAtMs":1700000000500,"id":` +
		qtjson.NewInt(int64(old.ID)).String() + `,"issueNumber":0,"issueTitle":"","lastError":"","merged":false,"mode":"","model":"","numTurns":3,"prNumber":7,"priority":50,"prompt":"old run ***","provider":"claude-code","repositoryName":"demo","repositoryOwner":"alice","startedAtMs":0,"status":"success","stoppable":false,"totalTokens":900,"unread":true,"worktree":true}`
	if got := string(sessions[2].Compact()); got != want {
		t.Fatalf("row\n got %s\nwant %s", got, want)
	}
	if got := string(list.Get("outside").Compact()); got != `[{"id":-1001,"kind":"claude","title":"x ***"}]` {
		t.Fatalf("outside %s", got)
	}
	if got := string(list.Get("repositories").Compact()); got != `[{"name":"demo","owner":"alice"}]` {
		t.Fatalf("repositories %s", got)
	}
	if got := string(list.Get("folders").Compact()); got != `[{"collapsed":false,"id":"f1","members":["alice/demo#7"],"name":"Release","repositoryName":"demo","repositoryOwner":"alice"}]` {
		t.Fatalf("folders %s", got)
	}
	if got := string(list.Get("queue").Compact()); got != `{"limit":3,"limitMax":20,"queue":[],"running":1}` {
		t.Fatalf("queue (held sessions are not queued) %s", got)
	}
	h.attach(false)
	if detached := h.value(h.phone, "agents.list", nil); detached.Get("running").Int() != 2 {
		t.Fatalf("without a desktop the running count comes from the store: %s", detached.Get("queue").Compact())
	}
	h.attach(true)
	h.save(queued, func(record *qtjson.Value) { record.Set("held", qtjson.NewBool(false)) })
	queue := h.value(h.phone, "agents.queue", map[string]any{})
	if got := string(queue.Compact()); got != `{"limit":3,"limitMax":20,"queue":[`+qtjson.NewInt(int64(queued.ID)).String()+`],"running":1}` {
		t.Fatalf("queue %s", got)
	}
}

func TestAgentsChangedBellIsSpaced(t *testing.T) {
	h := newHarness(t)
	h.credentials()
	h.push(qtjson.ObjectOf("seq", 1))
	h.phone.Next(t, "agents.changed", time.Second)
	first := time.Now()
	for i := 2; i < 6; i++ {
		h.push(qtjson.ObjectOf("seq", i))
	}
	h.phone.Next(t, "agents.changed", time.Second)
	if gap := time.Since(first); gap < 250*time.Millisecond {
		t.Fatalf("second bell after %s", gap)
	}
	select {
	case event := <-h.phone.Events:
		if event.Name == "agents.changed" {
			t.Fatalf("the pushes inside one window ring once")
		}
	case <-time.After(400 * time.Millisecond):
	}
}

func TestStaleContextIsIgnored(t *testing.T) {
	h := newHarness(t)
	h.push(qtjson.ObjectOf("seq", 5, "defaultMode", "Plan"))
	h.push(qtjson.ObjectOf("seq", 4, "defaultMode", "Ask"))
	snapshot, _ := h.model.snapshot()
	if snapshot.DefaultMode != "Plan" {
		t.Fatalf("stale push applied: %q", snapshot.DefaultMode)
	}
	_ = os.Getenv
}
