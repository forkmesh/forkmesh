package mirrornode

import (
	"context"
	"crypto/ed25519"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/http/cgi"
	"net/http/httptest"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"
)

// fakeRouterKey signs grants the way the app does; the fake gateway checks
// them with runnergrant.go, as the real gateway does with the Go verifier.
var fakeRouterKey = ed25519.NewKeyFromSeed(make([]byte, 32))

func mintGrant(t *testing.T, g runnerGrant) string {
	t.Helper()
	message := runnerGrantMessage(g)
	if message == "" {
		t.Fatalf("invalid grant %+v", g)
	}
	return base64.RawURLEncoding.EncodeToString([]byte(message)) + "." +
		base64.RawURLEncoding.EncodeToString(ed25519.Sign(fakeRouterKey, []byte(message)))
}

// fakeRunnerGateway serves /v1/runner/core1/proj.git with git http-backend,
// admitting a read grant for fetches and a push grant for pushes.
type fakeRunnerGateway struct {
	t      *testing.T
	bare   string
	mu     sync.Mutex
	grants []runnerGrant
}

func (g *fakeRunnerGateway) ServeHTTP(w http.ResponseWriter, request *http.Request) {
	const prefix = "/v1/runner/core1/proj.git"
	if !strings.HasPrefix(request.URL.Path, prefix+"/") {
		http.NotFound(w, request)
		return
	}
	token := request.Header.Get(runnerGrantHeader)
	encoded, signature, _ := strings.Cut(token, ".")
	message, _ := base64.RawURLEncoding.DecodeString(encoded)
	sig, _ := base64.RawURLEncoding.DecodeString(signature)
	grant, ok := parseRunnerGrantToken(token)
	push := strings.HasSuffix(request.URL.Path, "/git-receive-pack") ||
		request.URL.Query().Get("service") == "git-receive-pack"
	want := "read"
	if push {
		want = "push"
	}
	if !ok || !ed25519.Verify(fakeRouterKey.Public().(ed25519.PublicKey), message, sig) ||
		grant.Access != want || grant.Node != "core1" || grant.Repository != "proj" {
		http.Error(w, "grant refused", http.StatusForbidden)
		return
	}
	g.mu.Lock()
	g.grants = append(g.grants, grant)
	g.mu.Unlock()
	backend := filepath.Join(strings.TrimSpace(gitOutput(g.t, "--exec-path")), "git-http-backend")
	handler := &cgi.Handler{Path: backend, Env: []string{
		"GIT_PROJECT_ROOT=" + filepath.Dir(g.bare), "GIT_HTTP_EXPORT_ALL=1"}}
	request.URL.Path = "/" + filepath.Base(g.bare) + strings.TrimPrefix(request.URL.Path, prefix)
	handler.ServeHTTP(w, request)
}

func gitOutput(t *testing.T, args ...string) string {
	t.Helper()
	out, err := exec.Command("git", args...).Output()
	if err != nil {
		t.Fatal(err)
	}
	return string(out)
}

// fakeRunnerApp serves /api/runners/claim and /report, verifying the
// runner's request signature exactly as runner_leases.py does.
type fakeRunnerApp struct {
	t       *testing.T
	public  ed25519.PublicKey
	mu      sync.Mutex
	queue   []map[string]any
	claims  []map[string]any
	reports []map[string]any
	live    []map[string]any
	polls   []map[string]any
	// logins are handed out once on the next claim or poll; login answers
	// POST /api/runners/login/output (runnerlogin_test.go).
	logins      []map[string]any
	login       func(fields map[string]any) (int, any)
	waitMs      int64
	attachments map[string][]byte
	fetches     []map[string]any
}

