package mirrornode

import (
	"context"
	"crypto/rand"
	"net/http/cgi"
	"net/http/httptest"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"syscall"
	"testing"
)

const (
	testForkID     = "hr0123456789abcdef0123f0a1"
	testForkForkID = "hr0123456789abcdef0123f0a2"
)

func runGit(t *testing.T, dir string, args ...string) string {
	t.Helper()
	command := exec.Command("git", args...)
	command.Dir = dir
	command.Env = append(os.Environ(), "GIT_CONFIG_GLOBAL="+os.DevNull, "GIT_CONFIG_NOSYSTEM=1",
		"GIT_AUTHOR_NAME=t", "GIT_AUTHOR_EMAIL=t@example.test",
		"GIT_COMMITTER_NAME=t", "GIT_COMMITTER_EMAIL=t@example.test")
	output, err := command.CombinedOutput()
	if err != nil {
		t.Fatalf("git %v: %v\n%s", args, err, output)
	}
	return strings.TrimSpace(string(output))
}

// commitAndPush adds one commit with `size` random bytes and pushes it.
func commitAndPush(t *testing.T, work, gitDir, ref string, size int) string {
	t.Helper()
	blob := make([]byte, size)
	_, _ = rand.Read(blob)
	name := filepath.Join(work, "data-"+strings.ReplaceAll(ref, "/", "-")+"-"+
		runGit(t, work, "rev-parse", "--short", "HEAD")+".bin")
	if err := os.WriteFile(name, blob, 0o600); err != nil {
		t.Fatal(err)
	}
	runGit(t, work, "add", ".")
	runGit(t, work, "commit", "-q", "-m", "change")
	runGit(t, work, "push", "-q", gitDir, "HEAD:"+ref)
	return runGit(t, work, "rev-parse", "HEAD")
}

func objectBytes(t *testing.T, gitDir string) int64 {
	t.Helper()
	var total int64
	_ = filepath.Walk(filepath.Join(gitDir, "objects"), func(_ string, info os.FileInfo, err error) error {
		if err == nil && info.Mode().IsRegular() {
			total += info.Size()
		}
		return nil
	})
	return total
}

// unlinkedBytes counts regular files with a single link: real extra disk.
func unlinkedBytes(t *testing.T, dir string) int64 {
	t.Helper()
	var total int64
	_ = filepath.Walk(dir, func(_ string, info os.FileInfo, err error) error {
		if err != nil || !info.Mode().IsRegular() {
			return nil
		}
		if stat, ok := info.Sys().(*syscall.Stat_t); ok && stat.Nlink > 1 {
			return nil
		}
		total += info.Size()
		return nil
	})
	return total
}

func treeBytes(t *testing.T, dir string) int64 {
	t.Helper()
	var total int64
	_ = filepath.Walk(dir, func(_ string, info os.FileInfo, err error) error {
		if err == nil && info.Mode().IsRegular() {
			total += info.Size()
		}
		return nil
	})
	return total
}

