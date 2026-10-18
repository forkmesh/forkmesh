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
	runs, commit, catalog, problem := a.plan(context.Background(), cache, runnerActionsJob{Branch: "main"})
	if problem != "" || commit != head || len(catalog) != 5 {
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
	if _, _, _, problem := a.plan(context.Background(), cache, runnerActionsJob{Branch: "main",
		Commit: strings.Repeat("f", 40)}); !strings.Contains(problem, "not on main") {
		t.Fatalf("foreign commit: %q", problem)
	}
	if _, _, _, problem := a.plan(context.Background(), cache, runnerActionsJob{Branch: "gone"}); problem == "" {
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
	runs, commit, _, _ := a.plan(context.Background(), cache, runnerActionsJob{Branch: "main"})
	job := runnerActionsJob{JobID: 7, RepositoryOwner: "alice", RepositoryName: "proj", Branch: "main"}
	live := &runnerActionsLog{}
	status := a.runWorkflow(context.Background(), job, cache, &sync.Mutex{}, commit, 0, runs[0], live)
	tail := live.Tail()
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

type runnerActionsTestRepo struct {
	t   *testing.T
	dir string
}

func newRunnerActionsTestRepo(t *testing.T) *runnerActionsTestRepo {
	r := &runnerActionsTestRepo{t: t, dir: t.TempDir()}
	r.git("init", "-q", "-b", "main")
	return r
}

func (r *runnerActionsTestRepo) git(args ...string) string {
	r.t.Helper()
	command := exec.Command("git", args...)
	command.Dir = r.dir
	command.Env = append(os.Environ(), "GIT_CONFIG_GLOBAL=/dev/null", "GIT_CONFIG_NOSYSTEM=1",
		"GIT_AUTHOR_NAME=t", "GIT_AUTHOR_EMAIL=t@t", "GIT_COMMITTER_NAME=t", "GIT_COMMITTER_EMAIL=t@t")
	out, err := command.CombinedOutput()
	if err != nil {
		r.t.Fatalf("git %v: %v %s", args, err, out)
	}
	return strings.TrimSpace(string(out))
}

func (r *runnerActionsTestRepo) commit(files map[string]string) string {
	r.t.Helper()
	for name, content := range files {
		path := filepath.Join(r.dir, name)
		if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
			r.t.Fatal(err)
		}
		if err := os.WriteFile(path, []byte(content), 0o644); err != nil {
			r.t.Fatal(err)
		}
	}
	r.git("add", "-A")
	r.git("commit", "-q", "-m", "c")
	return r.git("rev-parse", "HEAD")
}

func TestRunnerActionsPlanForInstanceEvents(t *testing.T) {
	repo := newRunnerActionsTestRepo(t)
	first := repo.commit(map[string]string{
		"app/main.go":       "package main\n",
		".forkmesh/ci.yml":  "name: CI\non:\n  push:\n    paths: [\"app/**\"]\njobs:\n  t:\n    steps:\n      - run: echo ci\n",
		".forkmesh/all.yml": "name: All\non: push\nsteps:\n  - run: echo all\n",
		".forkmesh/nightly.yml": "name: Nightly\non:\n  schedule:\n    - cron: \"0 3 * * *\"\n  workflow_dispatch:\n" +
			"    inputs:\n      target:\n        default: all\nsteps:\n  - run: echo n\n",
		".forkmesh/triage.yml": "name: Triage\non:\n  issues:\n    types: [opened]\n  issue_comment:\nsteps:\n  - run: echo t\n",
	})
	repo.git("branch", "other")
	repo.git("checkout", "-q", "-b", "feature")
	second := repo.commit(map[string]string{"docs/readme.md": "hi\n"})
	repo.git("checkout", "-q", "main")
	cache := filepath.Join(repo.dir, ".git")
	a := newTestRunnerActions(t)
	names := func(runs []*runnerActionsRun) string {
		var out []string
		for _, run := range runs {
			out = append(out, run.Workflow)
		}
		return strings.Join(out, ",")
	}
	runs, commit, catalog, problem := a.plan(context.Background(), cache,
		runnerActionsJob{Branch: "feature", Commit: second, Before: first})
	if problem != "" || commit != second || names(runs) != "All" || runs[0].Event != "push" ||
		runs[0].Ref != "refs/heads/feature" {
		t.Fatalf("push with paths: %q %q %q", problem, commit, names(runs))
	}
	if len(catalog) != 4 || catalog[2].Path != ".forkmesh/nightly.yml" ||
		strings.Join(catalog[2].Events, ",") != "schedule,workflow_dispatch" ||
		strings.Join(catalog[2].Schedules, ",") != "0 3 * * *" || len(catalog[2].Inputs) != 1 ||
		catalog[2].Inputs[0].Default != "all" || catalog[2].Inputs[0].Type != "string" {
		t.Fatalf("catalog %+v", catalog)
	}
	encoded, _ := json.Marshal(catalog[0])
	if string(encoded) != `{"path":".forkmesh/all.yml","name":"All","events":["push"],"schedules":[],"inputs":[]}` {
		t.Fatalf("catalog json %s", encoded)
	}
	for _, before := range []string{"", strings.Repeat("0", 40)} {
		runs, _, _, _ = a.plan(context.Background(), cache, runnerActionsJob{Branch: "feature", Commit: second, Before: before})
		if names(runs) != "All,CI" {
			t.Fatalf("unknown before %q: %s", before, names(runs))
		}
	}
	if _, _, _, problem := a.plan(context.Background(), cache, runnerActionsJob{Branch: "main", Commit: second}); !strings.Contains(problem, "not on main") {
		t.Fatalf("default branch keeps the ancestor rule: %q", problem)
	}
	if _, commit, _, problem := a.plan(context.Background(), cache, runnerActionsJob{Branch: "other", Commit: second}); problem != "" || commit != second {
		t.Fatalf("another branch may run a pushed commit: %q", problem)
	}
	runs, commit, _, _ = a.plan(context.Background(), cache, runnerActionsJob{Branch: "main", Event: "schedule",
		Workflow: ".forkmesh/nightly.yml"})
	if names(runs) != "Nightly" || commit != first || runs[0].Event != "schedule" {
		t.Fatalf("schedule: %s %s", names(runs), commit)
	}
	if runs, _, _, _ := a.plan(context.Background(), cache, runnerActionsJob{Branch: "main", Event: "schedule",
		Workflow: ".forkmesh/ci.yml"}); len(runs) != 0 {
		t.Fatal("a workflow without that trigger must not run")
	}
	runs, commit, _, _ = a.plan(context.Background(), cache, runnerActionsJob{Branch: "feature", Commit: second,
		Event: "workflow_dispatch", Workflow: ".forkmesh/nightly.yml"})
	if names(runs) != "Nightly" || commit != second {
		t.Fatalf("dispatch: %s", names(runs))
	}
	for action, want := range map[string]string{"opened": "Triage", "closed": ""} {
		runs, _, _, _ := a.plan(context.Background(), cache, runnerActionsJob{Branch: "main", Event: "issues",
			Workflow: ".forkmesh/triage.yml", EventPayload: json.RawMessage(`{"action":"` + action + `"}`)})
		if names(runs) != want {
			t.Fatalf("issues %s: %q", action, names(runs))
		}
	}
	runs, _, _, _ = a.plan(context.Background(), cache, runnerActionsJob{Branch: "main", Event: "issue_comment",
		Workflow: ".forkmesh/triage.yml", EventPayload: json.RawMessage(`{"action":"deleted"}`)})
	if names(runs) != "Triage" {
		t.Fatal("issue_comment without types runs on every action")
	}
}

type capturedStep struct {
	dir    string
	env    map[string]string
	script string
}

func runInstanceWorkflow(t *testing.T, job runnerActionsJob, files map[string]string,
	step func(capturedStep, io.Writer) int) (string, string, []capturedStep, *runnerActions) {
	t.Helper()
	repo := newRunnerActionsTestRepo(t)
	repo.commit(files)
	repo.git("checkout", "-q", "-b", "feature")
	head := repo.commit(map[string]string{"bolt-jobs/worker/x.txt": "x\n"})
	cache := filepath.Join(repo.dir, ".git")
	a := newTestRunnerActions(t)
	var steps []capturedStep
	a.step = func(_ context.Context, dir string, env []string, script string, sink io.Writer) (int, error) {
		captured := capturedStep{dir: dir, env: map[string]string{}, script: script}
		for _, pair := range env {
			key, value, _ := strings.Cut(pair, "=")
			captured.env[key] = value
		}
		steps = append(steps, captured)
		return step(captured, sink), nil
	}
	job.Branch, job.Commit = "feature", head
	runs, commit, _, problem := a.plan(context.Background(), cache, job)
	if problem != "" || len(runs) != 1 {
		t.Fatalf("plan %q %d", problem, len(runs))
	}
	live := &runnerActionsLog{}
	status := a.runWorkflow(context.Background(), job, cache, &sync.Mutex{}, commit, 0, runs[0], live)
	return status, live.String(), steps, a
}

func gitConfigIn(t *testing.T, dir, key string) string {
	command := exec.Command("git", "-C", dir, "config", "--get", key)
	command.Env = append(os.Environ(), "GIT_CONFIG_GLOBAL=/dev/null", "GIT_CONFIG_NOSYSTEM=1")
	out, _ := command.Output()
	return strings.TrimSpace(string(out))
}

func TestRunnerActionsRunsInstanceWorkflowsInARealClone(t *testing.T) {
	files := map[string]string{
		".forkmesh/deploy.yml": `name: Deploy
on:
  workflow_dispatch:
    inputs:
      project:
        default: all
permissions:
  contents: write
env:
  STAGE: ${{ inputs.project }}-stage
jobs:
  deploy:
    defaults:
      run:
        working-directory: bolt-jobs
    env:
      LEVEL: job
    steps:
      - name: Build
        run: echo "${{ inputs.project }}" && echo $CLOUDFLARE_API_TOKEN
      - name: Only on main
        if: github.ref == 'refs/heads/main'
        run: echo never
      - name: Deploy
        working-directory: bolt-jobs/worker
        env:
          LEVEL: step
          SECRET_COPY: ${{ secrets.CLOUDFLARE_API_TOKEN }}
        run: deploy ${{ secrets.CLOUDFLARE_API_TOKEN }}
      - name: Report
        if: failure()
        run: echo report
`,
		"bolt-jobs/package.json": "{}\n",
	}
	now := time.Now().UnixMilli()
	grant := mintGrant(t, runnerGrant{Node: "core1", Owner: "alice", Repository: "proj", Access: "actions-push",
		Ref: "refs/heads/feature", Runner: "fmnext-r1", GrantID: "grant-0000000009", IssuedAt: now, ExpiresAt: now + 3600_000})
	job := runnerActionsJob{JobID: 3, RepositoryOwner: "alice", RepositoryName: "proj",
		CloneURL: "https://core1.test/v1/runner/alice/proj.git", Event: "workflow_dispatch",
		Workflow: ".forkmesh/deploy.yml", Before: strings.Repeat("a", 40),
		EventPayload: json.RawMessage(`{"inputs":{"project":"leaf"},"ref":"refs/heads/feature","sender":{"login":"dana"}}`),
		Inputs:       map[string]string{"project": "leaf"}, Secrets: map[string]string{"CLOUDFLARE_API_TOKEN": "cf-secret-123"},
		PushGrant: grant, PushRef: "refs/heads/feature", JobToken: "job-token-abcdef", APIURL: "https://git.example.org",
		Trusted: true, BotName: "alice-actions", BotEmail: "alice-actions@git.example.org"}
	var checked bool
	status, logText, steps, _ := runInstanceWorkflow(t, job, files, func(step capturedStep, sink io.Writer) int {
		if !checked {
			checked = true
			src := step.env["GITHUB_WORKSPACE"]
			if step.dir != filepath.Join(src, "bolt-jobs") {
				t.Errorf("dir %s", step.dir)
			}
			if gitConfigIn(t, src, "remote.origin.url") != job.CloneURL ||
				gitConfigIn(t, src, "http.extraheader") != runnerGrantHeader+": "+grant ||
				gitConfigIn(t, src, "user.name") != "alice-actions" ||
				gitConfigIn(t, src, "branch.feature.merge") != "refs/heads/feature" {
				t.Errorf("clone config is wrong")
			}
			command := exec.Command("git", "-C", src, "symbolic-ref", "HEAD")
			out, _ := command.Output()
			if strings.TrimSpace(string(out)) != "refs/heads/feature" {
				t.Errorf("HEAD %s", out)
			}
			command = exec.Command("git", "-C", src, "log", "--format=%H", "-n", "5")
			out, _ = command.Output()
			if len(strings.Fields(string(out))) != 2 {
				t.Errorf("the clone lacks history: %s", out)
			}
			payload, err := os.ReadFile(step.env["GITHUB_EVENT_PATH"])
			if err != nil || !strings.Contains(string(payload), `"login":"dana"`) {
				t.Errorf("event payload %s %v", payload, err)
			}
		}
		io.WriteString(sink, "token is cf-secret-123 and job-token-abcdef\n")
		if strings.HasPrefix(step.script, "deploy") {
			return 1
		}
		return 0
	})
	if status != actionFailed || len(steps) != 3 {
		t.Fatalf("status %s steps %d\n%s", status, len(steps), logText)
	}
	env := steps[0].env
	for key, want := range map[string]string{
		"FORKMESH_EVENT": "workflow_dispatch", "GITHUB_EVENT_NAME": "workflow_dispatch",
		"FORKMESH_TOKEN": "job-token-abcdef", "GITHUB_TOKEN": "job-token-abcdef",
		"FORKMESH_API_URL": "https://git.example.org", "GITHUB_REPOSITORY": "alice/proj",
		"GITHUB_SHA": steps[0].env["FORKMESH_COMMIT"], "GITHUB_REF": "refs/heads/feature", "GITHUB_REF_NAME": "feature",
		"GITHUB_ACTOR": "dana", "INPUT_PROJECT": "leaf", "CLOUDFLARE_API_TOKEN": "cf-secret-123",
		"FORKMESH_BEFORE": strings.Repeat("a", 40), "STAGE": "leaf-stage", "LEVEL": "job", "FM_EXPR_1": "leaf",
	} {
		if env[key] != want {
			t.Fatalf("env %s = %q, want %q", key, env[key], want)
		}
	}
	if steps[0].script != `echo "${FM_EXPR_1}" && echo $CLOUDFLARE_API_TOKEN` {
		t.Fatalf("script %q", steps[0].script)
	}
	deploy := steps[1]
	if deploy.script != `deploy "$CLOUDFLARE_API_TOKEN"` || deploy.env["LEVEL"] != "step" ||
		deploy.env["SECRET_COPY"] != "cf-secret-123" || !strings.HasSuffix(deploy.dir, filepath.Join("bolt-jobs", "worker")) {
		t.Fatalf("deploy %+v", deploy)
	}
	if !strings.HasPrefix(steps[2].script, "echo report") {
		t.Fatal("a failure() step must run after a failure")
	}
	for _, want := range []string{"Skipped: Only on main", "token is *** and ***", `step "Deploy" exited with status 1`} {
		if !strings.Contains(logText, want) {
			t.Fatalf("log lacks %q:\n%s", want, logText)
		}
	}
	if strings.Contains(logText, "cf-secret-123") || strings.Contains(logText, "job-token-abcdef") {
		t.Fatalf("secret leaked:\n%s", logText)
	}

	untrusted := job
	untrusted.Trusted = false
	_, logText, steps, _ = runInstanceWorkflow(t, untrusted, files, func(step capturedStep, sink io.Writer) int {
		src := step.env["GITHUB_WORKSPACE"]
		if gitConfigIn(t, src, "http.extraheader") != "" || gitConfigIn(t, src, "remote.origin.url") != job.CloneURL {
			t.Errorf("an untrusted clone must carry no credential")
		}
		return 0
	})
	if steps[0].env["FORKMESH_TOKEN"] != "" || steps[0].env["CLOUDFLARE_API_TOKEN"] != "" ||
		steps[1].script != `deploy "$FM_EXPR_1"` || steps[1].env["FM_EXPR_1"] != "" {
		t.Fatalf("untrusted job saw secrets: %+v", steps)
	}

	readOnly := map[string]string{".forkmesh/deploy.yml": strings.Replace(files[".forkmesh/deploy.yml"],
		"permissions:\n  contents: write\n", "", 1), "bolt-jobs/package.json": "{}\n"}
	_, _, _, _ = runInstanceWorkflow(t, job, readOnly, func(step capturedStep, sink io.Writer) int {
		if gitConfigIn(t, step.env["GITHUB_WORKSPACE"], "http.extraheader") != "" {
			t.Errorf("a workflow without contents: write must not get the push grant")
		}
		return 0
	})
	wrongGrant := job
	wrongGrant.PushGrant = mintGrant(t, runnerGrant{Node: "core1", Owner: "alice", Repository: "proj", Access: "actions-push",
		Ref: "refs/heads/main", Runner: "fmnext-r1", GrantID: "grant-0000000010", IssuedAt: now, ExpiresAt: now + 3600_000})
	_, _, _, _ = runInstanceWorkflow(t, wrongGrant, files, func(step capturedStep, sink io.Writer) int {
		if gitConfigIn(t, step.env["GITHUB_WORKSPACE"], "http.extraheader") != "" {
			t.Errorf("a grant for another branch must not be configured")
		}
		return 0
	})
}

func TestRunnerActionsJobIfAndWorkflowTimeout(t *testing.T) {
	files := map[string]string{".forkmesh/t.yml": `on: [push]
jobs:
  skipped:
    if: github.event_name == 'schedule'
    steps:
      - name: Never
        run: echo never
  slow:
    timeout-minutes: 1
    steps:
      - name: Missing dir
        working-directory: nowhere
        run: echo x
      - name: Always
        if: always()
        run: echo always
`}
	status, logText, steps, _ := runInstanceWorkflow(t, runnerActionsJob{JobID: 1, RepositoryOwner: "alice",
		RepositoryName: "proj", CloneURL: "https://core1.test/v1/runner/alice/proj.git"}, files,
		func(capturedStep, io.Writer) int { return 0 })
	if status != actionFailed || len(steps) != 1 || steps[0].script != "echo always" {
		t.Fatalf("status %s steps %+v\n%s", status, steps, logText)
	}
	for _, want := range []string{"Skipped: Never", "working directory nowhere is not in the checkout"} {
		if !strings.Contains(logText, want) {
			t.Fatalf("log lacks %q:\n%s", want, logText)
		}
	}
}

func TestRunnerActionsReportsProgressWhileRunning(t *testing.T) {
	a := newTestRunnerActions(t)
	a.tick = 5 * time.Millisecond
	var mu sync.Mutex
	var bodies []map[string]any
	a.post = func(_ context.Context, body []byte) (int, []byte, error) {
		var decoded map[string]any
		_ = json.Unmarshal(body, &decoded)
		mu.Lock()
		bodies = append(bodies, decoded)
		mu.Unlock()
		return 200, nil, nil
	}
	run := &runnerActionsRun{Workflow: "CI", Status: actionRunning, Event: "push"}
	live := &runnerActionsLog{}
	io.WriteString(live, "==> Step 1/1: build\n")
	stop, done := make(chan struct{}), make(chan struct{})
	job := runnerActionsJob{JobID: 4, LeaseID: "l", catalog: []runnerActionsCatalogEntry{{Path: ".forkmesh/ci.yml", Name: "CI"}}}
	go a.tickReports(context.Background(), job, []*runnerActionsRun{run}, "c", run, live, stop, done)
	deadline := time.Now().Add(5 * time.Second)
	for {
		mu.Lock()
		n := len(bodies)
		mu.Unlock()
		if n >= 2 || time.Now().After(deadline) {
			break
		}
		time.Sleep(5 * time.Millisecond)
	}
	close(stop)
	<-done
	if len(bodies) < 2 {
		t.Fatal("no progress reports")
	}
	runs := bodies[0]["runs"].([]any)
	first := runs[0].(map[string]any)
	if bodies[0]["status"] != "running" || first["event"] != "push" ||
		!strings.Contains(first["log"].(string), "Step 1/1") || !strings.Contains(first["logTail"].(string), "Step 1/1") ||
		len(bodies[0]["catalog"].([]any)) != 1 {
		t.Fatalf("progress %v", bodies[0])
	}
}

func TestRunnerActionsLogCapsAndMasks(t *testing.T) {
	live := &runnerActionsLog{}
	chunk := strings.Repeat("x", 1023) + "\n"
	for i := 0; i < 2048; i++ {
		io.WriteString(live, chunk)
	}
	io.WriteString(live, "THE END\n")
	full := live.String()
	if len(full) > runnerActionsLogMax+128 || !strings.HasSuffix(full, "THE END\n") ||
		!strings.Contains(full, "bytes of output omitted") {
		t.Fatalf("log len %d", len(full))
	}
	if tail := live.Tail(); len(tail) != runnerActionsLogTail || !strings.HasSuffix(tail, "THE END\n") {
		t.Fatalf("tail len %d", len(tail))
	}
	out := &strings.Builder{}
	mask := newMaskingWriter(out, []string{"abc", "secret-one", "line1\nline-two"})
	io.WriteString(mask, "a secret-")
	io.WriteString(mask, "one here abc\nline-two too\npartial secret-one")
	if out.String() != "a *** here abc\n*** too\n" {
		t.Fatalf("masked %q", out.String())
	}
	mask.Flush()
	if !strings.HasSuffix(out.String(), "partial ***") {
		t.Fatalf("flush %q", out.String())
	}
}
