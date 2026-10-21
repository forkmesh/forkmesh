package mirrornode

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func moderationItem(eventType string, fields map[string]any, approved bool) collaborationItem {
	event := map[string]any{"type": eventType, "id": eventType + "-web-1",
		"author": "owner-web-key", "authorName": "alice", "ts": float64(1700000300), "sig": "sig"}
	for key, value := range fields {
		event[key] = value
	}
	item := collaborationItem{Number: 3, Event: event}
	if approved {
		item.Meta = map[string]any{"ownerApproved": true}
	}
	return item
}

func discussionFile(t *testing.T, root string) string {
	t.Helper()
	raw, err := os.ReadFile(filepath.Join(root, ".forkmesh", "discussions", "3", "discussion.md"))
	if err != nil {
		t.Fatal(err)
	}
	return string(raw)
}

func TestDiscussionStatusNeedsTheOwnersApproval(t *testing.T) {
	root := answerFixture(t, "Ideas")
	if wrote, _, err := writeDiscussionItem(root, moderationItem("status", map[string]any{"status": "closed"}, false)); wrote || err != nil {
		t.Fatalf("an unapproved status change was written: %v %v", wrote, err)
	}
	if !strings.Contains(discussionFile(t, root), "status: open\n") {
		t.Fatal("an unapproved status change touched discussion.md")
	}
	if wrote, _, _ := writeDiscussionItem(root, moderationItem("status", map[string]any{"status": "frozen"}, true)); wrote {
		t.Fatal("an unknown status was written")
	}
	for _, status := range []string{"closed", "archived", "open"} {
		wrote, number, err := writeDiscussionItem(root, moderationItem("status", map[string]any{"status": status}, true))
		if !wrote || number != 3 || err != nil {
			t.Fatalf("the owner's %s was refused: %v %v %v", status, wrote, number, err)
		}
		text := discussionFile(t, root)
		if !strings.Contains(text, "status: "+status+"\n") || strings.Count(text, "status:") != 1 {
			t.Fatalf("status %s not recorded once:\n%s", status, text)
		}
		// Everything else about the opening post is untouched.
		for _, keep := range []string{"title: How?\n", "author: opener-key\n", "sig: s\n", "\nquestion\n"} {
			if !strings.Contains(text, keep) {
				t.Fatalf("status change lost %q:\n%s", keep, text)
			}
		}
	}
}

func TestClosedAndArchivedDiscussionsRefuseReplies(t *testing.T) {
	root := answerFixture(t, "Ideas")
	reply := collaborationItem{Number: 3, Event: map[string]any{
		"type": "comment", "id": "comment-web-2", "author": "bob-key", "authorName": "bob",
		"ts": float64(1700000400), "body": "late reply", "sig": "sig"}}
	for _, status := range []string{"closed", "archived"} {
		if wrote, _, err := writeDiscussionItem(root, moderationItem("status", map[string]any{"status": status}, true)); !wrote || err != nil {
			t.Fatal(err)
		}
		if wrote, _, err := writeDiscussionItem(root, reply); wrote || !errors.Is(err, errDiscussionLocked) {
			t.Fatalf("a reply to a %s discussion was taken: %v %v", status, wrote, err)
		}
	}
	if wrote, _, _ := writeDiscussionItem(root, moderationItem("status", map[string]any{"status": "open"}, true)); !wrote {
		t.Fatal("reopen refused")
	}
	if wrote, _, err := writeDiscussionItem(root, reply); !wrote || err != nil {
		t.Fatalf("a reply to a reopened discussion was refused: %v %v", wrote, err)
	}
}

func TestOwnerDeletesOneReply(t *testing.T) {
	root := answerFixture(t, "Ideas")
	dir := filepath.Join(root, ".forkmesh", "discussions", "3")
	if wrote, _, _ := writeDiscussionItem(root, moderationItem("delete", map[string]any{"target": "reply-1"}, false)); wrote {
		t.Fatal("an unapproved delete was consumed")
	}
	if _, err := os.Stat(filepath.Join(dir, "0002-comment.md")); err != nil {
		t.Fatal("an unapproved delete removed the reply")
	}
	if wrote, _, err := writeDiscussionItem(root, moderationItem("delete", map[string]any{"target": "reply-1"}, true)); !wrote || err != nil {
		t.Fatalf("the owner's delete was refused: %v %v", wrote, err)
	}
	if _, err := os.Stat(filepath.Join(dir, "0002-comment.md")); !os.IsNotExist(err) {
		t.Fatal("the reply is still there")
	}
	if _, err := os.Stat(filepath.Join(dir, "discussion.md")); err != nil {
		t.Fatal("deleting a reply removed the discussion")
	}
	// A replayed delete is consumed (acknowledged), not retried forever.
	if wrote, _, err := writeDiscussionItem(root, moderationItem("delete", map[string]any{"target": "reply-1"}, true)); !wrote || err != nil {
		t.Fatalf("a repeated delete was not consumed: %v %v", wrote, err)
	}
}

