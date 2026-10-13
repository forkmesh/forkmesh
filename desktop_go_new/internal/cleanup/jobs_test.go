package cleanup

import (
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"forkmesh.com/desktop-core/internal/coretest"
	"forkmesh.com/desktop-core/internal/rpc"
)

func startModule(t *testing.T, root string) (*Module, *coretest.Peer, func(string, any) (json.RawMessage, error)) {
	t.Helper()
	core := coretest.New(t)
	m := New()
	m.ProcRoot = filepath.Join(root, "proc")
	_ = os.MkdirAll(m.ProcRoot, 0o755)
	coretest.Start(t, core, m)
	t.Cleanup(m.Wait)
	ui := coretest.NewPeer("ui", rpc.UI)
	core.Hub.Add(ui)
	call := func(method string, params any) (json.RawMessage, error) {
		return coretest.Call(t, core, ui, method, params)
	}
	return m, ui, call
}

func slowCcache(t *testing.T, root string, seconds string) string {
	t.Helper()
	script := filepath.Join(root, "bin/ccache")
	touch(t, script, []byte("#!/bin/sh\nsleep "+seconds+"\necho "+root+"/nowhere\n"))
	if err := os.Chmod(script, 0o755); err != nil {
		t.Fatal(err)
	}
	return script
}

func TestScanRunsInTheCoreAndStreamsProgress(t *testing.T) {
	unixOnly(t)
	root := sandbox(t)
	c := baseContext(root)
	touch(t, c.Home+"/.cache/fm-rewrite/state.json")
	age(t, c.Home, 4)
	m, ui, call := startModule(t, root)
	raw, err := call("core.cleanup.scan", map[string]any{"token": "t1", "context": c})
	if err != nil {
		t.Fatal(err)
	}
	var started struct {
		ID       int64 `json:"id"`
		Attached bool  `json:"attached"`
	}
	_ = json.Unmarshal(raw, &started)
	if started.ID == 0 || started.Attached {
		t.Fatalf("started %s", raw)
	}
	var stages []string
	var reply ScanReply
	deadline := time.After(10 * time.Second)
	for reply.ID == 0 {
		select {
		case event := <-ui.Events:
			switch event.Name {
			case "core.cleanup.progress":
				var progress struct {
					Stage  string   `json:"stage"`
					Tokens []string `json:"tokens"`
				}
				_ = json.Unmarshal(event.Data, &progress)
				if !contains(progress.Tokens, "t1") {
					t.Errorf("progress without the token: %s", event.Data)
				}
				stages = append(stages, progress.Stage)
			case "core.cleanup.scanned":
				_ = json.Unmarshal(event.Data, &reply)
			}
		case <-deadline:
			t.Fatal("no core.cleanup.scanned")
		}
	}
	if strings.Join(stages, "|") != "Agent worktrees|Claude Code|Codex|Builds and temp files|Other caches" {
		t.Errorf("stages %v", stages)
	}
	if reply.ID != started.ID || !contains(reply.Tokens, "t1") || reply.Cancelled || reply.ScannedAtMs == 0 {
		t.Errorf("reply %+v", reply)
	}
	if item := find(reply.Items, CategoryOtherCaches, "fm-rewrite"); item == nil || !item.ConfirmEach {
		t.Errorf("items %+v", reply.Items)
	}
	m.Wait()
	if m.Busy() {
		t.Error("busy after the scan")
	}
	var state struct {
		Scanning bool       `json:"scanning"`
		LastScan *ScanReply `json:"lastScan"`
	}
	raw, _ = call("core.cleanup.state", nil)
	_ = json.Unmarshal(raw, &state)
	if state.Scanning || state.LastScan == nil || state.LastScan.ID != started.ID || len(state.LastScan.Items) != len(reply.Items) {
		t.Errorf("state %s", raw)
	}
}

