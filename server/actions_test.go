package mirrornode

import (
	"context"
	"crypto/ed25519"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

const ciWorkflow = `name: CI
on:
  push:
  workflow_dispatch:
env:
  GREETING: hello
steps:
  - name: Greet
    run: |
      echo "$GREETING from $FORKMESH_REPO at $FORKMESH_REF"
      test -f README.md
  - name: Second
    run: echo second step
`

type actionsFixture struct {
	t        *testing.T
	daemon   *Daemon
	runner   *actionsRunner
	source   string
	bare     string
	router   ed25519.PrivateKey
	routerID string
}

// newActionsFixture builds a daemon serving one repository and a runner whose
// sandbox is a plain `env -i` in the exported tree, so the run machinery is
// exercised on any machine. The real bubblewrap sandbox has its own test.
func newActionsFixture(t *testing.T) *actionsFixture {
	t.Helper()
	source, bare := makeBareRepo(t)
	identity, _ := testIdentity(t)
	public, private, err := ed25519.GenerateKey(nil)
	if err != nil {
		t.Fatal(err)
	}
	var gateway GatewayConfig
	gateway.Node.Name = "mirror9"
	gateway.Node.PublicKey = identity.PublicKey()
	gateway.RouterPublicKey = base64.RawURLEncoding.EncodeToString(public)
	gateway.Repositories = []Repository{{Owner: "alice", Name: "proj",
		Visibility: "public", Enabled: true, GitDir: bare}}
	directory := filepath.Join(t.TempDir(), "mirror-gateway")
	if err := os.MkdirAll(directory, 0o700); err != nil {
		t.Fatal(err)
	}
	daemon := &Daemon{
		config:   Config{GatewayConfig: filepath.Join(directory, "config.json"), EnableActions: true},
		gateway:  gateway,
		identity: identity,
	}
	runner := newActionsRunner(daemon)
	runner.probeSandbox = func(context.Context) error { return nil }
	runner.sandbox = func(work string, env []string) (string, []string, error) {
		args := []string{"-i", "-C", filepath.Join(work, "src"), "PATH=/usr/bin:/bin"}
		return "env", append(args, env...), nil
	}
	daemon.actions = runner
	ctx, cancel := context.WithCancel(context.Background())
	t.Cleanup(func() { cancel(); runner.wait() })
	runner.runCtx = ctx
	if !runner.available(ctx) {
		t.Fatal("test sandbox should be available")
	}
	return &actionsFixture{t: t, daemon: daemon, runner: runner, source: source,
		bare: bare, router: private, routerID: gateway.RouterPublicKey}
}

// push commits files in the source checkout and pushes the branch to the
// served bare repository, returning the new commit.
func (f *actionsFixture) push(branch string, files map[string]string) string {
	f.t.Helper()
	run(f.t, f.source, "git", "checkout", "-q", "-B", branch)
	for name, content := range files {
		path := filepath.Join(f.source, name)
		if err := os.MkdirAll(filepath.Dir(path), 0o700); err != nil {
			f.t.Fatal(err)
		}
		if err := os.WriteFile(path, []byte(content), 0o600); err != nil {
			f.t.Fatal(err)
		}
		run(f.t, f.source, "git", "add", name)
	}
	run(f.t, f.source, "git", "commit", "-q", "-m", "change "+branch)
	run(f.t, f.source, "git", "push", "-q", "-f", f.bare, branch)
	return run(f.t, f.source, "git", "rev-parse", "HEAD")
}

func (f *actionsFixture) runs() []actionRun {
	f.runner.mu.Lock()
	defer f.runner.mu.Unlock()
	return append([]actionRun(nil), f.runner.state.Runs...)
}

func (f *actionsFixture) settle() {
	f.t.Helper()
	deadline := time.Now().Add(30 * time.Second)
	for time.Now().Before(deadline) {
		busy := false
		for _, run := range f.runs() {
			busy = busy || !run.terminal()
		}
		if !busy {
			return
		}
		f.runner.startNext()
		time.Sleep(20 * time.Millisecond)
	}
	f.t.Fatalf("runs did not settle: %#v", f.runs())
}

func (f *actionsFixture) log(id int64) string {
	raw, _ := os.ReadFile(f.runner.logPath(id))
	return string(raw)
}

// control writes one envelope exactly as the gateway would, signed by the
// fixture's router key, and lets the runner consume it.
func (f *actionsFixture) control(requestID string, body map[string]any) actionsControlOutcome {
	f.t.Helper()
	f.writeEnvelope(requestID, body, f.runner.now().UnixMilli(), nil)
	f.runner.scanControl(context.Background())
	f.runner.mu.Lock()
	defer f.runner.mu.Unlock()
	for _, outcome := range f.runner.state.Outcomes {
		if outcome.RequestID == requestID {
			return outcome
		}
	}
	return actionsControlOutcome{}
}

func (f *actionsFixture) writeEnvelope(requestID string, body map[string]any, issuedAt int64, mutate func(map[string]any)) string {
	f.t.Helper()
	document := map[string]any{"schemaVersion": 1, "type": actionsControlWireType,
		"requestId": requestID, "actor": "alice", "issuedAt": issuedAt}
	for key, value := range body {
		document[key] = value
	}
	encoded, _ := json.Marshal(document)
	sum := sha256.Sum256(encoded)
	digest := hex.EncodeToString(sum[:])
	route := "/v1/repositories/alice/proj/actions-control"
	capabilityID := "capability_" + requestID
	signature := ed25519.Sign(f.router,
		actionsCapabilityMessage("mirror9", route, digest, capabilityID, issuedAt))
	envelope := map[string]any{
		"schemaVersion": 1, "type": actionsControlEnvelopeType, "node": "mirror9",
		"owner": "alice", "repository": "proj", "receivedAt": issuedAt,
		"capability": map[string]any{
			"method": "POST", "path": route, "requestId": capabilityID,
			"issuedAt": issuedAt, "bodySha256": digest,
			"signature": base64.RawURLEncoding.EncodeToString(signature),
		},
		"body": string(encoded),
	}
	if mutate != nil {
		mutate(envelope)
	}
	raw, _ := json.Marshal(envelope)
	directory := f.runner.controlDirectory()
	if err := os.MkdirAll(directory, 0o700); err != nil {
		f.t.Fatal(err)
	}
	path := filepath.Join(directory, requestID+".json")
	if err := os.WriteFile(path, raw, 0o640); err != nil {
		f.t.Fatal(err)
	}
	return path
}

func TestActionsFirstSightRecordsHeadsWithoutRunningHistory(t *testing.T) {
	f := newActionsFixture(t)
	f.push("main", map[string]string{".forkmesh/ci.yml": ciWorkflow})
	f.runner.scanRefs(context.Background())
	if runs := f.runs(); len(runs) != 0 {
		t.Fatalf("a node that just joined must not run what it cloned: %#v", runs)
	}
}

func TestActionsPushRunsTheWorkflowInTheExportedCommit(t *testing.T) {
	f := newActionsFixture(t)
	f.runner.scanRefs(context.Background())
	commit := f.push("main", map[string]string{".forkmesh/ci.yml": ciWorkflow})
	f.runner.scanRefs(context.Background())
	f.settle()
	runs := f.runs()
	if len(runs) != 1 {
		t.Fatalf("runs = %#v", runs)
	}
	got := runs[0]
	if got.Status != actionSuccess || got.Commit != commit || got.Ref != "refs/heads/main" ||
		got.Event != "push" || got.WorkflowPath != ".forkmesh/ci.yml" || got.WorkflowName != "CI" {
		t.Fatalf("run = %#v\n%s", got, f.log(got.ID))
	}
	if strings.Join(got.StepStatuses, ",") != "success,success" ||
		strings.Join(got.StepNames, ",") != "Greet,Second" {
		t.Fatalf("steps = %v %v", got.StepNames, got.StepStatuses)
	}
	output := f.log(got.ID)
	for _, want := range []string{"hello from alice/proj at refs/heads/main",
		"second step", "==> SUCCESS"} {
		if !strings.Contains(output, want) {
			t.Fatalf("log lacks %q:\n%s", want, output)
		}
	}
	// The scratch tree is gone; only the log is kept.
	if _, err := os.Stat(filepath.Join(f.runner.runDirectory(got.ID), "work")); !os.IsNotExist(err) {
		t.Fatal("the run workspace must be removed")
	}
	// An unchanged repository queues nothing more.
	f.runner.scanRefs(context.Background())
	if len(f.runs()) != 1 {
		t.Fatal("no ref moved, so no new run")
	}
}

func TestActionsFailingStepFailsTheRunAndSkipsTheRest(t *testing.T) {
	f := newActionsFixture(t)
	f.runner.scanRefs(context.Background())
	f.push("main", map[string]string{".forkmesh/ci.yml": "on: push\nsteps:\n  - name: Break\n    run: |\n      echo before\n      exit 7\n  - name: Never\n    run: echo never\n"})
	f.runner.scanRefs(context.Background())
	f.settle()
	got := f.runs()[0]
	if got.Status != actionFailed || strings.Join(got.StepStatuses, ",") != "failed,skipped" {
		t.Fatalf("run = %#v", got)
	}
	if output := f.log(got.ID); !strings.Contains(output, "exited with status 7") ||
		strings.Contains(output, "never") {
		t.Fatalf("log:\n%s", output)
	}
}

func TestActionsPushFollowsDefaultBranchUnlessBranchesAreNamed(t *testing.T) {
	f := newActionsFixture(t)
	f.push("main", map[string]string{
		".forkmesh/ci.yml":    "on: push\nsteps:\n  - run: echo default-only\n",
		".forkmesh/topic.yml": "name: Topic\non:\n  push:\n    branches: [\"agent/**\"]\nsteps:\n  - run: echo topic\n",
		".forkmesh/paths.yml": "name: Paths\non:\n  push:\n    paths: [\"app/**\"]\nsteps:\n  - run: echo paths\n",
	})
	f.runner.scanRefs(context.Background())

	f.push("agent/x", map[string]string{"notes.txt": "topic work\n"})
	f.runner.scanRefs(context.Background())
	f.settle()
	names := func() string {
		var out []string
		for _, run := range f.runs() {
			out = append(out, run.WorkflowName+"@"+strings.TrimPrefix(run.Ref, "refs/heads/"))
		}
		return strings.Join(out, " ")
	}
	if names() != "Topic@agent/x" {
		t.Fatalf("a topic branch runs only workflows that name it: %q", names())
	}

	// The ledger branches this node writes itself are nobody's push.
	f.push("forkmesh/pulls", map[string]string{"ledger.txt": "x\n"})
	f.runner.scanRefs(context.Background())
	if names() != "Topic@agent/x" {
		t.Fatalf("internal refs must not trigger runs: %q", names())
	}

	f.push("main", map[string]string{"docs.txt": "not under app\n"})
	f.runner.scanRefs(context.Background())
	f.settle()
	if names() != "Topic@agent/x ci@main" {
		t.Fatalf("the path filter must hold back Paths: %q", names())
	}
	f.push("main", map[string]string{"app/x.py": "print(1)\n"})
	f.runner.scanRefs(context.Background())
	f.settle()
	if !strings.Contains(names(), "Paths@main") {
		t.Fatalf("a push under app/ runs Paths: %q", names())
	}
}

func TestActionsNeedsHoldsThenSkipsAfterAFailure(t *testing.T) {
	f := newActionsFixture(t)
	f.runner.scanRefs(context.Background())
	f.push("main", map[string]string{
		".forkmesh/a-ci.yml":   "name: CI tests\non: push\nsteps:\n  - run: exit 1\n",
		".forkmesh/b-ship.yml": "name: Deploy\non: push\nneeds: [CI tests]\nsteps:\n  - run: echo shipped\n",
	})
	f.runner.scanRefs(context.Background())
	f.settle()
	status := map[string]string{}
	for _, run := range f.runs() {
		status[run.WorkflowName] = run.Status
	}
	if status["CI tests"] != actionFailed || status["Deploy"] != actionSkipped {
		t.Fatalf("status = %v", status)
	}
}

func TestActionsControlDispatchRerunCancelAndSwitches(t *testing.T) {
	f := newActionsFixture(t)
	f.push("main", map[string]string{
		".forkmesh/ci.yml":   ciWorkflow,
		".forkmesh/push.yml": "name: Push only\non: push\nsteps:\n  - run: echo x\n",
	})
	f.runner.scanRefs(context.Background())

	dispatched := f.control("dispatch_request_0001", map[string]any{
		"action": "dispatch", "workflow": ".forkmesh/ci.yml", "ref": "refs/heads/main"})
	if dispatched.Status != "applied" || dispatched.RunID == 0 || dispatched.Reason != "" {
		t.Fatalf("dispatch = %#v", dispatched)
	}
	f.settle()
	first := f.runs()[0]
	if first.Status != actionSuccess || first.Event != "workflow_dispatch" || first.Actor != "alice" {
		t.Fatalf("dispatched run = %#v\n%s", first, f.log(first.ID))
	}

	for name, want := range map[string]struct {
		body   map[string]any
		reason string
	}{
		"not_manual_req_0001": {map[string]any{"action": "dispatch",
			"workflow": ".forkmesh/push.yml", "ref": "refs/heads/main"}, "not_manual"},
		"missing_ref_req_0001": {map[string]any{"action": "dispatch",
			"workflow": ".forkmesh/ci.yml", "ref": "refs/heads/nope"}, "ref_not_found"},
		"missing_wf_req_00001": {map[string]any{"action": "dispatch",
			"workflow": ".forkmesh/gone.yml", "ref": "refs/heads/main"}, "workflow_not_found"},
		"missing_run_req_0001": {map[string]any{"action": "cancel", "runId": 999}, "run_not_found"},
		"finished_run_req_001": {map[string]any{"action": "cancel", "runId": first.ID}, "not_cancellable"},
		"approve_run_req_0001": {map[string]any{"action": "approve", "runId": first.ID}, "not_awaiting_approval"},
	} {
		if got := f.control(name, want.body); got.Status != "rejected" || got.Reason != want.reason {
			t.Fatalf("%s = %#v, want %s", name, got, want.reason)
		}
	}

	rerun := f.control("rerun_request_000001", map[string]any{"action": "rerun", "runId": first.ID})
	if rerun.Status != "applied" || rerun.RunID == first.ID || rerun.RunID == 0 {
		t.Fatalf("rerun = %#v", rerun)
	}
	f.settle()

	// Disabling a workflow refuses its dispatch and shows in the catalog.
	if got := f.control("disable_wf_req_00001", map[string]any{"action": "set-workflow-enabled",
		"workflow": ".forkmesh/ci.yml", "enabled": false}); got.Status != "applied" {
		t.Fatalf("disable workflow = %#v", got)
	}
	if got := f.control("disabled_dispatch_01", map[string]any{"action": "dispatch",
		"workflow": ".forkmesh/ci.yml", "ref": "refs/heads/main"}); got.Reason != "workflow_disabled" {
		t.Fatalf("dispatch of a disabled workflow = %#v", got)
	}
	// Pausing the repository refuses everything that would start a run.
	f.control("enable_wf_req_000001", map[string]any{"action": "set-workflow-enabled",
		"workflow": ".forkmesh/ci.yml", "enabled": true})
	f.control("pause_repo_req_00001", map[string]any{"action": "set-repository-enabled", "enabled": false})
	if got := f.control("paused_dispatch_0001", map[string]any{"action": "dispatch",
		"workflow": ".forkmesh/ci.yml", "ref": "refs/heads/main"}); got.Reason != "actions_disabled" {
		t.Fatalf("dispatch while paused = %#v", got)
	}
	before := len(f.runs())
	f.push("main", map[string]string{"more.txt": "x\n"})
	f.runner.scanRefs(context.Background())
	if len(f.runs()) != before {
		t.Fatal("a paused repository must not queue push runs")
	}
}

func TestActionsControlCancelStopsARunningRun(t *testing.T) {
	f := newActionsFixture(t)
	f.push("main", map[string]string{".forkmesh/slow.yml": "name: Slow\non: workflow_dispatch\nsteps:\n  - name: Wait\n    run: |\n      echo started\n      sleep 60\n  - name: After\n    run: echo after\n"})
	f.runner.scanRefs(context.Background())
	started := f.control("slow_dispatch_req_01", map[string]any{"action": "dispatch",
		"workflow": ".forkmesh/slow.yml", "ref": "refs/heads/main"})
	deadline := time.Now().Add(10 * time.Second)
	for !strings.Contains(f.log(started.RunID), "started") {
		if time.Now().After(deadline) {
			t.Fatalf("the run never started:\n%s", f.log(started.RunID))
		}
		time.Sleep(20 * time.Millisecond)
	}
	if got := f.control("slow_cancel_req_0001", map[string]any{"action": "cancel",
		"runId": started.RunID}); got.Status != "applied" {
		t.Fatalf("cancel = %#v", got)
	}
	f.settle()
	got := f.runs()[0]
	if got.Status != actionCancelled || strings.Join(got.StepStatuses, ",") != "cancelled,cancelled" {
		t.Fatalf("run = %#v", got)
	}
}

func TestActionsControlIsConsumedOnceAndNeverReplayed(t *testing.T) {
	f := newActionsFixture(t)
	f.push("main", map[string]string{".forkmesh/ci.yml": ciWorkflow})
	f.runner.scanRefs(context.Background())
	body := map[string]any{"action": "dispatch", "workflow": ".forkmesh/ci.yml", "ref": "refs/heads/main"}
	path := f.writeEnvelope("replayed_request_001", body, f.runner.now().UnixMilli(), nil)
	saved, _ := os.ReadFile(path)
	f.runner.scanControl(context.Background())
	if _, err := os.Stat(path); !os.IsNotExist(err) {
		t.Fatal("a control file is removed as soon as it is read")
	}
	f.settle()
	// A courier that kept a copy cannot make it run again.
	if err := os.WriteFile(path, saved, 0o640); err != nil {
		t.Fatal(err)
	}
	f.runner.scanControl(context.Background())
	f.settle()
	if runs := f.runs(); len(runs) != 1 {
		t.Fatalf("a replayed request ran again: %#v", runs)
	}
	// The ring survives a restart.
	restarted := newActionsRunner(f.daemon)
	restarted.mu.Lock()
	restarted.loadLocked()
	seen := actionsControlSeen(restarted.state.Seen, "replayed_request_001")
	restarted.mu.Unlock()
	if !seen {
		t.Fatal("the seen ring must be persisted")
	}
}

func TestActionsSummaryIsTheGatewaysVersionTwoDocument(t *testing.T) {
	f := newActionsFixture(t)
	f.runner.scanRefs(context.Background())
	f.push("main", map[string]string{
		".forkmesh/ci.yml":     ciWorkflow,
		".forkmesh/broken.yml": "name: Broken\non: push\n",
	})
	f.runner.scanRefs(context.Background())
	f.settle()
	f.control("summary_dispatch_001", map[string]any{"action": "dispatch",
		"workflow": ".forkmesh/ci.yml", "ref": "refs/heads/main"})
	f.settle()
	f.runner.writeSummary(context.Background())

	path := f.runner.summaryPath()
	info, err := os.Stat(path)
	if err != nil || info.Mode().Perm() != 0o600 {
		t.Fatalf("summary mode = %v %v", info, err)
	}
	raw, _ := os.ReadFile(path)
	if len(raw) > maxActionsSummaryBytes {
		t.Fatalf("summary is %d bytes", len(raw))
	}
	var summary map[string]any
	if err := json.Unmarshal(raw, &summary); err != nil {
		t.Fatal(err)
	}
	if keys := sortedKeys(summary); strings.Join(keys, ",") !=
		"controls,expiresAt,node,repositories,runs,schemaVersion,type,updatedAt" {
		t.Fatalf("summary keys = %v", keys)
	}
	if summary["schemaVersion"].(float64) != 2 || summary["type"] != actionsSummaryType ||
		summary["node"] != "mirror9" {
		t.Fatalf("summary header = %v", summary)
	}
	run := summary["runs"].([]any)[0].(map[string]any)
	if keys := sortedKeys(run); strings.Join(keys, ",") !=
		"commit,createdAt,finishedAt,id,logTail,owner,pullNumber,ref,repository,startedAt,status,steps,workflow,workflowPath" {
		t.Fatalf("run keys = %v", keys)
	}
	repository := summary["repositories"].([]any)[0].(map[string]any)
	workflows := repository["workflows"].([]any)
	if repository["actionsEnabled"] != true || len(workflows) != 2 {
		t.Fatalf("repository = %v", repository)
	}
	valid := map[string]bool{}
	for _, item := range workflows {
		entry := item.(map[string]any)
		if keys := sortedKeys(entry); strings.Join(keys, ",") != "enabled,events,manual,name,path,runsOn,valid" {
			t.Fatalf("workflow keys = %v", keys)
		}
		valid[entry["path"].(string)] = entry["valid"].(bool)
	}
	if !valid[".forkmesh/ci.yml"] || valid[".forkmesh/broken.yml"] {
		t.Fatalf("valid = %v", valid)
	}
	control := summary["controls"].([]any)[0].(map[string]any)
	if keys := sortedKeys(control); strings.Join(keys, ",") !=
		"action,finishedAt,owner,reason,repository,requestId,runId,status" {
		t.Fatalf("control keys = %v", keys)
	}
	// Nothing a workflow author wrote but the names survives into the file.
	for _, private := range []string{"GREETING", "echo ", f.bare, f.runner.dir} {
		if strings.Contains(string(raw), private) && private != "echo " {
			t.Fatalf("summary leaks %q", private)
		}
	}
	if strings.Contains(string(raw), "test -f README.md") {
		t.Fatal("summary must not carry step commands")
	}
}

func sortedKeys(object map[string]any) []string {
	keys := make([]string, 0, len(object))
	for key := range object {
		keys = append(keys, key)
	}
	// insertion sort keeps this helper dependency-free and obvious
	for i := 1; i < len(keys); i++ {
		for j := i; j > 0 && keys[j] < keys[j-1]; j-- {
			keys[j], keys[j-1] = keys[j-1], keys[j]
		}
	}
	return keys
}

func TestActionsSummaryFitsItsBoundByShorteningLogTails(t *testing.T) {
	f := newActionsFixture(t)
	f.push("main", map[string]string{".forkmesh/ci.yml": ciWorkflow})
	f.runner.scanRefs(context.Background())
	repo := f.daemon.gateway.Repositories[0]
	wf, err := parseWorkflow(".forkmesh/ci.yml", ciWorkflow)
	if err != nil {
		t.Fatal(err)
	}
	commit := run(t, f.source, "git", "rev-parse", "HEAD")
	f.runner.mu.Lock()
	f.runner.sandboxOK = false // keep them queued: this is about size only
	f.runner.mu.Unlock()
	noisy := strings.Repeat("\"quoted\\ line\" with \x01 control bytes and \u00e9\n", 2000)
	for i := 0; i < maxActionsSummaryRuns; i++ {
		id := f.runner.createRun(repo, wf, commit, "refs/heads/b"+string(rune('a'+i)), "workflow_dispatch", "alice")
		f.runner.appendLog(id, noisy)
	}
	summary := f.runner.buildSummary(context.Background())
	if summary == nil {
		t.Fatal("a summary of full log tails must be fitted, not dropped")
	}
	encoded, _ := json.Marshal(summary)
	if len(encoded)+1 > maxActionsSummaryBytes {
		t.Fatalf("summary is %d bytes", len(encoded))
	}
	if len(summary["runs"].([]map[string]any)) != maxActionsSummaryRuns {
		t.Fatal("runs are kept; only their tails shrink")
	}
}

func TestActionsRestartCancelsTheRunThatWasInFlight(t *testing.T) {
	f := newActionsFixture(t)
	f.runner.mu.Lock()
	f.runner.loadLocked()
	f.runner.state.Runs = []actionRun{{ID: 4, Owner: "alice", Repository: "proj",
		WorkflowName: "CI", Status: actionRunning, StepNames: []string{"a", "b"},
		StepStatuses: []string{stepSuccess, stepRunning}}}
	f.runner.state.NextRunID = 5
	f.runner.saveLocked()
	f.runner.mu.Unlock()
	restarted := newActionsRunner(f.daemon)
	restarted.mu.Lock()
	restarted.loadLocked()
	got := restarted.state.Runs[0]
	restarted.mu.Unlock()
	if got.Status != actionCancelled || strings.Join(got.StepStatuses, ",") != "success,cancelled" {
		t.Fatalf("run = %#v", got)
	}
}

func TestActionsWithoutASandboxRunNothingAndAdvertiseNothing(t *testing.T) {
	f := newActionsFixture(t)
	// The shipped gateway script knows the Actions keys; one that predates
	// them must never be handed a configuration naming them.
	current, err := filepath.Abs(filepath.Join("..", "tools", "mirror_gateway.py"))
	if err != nil {
		t.Fatal(err)
	}
	stale := filepath.Join(t.TempDir(), "mirror_gateway.py")
	if err := os.WriteFile(stale, []byte("# a gateway from before actions\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	if !gatewayScriptServesActions(current) || gatewayScriptServesActions(stale) ||
		gatewayScriptServesActions(filepath.Join(t.TempDir(), "missing.py")) {
		t.Fatal("gateway script detection is wrong")
	}
	f.daemon.config.GatewayScript = stale
	if f.runner.ready(context.Background()) {
		t.Fatal("a stale gateway script must keep Actions unadvertised")
	}
	f.daemon.config.GatewayScript = current
	if !f.runner.ready(context.Background()) {
		t.Fatal("a current script and a working sandbox are ready")
	}
	broken := newActionsRunner(f.daemon)
	broken.probeSandbox = func(context.Context) error { return os.ErrPermission }
	if broken.ready(context.Background()) {
		t.Fatal("a node that cannot sandbox must not be ready")
	}
	gateway := f.daemon.gateway
	gateway.Repositories[0].Operations = []string{"tree", "actions-status", "actions-control"}
	gateway.ActionsSummaryPath, gateway.ActionsControlPath = "/stale", "/stale"
	if err := ensureGatewayActions(&gateway, f.daemon.config.GatewayConfig, false); err != nil {
		t.Fatal(err)
	}
	if strings.Join(gateway.Repositories[0].Operations, ",") != "tree" ||
		gateway.ActionsSummaryPath != "" || gateway.ActionsControlPath != "" {
		t.Fatalf("operations were not withdrawn: %#v", gateway)
	}
	if err := ensureGatewayActions(&gateway, f.daemon.config.GatewayConfig, true); err != nil {
		t.Fatal(err)
	}
	if strings.Join(gateway.Repositories[0].Operations, ",") != "actions-control,actions-status,tree" {
		t.Fatalf("operations = %v", gateway.Repositories[0].Operations)
	}
	info, err := os.Stat(gateway.ActionsControlPath)
	if err != nil || info.Mode().Perm() != 0o700 ||
		filepath.Base(gateway.ActionsControlPath) != actionsControlDirectoryName ||
		filepath.Base(gateway.ActionsSummaryPath) != actionsSummaryFileName {
		t.Fatalf("control directory = %v %v", info, err)
	}
	// The daemon's own strict loader accepts what it just wrote. Before the
	// two path fields existed, DisallowUnknownFields made a configuration that
	// named them fatal at the next start.
	gateway.SchemaVersion = 1
	if _, err := writeGatewayConfig(f.daemon.config.GatewayConfig, gateway); err != nil {
		t.Fatal(err)
	}
	loaded, _, err := loadGatewayConfig(f.daemon.config.GatewayConfig)
	if err != nil {
		t.Fatalf("a configuration naming the Actions paths must load: %v", err)
	}
	if loaded.ActionsControlPath != gateway.ActionsControlPath ||
		loaded.ActionsSummaryPath != gateway.ActionsSummaryPath {
		t.Fatalf("paths did not round-trip: %#v", loaded)
	}
}

// The real sandbox, where this machine can build one.
func TestActionsBubblewrapHidesTheNodeAndTheNetwork(t *testing.T) {
	f := newActionsFixture(t)
	real := newActionsRunner(f.daemon)
	if err := real.probeBubblewrap(context.Background()); err != nil {
		t.Skipf("no usable bubblewrap here: %v", err)
	}
	f.runner.sandbox = real.bubblewrap
	secret := filepath.Join(filepath.Dir(f.daemon.config.GatewayConfig), "identity-secret.pem")
	if err := os.WriteFile(secret, []byte("PRIVATE"), 0o600); err != nil {
		t.Fatal(err)
	}
	t.Setenv("FORKMESH_TEST_SECRET", "leaked-from-daemon-environment")
	f.push("main", map[string]string{".forkmesh/probe.yml": `name: Probe
on: workflow_dispatch
env:
  PATH: /evil
  LD_PRELOAD: /evil.so
  SAFE_NAME: kept
steps:
  - name: Look around
    run: |
      echo "secret-env=[${FORKMESH_TEST_SECRET:-}]"
      echo "safe=[$SAFE_NAME] path=[$PATH] preload=[${LD_PRELOAD:-}]"
      echo "pwd=$(pwd) home=$HOME user=$(id -un)"
      if [ -e "` + secret + `" ]; then echo "SEES-NODE-STATE"; fi
      if [ -e "` + f.bare + `" ]; then echo "SEES-SERVED-REPOSITORY"; fi
      if touch /usr/forkmesh-probe 2>/dev/null; then echo "USR-WRITABLE"; fi
      routes=$(tail -n +2 /proc/net/route | wc -l)
      echo "routes=$routes"
      test -f README.md && echo "has-checkout"
`})
	f.runner.scanRefs(context.Background())
	outcome := f.control("sandbox_probe_req_01", map[string]any{"action": "dispatch",
		"workflow": ".forkmesh/probe.yml", "ref": "refs/heads/main"})
	f.settle()
	output := f.log(outcome.RunID)
	for _, want := range []string{"secret-env=[]", "safe=[kept]",
		"path=[/usr/local/bin:/usr/bin:/bin]", "preload=[]", "pwd=/workspace",
		"home=/home/forkmesh", "user=forkmesh", "routes=0", "has-checkout", "==> SUCCESS"} {
		if !strings.Contains(output, want) {
			t.Fatalf("sandbox log lacks %q:\n%s", want, output)
		}
	}
	for _, forbidden := range []string{"SEES-NODE-STATE", "SEES-SERVED-REPOSITORY",
		"USR-WRITABLE", "leaked-from-daemon-environment"} {
		if strings.Contains(output, forbidden) {
			t.Fatalf("sandbox breach %q:\n%s", forbidden, output)
		}
	}
}
