package mirrornode

import (
	"bufio"
	"context"
	"net/http"
	"net/http/httptest"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"
)

func TestUpstreamWatchTarget(t *testing.T) {
	endpoint, repository, ok := upstreamWatchTarget("https://app.forkmesh.com/forkmesh/forkmesh.git")
	if !ok || endpoint != "wss://app.forkmesh.com/api/nodes/events" || repository != "forkmesh/forkmesh" {
		t.Fatalf("target = %q %q %v", endpoint, repository, ok)
	}
	endpoint, repository, ok = upstreamWatchTarget("http://127.0.0.1:9000/Acme/Tool/")
	if !ok || endpoint != "ws://127.0.0.1:9000/api/nodes/events" || repository != "acme/tool" {
		t.Fatalf("loopback target = %q %q %v", endpoint, repository, ok)
	}
	for _, bad := range []string{
		"http://example.com/a/b", "https://user:pw@example.com/a/b",
		"https://example.com/a", "https://example.com/a/b/c",
		"https://example.com/a/b?x=1", "https://example.com/-a/b",
		"ssh://example.com/a/b", "https://example.com/a/..",
	} {
		if _, _, ok := upstreamWatchTarget(bad); ok {
			t.Fatalf("%s accepted", bad)
		}
	}
}

func TestSameRelay(t *testing.T) {
	cases := []struct {
		catalog, upstream string
		want              bool
	}{
		{"https://app.forkmesh.com/api/repositories", "https://forkmesh.com/forkmesh/forkmesh", true},
		{"https://forkmesh.com/api/repositories", "https://app.forkmesh.com/forkmesh/forkmesh", true},
		{"https://app.forkmesh.com/api/repositories", "https://mirror10.forkmesh.com/forkmesh/forkmesh.git", true},
		{"https://code.example.org/api/repositories", "https://app.forkmesh.com/forkmesh/forkmesh", false},
		{"https://git.example.org/api/repositories", "https://example.org/a/b", true},
		{"https://forkmesh.com.evil.test/api/repositories", "https://forkmesh.com/a/b", false},
	}
	for _, item := range cases {
		if got := sameRelay(item.catalog, item.upstream); got != item.want {
			t.Fatalf("sameRelay(%s, %s) = %v", item.catalog, item.upstream, got)
		}
	}
}

func TestInstanceUpstreamKeepsOperatorInterval(t *testing.T) {
	upstream := "https://app.forkmesh.com/forkmesh/forkmesh"
	daemon := &Daemon{config: Config{
		CatalogURL:           "https://code.example.org/api/repositories",
		SyncInterval:         Duration{15 * time.Second},
		Upstreams:            map[string][]string{"forkmesh/forkmesh": {upstream}},
		EventDrivenUpstreams: []string{upstream},
	}, events: &EventSocket{}}
	if got := daemon.fetchInterval(); got != 15*time.Second {
		t.Fatalf("instance fetch interval = %s, want 15s", got)
	}
	if err := daemon.setUpstreamWatches(); err != nil {
		t.Fatal(err)
	}
	if len(daemon.watches) != 1 || daemon.watches[0].watch != "forkmesh/forkmesh" ||
		daemon.watches[0].endpoint != "wss://app.forkmesh.com/api/nodes/events" {
		t.Fatalf("watches = %+v", daemon.watches)
	}
	fleet := &Daemon{config: Config{
		CatalogURL:           "https://app.forkmesh.com/api/repositories",
		Upstreams:            map[string][]string{"forkmesh/forkmesh": {"https://forkmesh.com/forkmesh/forkmesh"}},
		EventDrivenUpstreams: []string{"https://forkmesh.com/forkmesh/forkmesh"},
	}}
	if err := fleet.setUpstreamWatches(); err != nil || len(fleet.watches) != 0 {
		t.Fatalf("fleet node watches its own relay: %v %d", err, len(fleet.watches))
	}
}

