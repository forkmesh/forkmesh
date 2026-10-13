package mirrornode

import (
	"context"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func agentWorkspaceFixture(t *testing.T) (string, string) {
	t.Helper()
	_, bare := makeBareRepo(t)
	dir := filepath.Join(t.TempDir(), "work", agentSessionDirectory("session-a"))
	if err := prepareAgentWorkspace(context.Background(), bare, dir, "agent/web-1-task", "main", "n1"); err != nil {
		t.Fatal(err)
	}
	return bare, dir
}

func agentTrash(t *testing.T, dir string) []string {
	t.Helper()
	output := run(t, dir, "git", "for-each-ref", "--format=%(refname)", agentTrashPrefix(dir))
	if output == "" {
		return nil
	}
	return strings.Split(output, "\n")
}

func TestAgentWorkspaceKeepsUnpushedCommit(t *testing.T) {
	bare, dir := agentWorkspaceFixture(t)
	writeTestFile(t, filepath.Join(dir, "work.txt"), "agent work\n")
	run(t, dir, "git", "add", "work.txt")
	run(t, dir, "git", "commit", "-q", "-m", "unpushed")
	unpushed := run(t, dir, "git", "rev-parse", "HEAD")
	if err := prepareAgentWorkspace(context.Background(), bare, dir, "agent/web-1-task", "main", "n1"); err != nil {
		t.Fatal(err)
	}
	refs := agentTrash(t, dir)
	if len(refs) != 1 || strings.HasSuffix(refs[0], "-wip") {
		t.Fatalf("trash refs = %v", refs)
	}
	if got := run(t, dir, "git", "rev-parse", refs[0]); got != unpushed {
		t.Fatalf("trash ref = %s, want %s", got, unpushed)
	}
	if head, main := run(t, dir, "git", "rev-parse", "HEAD"), run(t, ".", "git", "--git-dir="+bare, "rev-parse", "main"); head != main {
		t.Fatalf("HEAD = %s, want the job target %s", head, main)
	}
}

func TestAgentWorkspaceKeepsDirtyTree(t *testing.T) {
	bare, dir := agentWorkspaceFixture(t)
	head := run(t, dir, "git", "rev-parse", "HEAD")
	writeTestFile(t, filepath.Join(dir, "README.md"), "edited\n")
	writeTestFile(t, filepath.Join(dir, "staged.txt"), "staged\n")
	run(t, dir, "git", "add", "staged.txt")
	writeTestFile(t, filepath.Join(dir, "untracked.txt"), "untracked\n")
	if err := prepareAgentWorkspace(context.Background(), bare, dir, "agent/web-1-task", "main", "n1"); err != nil {
		t.Fatal(err)
	}
	refs := agentTrash(t, dir)
	if len(refs) != 1 || !strings.HasSuffix(refs[0], "-wip") {
		t.Fatalf("trash refs = %v", refs)
	}
	for path, want := range map[string]string{"README.md": "edited", "staged.txt": "staged", "untracked.txt": "untracked"} {
		if got := run(t, dir, "git", "show", refs[0]+":"+path); got != want {
			t.Fatalf("%s in snapshot = %q, want %q", path, got, want)
		}
	}
	if parent := run(t, dir, "git", "rev-parse", refs[0]+"^"); parent != head {
		t.Fatalf("snapshot parent = %s, want %s", parent, head)
	}
	if status := run(t, dir, "git", "status", "--porcelain"); status != "" {
		t.Fatalf("workspace not reset: %s", status)
	}
	if stash := run(t, dir, "git", "stash", "list"); stash != "" {
		t.Fatalf("stash was used: %s", stash)
	}
}

func TestAgentWorkspaceMovesBrokenCloneAside(t *testing.T) {
	_, bare := makeBareRepo(t)
	dir := filepath.Join(t.TempDir(), "work", agentSessionDirectory("session-a"))
	if err := os.MkdirAll(filepath.Join(dir, ".git"), 0700); err != nil {
		t.Fatal(err)
	}
	writeTestFile(t, filepath.Join(dir, "notes.txt"), "keep me\n")
	if err := prepareAgentWorkspace(context.Background(), bare, dir, "agent/web-1-task", "main", "n1"); err != nil {
		t.Fatal(err)
	}
	moved, _ := filepath.Glob(dir + ".replaced-*")
	if len(moved) != 1 {
		t.Fatalf("moved aside = %v", moved)
	}
	if got := readTestFile(t, filepath.Join(moved[0], "notes.txt")); got != "keep me\n" {
		t.Fatalf("moved data = %q", got)
	}
	if got := readTestFile(t, filepath.Join(dir, "README.md")); got != "one\n" {
		t.Fatalf("fresh clone README = %q", got)
	}
}

func TestAgentWorkspaceSaveFailureChangesNothing(t *testing.T) {
	bare, dir := agentWorkspaceFixture(t)
	writeTestFile(t, filepath.Join(dir, "work.txt"), "agent work\n")
	run(t, dir, "git", "add", "work.txt")
	run(t, dir, "git", "commit", "-q", "-m", "unpushed")
	unpushed := run(t, dir, "git", "rev-parse", "HEAD")
	writeTestFile(t, filepath.Join(dir, "README.md"), "dirty\n")
	blocker := filepath.Join(dir, ".git", "refs", "forkmesh", "trash", "agent", filepath.Base(dir))
	if err := os.MkdirAll(filepath.Dir(blocker), 0700); err != nil {
		t.Fatal(err)
	}
	writeTestFile(t, blocker, unpushed+"\n")
	err := prepareAgentWorkspace(context.Background(), bare, dir, "agent/web-1-task", "main", "n1")
	if err == nil || !strings.Contains(err.Error(), "could not save the unpushed work") {
		t.Fatalf("err = %v", err)
	}
	if head := run(t, dir, "git", "rev-parse", "HEAD"); head != unpushed {
		t.Fatalf("HEAD moved to %s", head)
	}
	if got := readTestFile(t, filepath.Join(dir, "README.md")); got != "dirty\n" {
		t.Fatalf("dirty edit lost: %q", got)
	}
	if got := readTestFile(t, filepath.Join(dir, "work.txt")); got != "agent work\n" {
		t.Fatalf("committed file lost: %q", got)
	}
}

func TestAgentWorkspaceTrashKeepsNewestTwenty(t *testing.T) {
	bare, dir := agentWorkspaceFixture(t)
	head := run(t, dir, "git", "rev-parse", "HEAD")
	prefix := agentTrashPrefix(dir)
	for index := 0; index < 25; index++ {
		run(t, dir, "git", "update-ref", fmt.Sprintf("%s%d", prefix, 1000000+index), head)
	}
	writeTestFile(t, filepath.Join(dir, "work.txt"), "agent work\n")
	if err := prepareAgentWorkspace(context.Background(), bare, dir, "agent/web-1-task", "main", "n1"); err != nil {
		t.Fatal(err)
	}
	refs := agentTrash(t, dir)
	if len(refs) != agentTrashKeep {
		t.Fatalf("kept %d trash refs: %v", len(refs), refs)
	}
	kept := map[string]bool{}
	wip := false
	for _, ref := range refs {
		kept[ref] = true
		wip = wip || strings.HasSuffix(ref, "-wip")
	}
	if !wip || kept[prefix+"1000005"] || !kept[prefix+"1000006"] || !kept[prefix+"1000024"] {
		t.Fatalf("wrong refs kept: %v", refs)
	}
}
