package agentmodel

import (
	"encoding/base64"
	"path/filepath"
	"strings"
	"testing"

	"forkmesh.com/desktop-core/internal/coretest"
	"forkmesh.com/desktop-core/internal/qtjson"
	"forkmesh.com/desktop-core/internal/rpc"
)

func repoContext(seq int, path string) qtjson.Value {
	return qtjson.ObjectOf("seq", seq, "repositories", qtjson.NewArray(
		qtjson.ObjectOf("owner", "alice", "name", "demo", "localPath", path, "active", true)))
}

func TestAgentOpsOwnerGateAndArming(t *testing.T) {
	h := newHarness(t)
	stranger := coretest.NewPeer("lan:2", rpc.LAN)
	stranger.Dev = rpc.Device{Key: "k2", Name: "Other", User: "mallory"}
	h.fails(stranger, "agentops.prepare", map[string]any{"action": "stop", "ids": []int{1}}, "not_owner")
	h.fails(stranger, "diag.prepare", map[string]any{"action": "sendStalls"}, "not_owner")
	h.push(qtjson.ObjectOf("seq", 1))
	h.value(h.phone, "agentops.prepare", map[string]any{"action": "stop", "ids": []int{2, 1, 2}})
	h.attach(true)
	h.value(h.phone, "agentops.prepare", map[string]any{"action": "stop", "ids": []int{1}})
	h.value(h.phone, "agentops.prepare", map[string]any{"action": "stop", "ids": []int{1}})
	if got := h.desktop.noted("ui.agents.opsArm"); got != 1 {
		t.Fatalf("the desktop's agentops bell is armed once per desktop connection, got %d", got)
	}
	h.model.uiChanged(true)
	waitFor(t, "re-armed", func() bool { return h.desktop.noted("ui.agents.opsArm") == 2 })
}

func TestAgentOpsConfirmedRequestsReachTheDesktopMarked(t *testing.T) {
	h := newHarness(t)
	h.attach(true)
	session := h.create("owner", "alice", "name", "demo", "status", "success", "branchName", "agent/x")
	h.push(repoContext(1, "/nowhere"))
	prepared := h.value(h.phone, "agentops.prepare", map[string]any{"action": "merge", "id": session.ID})
	if prepared.Get("action").Str() != "merge" || !prepared.Get("destructive").BoolValue() ||
		prepared.Get("expiresInMs").Int() != 60000 ||
		prepared.Get("summary").Str() != mergeSummary("agent/x", "main", "alice/demo") {
		t.Fatalf("prepare %s", prepared.Compact())
	}
	token := prepared.Get("token").Str()
	h.fails(h.phone, "agentops.merge", map[string]any{"id": session.ID + 1, "token": token}, "confirmation_mismatch")
	h.fails(h.phone, "agentops.merge", map[string]any{"id": session.ID, "token": token}, "confirmation_required")
	token = h.value(h.phone, "agentops.prepare", map[string]any{"action": "merge", "id": session.ID}).Get("token").Str()
	forwarded := h.delegatedParams(h.phone, "agentops.merge", map[string]any{"id": session.ID, "token": token,
		"_coreConfirmed": "forged"})
	if forwarded.Get("_coreConfirmed").Str() != h.model.confirms.secret || forwarded.Get("id").Int() != session.ID {
		t.Fatalf("forwarded %s", forwarded.Compact())
	}
	plain := h.delegatedParams(h.phone, "agentops.accounts.select", map[string]any{"provider": "codex",
		"_coreConfirmed": "forged"})
	if plain.Has("_coreConfirmed") {
		t.Fatalf("a phone cannot pass the marker through: %s", plain.Compact())
	}
	other := coretest.NewPeer("lan:9", rpc.LAN)
	other.Dev = h.phone.Dev
	token = h.value(h.phone, "agentops.prepare", map[string]any{"action": "stop", "ids": []int{3}}).Get("token").Str()
	h.fails(other, "agentops.bulk", map[string]any{"action": "stop", "ids": []int{3}, "token": token}, "confirmation_mismatch")
	h.fails(h.phone, "agentops.bulk", map[string]any{"action": "nope", "ids": []int{3}}, "bad_request")
	h.fails(h.phone, "agentops.answer", map[string]any{"id": "x", "decision": "allow"}, "bad_request")
	h.fails(h.phone, "agentops.prepare", map[string]any{"action": "dance"}, "unknown_action")
	h.fails(h.phone, "agentops.prepare", map[string]any{"action": "purge", "ids": []any{1.5}}, "bad_request")
	h.fails(h.phone, "agentops.prepare", map[string]any{"action": "merge", "id": 999}, "unknown_session")
}

