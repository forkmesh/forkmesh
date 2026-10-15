package insights

import (
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	"forkmesh.com/desktop-core/internal/coretest"
	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/rpc"
)

type fakeDesktop struct {
	mu       sync.Mutex
	attached bool
	calls    []string
	writes   []daemon.SettingWrite
	replies  map[string]string
	params   map[string][]json.RawMessage
	persist  func(daemon.SettingWrite)
}

func (d *fakeDesktop) call(ctx context.Context, method string, params any) ([]byte, error) {
	d.mu.Lock()
	defer d.mu.Unlock()
	d.calls = append(d.calls, method)
	if !d.attached {
		return nil, rpc.Fail("desktop_unavailable")
	}
	raw, _ := json.Marshal(params)
	if d.params == nil {
		d.params = map[string][]json.RawMessage{}
	}
	d.params[method] = append(d.params[method], raw)
	if method == "ui.settings.write" {
		var body struct {
			Writes []daemon.SettingWrite `json:"writes"`
		}
		_ = json.Unmarshal(raw, &body)
		d.writes = append(d.writes, body.Writes...)
		if d.persist != nil {
			for _, write := range body.Writes {
				d.persist(write)
			}
		}
		return []byte(`{}`), nil
	}
	if reply, ok := d.replies[method]; ok {
		if strings.HasPrefix(reply, "!") {
			return nil, rpc.Fail(reply[1:])
		}
		return []byte(reply), nil
	}
	return nil, rpc.Fail("unsupported")
}

func (d *fakeDesktop) isAttached() bool {
	d.mu.Lock()
	defer d.mu.Unlock()
	return d.attached
}

type harness struct {
	t       *testing.T
	core    *daemon.Core
	module  *Module
	desktop *fakeDesktop
	ui      *coretest.Peer
	phone   *coretest.Peer
	extra   []daemon.Module
}

func newHarness(t *testing.T, extra ...daemon.Module) *harness {
	t.Helper()
	core := coretest.New(t)
	return startHarness(t, core, extra...)
}

func startHarness(t *testing.T, core *daemon.Core, extra ...daemon.Module) *harness {
	t.Helper()
	module := New()
	module.SaveDelay = 40 * time.Millisecond
	module.PushDelay = 5 * time.Millisecond
	module.FeedEvery = 10 * time.Millisecond
	desktop := &fakeDesktop{attached: true, replies: map[string]string{}}
	module.CallUI = desktop.call
	module.Attached = desktop.isAttached
	modules := append(append([]daemon.Module{}, extra...), module)
	coretest.Start(t, core, modules...)
	h := &harness{t: t, core: core, module: module, desktop: desktop, ui: coretest.NewPeer("ui:1", rpc.UI),
		phone: coretest.NewPeer("lan:1", rpc.LAN), extra: extra}
	h.phone.Dev = rpc.Device{Key: "phone-key", Name: "Pixel", User: "alice"}
	core.Hub.Add(h.ui)
	core.Hub.Add(h.phone)
	core.Set("control.owners", []string{"alice"})
	return h
}

func (h *harness) call(peer rpc.Peer, method string, params any) map[string]any {
	h.t.Helper()
	out := map[string]any{}
	coretest.MustCall(h.t, h.core, peer, method, params, &out)
	return out
}

func (h *harness) fails(peer rpc.Peer, method string, params any, code string) {
	h.t.Helper()
	_, err := coretest.Call(h.t, h.core, peer, method, params)
	if rpc.Code(err) != code {
		h.t.Fatalf("%s: got %v, want %s", method, err, code)
	}
}

func toolUse(id, name string, input map[string]any) map[string]any {
	return map[string]any{"type": "assistant", "message": map[string]any{"content": []any{
		map[string]any{"type": "tool_use", "id": id, "name": name, "input": input}}}}
}

func toolResult(id, content string, isError bool) map[string]any {
	return map[string]any{"type": "user", "message": map[string]any{"content": []any{
		map[string]any{"type": "tool_result", "tool_use_id": id, "content": content, "is_error": isError}}}}
}

func waitFor(t *testing.T, what string, check func() bool) {
	t.Helper()
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		if check() {
			return
		}
		time.Sleep(5 * time.Millisecond)
	}
	t.Fatalf("timed out waiting for %s", what)
}