func TestASecondScanAttachesAndCancelStopsIt(t *testing.T) {
	unixOnly(t)
	root := sandbox(t)
	c := baseContext(root)
	on := true
	c.RunCcache = &on
	m, ui, call := startModule(t, root)
	m.Ccache = slowCcache(t, root, "1")
	first, _ := call("core.cleanup.scan", map[string]any{"token": "old-desktop", "context": c})
	ui.Next(t, "core.cleanup.progress", 5*time.Second)
	if !m.Busy() {
		t.Fatal("not busy while scanning")
	}
	second, _ := call("core.cleanup.scan", map[string]any{"token": "new-desktop", "context": c})
	var a, b struct {
		ID       int64 `json:"id"`
		Attached bool  `json:"attached"`
	}
	_ = json.Unmarshal(first, &a)
	_ = json.Unmarshal(second, &b)
	if a.ID != b.ID || !b.Attached {
		t.Fatalf("attach %s %s", first, second)
	}
	if _, err := call("core.cleanup.deleteFiles", DeleteRequest{}); rpc.Code(err) != "busy" {
		t.Errorf("delete during a scan: %v", err)
	}
	raw, _ := call("core.cleanup.cancel", nil)
	if !strings.Contains(string(raw), `"cancelled":true`) {
		t.Errorf("cancel %s", raw)
	}
	var reply ScanReply
	_ = json.Unmarshal(ui.Next(t, "core.cleanup.scanned", 10*time.Second), &reply)
	if !reply.Cancelled || !contains(reply.Tokens, "old-desktop") || !contains(reply.Tokens, "new-desktop") {
		t.Errorf("reply %+v", reply)
	}
}

func TestDeleteRunsInTheCoreAndAppendsTheLog(t *testing.T) {
	unixOnly(t)
	root := sandbox(t)
	c := baseContext(root)
	touch(t, c.Home+"/.cache/fm-rewrite/state.json")
	touch(t, c.Home+"/.local/share/claude/versions/1.0.0")
	touch(t, c.SandboxDir+"/forkmesh-run-7-1/work/a")
	touch(t, c.SandboxDir+"/forkmesh-run-8-1/work/a")
	touch(t, c.Home+"/.cache/forkmesh-release-x/log")
	age(t, c.Home, 4)
	c.ClaimedSandboxes = []string{"forkmesh-run-8-1"}
	m, ui, call := startModule(t, root)
	if _, err := call("core.cleanup.scan", map[string]any{"context": c}); err != nil {
		t.Fatal(err)
	}
	var scanned ScanReply
	_ = json.Unmarshal(ui.Next(t, "core.cleanup.scanned", 10*time.Second), &scanned)
	m.Wait()
	operatorCache := find(scanned.Items, CategoryOtherCaches, "fm-rewrite")
	version := find(scanned.Items, CategoryClaude, "Claude Code 1.0.0")
	sandbox := find(scanned.Items, CategoryBuildTemp, "Actions sandbox · forkmesh-run-7-1")
	release := find(scanned.Items, CategoryOtherCaches, "forkmesh-release-x")
	if operatorCache == nil || version == nil || sandbox == nil || release == nil {
		t.Fatalf("scan %+v", scanned.Items)
	}
	logPath := filepath.Join(root, "logs/cleanup.log")
	request := DeleteRequest{
		Token:     "d1",
		Files:     []Step{{Item: *operatorCache}, {Item: *version}, {Item: *release, Choice: Choice{Confirmed: true}}},
		Sandboxes: []Step{{Item: *sandbox}},
		Skipped:   []Outcome{{Path: "/somewhere", Bytes: 3, Reason: "r", Error: "not confirmed"}},
		Claimed:   []string{"forkmesh-run-8-1"},
		Context:   Context{Home: c.Home, DataDir: c.DataDir},
		LogPath:   logPath,
	}
	if _, err := call("core.cleanup.deleteFiles", request); err != nil {
		t.Fatal(err)
	}
	var reply DeleteReply
	_ = json.Unmarshal(ui.Next(t, "core.cleanup.deleted", 10*time.Second), &reply)
	m.Wait()
	if !contains(reply.Tokens, "d1") || !reply.Logged || len(reply.Outcomes) != 4 {
		t.Fatalf("reply %+v", reply)
	}
	byPath := map[string]Outcome{}
	for _, outcome := range reply.Outcomes {
		byPath[outcome.Path] = outcome
	}
	if o := byPath[operatorCache.Path]; o.Removed || o.Error != "not confirmed" || !exists(operatorCache.Path) {
		t.Errorf("an unconfirmed operator cache is kept: %+v", o)
	}
	if o := byPath[release.Path]; o.Removed || o.Error != "protected" || !exists(release.Path) {
		t.Errorf("a protected item is kept even when the request says confirmed: %+v", o)
	}
	if o := byPath[version.Path]; !o.Removed || exists(version.Path) {
		t.Errorf("an old Claude version is removed: %+v", o)
	}
	if o := byPath[sandbox.Path]; !o.Removed || exists(sandbox.Path) || !exists(c.SandboxDir+"/forkmesh-run-8-1") {
		t.Errorf("the finished sandbox is removed: %+v", o)
	}
	if reply.Freed != version.Bytes+sandbox.Bytes {
		t.Errorf("freed %d", reply.Freed)
	}
	text, _ := os.ReadFile(logPath)
	lines := strings.Split(strings.TrimSpace(string(text)), "\n")
	if len(lines) != 5 || !strings.Contains(lines[0], "\tkept\t3\t/somewhere\tr\tnot confirmed") || !strings.Contains(string(text), "\tremoved\t") {
		t.Errorf("log:\n%s", text)
	}
	raw, err := call("core.cleanup.appendLog", map[string]any{"logPath": logPath, "outcomes": []Outcome{{Path: "/wt", Removed: true, Bytes: 9, Reason: "w"}}})
	if err != nil || !strings.Contains(string(raw), `"logged":true`) {
		t.Fatalf("appendLog %s %v", raw, err)
	}
	text, _ = os.ReadFile(logPath)
	if !strings.HasSuffix(string(text), "\tremoved\t9\t/wt\tw\n") {
		t.Errorf("appended:\n%s", text)
	}
}