func TestAgentOpsPrepareApprovalsAndGrantsUseFreshDesktopFacts(t *testing.T) {
	h := newHarness(t)
	id := strings.Repeat("a", 32)
	h.attach(true)
	h.desktop.context = qtjson.ObjectOf("seq", 9,
		"approvals", qtjson.NewArray(qtjson.ObjectOf("id", id, "question", "Run command:\nls", "title", "Fix")),
		"approvalIndex", qtjson.NewArray(qtjson.ObjectOf("id", id, "session", 4, "request", "r1")),
		"grants", qtjson.NewArray(qtjson.ObjectOf("id", "0123abcd-0123-4567-89ab-0123456789ab",
			"capability", "file.read", "resource", "/home/a/secret.txt", "decision", "allow", "lifetime", "persistent")))
	approvals := h.value(h.phone, "agentops.approvals", nil)
	if approvals.Get("count").Int() != 1 {
		t.Fatalf("approvals %s", approvals.Compact())
	}
	prepared := h.value(h.phone, "agentops.prepare", map[string]any{"action": "approve", "id": id, "lifetime": "session"})
	if prepared.Get("summary").Str() != approvalSummary(true, "session", "Run command:\nls", "Fix") {
		t.Fatalf("approve %s", prepared.Compact())
	}
	h.fails(h.phone, "agentops.prepare", map[string]any{"action": "approve", "id": strings.Repeat("b", 32)}, "unknown_approval")
	h.fails(h.phone, "agentops.prepare", map[string]any{"action": "approve", "id": id, "lifetime": "forever"}, "bad_request")
	answer := h.delegatedParams(h.phone, "agentops.answer", map[string]any{"id": id, "decision": "allow",
		"lifetime": "session", "token": prepared.Get("token").Str()})
	if answer.Get("_coreConfirmed").Str() == "" {
		t.Fatalf("answer %s", answer.Compact())
	}
	grants := h.value(h.phone, "agentops.grants", nil)
	if got := string(grants.Compact()); got != `{"grants":[{"capability":"file.read","decision":"allow","id":"0123abcd-0123-4567-89ab-0123456789ab","kind":"file","lifetime":"persistent","resource":"secret.txt"}]}` {
		t.Fatalf("grants %s", got)
	}
	revoke := h.value(h.phone, "agentops.prepare", map[string]any{"action": "revoke", "grantId": "0123abcd-0123-4567-89ab-0123456789ab"})
	if revoke.Get("summary").Str() != "Revoke the saved permission for file.read on secret.txt. Agents are asked again next time." {
		t.Fatalf("revoke %s", revoke.Compact())
	}
	h.fails(h.phone, "agentops.prepare", map[string]any{"action": "revoke", "grantId": "0123abcd-0123-4567-89ab-0123456789ac"}, "unknown_grant")
}