func (a *fakeRunnerApp) ServeHTTP(w http.ResponseWriter, request *http.Request) {
	body, _ := io.ReadAll(request.Body)
	digest := sha256.Sum256(body)
	header := request.Header
	message := strings.Join([]string{"forkmesh-masked-proxy-v1", header.Get("X-ForkMesh-Node"),
		request.Method, request.URL.Path, header.Get("X-ForkMesh-Body-Sha256"),
		header.Get("X-ForkMesh-Request-Id"), header.Get("X-ForkMesh-Issued-At")}, "\n")
	sig, _ := base64.RawURLEncoding.DecodeString(header.Get("X-ForkMesh-Signature"))
	if header.Get("X-ForkMesh-Node") != "runner1" || header.Get("X-ForkMesh-Body-Sha256") != hex.EncodeToString(digest[:]) ||
		!ed25519.Verify(a.public, []byte(message), sig) {
		http.Error(w, `{"error":"unauthorized"}`, http.StatusUnauthorized)
		return
	}
	var fields map[string]any
	_ = json.Unmarshal(body, &fields)
	a.mu.Lock()
	defer a.mu.Unlock()
	switch request.URL.Path {
	case runnerClaimPath:
		a.claims = append(a.claims, fields)
		jobs := []map[string]any{}
		if len(a.queue) > 0 {
			jobs, a.queue = append(jobs, a.queue[0]), a.queue[1:]
		}
		logins := a.logins
		a.logins = nil
		_ = json.NewEncoder(w).Encode(map[string]any{"ok": true, "jobs": jobs, "logins": logins, "waitMs": a.waitMs})
	case runnerPollPath:
		a.polls = append(a.polls, fields)
		logins := a.logins
		a.logins = nil
		_ = json.NewEncoder(w).Encode(map[string]any{"ok": true, "logins": logins})
	case runnerLoginOutputPath:
		if a.login == nil {
			http.NotFound(w, request)
			return
		}
		status, reply := a.login(fields)
		w.WriteHeader(status)
		_ = json.NewEncoder(w).Encode(reply)
	case runnerAttachmentPath:
		a.fetches = append(a.fetches, fields)
		data, ok := a.attachments[fmt.Sprint(fields["attachmentId"])]
		if !ok {
			http.Error(w, `{"error":"attachment_unavailable"}`, http.StatusGone)
			return
		}
		_, _ = w.Write(data)
	case runnerReportPath:
		if fields["live"] == true {
			a.live = append(a.live, fields)
		} else {
			a.reports = append(a.reports, fields)
		}
		_, _ = w.Write([]byte(`{"ok":true}`))
	default:
		http.NotFound(w, request)
	}
}

func (a *fakeRunnerApp) snapshot() ([]map[string]any, []map[string]any) {
	a.mu.Lock()
	defer a.mu.Unlock()
	return append([]map[string]any(nil), a.claims...), append([]map[string]any(nil), a.reports...)
}

type runnerFixture struct {
	daemon   *Daemon
	app      *fakeRunnerApp
	gateway  *fakeRunnerGateway
	bare     string
	cloneURL string
	work     string
}

func newRunnerFixture(t *testing.T, allowed []string) *runnerFixture {
	t.Helper()
	_, bare := makeBareRepo(t)
	run(t, ".", "git", "--git-dir="+bare, "config", "http.receivepack", "true")
	identity, _ := testIdentity(t)
	public, _ := base64.RawURLEncoding.DecodeString(identity.PublicKey())
	app := &fakeRunnerApp{t: t, public: ed25519.PublicKey(public)}
	appServer := httptest.NewServer(app)
	t.Cleanup(appServer.Close)
	gateway := &fakeRunnerGateway{t: t, bare: bare}
	gatewayServer := httptest.NewServer(gateway)
	t.Cleanup(gatewayServer.Close)
	var gatewayDoc GatewayConfig
	gatewayDoc.Node.Name = "runner1"
	gatewayDoc.Node.PublicKey = identity.PublicKey()
	work := filepath.Join(t.TempDir(), "runner-work")
	daemon := &Daemon{
		config: Config{Role: RoleRunner, Version: "test",
			GatewayConfig: filepath.Join(t.TempDir(), "state", "gateway.json"),
			Runner: &RunnerConfig{AppURL: appServer.URL, WorkRoot: work, MaxConcurrentRuns: 1,
				JobMemoryMax: "700M", JobCPUQuota: "90%", AllowedOwners: allowed}},
		gateway: gatewayDoc, identity: identity, syncRequests: make(chan struct{}, 1),
	}
	r := newAgentJobRunner(daemon)
	r.maxRuns = 1
	box := jobSandbox{memoryMax: "700M", cpuQuota: "90%", workRoot: work} // the rlimit fallback, for real
	r.command = box.runner()
	r.backend = &runnerBackend{r: r, cfg: daemon.config.Runner, identity: identity,
		node: "runner1", version: "test", box: box, client: http.DefaultClient, now: time.Now}
	daemon.agents = r
	return &runnerFixture{daemon: daemon, app: app, gateway: gateway, bare: bare,
		cloneURL: gatewayServer.URL + "/v1/runner/core1/proj.git", work: work}
}

