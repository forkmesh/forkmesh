package mirrornode

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"net"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func freeAddress(t *testing.T) string {
	t.Helper()
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	address := listener.Addr().String()
	listener.Close()
	return address
}

func TestDaemonSupervisesSyncsRepinsAndReportsReadiness(t *testing.T) {
	source, bare := makeBareRepo(t)
	identity, keyPath := testIdentity(t)
	gatewayAddress, statusAddress := freeAddress(t), freeAddress(t)
	gatewayPath := filepath.Join(t.TempDir(), "gateway.json")
	generationMarker := gatewayGenerationMarkerPath(gatewayPath)
	script := filepath.Join(t.TempDir(), "fake_gateway.py")
	python := fmt.Sprintf(`import hashlib,pathlib,signal,socket,sys
config=pathlib.Path(%q);marker=pathlib.Path(%q)
def acknowledge(*_):
 marker.parent.mkdir(parents=True,exist_ok=True)
 marker.write_text(hashlib.sha256(config.read_bytes()).hexdigest()+"\n")
s=socket.socket();s.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1);s.bind(("127.0.0.1",%s));s.listen()
signal.signal(signal.SIGHUP,acknowledge)
signal.signal(signal.SIGTERM,lambda *_:sys.exit(0))
acknowledge()
while True:
 c,_=s.accept();c.close()
`, gatewayPath, generationMarker, gatewayAddress[len("127.0.0.1:"):])
	if err := os.WriteFile(script, []byte(python), 0700); err != nil {
		t.Fatal(err)
	}
	var gateway GatewayConfig
	gateway.SchemaVersion = 1
	gateway.Node.Name = "mirror9"
	gateway.Node.PublicKey = identity.PublicKey()
	gateway.RouterPublicKey = identity.PublicKey()
	for _, owner := range []string{"forkmesh", "mirror9"} {
		repo := Repository{Owner: owner, Name: "forkmesh", Visibility: "public", Enabled: true, GitDir: bare}
		repo.Integrity.ExpectedRefsSHA256 = "0"
		gateway.Repositories = append(gateway.Repositories, repo)
	}
	if _, err := writeGatewayConfig(gatewayPath, gateway); err != nil {
		t.Fatal(err)
	}
	servicePath := filepath.Join(t.TempDir(), "service.json")
	if err := os.WriteFile(servicePath, []byte("{}"), 0600); err != nil {
		t.Fatal(err)
	}
	t.Setenv("FORKMESH_MIRROR_NODE_CONFIG", servicePath)
	cfg := Config{SchemaVersion: 1, Listen: statusAddress, GatewayProbe: gatewayAddress, GatewayConfig: gatewayPath, GatewayScript: script, Python: "/usr/bin/python3", IdentityKey: keyPath, SyncInterval: Duration{5 * time.Second}, SyncTimeout: Duration{10 * time.Second}, Upstreams: map[string][]string{"mirror9/forkmesh": {source}}, DisableCloudflared: true, DisableCatalog: true, Version: "test"}
	if err := cfg.validate(); err != nil {
		t.Fatal(err)
	}
	daemon, err := NewDaemon(cfg)
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan error, 1)
	go func() { done <- daemon.Run(ctx, "/bin/true") }()
	client := &http.Client{Timeout: time.Second}
	var status Status
	deadline := time.Now().Add(10 * time.Second)
	for time.Now().Before(deadline) {
		response, err := client.Get("http://" + statusAddress + "/v1/status")
		if err == nil {
			_ = json.NewDecoder(response.Body).Decode(&status)
			response.Body.Close()
			if status.Ready {
				break
			}
		}
		time.Sleep(100 * time.Millisecond)
	}
	if !status.Ready || !status.LastSyncOK || status.Repositories["mirror9/forkmesh"] == "" || !status.Processes["gateway"] {
		t.Fatalf("not ready: %+v", status)
	}
	if status.Restarts["gateway"] != 0 {
		t.Fatalf("fresh gateway restarted during initial reconciliation: %+v", status.Restarts)
	}
	response, err := client.Get("http://" + statusAddress + "/healthz")
	if err != nil || response.StatusCode != http.StatusOK {
		t.Fatalf("health: %v %+v", err, response)
	}
	response.Body.Close()
	response, err = client.Get("http://" + statusAddress + "/v1/control/sync")
	if err != nil || response.StatusCode != http.StatusMethodNotAllowed {
		t.Fatalf("sync GET should be rejected: %v %+v", err, response)
	}
	response.Body.Close()
	oldState := status.Repositories["mirror9/forkmesh"]
	if err := os.WriteFile(
		filepath.Join(source, "hot-reload.txt"), []byte("two\n"), 0600,
	); err != nil {
		t.Fatal(err)
	}
	run(t, source, "git", "add", "hot-reload.txt")
	run(t, source, "git", "commit", "-m", "two")
	beforeSyncCount := status.SyncCount
	response, err = client.Post(
		"http://"+statusAddress+"/v1/control/sync", "application/json", nil)
	if err != nil || response.StatusCode != http.StatusAccepted {
		t.Fatalf("sync POST: %v %+v", err, response)
	}
	response.Body.Close()
	deadline = time.Now().Add(10 * time.Second)
	for time.Now().Before(deadline) {
		response, err = client.Get("http://" + statusAddress + "/v1/status")
		if err == nil {
			_ = json.NewDecoder(response.Body).Decode(&status)
			response.Body.Close()
			if status.SyncCount > beforeSyncCount &&
				status.Repositories["mirror9/forkmesh"] != oldState {
				break
			}
		}
		time.Sleep(50 * time.Millisecond)
	}
	if status.SyncCount <= beforeSyncCount || status.Runtime.Goroutines == 0 ||
		status.Runtime.HeapBytes == 0 ||
		status.Repositories["mirror9/forkmesh"] == oldState {
		t.Fatalf("manual sync/runtime stats not reported: %+v", status)
	}
	if status.Restarts["gateway"] != 0 ||
		!gatewayServesCurrentGeneration(gatewayPath) {
		t.Fatalf("gateway did not hot-reload continuously: %+v", status.Restarts)
	}
	loaded, _, err := loadGatewayConfig(gatewayPath)
	if err != nil {
		t.Fatal(err)
	}
	if loaded.Repositories[0].Integrity.ExpectedRefsSHA256 == "0" || loaded.HealthSignerCommand[0] != "/bin/true" {
		t.Fatalf("gateway not migrated: %+v", loaded)
	}
	cancel()
	select {
	case err := <-done:
		if err != nil {
			t.Fatal(err)
		}
	case <-time.After(10 * time.Second):
		t.Fatal("daemon did not stop")
	}
}