func TestHostedForkBorrowsFromTheNetworkPool(t *testing.T) {
	var app fakeHostedApp
	appServer := httptest.NewServer(&app)
	defer appServer.Close()
	node := hostedTestDaemon(t, "fmnext-n1", appServer.URL)
	node.hosted.cfg.MaxRepositoryBytes = 1 << 30
	desired := []hostedDesired{{
		ID: testHostedID, Owner: "alice", Name: "proj", Visibility: "public",
		Primary: true, Init: testInit()}}
	app.answer = func(string, map[string]hostedCopyState) hostedReplicaResponse {
		return hostedReplicaResponse{Repositories: append([]hostedDesired(nil), desired...)}
	}
	ctx := context.Background()
	if err := node.hosted.reconcile(ctx); err != nil {
		t.Fatal(err)
	}
	parent := node.hosted.gitDir(testHostedID)

	// Give the parent some real history (8 MiB in 4 commits).
	work := t.TempDir()
	runGit(t, work, "clone", "-q", parent, "w")
	work = filepath.Join(work, "w")
	for index := 0; index < 4; index++ {
		commitAndPush(t, work, parent, "refs/heads/main", 2<<20)
	}
	parentMain := runGit(t, work, "rev-parse", "HEAD")
	parentBytes := objectBytes(t, parent)

	desired = append(desired, hostedDesired{
		ID: testForkID, Owner: "bob", Name: "proj", Visibility: "public", Primary: true,
		ForkOf: testHostedID, Network: testHostedID})
	if err := node.hosted.reconcile(ctx); err != nil {
		t.Fatal(err)
	}
	fork := node.hosted.gitDir(testForkID)
	alternates, err := os.ReadFile(filepath.Join(fork, "objects", "info", "alternates"))
	pool := filepath.Join(node.hosted.cfg.Root, hostedPoolDirectory, testHostedID+".git")
	if err != nil || strings.TrimSpace(string(alternates)) != filepath.Join(pool, "objects") {
		t.Fatalf("alternates = %q, %v", alternates, err)
	}
	if got := gitRefValue(ctx, fork, "refs/heads/main"); got != parentMain {
		t.Fatalf("fork main = %q, want %q", got, parentMain)
	}
	if head := gitValue(ctx, fork, "symbolic-ref", "HEAD"); head != "refs/heads/main" {
		t.Fatalf("fork HEAD = %q", head)
	}
	if _, ok := hostedEntry(node, testForkID); !ok {
		t.Fatal("the fork is not served")
	}
	forkBytes := treeBytes(t, fork)
	poolOwn := unlinkedBytes(t, filepath.Join(pool, "objects"))
	t.Logf("parent objects %d bytes; fork repository %d bytes; pool objects %d bytes, "+
		"of which %d not hard-linked to the parent", parentBytes, forkBytes, objectBytes(t, pool), poolOwn)
	if poolOwn > 64<<10 {
		t.Fatalf("the pool copied the parent's objects instead of linking them: %d", poolOwn)
	}
	if forkBytes > 256<<10 || objectBytes(t, fork) > 16<<10 {
		t.Fatalf("the fork copied objects: %d bytes", forkBytes)
	}

	// The fork's own push stays in the fork, and a fork of the fork borrows
	// from the same pool.
	runGit(t, work, "switch", "-q", "-c", "topic")
	topic := commitAndPush(t, work, fork, "refs/heads/topic", 64<<10)
	desired = append(desired, hostedDesired{
		ID: testForkForkID, Owner: "carol", Name: "proj", Visibility: "public", Primary: true,
		ForkOf: testForkID, Network: testHostedID})
	if err := node.hosted.reconcile(ctx); err != nil {
		t.Fatal(err)
	}
	second := node.hosted.gitDir(testForkForkID)
	if got := gitRefValue(ctx, second, "refs/heads/topic"); got != topic {
		t.Fatalf("fork of fork topic = %q, want %q", got, topic)
	}
	if objectBytes(t, second) > 16<<10 {
		t.Fatalf("the fork of a fork copied objects: %d", objectBytes(t, second))
	}
	if pools, _ := filepath.Glob(filepath.Join(node.hosted.cfg.Root, hostedPoolDirectory, "hr*.git")); len(pools) != 1 {
		t.Fatalf("pools = %v", pools)
	}

	// Rewriting and pruning the parent never breaks a fork.
	runGit(t, work, "switch", "-q", "--orphan", "fresh")
	runGit(t, work, "commit", "-q", "--allow-empty", "-m", "rewritten")
	runGit(t, work, "push", "-q", "-f", parent, "HEAD:refs/heads/main")
	runGit(t, parent, "-c", "gc.reflogExpire=now", "-c", "gc.reflogExpireUnreachable=now",
		"gc", "-q", "--prune=now")
	for _, dir := range []string{fork, second} {
		runGit(t, dir, "fsck", "--connectivity-only", "--no-dangling")
	}
	// Hosted maintenance never walks the pool.
	for _, dir := range node.hosted.hostedGitDirs() {
		if strings.Contains(dir, hostedPoolDirectory) {
			t.Fatalf("maintenance would touch the pool: %s", dir)
		}
	}
	// Maintenance of a fork keeps borrowed objects out of it.
	if err := maintainHostedRepository(ctx, fork); err != nil && err != errHostedMaintenanceBusy {
		t.Fatal(err)
	}
	if objectBytes(t, fork) > 256<<10 {
		t.Fatalf("maintenance copied borrowed objects into the fork: %d", objectBytes(t, fork))
	}
}

func TestHostedForkWaitsForItsParent(t *testing.T) {
	var app fakeHostedApp
	appServer := httptest.NewServer(&app)
	defer appServer.Close()
	node := hostedTestDaemon(t, "fmnext-n1", appServer.URL)
	app.answer = func(string, map[string]hostedCopyState) hostedReplicaResponse {
		return hostedReplicaResponse{Repositories: []hostedDesired{{
			ID: testForkID, Owner: "bob", Name: "proj", Visibility: "public",
			ForkOf: testHostedID, Network: testHostedID}}}
	}
	if err := node.hosted.reconcile(context.Background()); err != nil {
		t.Fatal(err)
	}
	fork := node.hosted.gitDir(testForkID)
	if _, err := os.Stat(filepath.Join(fork, "objects", "info", "alternates")); !os.IsNotExist(err) {
		t.Fatal("a fork borrowed from a parent this core does not hold")
	}
}