func (f *runnerFixture) job(t *testing.T, id int64, kind, ref string) map[string]any {
	now := time.Now().UnixMilli()
	grant := func(access, ref string) string {
		return mintGrant(t, runnerGrant{Node: "core1", Owner: "core1", Repository: "proj",
			Access: access, Ref: ref, Runner: "runner1", GrantID: "grant-" + access + "-" + strconv.FormatInt(id, 10),
			IssuedAt: now, ExpiresAt: now + 2*3600*1000})
	}
	return map[string]any{
		"jobId": id, "leaseId": "lease-" + strconv.FormatInt(id, 10), "kind": kind,
		"sessionId": "sess-a", "organization": "alice",
		"repositoryOwner": "core1", "repositoryName": "proj",
		"provider": "claude-code", "prompt": "Add a line to the readme", "createPr": true,
		"requestedBy": "alice", "branch": strings.TrimPrefix(ref, "refs/heads/"), "baseBranch": "main",
		"cloneUrl": f.cloneURL, "readGrant": grant("read", ""), "pushGrant": grant("push", ref),
		"pushRef": ref, "grantExpiresAt": now + 2*3600*1000, "timeoutMs": 2 * 3600 * 1000,
	}
}

func (f *runnerFixture) drain(t *testing.T) {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()
	if err := f.daemon.agents.runnerDrain(ctx); err != nil {
		t.Fatal(err)
	}
	f.daemon.agents.wait()
}