func TestAgentOpsDiffWorktreesAndMergeStats(t *testing.T) {
	h := newHarness(t)
	h.credentials("hunter2-secret")
	repo := gitRepo(t)
	runGit(t, repo, "checkout", "-q", "-b", "agent/x")
	writeFile(t, filepath.Join(repo, "a.txt"), "one\ntwo hunter2-secret\n")
	writeFile(t, filepath.Join(repo, "img.png"), "\x89PNG\r\n\x1a\nimage")
	runGit(t, repo, "add", ".")
	runGit(t, repo, "commit", "-q", "-m", "agent work")
	runGit(t, repo, "checkout", "-q", "main")
	worktree := filepath.Join(t.TempDir(), "wt")
	runGit(t, repo, "worktree", "add", "-q", worktree, "agent/x")
	writeFile(t, filepath.Join(worktree, "new.txt"), "fresh\n")
	writeFile(t, filepath.Join(worktree, "a.txt"), "one\ntwo hunter2-secret\nthree\n")
	session := h.create("owner", "alice", "name", "demo", "status", "success", "branchName", "agent/x")
	h.push(repoContext(1, repo))
	diff := h.value(h.phone, "agentops.diff", map[string]any{"id": session.ID})
	if diff.Get("base").Str() != "main" || diff.Get("files").Len() != 2 || !diff.Get("hasWorktree").BoolValue() ||
		!strings.Contains(diff.Get("committed").Str(), "+two ***") || !strings.Contains(diff.Get("uncommitted").Str(), "+three") ||
		diff.Get("untracked").At(0).Str() != "new.txt" || diff.Get("truncated").BoolValue() {
		t.Fatalf("diff %s", diff.Compact())
	}
	file := h.value(h.phone, "agentops.file", map[string]any{"id": session.ID, "path": "img.png", "change": "committed", "side": "new"})
	data, _ := base64.StdEncoding.DecodeString(file.Get("data").Str())
	if string(data) != "\x89PNG\r\n\x1a\nimage" || file.Get("mime").Str() != "image/png" || file.Get("size").Int() != len(data) {
		t.Fatalf("file %s", file.Compact())
	}
	h.fails(h.phone, "agentops.file", map[string]any{"id": session.ID, "path": "img.png", "change": "committed", "side": "old"}, "file_missing")
	h.fails(h.phone, "agentops.file", map[string]any{"id": session.ID, "path": "../x.png", "change": "committed", "side": "new"}, "bad_request")
	worktrees := h.value(h.phone, "agentops.worktrees", map[string]any{"owner": "alice", "name": "demo"})
	rows := worktrees.Get("worktrees").Items()
	if worktrees.Get("base").Str() != "main" || len(rows) != 2 || rows[1].Get("sessionId").Int() != session.ID ||
		rows[1].Get("ahead").Int() != 1 || !rows[0].Get("isMain").BoolValue() {
		t.Fatalf("worktrees %s", worktrees.Compact())
	}
	h.fails(h.phone, "agentops.worktrees", map[string]any{"owner": "bob", "name": "x"}, "no_checkout")
	runGit(t, repo, "merge", "-q", "--no-ff", "-m", "Merge branch 'agent/x' into main", "agent/x")
	stats := h.value(h.phone, "agentops.mergestats", map[string]any{"id": session.ID})
	if stats.Get("repo").Str() != "alice/demo" || stats.Get("branch").Str() != "agent/x" ||
		stats.Get("stats").Get("files").Int() != 2 || stats.Get("stats").Get("commits").Int() != 2 ||
		stats.Get("stats").Get("velocity").Len() != 5 {
		t.Fatalf("mergestats %s", stats.Compact())
	}
	newest := h.value(h.phone, "agentops.mergestats", nil)
	if newest.Get("branch").Str() != "agent/x" {
		t.Fatalf("newest session's repository %s", newest.Compact())
	}
	h.fails(h.phone, "agentops.diff", map[string]any{"id": 777}, "unknown_session")
}

func TestAgentOpsRecycleBin(t *testing.T) {
	h := newHarness(t)
	h.attach(true)
	h.push(qtjson.ObjectOf("seq", 1))
	keep := h.create("owner", "alice", "name", "demo", "status", "running", "prompt", "keep me\nmore")
	gone := h.create("owner", "alice", "name", "demo", "status", "success", "prompt", "gone")
	for _, session := range []int{keep.ID, gone.ID} {
		found, _ := h.store.Writer().Store().FindSession(session)
		record, _ := h.model.rawRecord(h.store.Writer().Store(), found)
		if ok, err := h.store.Writer().RecycleRecord(record); !ok || err != nil {
			t.Fatal(ok, err)
		}
	}
	h.fails(h.phone, "agentops.bin.restore", map[string]any{"ids": []any{}}, "bad_request")
	restored := h.value(h.phone, "agentops.bin.restore", map[string]any{"ids": []int{keep.ID, 999}})
	if got := string(restored.Get("restored").Compact()) + string(restored.Get("failed").Compact()); got != "["+
		qtjson.NewInt(int64(keep.ID)).String()+"][999]" || restored.Get("count").Int() != 1 ||
		restored.Get("retentionHours").Int() != 24 {
		t.Fatalf("restore %s", restored.Compact())
	}
	back, ok := h.store.Writer().Store().FindSession(keep.ID)
	if !ok || back.Status != "stopped" {
		t.Fatalf("a restored running session comes back stopped: %+v", back)
	}
	if h.desktop.noted("ui.agents.reload") < 2 {
		t.Fatalf("the desktop reloads its list and bin strip: %v", h.desktop.notes)
	}
	h.fails(h.phone, "agentops.bin.purge", map[string]any{"ids": []int{gone.ID}}, "confirmation_required")
	token := h.value(h.phone, "agentops.prepare", map[string]any{"action": "purge", "ids": []int{gone.ID}}).Get("token").Str()
	purged := h.value(h.phone, "agentops.bin.purge", map[string]any{"ids": []int{gone.ID}, "token": token})
	if got := string(purged.Get("purged").Compact()); got != "["+qtjson.NewInt(int64(gone.ID)).String()+"]" ||
		purged.Get("count").Int() != 0 {
		t.Fatalf("purge %s", purged.Compact())
	}
	h.fails(h.phone, "agentops.bin.retention", map[string]any{"hours": 0.5}, "bad_request")
	h.fails(h.phone, "agentops.bin.retention", map[string]any{"hours": "2"}, "bad_request")
	retention := h.value(h.phone, "agentops.bin.retention", map[string]any{"hours": 48})
	if retention.Get("retentionHours").Int() != 48 || len(h.desktop.writes) != 1 ||
		!strings.Contains(string(h.desktop.writes[0]), `"key":"agents/recycleRetentionHours"`) {
		t.Fatalf("retention %s %s", retention.Compact(), h.desktop.writes)
	}
	h.attach(false)
	h.fails(h.phone, "agentops.bin.retention", map[string]any{"hours": 48}, "desktop_unavailable")
}

