package mirrornode

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func writeLifecyclePull(t *testing.T, status string) (string, string) {
	t.Helper()
	root := t.TempDir()
	pullDir := filepath.Join(root, ".forkmesh", "pulls", "4")
	if err := os.MkdirAll(pullDir, 0o700); err != nil {
		t.Fatal(err)
	}
	record := pullMarkdown(4, map[string]any{"title": "Fix", "base": "main", "head": "fix",
		"author": "authorkey", "authorName": "alice", "sig": "s", "ts": 1})
	record = strings.Replace(record, "status: open", "status: "+status, 1)
	for name, body := range map[string]string{"pull.md": record, "changes.patch": "diff", "commits.mbox": "mbox"} {
		if err := os.WriteFile(filepath.Join(pullDir, name), []byte(body), 0o600); err != nil {
			t.Fatal(err)
		}
	}
	return root, pullDir
}

func lifecycleRecord(t *testing.T, pullDir string) *frontMatter {
	t.Helper()
	raw, err := os.ReadFile(filepath.Join(pullDir, "pull.md"))
	if err != nil {
		t.Fatal(err)
	}
	matter, ok := parseFrontMatter(raw)
	if !ok {
		t.Fatal("pull record lost its header")
	}
	return matter
}

func TestPullCloseEventClosesTheRecordAndKeepsThePayload(t *testing.T) {
	root, pullDir := writeLifecyclePull(t, "open")
	wrote, _, err := writePullItem(root, collaborationItem{Number: 4, Event: map[string]any{
		"type": "status", "state": "closed", "id": "c1", "author": "authorkey"}})
	if !wrote || err != nil {
		t.Fatalf("close event refused: wrote=%v err=%v", wrote, err)
	}
	if got := lifecycleRecord(t, pullDir).values["status"]; got != "closed" {
		t.Fatalf("status = %q, want closed", got)
	}
	if _, err := os.Stat(filepath.Join(pullDir, "changes.patch")); err != nil {
		t.Fatalf("close removed the patch: %v", err)
	}
}

func TestPullDeleteEventRetiresTheRecordAndDropsThePayload(t *testing.T) {
	root, pullDir := writeLifecyclePull(t, "closed")
	wrote, _, err := writePullItem(root, collaborationItem{Number: 4, Event: map[string]any{
		"type": "delete", "id": "d1", "author": "authorkey"}})
	if !wrote || err != nil {
		t.Fatalf("delete event refused: wrote=%v err=%v", wrote, err)
	}
	record := lifecycleRecord(t, pullDir)
	if got := record.values["status"]; got != "deleted" {
		t.Fatalf("status = %q, want deleted", got)
	}
	for _, name := range []string{"changes.patch", "commits.mbox"} {
		if _, err := os.Stat(filepath.Join(pullDir, name)); !os.IsNotExist(err) {
			t.Fatalf("%s survived delete: %v", name, err)
		}
	}
	if !pullRecordRetired(record) {
		t.Fatal("a deleted pull must retire its visible branch like a merged one")
	}
}

func TestPullLifecycleEventsNeverRewriteAMergedPull(t *testing.T) {
	for _, event := range []map[string]any{
		{"type": "status", "state": "closed", "id": "c2"},
		{"type": "delete", "id": "d2"},
	} {
		root, pullDir := writeLifecyclePull(t, "merged")
		if _, _, err := writePullItem(root, collaborationItem{Number: 4, Event: event}); err != nil {
			t.Fatal(err)
		}
		if got := lifecycleRecord(t, pullDir).values["status"]; got != "merged" {
			t.Fatalf("%v changed a merged pull to %q", event["type"], got)
		}
		if _, err := os.Stat(filepath.Join(pullDir, "changes.patch")); err != nil {
			t.Fatalf("%v removed a merged pull's patch: %v", event["type"], err)
		}
	}
}

func TestPullStatusEventOnlyCloses(t *testing.T) {
	root, pullDir := writeLifecyclePull(t, "closed")
	if _, _, err := writePullItem(root, collaborationItem{Number: 4, Event: map[string]any{
		"type": "status", "state": "open", "id": "o1"}}); err != nil {
		t.Fatal(err)
	}
	if got := lifecycleRecord(t, pullDir).values["status"]; got != "closed" {
		t.Fatalf("status = %q, want closed", got)
	}
}
