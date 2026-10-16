package mirrornode

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func answerFixture(t *testing.T, category string) string {
	t.Helper()
	root := t.TempDir()
	dir := filepath.Join(root, ".forkmesh", "discussions", "3")
	if err := os.MkdirAll(dir, 0700); err != nil {
		t.Fatal(err)
	}
	writeFile(t, filepath.Join(dir, "discussion.md"), strings.Join([]string{
		"---", "schema: forkmesh-discussion-v1", "number: 3", "title: How?",
		"category: " + category, "status: open", "author: opener-key", "type: open",
		"id: open-3", "ts: 1700000000000", "sig: s", "---", "", "question", ""}, "\n"))
	writeFile(t, filepath.Join(dir, "0002-comment.md"), strings.Join([]string{
		"---", "type: comment", "id: reply-1", "author: helper-key", "authorName: helper",
		"ts: 1700000100000", "sig: s", "---", "", "an answer", ""}, "\n"))
	return root
}

func answerItem(author, target string, ts int, approved bool) collaborationItem {
	item := collaborationItem{Number: 3, Event: map[string]any{
		"type": "answer", "id": "answer-1", "author": author, "authorName": "a",
		"ts": float64(ts), "target": target, "sig": "sig"}}
	if approved {
		item.Meta = map[string]any{"ownerApproved": true}
	}
	return item
}

func readAnswer(t *testing.T, root string) string {
	t.Helper()
	raw, err := os.ReadFile(filepath.Join(root, ".forkmesh", "discussions", "3", "answer.md"))
	if err != nil {
		return ""
	}
	return string(raw)
}

func TestDiscussionAnswerIntakeTakesTheOpenerOrAnOwnerApproval(t *testing.T) {
	root := answerFixture(t, "Q&A")
	if wrote, _, err := writeDiscussionItem(root, answerItem("stranger", "reply-1", 1700000200, false)); wrote || err != nil {
		t.Fatalf("a stranger's answer was written: %v %v", wrote, err)
	}
	if wrote, _, err := writeDiscussionItem(root, answerItem("opener-key", "nope", 1700000200, false)); wrote || err != nil {
		t.Fatalf("an answer naming no reply was written: %v %v", wrote, err)
	}
	if wrote, number, err := writeDiscussionItem(root, answerItem("opener-key", "reply-1", 1700000200, false)); !wrote || number != 3 || err != nil {
		t.Fatalf("the opener's answer was refused: %v %v %v", wrote, number, err)
	}
	answer := readAnswer(t, root)
	for _, want := range []string{"type: answer", "author: opener-key", "target: reply-1", "targetAuthor: helper-key", "ts: 1700000200"} {
		if !strings.Contains(answer, want+"\n") {
			t.Fatalf("answer.md lacks %q:\n%s", want, answer)
		}
	}
	if wrote, _, _ := writeDiscussionItem(root, answerItem("owner-web-key", "", 1700000100, true)); !wrote {
		t.Fatal("an older owner clear was not consumed")
	}
	if !strings.Contains(readAnswer(t, root), "target: reply-1\n") {
		t.Fatal("an older event replaced a newer answer")
	}
	if wrote, _, _ := writeDiscussionItem(root, answerItem("owner-web-key", "", 1700000300, true)); !wrote {
		t.Fatal("the owner's clear was refused")
	}
	if !strings.Contains(readAnswer(t, root), "target: \n") {
		t.Fatalf("the clear did not empty the target:\n%s", readAnswer(t, root))
	}
}

func TestDiscussionAnswerIntakeOnlyAnswersQuestions(t *testing.T) {
	root := answerFixture(t, "Ideas")
	if wrote, _, _ := writeDiscussionItem(root, answerItem("opener-key", "reply-1", 1700000200, false)); wrote || readAnswer(t, root) != "" {
		t.Fatal("an Ideas discussion took an answer")
	}
}