func TestWebOpenedDiscussionIsWrittenLikeADesktopOne(t *testing.T) {
	root := t.TempDir()
	item := collaborationItem{Number: 0, TitleIfNew: "Roadmap", Event: map[string]any{
		"type": "open", "id": "open-web-1700000000000-abc", "author": "alice-web-key",
		"authorName": "alice", "ts": float64(1700000000000), "title": "Roadmap",
		"body": "What next?", "category": "Q&A", "sig": "sig"}}
	wrote, number, err := writeDiscussionItem(root, item)
	if !wrote || number != 1 || err != nil {
		t.Fatalf("open: %v %v %v", wrote, number, err)
	}
	text, _ := os.ReadFile(filepath.Join(root, ".forkmesh", "discussions", "1", "discussion.md"))
	for _, want := range []string{"schema: forkmesh-discussion-v1\n", "number: 1\n", "title: Roadmap\n",
		"category: Q&A\n", "status: open\n", "author: alice-web-key\n", "authorName: alice\n",
		"type: open\n", "ts: 1700000000000\n", "sig: sig\n", "\nWhat next?\n"} {
		if !strings.Contains(string(text), want) {
			t.Fatalf("discussion.md lacks %q:\n%s", want, text)
		}
	}
	matter, ok := parseFrontMatter(text)
	if !ok || matter.values["status"] != "open" {
		t.Fatal("the written discussion does not parse")
	}
}

func TestDiscussionInboxMaterializesWebEventsAndAcknowledgesLockedReplies(t *testing.T) {
	_, bare := makeBareRepo(t)
	repo := Repository{Owner: "alice", Name: "garden", GitDir: bare}
	rows := []json.RawMessage{
		json.RawMessage(`{"id":1,"number":0,"titleIfNew":"Roadmap","event":{"type":"open","id":"open-web-1","title":"Roadmap","body":"What next?","category":"Ideas","author":"alice-key","authorName":"alice","ts":1700000000000,"sig":"s1"}}`),
		json.RawMessage(`{"id":2,"number":1,"event":{"type":"comment","id":"comment-web-2","body":"tests first","author":"bob-key","authorName":"bob","ts":1700000001,"sig":"s2"}}`),
		json.RawMessage(`{"id":3,"number":1,"event":{"type":"status","id":"status-web-3","status":"closed","author":"alice-key","authorName":"alice","ts":1700000002,"sig":"s3"},"meta":{"ownerApproved":true}}`),
		json.RawMessage(`{"id":4,"number":1,"event":{"type":"comment","id":"comment-web-4","body":"too late","author":"bob-key","authorName":"bob","ts":1700000003,"sig":"s4"}}`),
		json.RawMessage(`{"id":5,"number":1,"event":{"type":"delete","id":"delete-web-5","target":"comment-web-2","author":"alice-key","authorName":"alice","ts":1700000004,"sig":"s5"},"meta":{"ownerApproved":true}}`),
		json.RawMessage(`{"id":6,"number":1,"event":{"type":"status","id":"status-web-6","status":"open","author":"bob-key","authorName":"bob","ts":1700000005,"sig":"s6"}}`),
	}
	ids, err := materializeDiscussionInbox(context.Background(), repo, rows, "mirror9")
	if err != nil {
		t.Fatal(err)
	}
	// Row 6 (a status change nobody approved) stays queued; the locked reply
	// (row 4) is acknowledged because it can never be written.
	if fmt.Sprint(ids) != "[1 2 3 4 5]" {
		t.Fatalf("acknowledged %v", ids)
	}
	listing := run(t, ".", "git", "--git-dir="+bare, "ls-tree", "-r", "--name-only", "main", ".forkmesh/discussions")
	if !strings.Contains(listing, ".forkmesh/discussions/1/discussion.md") || strings.Contains(listing, "comment.md") {
		t.Fatalf("unexpected discussion tree:\n%s", listing)
	}
	opened := run(t, ".", "git", "--git-dir="+bare, "show", "main:.forkmesh/discussions/1/discussion.md")
	if !strings.Contains(opened, "status: closed\n") || !strings.Contains(opened, "title: Roadmap\n") {
		t.Fatalf("discussion.md:\n%s", opened)
	}
}
