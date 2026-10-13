package mirrornode

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strings"
	"testing"
	"time"
)

func run(t *testing.T, dir string, args ...string) string {
	t.Helper()
	cmd := exec.Command(args[0], args[1:]...)
	cmd.Dir = dir
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("%v: %v: %s", args, err, out)
	}
	return strings.TrimSpace(string(out))
}

func makeBareRepo(t *testing.T) (string, string) {
	t.Helper()
	root := t.TempDir()
	source := filepath.Join(root, "source")
	bare := filepath.Join(root, "mirror.git")
	run(t, root, "git", "init", "-b", "main", source)
	run(t, source, "git", "config", "user.email", "test@example.test")
	run(t, source, "git", "config", "user.name", "Test")
	if err := os.WriteFile(filepath.Join(source, "README.md"), []byte("one\n"), 0600); err != nil {
		t.Fatal(err)
	}
	run(t, source, "git", "add", "README.md")
	run(t, source, "git", "commit", "-m", "one")
	run(t, root, "git", "clone", "--bare", source, bare)
	return source, bare
}

func TestRefsSHA256MatchesGatewayCanonicalAndIgnoresToolRefs(t *testing.T) {
	_, bare := makeBareRepo(t)
	run(t, ".", "git", "--git-dir="+bare, "branch", "z")
	run(t, ".", "git", "--git-dir="+bare, "tag", "v1")
	run(t, ".", "git", "--git-dir="+bare, "update-ref", "refs/codex/tmp", "HEAD")
	digest, err := refsSHA256(context.Background(), bare)
	if err != nil {
		t.Fatal(err)
	}
	lines := []string{run(t, ".", "git", "--git-dir="+bare, "rev-parse", "refs/heads/main") + " refs/heads/main", run(t, ".", "git", "--git-dir="+bare, "rev-parse", "refs/heads/z") + " refs/heads/z", run(t, ".", "git", "--git-dir="+bare, "rev-parse", "refs/tags/v1") + " refs/tags/v1"}
	want := sha256.Sum256([]byte(strings.Join(lines, "\n")))
	if digest != hex.EncodeToString(want[:]) {
		t.Fatalf("got %s want %x", digest, want)
	}
}

func TestSyncRepositoryFetchesStableRefsAndReportsFailure(t *testing.T) {
	source, bare := makeBareRepo(t)
	if err := os.WriteFile(filepath.Join(source, "README.md"), []byte("two\n"), 0600); err != nil {
		t.Fatal(err)
	}
	run(t, source, "git", "add", "README.md")
	run(t, source, "git", "commit", "-m", "two")
	repo := Repository{GitDir: bare}
	if err := syncRepository(context.Background(), repo, []string{"/missing", source}, ""); err != nil {
		t.Fatal(err)
	}
	if got, want := run(t, ".", "git", "--git-dir="+bare, "rev-parse", "main"), run(t, source, "git", "rev-parse", "main"); got != want {
		t.Fatalf("got %s want %s", got, want)
	}
	if err := syncRepository(context.Background(), repo, []string{"/missing"}, ""); err == nil {
		t.Fatal("expected failure")
	}
}

