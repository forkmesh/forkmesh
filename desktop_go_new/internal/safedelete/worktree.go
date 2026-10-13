package safedelete

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

type GitRun func(dir string, args []string) (bool, string)

func LinkedWorktreeRefusal(repo, worktree string) string {
	if strings.TrimSpace(worktree) == "" {
		return "the worktree path is empty"
	}
	if !filepath.IsAbs(worktree) {
		return fmt.Sprintf("%s is not an absolute path", worktree)
	}
	path := safeDeleteClean(worktree)
	if path == string(filepath.Separator) {
		return "the path is /"
	}
	for _, home := range safeDeleteHomes() {
		if safeDeleteWithin(home, path) {
			return fmt.Sprintf("%s is HOME (%s) or contains it", path, home)
		}
	}
	if strings.TrimSpace(repo) == "" {
		return "no repository was given"
	}
	if safeDeleteWithin(safeDeleteClean(repo), path) {
		return fmt.Sprintf("%s is or contains the repository %s", path, repo)
	}
	if info, err := os.Lstat(filepath.Join(path, ".git")); err == nil && info.IsDir() {
		return fmt.Sprintf("%s is a git checkout (it has a .git folder)", path)
	}
	for _, root := range safeDeleteProtectedRoots() {
		if safeDeleteWithin(root.path, path) {
			return fmt.Sprintf("%s is or contains protected ForkMesh folder %s", path, root.path)
		}
	}
	return ""
}

func ForceRemoveWorktree(run GitRun, repo, worktree string) (bool, string, error) {
	if reason := LinkedWorktreeRefusal(repo, worktree); reason != "" {
		return false, "", fmt.Errorf("refusing to remove the worktree: %s", reason)
	}
	ok, errText := run(repo, []string{"worktree", "remove", "--force", worktree})
	return ok, errText, nil
}