func TestDesktopIngestPushesAndSaves(t *testing.T) {
	h := newHarness(t)
	h.fails(h.phone, "core.insights.ingest", map[string]any{}, "forbidden")
	h.call(h.ui, "core.insights.ingest", map[string]any{"sessionId": 4, "provider": "claude-code", "model": "opus",
		"event": toolUse("t1", "Bash", map[string]any{"command": "git status"})})
	var changed struct {
		Tools json.RawMessage `json:"tools"`
		Root  string          `json:"root"`
	}
	if err := json.Unmarshal(h.ui.Next(t, "core.insights.changed", 2*time.Second), &changed); err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(changed.Tools), `"inFlight":1`) || changed.Root != h.module.Root() {
		t.Fatalf("snapshot %s root %s", changed.Tools, changed.Root)
	}
	h.call(h.ui, "core.insights.ingest", map[string]any{"sessionId": 4, "provider": "claude-code", "model": "opus",
		"event": toolResult("t1", "clean", false)})
	path := filepath.Join(h.module.Root(), ToolLedgerFile)
	waitFor(t, "the ledger file", func() bool {
		data, err := os.ReadFile(path)
		return err == nil && strings.Contains(string(data), `"Bash · git status"`) == false && strings.Contains(string(data), `"command":"git status"`)
	})
	state := h.call(h.ui, "core.insights.state", map[string]any{})
	if _, ok := state["guardrails"]; !ok {
		t.Fatalf("state without guardrails: %v", state)
	}
	h.call(h.ui, "core.insights.guardrail", map[string]any{"op": "blocked", "repository": "alice/demo", "tool": "Read",
		"path": "/etc/passwd", "outside": true})
	guard := filepath.Join(h.module.Root(), GuardrailLedgerFile)
	waitFor(t, "the guardrail file", func() bool {
		data, err := os.ReadFile(guard)
		return err == nil && strings.Contains(string(data), `"alice/demo"`)
	})
	h.fails(h.ui, "core.insights.guardrail", map[string]any{"op": "bogus"}, "bad_op")
	h.call(h.ui, "core.insights.ingest", map[string]any{"sessionId": 5, "event": toolUse("t2", "Read",
		map[string]any{"file_path": "/nope"})})
	h.call(h.ui, "core.insights.ingest", map[string]any{"sessionId": 5, "event": map[string]any{"type": "result"}})
	h.module.mu.Lock()
	inFlight := h.module.ledger.Totals(-1).InFlight
	signals := h.module.improvements.SignalCount()
	h.module.mu.Unlock()
	if inFlight != 0 || signals == 0 {
		t.Fatalf("inFlight %d signals %d", inFlight, signals)
	}
	h.call(h.ui, "core.insights.clear", map[string]any{"what": "improvements"})
	h.module.mu.Lock()
	signals = h.module.improvements.SignalCount()
	h.module.mu.Unlock()
	if signals != 0 {
		t.Fatalf("improvements not cleared: %d", signals)
	}
}

func TestStopSavesAndReloadsOnRestart(t *testing.T) {
	core := coretest.New(t)
	module := New()
	module.SaveDelay = time.Hour
	coretest.Start(t, core, module)
	ui := coretest.NewPeer("ui:1", rpc.UI)
	coretest.MustCall(t, core, ui, "core.insights.ingest", map[string]any{"sessionId": 1,
		"event": toolUse("a", "Grep", map[string]any{"pattern": "x"})}, nil)
	coretest.MustCall(t, core, ui, "core.insights.ingest", map[string]any{"sessionId": 1,
		"event": toolResult("a", "", false)}, nil)
	module.Stop()
	again := New()
	again.core = core
	again.root = module.root
	again.offsetsPath = module.offsetsPath
	again.open()
	if !again.ledger.HasSamples() || again.ledger.Stats("grep").Calls != 1 {
		t.Fatalf("reloaded ledger lost the call: %+v", again.ledger.Stats("grep"))
	}
}

type fakeRuns struct {
	mu     sync.Mutex
	runs   []map[string]any
	peers  map[string]rpc.Peer
	afters map[string]int64
	stored map[string][]map[string]any
}

func (f *fakeRuns) Name() string { return "agentrun" }

func (f *fakeRuns) Start(ctx context.Context, core *daemon.Core) error {
	core.Registry.Handle("core.agentrun.list", f.Name(), func(ctx context.Context, req *rpc.Request) (any, error) {
		f.mu.Lock()
		defer f.mu.Unlock()
		runs := append([]map[string]any(nil), f.runs...)
		return map[string]any{"runs": runs}, nil
	})
	core.Registry.Handle("core.agentrun.attach", f.Name(), func(ctx context.Context, req *rpc.Request) (any, error) {
		var params struct {
			Key   string `json:"key"`
			After int64  `json:"after"`
		}
		_ = req.Bind(&params)
		f.mu.Lock()
		defer f.mu.Unlock()
		f.peers[params.Key] = req.Peer
		f.afters[params.Key] = params.After
		for _, event := range f.stored[params.Key] {
			if so, _ := event["so"].(int64); so > params.After {
				req.Peer.Send("core.agentrun.event", event)
			}
		}
		return map[string]any{"key": params.Key}, nil
	})
	return nil
}

