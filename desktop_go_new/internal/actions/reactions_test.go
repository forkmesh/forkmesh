package actions

import (
	"context"
	"encoding/json"
	"strings"
	"sync"
	"testing"
	"time"

	"forkmesh.com/desktop-core/internal/rpc"
)

func (qf *queueFixture) waitEvent(t *testing.T, name string, match func(map[string]any) bool) map[string]any {
	t.Helper()
	deadline := time.After(90 * time.Second)
	for {
		select {
		case event := <-qf.ui.Events:
			if event.Name != name {
				continue
			}
			data := map[string]any{}
			_ = json.Unmarshal(event.Data, &data)
			if match == nil || match(data) {
				return data
			}
		case <-deadline:
			t.Fatalf("no %s event; log:\n%s", name, qf.logText())
		}
	}
}

func TestReleaseLandingReachesTheServedMirrorInTheCore(t *testing.T) {
	workflow := "name: Release\non: [release]\nsteps:\n  - run: |\n      mkdir -p .forkmesh/releases\n      echo manifest > .forkmesh/releases/v1.json\n      echo blob > \"$FAKE_RELEASE/blob\"\n"
	qf := newQueueFixture(t, workflow, nil)
	qf.mustCall(qf.ui, "core.actions.queue.attach", qf.snapshot(true))
	qf.mustCall(qf.ui, "core.actions.queue.commit", map[string]any{"owner": "owner", "name": "repo", "commit": qf.commit,
		"ref": "refs/tags/v0.2.0", "trigger": "release"})
	finished := qf.waitEvent(t, "actions.finished", nil)
	if finished["ok"] != true || finished["landed"] != true {
		run, _ := qf.store.FindRun(1)
		t.Fatalf("finished %v\n%s\n%s", finished, qf.logText(), qf.store.ReadLog(run))
	}
	served, _ := finished["served"].(map[string]any)
	if served["ok"] != true || served["skipped"] == true {
		t.Fatalf("served %v", finished)
	}
	work := runGit(t, qf.work, "rev-parse", "HEAD")
	if runGit(t, qf.work, "log", "-1", "--format=%s") != "release: publish v0.2.0 artifacts" {
		t.Fatal("no release metadata commit in the working copy")
	}
	if mirror := runGit(t, qf.mirror, "rev-parse", "refs/heads/main"); mirror != work {
		t.Fatalf("served mirror main %s, working copy %s", mirror, work)
	}
	active := qf.mustCall(qf.ui, "core.actions.active", nil)
	records := active["finished"].([]any)
	record := records[len(records)-1].(map[string]any)
	if record["landed"] != true || record["served"].(map[string]any)["ok"] != true || record["autoFix"] == true {
		t.Fatalf("finished record %v", record)
	}
}

func TestFailedQueueRunHandsItsFailureToTheAgentThroughTheCore(t *testing.T) {
	workflow := "name: CI\non: [push]\nsteps:\n  - name: Break\n    run: |\n      echo broken-build-line\n      exit 3\n"
	qf := newQueueFixture(t, workflow, nil)
	var mu sync.Mutex
	var delivered []AutoFixRequest
	desktopUp := false
	qf.module.AutoFix = func(ctx context.Context, request AutoFixRequest) error {
		mu.Lock()
		defer mu.Unlock()
		if !desktopUp {
			return rpc.Fail("desktop_unavailable")
		}
		delivered = append(delivered, request)
		return nil
	}
	qf.spoolPush(t, strings.Repeat("0", 40))
	qf.mustCall(qf.ui, "core.actions.queue.attach", qf.snapshot(true))
	finished := qf.waitEvent(t, "actions.finished", nil)
	if finished["ok"] != false || finished["autoFix"] != true {
		t.Fatalf("finished %v", finished)
	}
	deadline := time.Now().Add(10 * time.Second)
	for len(qf.module.pendingAutoFix()) == 0 && time.Now().Before(deadline) {
		time.Sleep(20 * time.Millisecond)
	}
	pending := qf.module.pendingAutoFix()
	if len(pending) != 1 {
		t.Fatalf("pending %v", pending)
	}
	mu.Lock()
	desktopUp = true
	mu.Unlock()
	qf.mustCall(qf.ui, "core.actions.queue.attach", qf.snapshot(true))
	deadline = time.Now().Add(10 * time.Second)
	for time.Now().Before(deadline) {
		mu.Lock()
		count := len(delivered)
		mu.Unlock()
		if count > 0 {
			break
		}
		time.Sleep(20 * time.Millisecond)
	}
	mu.Lock()
	defer mu.Unlock()
	if len(delivered) != 1 {
		t.Fatalf("delivered %v", delivered)
	}
	request := delivered[0]
	run, _ := qf.store.FindRun(request.Run)
	if request.Owner != "owner" || request.Name != "repo" || request.Branch != "main" || request.Workflow != "CI" {
		t.Fatalf("request %+v", request)
	}
	head := "The \"CI\" CI workflow failed for owner/repo (commit " + qtLeft(qf.commit, 8) + ", ref refs/heads/main). Find what broke and fix it so the workflow succeeds.\n" +
		"\nFull run log: " + qf.store.LogPath(run) + "\nRead that whole file on this machine; what follows is only its tail.\n\nLog excerpt:\n\n"
	if !strings.HasPrefix(request.Prompt, head) || !strings.Contains(request.Prompt, "broken-build-line") {
		t.Fatalf("prompt %q", request.Prompt)
	}
	if len(qf.module.pendingAutoFix()) != 0 {
		t.Fatal("the delivered auto-fix stayed pending")
	}
}

func TestAutoFixPromptKeepsTheTail(t *testing.T) {
	run := Run{WorkflowName: "CI", Owner: "o", Name: "r", Commit: "0123456789abcdef", Ref: "refs/heads/topic"}
	log := strings.Repeat("é", autoFixLogChars) + "END"
	prompt := AutoFixPrompt(run, log, "")
	if !strings.Contains(prompt, "\nLog excerpt:\n\n"+autoFixTruncatedLabel) || !strings.HasSuffix(prompt, "END") ||
		strings.Contains(prompt, "Full run log") || utf16Len(prompt[strings.Index(prompt, autoFixTruncatedLabel)+len(autoFixTruncatedLabel):]) != autoFixLogChars {
		t.Fatal(prompt[:200])
	}
	if autoFixBranch("refs/heads/a/b") != "a/b" || autoFixBranch("refs/tags/v1") != "refs/tags/v1" {
		t.Fatal("branch")
	}
}
