package diag

import (
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"forkmesh.com/desktop-core/internal/coretest"
	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/rpc"
)

type fakeHost struct {
	mu    sync.Mutex
	ticks float64
}

func (f *fakeHost) read() reading {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.ticks++
	return reading{MemoryTotal: 1000, MemoryAvailable: 250, DiskTotal: 100, DiskFree: 40, SwapTotal: 0,
		CPUValid: true, CPUBusy: f.ticks * 50, CPUTotal: f.ticks * 100, CPUCount: 4, Pressure: pressure{Load1: 0.5}}
}

func startModule(t *testing.T, interval time.Duration) (*daemon.Core, *Module) {
	t.Helper()
	core := coretest.New(t)
	module := New()
	module.Interval = interval
	host := &fakeHost{}
	module.readHost = host.read
	module.readProc = func(pid int) processStats {
		return processStats{Resident: int64(pid), Swapped: 0, OpenFiles: 10, OpenFileLimit: 1024, Threads: 3}
	}
	module.Owners = func() []string { return []string{"alice"} }
	coretest.Start(t, core, module)
	return core, module
}

func ownerPeer(id string) *coretest.Peer {
	peer := coretest.NewPeer(id, rpc.LAN)
	peer.Dev.User = "Alice"
	return peer
}

func TestOwnerGate(t *testing.T) {
	core, _ := startModule(t, time.Hour)
	stranger := coretest.NewPeer("p1", rpc.Relay)
	stranger.Dev.User = "mallory"
	for _, method := range []string{"diag.watch", "diag.unwatch", "diag.log", "diag.buildMac", "diag.health", "diag.stalls", "diag.stall", "diag.processes"} {
		if _, err := coretest.Call(t, core, stranger, method, nil); rpc.Code(err) != "not_owner" {
			t.Fatalf("%s from a stranger: %v", method, err)
		}
	}
	keyless := ownerPeer("p2")
	keyless.Dev.Key = ""
	if _, err := coretest.Call(t, core, keyless, "diag.log", nil); rpc.Code(err) != "not_owner" {
		t.Fatalf("keyless device: %v", err)
	}
	if _, err := coretest.Call(t, core, ownerPeer("p3"), "diag.log", nil); err != nil {
		t.Fatalf("owner: %v", err)
	}
	if _, err := coretest.Call(t, core, coretest.NewPeer("ui", rpc.UI), "diag.log", nil); err != nil {
		t.Fatalf("desktop: %v", err)
	}
	for _, method := range []string{"diag.backups", "diag.prepare", "diag.run"} {
		if core.Registry.Native(method) {
			t.Fatalf("%s must stay with the desktop", method)
		}
	}
}