func TestTransientUpstreamFailureDoesNotWithdrawReadableRepository(t *testing.T) {
	_, bare := makeBareRepo(t)
	digest, err := refsSHA256(context.Background(), bare)
	if err != nil {
		t.Fatal(err)
	}
	repo := Repository{
		Owner: "mirror9", Name: "forkmesh", Visibility: "public",
		Enabled: true, GitDir: bare,
	}
	repo.Integrity.ExpectedRefsSHA256 = digest
	daemon := &Daemon{
		config: Config{
			DisableCatalog: true,
			SyncTimeout:    Duration{time.Second},
			Upstreams: map[string][]string{
				"mirror9/forkmesh": {filepath.Join(t.TempDir(), "unavailable.git")},
			},
		},
		gateway:    GatewayConfig{Repositories: []Repository{repo}},
		supervisor: NewSupervisor(),
		repoStates: map[string]string{},
	}

	daemon.syncOnce(context.Background())
	daemon.mu.RLock()
	defer daemon.mu.RUnlock()
	if !daemon.lastSyncOK {
		t.Fatalf("readable repository was withdrawn after refresh failure: %s",
			daemon.lastSyncErr)
	}
	if daemon.repoStates["mirror9/forkmesh"] != digest {
		t.Fatalf("readable state was lost: %v", daemon.repoStates)
	}
	if !strings.Contains(daemon.lastSyncErr, "all upstreams failed") {
		t.Fatalf("refresh warning was not retained: %q", daemon.lastSyncErr)
	}
}

func TestReloadGatewayGenerationWaitsForExactHotReloadMarker(t *testing.T) {
	// The fake gateway (gateway_reload_test.go) listens, acknowledges its
	// startup configuration, and hot-reloads on SIGHUP.
	f := startReloadGateway(t, "slow", 0)
	writeFakeGeneration(t, f.gatewayPath, "second generation")
	if gatewayServesCurrentGeneration(f.gatewayPath) {
		t.Fatal("a stale acknowledgement matched the new configuration")
	}
	if !f.daemon.reloadGatewayGeneration(context.Background()) {
		t.Fatal("daemon did not observe the gateway reload acknowledgement")
	}
	if !gatewayServesCurrentGeneration(f.gatewayPath) {
		t.Fatal("gateway acknowledged a different configuration generation")
	}
	running, restarts := f.supervisor.Snapshot()
	if !running["gateway"] || restarts["gateway"] != 0 {
		t.Fatalf("hot reload replaced the gateway: running=%v restarts=%v",
			running, restarts)
	}
}

func TestNewDaemonRejectsIdentityMismatchAndUnsupportedRepo(t *testing.T) {
	identity, key := testIdentity(t)
	other, _ := testIdentity(t)
	path := filepath.Join(t.TempDir(), "gateway.json")
	var gateway GatewayConfig
	gateway.SchemaVersion = 1
	gateway.Node.Name = "mirror9"
	gateway.Node.PublicKey = other.PublicKey()
	gateway.Repositories = []Repository{{Owner: "mirror9", Name: "x", Visibility: "public", Enabled: true, GitDir: "/tmp/x"}}
	if _, err := writeGatewayConfig(path, gateway); err != nil {
		t.Fatal(err)
	}
	cfg := Config{GatewayConfig: path, IdentityKey: key}
	if _, err := NewDaemon(cfg); err == nil {
		t.Fatal("identity mismatch accepted")
	}
	gateway.Node.PublicKey = identity.PublicKey()
	gateway.Repositories[0].Visibility = "private"
	if _, err := writeGatewayConfig(path, gateway); err != nil {
		t.Fatal(err)
	}
	if _, err := NewDaemon(cfg); err == nil {
		t.Fatal("private repository accepted")
	}
}

func TestNewDaemonRequestsInitialCollaborationCatchUp(t *testing.T) {
	identity, key := testIdentity(t)
	_, bare := makeBareRepo(t)
	path := filepath.Join(t.TempDir(), "gateway.json")
	var gateway GatewayConfig
	gateway.SchemaVersion = 1
	gateway.Node.Name = "mirror9"
	gateway.Node.PublicKey = identity.PublicKey()
	gateway.Repositories = []Repository{{
		Owner: "forkmesh", Name: "forkmesh", Visibility: "public",
		Enabled: true, GitDir: bare,
	}}
	if _, err := writeGatewayConfig(path, gateway); err != nil {
		t.Fatal(err)
	}
	daemon, err := NewDaemon(Config{
		GatewayConfig: path, IdentityKey: key,
		CatalogURL: "https://app.forkmesh.com/api/repositories",
	})
	if err != nil {
		t.Fatal(err)
	}
	if !daemon.collaborationCatchUp {
		t.Fatal("startup did not request collaboration catch-up")
	}
}