func TestUpstreamWatchWakesSyncWithoutSigning(t *testing.T) {
	var (
		mu    sync.Mutex
		query string
	)
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/api/nodes/events" {
			http.NotFound(w, r)
			return
		}
		mu.Lock()
		query = r.URL.RawQuery
		mu.Unlock()
		conn := acceptTestWebSocket(t, w, r)
		defer conn.Close()
		writeTestServerText(t, conn, `{"type":"hello","accepts":[],"role":"watch"}`)
		writeTestServerText(t, conn, `{"type":"event","topic":"commits","repo":"forkmesh/forkmesh","state":"`+strings.Repeat("a", 64)+`","generation":5}`)
		_, _ = bufio.NewReader(conn).ReadByte()
	}))
	defer server.Close()
	upstream := server.URL + "/forkmesh/forkmesh"
	daemon := &Daemon{
		config: Config{
			CatalogURL:           "https://git.example.org/api/repositories",
			Upstreams:            map[string][]string{"forkmesh/forkmesh": {upstream}},
			EventDrivenUpstreams: []string{upstream},
		},
		syncRequests: make(chan struct{}, 1),
	}
	if err := daemon.setUpstreamWatches(); err != nil || len(daemon.watches) != 1 {
		t.Fatalf("watches: %v %d", err, len(daemon.watches))
	}
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	go daemon.watches[0].Run(ctx)
	for i := 0; i < 2; i++ {
		select {
		case <-daemon.syncRequests:
		case <-time.After(5 * time.Second):
			t.Fatalf("sync request %d not raised", i)
		}
	}
	mu.Lock()
	got := query
	mu.Unlock()
	if got != "watch=forkmesh%2Fforkmesh" {
		t.Fatalf("watch query = %q", got)
	}
	deadline := time.Now().Add(5 * time.Second)
	for {
		daemon.mu.RLock()
		up := daemon.watchUp[upstream]
		daemon.mu.RUnlock()
		if up {
			break
		}
		if time.Now().After(deadline) {
			t.Fatal("watch never reported connected")
		}
		time.Sleep(10 * time.Millisecond)
	}
}

func TestReadOnlyMirrorGatewayAndCatalog(t *testing.T) {
	gateway := GatewayConfig{Repositories: []Repository{
		{Owner: "forkmesh", Name: "forkmesh", GitDir: "/m.git", Operations: []string{"blob", "merge-pull", "update-pull-branch", "merge-agent-branch", "delete-agent-branch", "git-upload-pack"}},
		{Owner: "acme", Name: "forkmesh", GitDir: "/m.git", Operations: []string{"blob", "merge-pull"}},
		{Owner: "acme", Name: "app", GitDir: "/h.git", Operations: []string{"blob", "git-receive-pack", "merge-pull"}},
	}}
	daemon := &Daemon{gateway: gateway, config: Config{
		Upstreams: map[string][]string{"forkmesh/forkmesh": {"https://app.forkmesh.com/forkmesh/forkmesh"}},
		MirrorOf:  map[string]string{"forkmesh/forkmesh": "https://forkmesh.com/forkmesh/forkmesh"},
	}}
	if got := daemon.mirrorOfFor(gateway.Repositories[1]); got != "https://forkmesh.com/forkmesh/forkmesh" {
		t.Fatalf("alias mirrorOf = %q", got)
	}
	if got := daemon.mirrorOfFor(gateway.Repositories[2]); got != "" {
		t.Fatalf("hosted mirrorOf = %q", got)
	}
	home := &Daemon{gateway: gateway, config: daemon.config}
	home.config.CatalogURL = "https://app.forkmesh.com/api/repositories"
	if got := home.mirrorOfFor(gateway.Repositories[0]); got != "" {
		t.Fatalf("a node on the upstream's own relay labels the canonical repository a mirror: %q", got)
	}
	ensureReadOnlyMirrorOperations(&daemon.gateway, daemon.readOnlyMirrorDirs())
	want := map[string]string{
		"forkmesh/forkmesh": "blob,git-upload-pack",
		"acme/forkmesh":     "blob",
		"acme/app":          "blob,git-receive-pack,merge-pull",
	}
	for _, repo := range daemon.gateway.Repositories {
		if got := strings.Join(repo.Operations, ","); got != want[repositoryKey(repo)] {
			t.Fatalf("%s operations = %s", repositoryKey(repo), got)
		}
	}
}

