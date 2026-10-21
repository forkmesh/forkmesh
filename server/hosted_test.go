package mirrornode

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"io"
	"net/http"
	"net/http/cgi"
	"net/http/httptest"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"
)

const (
	testHostedID    = "hr0123456789abcdef01234567"
	testHostedGrant = "Z3JhbnQtcGF5bG9hZC0wMTIzNDU2Nzg5." +
		"SSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSS"
)

// fakeHostedApp answers /api/hosted-repositories/replicas with a scripted
// desired set per node and records what each node reported.
type fakeHostedApp struct {
	mu      sync.Mutex
	answer  func(node string, reported map[string]hostedCopyState) hostedReplicaResponse
	reports map[string][]map[string]hostedCopyState
	pushes  map[string][]map[string]hostedPushReport
}

func (f *fakeHostedApp) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	if r.URL.Path != hostedReplicasPath || r.Method != http.MethodPost {
		http.NotFound(w, r)
		return
	}
	node := r.Header.Get("X-ForkMesh-Node")
	body, _ := io.ReadAll(r.Body)
	digest := sha256.Sum256(body)
	if node == "" || r.Header.Get("X-ForkMesh-Signature") == "" ||
		r.Header.Get("X-ForkMesh-Body-Sha256") != hex.EncodeToString(digest[:]) {
		http.Error(w, "unsigned", http.StatusUnauthorized)
		return
	}
	var request hostedReplicaRequest
	if err := json.Unmarshal(body, &request); err != nil {
		http.Error(w, "bad json", http.StatusBadRequest)
		return
	}
	f.mu.Lock()
	if f.reports == nil {
		f.reports = map[string][]map[string]hostedCopyState{}
	}
	f.reports[node] = append(f.reports[node], request.Repositories)
	if f.pushes == nil {
		f.pushes = map[string][]map[string]hostedPushReport{}
	}
	f.pushes[node] = append(f.pushes[node], request.PushMirrors)
	answer := f.answer
	f.mu.Unlock()
	response := answer(node, request.Repositories)
	response.OK = true
	_ = json.NewEncoder(w).Encode(response)
}

func (f *fakeHostedApp) lastReport(node string) map[string]hostedCopyState {
	f.mu.Lock()
	defer f.mu.Unlock()
	reports := f.reports[node]
	if len(reports) == 0 {
		return nil
	}
	return reports[len(reports)-1]
}

// peerGateway serves a node's hosted root on /v1/runner/<owner>/<repo>.git
// through git http-backend, and only with the expected grant header: the
// shape of tools/mirror_gateway.py's runner route.
func peerGateway(t *testing.T, root, id, grant string) *httptest.Server {
	t.Helper()
	gitPath, err := exec.LookPath("git")
	if err != nil {
		t.Skip("git is not installed")
	}
	backend := &cgi.Handler{
		Path: gitPath,
		Args: []string{"http-backend"},
		Env:  []string{"GIT_PROJECT_ROOT=" + root, "GIT_HTTP_EXPORT_ALL=1"},
	}
	return httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("X-ForkMesh-Runner-Grant") != grant {
			http.NotFound(w, r)
			return
		}
		pieces := strings.SplitN(strings.TrimPrefix(r.URL.Path, "/v1/runner/"), "/", 3)
		if len(pieces) != 3 || !strings.HasSuffix(pieces[1], ".git") {
			http.NotFound(w, r)
			return
		}
		r.URL.Path = "/" + id + ".git/" + pieces[2]
		backend.ServeHTTP(w, r)
	}))
}

func hostedTestDaemon(t *testing.T, name, catalogURL string) *Daemon {
	t.Helper()
	identity, _ := testIdentity(t)
	_, seed := makeBareRepo(t)
	dir := t.TempDir()
	gatewayPath := filepath.Join(dir, "gateway.json")
	var gateway GatewayConfig
	gateway.SchemaVersion = 1
	gateway.Node.Name = name
	gateway.Node.PublicKey = identity.PublicKey()
	seedRepo := Repository{Owner: "forkmesh", Name: "forkmesh", Visibility: "public",
		Enabled: true, GitDir: seed, Operations: []string{"git-info-refs", "git-upload-pack"}}
	digest, err := refsSHA256(context.Background(), seed)
	if err != nil {
		t.Fatal(err)
	}
	seedRepo.Integrity.ExpectedRefsSHA256 = digest
	gateway.Repositories = []Repository{seedRepo}
	if _, err := writeGatewayConfig(gatewayPath, gateway); err != nil {
		t.Fatal(err)
	}
	daemon := &Daemon{
		config: Config{
			GatewayConfig: gatewayPath, CatalogURL: catalogURL + "/api/repositories",
			PublishOwner: "forkmesh", Version: "test", Role: RoleCore,
			Hosted: &HostedConfig{Enabled: true, Root: filepath.Join(dir, "hosted")},
		},
		gateway: gateway, identity: identity, supervisor: NewSupervisor(),
		syncRequests: make(chan struct{}, 1), publishRequests: make(chan struct{}, 1),
	}
	hosted, err := newHostedReplicator(daemon)
	if err != nil {
		t.Fatal(err)
	}
	daemon.hosted = hosted
	return daemon
}