// Relay pushes drive both repository sync and native collaboration intake.
func TestDaemonNodeEventsTriggerSync(t *testing.T) {
	daemon := &Daemon{
		syncRequests:            make(chan struct{}, 1),
		collaborationRetryAt:    time.Now().Add(time.Hour),
		collaborationRetryDelay: controllerRetryCeil,
		catalogRetryAt:          time.Now().Add(time.Hour),
		catalogRetryDelay:       controllerRetryCeil,
	}
	target := strings.Repeat("a", 64)
	daemon.handleNodeEvent(Event{
		Topic: "commits", Repo: "forkmesh/forkmesh", State: target})
	select {
	case <-daemon.syncRequests:
	default:
		t.Fatal("commits event did not queue a sync")
	}
	if daemon.desiredStates["forkmesh/forkmesh"] != target {
		t.Fatal("commits event did not retain its target refs state")
	}
	if !daemon.catalogRetryAt.IsZero() || daemon.catalogRetryDelay != 0 {
		t.Fatal("live commit event did not bypass stale catalog backoff")
	}
	daemon.handleNodeEvent(Event{Topic: "issues", Repo: "forkmesh/forkmesh"})
	select {
	case <-daemon.syncRequests:
	default:
		t.Fatal("inbox event did not queue native intake")
	}
	_, pending := daemon.takeCollaborationRequests()
	if !pending["forkmesh/forkmesh"]["issues"] {
		t.Fatal("issue event did not retain its repository and inbox kind")
	}
	daemon.handleNodeEvent(Event{CatchUp: true})
	select {
	case <-daemon.syncRequests:
	default:
		t.Fatal("catch-up event did not queue a sync")
	}
	catchUp, _ := daemon.takeCollaborationRequests()
	if !catchUp {
		t.Fatal("catch-up did not request all native collaboration inboxes")
	}
}

func TestDaemonIgnoresOlderAndCompletedCommitReplays(t *testing.T) {
	queue := make(chan struct{}, 1)
	d := &Daemon{
		repoStates:         map[string]string{},
		desiredStates:      map[string]string{},
		desiredGenerations: map[string]int64{},
		syncRequests:       queue,
	}
	key := "forkmesh/forkmesh"
	newState := strings.Repeat("b", 64)
	d.handleNodeEvent(Event{
		Topic: "commits", Repo: key, State: newState, Generation: 200,
	})
	<-queue
	d.mu.Lock()
	d.repoStates[key] = newState
	d.mu.Unlock()
	if d.convergencePending() {
		t.Fatal("landed generation remained pending")
	}

	for _, event := range []Event{
		{Topic: "commits", Repo: key, State: strings.Repeat("a", 64), Generation: 100},
		{Topic: "commits", Repo: key, State: newState, Generation: 200},
	} {
		d.handleNodeEvent(event)
		select {
		case <-queue:
			t.Fatalf("stale replay queued a sync: %+v", event)
		default:
		}
	}
	if _, pending := d.desiredStates[key]; pending {
		t.Fatal("stale replay restored a completed target")
	}

	sameTickState := strings.Repeat("d", 64)
	d.handleNodeEvent(Event{
		Topic: "commits", Repo: key, State: sameTickState, Generation: 200,
	})
	select {
	case <-queue:
	default:
		t.Fatal("distinct state in the same generation did not queue a sync")
	}
	if d.desiredStates[key] != sameTickState {
		t.Fatal("distinct same-generation target was not retained")
	}

	latest := strings.Repeat("c", 64)
	d.handleNodeEvent(Event{
		Topic: "commits", Repo: key, State: latest, Generation: 300,
	})
	select {
	case <-queue:
	default:
		t.Fatal("newer replay did not queue a sync")
	}
	if d.desiredStates[key] != latest || d.desiredGenerations[key] != 300 {
		t.Fatal("newest generation was not retained")
	}
}

func TestControllerRetryBackoffRetainsWorkWithoutPollingEverySync(t *testing.T) {
	now := time.Unix(2_000_000_000, 0)
	daemon := &Daemon{
		collaborationCatchUp: true,
		collaborationPending: map[string]map[string]bool{},
	}
	catchUp, _ := daemon.takeCollaborationRequestsAt(now)
	if !catchUp {
		t.Fatal("initial collaboration catch-up was not due")
	}
	daemon.requeueCollaboration("forkmesh/forkmesh", "issues")
	daemon.finishCollaborationAttempt(true, now)
	if retry := daemon.collaborationRetryDelay; retry != controllerRetryFloor {
		t.Fatalf("first collaboration retry = %s, want %s", retry,
			controllerRetryFloor)
	}
	if catchUp, pending := daemon.takeCollaborationRequestsAt(
		now.Add(controllerRetryFloor / 2)); catchUp || len(pending) != 0 {
		t.Fatal("failed collaboration work was consumed before its backoff")
	}
	daemon.mu.RLock()
	retained := daemon.collaborationPending["forkmesh/forkmesh"]["issues"]
	daemon.mu.RUnlock()
	if !retained {
		t.Fatal("backoff discarded the pending collaboration inbox")
	}
	_, pending := daemon.takeCollaborationRequestsAt(
		now.Add(controllerRetryFloor))
	if !pending["forkmesh/forkmesh"]["issues"] {
		t.Fatal("pending collaboration work was not released when retry became due")
	}
	daemon.requeueCollaboration("forkmesh/forkmesh", "issues")
	daemon.finishCollaborationAttempt(true, now.Add(controllerRetryFloor))
	if retry := daemon.collaborationRetryDelay; retry != 2*controllerRetryFloor {
		t.Fatalf("second collaboration retry = %s, want %s", retry,
			2*controllerRetryFloor)
	}

	daemon.finishCatalogAttempt(true, now)
	if daemon.catalogAttemptDue(now.Add(controllerRetryFloor / 2)) {
		t.Fatal("catalog publication ignored its failure backoff")
	}
	if !daemon.catalogAttemptDue(now.Add(controllerRetryFloor)) {
		t.Fatal("catalog publication did not become due after backoff")
	}
}

