package mirrornode

import (
	"context"
	"crypto/ed25519"
	"encoding/base64"
	"errors"
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"
)

// pathRecorder logs every request path before handing it to next.
type pathRecorder struct {
	mu    sync.Mutex
	paths []string
	next  http.Handler
}

func (p *pathRecorder) ServeHTTP(w http.ResponseWriter, request *http.Request) {
	p.mu.Lock()
	p.paths = append(p.paths, request.URL.Path)
	p.mu.Unlock()
	p.next.ServeHTTP(w, request)
}

func (p *pathRecorder) snapshot() []string {
	p.mu.Lock()
	defer p.mu.Unlock()
	return append([]string(nil), p.paths...)
}

// A pool runner has no node account: it must never open /api/nodes/events or
// make catalog, endpoint-lease, fork-job, intake or reclaim calls, even when a
// catalogUrl is left in its config. It talks only to /api/runners/*.
func TestRunnerStartsNoNodeEventChannelAndNoNodeAccountCalls(t *testing.T) {
	installFakeClaude(t)
	identity, key := testIdentity(t)
	public, _ := base64.RawURLEncoding.DecodeString(identity.PublicKey())
	app := &fakeRunnerApp{t: t, public: ed25519.PublicKey(public)}
	recorder := &pathRecorder{next: app}
	server := httptest.NewServer(recorder)
	t.Cleanup(server.Close)
	cfg := Config{Role: RoleRunner, GatewayConfig: writeRoleGateway(t, identity, nil),
		IdentityKey: key, Listen: freeAddress(t), CatalogURL: server.URL + "/api/repositories",
		Runner: &RunnerConfig{AppURL: server.URL, WorkRoot: filepath.Join(t.TempDir(), "work"),
			MaxConcurrentRuns: 1, JobMemoryMax: "700M", JobCPUQuota: "90%"}}
	if err := cfg.applyRole(); err != nil {
		t.Fatal(err)
	}
	daemon, err := NewDaemon(cfg)
	if err != nil {
		t.Fatal(err)
	}
	if daemon.events != nil || daemon.terminals != nil {
		t.Fatal("runner daemon built a node event channel")
	}
	daemon.runnerWait = func(int) time.Duration { return 20 * time.Millisecond }
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan error, 1)
	go func() { done <- daemon.Run(ctx, "/usr/bin/true") }()
	deadline := time.Now().Add(10 * time.Second)
	for time.Now().Before(deadline) {
		if claims, _ := app.snapshot(); len(claims) >= 3 {
			break
		}
		time.Sleep(20 * time.Millisecond)
	}
	// Leave time for anything else the daemon might start.
	time.Sleep(200 * time.Millisecond)
	cancel()
	if err := <-done; err != nil {
		t.Fatal(err)
	}
	paths := recorder.snapshot()
	if claims, _ := app.snapshot(); len(claims) < 3 {
		t.Fatalf("runner polled claims %d times; paths %v", len(claims), paths)
	}
	for _, path := range paths {
		if !strings.HasPrefix(path, "/api/runners/") {
			t.Fatalf("runner called %s (all paths %v)", path, paths)
		}
	}
}

func TestRunnerPollDelayJittersAndBacksOff(t *testing.T) {
	for _, tc := range []struct {
		failures int
		base     time.Duration
	}{
		{0, runnerIdlePoll}, {1, 30 * time.Second}, {2, time.Minute}, {3, 2 * time.Minute},
		{4, 4 * time.Minute}, {5, runnerErrorCeil}, {50, runnerErrorCeil},
	} {
		low := runnerPollDelay(tc.failures, 0)
		mid := runnerPollDelay(tc.failures, 0.5)
		high := runnerPollDelay(tc.failures, 0.999999)
		if mid != tc.base || low != tc.base*3/4 || high > tc.base*5/4 || high < tc.base*5/4-time.Millisecond {
			t.Fatalf("failures=%d: delays %v/%v/%v, want %v ±25%%", tc.failures, low, mid, high, tc.base)
		}
	}
	if lo, hi := runnerPollDelay(0, 0), runnerPollDelay(0, 0.999999); lo < 15*time.Second || hi > 25*time.Second {
		t.Fatalf("idle poll %v..%v is outside 15-25 s", lo, hi)
	}
}

// The loop counts consecutive failures for the backoff, resets on success,
// and a finished job (requestSync) re-polls at once instead of waiting.
func TestRunnerLoopBacksOffOnErrorsAndRepollsWhenWoken(t *testing.T) {
	f := newRunnerFixture(t, nil)
	installFakeClaude(t)
	var fail sync.Mutex
	failing := true
	var wrapped http.Handler = f.app
	errorServer := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		fail.Lock()
		bad := failing
		fail.Unlock()
		if bad {
			http.Error(w, "down", http.StatusBadGateway)
			return
		}
		wrapped.ServeHTTP(w, r)
	}))
	t.Cleanup(errorServer.Close)
	f.daemon.config.Runner.AppURL = errorServer.URL
	waits := make(chan int, 64)
	f.daemon.runnerWait = func(failures int) time.Duration {
		waits <- failures
		if failures == 0 {
			return time.Hour // only a wake-up may trigger the next poll
		}
		return 5 * time.Millisecond
	}
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	go f.daemon.runnerLoop(ctx)
	next := func() int {
		select {
		case n := <-waits:
			return n
		case <-time.After(10 * time.Second):
			t.Fatal(errors.New("runner loop stalled"))
			return -1
		}
	}
	for want := 1; want <= 3; want++ {
		if got := next(); got != want {
			t.Fatalf("consecutive failure count = %d, want %d", got, want)
		}
	}
	fail.Lock()
	failing = false
	fail.Unlock()
	for next() != 0 {
	}
	before, _ := f.app.snapshot()
	f.daemon.requestSync() // what agentJobRunner.finish does
	if got := next(); got != 0 {
		t.Fatalf("after wake-up failures = %d", got)
	}
	if after, _ := f.app.snapshot(); len(after) != len(before)+1 {
		t.Fatalf("wake-up did not re-poll: claims %d -> %d", len(before), len(after))
	}
}

func TestRunnerLoopAsksTheAppToHoldTheClaimAndRepollsAtOnce(t *testing.T) {
	f := newRunnerFixture(t, nil)
	installFakeClaude(t)
	f.app.mu.Lock()
	f.app.waitMs = runnerClaimWait.Milliseconds()
	f.app.mu.Unlock()
	f.daemon.runnerWait = func(int) time.Duration { return time.Hour }
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	go f.daemon.runnerLoop(ctx)
	deadline := time.Now().Add(10 * time.Second)
	for {
		claims, _ := f.app.snapshot()
		if len(claims) >= 3 {
			for _, claim := range claims {
				if claim["waitMs"] != float64(runnerClaimWait.Milliseconds()) {
					t.Fatalf("claim asked for waitMs %v", claim["waitMs"])
				}
			}
			return
		}
		if time.Now().After(deadline) {
			t.Fatalf("held claims were not re-polled: %d claims", len(claims))
		}
		time.Sleep(20 * time.Millisecond)
	}
}
