package relay

import (
	"context"
	"encoding/base64"
	"encoding/json"
	"io"
	"log"
	"net/url"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	"forkmesh.com/desktop-core/internal/control"
	"forkmesh.com/desktop-core/internal/coretest"
	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/identity"
)

type sentPing struct {
	target string
	agent  string
	ping   OnlinePing
}

type pingRecorder struct {
	sent chan sentPing
}

func newPingRecorder() *pingRecorder { return &pingRecorder{sent: make(chan sentPing, 8)} }

func (r *pingRecorder) send(_ context.Context, target, agent string, body []byte) (int, error) {
	var ping OnlinePing
	_ = json.Unmarshal(body, &ping)
	r.sent <- sentPing{target: target, agent: agent, ping: ping}
	return 200, nil
}

func (r *pingRecorder) next(t *testing.T) sentPing {
	t.Helper()
	select {
	case sent := <-r.sent:
		return sent
	case <-time.After(wait):
		t.Fatal("no online ping was sent")
		return sentPing{}
	}
}

func (r *pingRecorder) none(t *testing.T) {
	t.Helper()
	select {
	case sent := <-r.sent:
		t.Fatalf("unexpected online ping %+v", sent.ping)
	case <-time.After(150 * time.Millisecond):
	}
}

func linkPhone(t *testing.T, core *daemon.Core, user string, lastSeen time.Time) {
	t.Helper()
	key := base64.RawURLEncoding.EncodeToString(make([]byte, 32))
	devices := map[string]control.Device{key: {Key: key, Name: "phone", User: user, Platform: "ios",
		PairedAtMs: lastSeen.UnixMilli(), LastSeenMs: lastSeen.UnixMilli()}}
	if err := control.SaveDevices(control.DevicesPath(core.Layout.Data), devices); err != nil {
		t.Fatal(err)
	}
}

type clock struct {
	mu  sync.Mutex
	now time.Time
}

func (c *clock) Now() time.Time {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.now
}

func (c *clock) advance(d time.Duration) {
	c.mu.Lock()
	c.now = c.now.Add(d)
	c.mu.Unlock()
}

func onlineModule(t *testing.T) (*Module, *pingRecorder, *clock, *identity.Identity) {
	t.Helper()
	core := coretest.New(t)
	id := writeIdentity(t, core)
	ticks := &clock{now: time.UnixMilli(1_790_000_000_000)}
	recorder := newPingRecorder()
	module := New()
	module.core = core
	module.ctx = context.Background()
	module.Now = ticks.Now
	module.SendOnline = recorder.send
	module.identity = id
	module.config = Config{Endpoint: "wss://app.example.test/api/nodes/events", Owner: "Alice", Account: "alice", Node: "Box 1"}
	return module, recorder, ticks, id
}

func TestOnlinePingIsSignedLikeTheDesktopPing(t *testing.T) {
	id, err := identity.FromSeed(make([]byte, 32))
	if err != nil {
		t.Fatal(err)
	}
	now := time.UnixMilli(1_790_000_000_000)
	ping := NewOnlinePing("Box 1", time.Time{}, now)
	if ping.PingID != "1790000000000-online" || ping.Node != "box-1" || ping.Kind != "desktop-online" ||
		ping.Title != "Box 1 is online" || ping.Body != "ForkMesh started on this desktop and reached the relay." {
		t.Fatalf("ping %+v", ping)
	}
	target, ok := OnlinePingURL("wss://app.example.test/api/nodes/events", "Alice", id, ping, now)
	if !ok {
		t.Fatal("no url")
	}
	parsed, err := url.Parse(target)
	if err != nil {
		t.Fatal(err)
	}
	query := parsed.Query()
	if parsed.Scheme != "https" || parsed.Host != "app.example.test" || parsed.Path != "/api/notifications/desktop" ||
		query.Get("user") != "alice" || query.Get("ts") != "1790000000000" {
		t.Fatalf("url %s", target)
	}
	canonical := strings.Join([]string{"forkmesh-account-desktop-ping-v1", "alice", "1790000000000-online", "box-1",
		"desktop-online", "", "", "", "0", "Box 1 is online", "ForkMesh started on this desktop and reached the relay.",
		"1790000000000"}, "\n")
	if !identity.Verify(id.PublicKey(), query.Get("sig"), []byte(canonical)) {
		t.Fatal("the signature does not cover the canonical the server checks")
	}
	if _, ok := OnlinePingURL("https://app.example.test/x", "alice", id, ping, now); ok {
		t.Fatal("a non-websocket endpoint was accepted")
	}
	if plain, _ := OnlinePingURL("ws://127.0.0.1:9/api/nodes/events", "alice", id, ping, now); !strings.HasPrefix(plain, "http://127.0.0.1:9/") {
		t.Fatalf("plain relay %s", plain)
	}
}