func TestManualSyncRequestsCollaborationCatchUp(t *testing.T) {
	daemon := &Daemon{syncRequests: make(chan struct{}, 1)}
	request := httptest.NewRequest(http.MethodPost, "/v1/control/sync", nil)
	response := httptest.NewRecorder()
	daemon.handleSync(response, request)
	if response.Code != http.StatusAccepted {
		t.Fatalf("manual sync status: %d", response.Code)
	}
	catchUp, _ := daemon.takeCollaborationRequests()
	if !catchUp {
		t.Fatal("manual sync did not request collaboration catch-up")
	}
}

func TestLocalReceivePublishesBeforeUpstreamReconciliation(t *testing.T) {
	d := &Daemon{
		syncRequests:    make(chan struct{}, 1),
		publishRequests: make(chan struct{}, 1),
	}
	request := httptest.NewRequest(
		http.MethodPost, "/v1/control/sync?local=1", nil)
	response := httptest.NewRecorder()
	d.handleSync(response, request)
	if response.Code != http.StatusAccepted {
		t.Fatalf("local receive status: %d", response.Code)
	}
	select {
	case <-d.publishRequests:
	default:
		t.Fatal("local receive did not queue an exact-generation publish")
	}
	select {
	case <-d.syncRequests:
		t.Fatal("local receive fetched an upstream before publishing")
	default:
	}
}

func TestStatusServerRejectsCrossOriginAndReboundRequests(t *testing.T) {
	called := false
	handler := localRequestOnly(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		called = true
		w.WriteHeader(http.StatusNoContent)
	}))

	for _, test := range []struct {
		name, host, origin, fetchSite string
	}{
		{"dns rebinding host", "attacker.example:8791", "", ""},
		{"cross-origin form", "127.0.0.1:8791", "https://attacker.example", "cross-site"},
		{"opaque browser origin", "localhost:8791", "null", "cross-site"},
	} {
		t.Run(test.name, func(t *testing.T) {
			called = false
			request := httptest.NewRequest(http.MethodPost, "http://"+test.host+"/v1/control/sync", nil)
			request.Host = test.host
			if test.origin != "" {
				request.Header.Set("Origin", test.origin)
			}
			if test.fetchSite != "" {
				request.Header.Set("Sec-Fetch-Site", test.fetchSite)
			}
			response := httptest.NewRecorder()
			handler.ServeHTTP(response, request)
			if response.Code != http.StatusForbidden || called {
				t.Fatalf("request was not rejected: status=%d called=%v", response.Code, called)
			}
		})
	}

	request := httptest.NewRequest(http.MethodGet, "http://[::1]:8791/v1/status", nil)
	request.Host = "[::1]:8791"
	request.Header.Set("Origin", "http://localhost:3000")
	response := httptest.NewRecorder()
	handler.ServeHTTP(response, request)
	if response.Code != http.StatusNoContent || !called {
		t.Fatalf("local request was rejected: status=%d", response.Code)
	}
}

// The relay verifies an endpoint registration by fetching the node's manifest
// and health through the public origin. A gateway that the supervisor has just
// (re)started is "running" before it listens, and registering in that window
// earned every configuration reload an invalid_manifest / unreachable_mirror_url
// rejection on 2026-09-09/10.
func TestEndpointLeaseWaitsForListeningGateway(t *testing.T) {
	running := map[string]bool{"gateway": true, "cloudflared": true}
	if endpointLeaseAllowed(running, false, false) {
		t.Fatal("a supervised but not yet listening gateway must not register")
	}
	if !endpointLeaseAllowed(running, false, true) {
		t.Fatal("a listening gateway behind a running tunnel must register")
	}
	if endpointLeaseAllowed(map[string]bool{"gateway": true}, false, true) {
		t.Fatal("a tunnel origin must wait for cloudflared")
	}
	if !endpointLeaseAllowed(map[string]bool{"gateway": true}, true, true) {
		t.Fatal("a direct origin needs no tunnel")
	}
	if endpointLeaseAllowed(map[string]bool{}, true, true) {
		t.Fatal("a stopped gateway must not register even if something listens")
	}
}

func TestGatewayListeningProbesTheLoopbackListener(t *testing.T) {
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	daemon := &Daemon{config: Config{GatewayProbe: listener.Addr().String()}}
	if !daemon.gatewayListening() {
		t.Fatal("an open loopback listener must count as listening")
	}
	listener.Close()
	if daemon.gatewayListening() {
		t.Fatal("a closed listener must not count as listening")
	}
}

func TestDirtyEndpointStateQueuesOnePublishOnlyRetry(t *testing.T) {
	previous := endpointStateRetryDelay
	endpointStateRetryDelay = 10 * time.Millisecond
	t.Cleanup(func() { endpointStateRetryDelay = previous })

	daemon := &Daemon{
		syncRequests:    make(chan struct{}, 1),
		publishRequests: make(chan struct{}, 1),
	}
	daemon.markEndpointStateDirty()
	daemon.markEndpointStateDirty()

	select {
	case <-daemon.publishRequests:
	case <-time.After(time.Second):
		t.Fatal("changed endpoint state did not queue a publish-only retry")
	}
	daemon.mu.RLock()
	dirty, retry := daemon.endpointDirty, daemon.endpointRetry
	daemon.mu.RUnlock()
	if !dirty || retry {
		t.Fatalf("unexpected endpoint retry state: dirty=%v retry=%v", dirty, retry)
	}
	select {
	case <-daemon.publishRequests:
		t.Fatal("duplicate dirty notifications queued more than one retry")
	case <-time.After(30 * time.Millisecond):
	}
	select {
	case <-daemon.syncRequests:
		t.Fatal("endpoint retry unnecessarily queued a repository fetch")
	default:
	}
}