func TestSyncRepositoryBootstrapsBesidePreexistingReleaseCAS(t *testing.T) {
	// A stock VPS can carry an init.defaultBranch the upstream does not:
	// forcing master here proves the seeding `git init --bare` overrides it
	// with main. A mirror born on master serves refs, but every catalog
	// publication fails on an unborn HEAD and the node stays invisible to the
	// relay until ensureHeadResolves repairs it.
	t.Setenv("GIT_CONFIG_COUNT", "1")
	t.Setenv("GIT_CONFIG_KEY_0", "init.defaultBranch")
	t.Setenv("GIT_CONFIG_VALUE_0", "master")
	source, _ := makeBareRepo(t)
	root := t.TempDir()
	destination := filepath.Join(root, "mirror.git")
	releaseBlob := filepath.Join(destination, "forkmesh-releases", "sha256", "aa", "blob", "data")
	if err := os.MkdirAll(filepath.Dir(releaseBlob), 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(releaseBlob, []byte("release bytes"), 0600); err != nil {
		t.Fatal(err)
	}

	if err := syncRepository(context.Background(), Repository{GitDir: destination}, []string{source}, "mirror26"); err != nil {
		t.Fatal(err)
	}
	if got, want := run(t, ".", "git", "--git-dir="+destination, "rev-parse", "main"), run(t, source, "git", "rev-parse", "main"); got != want {
		t.Fatalf("got %s want %s", got, want)
	}
	if got := run(t, ".", "git", "--git-dir="+destination, "symbolic-ref", "HEAD"); got != "refs/heads/main" {
		t.Fatalf("HEAD is %s, want refs/heads/main", got)
	}
	if got, want := run(t, ".", "git", "--git-dir="+destination, "rev-parse", "HEAD"), run(t, source, "git", "rev-parse", "main"); got != want {
		t.Fatalf("HEAD resolves to %s want %s", got, want)
	}
	if raw, err := os.ReadFile(releaseBlob); err != nil || string(raw) != "release bytes" {
		t.Fatalf("release CAS was not preserved: %q %v", raw, err)
	}
}

func TestEnsureHeadResolvesPrefersMainThenFirstHeadAndKeepsHealthyHEAD(t *testing.T) {
	_, bare := makeBareRepo(t)
	headCommit := run(t, ".", "git", "--git-dir="+bare, "rev-parse", "main")
	run(t, ".", "git", "--git-dir="+bare, "branch", "aardvark")

	// A healthy HEAD is left alone.
	ensureHeadResolves(context.Background(), bare)
	if got := run(t, ".", "git", "--git-dir="+bare, "symbolic-ref", "HEAD"); got != "refs/heads/main" {
		t.Fatalf("healthy HEAD moved to %s", got)
	}

	// An unborn HEAD lands on main when the upstream carries one.
	run(t, ".", "git", "--git-dir="+bare, "symbolic-ref", "HEAD", "refs/heads/unborn")
	ensureHeadResolves(context.Background(), bare)
	if got := run(t, ".", "git", "--git-dir="+bare, "symbolic-ref", "HEAD"); got != "refs/heads/main" {
		t.Fatalf("unborn HEAD repaired to %s, want refs/heads/main", got)
	}
	if got := run(t, ".", "git", "--git-dir="+bare, "rev-parse", "HEAD"); got != headCommit {
		t.Fatalf("repaired HEAD resolves to %s want %s", got, headCommit)
	}

	// Without a main branch the first head wins so HEAD still resolves.
	run(t, ".", "git", "--git-dir="+bare, "symbolic-ref", "HEAD", "refs/heads/unborn")
	run(t, ".", "git", "--git-dir="+bare, "branch", "-D", "main")
	ensureHeadResolves(context.Background(), bare)
	if got := run(t, ".", "git", "--git-dir="+bare, "symbolic-ref", "HEAD"); got != "refs/heads/aardvark" {
		t.Fatalf("fallback HEAD is %s, want refs/heads/aardvark", got)
	}
}

func TestSyncRepositoryRejectsUnexpectedNonRepositoryContents(t *testing.T) {
	source, _ := makeBareRepo(t)
	destination := filepath.Join(t.TempDir(), "mirror.git")
	if err := os.MkdirAll(destination, 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(destination, "operator-data"), []byte("keep"), 0600); err != nil {
		t.Fatal(err)
	}
	err := syncRepository(context.Background(), Repository{GitDir: destination}, []string{source}, "mirror26")
	if err == nil || !strings.Contains(err.Error(), "unexpected entry") {
		t.Fatalf("unexpected error: %v", err)
	}
}

func TestGitRoutingArgsExcludeOnlyAValidMirrorNode(t *testing.T) {
	got := gitRoutingArgs("Mirror25")
	want := []string{"-c", "http.extraHeader=X-ForkMesh-Avoid-Node: mirror25"}
	if strings.Join(got, "\n") != strings.Join(want, "\n") {
		t.Fatalf("got %#v want %#v", got, want)
	}
	if got := gitRoutingArgs("mirror25\nInjected: value"); len(got) != 0 {
		t.Fatalf("accepted unsafe routing header: %#v", got)
	}
}

func TestCloneRejectsCommandExecutingGitTransport(t *testing.T) {
	root := t.TempDir()
	marker := filepath.Join(root, "transport-executed")
	err := cloneMirrorRepository(
		context.Background(), "ext::touch "+marker,
		filepath.Join(root, "mirror.git"), "mirror9")
	if err == nil {
		t.Fatal("command transport unexpectedly cloned")
	}
	if _, statErr := os.Stat(marker); !errors.Is(statErr, os.ErrNotExist) {
		t.Fatalf("ext transport executed a local command: %v", statErr)
	}
}

func TestPublicRepositorySyncIsReadableThroughSSHMount(t *testing.T) {
	source, _ := makeBareRepo(t)
	destination := filepath.Join(t.TempDir(), "public.git")
	if err := syncRepository(context.Background(), Repository{
		GitDir: destination, Visibility: "public",
	}, []string{source}, ""); err != nil {
		t.Fatal(err)
	}
	root, err := os.Stat(destination)
	if err != nil {
		t.Fatal(err)
	}
	if root.Mode().Perm()&0005 != 0005 {
		t.Fatalf("public repository root is not traversable: %o", root.Mode().Perm())
	}
	for _, path := range []string{
		filepath.Join(destination, "HEAD"),
		filepath.Join(destination, "packed-refs"),
	} {
		info, err := os.Stat(path)
		if err != nil {
			t.Fatal(err)
		}
		if info.Mode().Perm()&0004 == 0 {
			t.Fatalf("%s is not readable through the public SSH mount: %o",
				path, info.Mode().Perm())
		}
	}
}

func TestSyncRepositoryDoesNotRegressToAStaleRoundRobinPeer(t *testing.T) {
	source, bare := makeBareRepo(t)
	initial := run(t, source, "git", "rev-parse", "main")
	run(t, ".", "git", "--git-dir="+bare, "tag", "local-release", initial)
	newer := filepath.Join(t.TempDir(), "newer")
	run(t, ".", "git", "clone", bare, newer)
	run(t, newer, "git", "config", "user.email", "test@example.test")
	run(t, newer, "git", "config", "user.name", "Test")
	if err := os.WriteFile(filepath.Join(newer, "new.txt"), []byte("newer\n"), 0600); err != nil {
		t.Fatal(err)
	}
	run(t, newer, "git", "add", "new.txt")
	run(t, newer, "git", "commit", "-m", "newer local state")
	run(t, newer, "git", "push", "origin", "main")
	newTip := run(t, newer, "git", "rev-parse", "main")
	if err := syncRepository(context.Background(), Repository{GitDir: bare}, []string{source}, ""); err != nil {
		t.Fatal(err)
	}
	if got := run(t, ".", "git", "--git-dir="+bare, "rev-parse", "main"); got != newTip {
		t.Fatalf("stale peer rewound main to %s", got)
	}
	if got := run(t, ".", "git", "--git-dir="+bare, "rev-parse", "refs/tags/local-release"); got != initial {
		t.Fatalf("stale peer removed release tag: %s", got)
	}
	if refs := run(t, ".", "git", "--git-dir="+bare, "for-each-ref", "--format=%(refname)", "refs/forkmesh/upstream"); refs != "" {
		t.Fatalf("staging refs leaked: %s", refs)
	}
}

func TestReconcileUpstreamRefsRetriesDirectRefUpdateRace(t *testing.T) {
	source, bare := makeBareRepo(t)
	if err := os.WriteFile(filepath.Join(source, "README.md"), []byte("upstream\n"), 0600); err != nil {
		t.Fatal(err)
	}
	run(t, source, "git", "add", "README.md")
	run(t, source, "git", "commit", "-m", "upstream")
	incoming := run(t, source, "git", "rev-parse", "main")
	run(t, ".", "git", "--git-dir="+bare, "fetch", source,
		"refs/heads/main:refs/forkmesh/upstream/heads/main")

	if err := os.WriteFile(filepath.Join(source, "README.md"), []byte("direct\n"), 0600); err != nil {
		t.Fatal(err)
	}
	run(t, source, "git", "add", "README.md")
	run(t, source, "git", "commit", "-m", "direct")
	direct := run(t, source, "git", "rev-parse", "main")
	run(t, ".", "git", "--git-dir="+bare, "fetch", source,
		"refs/heads/main:refs/forkmesh/direct-race")

	attempts := 0
	err := reconcileUpstreamRefsWithUpdater(context.Background(), bare,
		func(ctx context.Context, gitDir, ref, newValue, oldValue string) ([]byte, error) {
			attempts++
			if attempts == 1 {
				run(t, ".", "git", "--git-dir="+gitDir, "update-ref", ref, direct)
			}
			return updateRef(ctx, gitDir, ref, newValue, oldValue)
		})
	if err != nil {
		t.Fatal(err)
	}
	if attempts != 1 {
		t.Fatalf("update-ref ran %d times, want one conflicted attempt", attempts)
	}
	if got := run(t, ".", "git", "--git-dir="+bare, "rev-parse", "main"); got != direct {
		t.Fatalf("direct receive was overwritten: got %s want %s (fetched %s)", got, direct, incoming)
	}
}

func TestUpdateRefDoesNotOverwriteConcurrentBranchCreation(t *testing.T) {
	source, bare := makeBareRepo(t)
	base := run(t, source, "git", "rev-parse", "main")
	if err := os.WriteFile(filepath.Join(source, "README.md"), []byte("incoming\n"), 0600); err != nil {
		t.Fatal(err)
	}
	run(t, source, "git", "add", "README.md")
	run(t, source, "git", "commit", "-m", "incoming")
	incoming := run(t, source, "git", "rev-parse", "main")
	run(t, ".", "git", "--git-dir="+bare, "fetch", source,
		"refs/heads/main:refs/forkmesh/incoming")
	run(t, ".", "git", "--git-dir="+bare, "update-ref", "refs/heads/race", base)

	output, err := updateRef(context.Background(), bare, "refs/heads/race", incoming, "")
	if err == nil || !isRefUpdateContention(output) {
		t.Fatalf("concurrent branch creation was not rejected: %v: %s", err, output)
	}
	if got := run(t, ".", "git", "--git-dir="+bare, "rev-parse", "refs/heads/race"); got != base {
		t.Fatalf("concurrent branch was overwritten: got %s want %s", got, base)
	}
}

func TestWriteAndLoadGatewayConfigAreAtomicAndStrict(t *testing.T) {
	_, bare := makeBareRepo(t)
	identity, _ := testIdentity(t)
	path := filepath.Join(t.TempDir(), "gateway.json")
	var cfg GatewayConfig
	cfg.SchemaVersion = 1
	cfg.Node.Name = "mirror9"
	cfg.Node.PublicKey = identity.PublicKey()
	cfg.RouterPublicKey = identity.PublicKey()
	cfg.Repositories = []Repository{{Owner: "mirror9", Name: "forkmesh", ForkedFrom: "alice/forkmesh", Visibility: "public", Enabled: true, GitDir: bare}}
	changed, err := writeGatewayConfig(path, cfg)
	if err != nil || !changed {
		t.Fatalf("%v %v", changed, err)
	}
	changed, err = writeGatewayConfig(path, cfg)
	if err != nil || changed {
		t.Fatalf("identical write changed: %v %v", changed, err)
	}
	loaded, _, err := loadGatewayConfig(path)
	if err != nil || loaded.Node.Name != "mirror9" ||
		loaded.Repositories[0].ForkedFrom != "alice/forkmesh" {
		t.Fatalf("%+v %v", loaded, err)
	}
	legacyStore := filepath.Join(bare, "forkmesh-releases")
	if err := os.Mkdir(legacyStore, 0700); err != nil {
		t.Fatal(err)
	}
	loaded, _, err = loadGatewayConfig(path)
	if err != nil || loaded.Repositories[0].ReleaseStore != legacyStore {
		t.Fatalf("legacy release store was not recovered: %+v %v", loaded, err)
	}
	raw, _ := json.Marshal(map[string]any{"schemaVersion": 1, "node": map[string]string{"name": "mirror9", "publicKey": identity.PublicKey()}, "repositories": []any{}, "unknown": true})
	if err := os.WriteFile(path, raw, 0600); err != nil {
		t.Fatal(err)
	}
	if _, _, err := loadGatewayConfig(path); err == nil {
		t.Fatal("unknown field accepted")
	}
}

func TestGatewayConfigPreservesDashboardAndForkRegistryPassthrough(t *testing.T) {
	// The Python gateway owns the dashboard/dynamicRepositoriesPath schema; the
	// daemon must round-trip an operator-authored gateway config through its
	// load/patch/rewrite cycle without dropping or rejecting these fields.
	_, bare := makeBareRepo(t)
	identity, _ := testIdentity(t)
	path := filepath.Join(t.TempDir(), "gateway.json")
	source := map[string]any{
		"schemaVersion":           1,
		"node":                    map[string]string{"name": "mirror33", "publicKey": identity.PublicKey()},
		"routerPublicKey":         identity.PublicKey(),
		"repositories":            []any{map[string]any{"owner": "forkmesh", "name": "forkmesh", "visibility": "public", "enabled": true, "gitDir": bare}},
		"dynamicRepositoriesPath": "/var/lib/forkmesh/dynamic-repositories.json",
		"dashboard": map[string]any{
			"enabled":          true,
			"assetsRepository": "forkmesh/forkmesh",
			"sshOrigin":        "ssh://git@ssh.forkmesh.com",
		},
	}
	raw, _ := json.MarshalIndent(source, "", "  ")
	if err := os.WriteFile(path, raw, 0600); err != nil {
		t.Fatal(err)
	}
	loaded, _, err := loadGatewayConfig(path)
	if err != nil {
		t.Fatalf("load rejected passthrough fields: %v", err)
	}
	if loaded.DynamicRepositoriesPath != "/var/lib/forkmesh/dynamic-repositories.json" {
		t.Fatalf("dynamic registry path dropped: %q", loaded.DynamicRepositoriesPath)
	}
	if len(loaded.Dashboard) == 0 {
		t.Fatal("dashboard block dropped on load")
	}
	// Simulate the daemon's patch and rewrite.
	loaded.HealthSignerCommand = []string{"/bin/true", "--sign-mirror-health"}
	if _, err := writeGatewayConfig(path, loaded); err != nil {
		t.Fatalf("rewrite failed: %v", err)
	}
	roundTripped := map[string]any{}
	rewritten, _ := os.ReadFile(path)
	if err := json.Unmarshal(rewritten, &roundTripped); err != nil {
		t.Fatal(err)
	}
	dashboard, ok := roundTripped["dashboard"].(map[string]any)
	if !ok || dashboard["assetsRepository"] != "forkmesh/forkmesh" {
		t.Fatalf("dashboard not preserved through rewrite: %v", roundTripped["dashboard"])
	}
	if roundTripped["dynamicRepositoriesPath"] != "/var/lib/forkmesh/dynamic-repositories.json" {
		t.Fatalf("dynamic registry path not preserved: %v", roundTripped["dynamicRepositoriesPath"])
	}
}

func TestCatalogPathCounts(t *testing.T) {
	i, m, p, d, a := catalogPathCounts([]string{".forkmesh/issues/open/9/issue-9.json", ".forkmesh/issues/closed/11/issue-11.json", ".forkmesh/pulls/open/2.json", ".forkmesh/discussions/1/discussion.json", ".forkmesh/artifacts/a"})
	if i != 1 || m != 11 || p != 1 || d != 1 || a != 1 {
		t.Fatalf("%d %d %d %d %d", i, m, p, d, a)
	}
}

func TestGitRoutingArgsJoinAvoidedNodes(t *testing.T) {
	got := gitRoutingArgs("n3", "N2", "n3", "bad\nnode", "n1", "n4", "n5")
	want := []string{"-c", "http.extraHeader=X-ForkMesh-Avoid-Node: n3,n2,n1,n4"}
	if strings.Join(got, "\n") != strings.Join(want, "\n") {
		t.Fatalf("got %#v want %#v", got, want)
	}
}

func TestRedirectedPeerReadsTheLastRedirect(t *testing.T) {
	output := []byte("warning: redirecting to https://n1.next.forkmesh.com/forkmesh/forkmesh.git/\n" +
		"warning: redirecting to https://n2.next.forkmesh.com/forkmesh/forkmesh.git/\nfatal: expected 'acknowledgments'\n")
	if got := redirectedPeer(output); got != "n2" {
		t.Fatalf("got %q", got)
	}
	if got := redirectedPeer([]byte("fatal: unable to access")); got != "" {
		t.Fatalf("got %q", got)
	}
}

func TestDropBrokenRefsRemovesEmptyMissingAndStaleLockRefs(t *testing.T) {
	_, bare := makeBareRepo(t)
	main := run(t, ".", "git", "--git-dir="+bare, "rev-parse", "refs/heads/main")
	run(t, ".", "git", "--git-dir="+bare, "update-ref", "refs/heads/good", main)
	heads := filepath.Join(bare, "refs", "heads")
	upstream := filepath.Join(bare, "refs", "forkmesh", "upstream", "heads")
	if err := os.MkdirAll(upstream, 0700); err != nil {
		t.Fatal(err)
	}
	write := func(path, content string) {
		if err := os.WriteFile(path, []byte(content), 0600); err != nil {
			t.Fatal(err)
		}
	}
	write(filepath.Join(heads, "empty"), "")
	write(filepath.Join(heads, "missing"), strings.Repeat("ab", 20)+"\n")
	write(filepath.Join(upstream, "empty"), "")
	staleLock := filepath.Join(heads, "stuck.lock")
	write(staleLock, "")
	old := time.Now().Add(-time.Hour)
	if err := os.Chtimes(staleLock, old, old); err != nil {
		t.Fatal(err)
	}
	freshLock := filepath.Join(heads, "busy.lock")
	write(freshLock, "")
	dropped := dropBrokenRefs(context.Background(), bare)
	sort.Strings(dropped)
	want := []string{"refs/forkmesh/upstream/heads/empty", "refs/heads/empty", "refs/heads/missing", "refs/heads/stuck.lock"}
	if strings.Join(dropped, ",") != strings.Join(want, ",") {
		t.Fatalf("dropped %v want %v", dropped, want)
	}
	if _, err := os.Stat(freshLock); err != nil {
		t.Fatalf("a fresh lock was removed: %v", err)
	}
	if got := run(t, ".", "git", "--git-dir="+bare, "rev-parse", "refs/heads/good"); got != main {
		t.Fatalf("a healthy ref moved: %s", got)
	}
	run(t, ".", "git", "--git-dir="+bare, "fsck", "--connectivity-only")
}

func TestSyncRepositoryHealsABrokenLocalRefBeforeFetching(t *testing.T) {
	source, bare := makeBareRepo(t)
	run(t, source, "git", "branch", "feature")
	if err := os.WriteFile(filepath.Join(bare, "refs", "heads", "feature"), nil, 0600); err != nil {
		t.Fatal(err)
	}
	if err := syncRepository(context.Background(), Repository{GitDir: bare}, []string{source}, ""); err != nil {
		t.Fatal(err)
	}
	if got, want := run(t, ".", "git", "--git-dir="+bare, "rev-parse", "refs/heads/feature"), run(t, source, "git", "rev-parse", "feature"); got != want {
		t.Fatalf("got %s want %s", got, want)
	}
}

func TestClearRefNamespaceDeletesEveryStagedRefAndNothingElse(t *testing.T) {
	_, bare := makeBareRepo(t)
	run(t, ".", "git", "--git-dir="+bare, "tag", "v1")
	var batch strings.Builder
	for i := 0; i < 300; i++ {
		batch.WriteString(fmt.Sprintf("create refs/forkmesh/upstream/heads/b%d HEAD\n", i))
	}
	cmd := exec.Command("git", "--git-dir="+bare, "update-ref", "--stdin")
	cmd.Stdin = strings.NewReader(strings.ReplaceAll(batch.String(), "HEAD",
		run(t, ".", "git", "--git-dir="+bare, "rev-parse", "HEAD")))
	if out, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("seed refs: %v: %s", err, out)
	}
	clearRefNamespace(context.Background(), bare, "refs/forkmesh/upstream")
	if left := run(t, ".", "git", "--git-dir="+bare, "for-each-ref",
		"--format=%(refname)", "refs/forkmesh/upstream"); left != "" {
		t.Fatalf("staged refs left behind: %q", left)
	}
	values, err := gitRefValues(context.Background(), bare, "refs/heads", "refs/tags")
	if err != nil {
		t.Fatal(err)
	}
	if len(values) != 2 || values["refs/heads/main"] == "" || values["refs/tags/v1"] == "" {
		t.Fatalf("local refs changed: %v", values)
	}
}