func (f *fakeRuns) emit(key string, so int64, event map[string]any) {
	line, _ := json.Marshal(event)
	payload := map[string]any{"key": key, "so": so, "line": string(line)}
	f.mu.Lock()
	f.stored[key] = append(f.stored[key], payload)
	peer := f.peers[key]
	f.mu.Unlock()
	if peer != nil {
		peer.Send("core.agentrun.event", payload)
	}
}

func TestFeedSubscribesToCoreRuns(t *testing.T) {
	runs := &fakeRuns{peers: map[string]rpc.Peer{}, afters: map[string]int64{}, stored: map[string][]map[string]any{}}
	runs.emit("s7-1", 100, toolUse("u1", "Bash", map[string]any{"command": "ls"}))
	runs.runs = []map[string]any{{"key": "s7-1", "sessionId": 7}}
	core := coretest.New(t)
	h := startHarness(t, core, runs)
	waitFor(t, "the replayed call", func() bool {
		h.module.mu.Lock()
		defer h.module.mu.Unlock()
		return h.module.ledger.Stats("bash").Calls == 1
	})
	runs.emit("s7-1", 150, toolResult("u1", "a\nb", false))
	runs.emit("s7-1", 150, toolResult("u1", "a\nb", false))
	runs.emit("s7-1", 120, toolUse("u2", "Bash", map[string]any{"command": "ls"}))
	runs.emit("s7-1", 200, map[string]any{"type": "result"})
	waitFor(t, "the finished call", func() bool {
		h.module.mu.Lock()
		defer h.module.mu.Unlock()
		stats := h.module.ledger.Stats("bash")
		return stats.LatencySamples == 1 && h.module.offsets["s7-1"] == 200
	})
	h.module.mu.Lock()
	stats := h.module.ledger.Stats("bash")
	h.module.mu.Unlock()
	if stats.Calls != 1 || stats.InFlight != 0 {
		t.Fatalf("bash stats %+v", stats)
	}
	h.module.saveTools()
	data, err := os.ReadFile(h.module.offsetsPath)
	if err != nil || !strings.Contains(string(data), `"s7-1":200`) {
		t.Fatalf("offsets %s %v", data, err)
	}
	second := coretest.New(t)
	second.Layout = core.Layout
	runs2 := &fakeRuns{peers: map[string]rpc.Peer{}, afters: map[string]int64{}, stored: runs.stored,
		runs: runs.runs}
	h2 := startHarness(t, second, runs2)
	waitFor(t, "the re-attach", func() bool {
		runs2.mu.Lock()
		defer runs2.mu.Unlock()
		return runs2.peers["s7-1"] != nil
	})
	runs2.mu.Lock()
	after := runs2.afters["s7-1"]
	runs2.mu.Unlock()
	if after != 200 {
		t.Fatalf("re-attached after %d, want 200", after)
	}
	h2.module.mu.Lock()
	calls := h2.module.ledger.Stats("bash").Calls
	h2.module.mu.Unlock()
	if calls != 1 {
		t.Fatalf("restart double counted: %d calls", calls)
	}
}

type repoEntry struct {
	owner, name, localPath string
	guarded                bool
	allowed                string
}

func writeSettings(t *testing.T, core *daemon.Core, repos []repoEntry, extra ...string) {
	t.Helper()
	path := core.Layout.SettingsFile()
	if err := os.MkdirAll(filepath.Dir(path), 0o700); err != nil {
		t.Fatal(err)
	}
	var b strings.Builder
	b.WriteString("[account]\nnodeName=alice\n\n[repositories]\n")
	for i, repo := range repos {
		prefix := "items\\" + string(rune('1'+i)) + "\\"
		b.WriteString(prefix + "owner=" + repo.owner + "\n")
		b.WriteString(prefix + "name=" + repo.name + "\n")
		b.WriteString(prefix + "localPath=" + repo.localPath + "\n")
		if repo.guarded {
			b.WriteString(prefix + "agentGuardrailsEnabled=true\n")
		}
		if repo.allowed != "" {
			b.WriteString(prefix + "agentAllowedDirectories=" + repo.allowed + "\n")
		}
	}
	b.WriteString("items\\size=" + string(rune('0'+len(repos))) + "\n\n")
	for _, line := range extra {
		b.WriteString(line + "\n")
	}
	if err := os.WriteFile(path, []byte(b.String()), 0o600); err != nil {
		t.Fatal(err)
	}
	future := time.Now().Add(time.Duration(len(b.String())) * time.Millisecond)
	_ = os.Chtimes(path, future, future)
}