// Relay-hosted repositories use pushes for speed and one fixed reconciliation
// cadence for a notification the best-effort fan-out could not deliver -- a
// faster operator interval must not turn the fleet into a poll storm against
// the relay's ingress object. Third-party upstreams have no push source and
// retain the configured cadence.
func TestFetchInterval(t *testing.T) {
	daemon := &Daemon{config: Config{
		CatalogURL:   "https://app.forkmesh.com/api/repositories",
		SyncInterval: Duration{30 * time.Second},
		Upstreams: map[string][]string{
			"mirror9/forkmesh": {"https://forkmesh.com/forkmesh/forkmesh"},
		},
	}, events: &EventSocket{}}
	if got := daemon.fetchInterval(); got != relaySafetySyncInterval {
		t.Fatalf("relay safety interval = %s, want %s", got, relaySafetySyncInterval)
	}
	daemon.config.Upstreams["c/d"] = []string{"https://github.com/c/d.git"}
	if got := daemon.fetchInterval(); got != 30*time.Second {
		t.Fatalf("external fetch interval = %s, want 30s", got)
	}
	// A direct managed peer is external by hostname but still uses WebSocket
	// commit generations when it is explicitly covered by relay events.
	daemon.config.Upstreams = map[string][]string{
		"mirror9/forkmesh": {"https://mirror10.forkmesh.com/forkmesh/forkmesh.git"},
	}
	daemon.config.EventDrivenUpstreams = []string{
		"https://mirror10.forkmesh.com/forkmesh/forkmesh.git",
	}
	if got := daemon.fetchInterval(); got != relaySafetySyncInterval {
		t.Fatalf("event-driven peer safety interval = %s, want %s", got,
			relaySafetySyncInterval)
	}
	withoutEvents := &Daemon{config: Config{
		SyncInterval: Duration{45 * time.Second},
	}}
	if got := withoutEvents.fetchInterval(); got != 45*time.Second {
		t.Fatalf("eventless fetch interval = %s, want 45s", got)
	}
	daemon.config.Upstreams = map[string][]string{
		"mirror9/forkmesh": {"https://forkmesh.com/forkmesh/forkmesh"},
	}
	daemon.config.DisableCatalog = true
	if got := daemon.fetchInterval(); got != 30*time.Second {
		t.Fatalf("catalog-disabled fetch interval = %s, want 30s", got)
	}
	daemon.config.DisableCatalog = false
	// A slow operator interval cannot leave relay mirrors stale for that whole
	// window after one notification is lost.
	daemon.config.SyncInterval = Duration{20 * time.Minute}
	if got := daemon.fetchInterval(); got != relaySafetySyncInterval {
		t.Fatalf("slow relay safety interval = %s, want %s", got,
			relaySafetySyncInterval)
	}
	// Nor can a fast one poll the relay more often than the safety cadence.
	daemon.config.SyncInterval = Duration{5 * time.Second}
	if got := daemon.fetchInterval(); got != relaySafetySyncInterval {
		t.Fatalf("fast relay safety interval = %s, want %s", got,
			relaySafetySyncInterval)
	}
}

// A pending endpoint state retries on a backoff that stops short of the lease
// heartbeat, and a successful lease resets it; the retry must never collapse
// back into a fixed seven-second loop.
func TestEndpointStateRetryBacksOff(t *testing.T) {
	daemon := &Daemon{syncRequests: make(chan struct{}, 1)}
	want := []time.Duration{
		endpointStateRetryDelay, 2 * endpointStateRetryDelay,
		4 * endpointStateRetryDelay,
	}
	for i, expected := range want {
		daemon.markEndpointStateDirty()
		daemon.mu.Lock()
		got := daemon.endpointRetryDelay
		daemon.endpointRetry = false
		daemon.mu.Unlock()
		if got != expected {
			t.Fatalf("retry %d delay = %s, want %s", i, got, expected)
		}
	}
	daemon.mu.Lock()
	daemon.endpointRetryDelay = endpointStateRetryCeil
	daemon.endpointRetry = false
	daemon.mu.Unlock()
	daemon.markEndpointStateDirty()
	daemon.mu.Lock()
	capped := daemon.endpointRetryDelay
	daemon.mu.Unlock()
	if capped != endpointStateRetryCeil {
		t.Fatalf("capped retry delay = %s, want %s", capped, endpointStateRetryCeil)
	}
}

func TestEndpointStateRetryRequestsPublishWithoutRepositoryFetch(t *testing.T) {
	floor, ceiling := endpointStateRetryDelay, endpointStateRetryCeil
	endpointStateRetryDelay, endpointStateRetryCeil =
		5*time.Millisecond, 10*time.Millisecond
	t.Cleanup(func() {
		endpointStateRetryDelay, endpointStateRetryCeil = floor, ceiling
	})
	daemon := &Daemon{
		syncRequests:    make(chan struct{}, 1),
		publishRequests: make(chan struct{}, 1),
	}
	daemon.markEndpointStateDirty()
	select {
	case <-daemon.publishRequests:
	case <-time.After(time.Second):
		t.Fatal("endpoint retry did not request a publish-only cycle")
	}
	select {
	case <-daemon.syncRequests:
		t.Fatal("endpoint retry requested a repository fetch")
	default:
	}
}

func TestPublishCycleDoesNotConsumeCollaborationWork(t *testing.T) {
	daemon := &Daemon{
		config: Config{
			DisableCatalog: true,
			SyncTimeout:    Duration{time.Second},
		},
		supervisor: NewSupervisor(),
		collaborationPending: map[string]map[string]bool{
			"alice/project": {"issues": true},
		},
		collaborationCatchUp: true,
		repoStates:           map[string]string{},
	}
	daemon.publishOnce(context.Background())
	daemon.mu.RLock()
	defer daemon.mu.RUnlock()
	if !daemon.collaborationCatchUp ||
		!daemon.collaborationPending["alice/project"]["issues"] {
		t.Fatal("publish-only cycle consumed pending collaboration work")
	}
}

