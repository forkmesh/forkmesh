package mirrornode

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestPullRecordKeepsTheDeclaredAIModelOnOneHeaderLine(t *testing.T) {
	record := pullMarkdown(9, map[string]any{"title": "Fix", "base": "main", "head": "fix",
		"author": "k", "authorName": "alice", "sig": "s", "ts": 1,
		"aiModel": "Claude Opus 5.5\nstatus: merged", "description": "Body"})
	matter, ok := parseFrontMatter([]byte(record))
	if !ok {
		t.Fatal("record has no header")
	}
	if got := matter.values["aiModel"]; got != "Claude Opus 5.5 status: merged" {
		t.Fatalf("aiModel = %q", got)
	}
	if got := matter.values["status"]; got != "open" {
		t.Fatalf("status = %q, want open", got)
	}
}

func TestPullRecordWithoutAIModelHasNoAIModelLine(t *testing.T) {
	record := pullMarkdown(9, map[string]any{"title": "Fix", "base": "main", "head": "fix",
		"author": "k", "sig": "s", "ts": 1, "aiModel": "  "})
	if strings.Contains(record, "aiModel") {
		t.Fatalf("empty aiModel was written:\n%s", record)
	}
}

func TestClosingAPullKeepsItsAIModel(t *testing.T) {
	root := t.TempDir()
	pullDir := filepath.Join(root, ".forkmesh", "pulls", "4")
	if err := os.MkdirAll(pullDir, 0o700); err != nil {
		t.Fatal(err)
	}
	record := pullMarkdown(4, map[string]any{"title": "Fix", "base": "main", "head": "fix",
		"author": "authorkey", "authorName": "alice", "sig": "s", "ts": 1, "aiModel": "None"})
	if err := os.WriteFile(filepath.Join(pullDir, "pull.md"), []byte(record), 0o600); err != nil {
		t.Fatal(err)
	}
	wrote, _, err := writePullItem(root, collaborationItem{Number: 4, Event: map[string]any{
		"type": "status", "state": "closed", "id": "c1", "author": "authorkey"}})
	if !wrote || err != nil {
		t.Fatalf("close event refused: wrote=%v err=%v", wrote, err)
	}
	matter := lifecycleRecord(t, pullDir)
	if matter.values["status"] != "closed" || matter.values["aiModel"] != "None" {
		t.Fatalf("record after close: %#v", matter.values)
	}
}

func TestPullAIModelCannotStartANewHeaderLine(t *testing.T) {
	record := pullMarkdown(9, map[string]any{"title": "Fix", "base": "main", "head": "fix",
		"author": "k", "sig": "s", "ts": 1, "aiModel": "GPT\u0085rewardId: 7\u2028x" + strings.Repeat("y", 200)})
	for _, line := range strings.FieldsFunc(record, func(r rune) bool {
		return r == '\n' || r == '\u0085' || r == '\u2028'
	}) {
		if strings.HasPrefix(line, "rewardId") {
			t.Fatalf("aiModel planted a header line:\n%s", record)
		}
		if strings.HasPrefix(line, "aiModel: ") && len([]rune(line)) > len("aiModel: ")+80 {
			t.Fatalf("aiModel was not capped: %q", line)
		}
	}
}