func hostedEntry(d *Daemon, id string) (Repository, bool) {
	for _, repo := range d.gateway.Repositories {
		if hostedIDFromGitDir(d.hosted.cfg.Root, repo.GitDir) == id {
			return repo, true
		}
	}
	return Repository{}, false
}

func testInit() *hostedInit {
	return &hostedInit{
		DefaultBranch: "main", Message: "Initial commit",
		AuthorName: "alice", AuthorEmail: "alice@users.noreply.next.forkmesh.com",
		Time: 1790000000,
		Files: []hostedInitFile{
			{Path: "README.md", Content: "# proj\n\nA hosted repository.\n"},
			{Path: ".gitignore", Content: "node_modules/\n"},
		},
	}
}

func TestHostedConfigValidation(t *testing.T) {
	base := func() Config {
		return Config{Role: RoleCore, CatalogURL: "https://app.example/api/repositories"}
	}
	cfg := base()
	cfg.Hosted = &HostedConfig{Enabled: true, Root: "relative"}
	if err := cfg.validateHosted(); err == nil {
		t.Fatal("a relative hosted.root was accepted")
	}
	cfg = base()
	cfg.Role = RoleRunner
	cfg.Hosted = &HostedConfig{Enabled: true, Root: "/var/lib/forkmesh/hosted"}
	if err := cfg.validateHosted(); err == nil {
		t.Fatal("a runner accepted hosted repositories")
	}
	cfg = base()
	cfg.Hosted = &HostedConfig{Enabled: true, Root: "/var/lib/forkmesh/hosted",
		ImportHosts: []string{"GitHub.com"}}
	if err := cfg.validateHosted(); err == nil {
		t.Fatal("an upper-case import host was accepted")
	}
	cfg = base()
	cfg.Hosted = &HostedConfig{Enabled: true, Root: "/var/lib/forkmesh/hosted"}
	if err := cfg.validateHosted(); err != nil {
		t.Fatal(err)
	}
}