// A live event socket does not make its best-effort delivery durable. Prove the
// slow safety pass notices a source update even when no commit event arrives.
func TestSyncLoopReconcilesAfterDroppedRelayEvent(t *testing.T) {
	work, upstream := makeBareRepo(t)
	gitDir := filepath.Join(t.TempDir(), "repositories", "forkmesh.git")
	gatewayPath := filepath.Join(t.TempDir(), "gateway.json")
	repo := Repository{Owner: "mirror9", Name: "forkmesh", Visibility: "public", Enabled: true, GitDir: gitDir}
	daemon := &Daemon{
		config: Config{
			// The fixture uses a local bare repository rather than an HTTP
			// relay, so it correctly follows the configured external-upstream
			// cadence. TestFetchInterval separately proves relay upstreams use
			// the fixed sparse safety interval.
			SyncInterval:  Duration{20 * time.Millisecond},
			SyncTimeout:   Duration{30 * time.Second},
			GatewayConfig: gatewayPath,
			CatalogURL:    "https://app.forkmesh.com/api/repositories",
			PublishOwner:  "not-mirror9",
			Upstreams: map[string][]string{
				"mirror9/forkmesh": {upstream},
			},
		},
		supervisor:   NewSupervisor(),
		events:       &EventSocket{},
		syncRequests: make(chan struct{}, 1),
	}
	daemon.gateway.Repositories = []Repository{repo}
	if _, err := writeGatewayConfig(gatewayPath, daemon.gateway); err != nil {
		t.Fatal(err)
	}
	daemon.syncOnce(context.Background())

	before, err := refsSHA256(context.Background(), gitDir)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(work, "after-event-loss.txt"), []byte("new refs\n"), 0600); err != nil {
		t.Fatal(err)
	}
	run(t, work, "git", "add", "after-event-loss.txt")
	run(t, work, "git", "commit", "-m", "advance source without event")
	run(t, work, "git", "push", upstream, "HEAD:main")

	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan struct{})
	go func() {
		daemon.syncLoop(ctx)
		close(done)
	}()
	defer func() {
		cancel()
		<-done
	}()
	deadline := time.Now().Add(15 * time.Second)
	for time.Now().Before(deadline) {
		after, digestErr := refsSHA256(context.Background(), gitDir)
		daemon.mu.RLock()
		cycles := daemon.syncCount
		daemon.mu.RUnlock()
		if digestErr == nil && after != before && cycles >= 2 {
			return
		}
		time.Sleep(20 * time.Millisecond)
	}
	t.Fatal("relay safety pass did not reconcile refs after a dropped event")
}

// The reconciliation pass is only a backstop: a commits frame must wake the
// fetch loop immediately, independent of that timer's long interval.
func TestSyncLoopFetchesImmediatelyOnCommitEvent(t *testing.T) {
	work, upstream := makeBareRepo(t)
	gitDir := filepath.Join(t.TempDir(), "repositories", "forkmesh.git")
	gatewayPath := filepath.Join(t.TempDir(), "gateway.json")
	repo := Repository{Owner: "mirror9", Name: "forkmesh", Visibility: "public", Enabled: true, GitDir: gitDir}
	daemon := &Daemon{
		config: Config{
			SyncInterval:  Duration{time.Hour},
			SyncTimeout:   Duration{30 * time.Second},
			GatewayConfig: gatewayPath,
			CatalogURL:    "https://app.forkmesh.com/api/repositories",
			PublishOwner:  "not-mirror9",
			Upstreams: map[string][]string{
				"mirror9/forkmesh": {upstream},
			},
		},
		supervisor:   NewSupervisor(),
		events:       &EventSocket{},
		syncRequests: make(chan struct{}, 1),
	}
	daemon.gateway.Repositories = []Repository{repo}
	if _, err := writeGatewayConfig(gatewayPath, daemon.gateway); err != nil {
		t.Fatal(err)
	}
	daemon.syncOnce(context.Background())
	before, err := refsSHA256(context.Background(), gitDir)
	if err != nil {
		t.Fatal(err)
	}

	if err := os.WriteFile(filepath.Join(work, "pushed.txt"), []byte("instant refs\n"), 0600); err != nil {
		t.Fatal(err)
	}
	run(t, work, "git", "add", "pushed.txt")
	run(t, work, "git", "commit", "-m", "push immediate mirror update")
	run(t, work, "git", "push", upstream, "HEAD:main")

	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan struct{})
	go func() {
		daemon.syncLoop(ctx)
		close(done)
	}()
	defer func() {
		cancel()
		<-done
	}()
	daemon.handleNodeEvent(Event{Topic: "commits", Repo: "forkmesh/forkmesh"})
	deadline := time.Now().Add(15 * time.Second)
	for time.Now().Before(deadline) {
		after, digestErr := refsSHA256(context.Background(), gitDir)
		daemon.mu.RLock()
		cycles := daemon.syncCount
		daemon.mu.RUnlock()
		if digestErr == nil && after != before && cycles >= 2 {
			return
		}
		time.Sleep(20 * time.Millisecond)
	}
	t.Fatal("commit event did not immediately reconcile mirror refs")
}

