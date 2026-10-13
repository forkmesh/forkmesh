package mirrornode

import (
	"context"
	"encoding/json"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"
)

func runnerActionsRepo(t *testing.T, files map[string]string) (string, string) {
	t.Helper()
	dir := t.TempDir()
	run := func(args ...string) string {
		command := exec.Command("git", args...)
		command.Dir = dir
		command.Env = append(os.Environ(), "GIT_CONFIG_GLOBAL=/dev/null", "GIT_CONFIG_NOSYSTEM=1",
			"GIT_AUTHOR_NAME=t", "GIT_AUTHOR_EMAIL=t@t", "GIT_COMMITTER_NAME=t", "GIT_COMMITTER_EMAIL=t@t")
		out, err := command.CombinedOutput()
		if err != nil {
			t.Fatalf("git %v: %v %s", args, err, out)
		}
		return strings.TrimSpace(string(out))
	}
	run("init", "-q", "-b", "main")
	for name, content := range files {
		path := filepath.Join(dir, name)
		if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(path, []byte(content), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	run("add", "-A")
	run("commit", "-q", "-m", "one")
	return filepath.Join(dir, ".git"), run("rev-parse", "HEAD")
}

func newTestRunnerActions(t *testing.T) *runnerActions {
	t.Helper()
	backend := &runnerBackend{cfg: &RunnerConfig{WorkRoot: t.TempDir()}, node: "fmnext-r1"}
	return newRunnerActions(backend)
}

func TestRunnerActionsPlanTakesOnlyPushWorkflowsARunnerMayRun(t *testing.T) {
	cache, head := runnerActionsRepo(t, map[string]string{
		".forkmesh/ci.yml":      "name: CI\non: [push]\njobs:\n  t:\n    steps:\n      - run: echo ci\n",
		".forkmesh/deploy.yml":  "name: Deploy\non: [workflow_dispatch]\njobs:\n  t:\n    steps:\n      - run: echo d\n",
		".forkmesh/desk.yml":    "name: Desk\non: [push]\nruns-on: forkmesh\njobs:\n  t:\n    steps:\n      - run: echo m\n",
		".forkmesh/release.yml": "name: Rel\non:\n  push:\n    branches: [release]\njobs:\n  t:\n    steps:\n      - run: echo r\n",
		".forkmesh/runner.yml":  "name: On runner\non: [push]\nruns-on: runner\njobs:\n  t:\n    steps:\n      - run: echo r\n",
	})
	a := newTestRunnerActions(t)
	runs, commit, problem := a.plan(context.Background(), cache, runnerActionsJob{Branch: "main"})
	if problem != "" || commit != head {
		t.Fatalf("plan: %q %q", problem, commit)
	}
	var names []string
	for _, run := range runs {
		names = append(names, run.Workflow)
		if run.Ref != "refs/heads/main" || run.Status != actionQueued || run.Commit != head {
			t.Fatalf("run %+v", run)
		}
	}
	if strings.Join(names, ",") != "CI,On runner" {
		t.Fatalf("workflows %v", names)
	}
	if _, _, problem := a.plan(context.Background(), cache, runnerActionsJob{Branch: "main",
		Commit: strings.Repeat("f", 40)}); !strings.Contains(problem, "not on main") {
		t.Fatalf("foreign commit: %q", problem)
	}
	if _, _, problem := a.plan(context.Background(), cache, runnerActionsJob{Branch: "gone"}); problem == "" {
		t.Fatal("a missing branch must not plan runs")
	}
}

func TestRunnerActionsRunsStepsInAFreshWorkspaceAndStopsAtAFailure(t *testing.T) {
	cache, head := runnerActionsRepo(t, map[string]string{
		"hello.txt": "hi\n",
		".forkmesh/ci.yml": "name: CI\non: [push]\nenv:\n  MODE: fast\njobs:\n  t:\n    steps:\n" +
			"      - name: one\n        run: echo ${{ vars.TOKEN }}\n      - name: two\n        run: exit 3\n" +
			"      - name: three\n        run: echo never\n",
	})
	a := newTestRunnerActions(t)
	var scripts []string
	var seen []string
	a.step = func(_ context.Context, work string, env []string, script string, sink io.Writer) (int, error) {
		content, err := os.ReadFile(filepath.Join(work, "hello.txt"))
		if err != nil || string(content) != "hi\n" {
			t.Errorf("workspace lacks the commit: %v", err)
		}
		scripts = append(scripts, script)
		seen = env
		io.WriteString(sink, "out "+script+"\n")
		if strings.Contains(script, "exit 3") {
			return 3, nil
		}
		return 0, nil
	}
	runs, commit, _ := a.plan(context.Background(), cache, runnerActionsJob{Branch: "main"})
	job := runnerActionsJob{JobID: 7, RepositoryOwner: "alice", RepositoryName: "proj", Branch: "main"}
	status, tail := a.runWorkflow(context.Background(), job, cache, &sync.Mutex{}, commit, 0, runs[0])
	if status != actionFailed || len(scripts) != 2 || strings.Contains(scripts[0], "vars.") {
		t.Fatalf("status %s scripts %q", status, scripts)
	}
	for _, want := range []string{"Actions variables", `step "two" exited with status 3`, "out exit 3"} {
		if !strings.Contains(tail, want) {
			t.Fatalf("log tail lacks %q:\n%s", want, tail)
		}
	}
	joined := strings.Join(seen, "\n")
	for _, want := range []string{"MODE=fast", "FORKMESH_COMMIT=" + head, "FORKMESH_REPO=alice/proj", "CI=true"} {
		if !strings.Contains(joined, want) {
			t.Fatalf("env lacks %q: %v", want, seen)
		}
	}
	if !strings.Contains(joined, "HOME="+filepath.Join(a.b.cfg.WorkRoot, runnerActionsDir)) {
		t.Fatalf("HOME must be the run's own scratch home: %v", seen)
	}
	entries, _ := os.ReadDir(filepath.Join(a.b.cfg.WorkRoot, runnerActionsDir))
	if len(entries) != 0 {
		t.Fatalf("the run workspace was left behind: %v", entries)
	}
}

func TestRunnerActionsOrderNeedsAndSkipAfterAFailedDependency(t *testing.T) {
	ci := &runnerActionsRun{name: "CI", path: ".forkmesh/ci.yml", Status: actionQueued}
	deploy := &runnerActionsRun{name: "Deploy", path: ".forkmesh/deploy.yml", Status: actionQueued,
		needs: []string{".forkmesh/ci.yml"}}
	runs := []*runnerActionsRun{deploy, ci}
	if index, verdict := nextRunnerActionsRun(runs); index != 1 || verdict != "go" {
		t.Fatalf("first: %d %s", index, verdict)
	}
	ci.Status = actionFailed
	if index, verdict := nextRunnerActionsRun(runs); index != 0 || verdict != "skip" {
		t.Fatalf("after a failure: %d %s", index, verdict)
	}
	deploy.Status = actionSkipped
	if index, _ := nextRunnerActionsRun(runs); index != -1 {
		t.Fatal("nothing is left to run")
	}
}

func TestRunnerActionsReportsAndRefusesALeaseThatIsNotItsOwn(t *testing.T) {
	a := newTestRunnerActions(t)
	var bodies []map[string]any
	a.post = func(_ context.Context, body []byte) (int, []byte, error) {
		var decoded map[string]any
		_ = json.Unmarshal(body, &decoded)
		bodies = append(bodies, decoded)
		return 200, []byte(`{"ok":true}`), nil
	}
	now := time.Now().UnixMilli()
	grant := mintGrant(t, runnerGrant{Node: "core1", Owner: "alice", Repository: "proj", Access: "read",
		Runner: "someone-else", GrantID: "grant-0000000001", IssuedAt: now, ExpiresAt: now + 3600_000})
	job := runnerActionsJob{Kind: "actions", JobID: 9, LeaseID: "lease", RepositoryOwner: "alice",
		RepositoryName: "proj", Branch: "main", ReadGrant: grant,
		CloneURL: "https://core1.test/v1/runner/alice/proj.git"}
	raw, _ := json.Marshal(job)
	if !isRunnerActionsJob(raw) {
		t.Fatal("an actions lease must be recognised")
	}
	a.accept(context.Background(), raw)
	a.wait()
	if len(bodies) != 1 || bodies[0]["status"] != "failed" || bodies[0]["jobId"] != float64(9) ||
		!strings.Contains(bodies[0]["message"].(string), "read grant") {
		t.Fatalf("reports %v", bodies)
	}
	if !a.free() {
		t.Fatal("the runner must be free again")
	}
	good := job
	good.ReadGrant = mintGrant(t, runnerGrant{Node: "core1", Owner: "alice", Repository: "proj",
		Access: "read", Runner: "fmnext-r1", GrantID: "grant-0000000002", IssuedAt: now, ExpiresAt: now + 3600_000})
	if problem := a.validate(good); problem != "" {
		t.Fatalf("a matching lease was refused: %s", problem)
	}
	good.CloneURL = "https://core1.test/v1/runner/bob/proj.git"
	if a.validate(good) == "" {
		t.Fatal("a clone URL for another repository must be refused")
	}
}

func TestTailBufferKeepsTheEnd(t *testing.T) {
	tail := &tailBuffer{limit: 8}
	io.WriteString(tail, "abcdef")
	io.WriteString(tail, "ghijkl")
	if tail.String() != "efghijkl" {
		t.Fatalf("tail %q", tail.String())
	}
}
