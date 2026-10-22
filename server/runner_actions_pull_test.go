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
)

func runnerActionsGit(t *testing.T, gitDir string, args ...string) string {
	t.Helper()
	command := exec.Command("git", args...)
	command.Dir = filepath.Dir(gitDir)
	command.Env = append(os.Environ(), "GIT_CONFIG_GLOBAL=/dev/null", "GIT_CONFIG_NOSYSTEM=1",
		"GIT_AUTHOR_NAME=t", "GIT_AUTHOR_EMAIL=t@t", "GIT_COMMITTER_NAME=t", "GIT_COMMITTER_EMAIL=t@t")
	out, err := command.CombinedOutput()
	if err != nil {
		t.Fatalf("git %v: %v %s", args, err, out)
	}
	return strings.TrimSpace(string(out))
}

func runnerActionsPullHead(t *testing.T, gitDir string, files map[string]string) string {
	t.Helper()
	runnerActionsGit(t, gitDir, "checkout", "-q", "-b", "contributor")
	for name, content := range files {
		path := filepath.Join(filepath.Dir(gitDir), name)
		if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(path, []byte(content), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	runnerActionsGit(t, gitDir, "add", "-A")
	runnerActionsGit(t, gitDir, "commit", "-q", "-m", "pull")
	head := runnerActionsGit(t, gitDir, "rev-parse", "HEAD")
	runnerActionsGit(t, gitDir, "update-ref", "refs/pr/7/head", head)
	runnerActionsGit(t, gitDir, "checkout", "-q", "main")
	runnerActionsGit(t, gitDir, "branch", "-q", "-D", "contributor")
	return head
}

const runnerActionsPullCI = "name: CI\non:\n  pull_request:\n    branches: [main]\njobs:\n  t:\n    steps:\n      - name: test\n        run: echo base-workflow\n"

func pullJob(head, base string) runnerActionsJob {
	return runnerActionsJob{Kind: "actions", JobID: 9, LeaseID: "lease", RepositoryOwner: "alice",
		RepositoryName: "proj", Branch: "pull/7", Commit: head, Event: "pull_request",
		Workflow: ".forkmesh/ci.yml", PullNumber: 7, PullRef: "refs/pr/7/head", BaseRef: "main",
		BaseCommit: base, Before: base}
}

func TestRunnerActionsPullRequestRunsTheBaseWorkflowOnTheHead(t *testing.T) {
	cache, base := runnerActionsRepo(t, map[string]string{".forkmesh/ci.yml": runnerActionsPullCI, "app.txt": "old\n"})
	head := runnerActionsPullHead(t, cache, map[string]string{
		".forkmesh/ci.yml": strings.Replace(runnerActionsPullCI, "echo base-workflow", "echo pwned", 1),
		"app.txt":          "new\n",
	})
	a := newTestRunnerActions(t)
	job := pullJob(head, base)
	runs, commit, catalog, problem := a.planJob(context.Background(), cache, &job)
	if problem != "" || commit != head || len(runs) != 1 || len(catalog) != 1 {
		t.Fatalf("plan: %q %q %d", problem, commit, len(runs))
	}
	if runs[0].Ref != "refs/pr/7/head" || runs[0].Commit != head || runs[0].Event != "pull_request" {
		t.Fatalf("run %+v", runs[0])
	}
	if len(runs[0].wf.Steps) != 1 || runs[0].wf.Steps[0].Run != "echo base-workflow" {
		t.Fatalf("the pull request's own workflow was used: %+v", runs[0].wf.Steps)
	}
	var env []string
	var workspace string
	a.step = func(_ context.Context, work string, stepEnv []string, script string, sink io.Writer) (int, error) {
		content, _ := os.ReadFile(filepath.Join(work, "app.txt"))
		workspace, env = string(content), stepEnv
		return 0, nil
	}
	job.Secrets = map[string]string{"DEPLOY_TOKEN": "s3cret-value"}
	job.JobToken, job.PushGrant, job.Trusted = "token-value", "grant", true
	job.dropPrivileges()
	live := &runnerActionsLog{}
	if status := a.runWorkflow(context.Background(), job, cache, &sync.Mutex{}, commit, 0, runs[0], live); status != actionSuccess {
		t.Fatalf("status %s\n%s", status, live.String())
	}
	if workspace != "new\n" {
		t.Fatalf("the workspace is not the pull request head: %q", workspace)
	}
	joined := strings.Join(env, "\n")
	for _, want := range []string{"FORKMESH_EVENT=pull_request", "FORKMESH_PULL_NUMBER=7", "FORKMESH_BASE_REF=main",
		"FORKMESH_BASE_SHA=" + base, "FORKMESH_BEFORE=" + base, "GITHUB_SHA=" + head, "GITHUB_REF=refs/pr/7/head"} {
		if !strings.Contains(joined, want) {
			t.Fatalf("env lacks %q:\n%s", want, joined)
		}
	}
	for _, banned := range []string{"s3cret-value", "token-value", "FORKMESH_TOKEN", "GITHUB_TOKEN", "DEPLOY_TOKEN"} {
		if strings.Contains(joined, banned) {
			t.Fatalf("a pull request run got %q", banned)
		}
	}
	if log := live.String(); !strings.Contains(log, "not from the pull request") || !strings.Contains(log, "no secrets") {
		t.Fatalf("log header:\n%s", log)
	}
}

func TestRunnerActionsPullRequestGateStopsWorkflowChanges(t *testing.T) {
	cache, base := runnerActionsRepo(t, map[string]string{".forkmesh/ci.yml": runnerActionsPullCI, "app.txt": "old\n"})
	head := runnerActionsPullHead(t, cache, map[string]string{".forkmesh/extra.yml": "name: X\non: [pull_request]\n"})
	a := newTestRunnerActions(t)
	job := pullJob(head, base)
	job.GateWorkflowChanges = true
	runs, _, _, problem := a.planJob(context.Background(), cache, &job)
	if len(runs) != 0 || !strings.Contains(problem, "approve") || job.approvalRequired != "workflows" {
		t.Fatalf("gate: %d %q %q", len(runs), problem, job.approvalRequired)
	}
	body, _, err := a.reportBody(job, "completed", nil, head, problem)
	var envelope map[string]any
	if err != nil || json.Unmarshal(body, &envelope) != nil || envelope["approvalRequired"] != "workflows" {
		t.Fatalf("report: %s", body)
	}
	approved := pullJob(head, base)
	if runs, _, _, problem := a.planJob(context.Background(), cache, &approved); problem != "" || len(runs) != 1 {
		t.Fatalf("an approved run must plan: %q", problem)
	}
}

func TestRunnerActionsPullRequestGateLetsOrdinaryChangesThrough(t *testing.T) {
	cache, base := runnerActionsRepo(t, map[string]string{".forkmesh/ci.yml": runnerActionsPullCI, "app.txt": "old\n"})
	head := runnerActionsPullHead(t, cache, map[string]string{"app.txt": "new\n"})
	a := newTestRunnerActions(t)
	job := pullJob(head, base)
	job.GateWorkflowChanges = true
	if runs, _, _, problem := a.planJob(context.Background(), cache, &job); problem != "" || len(runs) != 1 || job.approvalRequired != "" {
		t.Fatalf("plan: %q %d", problem, len(runs))
	}
	other := pullJob(head, base)
	other.BaseRef = "release"
	other.BaseCommit = ""
	if _, _, _, problem := a.planJob(context.Background(), cache, &other); !strings.Contains(problem, "release") {
		t.Fatalf("missing base: %q", problem)
	}
	runnerActionsGit(t, cache, "branch", "release")
	if runs, _, _, problem := a.planJob(context.Background(), cache, &other); problem != "" || len(runs) != 0 {
		t.Fatalf("on.pull_request.branches must filter the base: %q %d", problem, len(runs))
	}
}

func TestRunnerActionsPullRequestJobsAreValidatedAndUnprivileged(t *testing.T) {
	base, head := strings.Repeat("b", 40), strings.Repeat("c", 40)
	if problem := validRunnerActionsPull(pullJob(head, base)); problem != "" {
		t.Fatalf("valid job refused: %q", problem)
	}
	for name, edit := range map[string]func(*runnerActionsJob){
		"other pull ref": func(j *runnerActionsJob) { j.PullRef = "refs/pr/8/head" },
		"tag ref":        func(j *runnerActionsJob) { j.PullRef = "refs/tags/v1" },
		"dotted branch":  func(j *runnerActionsJob) { j.PullRef = "refs/heads/a..b" },
		"no number":      func(j *runnerActionsJob) { j.PullNumber = 0 },
		"no head":        func(j *runnerActionsJob) { j.Commit = "" },
		"bad base":       func(j *runnerActionsJob) { j.BaseRef = "../main" },
		"bad base oid":   func(j *runnerActionsJob) { j.BaseCommit = "main" },
		"push with pull": func(j *runnerActionsJob) { j.Event = "push" },
	} {
		job := pullJob(head, base)
		edit(&job)
		if validRunnerActionsPull(job) == "" {
			t.Fatalf("%s accepted", name)
		}
	}
	branch := pullJob(head, base)
	branch.PullRef = "refs/heads/pr/bob/fix"
	if problem := validRunnerActionsPull(branch); problem != "" {
		t.Fatalf("branch-backed pull refused: %q", problem)
	}
	job := pullJob(head, base)
	job.Trusted, job.Secrets, job.PushGrant, job.PushRef = true, map[string]string{"A": "b"}, "g", "refs/heads/main"
	job.JobToken, job.APIURL, job.BotName, job.BotEmail = "t", "https://x", "bot", "bot@x"
	job.dropPrivileges()
	if job.Trusted || job.Secrets != nil || job.PushGrant != "" || job.JobToken != "" || job.BotName != "" || job.APIURL != "" {
		t.Fatalf("privileges kept: %+v", job)
	}
	push := runnerActionsJob{Event: "push", Trusted: true, JobToken: "t"}
	push.dropPrivileges()
	if !push.Trusted || push.JobToken != "t" {
		t.Fatal("a push job lost its privileges")
	}
}