// create -> the primary initializes, a push lands on the primary, the replica
// converges from it through the grant-protected runner route, and a private
// repository is registered as "restricted".
func TestHostedReplicationCreatePushReplicate(t *testing.T) {
	var app fakeHostedApp
	appServer := httptest.NewServer(&app)
	defer appServer.Close()
	primary := hostedTestDaemon(t, "fmnext-n1", appServer.URL)
	replica := hostedTestDaemon(t, "fmnext-n2", appServer.URL)
	source := peerGateway(t, primary.hosted.cfg.Root, testHostedID, testHostedGrant)
	defer source.Close()

	var mu sync.Mutex
	pushedAt := int64(0)
	visibility := "public"
	app.answer = func(node string, reported map[string]hostedCopyState) hostedReplicaResponse {
		mu.Lock()
		defer mu.Unlock()
		item := hostedDesired{ID: testHostedID, Owner: "alice", Name: "proj",
			Visibility: visibility, PushedAt: pushedAt, Init: testInit()}
		state, known := reported[testHostedID]
		if node == "fmnext-n1" {
			item.Primary = true
		} else if !known || state.Empty || state.PushedAt < pushedAt {
			item.Sources = []hostedSource{{Node: "fmnext-n1",
				URL: source.URL + "/v1/runner/alice/proj.git", Grant: testHostedGrant}}
		}
		return hostedReplicaResponse{Repositories: []hostedDesired{item}}
	}
	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()

	if err := primary.hosted.reconcile(ctx); err != nil {
		t.Fatal(err)
	}
	primaryDir := primary.hosted.gitDir(testHostedID)
	head := run(t, ".", "git", "--git-dir="+primaryDir, "rev-parse", "refs/heads/main")
	readme := run(t, ".", "git", "--git-dir="+primaryDir, "show", "main:README.md")
	if !strings.Contains(readme, "A hosted repository.") {
		t.Fatalf("README = %q", readme)
	}
	entry, ok := hostedEntry(primary, testHostedID)
	if !ok || entry.Visibility != "public" || entry.Owner != "alice" ||
		!strings.Contains(strings.Join(entry.Operations, ","), "git-receive-pack") {
		t.Fatalf("gateway entry = %+v (found %v)", entry, ok)
	}
	loaded, _, err := loadGatewayConfig(primary.config.GatewayConfig)
	if err != nil || len(loaded.Repositories) != 2 {
		t.Fatalf("persisted gateway config: %v %+v", err, loaded.Repositories)
	}
	// The second round reported the initialized copy, with its routing digest.
	report := app.lastReport("fmnext-n1")[testHostedID]
	wantDigest, _ := routingRefsSHA256(ctx, primaryDir)
	if report.Empty || report.Digest != wantDigest || report.SizeBytes <= 0 {
		t.Fatalf("primary report = %+v", report)
	}

	// The same init gives the same commit on another node (determinism).
	other := hostedTestDaemon(t, "fmnext-n3", appServer.URL)
	otherDir := other.hosted.gitDir(testHostedID)
	if _, err := other.hosted.ensureBare(ctx, hostedDesired{ID: testHostedID}, otherDir); err != nil {
		t.Fatal(err)
	}
	if err := other.hosted.initialize(ctx, otherDir, *testInit()); err != nil {
		t.Fatal(err)
	}
	if got := run(t, ".", "git", "--git-dir="+otherDir, "rev-parse", "refs/heads/main"); got != head {
		t.Fatalf("init is not deterministic: %s != %s", got, head)
	}

	// The replica fetches the initialized repository from the primary.
	if err := replica.hosted.reconcile(ctx); err != nil {
		t.Fatal(err)
	}
	replicaDir := replica.hosted.gitDir(testHostedID)
	if got := run(t, ".", "git", "--git-dir="+replicaDir, "rev-parse", "refs/heads/main"); got != head {
		t.Fatalf("replica main = %s, want %s", got, head)
	}
	if run(t, ".", "git", "--git-dir="+replicaDir, "for-each-ref", "refs/forkmesh") != "" {
		t.Fatal("staging refs were left behind")
	}

	// A push lands on the primary (a second commit and a feature branch).
	work := filepath.Join(t.TempDir(), "work")
	run(t, ".", "git", "clone", "--quiet", primaryDir, work)
	run(t, work, "git", "config", "user.email", "alice@example.test")
	run(t, work, "git", "config", "user.name", "Alice")
	if err := os.WriteFile(filepath.Join(work, "main.go"), []byte("package main\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	run(t, work, "git", "add", "main.go")
	run(t, work, "git", "commit", "--quiet", "-m", "code")
	run(t, work, "git", "push", "--quiet", "origin", "main", "main:refs/heads/feature")
	pushedHead := run(t, work, "git", "rev-parse", "HEAD")
	mu.Lock()
	pushedAt = 1790000123000
	visibility = "private"
	mu.Unlock()

	if err := replica.hosted.reconcile(ctx); err != nil {
		t.Fatal(err)
	}
	for _, ref := range []string{"refs/heads/main", "refs/heads/feature"} {
		if got := run(t, ".", "git", "--git-dir="+replicaDir, "rev-parse", ref); got != pushedHead {
			t.Fatalf("replica %s = %s, want %s", ref, got, pushedHead)
		}
	}
	entry, _ = hostedEntry(replica, testHostedID)
	if entry.Visibility != "restricted" {
		t.Fatalf("a private repository is served as %q", entry.Visibility)
	}
	digest, _ := refsSHA256(ctx, replicaDir)
	if entry.Integrity.ExpectedRefsSHA256 != digest {
		t.Fatal("the gateway pin does not follow the replicated refs")
	}
	if got := app.lastReport("fmnext-n2")[testHostedID].PushedAt; got != pushedAt {
		t.Fatalf("replica reported pushedAt %d, want %d", got, pushedAt)
	}
}

func TestHostedRejectsBadGrantsAndSources(t *testing.T) {
	var app fakeHostedApp
	appServer := httptest.NewServer(&app)
	defer appServer.Close()
	replica := hostedTestDaemon(t, "fmnext-n2", appServer.URL)
	ctx := context.Background()
	dir := replica.hosted.gitDir(testHostedID)
	if _, err := replica.hosted.ensureBare(ctx, hostedDesired{ID: testHostedID}, dir); err != nil {
		t.Fatal(err)
	}
	for _, source := range []hostedSource{
		{Node: "fmnext-n1", URL: "https://example.test/alice/proj.git", Grant: testHostedGrant},
		{Node: "fmnext-n1", URL: "http://example.test/v1/runner/alice/proj.git", Grant: testHostedGrant},
		{Node: "fmnext-n1", URL: "https://user:pw@example.test/v1/runner/alice/proj.git", Grant: testHostedGrant},
		{Node: "fmnext-n1", URL: "https://example.test/v1/runner/alice/proj.git", Grant: "short"},
	} {
		if err := replica.hosted.fetchFromPeers(ctx, dir, []hostedSource{source}, nil); err == nil ||
			!strings.Contains(err.Error(), "invalid source") {
			t.Fatalf("source %+v was not refused: %v", source, err)
		}
	}
	for raw, want := range map[string]bool{
		"https://github.com/owner/repo.git":      true,
		"https://codeberg.org/owner/repo":        true,
		"http://github.com/owner/repo.git":       false,
		"https://evil.example/owner/repo.git":    false,
		"https://github.com:8443/owner/repo.git": false,
		"https://x@github.com/owner/repo.git":    false,
		"https://github.com/":                    false,
		"https://github.com.evil/owner/repo.git": false,
	} {
		if got := replica.hosted.importAllowed(raw); got != want {
			t.Fatalf("importAllowed(%q) = %v", raw, got)
		}
	}
}

func TestHostedRemovalMovesTheRepositoryToTrash(t *testing.T) {
	var app fakeHostedApp
	appServer := httptest.NewServer(&app)
	defer appServer.Close()
	node := hostedTestDaemon(t, "fmnext-n1", appServer.URL)
	removed := false
	app.answer = func(string, map[string]hostedCopyState) hostedReplicaResponse {
		if removed {
			return hostedReplicaResponse{Removed: []hostedRemoved{
				{ID: testHostedID, Owner: "alice", Name: "proj"}}}
		}
		return hostedReplicaResponse{Repositories: []hostedDesired{{
			ID: testHostedID, Owner: "alice", Name: "proj", Visibility: "public",
			Primary: true, Init: testInit()}}}
	}
	ctx := context.Background()
	if err := node.hosted.reconcile(ctx); err != nil {
		t.Fatal(err)
	}
	if _, ok := hostedEntry(node, testHostedID); !ok {
		t.Fatal("repository was not registered")
	}
	removed = true
	if err := node.hosted.reconcile(ctx); err != nil {
		t.Fatal(err)
	}
	if _, ok := hostedEntry(node, testHostedID); ok {
		t.Fatal("a removed repository is still served")
	}
	if _, err := os.Stat(node.hosted.gitDir(testHostedID)); !os.IsNotExist(err) {
		t.Fatal("the removed repository stayed in the served root")
	}
	trashed, _ := filepath.Glob(filepath.Join(node.hosted.cfg.Root, ".trash", testHostedID+"-*.git"))
	if len(trashed) != 1 {
		t.Fatalf("trash = %v", trashed)
	}
	// The seed repository is untouched.
	if len(node.gateway.Repositories) != 1 || node.gateway.Repositories[0].Owner != "forkmesh" {
		t.Fatalf("gateway = %+v", node.gateway.Repositories)
	}
}

func TestHostedSkipsNamesServedByConfiguredRepositories(t *testing.T) {
	var app fakeHostedApp
	appServer := httptest.NewServer(&app)
	defer appServer.Close()
	node := hostedTestDaemon(t, "fmnext-n1", appServer.URL)
	app.answer = func(string, map[string]hostedCopyState) hostedReplicaResponse {
		return hostedReplicaResponse{Repositories: []hostedDesired{{
			ID: testHostedID, Owner: "forkmesh", Name: "ForkMesh", Visibility: "public",
			Primary: true}}}
	}
	err := node.hosted.reconcile(context.Background())
	if err == nil || !strings.Contains(err.Error(), "served by a configured repository") {
		t.Fatalf("collision was not refused: %v", err)
	}
	if len(node.gateway.Repositories) != 1 {
		t.Fatal("a hosted entry shadowed a configured repository")
	}
}

func TestRoutingRefsSHA256MatchesTheGatewayFormula(t *testing.T) {
	_, bare := makeBareRepo(t)
	run(t, ".", "git", "--git-dir="+bare, "branch", "other")
	run(t, ".", "git", "--git-dir="+bare, "tag", "v1")
	digest, err := routingRefsSHA256(context.Background(), bare)
	if err != nil {
		t.Fatal(err)
	}
	// Default branch and tags only; other branches never move the pin.
	lines := []string{
		run(t, ".", "git", "--git-dir="+bare, "rev-parse", "refs/heads/main") + " refs/heads/main",
		run(t, ".", "git", "--git-dir="+bare, "rev-parse", "refs/tags/v1") + " refs/tags/v1",
	}
	want := sha256.Sum256([]byte(strings.Join(lines, "\n")))
	if digest != hex.EncodeToString(want[:]) {
		t.Fatalf("got %s want %x", digest, want)
	}
}

func TestHostedEventWakesTheReplicator(t *testing.T) {
	var app fakeHostedApp
	appServer := httptest.NewServer(&app)
	defer appServer.Close()
	node := hostedTestDaemon(t, "fmnext-n1", appServer.URL)
	if !node.hosted.due(time.Now()) {
		t.Fatal("a fresh replicator is not due")
	}
	if node.hosted.due(time.Now()) {
		t.Fatal("the safety interval did not hold")
	}
	node.handleNodeEvent(Event{Topic: "hosted", Repo: "alice/proj"})
	if !node.hosted.due(time.Now()) {
		t.Fatal("the hosted event did not wake the replicator")
	}
	if node.hosted.due(time.Now().Add(hostedSafetyInterval+time.Second)) != true {
		t.Fatal("the safety interval never fires")
	}
}

func TestHostedOrganizationAliasSharesOneGitDir(t *testing.T) {
	var app fakeHostedApp
	appServer := httptest.NewServer(&app)
	defer appServer.Close()
	node := hostedTestDaemon(t, "fmnext-n1", appServer.URL)
	app.answer = func(string, map[string]hostedCopyState) hostedReplicaResponse {
		return hostedReplicaResponse{Repositories: []hostedDesired{{
			ID: testHostedID, Owner: "alice", Name: "proj", Visibility: "private",
			Primary: true, Init: testInit(), Aliases: []string{"acme", "Bad Name"}}}}
	}
	if err := node.hosted.reconcile(context.Background()); err != nil {
		t.Fatal(err)
	}
	var owners []string
	for _, repo := range node.gateway.Repositories {
		if hostedIDFromGitDir(node.hosted.cfg.Root, repo.GitDir) == testHostedID {
			owners = append(owners, repo.Owner)
			if repo.Visibility != "restricted" {
				t.Fatalf("alias entry visibility = %q", repo.Visibility)
			}
		}
	}
	if strings.Join(owners, ",") != "alice,acme" {
		t.Fatalf("hosted owners = %v", owners)
	}
}

func TestHostedPublicRepositoryOffersWebMerges(t *testing.T) {
	var app fakeHostedApp
	appServer := httptest.NewServer(&app)
	defer appServer.Close()
	node := hostedTestDaemon(t, "fmnext-n1", appServer.URL)
	node.gateway.MergeExecutorCommand = []string{"/usr/bin/forkmesh-mirror-node", "--execute-mirror-merge"}
	node.agentBranchOps = true
	visibility := "public"
	app.answer = func(string, map[string]hostedCopyState) hostedReplicaResponse {
		return hostedReplicaResponse{Repositories: []hostedDesired{{
			ID: testHostedID, Owner: "alice", Name: "proj", Visibility: visibility,
			Primary: true, Init: testInit()}}}
	}
	if err := node.hosted.reconcile(context.Background()); err != nil {
		t.Fatal(err)
	}
	entry, _ := hostedEntry(node, testHostedID)
	for _, op := range []string{"git-receive-pack", "merge-preview", "merge-pull",
		"update-pull-branch", agentBranchMergeOp, agentBranchDeleteOp} {
		if !hasOperation(entry.Operations, op) {
			t.Fatalf("public hosted entry lacks %s: %v", op, entry.Operations)
		}
	}

	visibility = "private"
	if err := node.hosted.reconcile(context.Background()); err != nil {
		t.Fatal(err)
	}
	entry, _ = hostedEntry(node, testHostedID)
	for _, op := range []string{"merge-pull", "update-pull-branch", agentBranchMergeOp} {
		if hasOperation(entry.Operations, op) {
			t.Fatalf("private hosted entry offers %s: %v", op, entry.Operations)
		}
	}

	visibility = "public"
	node.gateway.MergeExecutorCommand = nil
	if err := node.hosted.reconcile(context.Background()); err != nil {
		t.Fatal(err)
	}
	entry, _ = hostedEntry(node, testHostedID)
	if hasOperation(entry.Operations, "merge-pull") {
		t.Fatalf("a node without the executor offers merge-pull: %v", entry.Operations)
	}
}
