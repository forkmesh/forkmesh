package actions

import (
	"context"
	"encoding/json"
	"strings"
	"testing"
	"time"

	"forkmesh.com/desktop-core/internal/coretest"
	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/rpc"
)

type moduleFixture struct {
	*fixture
	core      *daemon.Core
	module    *Module
	ui        *coretest.Peer
	phone     *coretest.Peer
	performed []Intent
	facts     RepoFacts
}

func newModuleFixture(t *testing.T, workflow string) *moduleFixture {
	f := newFixture(t, workflow, nil)
	core := coretest.New(t)
	mf := &moduleFixture{fixture: f, core: core}
	mf.facts = RepoFacts{Repo: "owner/repo", Owner: "owner", Name: "repo", MirrorPath: f.mirror, LocalPath: f.work, Enabled: true,
		NodeLabels: NodeLabels("this-node", "", "")}
	module := New()
	module.Root = f.store.Root
	module.Facts = func(ctx context.Context, peer rpc.Peer, method, repo string) (RepoFacts, error) {
		if peer != nil && peer.Device().User != "owner" {
			return RepoFacts{}, rpc.Fail("not_owner")
		}
		if _, _, ok := ParseRepoSlug(repo); !ok {
			return RepoFacts{}, rpc.Fail("bad_request")
		}
		if !strings.EqualFold(strings.TrimSpace(repo), "owner/repo") {
			return RepoFacts{}, rpc.Fail("unknown_repo")
		}
		return mf.facts, nil
	}
	module.Perform = func(ctx context.Context, intent Intent, who string, peer rpc.Peer) (map[string]any, error) {
		mf.performed = append(mf.performed, intent)
		return map[string]any{"started": true, "action": intent.Action, "subject": intent.Repo, "message": "ok " + who}, nil
	}
	mf.module = module
	coretest.Start(t, core, module)
	mf.ui = coretest.NewPeer("ui", rpc.UI)
	core.Hub.Add(mf.ui)
	mf.phone = coretest.NewPeer("phone", rpc.LAN)
	mf.phone.Dev.User = "owner"
	return mf
}

func (mf *moduleFixture) call(peer rpc.Peer, method string, params any) (map[string]any, error) {
	raw, err := coretest.Call(mf.t, mf.core, peer, method, params)
	if err != nil {
		return nil, err
	}
	out := map[string]any{}
	if err := json.Unmarshal(raw, &out); err != nil {
		mf.t.Fatal(err)
	}
	return out, nil
}

func (mf *moduleFixture) mustCall(peer rpc.Peer, method string, params any) map[string]any {
	mf.t.Helper()
	out, err := mf.call(peer, method, params)
	if err != nil {
		mf.t.Fatalf("%s: %v", method, err)
	}
	return out
}

func (mf *moduleFixture) expectError(peer rpc.Peer, method string, params any, code string) {
	mf.t.Helper()
	if _, err := mf.call(peer, method, params); rpc.Code(err) != code {
		mf.t.Fatalf("%s %v: got %v, want %s", method, params, err, code)
	}
}

