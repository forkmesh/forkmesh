package safedelete

import (
	"os"
	"path/filepath"
	"testing"
)

func TestForceRemoveWorktreeRefusals(t *testing.T) {
	_, home, work := paritySandbox(t)
	repo := filepath.Join(work, "repo")
	linked := filepath.Join(work, "linked")
	checkout := filepath.Join(work, "other")
	parityMkdir(t, filepath.Join(repo, ".git"), linked, filepath.Join(checkout, ".git"),
		filepath.Join(home, "projects", "forkmesh", "data"))
	if err := os.WriteFile(filepath.Join(linked, ".git"), []byte("gitdir: x\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	var calls [][]string
	run := func(dir string, args []string) (bool, string) {
		calls = append(calls, append([]string{dir}, args...))
		return true, ""
	}
	for _, target := range []string{"", "relative/x", "/", home, repo, checkout, work,
		filepath.Join(home, "projects", "forkmesh", "data")} {
		if _, _, err := ForceRemoveWorktree(run, repo, target); err == nil {
			t.Errorf("%q was not refused", target)
		}
	}
	if len(calls) != 0 {
		t.Fatalf("git ran for a refused path: %v", calls)
	}
	ok, _, err := ForceRemoveWorktree(run, repo, linked)
	if err != nil || !ok || len(calls) != 1 || calls[0][0] != repo || calls[0][4] != linked {
		t.Fatalf("linked worktree: ok=%v err=%v calls=%v", ok, err, calls)
	}
}