func TestRunnerClaimsClonesRunsPushesOnlyItsRefReportsAndWipes(t *testing.T) {
	installFakeClaude(t)
	f := newRunnerFixture(t, nil)
	mainBefore := gitValue(context.Background(), f.bare, "rev-parse", "refs/heads/main")
	f.app.queue = append(f.app.queue, f.job(t, 7, "start", "refs/heads/agent/sess-a"))
	f.drain(t)

	claims, reports := f.app.snapshot()
	if len(claims) != 1 || claims[0]["max"] != float64(1) ||
		!strings.Contains(strings.Join(anyStrings(claims[0]["providers"]), ","), "claude-code") {
		t.Fatalf("claims = %#v", claims)
	}
	if len(reports) != 2 || reports[0]["status"] != "running" || reports[1]["status"] != "completed" ||
		reports[1]["jobId"] != float64(7) || reports[1]["leaseId"] != "lease-7" {
		t.Fatalf("reports = %#v", reports)
	}
	info, _ := reports[1]["agentInfo"].(map[string]any)
	if info["branchName"] != "agent/sess-a" || info["owner"] != "core1" || info["prNumber"] != float64(0) {
		t.Fatalf("agentInfo = %#v", info)
	}
	if readme := gitValue(context.Background(), f.bare, "show", "refs/heads/agent/sess-a:README.md"); !strings.Contains(readme, "agent line") {
		t.Fatalf("pushed branch README = %q", readme)
	}
	if after := gitValue(context.Background(), f.bare, "rev-parse", "refs/heads/main"); after != mainBefore {
		t.Fatal("the runner moved main")
	}
	heads := gitLines(context.Background(), f.bare, "for-each-ref", "--format=%(refname)")
	if strings.Join(heads, ",") != "refs/heads/agent/sess-a,refs/heads/main" {
		t.Fatalf("refs after the run = %v", heads)
	}
	// The workdir is gone, and nothing in the work root kept a grant.
	// Only the daemon's .publish (now empty) and the session homes remain.
	entries, _ := os.ReadDir(f.work)
	for _, entry := range entries {
		if entry.Name() != jobHomesDir && entry.Name() != jobPublishDir && entry.Name() != jobCacheDir {
			t.Fatalf("work root not wiped: %v", entries)
		}
	}
	if published, _ := os.ReadDir(filepath.Join(f.work, jobPublishDir)); len(published) != 0 {
		t.Fatalf("publish repository left behind: %v", published)
	}
	homes, _ := os.ReadDir(filepath.Join(f.work, jobHomesDir))
	for _, home := range homes {
		if _, err := os.Stat(filepath.Join(f.work, jobHomesDir, home.Name(), "tmp")); err == nil {
			t.Fatalf("session home %s kept its tmp", home.Name())
		}
	}
	var sawRead, sawPush bool
	for _, grant := range f.gateway.grants {
		sawRead = sawRead || grant.Access == "read"
		sawPush = sawPush || (grant.Access == "push" && grant.Ref == "refs/heads/agent/sess-a")
	}
	if !sawRead || !sawPush {
		t.Fatalf("gateway grants = %+v", f.gateway.grants)
	}

	cached, _ := os.ReadDir(filepath.Join(f.work, jobCacheDir))
	if len(cached) != 1 || !strings.HasSuffix(cached[0].Name(), ".git") {
		t.Fatalf("repository cache = %v", cached)
	}
	cache := filepath.Join(f.work, jobCacheDir, cached[0].Name())

	// A follow-up on the same session builds on the pushed branch.
	f.app.queue = append(f.app.queue, f.job(t, 8, "steer", "refs/heads/agent/sess-a"))
	f.drain(t)
	_, reports = f.app.snapshot()
	if len(reports) != 4 || reports[3]["status"] != "completed" {
		t.Fatalf("follow-up reports = %#v", reports)
	}
	if readme := gitValue(context.Background(), f.bare, "show", "refs/heads/agent/sess-a:README.md"); strings.Count(readme, "agent line") != 2 {
		t.Fatalf("follow-up did not build on the branch: %q", readme)
	}
	if got, want := gitValue(context.Background(), cache, "rev-parse", "refs/heads/agent/sess-a"), gitValue(context.Background(), f.bare, "rev-parse", "refs/heads/agent/sess-a^"); got == "" || got != want {
		t.Fatalf("cache was not refreshed before the follow-up: %q, want %q", got, want)
	}
}

func TestRunnerCacheFollowsTheCoreAndFallsBackWhenBroken(t *testing.T) {
	installFakeClaude(t)
	f := newRunnerFixture(t, nil)
	f.app.queue = append(f.app.queue, f.job(t, 7, "start", "refs/heads/agent/sess-a"))
	f.drain(t)
	cached, _ := os.ReadDir(filepath.Join(f.work, jobCacheDir))
	if len(cached) != 1 {
		t.Fatalf("repository cache = %v", cached)
	}
	cache := filepath.Join(f.work, jobCacheDir, cached[0].Name())

	run(t, ".", "git", "--git-dir="+f.bare, "update-ref", "refs/heads/gone", "refs/heads/main")
	f.app.queue = append(f.app.queue, f.job(t, 8, "steer", "refs/heads/agent/sess-a"))
	f.drain(t)
	if got := gitValue(context.Background(), cache, "rev-parse", "--verify", "-q", "refs/heads/gone"); got == "" {
		t.Fatal("cache did not fetch a new branch")
	}
	run(t, ".", "git", "--git-dir="+f.bare, "update-ref", "-d", "refs/heads/gone")

	if err := os.WriteFile(filepath.Join(cache, "HEAD"), []byte("garbage\n"), 0600); err != nil {
		t.Fatal(err)
	}
	f.app.queue = append(f.app.queue, f.job(t, 9, "steer", "refs/heads/agent/sess-a"))
	f.drain(t)
	_, reports := f.app.snapshot()
	if len(reports) != 6 || reports[5]["status"] != "completed" {
		t.Fatalf("reports with a broken cache = %#v", reports)
	}
	if readme := gitValue(context.Background(), f.bare, "show", "refs/heads/agent/sess-a:README.md"); strings.Count(readme, "agent line") != 3 {
		t.Fatalf("run with a broken cache did not build on the branch: %q", readme)
	}
	if _, err := os.Stat(cache); err != nil {
		t.Fatalf("broken cache was removed: %v", err)
	}
}