func hostedForkParentNode(t *testing.T, visibility string, primary bool) (*fakeHostedApp, string, string) {
	t.Helper()
	app := &fakeHostedApp{}
	appServer := httptest.NewServer(app)
	t.Cleanup(appServer.Close)
	node := hostedTestDaemon(t, "fmnext-n1", appServer.URL)
	desired := []hostedDesired{{
		ID: testHostedID, Owner: "alice", Name: "proj", Visibility: visibility,
		Primary: true, Init: testInit()}}
	app.answer = func(string, map[string]hostedCopyState) hostedReplicaResponse {
		return hostedReplicaResponse{Repositories: append([]hostedDesired(nil), desired...)}
	}
	ctx := context.Background()
	if err := node.hosted.reconcile(ctx); err != nil {
		t.Fatal(err)
	}
	desired = append(desired, hostedDesired{
		ID: testForkID, Owner: "bob", Name: "proj", Visibility: "public", Primary: primary,
		ForkOf: testHostedID, Network: testHostedID})
	if err := node.hosted.reconcile(ctx); err != nil {
		t.Fatal(err)
	}
	return app, node.hosted.gitDir(testForkID), filepath.Join(node.hosted.cfg.Root, hostedPoolDirectory)
}

func TestHostedForkRefsComeFromThePrimaryOnly(t *testing.T) {
	_, fork, pools := hostedForkParentNode(t, "public", false)
	alternates, err := os.ReadFile(filepath.Join(fork, "objects", "info", "alternates"))
	if err != nil || strings.TrimSpace(string(alternates)) != filepath.Join(pools, testHostedID+".git", "objects") {
		t.Fatalf("alternates = %q, %v", alternates, err)
	}
	if refs := gitLines(context.Background(), fork, "for-each-ref", "--format=%(refname)"); len(refs) != 0 {
		t.Fatalf("a non-primary core copied its own parent tips: %v", refs)
	}
}

func TestHostedForkOfAPrivateParentIsNotPooled(t *testing.T) {
	_, fork, pools := hostedForkParentNode(t, "private", true)
	if _, err := os.Stat(pools); !os.IsNotExist(err) {
		t.Fatal("a private parent's objects joined a pool")
	}
	if _, err := os.Stat(filepath.Join(fork, "objects", "info", "alternates")); !os.IsNotExist(err) {
		t.Fatal("a fork borrowed from a private parent")
	}
}

func TestHostedForkEntriesAreValidated(t *testing.T) {
	base := hostedDesired{ID: testForkID, Owner: "bob", Name: "proj", Visibility: "public"}
	for _, item := range []hostedDesired{
		func() hostedDesired { item := base; item.ForkOf = testForkID; return item }(),
		func() hostedDesired { item := base; item.ForkOf = "../x"; return item }(),
		func() hostedDesired {
			item := base
			item.ForkOf = testHostedID
			item.Visibility = "private"
			return item
		}(),
		func() hostedDesired { item := base; item.Network = testHostedID; return item }(),
		func() hostedDesired { item := base; item.ForkOf = testHostedID; item.Network = "x"; return item }(),
	} {
		if validHostedDesired(item) {
			t.Fatalf("accepted %+v", item)
		}
	}
	item := base
	item.ForkOf, item.Network = testHostedID, testHostedID
	if !validHostedDesired(item) {
		t.Fatal("refused a valid fork")
	}
}

const testUpstreamNetwork = "hr0123456789abcdef0123f0b1"

func upstreamForkNode(t *testing.T) (*Daemon, *[]hostedDesired) {
	t.Helper()
	app := &fakeHostedApp{}
	appServer := httptest.NewServer(app)
	t.Cleanup(appServer.Close)
	node := hostedTestDaemon(t, "fmnext-n1", appServer.URL)
	node.hosted.cfg.MaxRepositoryBytes = 1 << 30
	desired := &[]hostedDesired{}
	app.answer = func(string, map[string]hostedCopyState) hostedReplicaResponse {
		return hostedReplicaResponse{Repositories: append([]hostedDesired(nil), (*desired)...)}
	}
	return node, desired
}