func TestDeleteRechecksWorktreesForTheDesktop(t *testing.T) {
	unixOnly(t)
	root := sandbox(t)
	wt := root + "/wt/one"
	_ = os.MkdirAll(wt, 0o755)
	m, ui, call := startModule(t, root)
	items := []Item{
		{ID: "busy", Kind: KindWorktree, Path: wt, Status: StatusOrphaned},
		{ID: "gone", Kind: KindWorktree, Path: root + "/wt/two", Status: StatusOrphaned},
		{ID: "dirty", Kind: KindWorktree, Path: wt, Status: StatusOrphaned, DirtyFiles: 2},
	}
	_ = os.MkdirAll(m.ProcRoot+"/77", 0o755)
	_ = os.Symlink(wt, m.ProcRoot+"/77/cwd")
	touch(t, m.ProcRoot+"/77/comm", []byte("bash\n"))
	var steps []Step
	for _, item := range items {
		steps = append(steps, Step{Item: item})
	}
	if _, err := call("core.cleanup.deleteFiles", DeleteRequest{Worktrees: steps, LogPath: root + "/logs/c.log"}); err != nil {
		t.Fatal(err)
	}
	var reply DeleteReply
	_ = json.Unmarshal(ui.Next(t, "core.cleanup.deleted", 10*time.Second), &reply)
	m.Wait()
	want := map[string]string{"busy": "now open in bash (PID 77)", "gone": "already gone", "dirty": "has uncommitted changes; tick Discard to remove it"}
	for id, why := range want {
		if reply.WorktreeRefusals[id] != why {
			t.Errorf("%s: %q, want %q", id, reply.WorktreeRefusals[id], why)
		}
	}
	if !exists(wt) {
		t.Error("the core removed a worktree itself")
	}
}

func TestScanWaitsForARunningDelete(t *testing.T) {
	unixOnly(t)
	root := sandbox(t)
	m, ui, _ := startModule(t, root)
	hold := make(chan struct{})
	job := &deleteJob{id: 99, done: hold}
	m.jobs.mu.Lock()
	m.jobs.deleting = job
	m.jobs.mu.Unlock()
	id, attached := m.StartScan("t", baseContext(root))
	if id == 0 || attached {
		t.Fatal("scan did not start")
	}
	select {
	case event := <-ui.Events:
		t.Fatalf("scan ran during a delete: %s", event.Name)
	case <-time.After(200 * time.Millisecond):
	}
	m.jobs.mu.Lock()
	m.jobs.deleting = nil
	m.jobs.mu.Unlock()
	close(hold)
	ui.Next(t, "core.cleanup.scanned", 10*time.Second)
}

func TestNewMethodsAreDesktopOnly(t *testing.T) {
	core := coretest.New(t)
	coretest.Start(t, core, New())
	for _, transport := range []rpc.Transport{rpc.LAN, rpc.Relay} {
		phone := coretest.NewPeer("phone", transport)
		for _, method := range []string{"core.cleanup.scan", "core.cleanup.cancel", "core.cleanup.state", "core.cleanup.deleteFiles", "core.cleanup.appendLog"} {
			if _, err := coretest.Call(t, core, phone, method, map[string]any{}); rpc.Code(err) != "forbidden" {
				t.Errorf("%s over %v: %v", method, transport, err)
			}
		}
	}
}