// A round-robin Git URL can answer successfully from a peer that has not seen
// the source's new head yet. The signed target state in the commit event keeps
// that successful no-op from ending the convergence attempt.
func TestSyncLoopRetriesStalePeerUntilTargetStateLands(t *testing.T) {
	floor, ceil := convergenceRetryFloor, convergenceRetryCeil
	convergenceRetryFloor, convergenceRetryCeil =
		20*time.Millisecond, 40*time.Millisecond
	t.Cleanup(func() {
		convergenceRetryFloor, convergenceRetryCeil = floor, ceil
	})

	work, upstream := makeBareRepo(t)
	gitDir := filepath.Join(t.TempDir(), "repositories", "forkmesh.git")
	gatewayPath := filepath.Join(t.TempDir(), "gateway.json")
	repo := Repository{
		Owner: "mirror9", Name: "forkmesh", Visibility: "public",
		Enabled: true, GitDir: gitDir,
	}
	daemon := &Daemon{
		config: Config{
			SyncInterval:  Duration{time.Hour},
			SyncTimeout:   Duration{30 * time.Second},
			GatewayConfig: gatewayPath,
			CatalogURL:    "https://app.forkmesh.com/api/repositories",
			PublishOwner:  "not-mirror9",
			Upstreams: map[string][]string{
				"mirror9/forkmesh": {upstream},
			},
		},
		supervisor:    NewSupervisor(),
		events:        &EventSocket{},
		repoStates:    map[string]string{},
		desiredStates: map[string]string{},
		syncRequests:  make(chan struct{}, 1),
	}
	daemon.gateway.Repositories = []Repository{repo}
	if _, err := writeGatewayConfig(gatewayPath, daemon.gateway); err != nil {
		t.Fatal(err)
	}
	daemon.syncOnce(context.Background())

	if err := os.WriteFile(
		filepath.Join(work, "target-state.txt"), []byte("target\n"), 0600,
	); err != nil {
		t.Fatal(err)
	}
	run(t, work, "git", "add", "target-state.txt")
	run(t, work, "git", "commit", "-m", "target state")
	target, err := refsSHA256(context.Background(), filepath.Join(work, ".git"))
	if err != nil {
		t.Fatal(err)
	}

	beforeCycles := daemon.syncCount
	daemon.handleNodeEvent(Event{
		Topic: "commits", Repo: "mirror9/forkmesh", State: target})
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan struct{})
	go func() {
		daemon.syncLoop(ctx)
		close(done)
	}()
	defer func() {
		cancel()
		<-done
	}()

	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		daemon.mu.RLock()
		cycles := daemon.syncCount
		daemon.mu.RUnlock()
		if cycles > beforeCycles {
			break // the event-driven fetch succeeded against the stale peer
		}
		time.Sleep(10 * time.Millisecond)
	}
	run(t, work, "git", "push", upstream, "HEAD:main")

	deadline = time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		daemon.mu.RLock()
		state := daemon.repoStates["mirror9/forkmesh"]
		pending := len(daemon.desiredStates)
		daemon.mu.RUnlock()
		if state == target && pending == 0 {
			return
		}
		time.Sleep(10 * time.Millisecond)
	}
	t.Fatal("target-state retry did not converge after the stale peer advanced")
}

// A first clone that fails leaves the node with nothing to serve: the gateway
// refuses a repository whose refs it cannot read, so the node never publishes
// and never appears in the catalog. Relay pushes cannot rescue it — they only
// arrive when a source moves a head — so the bootstrap retry has to keep trying
// the clone on its own until one lands.
func TestSyncLoopRetriesAFailedFirstClone(t *testing.T) {
	floor, ceil := bootstrapRetryFloor, bootstrapRetryCeil
	bootstrapRetryFloor, bootstrapRetryCeil = 20*time.Millisecond, 40*time.Millisecond
	t.Cleanup(func() { bootstrapRetryFloor, bootstrapRetryCeil = floor, ceil })

	_, bare := makeBareRepo(t)
	upstream := filepath.Join(t.TempDir(), "upstream.git")
	gitDir := filepath.Join(t.TempDir(), "repositories", "forkmesh-forkmesh.git")
	repo := Repository{Owner: "forkmesh", Name: "forkmesh", Visibility: "public", Enabled: true, GitDir: gitDir}
	daemon := &Daemon{
		config: Config{
			SyncInterval:   Duration{time.Hour},
			SyncTimeout:    Duration{30 * time.Second},
			CatalogURL:     "https://app.forkmesh.com/api/repositories",
			DisableCatalog: true,
			Upstreams:      map[string][]string{"forkmesh/forkmesh": {upstream}},
		},
		supervisor:   NewSupervisor(),
		events:       &EventSocket{},
		syncRequests: make(chan struct{}, 1),
	}
	daemon.gateway.Repositories = []Repository{repo}

	// The upstream does not exist yet, so this first sync cannot clone.
	daemon.syncOnce(context.Background())
	if !daemon.bootstrapPending() {
		t.Fatal("a repository with no local clone must report bootstrap pending")
	}
	if _, err := os.Stat(gitDir); !errors.Is(err, os.ErrNotExist) {
		t.Fatalf("clone unexpectedly present: %v", err)
	}

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	go daemon.syncLoop(ctx)

	// Publish the upstream after the loop is already running: only the
	// bootstrap retry can notice, since no push and no ticker will fire.
	if err := os.Rename(bare, upstream); err != nil {
		t.Fatal(err)
	}
	deadline := time.Now().Add(15 * time.Second)
	for time.Now().Before(deadline) {
		if _, err := os.Stat(gitDir); err == nil {
			break
		}
		time.Sleep(20 * time.Millisecond)
	}
	if _, err := os.Stat(gitDir); err != nil {
		t.Fatalf("bootstrap retry never cloned the repository: %v", err)
	}
	deadline = time.Now().Add(15 * time.Second)
	for time.Now().Before(deadline) && daemon.bootstrapPending() {
		time.Sleep(20 * time.Millisecond)
	}
	if daemon.bootstrapPending() {
		t.Fatal("bootstrap stayed pending after the clone landed")
	}
}

