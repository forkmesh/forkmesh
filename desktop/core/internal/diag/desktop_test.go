package diag

import (
	"context"
	"encoding/json"
	"strings"
	"testing"
	"time"

	"forkmesh.com/desktop-core/internal/agentproc"
	"forkmesh.com/desktop-core/internal/agents"
	"forkmesh.com/desktop-core/internal/coretest"
	"forkmesh.com/desktop-core/internal/rpc"
)

func TestStallRowMatchesDesktop(t *testing.T) {
	entry := "[2026-10-03 10:00:00]   UI stalled ~600 ms token=abcdef123456\nframe 1\nframe 2"
	row := stallRow(4, entry)
	if row["index"] != 4 || row["when"] != "2026-10-03 10:00:00" || row["lines"] != 3 || row["bytes"] != len(entry) {
		t.Fatalf("row = %v", row)
	}
	if row["summary"] != "UI stalled ~600 ms token=[redacted]" {
		t.Fatalf("summary = %q", row["summary"])
	}
	plain := stallRow(0, "no stamp here")
	if plain["when"] != "" || plain["summary"] != "no stamp here" {
		t.Fatalf("plain = %v", plain)
	}
	if empty := stallRow(0, "[] x"); empty["when"] != "" || empty["summary"] != "[] x" {
		t.Fatalf("an empty stamp is not a stamp: %v", empty)
	}
	detail := stallDetail(1, "[w] head\n"+strings.Repeat("a", 30)+"\nlast", 40)
	if detail["text"] != "[w] head\n"+strings.Repeat("a", 30) || detail["truncated"] != true {
		t.Fatalf("detail = %v", detail)
	}
	whole := stallDetail(1, "[w] head\nline", 64000)
	if whole["text"] != "[w] head\nline" || whole["truncated"] != false {
		t.Fatalf("whole = %v", whole)
	}
}

func TestStallListOwnedByCore(t *testing.T) {
	core, module := startModule(t, time.Hour)
	ui := coretest.NewPeer("ui", rpc.UI)
	phone := ownerPeer("phone")
	if _, err := coretest.Call(t, core, phone, "core.diag.stall", map[string]any{"entry": "x"}); rpc.Code(err) != "forbidden" {
		t.Fatalf("phones may not push stalls: %v", err)
	}
	if _, err := coretest.Call(t, core, phone, "core.diag.stalls", map[string]any{"entries": []string{}}); rpc.Code(err) != "forbidden" {
		t.Fatalf("phones may not sync stalls: %v", err)
	}
	canSend := true
	asked := 0
	module.UIState = func(ctx context.Context, roots bool) (desktopState, error) {
		asked++
		return desktopState{CanSendStalls: canSend}, nil
	}
	var list struct {
		Stalls  []map[string]any `json:"stalls"`
		CanSend bool             `json:"canSend"`
	}
	coretest.MustCall(t, core, phone, "diag.stalls", nil, &list)
	if len(list.Stalls) != 0 || list.CanSend || asked != 0 {
		t.Fatalf("empty list = %+v, asked %d", list, asked)
	}
	coretest.MustCall(t, core, ui, "core.diag.stalls", map[string]any{"entries": []string{"[a] one", "[b] two"}}, nil)
	coretest.MustCall(t, core, ui, "core.diag.stall", map[string]any{"entry": "[c] three\nstack"}, nil)
	coretest.MustCall(t, core, phone, "diag.stalls", nil, &list)
	if len(list.Stalls) != 3 || !list.CanSend || list.Stalls[0]["index"].(float64) != 2 || list.Stalls[2]["when"] != "a" {
		t.Fatalf("list = %+v", list)
	}
	canSend = false
	coretest.MustCall(t, core, phone, "diag.stalls", nil, &list)
	if list.CanSend {
		t.Fatal("canSend follows the desktop's stall repository")
	}
	module.UIState = func(ctx context.Context, roots bool) (desktopState, error) {
		return desktopState{}, rpc.Fail("desktop_unavailable")
	}
	coretest.MustCall(t, core, phone, "diag.stalls", nil, &list)
	if list.CanSend || len(list.Stalls) != 3 {
		t.Fatalf("no desktop, nothing can be sent: %+v", list)
	}
	var detail map[string]any
	coretest.MustCall(t, core, phone, "diag.stall", map[string]any{"index": 2}, &detail)
	if detail["text"] != "[c] three\nstack" || detail["when"] != "c" {
		t.Fatalf("detail = %v", detail)
	}
	for _, bad := range []any{3, -1, 1.5, "1"} {
		if _, err := coretest.Call(t, core, phone, "diag.stall", map[string]any{"index": bad}); rpc.Code(err) != "unknown_stall" {
			t.Fatalf("index %v: %v", bad, err)
		}
	}
	for i := 0; i < 120; i++ {
		coretest.MustCall(t, core, ui, "core.diag.stall", map[string]any{"entry": "[n] more"}, nil)
	}
	if got := len(module.stallSnapshot()); got != maxStallEntries {
		t.Fatalf("kept %d stalls", got)
	}
	var health map[string]any
	coretest.MustCall(t, core, phone, "diag.health", nil, &health)
	if health["stalls"].(float64) != maxStallEntries {
		t.Fatalf("health stalls = %v", health["stalls"])
	}
	coretest.MustCall(t, core, ui, "core.diag.stall", map[string]any{"clear": true}, nil)
	if len(module.stallSnapshot()) != 0 {
		t.Fatal("clear empties the list")
	}
	if _, err := coretest.Call(t, core, ui, "core.diag.stall", map[string]any{}); rpc.Code(err) != "bad_request" {
		t.Fatalf("no entry: %v", err)
	}
}