func TestCoreStartStreamsEventsAndKeepsFinished(t *testing.T) {
	workflow := "name: CI\non: [push, workflow_dispatch]\nsteps:\n  - name: Hello\n    run: echo hello-from-core\n"
	mf := newModuleFixture(t, workflow)
	run := mf.run(70, workflow)
	run.Status = StatusQueued
	params := map[string]any{"run": run.Record(), "workflow": map[string]any{"path": run.WorkflowPath, "content": run.WorkflowContent},
		"mirror": mf.mirror, "worktree": mf.work, "variables": map[string]string{}, "root": mf.store.Root}
	mf.expectError(mf.phone, "core.actions.start", params, "forbidden")
	started := mf.mustCall(mf.ui, "core.actions.start", params)
	if started["started"] != true {
		t.Fatal(started)
	}
	mf.expectError(mf.ui, "core.actions.start", params, "already_running")
	if !mf.module.Busy() {
		t.Fatal("a running Action keeps the core busy")
	}
	var finished map[string]any
	deadline := time.After(60 * time.Second)
	sawLog := false
	for finished == nil {
		select {
		case event := <-mf.ui.Events:
			var data map[string]any
			_ = json.Unmarshal(event.Data, &data)
			if event.Name == "actions.log" && strings.Contains(data["text"].(string), "hello-from-core") {
				sawLog = true
			}
			if event.Name == "actions.finished" {
				finished = data
			}
		case <-deadline:
			t.Fatal("no finish")
		}
	}
	if !sawLog || finished["ok"] != true || finished["record"].(map[string]any)["status"] != StatusSuccess {
		t.Fatalf("finished: %v log %v", finished, sawLog)
	}
	active := mf.mustCall(mf.ui, "core.actions.active", nil)
	if len(active["active"].([]any)) != 0 || len(active["finished"].([]any)) != 1 {
		t.Fatalf("active: %v", active)
	}
	mf.mustCall(mf.ui, "core.actions.ack", map[string]any{"runs": []int{70}})
	active = mf.mustCall(mf.ui, "core.actions.active", nil)
	if len(active["finished"].([]any)) != 0 || mf.module.Busy() {
		t.Fatalf("ack: %v", active)
	}
	logReply := mf.mustCall(mf.ui, "core.actions.log", map[string]any{"run": 70})
	if !strings.Contains(logReply["text"].(string), "hello-from-core") {
		t.Fatal(logReply)
	}
	runs := mf.mustCall(mf.ui, "core.actions.runs", map[string]any{"owner": "OWNER", "name": "repo"})
	if len(runs["runs"].([]any)) != 1 {
		t.Fatal(runs)
	}
	parsed := mf.mustCall(mf.ui, "core.actions.parse", map[string]any{"path": ".forkmesh/x.yml", "content": "on: push\n"})
	if parsed["error"] != "workflow has no runnable steps" {
		t.Fatal(parsed)
	}
	listing := mf.mustCall(mf.ui, "core.actions.workflows", map[string]any{"mirrorPath": mf.mirror})
	if len(listing["workflows"].([]any)) != 1 {
		t.Fatal(listing)
	}
	digest := mf.mustCall(mf.ui, "core.actions.digest", map[string]any{"repository": mf.mirror, "commit": mf.commit})
	if digest["executionDigest"] != run.ExecutionDigest {
		t.Fatal(digest)
	}
	mismatch := params
	mismatch["root"] = t.TempDir()
	mf.expectError(mf.ui, "core.actions.start", mismatch, "store_mismatch")
}

func TestCoreStopInterruptsActiveRuns(t *testing.T) {
	workflow := "name: Long\non: push\nsteps:\n  - run: echo long-started; sleep 30\n"
	mf := newModuleFixture(t, workflow)
	run := mf.run(71, workflow)
	mf.mustCall(mf.ui, "core.actions.start", map[string]any{"run": run.Record(), "mirror": mf.mirror})
	deadline := time.Now().Add(30 * time.Second)
	for !strings.Contains(mf.store.ReadLog(run), "long-started") && time.Now().Before(deadline) {
		time.Sleep(20 * time.Millisecond)
	}
	active := mf.mustCall(mf.ui, "core.actions.active", nil)
	if len(active["active"].([]any)) != 1 || len(active["sandboxes"].([]any)) != 1 {
		t.Fatalf("active: %v", active)
	}
	mf.module.Stop()
	stored := mf.stored(71)
	if stored.Status != StatusRunning || !strings.Contains(mf.store.ReadLog(run), "INTERRUPTED") || mf.module.Busy() {
		t.Fatalf("interrupted: %+v", stored)
	}
}

func TestCoreStopMethod(t *testing.T) {
	workflow := "name: Long\non: push\nsteps:\n  - run: echo begun; sleep 30\n"
	mf := newModuleFixture(t, workflow)
	run := mf.run(72, workflow)
	mf.mustCall(mf.ui, "core.actions.start", map[string]any{"run": run.Record(), "mirror": mf.mirror})
	deadline := time.Now().Add(30 * time.Second)
	for !strings.Contains(mf.store.ReadLog(run), "begun") && time.Now().Before(deadline) {
		time.Sleep(20 * time.Millisecond)
	}
	if reply := mf.mustCall(mf.ui, "core.actions.stop", map[string]any{"run": 72}); reply["stopping"] != true {
		t.Fatal(reply)
	}
	for mf.module.Busy() && time.Now().Before(deadline) {
		time.Sleep(20 * time.Millisecond)
	}
	if stored := mf.stored(72); stored.Status != StatusCancelled {
		t.Fatalf("stopped: %+v", stored)
	}
	if reply := mf.mustCall(mf.ui, "core.actions.stop", map[string]any{"run": 72}); reply["stopping"] != false {
		t.Fatal(reply)
	}
}

func (mf *moduleFixture) seed(id int, status string, created int64) Run {
	run := NewRun()
	run.ID = id
	run.Owner = "owner"
	run.Name = "repo"
	run.WorkflowPath = ".forkmesh/ci.yml"
	run.WorkflowName = "CI"
	run.WorkflowContent = "name: CI\non: push\nsteps:\n  - name: Build\n    run: make\n  - run: |\n      make test\n      echo done\n"
	run.Commit = mf.commit
	run.Ref = "refs/heads/main"
	run.Status = status
	run.CreatedAtMs = created
	run.StepStatuses = []string{StepSuccess}
	if err := mf.store.SaveRun(run); err != nil {
		mf.t.Fatal(err)
	}
	return run
}