func upstreamFork(id, owner, name, cloneURL string, primary bool) hostedDesired {
	return hostedDesired{
		ID: id, Owner: "bob", Name: name, Visibility: "public", Primary: primary,
		ForkUpstream: &hostedForkUpstream{Owner: owner, Name: name, CloneURL: cloneURL},
		Network:      testUpstreamNetwork}
}

func gitHTTPServer(t *testing.T, root string) *httptest.Server {
	t.Helper()
	gitPath, err := exec.LookPath("git")
	if err != nil {
		t.Skip("git is not installed")
	}
	server := httptest.NewServer(&cgi.Handler{
		Path: gitPath, Args: []string{"http-backend"},
		Env: []string{"GIT_PROJECT_ROOT=" + root, "GIT_HTTP_EXPORT_ALL=1"}})
	t.Cleanup(server.Close)
	return server
}

func TestHostedForkOfANodePublishedRepositoryBorrowsTheGatewayCopy(t *testing.T) {
	node, desired := upstreamForkNode(t)
	ctx := context.Background()
	seed := node.gateway.Repositories[0].GitDir
	seedMain := gitRefValue(ctx, seed, "refs/heads/main")
	*desired = []hostedDesired{upstreamFork(testForkID, "forkmesh", "forkmesh",
		"https://unreachable.invalid/forkmesh/forkmesh", true)}
	if err := node.hosted.reconcile(ctx); err != nil {
		t.Fatal(err)
	}
	fork := node.hosted.gitDir(testForkID)
	pool := filepath.Join(node.hosted.cfg.Root, hostedPoolDirectory, testUpstreamNetwork+".git")
	alternates, err := os.ReadFile(filepath.Join(fork, "objects", "info", "alternates"))
	if err != nil || strings.TrimSpace(string(alternates)) != filepath.Join(pool, "objects") {
		t.Fatalf("alternates = %q, %v", alternates, err)
	}
	if got := gitRefValue(ctx, fork, "refs/heads/main"); got == "" || got != seedMain {
		t.Fatalf("fork main = %q, want %q", got, seedMain)
	}
	if head := gitValue(ctx, fork, "symbolic-ref", "HEAD"); head != "refs/heads/main" {
		t.Fatalf("fork HEAD = %q", head)
	}
	if objectBytes(t, fork) > 16<<10 {
		t.Fatalf("the fork copied objects: %d", objectBytes(t, fork))
	}
	if _, ok := hostedEntry(node, testForkID); !ok {
		t.Fatal("the fork is not served")
	}
	if _, err := os.Stat(filepath.Join(seed, hostedMaintenanceLock)); !os.IsNotExist(err) {
		t.Fatal("the gateway copy was given a maintenance lock")
	}
	if got := gitRefValue(ctx, pool, "refs/pools/"+testUpstreamNetwork+"/heads/main"); got != seedMain {
		t.Fatalf("pool ref = %q", got)
	}

	*desired = append(*desired, upstreamFork(testForkForkID, "forkmesh", "forkmesh",
		"https://unreachable.invalid/forkmesh/forkmesh", false))
	(*desired)[1].Owner = "carol"
	if err := node.hosted.reconcile(ctx); err != nil {
		t.Fatal(err)
	}
	replica := node.hosted.gitDir(testForkForkID)
	if _, err := os.Stat(filepath.Join(replica, "objects", "info", "alternates")); err != nil {
		t.Fatal("a replica did not borrow from the network pool")
	}
	if refs := gitLines(ctx, replica, "for-each-ref", "--format=%(refname)"); len(refs) != 0 {
		t.Fatalf("a non-primary core chose the fork's refs: %v", refs)
	}
}