func TestHealthFromDesktopAndFallback(t *testing.T) {
	core, module := startModule(t, time.Hour)
	module.DesktopPID = func() int { return 777 }
	module.UIState = func(ctx context.Context, roots bool) (desktopState, error) {
		return desktopState{Desktop: "box", Agents: agentCounts{Running: 2, Queued: 1, Total: 5}}, nil
	}
	phone := ownerPeer("phone")
	var health map[string]any
	coretest.MustCall(t, core, phone, "diag.health", nil, &health)
	keys := []string{"level", "cpuPercent", "cpuCount", "load1", "memory", "pressure", "disk", "agents", "stalls",
		"process", "desktop", "sampledAtMs", "pressureLine"}
	if len(health) != len(keys) {
		t.Fatalf("health keys = %v", health)
	}
	for _, key := range keys {
		if _, ok := health[key]; !ok {
			t.Fatalf("health lacks %s: %v", key, health)
		}
	}
	agentsOut := health["agents"].(map[string]any)
	if agentsOut["running"].(float64) != 2 || agentsOut["queued"].(float64) != 1 || agentsOut["total"].(float64) != 5 ||
		health["desktop"] != "box" {
		t.Fatalf("health = %v", health)
	}
	if health["memory"].(map[string]any)["appResident"].(float64) != 777 ||
		!strings.Contains(health["pressureLine"].(string), "this process") {
		t.Fatalf("desktop process stats: %v", health)
	}
	module.UIState = func(ctx context.Context, roots bool) (desktopState, error) {
		return desktopState{}, rpc.Fail("desktop_unavailable")
	}
	module.Sessions = func() []agents.Session {
		return []agents.Session{
			{ID: 1, Status: agents.StatusRunning},
			{ID: 2, Status: agents.StatusQueued},
			{ID: 3, Status: agents.StatusQueued, Merged: true},
			{ID: 4, Status: agents.StatusSuccess},
		}
	}
	core.Set("control.info", map[string]any{"name": "core-box", "user": "alice"})
	coretest.MustCall(t, core, phone, "diag.health", nil, &health)
	agentsOut = health["agents"].(map[string]any)
	if agentsOut["running"].(float64) != 2 || agentsOut["queued"].(float64) != 1 || agentsOut["total"].(float64) != 4 ||
		health["desktop"] != "core-box" {
		t.Fatalf("fallback health = %v", health)
	}
}

const psFixture = `  100     1 alice   900000  9.0 /usr/bin/claude --print token=abc
  101   100 alice   500000  5.0 /usr/bin/node tool.js
  102   101 alice   400000  4.0 cc1plus -O2 huge.cpp
  200     1 root    300000  3.0 /usr/sbin/sshd
  bad line
  300     1 bob         x   1.0 broken rss
  301     1 bob     100000  0.5
`

