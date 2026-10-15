package safedelete

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func sandboxEnv(t *testing.T) string {
	t.Helper()
	home := t.TempDir()
	t.Setenv("HOME", home)
	for _, name := range []string{"FORKMESH_HOME", "FORKMESH_PROJECTS_DIR", "FORKMESH_PROJECT_DIR", "FORKMESH_DATA_DIR", "FORKMESH_CONFIG_DIR"} {
		t.Setenv(name, "")
	}
	return home
}

func TestRemovesTreeInsideParent(t *testing.T) {
	base := t.TempDir()
	sandboxEnv(t)
	target := filepath.Join(base, "forkmesh-run-1-1")
	if err := os.MkdirAll(filepath.Join(target, "a", "b"), 0o700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(target, "a", "b", "f"), []byte("x"), 0o600); err != nil {
		t.Fatal(err)
	}
	if err := RemoveTreeUnder(base, target); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(target); !os.IsNotExist(err) {
		t.Fatalf("still there: %v", err)
	}
}

func TestRefusesOutsideParentAndCheckouts(t *testing.T) {
	base := t.TempDir()
	home := sandboxEnv(t)
	other := t.TempDir()
	if err := RemoveTreeUnder(base, other); err == nil || !strings.Contains(err.Error(), "not inside") {
		t.Fatalf("outside parent: %v", err)
	}
	checkout := filepath.Join(base, "repo")
	if err := os.MkdirAll(filepath.Join(checkout, ".git"), 0o700); err != nil {
		t.Fatal(err)
	}
	if err := RemoveTreeUnder(base, checkout); err == nil {
		t.Fatal("removed a checkout")
	}
	worktree := filepath.Join(base, "wt")
	if err := os.MkdirAll(worktree, 0o700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(worktree, ".git"), []byte("gitdir: x"), 0o600); err != nil {
		t.Fatal(err)
	}
	if Refusal(base, worktree) == "" {
		t.Fatal("plain delete accepted a worktree")
	}
	if WorktreeRefusal(base, worktree) != "" {
		t.Fatal("own worktree refused")
	}
	if err := RemoveTreeUnder(filepath.Dir(home), home); err == nil {
		t.Fatal("removed HOME")
	}
	data := filepath.Join(home, "projects", "forkmesh", "data")
	if err := os.MkdirAll(filepath.Join(data, "actions", "sandbox", "forkmesh-run-2-2"), 0o700); err != nil {
		t.Fatal(err)
	}
	if Refusal(filepath.Join(data), filepath.Join(data, "identity")) == "" {
		t.Fatal("top-level data folder accepted")
	}
	if reason := Refusal(filepath.Join(data, "actions", "sandbox"), filepath.Join(data, "actions", "sandbox", "forkmesh-run-2-2")); reason != "" {
		t.Fatalf("sandbox tree refused: %s", reason)
	}
}