func TestHostedForkOfANodePublishedRepositoryFetchesItsCloneURL(t *testing.T) {
	node, desired := upstreamForkNode(t)
	ctx := context.Background()
	root := t.TempDir()
	source, bare := makeBareRepo(t)
	if err := os.Rename(bare, filepath.Join(root, "tool.git")); err != nil {
		t.Fatal(err)
	}
	sourceMain := runGit(t, source, "rev-parse", "HEAD")
	server := gitHTTPServer(t, root)

	*desired = []hostedDesired{upstreamFork(testForkForkID, "dana", "tool", server.URL+"/tool.git", false)}
	if err := node.hosted.reconcile(ctx); err != nil {
		t.Fatal(err)
	}
	pool := filepath.Join(node.hosted.cfg.Root, hostedPoolDirectory, testUpstreamNetwork+".git")
	if _, err := os.Stat(pool); !os.IsNotExist(err) {
		t.Fatal("a replica fetched the parent itself")
	}

	*desired = []hostedDesired{upstreamFork(testForkID, "dana", "tool", server.URL+"/tool.git", true)}
	if err := node.hosted.reconcile(ctx); err != nil {
		t.Fatal(err)
	}
	fork := node.hosted.gitDir(testForkID)
	if got := gitRefValue(ctx, fork, "refs/heads/main"); got != sourceMain {
		t.Fatalf("fork main = %q, want %q", got, sourceMain)
	}
	if objectBytes(t, fork) > 16<<10 {
		t.Fatalf("the fork holds the parent's objects instead of the pool: %d", objectBytes(t, fork))
	}
	if got := gitValue(ctx, pool, "config", "--get", "gc.pruneExpire"); got != "never" {
		t.Fatalf("pool gc.pruneExpire = %q", got)
	}
	runGit(t, fork, "fsck", "--connectivity-only", "--no-dangling")

	*desired = append(*desired, upstreamFork(testForkForkID, "dana", "tool", server.URL+"/tool.git", false))
	(*desired)[1].Owner = "carol"
	if err := node.hosted.reconcile(ctx); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(filepath.Join(node.hosted.gitDir(testForkForkID), "objects", "info", "alternates")); err != nil {
		t.Fatal("a replica did not borrow from an existing pool")
	}
}

func TestHostedForkOfANodePublishedRepositoryStaysWithinTheSizeLimit(t *testing.T) {
	node, desired := upstreamForkNode(t)
	node.hosted.cfg.MaxRepositoryBytes = 64 << 10
	root := t.TempDir()
	source, bare := makeBareRepo(t)
	commitAndPush(t, source, bare, "refs/heads/main", 1<<20)
	if err := os.Rename(bare, filepath.Join(root, "big.git")); err != nil {
		t.Fatal(err)
	}
	server := gitHTTPServer(t, root)
	*desired = []hostedDesired{upstreamFork(testForkID, "dana", "big", server.URL+"/big.git", true)}
	if err := node.hosted.reconcile(context.Background()); err == nil ||
		!strings.Contains(err.Error(), "larger than the hosted limit") {
		t.Fatalf("a parent over the size limit was fetched: %v", err)
	}
	if refs := gitLines(context.Background(), node.hosted.gitDir(testForkID), "for-each-ref",
		"--format=%(refname)"); len(refs) != 0 {
		t.Fatalf("fork refs = %v", refs)
	}
}

func TestHostedForkUpstreamEntriesAreValidated(t *testing.T) {
	valid := upstreamFork(testForkID, "forkmesh", "forkmesh", "https://app.forkmesh.com/forkmesh/forkmesh", true)
	if !validHostedDesired(valid) {
		t.Fatal("refused a valid upstream fork")
	}
	for _, mutate := range []func(*hostedDesired){
		func(item *hostedDesired) { item.ForkOf = testHostedID },
		func(item *hostedDesired) { item.Network = "" },
		func(item *hostedDesired) { item.Visibility = "private" },
		func(item *hostedDesired) { item.ForkUpstream.Owner = "Bad Owner" },
		func(item *hostedDesired) { item.ForkUpstream.Name = ".hidden" },
		func(item *hostedDesired) { item.ForkUpstream.CloneURL = "http://app.forkmesh.com/forkmesh/forkmesh" },
		func(item *hostedDesired) {
			item.ForkUpstream.CloneURL = "https://u:p@app.forkmesh.com/forkmesh/forkmesh"
		},
		func(item *hostedDesired) {
			item.ForkUpstream.CloneURL = "https://app.forkmesh.com/forkmesh/forkmesh?x=1"
		},
		func(item *hostedDesired) { item.ForkUpstream.CloneURL = "https://app.forkmesh.com/forkmesh/forkmesh#x" },
		func(item *hostedDesired) { item.ForkUpstream.CloneURL = "https://app.forkmesh.com/../forkmesh" },
		func(item *hostedDesired) { item.ForkUpstream.CloneURL = "https://10.0.0.1/forkmesh/forkmesh" },
		func(item *hostedDesired) { item.ForkUpstream.CloneURL = "https://localhost/forkmesh/forkmesh" },
		func(item *hostedDesired) { item.ForkUpstream.CloneURL = "ssh://app.forkmesh.com/forkmesh/forkmesh" },
		func(item *hostedDesired) { item.ForkUpstream.CloneURL = "https://app.forkmesh.com/" },
	} {
		item := valid
		upstream := *valid.ForkUpstream
		item.ForkUpstream = &upstream
		mutate(&item)
		if validHostedDesired(item) {
			t.Fatalf("accepted %+v %+v", item, *item.ForkUpstream)
		}
	}
}