func TestProcessesMapAgentTrees(t *testing.T) {
	core, module := startModule(t, time.Hour)
	module.RunPS = func(ctx context.Context) ([]byte, error) { return []byte(psFixture), nil }
	module.UIState = func(ctx context.Context, roots bool) (desktopState, error) {
		if !roots {
			t.Error("diag.processes asks for the roots")
		}
		return desktopState{Roots: []agentRoot{{Pid: 100, Session: 7, Label: "Fix the build", Stoppable: true}}}, nil
	}
	phone := ownerPeer("phone")
	var out struct {
		Processes   []map[string]any `json:"processes"`
		MemoryTotal float64          `json:"memoryTotal"`
	}
	coretest.MustCall(t, core, phone, "diag.processes", nil, &out)
	if len(out.Processes) != 4 || out.MemoryTotal != 1000 {
		t.Fatalf("processes = %+v", out)
	}
	first := out.Processes[0]
	if first["name"] != "claude" || first["rssBytes"].(float64) != 900000*1024 || first["percent"].(float64) != 9 ||
		first["command"] != "/usr/bin/claude --print [redacted]" || first["user"] != "alice" {
		t.Fatalf("first = %v", first)
	}
	for i, wantAgent := range []bool{true, true, true, false} {
		agent, ok := out.Processes[i]["agent"].(map[string]any)
		if ok != wantAgent {
			t.Fatalf("row %d agent = %v", i, out.Processes[i])
		}
		if ok && (agent["session"].(float64) != 7 || agent["label"] != "Fix the build" || agent["stoppable"] != true) {
			t.Fatalf("row %d agent = %v", i, agent)
		}
	}
	module.UIState = func(ctx context.Context, roots bool) (desktopState, error) {
		return desktopState{}, rpc.Fail("desktop_unavailable")
	}
	module.Sessions = func() []agents.Session {
		return []agents.Session{{ID: 9, Status: agents.StatusRunning, Prompt: "first line\nsecond"}}
	}
	module.AgentProcesses = func() []agentproc.Process {
		return []agentproc.Process{
			{Key: "agent-9-1700000000000", Pid: 101, Running: true},
			{Key: "core-abc", Pid: 200, Running: true},
			{Key: "agent-9-1", Pid: 102, Running: false},
		}
	}
	out.Processes = nil
	coretest.MustCall(t, core, phone, "diag.processes", nil, &out)
	if _, ok := out.Processes[0]["agent"]; ok {
		t.Fatalf("the parent of the agent is not part of it: %v", out.Processes[0])
	}
	agent := out.Processes[2]["agent"].(map[string]any)
	if agent["session"].(float64) != 9 || agent["label"] != "first line" {
		t.Fatalf("fallback agent = %v", agent)
	}
	if _, ok := out.Processes[3]["agent"]; ok {
		t.Fatal("a non-agent key is not a root")
	}
	module.RunPS = func(ctx context.Context) ([]byte, error) { return nil, errPSUnavailable }
	if _, err := coretest.Call(t, core, phone, "diag.processes", nil); rpc.Code(err) != "ps_unavailable" {
		t.Fatalf("missing ps: %v", err)
	}
	module.RunPS = func(ctx context.Context) ([]byte, error) { return nil, rpc.Fail("ps_failed") }
	if _, err := coretest.Call(t, core, phone, "diag.processes", nil); rpc.Code(err) != "ps_failed" {
		t.Fatalf("failing ps: %v", err)
	}
}

func TestParseProcessTableAndSummary(t *testing.T) {
	parents := map[int64]int64{}
	rows := parseProcessTable([]byte(psFixture), 2, parents)
	if len(rows) != 2 || parents[102] != 101 || parents[200] != 1 || rows[1].Name != "node" {
		t.Fatalf("rows = %+v parents = %v", rows, parents)
	}
	long := commandSummary(strings.Repeat("x", 200), 160)
	if utf16Len(long) != 160 || !strings.HasSuffix(long, "…") {
		t.Fatalf("long = %q", long)
	}
	if got := commandSummary("run  --api-key=1  SECRET_X --password", 160); got != "run [redacted] [redacted] [redacted]" {
		t.Fatalf("summary = %q", got)
	}
}