func TestStopAndQueueMoveWhileTheDesktopIsAway(t *testing.T) {
	h := newHarness(t)
	h.credentials()
	a := h.create("owner", "alice", "name", "demo", "status", "queued", "priority", 10)
	b := h.create("owner", "alice", "name", "demo", "status", "queued", "priority", 20)
	c := h.create("owner", "alice", "name", "demo", "status", "queued", "priority", 30)
	done := h.create("owner", "alice", "name", "demo", "status", "success")
	h.push(qtjson.ObjectOf("seq", 1, "queue", intsJSON([]int{a.ID, b.ID, c.ID}), "limit", 2))
	moved := h.value(h.phone, "agents.queue.move", map[string]any{"id": c.ID, "to": 0})
	if got := string(moved.Get("queue").Compact()); got != "["+strings.Join([]string{itoa(c.ID), itoa(a.ID), itoa(b.ID)}, ",")+"]" {
		t.Fatalf("moved %s", moved.Compact())
	}
	moved2, _ := h.store.Writer().Store().FindSession(c.ID)
	if moved2.Priority != 5 {
		t.Fatalf("priority %d (half way between 0 and 10)", moved2.Priority)
	}
	h.fails(h.phone, "agents.queue.move", map[string]any{"id": done.ID, "to": 0}, "not_queued")
	h.fails(h.phone, "agents.queue.move", map[string]any{"id": a.ID}, "not_queued")
	h.call(h.phone, "agents.stop", map[string]any{"id": a.ID}, nil)
	stopped, _ := h.store.Writer().Store().FindSession(a.ID)
	if stopped.Status != "stopped" || stopped.FinishedAtMs == 0 {
		t.Fatalf("stopped %+v", stopped)
	}
	queue := h.value(h.phone, "agents.queue", nil)
	if got := string(queue.Get("queue").Compact()); got != "["+itoa(c.ID)+","+itoa(b.ID)+"]" {
		t.Fatalf("queue after stop %s", got)
	}
	h.fails(h.phone, "agents.stop", map[string]any{"id": done.ID}, "not_running")
	h.fails(h.phone, "agents.start", map[string]any{"task": "x"}, "desktop_unavailable")
	h.attach(true)
	h.delegatedParams(h.phone, "agents.stop", map[string]any{"id": b.ID})
	h.delegatedParams(h.phone, "agents.start", map[string]any{"task": "x"})
}

func itoa(n int) string { return qtjson.NewInt(int64(n)).String() }

func TestAttachmentServedRawOnTheLocalNetwork(t *testing.T) {
	h := newHarness(t)
	h.credentials()
	image := filepath.Join(t.TempDir(), "x.png")
	writeFile(t, image, "\x89PNG\r\n\x1a\nrest")
	session := h.create("owner", "alice", "name", "demo", "status", "running", "prompt", "see\nAttached image: "+image)
	h.push(qtjson.ObjectOf("seq", 1))
	entries := h.model.attachmentEntries([]string{image}, session.ID, 1)
	payload := h.value(h.phone, "agents.attachment", map[string]any{"id": session.ID, "attachment": entries[0].ID})
	data, _ := base64.StdEncoding.DecodeString(payload.Get("data").Str())
	if string(data) != "\x89PNG\r\n\x1a\nrest" || payload.Get("mediaType").Str() != "image/png" ||
		payload.Get("preview").BoolValue() || payload.Get("name").Str() != "x.png" {
		t.Fatalf("payload %s", payload.Compact())
	}
	relay := coretest.NewPeer("relay:1", rpc.Relay)
	h.delegatedParams(relay, "agents.attachment", map[string]any{"id": session.ID, "attachment": entries[0].ID})
	h.delegatedParams(h.phone, "agents.attachment", map[string]any{"id": session.ID, "attachment": entries[0].ID, "preview": true})
	h.fails(h.phone, "agents.attachment", map[string]any{"id": session.ID, "attachment": "nothex"}, "unknown_attachment")
	h.fails(h.phone, "agents.attachment", map[string]any{"id": session.ID, "attachment": strings.Repeat("0", 32)}, "unknown_attachment")
}