func anyStrings(value any) []string {
	items, _ := value.([]any)
	out := make([]string, 0, len(items))
	for _, item := range items {
		out = append(out, item.(string))
	}
	return out
}

func TestRunnerRejectsInconsistentLeasesWithoutRunning(t *testing.T) {
	calls := installFakeClaude(t)
	f := newRunnerFixture(t, []string{"alice"})
	good := func(id int64) map[string]any { return f.job(t, id, "start", "refs/heads/agent/sess-a") }
	cases := map[string]func(map[string]any){
		"push ref differs from grant": func(j map[string]any) { j["pushRef"] = "refs/heads/agent/other"; j["branch"] = "agent/other" },
		"push to main":                func(j map[string]any) { j["pushRef"] = "refs/heads/main"; j["branch"] = "main" },
		"grants swapped":              func(j map[string]any) { j["readGrant"], j["pushGrant"] = j["pushGrant"], j["readGrant"] },
		"grant for another runner": func(j map[string]any) {
			now := time.Now().UnixMilli()
			j["pushGrant"] = mintGrant(t, runnerGrant{Node: "core1", Owner: "core1", Repository: "proj",
				Access: "push", Ref: "refs/heads/agent/sess-a", Runner: "runner2", GrantID: "grant-other-1",
				IssuedAt: now, ExpiresAt: now + 3600*1000})
		},
		"grant about to expire": func(j map[string]any) {
			now := time.Now().UnixMilli()
			j["pushGrant"] = mintGrant(t, runnerGrant{Node: "core1", Owner: "core1", Repository: "proj",
				Access: "push", Ref: "refs/heads/agent/sess-a", Runner: "runner1", GrantID: "grant-short-1",
				IssuedAt: now - 3000, ExpiresAt: now + 60*1000})
		},
		"clone url credentials": func(j map[string]any) { j["cloneUrl"] = "https://u:p@core.test/v1/runner/core1/proj.git" },
		"clone url not runner":  func(j map[string]any) { j["cloneUrl"] = "https://core.test/core1/proj.git" },
		"clone url other repo":  func(j map[string]any) { j["cloneUrl"] = strings.Replace(f.cloneURL, "proj.git", "other.git", 1) },
		"owner not allowed":     func(j map[string]any) { j["organization"] = "mallory" },
		"repository mismatch":   func(j map[string]any) { j["repositoryName"] = "other" },
		"unknown model":         func(j map[string]any) { j["model"] = "gpt-2" },
	}
	id := int64(100)
	for name, mutate := range cases {
		id++
		job := good(id)
		mutate(job)
		f.app.queue = append(f.app.queue, job)
		f.drain(t)
		_, reports := f.app.snapshot()
		last := reports[len(reports)-1]
		if last["status"] != "rejected" || last["jobId"] != float64(id) {
			t.Fatalf("%s: report = %#v", name, last)
		}
	}
	if log, _ := os.ReadFile(calls); len(log) != 0 {
		t.Fatalf("a rejected lease ran the agent: %s", log)
	}
	if heads := gitLines(context.Background(), f.bare, "for-each-ref", "--format=%(refname)", "refs/heads/agent"); len(heads) != 0 {
		t.Fatalf("a rejected lease pushed: %v", heads)
	}
}