func TestDetailVitalsForTheDesktop(t *testing.T) {
	core, module := startModule(t, 20*time.Millisecond)
	secs := 10.0
	module.readCPU = func(pid int) (float64, bool) {
		secs += 0.5
		return secs, pid == 4242
	}
	module.readDisk = func(path string) (int64, int64) { return 500, 200 }
	ui := coretest.NewPeer("ui", rpc.UI)
	var out struct {
		Detail map[string]any `json:"detail"`
	}
	coretest.MustCall(t, core, ui, "core.vitals", map[string]any{"pid": 4242, "watch": true, "detail": true,
		"disks": []any{"/home/a", "relative", "/home/a", 5}}, &out)
	if out.Detail["memoryTotal"].(float64) != 1000 || out.Detail["memoryAvailable"].(float64) != 250 {
		t.Fatalf("detail = %v", out.Detail)
	}
	disks := out.Detail["disks"].([]any)
	if len(disks) != 1 || disks[0].(map[string]any)["path"] != "/home/a" || disks[0].(map[string]any)["free"].(float64) != 200 {
		t.Fatalf("disks = %v", disks)
	}
	var event map[string]any
	if err := json.Unmarshal(ui.Next(t, "core.vitals.sample", 2*time.Second), &event); err != nil {
		t.Fatal(err)
	}
	app := event["app"].(map[string]any)
	if app["pid"].(float64) != 4242 || app["resident"].(float64) != 4242 || app["threads"].(float64) != 3 ||
		app["openFileLimit"].(float64) != 1024 || app["cpuPercent"].(float64) < 0 {
		t.Fatalf("app = %v", app)
	}
	if event["memory"].(float64) != 75 || event["swapTotal"].(float64) != 0 {
		t.Fatalf("event = %v", event)
	}
	for len(ui.Events) > 0 {
		<-ui.Events
	}
	select {
	case got := <-ui.Events:
		if got.Name == "diag.vitals" {
			t.Fatal("a detail watcher gets core.vitals.sample, not the phone event")
		}
	case <-time.After(100 * time.Millisecond):
	}
	phone := ownerPeer("phone")
	coretest.MustCall(t, core, phone, "diag.watch", nil, nil)
	var plain map[string]any
	if err := json.Unmarshal(phone.Next(t, "diag.vitals", 2*time.Second), &plain); err != nil {
		t.Fatal(err)
	}
	if len(plain) != 4 {
		t.Fatalf("phones keep the four-field sample: %v", plain)
	}
	coretest.MustCall(t, core, ui, "core.vitals", map[string]any{"watch": false}, nil)
	if module.watching() != 1 {
		t.Fatal("the desktop unsubscribes on its own")
	}
}

func TestAppMeter(t *testing.T) {
	var meter appMeter
	start := time.Unix(100, 0)
	if meter.update(1, true, start) != -1 {
		t.Fatal("the first reading has no rate")
	}
	if got := meter.update(1.5, true, start.Add(time.Second)); got != 50 {
		t.Fatalf("rate = %v", got)
	}
	if got := meter.update(1.0, true, start.Add(2*time.Second)); got != 0 {
		t.Fatalf("negative rate = %v", got)
	}
	if meter.update(0, false, start) != -1 {
		t.Fatal("unreadable process")
	}
}

func TestDetailVitalsAddTheRepositoryDisk(t *testing.T) {
	core, module := startModule(t, 20*time.Millisecond)
	module.readDisk = func(path string) (int64, int64) {
		if path == "/repos/big" {
			return 9000, 1000
		}
		return 500, 200
	}
	ui := coretest.NewPeer("ui", rpc.UI)
	coretest.MustCall(t, core, ui, "core.vitals", map[string]any{"pid": 1, "watch": true, "detail": true,
		"disks": []any{"/home/a", "/data"}}, nil)
	var out struct {
		Detail map[string]any `json:"detail"`
	}
	coretest.MustCall(t, core, ui, "core.vitals", map[string]any{"pid": 1, "watch": true, "detail": true,
		"disks": []any{"/home/a", "/data", "/repos/big"}}, &out)
	if disks := out.Detail["disks"].([]any); len(disks) != 3 || disks[2].(map[string]any)["total"].(float64) != 9000 {
		t.Fatalf("reply disks = %v", disks)
	}
	if module.watching() != 1 {
		t.Fatalf("a second core.vitals from the same desktop replaces its watch, watchers = %d", module.watching())
	}
	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		var event map[string]any
		if err := json.Unmarshal(ui.Next(t, "core.vitals.sample", 2*time.Second), &event); err != nil {
			t.Fatal(err)
		}
		disks := event["disks"].([]any)
		if len(disks) == 3 && disks[2].(map[string]any)["path"] == "/repos/big" &&
			disks[2].(map[string]any)["free"].(float64) == 1000 {
			return
		}
	}
	t.Fatal("samples never carried the repository disk")
}