func TestMirrorOfConfigValidation(t *testing.T) {
	path := filepath.Join(t.TempDir(), "node.json")
	good := `{"schemaVersion":1,"gatewayConfig":"/a","gatewayScript":"/b","python":"/c","cloudflared":"/d","connectorToken":"/e","identityKey":"/f","catalogUrl":"https://example.test/api/repositories","publishOwner":"forkmesh","upstreams":{"forkmesh/forkmesh":["https://app.forkmesh.com/forkmesh/forkmesh"]},"mirrorOf":{"forkmesh/forkmesh":"https://forkmesh.com/forkmesh/forkmesh"}}`
	load := func(raw string) (Config, error) {
		if err := os.WriteFile(path, []byte(raw), 0600); err != nil {
			t.Fatal(err)
		}
		return LoadConfig(path)
	}
	cfg, err := load(good)
	if err != nil || cfg.MirrorOf["forkmesh/forkmesh"] != "https://forkmesh.com/forkmesh/forkmesh" {
		t.Fatalf("mirrorOf config: %v %+v", err, cfg.MirrorOf)
	}
	for _, page := range []string{"http://forkmesh.com/a/b", "https://u@forkmesh.com/a", "https://forkmesh.com/a/b?x", "https://forkmesh.com/a&b", "https://forkmesh.com"} {
		if _, err := load(strings.Replace(good, "https://forkmesh.com/forkmesh/forkmesh", page, 1)); err == nil {
			t.Fatalf("%s accepted", page)
		}
	}
	if _, err := load(strings.Replace(good, `"mirrorOf":{"forkmesh/forkmesh"`, `"mirrorOf":{"other/repo"`, 1)); err == nil {
		t.Fatal("mirrorOf without upstream accepted")
	}
}

func TestCatalogRecordCarriesMirrorMetadata(t *testing.T) {
	gitDir := filepath.Join(t.TempDir(), "repo.git")
	work := t.TempDir()
	runGit := func(dir string, args ...string) {
		t.Helper()
		cmd := exec.Command("git", args...)
		cmd.Dir = dir
		cmd.Env = append(os.Environ(), "GIT_AUTHOR_NAME=a", "GIT_AUTHOR_EMAIL=a@example.test", "GIT_COMMITTER_NAME=a", "GIT_COMMITTER_EMAIL=a@example.test", "HOME="+work)
		if output, err := cmd.CombinedOutput(); err != nil {
			t.Fatalf("git %v: %v %s", args, err, output)
		}
	}
	runGit(work, "init", "-q", "--initial-branch=main", ".")
	if err := os.WriteFile(filepath.Join(work, "a.txt"), []byte("a"), 0600); err != nil {
		t.Fatal(err)
	}
	runGit(work, "add", "a.txt")
	runGit(work, "commit", "-q", "-m", "a")
	runGit(work, "clone", "-q", "--bare", work, gitDir)
	publisher := &CatalogPublisher{Owner: "forkmesh", Node: "forkmesh", Version: "test", Identity: testEventIdentity(t), DisableAgents: true}
	repo := Repository{Owner: "forkmesh", Name: "forkmesh", GitDir: gitDir}
	state := strings.Repeat("b", 64)
	record, err := publisher.record(context.Background(), repo, state)
	if err != nil {
		t.Fatal(err)
	}
	if _, ok := record["mirrorOf"]; ok {
		t.Fatalf("plain record carries mirror fields: %v", record["mirrorOf"])
	}
	publisher.MirrorOf = "https://forkmesh.com/forkmesh/forkmesh"
	publisher.LastSyncedAt = time.UnixMilli(1792285262771)
	record, err = publisher.record(context.Background(), repo, state)
	if err != nil {
		t.Fatal(err)
	}
	if record["mirrorOf"] != publisher.MirrorOf || record["upstreamCommit"] != record["commit"] ||
		record["lastSyncedAt"] != "1792285262771" {
		t.Fatalf("mirror fields = %v %v %v", record["mirrorOf"], record["upstreamCommit"], record["lastSyncedAt"])
	}
}