func TestNodeTokenAndOfflineSpanMatchTheDesktop(t *testing.T) {
	for in, want := range map[string]string{"": "desktop", "  Jett's Mac ": "jett-s-mac", "box_1.local": "box_1.local", "café": "caf-"} {
		if got := NodeToken(in); got != want {
			t.Fatalf("NodeToken(%q) = %q, want %q", in, got, want)
		}
	}
	for gap, want := range map[time.Duration]string{0: "1 min", 61 * time.Second: "1 min", 90 * time.Minute: "1 h 30 min",
		2 * time.Hour: "2 h", 26 * time.Hour: "1 d 2 h", 48 * time.Hour: "2 d"} {
		if got := OfflineSpan(gap); got != want {
			t.Fatalf("OfflineSpan(%v) = %q, want %q", gap, got, want)
		}
	}
}

func TestAnnounceAtStartSkipsOnlyAQuickRequestedRestart(t *testing.T) {
	now := time.UnixMilli(1_790_000_000_000)
	cases := []struct {
		stopped time.Time
		restart bool
		want    bool
	}{
		{time.Time{}, false, true},
		{now.Add(-time.Minute), false, true},
		{now.Add(-time.Minute), true, false},
		{now.Add(-OnlineGap), true, true},
		{time.Time{}, true, true},
	}
	for _, c := range cases {
		if got := AnnounceAtStart(c.stopped, c.restart, now); got != c.want {
			t.Fatalf("AnnounceAtStart(%v, %v) = %v", c.stopped, c.restart, got)
		}
	}
}

func TestLinkTellsThePhoneOnceAndAgainAfterAGap(t *testing.T) {
	module, recorder, ticks, _ := onlineModule(t)
	linkPhone(t, module.core, "alice", ticks.Now())
	module.linkChanged(true)
	first := recorder.next(t)
	if first.ping.Title != "Box 1 is online" || !strings.Contains(first.agent, "node=Box%201") {
		t.Fatalf("first ping %+v", first)
	}
	module.linkChanged(false)
	ticks.advance(time.Minute)
	module.linkChanged(true)
	recorder.none(t)
	module.linkChanged(false)
	ticks.advance(11 * time.Minute)
	module.linkChanged(true)
	if again := recorder.next(t); again.ping.Body != "Back online after 11 min offline." {
		t.Fatalf("after a gap %+v", again.ping)
	}
	module.linkChanged(true)
	recorder.none(t)
}

func TestNoOnlinePingWithoutAPhoneOrWhenTurnedOff(t *testing.T) {
	module, recorder, ticks, _ := onlineModule(t)
	module.linkChanged(true)
	recorder.none(t)

	module, recorder, ticks, _ = onlineModule(t)
	linkPhone(t, module.core, "bob", ticks.Now())
	module.linkChanged(true)
	recorder.none(t)

	module, recorder, ticks, _ = onlineModule(t)
	linkPhone(t, module.core, "alice", ticks.Now().Add(-31*24*time.Hour))
	module.linkChanged(true)
	recorder.none(t)

	module, recorder, ticks, _ = onlineModule(t)
	linkPhone(t, module.core, "alice", ticks.Now())
	settings := module.core.Layout.SettingsFile()
	if err := os.MkdirAll(filepath.Dir(settings), 0o700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(settings, []byte("[notifications]\nphoneOnlinePing=false\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	module.linkChanged(true)
	recorder.none(t)
}

func TestRestartedCoreTellsThePhoneWithoutTheDesktop(t *testing.T) {
	f := newFixture(t, defaultHello(), nil)
	recorder := newPingRecorder()
	f.module.SendOnline = recorder.send
	linkPhone(t, f.core, "alice", time.Now())
	f.configure(t, "alice", "alice", "box-1")
	f.relay.next(t)
	if first := recorder.next(t); first.ping.Title != "box-1 is online" {
		t.Fatalf("first ping %+v", first.ping)
	}
	f.module.Stop()

	core := daemon.NewCore(f.core.Layout, "test", log.New(io.Discard, "", 0))
	module := New()
	module.Timings = fastTimings()
	module.SendOnline = recorder.send
	coretest.Start(t, core, module)
	f.relay.next(t)
	sent := recorder.next(t)
	if sent.ping.Title != "box-1 is online" || !strings.Contains(sent.target, "/api/notifications/desktop?") {
		t.Fatalf("boot ping %+v", sent)
	}
	recorder.none(t)
}