func TestDiagPrepareAndRunStopAnAgent(t *testing.T) {
	h := newHarness(t)
	h.credentials()
	session := h.create("owner", "alice", "name", "demo", "status", "running", "prompt", "  first line  \nsecond")
	h.fails(h.phone, "diag.prepare", map[string]any{"action": "stopAgent"}, "bad_session")
	h.fails(h.phone, "diag.prepare", map[string]any{"action": "sendStall", "index": 1001}, "bad_index")
	h.fails(h.phone, "diag.prepare", map[string]any{"action": "x"}, "unsupported_action")
	prepared := h.value(h.phone, "diag.prepare", map[string]any{"action": "stopAgent", "session": session.ID})
	if prepared.Get("summary").Str() != `Stop the agent run "first line". Everything it started goes down with it; its worktree and branch are kept.` ||
		prepared.Get("subject").Str() != itoa(session.ID) {
		t.Fatalf("prepare %s", prepared.Compact())
	}
	h.fails(h.phone, "diag.run", map[string]any{"action": "stopAgent", "session": session.ID}, "confirmation_required")
	token := h.value(h.phone, "diag.prepare", map[string]any{"action": "stopAgent", "session": session.ID}).Get("token").Str()
	ran := h.value(h.phone, "diag.run", map[string]any{"action": "stopAgent", "session": session.ID, "token": token})
	if ran.Get("message").Str() != "Stop requested." || !ran.Get("started").BoolValue() {
		t.Fatalf("run %s", ran.Compact())
	}
	h.phone.Next(t, "diag.changed", 2e9)
	h.fails(h.phone, "diag.prepare", map[string]any{"action": "stopAgent", "session": session.ID}, "not_running")
	h.attach(true)
	h.store.Writer()
	again := h.create("owner", "alice", "name", "demo", "status", "running")
	token = h.value(h.phone, "diag.prepare", map[string]any{"action": "stopAgent", "session": again.ID}).Get("token").Str()
	forwarded := h.delegatedParams(h.phone, "diag.run", map[string]any{"action": "stopAgent", "session": again.ID, "token": token})
	if forwarded.Get("_coreConfirmed").Str() != h.model.confirms.secret {
		t.Fatalf("diag.run goes to the desktop confirmed: %s", forwarded.Compact())
	}
}

func TestAgentOpsUsageSummarisedInTheCore(t *testing.T) {
	h := newHarness(t)
	now := h.model.nowMs()
	h.desktop.usage = qtjson.ObjectOf("activeProvider", "codex", "claudeSpendUsd", 2.5,
		"providers", qtjson.NewArray(qtjson.ObjectOf("id", "claude-code", "label", "Claude Code",
			"windows", qtjson.NewArray(
				qtjson.ObjectOf("id", "5h", "label", "5-hour", "usedPercent", 90, "resetAtMs", float64(now+60000)),
				qtjson.ObjectOf("id", "weekly", "label", "Weekly", "usedPercent", 50, "resetAtMs", float64(now-1))),
			"accounts", qtjson.NewArray(), "creditsEnabled", false, "creditsUsed", -1, "creditsLimit", -1)))
	h.fails(h.phone, "agentops.usage", nil, "desktop_unavailable")
	h.attach(true)
	usage := h.value(h.phone, "agentops.usage", nil)
	windows := usage.Get("providers").At(0).Get("windows")
	if usage.Get("activeProvider").Str() != "codex" || usage.Get("spend").Get("claudeUsd").Float() != 2.5 ||
		windows.At(0).Get("state").Str() != "high" || windows.At(1).Get("usedPercent").Int() != 0 ||
		windows.At(1).Has("resetAtMs") {
		t.Fatalf("usage %s", usage.Compact())
	}
	h.attach(false)
	if cached := h.value(h.phone, "agentops.usage", nil); cached.Get("providers").Len() != 1 {
		t.Fatalf("the last answer is kept for a desktop that went away: %s", cached.Compact())
	}
}