func TestRunnerClaimsOnlyWithAProviderAndFreeSlots(t *testing.T) {
	f := newRunnerFixture(t, nil)
	t.Setenv("PATH", t.TempDir()) // no CLI at all
	t.Setenv("ANTHROPIC_API_KEY", "")
	f.drain(t)
	if claims, _ := f.app.snapshot(); len(claims) != 0 {
		t.Fatalf("claimed without a usable provider: %#v", claims)
	}
	installFakeClaude(t)
	f.daemon.agents.mu.Lock()
	f.daemon.agents.running = 1 // the one slot is busy
	f.daemon.agents.mu.Unlock()
	f.drain(t)
	if claims, _ := f.app.snapshot(); len(claims) != 0 {
		t.Fatalf("claimed beyond maxConcurrentRuns: %#v", claims)
	}
}

func TestRemoveUnderRootRefusesEverythingOutsideTheRoot(t *testing.T) {
	root := filepath.Join(t.TempDir(), "work")
	inside := filepath.Join(root, "abc")
	if err := os.MkdirAll(filepath.Join(inside, "x"), 0700); err != nil {
		t.Fatal(err)
	}
	outside := t.TempDir()
	for _, path := range []string{root, filepath.Dir(root), outside, root + "/../x", "/", root + "-sibling"} {
		if err := removeUnderRoot(root, path); err == nil {
			t.Fatalf("removed %s", path)
		}
	}
	if err := removeUnderRoot("/", "/tmp/x"); err == nil {
		t.Fatal("a / root was accepted")
	}
	if err := removeUnderRoot(root, inside); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(inside); !os.IsNotExist(err) {
		t.Fatal("inside path survived")
	}
	if _, err := os.Stat(outside); err != nil {
		t.Fatal("outside path was touched")
	}
}

func TestRunnerGrantMessageIsCanonicalAndVerifierAcceptsOnlyOwnNode(t *testing.T) {
	g := runnerGrant{Node: "core1", Owner: "core1", Repository: "proj", Access: "push",
		Ref: "refs/heads/agent/sess-a", Runner: "runner1", GrantID: "grant-abcdefgh",
		IssuedAt: 1000, ExpiresAt: 2000}
	want := "forkmesh-runner-grant-v1\ncore1\ncore1\nproj\npush\nrefs/heads/agent/sess-a\nrunner1\ngrant-abcdefgh\n1000\n2000"
	if got := runnerGrantMessage(g); got != want {
		t.Fatalf("message = %q", got)
	}
	if !validateCapabilityPayload([]byte(want), "core1") || validateCapabilityPayload([]byte(want), "core2") {
		t.Fatal("the gateway verifier does not bind grants to its own node")
	}
	for name, mutate := range map[string]func(*runnerGrant){
		"read with ref":  func(g *runnerGrant) { g.Access = "read" },
		"push to main":   func(g *runnerGrant) { g.Ref = "refs/heads/main" },
		"traversal":      func(g *runnerGrant) { g.Ref = "refs/heads/agent/../main" },
		"lock suffix":    func(g *runnerGrant) { g.Ref = "refs/heads/agent/x.lock" },
		"tag":            func(g *runnerGrant) { g.Ref = "refs/tags/agent/x" },
		"too long-lived": func(g *runnerGrant) { g.ExpiresAt = g.IssuedAt + runnerGrantMaxLifetimeMs + 1 },
		"expired order":  func(g *runnerGrant) { g.ExpiresAt = g.IssuedAt },
		"bad access":     func(g *runnerGrant) { g.Access = "admin" },
		"bad runner":     func(g *runnerGrant) { g.Runner = "Runner!" },
	} {
		bad := g
		mutate(&bad)
		if runnerGrantMessage(bad) != "" {
			t.Fatalf("%s accepted", name)
		}
	}
	if _, ok := parseRunnerGrantMessage(want + "\n"); ok {
		t.Fatal("non-canonical grant parsed")
	}
	if _, ok := parseRunnerGrantMessage(strings.Replace(want, "\n1000\n", "\n01000\n", 1)); ok {
		t.Fatal("zero-padded timestamp parsed")
	}
}