func TestPhoneActionsListAndLog(t *testing.T) {
	workflow := "name: CI\non: [push, workflow_dispatch]\nruns-on: elsewhere\nsteps:\n  - run: echo x\n"
	mf := newModuleFixture(t, workflow)
	mf.seed(1, StatusSuccess, 1000)
	awaiting := mf.seed(2, StatusAwaitingApproval, 3000)
	mf.seed(3, StatusRunning, 2000)
	mf.store.AppendLog(awaiting, "first line")
	mf.store.AppendLog(awaiting, "second \U0001F680 line")
	stranger := coretest.NewPeer("stranger", rpc.LAN)
	stranger.Dev.User = "someone"
	mf.expectError(stranger, "repos.actions", map[string]any{"repo": "owner/repo"}, "not_owner")
	mf.expectError(mf.phone, "repos.actions", map[string]any{"repo": "nope"}, "bad_request")
	mf.expectError(mf.phone, "repos.actions", map[string]any{"repo": "other/repo"}, "unknown_repo")
	reply := mf.mustCall(mf.phone, "repos.actions", map[string]any{"repo": "owner/repo"})
	if reply["repo"] != "owner/repo" || reply["enabled"] != true || reply["mirror"] != true || reply["autoApprove"] != false {
		t.Fatal(reply)
	}
	runs := reply["runs"].([]any)
	if len(runs) != 3 || runs[0].(map[string]any)["id"] != 2.0 || runs[2].(map[string]any)["id"] != 1.0 {
		t.Fatalf("runs newest first: %v", runs)
	}
	steps := runs[0].(map[string]any)["steps"].([]any)
	if len(steps) != 2 || steps[0].(map[string]any)["name"] != "Build" || steps[1].(map[string]any)["name"] != "make test" || steps[1].(map[string]any)["status"] != StepPending {
		t.Fatalf("steps: %v", steps)
	}
	workflows := reply["workflows"].([]any)
	row := workflows[0].(map[string]any)
	if len(workflows) != 1 || row["path"] != ".forkmesh/ci.yml" || row["manual"] != true || row["here"] != false || row["runsOn"] != "elsewhere" {
		t.Fatalf("workflows: %v", workflows)
	}
	if triggers := row["triggers"].([]any); len(triggers) != 2 || triggers[0] != "push" || triggers[1] != "manual" {
		t.Fatalf("triggers: %v", triggers)
	}
	branches := reply["branches"].([]any)
	if len(branches) != 1 || branches[0] != "main" {
		t.Fatalf("branches: %v", branches)
	}
	summary := reply["summary"].(map[string]any)
	if summary["running"] != 1.0 || summary["awaiting"] != 1.0 || summary["latestStatus"] != StatusAwaitingApproval {
		t.Fatalf("summary: %v", summary)
	}
	mf.facts.WorkflowNodes = []string{".forkmesh/ci.yml\tthis-node"}
	reply = mf.mustCall(mf.phone, "repos.actions", map[string]any{"repo": "owner/repo"})
	if row := reply["workflows"].([]any)[0].(map[string]any); row["here"] != true || row["runsOn"] != "this-node" {
		t.Fatalf("pinned: %v", row)
	}

	mf.expectError(mf.phone, "repos.actions.log", map[string]any{"repo": "owner/repo", "run": 99}, "unknown_run")
	log := mf.mustCall(mf.phone, "repos.actions.log", map[string]any{"repo": "Owner/repo", "run": 2})
	if log["repo"] != "Owner/repo" || log["status"] != StatusAwaitingApproval || log["text"] != "first line\nsecond \U0001F680 line\n" || log["reset"] != false || log["from"] != 0.0 {
		t.Fatalf("log: %v", log)
	}
	total := log["next"].(float64)
	if total != float64(utf16Len("first line\nsecond \U0001F680 line\n")) {
		t.Fatalf("next counts UTF-16 units: %v", total)
	}
	again := mf.mustCall(mf.phone, "repos.actions.log", map[string]any{"repo": "owner/repo", "run": 2, "since": total})
	if again["text"] != "" || again["from"] != total {
		t.Fatal(again)
	}
	reset := mf.mustCall(mf.phone, "repos.actions.log", map[string]any{"repo": "owner/repo", "run": 2, "since": total + 50})
	if reset["reset"] != true || reset["from"] != 0.0 {
		t.Fatal(reset)
	}
}