func TestPhoneToolsAndGuardrails(t *testing.T) {
	h := newHarness(t)
	writeSettings(t, h.core, []repoEntry{{owner: "alice", name: "demo", localPath: "/tmp/demo", guarded: true,
		allowed: "src, ./docs/, src/lib"}, {owner: "bob", name: "remote", localPath: ""}},
		"[agents]", "disabledToolFamilies=bash, bogus")
	stranger := coretest.NewPeer("lan:2", rpc.LAN)
	stranger.Dev = rpc.Device{Key: "k2", Name: "Other", User: "mallory"}
	h.fails(stranger, "insights.tools", map[string]any{}, "not_owner")
	h.call(h.ui, "core.insights.ingest", map[string]any{"sessionId": 1, "event": toolUse("a", "Bash",
		map[string]any{"command": "make test"})})
	tools := h.call(h.phone, "insights.tools", map[string]any{})
	if disabled, _ := tools["disabled"].([]any); len(disabled) != 1 || disabled[0] != "bash" {
		t.Fatalf("disabled %v", tools["disabled"])
	}
	families, _ := tools["families"].([]any)
	if len(families) != len(Families())-3 {
		t.Fatalf("families %d", len(families))
	}
	var bash map[string]any
	for _, row := range families {
		if row.(map[string]any)["family"] == "bash" {
			bash = row.(map[string]any)
		}
	}
	if bash["inFlight"] != float64(1) || bash["disabled"] != true {
		t.Fatalf("bash row %v", bash)
	}
	usage := tools["usage"].([]any)
	if len(usage) != 1 || usage[0].(map[string]any)["tool"] != "Bash" {
		t.Fatalf("usage %v", usage)
	}
	guard := h.call(h.phone, "insights.guardrails", map[string]any{})
	policies := guard["policies"].([]any)
	if len(policies) != 1 {
		t.Fatalf("policies %v", policies)
	}
	policy := policies[0].(map[string]any)
	if policy["repository"] != "alice/demo" || policy["enabled"] != true {
		t.Fatalf("policy %v", policy)
	}
	if dirs := policy["allowedDirectories"].([]any); len(dirs) != 2 || dirs[0] != "docs" || dirs[1] != "src" {
		t.Fatalf("allowed %v", dirs)
	}
}

func TestToggleFamilyGoesThroughTheDesktop(t *testing.T) {
	h := newHarness(t)
	h.fails(h.phone, "insights.run", map[string]any{"action": "toggleFamily", "family": "bash", "enabled": false},
		"confirmation_required")
	h.fails(h.phone, "insights.prepare", map[string]any{"action": "toggleFamily", "family": "nope", "enabled": false},
		"bad_family")
	prepared := h.call(h.phone, "insights.prepare", map[string]any{"action": "toggleFamily", "family": "bash", "enabled": false})
	if prepared["subject"] != "bash|off" || !strings.Contains(prepared["summary"].(string), "Switch the Shell tool family off") {
		t.Fatalf("prepare %v", prepared)
	}
	h.desktop.replies["ui.insights.toggleFamily"] = `{"ok":true}`
	ran := h.call(h.phone, "insights.run", map[string]any{"action": "toggleFamily", "family": "bash", "enabled": false,
		"token": prepared["token"]})
	if ran["message"] != "Switched off." {
		t.Fatalf("run %v", ran)
	}
	if got := string(h.desktop.params["ui.insights.toggleFamily"][0]); got != `{"enabled":false,"family":"bash"}` {
		t.Fatalf("desktop params %s", got)
	}
	h.phone.Next(t, insightsChanged, time.Second)
	h.fails(h.phone, "insights.run", map[string]any{"action": "toggleFamily", "family": "bash", "enabled": false,
		"token": prepared["token"]}, "confirmation_required")
	again := h.call(h.phone, "insights.prepare", map[string]any{"action": "toggleFamily", "family": "bash", "enabled": true})
	h.desktop.mu.Lock()
	h.desktop.attached = false
	h.desktop.mu.Unlock()
	h.fails(h.phone, "insights.run", map[string]any{"action": "toggleFamily", "family": "bash", "enabled": true,
		"token": again["token"]}, "desktop_unavailable")
}