func TestDefaultOwnersFromSettingsAndControlInfo(t *testing.T) {
	core := coretest.New(t)
	module := New()
	module.readHost = (&fakeHost{}).read
	module.Interval = time.Hour
	coretest.Start(t, core, module)
	peer := ownerPeer("p1")
	if _, err := coretest.Call(t, core, peer, "diag.log", nil); rpc.Code(err) != "not_owner" {
		t.Fatalf("no owner known must refuse: %v", err)
	}
	core.Set("control.info", map[string]any{"user": "alice"})
	if _, err := coretest.Call(t, core, peer, "diag.log", nil); err != nil {
		t.Fatalf("control.info owner: %v", err)
	}
	core.Set("control.info", nil)
	if err := os.MkdirAll(filepath.Dir(core.Layout.SettingsFile()), 0o700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(core.Layout.SettingsFile(), []byte("[account]\nnodeName=alice\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	if _, err := coretest.Call(t, core, peer, "diag.log", nil); err != nil {
		t.Fatalf("settings owner: %v", err)
	}
}

func TestWatchStreamsVitals(t *testing.T) {
	core, module := startModule(t, 20*time.Millisecond)
	phone := ownerPeer("phone")
	var first history
	coretest.MustCall(t, core, phone, "diag.watch", nil, &first)
	var got sample
	if err := json.Unmarshal(phone.Next(t, "diag.vitals", 2*time.Second), &got); err != nil {
		t.Fatal(err)
	}
	if got.Memory != 75 || got.Disk != 60 || got.Swap != -1 || got.CPU != 50 {
		t.Fatalf("event = %+v", got)
	}
	phone.Next(t, "diag.vitals", 2*time.Second)
	var later history
	coretest.MustCall(t, core, phone, "diag.watch", nil, &later)
	if len(later.Memory) < 2 || len(later.CPU) != len(later.Memory) || later.Memory[0] != 75 {
		t.Fatalf("history = %+v", later)
	}
	if module.watching() != 1 {
		t.Fatalf("watching twice must keep one stream, got %d", module.watching())
	}
	coretest.MustCall(t, core, phone, "diag.unwatch", nil, nil)
	if module.watching() != 0 {
		t.Fatal("unwatch must stop the stream")
	}
	time.Sleep(60 * time.Millisecond)
	for len(phone.Events) > 0 {
		<-phone.Events
	}
	time.Sleep(80 * time.Millisecond)
	if len(phone.Events) != 0 {
		t.Fatal("events after unwatch")
	}
}

func TestWatchStopsWhenPeerLeaves(t *testing.T) {
	core, module := startModule(t, 20*time.Millisecond)
	phone := ownerPeer("phone")
	coretest.MustCall(t, core, phone, "diag.watch", nil, nil)
	phone.Next(t, "diag.vitals", 2*time.Second)
	phone.Close()
	deadline := time.Now().Add(2 * time.Second)
	for module.watching() != 0 {
		if time.Now().After(deadline) {
			t.Fatal("watcher kept after the peer left")
		}
		time.Sleep(5 * time.Millisecond)
	}
	other := ownerPeer("phone")
	coretest.MustCall(t, core, other, "diag.watch", nil, nil)
	other.Next(t, "diag.vitals", 2*time.Second)
	coretest.MustCall(t, core, phone, "diag.unwatch", nil, nil)
	if module.watching() != 1 {
		t.Fatal("an old connection with the same id must not cancel the new stream")
	}
}

func TestLogThroughCore(t *testing.T) {
	core, _ := startModule(t, time.Hour)
	if err := os.MkdirAll(core.Layout.Data, 0o700); err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(core.Layout.Data, "network_log.txt")
	if err := os.WriteFile(path, []byte("2026-10-02 10:00:00  boot\n2026-10-02 10:00:01  boom failed\n2026-10-02 10:00:02  idle\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	phone := ownerPeer("phone")
	var page map[string]any
	coretest.MustCall(t, core, phone, "diag.log", map[string]any{"lines": 5, "query": "boom", "severity": "error"}, &page)
	lines := page["lines"].([]any)
	if len(lines) != 1 || page["scanned"].(float64) != 3 || page["more"].(bool) || page["start"].(float64) != 0 {
		t.Fatalf("page = %v", page)
	}
	row := lines[0].(map[string]any)
	for _, key := range []string{"offset", "t", "severity", "text"} {
		if _, ok := row[key]; !ok {
			t.Fatalf("row lacks %s: %v", key, row)
		}
	}
	coretest.MustCall(t, core, phone, "diag.log", map[string]any{"lines": 2.5, "before": "x"}, &page)
	if len(page["lines"].([]any)) != 3 {
		t.Fatalf("non-integer lines falls back to 200, string before to the end: %v", page)
	}
	coretest.MustCall(t, core, phone, "diag.log", map[string]any{"lines": 1, "before": 26}, &page)
	if page["end"].(float64) != 26 || len(page["lines"].([]any)) != 1 || page["more"].(bool) {
		t.Fatalf("before page = %v", page)
	}
}

func TestCoreVitals(t *testing.T) {
	core, module := startModule(t, 20*time.Millisecond)
	if _, err := coretest.Call(t, core, ownerPeer("phone"), "core.vitals", nil); rpc.Code(err) != "forbidden" {
		t.Fatalf("phones may not call core.vitals: %v", err)
	}
	ui := coretest.NewPeer("ui", rpc.UI)
	var out struct {
		Memory  []float64      `json:"memory"`
		CPU     []float64      `json:"cpu"`
		Latest  sample         `json:"latest"`
		System  map[string]any `json:"system"`
		Seconds int            `json:"seconds"`
	}
	coretest.MustCall(t, core, ui, "core.vitals", map[string]any{"pid": 4242, "watch": true}, &out)
	if out.Seconds != vitalsSeconds || out.Memory == nil || out.System["level"] != "ok" {
		t.Fatalf("vitals = %+v", out)
	}
	memory := out.System["memory"].(map[string]any)
	if memory["appResident"].(float64) != 4242 || memory["total"].(float64) != 1000 {
		t.Fatalf("memory = %v", memory)
	}
	ui.Next(t, "diag.vitals", 2*time.Second)
	coretest.MustCall(t, core, ui, "core.vitals", map[string]any{"watch": false}, &out)
	if module.watching() != 0 {
		t.Fatal("watch:false must unsubscribe")
	}
	if out.System["process"].(map[string]any)["threads"].(float64) != -1 {
		t.Fatal("no pid means unknown process stats")
	}
}

func TestBuildMacSingleFlightAndCache(t *testing.T) {
	core, module := startModule(t, time.Hour)
	phone := ownerPeer("phone")
	var status map[string]any
	coretest.MustCall(t, core, phone, "diag.buildMac", nil, &status)
	if len(status) != 1 || status["configured"] != false {
		t.Fatalf("unconfigured = %v", status)
	}
	dir := filepath.Join(core.Layout.SSH, "mac-build")
	if err := os.MkdirAll(dir, 0o700); err != nil {
		t.Fatal(err)
	}
	for _, name := range []string{"id_ed25519", "known_hosts"} {
		if err := os.WriteFile(filepath.Join(dir, name), []byte("x"), 0o600); err != nil {
			t.Fatal(err)
		}
	}
	if err := os.MkdirAll(filepath.Dir(core.Layout.SettingsFile()), 0o700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(core.Layout.SettingsFile(), []byte("[actions]\nvariables=\"@ByteArray({\\\"MAC_BUILD_HOST\\\":\\\" mac.lan \\\",\\\"X\\\":\\\"y\\\"})\"\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	var calls atomic.Int32
	var seenArgs []string
	release := make(chan struct{})
	now := int64(1_000_000)
	module.mac.now = func() int64 { return atomic.LoadInt64(&now) }
	module.mac.probe = func(ctx context.Context, args []string) ([]byte, bool) {
		calls.Add(1)
		seenArgs = args
		<-release
		return []byte("60\nmacos-desktop\n"), true
	}
	var wg sync.WaitGroup
	results := make([]map[string]any, 3)
	for i := range results {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			coretest.MustCall(t, core, ownerPeer("phone"), "diag.buildMac", nil, &results[i])
		}(i)
	}
	time.Sleep(50 * time.Millisecond)
	close(release)
	wg.Wait()
	if calls.Load() != 1 {
		t.Fatalf("probe ran %d times", calls.Load())
	}
	for _, result := range results {
		if result["state"] != "building" || result["job"] != "macos-desktop" || result["sinceMs"].(float64) != 940_000 {
			t.Fatalf("result = %v", result)
		}
	}
	if host := seenArgs[len(seenArgs)-2]; host != "build@mac.lan" {
		t.Fatalf("host = %s", host)
	}
	coretest.MustCall(t, core, phone, "diag.buildMac", nil, &status)
	if calls.Load() != 1 {
		t.Fatal("cached answer expected within 20 s")
	}
	atomic.AddInt64(&now, buildMacCacheMs)
	module.mac.probe = func(ctx context.Context, args []string) ([]byte, bool) {
		calls.Add(1)
		return nil, false
	}
	coretest.MustCall(t, core, phone, "diag.buildMac", nil, &status)
	if calls.Load() != 2 || status["state"] != "offline" {
		t.Fatalf("refresh = %v", status)
	}
}

func TestMacBuildHost(t *testing.T) {
	if macBuildHost("") != buildMacDefaultHost || macBuildHost(`{"MAC_BUILD_HOST":"  "}`) != buildMacDefaultHost {
		t.Fatal("default host")
	}
	if macBuildHost(`{"MAC_BUILD_HOST":"10.0.0.2"}`) != "10.0.0.2" {
		t.Fatal("configured host")
	}
}