func TestCycleSkipsUnchangedCatalogPublishes(t *testing.T) {
	_, bare := makeBareRepo(t)
	identity, _ := testIdentity(t)
	var posts int
	server := httptest.NewServer(http.HandlerFunc(
		func(w http.ResponseWriter, r *http.Request) {
			posts++
			w.WriteHeader(http.StatusCreated)
			_, _ = w.Write([]byte(`{"ok":true}`))
		}))
	defer server.Close()

	gatewayPath := filepath.Join(t.TempDir(), "gateway.json")
	releaseStore := filepath.Join(t.TempDir(), "releases")
	var gateway GatewayConfig
	gateway.SchemaVersion = 1
	gateway.Node.Name = "mirror9"
	gateway.Node.PublicKey = identity.PublicKey()
	gateway.RouterPublicKey = identity.PublicKey()
	repo := Repository{Owner: "mirror9", Name: "forkmesh", Visibility: "public", Enabled: true, GitDir: bare, ReleaseStore: releaseStore}
	repo.Integrity.ExpectedRefsSHA256 = "0"
	gateway.Repositories = append(gateway.Repositories, repo)
	if _, err := writeGatewayConfig(gatewayPath, gateway); err != nil {
		t.Fatal(err)
	}
	daemon := &Daemon{
		config: Config{
			GatewayConfig: gatewayPath,
			CatalogURL:    server.URL,
			PublishOwner:  "mirror9",
			SyncTimeout:   Duration{10 * time.Second},
			Version:       "test",
		},
		gateway:    gateway,
		identity:   identity,
		supervisor: NewSupervisor(),
		repoStates: map[string]string{},
	}

	// First heartbeat publishes; the identical second one must not POST again
	// while the republish lease is fresh.
	daemon.publishOnce(context.Background())
	if posts != 1 {
		t.Fatalf("first cycle POSTs = %d, want 1", posts)
	}
	daemon.publishOnce(context.Background())
	if posts != 1 {
		t.Fatalf("unchanged cycle re-POSTed: %d", posts)
	}

	// CAS availability is independently mutable state. Restoring an artifact
	// without moving a Git ref must refresh the signed catalog count promptly.
	digest := strings.Repeat("a", 64)
	dataPath := filepath.Join(releaseStore, "sha256", "aa", digest, "data")
	if err := os.MkdirAll(filepath.Dir(dataPath), 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(dataPath, []byte("release"), 0600); err != nil {
		t.Fatal(err)
	}
	daemon.publishOnce(context.Background())
	if posts != 2 {
		t.Fatalf("artifact availability change did not publish: %d", posts)
	}

	// An expired lease renews even with nothing changed.
	key := repositoryKey(daemon.gateway.Repositories[0])
	state := daemon.catalogPublished[key]
	state.at = time.Now().Add(-catalogRepublishInterval - time.Second)
	daemon.catalogPublished[key] = state
	daemon.publishOnce(context.Background())
	if posts != 3 {
		t.Fatalf("expired lease did not renew: %d", posts)
	}

	// A fingerprint change (here: the daemon version, in production usually
	// the refs digest) publishes immediately.
	daemon.config.Version = "test2"
	daemon.publishOnce(context.Background())
	if posts != 4 {
		t.Fatalf("changed fingerprint did not publish: %d", posts)
	}
	daemon.publishOnce(context.Background())
	if posts != 4 {
		t.Fatalf("post-change unchanged cycle re-POSTed: %d", posts)
	}
}

func TestCatalogFailureDoesNotPostEveryGitSafetyCycle(t *testing.T) {
	_, bare := makeBareRepo(t)
	identity, _ := testIdentity(t)
	var posts int
	server := httptest.NewServer(http.HandlerFunc(
		func(w http.ResponseWriter, _ *http.Request) {
			posts++
			w.WriteHeader(http.StatusTooManyRequests)
			_, _ = w.Write([]byte(`{"error":"quota"}`))
		}))
	defer server.Close()

	gatewayPath := filepath.Join(t.TempDir(), "gateway.json")
	var gateway GatewayConfig
	gateway.SchemaVersion = 1
	gateway.Node.Name = "mirror9"
	gateway.Node.PublicKey = identity.PublicKey()
	gateway.RouterPublicKey = identity.PublicKey()
	repo := Repository{Owner: "mirror9", Name: "forkmesh", Visibility: "public", Enabled: true, GitDir: bare}
	repo.Integrity.ExpectedRefsSHA256 = "0"
	gateway.Repositories = append(gateway.Repositories, repo)
	if _, err := writeGatewayConfig(gatewayPath, gateway); err != nil {
		t.Fatal(err)
	}
	daemon := &Daemon{
		config: Config{
			GatewayConfig: gatewayPath,
			CatalogURL:    server.URL,
			PublishOwner:  "mirror9",
			SyncTimeout:   Duration{10 * time.Second},
			Version:       "test",
		},
		gateway:    gateway,
		identity:   identity,
		supervisor: NewSupervisor(),
		repoStates: map[string]string{},
	}

	daemon.publishOnce(context.Background())
	daemon.publishOnce(context.Background())
	if posts != 1 {
		t.Fatalf("failed catalog publish POSTed on every safety cycle: %d", posts)
	}
	daemon.mu.Lock()
	daemon.catalogRetryAt = time.Now().Add(-time.Second)
	daemon.mu.Unlock()
	daemon.publishOnce(context.Background())
	if posts != 2 {
		t.Fatalf("due catalog retry POSTs = %d, want 2", posts)
	}
}

func TestCollaborationCatchUpDrainsEachRepositoryOnce(t *testing.T) {
	daemon := &Daemon{
		config: Config{PublishOwner: "node1"},
		gateway: GatewayConfig{Repositories: []Repository{
			{Owner: "node1", Name: "platform", GitDir: "/srv/platform.git"},
			{Owner: "acme", Name: "platform", GitDir: "/srv/platform.git"},
			{Owner: "alice", Name: "Widgets", GitDir: "/srv/widgets.git"},
			{Owner: "acme", Name: "Widgets", GitDir: "/srv/widgets.git"},
			{Owner: "node1", Name: "solo", GitDir: "/srv/solo.git"},
		}},
	}
	var drained []string
	for _, repo := range daemon.gateway.Repositories {
		if daemon.preferredCollaborationAlias(repo) {
			drained = append(drained, repositoryKey(repo))
		}
	}
	want := []string{"acme/platform", "alice/widgets", "node1/solo"}
	if strings.Join(drained, ",") != strings.Join(want, ",") {
		t.Fatalf("catch-up drained %v, want %v", drained, want)
	}
}