func TestShortcutScriptRunsOnTheDesktop(t *testing.T) {
	h := newHarness(t)
	checkout := t.TempDir()
	script := filepath.Join(checkout, ".forkmesh", "shortcuts", "check.sh")
	if err := os.MkdirAll(filepath.Dir(script), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(script, []byte("# name: Check it\necho done-line\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	writeSettings(t, h.core, []repoEntry{{owner: "alice", name: "demo", localPath: checkout}})
	prepared := h.call(h.phone, "insights.prepare", map[string]any{"action": "runShortcut", "repo": "alice/demo",
		"scope": "shortcut", "file": "check.sh"})
	if !strings.HasPrefix(prepared["summary"].(string), "Run the \"check.sh\" script on the desktop in alice/demo.") {
		t.Fatalf("summary %v", prepared["summary"])
	}
	h.desktop.replies["ui.insights.runShortcutScript"] = `{"run":4,"restart":false,"name":"Check it"}`
	ran := h.call(h.phone, "insights.run", map[string]any{"action": "runShortcut", "repo": "alice/demo",
		"scope": "shortcut", "file": "check.sh", "token": prepared["token"]})
	if ran["message"] != "Running Check it on the desktop." || ran["run"] != float64(4) {
		t.Fatalf("run %v", ran)
	}
	if got := string(h.desktop.params["ui.insights.runShortcutScript"][0]); got != `{"file":"check.sh","repo":"alice/demo"}` {
		t.Fatalf("desktop params %s", got)
	}
	h.phone.Next(t, insightsChanged, time.Second)
	again := h.call(h.phone, "insights.prepare", map[string]any{"action": "runShortcut", "repo": "alice/demo",
		"scope": "shortcut", "file": "check.sh"})
	h.desktop.replies["ui.insights.runShortcutScript"] = `{"run":5,"restart":true,"name":"Check it"}`
	ran = h.call(h.phone, "insights.run", map[string]any{"action": "runShortcut", "repo": "alice/demo",
		"scope": "shortcut", "file": "check.sh", "token": again["token"]})
	if ran["message"] != "Stopping Check it to run it again on the desktop." {
		t.Fatalf("restart %v", ran)
	}
	if h.core.Registry.Native("insights.shortcutRun") {
		t.Fatal("insights.shortcutRun should be answered by the desktop")
	}
}

func TestNetworkSamplesTheDesktopRoots(t *testing.T) {
	h := newHarness(t)
	h.desktop.replies["ui.insights.networkRoots"] = `{"roots":[{"session":9,"pid":1234,"label":"Fix it"}]}`
	h.module.Sample = func(roots []NetRoot) NetSample {
		if len(roots) != 1 || roots[0].Pid != 1234 {
			t.Errorf("roots %v", roots)
		}
		return NetSample{Owner: map[uint64]int{11: 9, 12: 9}, Sockets: []NetSocket{
			{Inode: 11, Peer: "140.82.112.3", Port: 443, Sent: 10, Received: 20},
			{Inode: 12, Peer: "127.0.0.1", Port: 80, Sent: 1, Received: 1},
			{Inode: 13, Peer: "8.8.8.8", Port: 53, Sent: 1, Received: 1},
		}}
	}
	out := h.call(h.phone, "insights.network", map[string]any{})
	agents := out["agents"].([]any)
	if len(agents) != 1 || out["sampledAtMs"] == nil {
		t.Fatalf("network %v", out)
	}
	row := agents[0].(map[string]any)
	if row["label"] != "Fix it" || row["hostCount"] != float64(1) {
		t.Fatalf("row %v", row)
	}
	h.desktop.replies["ui.insights.networkRoots"] = `{"roots":[]}`
	empty := h.call(h.phone, "insights.network", map[string]any{})
	if _, has := empty["sampledAtMs"]; has || len(empty["agents"].([]any)) != 0 {
		t.Fatalf("empty %v", empty)
	}
}

func TestSampleNetworkOwnProcess(t *testing.T) {
	if _, err := os.Stat("/proc/self/fd"); err != nil {
		t.Skip("no /proc")
	}
	sample := SampleNetwork([]NetRoot{{Session: 1, Pid: int64(os.Getpid())}})
	for _, socket := range sample.Sockets {
		if socket.Inode == 0 {
			t.Fatalf("socket without inode %+v", socket)
		}
	}
}
