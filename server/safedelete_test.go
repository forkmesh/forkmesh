package mirrornode

import (
	"os"
	"path/filepath"
	"testing"
)

func safeDeleteSandbox(t *testing.T) (string, string, string) {
	t.Helper()
	root, err := filepath.EvalSymlinks(t.TempDir())
	if err != nil {
		t.Fatal(err)
	}
	home := filepath.Join(root, "home")
	work := filepath.Join(root, "work")
	for _, dir := range []string{home, work} {
		if err := os.MkdirAll(dir, 0o700); err != nil {
			t.Fatal(err)
		}
	}
	t.Setenv("HOME", home)
	for _, variable := range []string{"FORKMESH_HOME", "FORKMESH_PROJECTS_DIR", "FORKMESH_DATA_DIR",
		"FORKMESH_CONFIG_DIR", "FORKMESH_PROJECT_DIR"} {
		t.Setenv(variable, "")
	}
	return root, home, work
}

func safeDeleteMkdir(t *testing.T, paths ...string) {
	t.Helper()
	for _, path := range paths {
		if err := os.MkdirAll(path, 0o700); err != nil {
			t.Fatal(err)
		}
	}
}

func TestSafeDeleteRefusals(t *testing.T) {
	root, home, work := safeDeleteSandbox(t)
	checkout := filepath.Join(work, "checkout")
	linked := filepath.Join(work, "linked")
	projects := filepath.Join(home, "projects")
	project := filepath.Join(projects, "forkmesh")
	safeDeleteMkdir(t, filepath.Join(checkout, ".git", "objects"), linked,
		filepath.Join(projects, "forkmesh-backup", "data"), filepath.Join(work, "scratch", "a"))
	if err := os.WriteFile(filepath.Join(linked, ".git"), []byte("gitdir: /x/.git/worktrees/linked\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	for _, sub := range safeDeleteProjectRoots {
		safeDeleteMkdir(t, filepath.Join(project, sub))
	}
	cases := []struct {
		name, parent, target string
		allow                deleteAllow
		refused              bool
	}{
		{"empty", work, "", deleteTree, true},
		{"blank", work, "   ", deleteTree, true},
		{"root", "/", "/", deleteTree, true},
		{"home", root, home, deleteTree, true},
		{"ancestor of home", filepath.Dir(root), root, deleteTree, true},
		{"no parent", "", filepath.Join(work, "scratch"), deleteTree, true},
		{"slash parent", "/", filepath.Join(work, "scratch"), deleteTree, true},
		{"outside parent", filepath.Join(work, "elsewhere"), filepath.Join(work, "scratch"), deleteTree, true},
		{"parent itself", work, work, deleteTree, true},
		{"dot-dot escape", work, filepath.Join(work, "scratch", "..", ".."), deleteTree, true},
		{"checkout", work, checkout, deleteTree, true},
		{"checkout as worktree", work, checkout, deleteOwnWorktree, true},
		{"own clone", work, checkout, deleteOwnClone, false},
		{"dot git", checkout, filepath.Join(checkout, ".git"), deleteOwnClone, true},
		{"git objects", filepath.Join(checkout, ".git"), filepath.Join(checkout, ".git", "objects"), deleteOwnClone, true},
		{"linked worktree", work, linked, deleteTree, true},
		{"own linked worktree", work, linked, deleteOwnWorktree, false},
		{"projects root", home, projects, deleteTree, true},
		{"home project", projects, project, deleteTree, true},
		{"src", project, filepath.Join(project, "src"), deleteTree, true},
		{"data", project, filepath.Join(project, "data"), deleteTree, true},
		{"config", project, filepath.Join(project, "config"), deleteTree, true},
		{"ssh", project, filepath.Join(project, "ssh"), deleteTree, true},
		{"state", project, filepath.Join(project, "state"), deleteTree, true},
		{"logs", project, filepath.Join(project, "logs"), deleteTree, true},
		{"inside src", filepath.Join(project, "src"), filepath.Join(project, "src", "desktop"), deleteTree, true},
		{"build in src", filepath.Join(project, "src", "desktop"), filepath.Join(project, "src", "desktop", "build"), deleteTree, false},
		{"top of data", filepath.Join(project, "data"), filepath.Join(project, "data", "identity"), deleteTree, true},
		{"nested data", filepath.Join(project, "data", "mirrors"), filepath.Join(project, "data", "mirrors", "repo"), deleteTree, false},
		{"ssh fleet", filepath.Join(project, "ssh"), filepath.Join(project, "ssh", "fleet"), deleteTree, true},
		{"shared builds", filepath.Join(project, "builds"), filepath.Join(project, "builds", "x"), deleteTree, false},
		{"issue metadata in src", filepath.Join(project, "src", ".forkmesh", "issues"), filepath.Join(project, "src", ".forkmesh", "issues", "7"), deleteTree, false},
		{"metadata root in src", filepath.Join(project, "src"), filepath.Join(project, "src", ".forkmesh"), deleteTree, true},
		{"linux toolchain", filepath.Join(project, "toolchain-linux"), filepath.Join(project, "toolchain-linux", "x"), deleteTree, false},
		{"unknown project folder", project, filepath.Join(project, "notes"), deleteTree, true},
		{"backup project", filepath.Join(projects, "forkmesh-backup", "data"), filepath.Join(projects, "forkmesh-backup", "data", "x"), deleteTree, true},
		{"legacy state", home, filepath.Join(home, ".forkmesh"), deleteTree, true},
		{"scratch", work, filepath.Join(work, "scratch"), deleteTree, false},
	}
	for _, c := range cases {
		reason := safeDeleteRefusal(c.parent, c.target, c.allow)
		if (reason != "") != c.refused {
			t.Errorf("%s: refused=%v reason=%q", c.name, reason != "", reason)
		}
	}
}

func TestSafeDeleteHonoursForkmeshHome(t *testing.T) {
	root, _, _ := safeDeleteSandbox(t)
	custom := filepath.Join(root, "custom")
	safeDeleteMkdir(t, filepath.Join(custom, "data", "x"))
	t.Setenv("FORKMESH_HOME", custom)
	if safeDeleteRefusal(custom, filepath.Join(custom, "data"), deleteTree) == "" {
		t.Fatal("FORKMESH_HOME data was not protected")
	}
	if safeDeleteRefusal(root, custom, deleteTree) == "" {
		t.Fatal("FORKMESH_HOME itself was not protected")
	}
}

func TestRemoveTreeUnderDeletesOnlyWhatIsAllowed(t *testing.T) {
	_, _, work := safeDeleteSandbox(t)
	checkout := filepath.Join(work, "checkout")
	scratch := filepath.Join(work, "scratch")
	safeDeleteMkdir(t, filepath.Join(checkout, ".git"), filepath.Join(scratch, "a", "b"))
	if err := removeTreeUnder(work, checkout); err == nil {
		t.Fatal("a checkout was removed")
	}
	if _, err := os.Stat(filepath.Join(checkout, ".git")); err != nil {
		t.Fatal("the refused checkout was touched")
	}
	if err := removeTreeUnder(work, scratch); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(scratch); !os.IsNotExist(err) {
		t.Fatal("scratch survived")
	}
	if err := removeTreeUnder(work, filepath.Join(work, "missing")); err != nil {
		t.Fatal(err)
	}
	if err := removeOwnCloneUnder(work, checkout); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(checkout); !os.IsNotExist(err) {
		t.Fatal("own clone survived")
	}
}

func TestSafeDeleteCleanResolvesTheDeepestExistingAncestor(t *testing.T) {
	real, err := filepath.EvalSymlinks(t.TempDir())
	if err != nil {
		t.Fatal(err)
	}
	link := filepath.Join(t.TempDir(), "link")
	if err := os.Symlink(real, link); err != nil {
		t.Fatal(err)
	}
	if got, want := safeDeleteClean(filepath.Join(link, "missing", "deeper", "leaf")),
		filepath.Join(real, "missing", "deeper", "leaf"); got != want {
		t.Fatalf("safeDeleteClean = %s want %s", got, want)
	}
	if got, want := safeDeleteClean(filepath.Join(link, "leaf")), filepath.Join(real, "leaf"); got != want {
		t.Fatalf("safeDeleteClean = %s want %s", got, want)
	}
	if err := agentWorkOutsideNodeKey(filepath.Join(link, "identity", "agent-jobs", "work"),
		filepath.Join(real, "identity", "ed25519.pem")); err == nil {
		t.Fatal("a work root under the key folder was accepted through a symlinked parent")
	}
}
