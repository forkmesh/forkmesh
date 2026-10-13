package actions

import (
	"context"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"sync"
	"testing"
	"time"

	"forkmesh.com/desktop-core/internal/coretest"
	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/rpc"
)

type iniStore struct {
	mu     sync.Mutex
	path   string
	values map[string]string
	writes int
}

func iniQuote(text string) string {
	escaped := strings.ReplaceAll(text, `\`, `\\`)
	escaped = strings.ReplaceAll(escaped, `"`, `\"`)
	return `"` + escaped + `"`
}

func newIniStore(t *testing.T, path string) *iniStore {
	store := &iniStore{path: path, values: map[string]string{}}
	store.flush(t)
	return store
}

func (s *iniStore) flush(t *testing.T) {
	keys := make([]string, 0, len(s.values))
	for key := range s.values {
		keys = append(keys, key)
	}
	sort.Strings(keys)
	var out strings.Builder
	out.WriteString("[General]\n")
	for _, key := range keys {
		out.WriteString(strings.ReplaceAll(key, "/", `\`) + "=" + s.values[key] + "\n")
	}
	if err := os.MkdirAll(filepath.Dir(s.path), 0o700); err != nil {
		t.Fatal(err)
	}
	temp := s.path + ".tmp"
	if err := os.WriteFile(temp, []byte(out.String()), 0o600); err != nil {
		t.Fatal(err)
	}
	if err := os.Rename(temp, s.path); err != nil {
		t.Fatal(err)
	}
}

func (s *iniStore) set(t *testing.T, key, raw string) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.values[key] = raw
	s.flush(t)
}

func (s *iniStore) writer(t *testing.T) SettingsWriter {
	return func(ctx context.Context, writes []daemon.SettingWrite) error {
		s.mu.Lock()
		defer s.mu.Unlock()
		for _, write := range writes {
			if write.Remove {
				for key := range s.values {
					if key == write.Key || strings.HasPrefix(key, write.Key+"/") {
						delete(s.values, key)
					}
				}
				continue
			}
			switch write.Type {
			case "bool":
				s.values[write.Key] = fmt.Sprint(write.Value)
			case "int":
				s.values[write.Key] = fmt.Sprint(write.Value)
			case "string":
				s.values[write.Key] = iniQuote(fmt.Sprint(write.Value))
			case "stringList":
				items := write.Value.([]string)
				quoted := make([]string, len(items))
				for i, item := range items {
					quoted[i] = item
					if strings.ContainsAny(item, ",\"\\ ") {
						quoted[i] = iniQuote(item)
					}
				}
				switch len(items) {
				case 0:
					s.values[write.Key] = "@Invalid()"
				default:
					s.values[write.Key] = strings.Join(quoted, ", ")
				}
			case "json":
				data, _ := json.Marshal(write.Value)
				s.values[write.Key] = iniQuote("@ByteArray(" + string(data) + ")")
			case "bytes":
				data, _ := base64.StdEncoding.DecodeString(fmt.Sprint(write.Value))
				s.values[write.Key] = iniQuote("@ByteArray(" + string(data) + ")")
			default:
				return rpc.Fail("bad_type")
			}
		}
		s.writes++
		s.flush(t)
		return nil
	}
}

type queueFixture struct {
	*fixture
	core     *daemon.Core
	module   *Module
	ini      *iniStore
	ui       *coretest.Peer
	phone    *coretest.Peer
	logMu    sync.Mutex
	logs     []string
	unpinned []int
}

const queueWorkflow = "name: CI\non: [push, workflow_dispatch]\nsteps:\n  - name: Hello\n    run: echo hello-from-queue\n"

func newQueueFixture(t *testing.T, workflow string, extra map[string]string) *queueFixture {
	f := newFixture(t, workflow, extra)
	core := coretest.New(t)
	qf := &queueFixture{fixture: f, core: core}
	qf.ini = newIniStore(t, core.Settings.Path)
	module := New()
	module.Root = f.store.Root
	module.Concurrency = 2
	module.WriteSettings = qf.ini.writer(t)
	module.SetWorkflowDisabled = func(owner, name, path string, disabled bool) error { return nil }
	module.RenewMirror = func() {}
	module.UnpinMirror = func(id int) { qf.unpinned = append(qf.unpinned, id) }
	module.Agents = func() []AgentCount {
		return []AgentCount{{Owner: "owner", Name: "repo", Active: true}, {Owner: "owner", Name: "repo"}, {Owner: "other", Name: "x"}}
	}
	module.LogSink = func(text string) {
		qf.logMu.Lock()
		qf.logs = append(qf.logs, text)
		qf.logMu.Unlock()
	}
	qf.module = module
	coretest.Start(t, core, module)
	qf.ui = coretest.NewPeer("ui", rpc.UI)
	core.Hub.Add(qf.ui)
	qf.phone = coretest.NewPeer("phone", rpc.LAN)
	qf.phone.Dev = rpc.Device{Key: "device-key", Name: "Pixel", User: "owner"}
	core.Set("control.owners", []string{"owner"})
	return qf
}

func (qf *queueFixture) snapshot(autoApprove bool) map[string]any {
	return map[string]any{
		"repos": []map[string]any{{
			"owner": "owner", "name": "repo", "description": "demo", "mirrorPath": qf.mirror, "localPath": qf.work,
			"writablePath": qf.work, "relay": true, "enabled": true, "autoApprove": autoApprove,
		}},
		"nodeLabels": NodeLabels("this-node", "", ""),
		"machine":    "this-node",
	}
}

func (qf *queueFixture) call(peer rpc.Peer, method string, params any) (map[string]any, error) {
	raw, err := coretest.Call(qf.t, qf.core, peer, method, params)
	if err != nil {
		return nil, err
	}
	out := map[string]any{}
	if err := json.Unmarshal(raw, &out); err != nil {
		qf.t.Fatal(err)
	}
	return out, nil
}

func (qf *queueFixture) mustCall(peer rpc.Peer, method string, params any) map[string]any {
	qf.t.Helper()
	out, err := qf.call(peer, method, params)
	if err != nil {
		qf.t.Fatalf("%s: %v", method, err)
	}
	return out
}

func (qf *queueFixture) logText() string {
	qf.logMu.Lock()
	defer qf.logMu.Unlock()
	return strings.Join(qf.logs, "\n")
}

func (qf *queueFixture) waitRun(t *testing.T, match func(Run) bool, what string) Run {
	t.Helper()
	deadline := time.Now().Add(90 * time.Second)
	for time.Now().Before(deadline) {
		for _, run := range qf.store.LoadAllRuns() {
			if match(run) {
				return run
			}
		}
		time.Sleep(50 * time.Millisecond)
	}
	t.Fatalf("timed out waiting for %s; runs %+v\nlog:\n%s", what, qf.store.LoadAllRuns(), qf.logText())
	return Run{}
}

func (qf *queueFixture) spoolPush(t *testing.T, before string) {
	text := fmt.Sprintf("owner owner\nname repo\nmirror %s\nref %s %s refs/heads/main\n", qf.mirror, before, qf.commit)
	if err := os.WriteFile(filepath.Join(qf.store.SpoolDir(), "1-1.push"), []byte(text), 0o644); err != nil {
		t.Fatal(err)
	}
}

func TestQueuePushRunsAutoApprovedWorkflow(t *testing.T) {
	qf := newQueueFixture(t, queueWorkflow, nil)
	qf.expectForbidden(t)
	qf.spoolPush(t, strings.Repeat("0", 40))
	qf.mustCall(qf.ui, "core.actions.queue.attach", qf.snapshot(true))
	run := qf.waitRun(t, func(r Run) bool { return r.Status == StatusSuccess }, "a successful run")
	if run.Commit != qf.commit || run.Ref != "refs/heads/main" || run.WorkflowPath != ".forkmesh/ci.yml" {
		t.Fatalf("run %+v", run)
	}
	if !strings.Contains(qf.store.ReadLog(run), "hello-from-queue") {
		t.Fatal(qf.store.ReadLog(run))
	}
	logs := qf.logText()
	for _, want := range []string{"Push to owner/repo on main → " + qtLeft(qf.commit, 8), "Actions: queued \"CI\" for owner/repo @ " + qtLeft(qf.commit, 8)} {
		if !strings.Contains(logs, want) {
			t.Fatalf("missing %q in\n%s", want, logs)
		}
	}
	deadline := time.Now().Add(10 * time.Second)
	for qf.module.pendingApprovalCount() > 0 && time.Now().Before(deadline) {
		time.Sleep(20 * time.Millisecond)
	}
	if !IsApproved(qf.core.Settings.Get(), run.RepoKey(), run.WorkflowPath, run.WorkflowContent, run.RepositoryTree, run.ExecutionDigest) {
		t.Fatal("the automatic approval was not written through the settings writer")
	}
	if _, err := os.Stat(filepath.Join(qf.store.SpoolDir(), "1-1.push")); !os.IsNotExist(err) {
		t.Fatal("the push spool file was not consumed")
	}
	sawStarted := false
	for len(qf.ui.Events) > 0 {
		event := <-qf.ui.Events
		if event.Name == "actions.started" {
			sawStarted = true
		}
	}
	if !sawStarted {
		t.Fatal("no actions.started event")
	}
}

func (qf *queueFixture) expectForbidden(t *testing.T) {
	if _, err := qf.call(qf.phone, "core.actions.queue.attach", qf.snapshot(true)); rpc.Code(err) != "forbidden" {
		t.Fatalf("phone attach: %v", err)
	}
	if _, err := qf.call(qf.ui, "core.actions.queue.approve", map[string]any{"run": 1}); rpc.Code(err) != "queue_not_attached" {
		t.Fatalf("approve before attach: %v", err)
	}
}

func TestQueueManualRunWaitsForApprovalThenRuns(t *testing.T) {
	qf := newQueueFixture(t, queueWorkflow, nil)
	qf.mustCall(qf.ui, "core.actions.queue.attach", qf.snapshot(false))
	if _, err := qf.call(qf.ui, "core.actions.queue.manual", map[string]any{"owner": "owner", "name": "repo", "path": ".forkmesh/ci.yml", "branch": "nope"}); rpc.Code(err) != "Branch not found in this repo's mirror: nope" {
		t.Fatalf("missing branch: %v", err)
	}
	first := qf.mustCall(qf.ui, "core.actions.queue.manual", map[string]any{"owner": "owner", "name": "repo", "path": ".forkmesh/ci.yml", "branch": "main"})
	firstID := int(first["run"].(float64))
	run := qf.waitRun(t, func(r Run) bool { return r.ID == firstID }, "the manual run")
	if run.Status != StatusAwaitingApproval {
		t.Fatalf("status %s", run.Status)
	}
	second := qf.mustCall(qf.ui, "core.actions.queue.manual", map[string]any{"owner": "owner", "name": "repo", "path": ".forkmesh/ci.yml", "branch": "main"})
	secondID := int(second["run"].(float64))
	if _, ok := qf.store.FindRun(firstID); ok {
		t.Fatal("the superseded pending run was not deleted")
	}
	if !strings.Contains(qf.logText(), "Actions: cancelled pending \"CI\" for owner/repo") {
		t.Fatal(qf.logText())
	}
	if _, err := qf.call(qf.ui, "core.actions.queue.reject", map[string]any{"run": firstID}); rpc.Code(err) != "wrong_state" {
		t.Fatalf("reject deleted: %v", err)
	}
	qf.mustCall(qf.ui, "core.actions.queue.approve", map[string]any{"run": secondID})
	qf.waitRun(t, func(r Run) bool { return r.ID == secondID && r.Status == StatusSuccess }, "the approved run")
	if !strings.Contains(qf.logText(), "Actions: approved \"CI\" for owner/repo.") {
		t.Fatal(qf.logText())
	}
	rerun := qf.mustCall(qf.ui, "core.actions.queue.rerun", map[string]any{"run": secondID})
	rerunID := int(rerun["run"].(float64))
	qf.waitRun(t, func(r Run) bool { return r.ID == rerunID && r.Status == StatusSuccess }, "the rerun")
	if qf.ini.writes == 0 {
		t.Fatal("approval never reached the settings writer")
	}
}

func TestQueueRestoreRequeuesInterruptedRunAndSkip(t *testing.T) {
	qf := newQueueFixture(t, queueWorkflow, nil)
	interrupted := qf.run(0, queueWorkflow)
	interrupted.Status = StatusRunning
	interrupted.StartedAtMs = nowMs()
	created, err := qf.store.CreateRun(interrupted)
	if err != nil {
		t.Fatal(err)
	}
	native := qf.run(0, queueWorkflow)
	native.WorkflowPath = ".forkmesh/other.yml"
	native.Status = StatusRunning
	nativeRun, _ := qf.store.CreateRun(native)
	skipped := qf.run(0, queueWorkflow)
	skipped.WorkflowPath = ".forkmesh/third.yml"
	skipped.Status = StatusAwaitingApproval
	skippedRun, _ := qf.store.CreateRun(skipped)
	approve := func(run Run) {
		qf.module.approve(run.RepoKey(), run.WorkflowPath, run.WorkflowContent, run.RepositoryTree, run.ExecutionDigest)
	}
	approve(created)
	snap := qf.snapshot(false)
	snap["native"] = []int{nativeRun.ID}
	qf.mustCall(qf.ui, "core.actions.queue.attach", snap)
	qf.waitRun(t, func(r Run) bool { return r.ID == created.ID && r.Status == StatusSuccess }, "the restarted run")
	if !strings.Contains(qf.store.ReadLog(created), "RESTARTING: ForkMesh exited while this run was active") {
		t.Fatal(qf.store.ReadLog(created))
	}
	if still, _ := qf.store.FindRun(nativeRun.ID); still.Status != StatusRunning {
		t.Fatalf("the desktop's native run was touched: %s", still.Status)
	}
	qf.mustCall(qf.ui, "core.actions.queue.skip", map[string]any{"run": skippedRun.ID})
	if run, _ := qf.store.FindRun(skippedRun.ID); run.Status != StatusSkipped {
		t.Fatalf("skip: %s", run.Status)
	}
}

func TestQueueNeedsAndNodePins(t *testing.T) {
	deploy := "name: Deploy\non: push\nneeds: CI\nsteps:\n  - run: echo deploy\n"
	elsewhere := "name: Elsewhere\non: push\nruns-on: other-node\nsteps:\n  - run: echo no\n"
	qf := newQueueFixture(t, queueWorkflow, map[string]string{".forkmesh/deploy.yml": deploy, ".forkmesh/elsewhere.yml": elsewhere})
	qf.spoolPush(t, strings.Repeat("0", 40))
	qf.mustCall(qf.ui, "core.actions.queue.attach", qf.snapshot(true))
	ci := qf.waitRun(t, func(r Run) bool { return r.WorkflowName == "CI" && r.Status == StatusSuccess }, "CI")
	dep := qf.waitRun(t, func(r Run) bool { return r.WorkflowName == "Deploy" && r.Status == StatusSuccess }, "Deploy")
	if dep.StartedAtMs < ci.FinishedAtMs {
		t.Fatalf("Deploy started before CI finished: %d < %d", dep.StartedAtMs, ci.FinishedAtMs)
	}
	if !strings.Contains(qf.logText(), "“Elsewhere” is dedicated to other-node — this node (this-node") {
		t.Fatal(qf.logText())
	}
}

func TestPhoneIntentsAreNativeOnceTheQueueIsAttached(t *testing.T) {
	qf := newQueueFixture(t, queueWorkflow, nil)
	stranger := coretest.NewPeer("stranger", rpc.LAN)
	stranger.Dev = rpc.Device{Key: "k2", User: "mallory"}
	qf.mustCall(qf.ui, "core.actions.queue.attach", qf.snapshot(true))
	if _, err := qf.call(stranger, "repos.actions", map[string]any{"repo": "owner/repo"}); rpc.Code(err) != "not_owner" {
		t.Fatalf("stranger: %v", err)
	}
	if _, err := qf.call(qf.phone, "repos.actions", map[string]any{"repo": "owner/none"}); rpc.Code(err) != "unknown_repo" {
		t.Fatalf("unknown: %v", err)
	}
	prepared := qf.mustCall(qf.phone, "repos.prepare", map[string]any{"repo": "owner/repo", "action": "workflow.run", "path": ".forkmesh/ci.yml", "branch": "main"})
	started := qf.mustCall(qf.phone, "repos.run", map[string]any{"token": prepared["token"]})
	message, _ := started["message"].(string)
	var id int
	if _, err := fmt.Sscanf(message, "Started run #%d.", &id); err != nil || started["started"] != true || started["subject"] != "owner/repo" {
		t.Fatalf("started: %v", started)
	}
	qf.waitRun(t, func(r Run) bool { return r.ID == id && r.Status == StatusSuccess }, "the phone's run")
	if !strings.Contains(qf.logText(), "Actions: run workflow for owner/repo:.forkmesh/ci.yml@main requested by Pixel (owner), on the local network.") {
		t.Fatal(qf.logText())
	}
	prepared = qf.mustCall(qf.phone, "repos.prepare", map[string]any{"repo": "owner/repo", "action": "workflow.disable", "path": ".forkmesh/ci.yml"})
	off := qf.mustCall(qf.phone, "repos.run", map[string]any{"token": prepared["token"]})
	if off["message"] != "Turned off .forkmesh/ci.yml." {
		t.Fatal(off)
	}
	list := qf.mustCall(qf.phone, "repos.list", nil)
	if list["desktop"] != "this-node" {
		t.Fatal(list)
	}
	rows := list["repos"].([]any)
	row := rows[0].(map[string]any)
	agents := row["agents"].(map[string]any)
	if len(rows) != 1 || row["repo"] != "owner/repo" || row["checkout"] != true || row["branch"] != "main" ||
		agents["running"].(float64) != 1 || agents["total"].(float64) != 2 || row["description"] != "demo" {
		t.Fatalf("row %v", row)
	}
	if summary := row["actions"].(map[string]any); summary["latestStatus"] != StatusSuccess {
		t.Fatalf("summary %v", summary)
	}
	if _, err := qf.call(stranger, "repos.list", nil); rpc.Code(err) != "not_owner" {
		t.Fatalf("stranger list: %v", err)
	}
}

func TestReposListFallsBackToSettings(t *testing.T) {
	qf := newQueueFixture(t, queueWorkflow, nil)
	qf.ini.set(t, "repositories/items/size", "1")
	qf.ini.set(t, "repositories/items/1/owner", "owner")
	qf.ini.set(t, "repositories/items/1/name", "repo")
	qf.ini.set(t, "repositories/items/1/localPath", iniQuote(qf.work))
	qf.ini.set(t, "account/nodeName", "owner")
	list := qf.mustCall(qf.phone, "repos.list", nil)
	rows := list["repos"].([]any)
	if len(rows) != 1 || list["desktop"] != "owner" {
		t.Fatal(list)
	}
	row := rows[0].(map[string]any)
	if row["actionsEnabled"] != true || row["mirror"] != true || row["checkout"] != true {
		t.Fatal(row)
	}
}