func TestPhonePrepareAndRun(t *testing.T) {
	workflow := "name: CI\non: [workflow_dispatch]\nsteps:\n  - run: echo x\n"
	mf := newModuleFixture(t, workflow)
	mf.seed(5, StatusAwaitingApproval, 1000)
	mf.seed(6, StatusSuccess, 2000)
	repo := "owner/repo"
	mf.expectError(mf.phone, "repos.prepare", map[string]any{"repo": repo, "action": "run.explode"}, "unsupported_action")
	mf.expectError(mf.phone, "repos.prepare", map[string]any{"repo": repo, "action": "run.approve", "run": 77}, "unknown_run")
	mf.expectError(mf.phone, "repos.prepare", map[string]any{"repo": repo, "action": "run.approve", "run": 6}, "wrong_state")
	mf.expectError(mf.phone, "repos.prepare", map[string]any{"repo": repo, "action": "workflow.disable", "path": "../x.yml"}, "bad_request")
	mf.expectError(mf.phone, "repos.prepare", map[string]any{"repo": repo, "action": "workflow.run", "path": ".forkmesh/ci.yml", "branch": "-x"}, "bad_request")
	mf.facts.DisabledWorkflows = []string{".forkmesh/ci.yml"}
	mf.expectError(mf.phone, "repos.prepare", map[string]any{"repo": repo, "action": "workflow.run", "path": ".forkmesh/ci.yml", "branch": "main"}, "workflow_disabled")
	mf.facts.MirrorPath = ""
	mf.expectError(mf.phone, "repos.prepare", map[string]any{"repo": repo, "action": "workflow.run", "path": ".forkmesh/ci.yml", "branch": "main"}, "no_mirror")
	mf.facts.Enabled = false
	mf.expectError(mf.phone, "repos.prepare", map[string]any{"repo": repo, "action": "workflow.run", "path": ".forkmesh/ci.yml", "branch": "main"}, "actions_disabled")
	mf.facts.Enabled, mf.facts.MirrorPath, mf.facts.DisabledWorkflows = true, mf.mirror, nil

	prepared := mf.mustCall(mf.phone, "repos.prepare", map[string]any{"repo": " owner/repo ", "action": "run.approve", "run": 5})
	token := prepared["token"].(string)
	if len(token) != 48 || prepared["subject"] != repo || prepared["expiresInMs"] != 60000.0 ||
		prepared["summary"] != "Approve run #5, \"CI\" of owner/repo. The desktop checks the repository has not changed, then runs it." {
		t.Fatalf("prepared: %v", prepared)
	}
	other := coretest.NewPeer("other", rpc.LAN)
	other.Dev.User = "owner"
	mf.expectError(other, "repos.run", map[string]any{"token": token}, "confirmation_mismatch")
	mf.expectError(mf.phone, "repos.run", map[string]any{"token": token}, "confirmation_required")

	prepared = mf.mustCall(mf.phone, "repos.prepare", map[string]any{"repo": repo, "action": "workflow.run", "path": ".forkmesh/ci.yml", "branch": "main"})
	result := mf.mustCall(mf.phone, "repos.run", map[string]any{"token": prepared["token"]})
	if result["started"] != true || len(mf.performed) != 1 {
		t.Fatal(result)
	}
	want := Intent{Action: "workflow.run", Repo: repo, Path: ".forkmesh/ci.yml", Branch: "main"}
	if mf.performed[0] != want || result["message"] != "ok phone (owner), on the local network" {
		t.Fatalf("performed %+v %v", mf.performed[0], result)
	}
	mf.core.Set("control.owners", []string{"someone-else"})
	prepared = mf.mustCall(mf.phone, "repos.prepare", map[string]any{"repo": repo, "action": "workflow.enable", "path": ".forkmesh/ci.yml"})
	mf.expectError(mf.phone, "repos.run", map[string]any{"token": prepared["token"]}, "not_owner")
	mf.core.Set("control.owners", []string{"Owner"})
	mf.mustCall(mf.phone, "repos.run", map[string]any{"token": prepared["token"]})
	expired := newConfirmations()
	token, _ = expired.issue("k", Intent{Action: "run.stop"}, 0)
	if _, failure := expired.consume(token, "k", confirmationLifetime); failure != "confirmation_expired" {
		t.Fatal(failure)
	}
	for i := 0; i < maxOutstanding+5; i++ {
		expired.issue("k", Intent{Action: "run.stop"}, int64(i))
	}
	if expired.outstanding() != maxOutstanding {
		t.Fatal(expired.outstanding())
	}
}
