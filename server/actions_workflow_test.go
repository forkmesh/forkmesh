package mirrornode

import (
	"errors"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"
)

func TestParseWorkflowReadsTheDesktopSubset(t *testing.T) {
	content := `# leading comment
name: "CI tests"   # trailing comment
on:
  push:
    paths:
      - app/**
      - "tools/*.py"
  workflow_dispatch:
runs-on: [linux, mirror]
env:
  MODE: test
jobs:
  test:
    steps:
      - name: Unit tests
        run: |
          set -eu
          # a shell comment survives
          python3 - <<'PY'
          	print("tab kept")
          PY
      - uses: actions/checkout@v4
      - run: echo "one: two"
  package:
    steps:
    - name: Folded
      run: >-
        echo a
        b
`
	parsed, err := parseWorkflow(".forkmesh/ci.yml", content)
	if err != nil {
		t.Fatal(err)
	}
	if parsed.Name != "CI tests" {
		t.Fatalf("name = %q", parsed.Name)
	}
	if !reflect.DeepEqual(parsed.Events, []string{"push", "workflow_dispatch"}) || !parsed.manual() {
		t.Fatalf("events = %v", parsed.Events)
	}
	if !reflect.DeepEqual(parsed.PushPaths, []string{"app/**", "tools/*.py"}) {
		t.Fatalf("paths = %v", parsed.PushPaths)
	}
	if !reflect.DeepEqual(parsed.RunsOn, []string{"linux", "mirror"}) {
		t.Fatalf("runs-on = %v", parsed.RunsOn)
	}
	if !reflect.DeepEqual(parsed.Env, [][2]string{{"MODE", "test"}}) {
		t.Fatalf("env = %v", parsed.Env)
	}
	// `uses:` is dropped; jobs are flattened in file order.
	if len(parsed.Steps) != 3 {
		t.Fatalf("steps = %#v", parsed.Steps)
	}
	first := parsed.Steps[0]
	if first.Name != "Unit tests" ||
		!strings.Contains(first.Run, "# a shell comment survives\n") ||
		!strings.Contains(first.Run, "\tprint(\"tab kept\")\n") ||
		!strings.HasPrefix(first.Run, "set -eu\n") {
		t.Fatalf("block scalar = %q", first.Run)
	}
	if parsed.Steps[1].Run != `echo "one: two"` || parsed.Steps[1].Name != `echo "one: two"` {
		t.Fatalf("inline run = %#v", parsed.Steps[1])
	}
	if parsed.Steps[2].Run != "echo a b" {
		t.Fatalf("folded = %q", parsed.Steps[2].Run)
	}
}

func TestParseWorkflowEventForms(t *testing.T) {
	for content, want := range map[string][]string{
		"on: push\nsteps:\n  - run: true\n":                      {"push"},
		"on: [push, pull_request]\nsteps:\n  - run: true\n":      {"push", "pull_request"},
		"on:\n  - push\n  - release\nsteps:\n  - run: true\n":    {"push", "release"},
		"\"on\":\n  workflow_dispatch:\nsteps:\n  - run: true\n": {"workflow_dispatch"},
		"steps:\n  - run: true\n":                                nil,
	} {
		parsed, err := parseWorkflow(".forkmesh/a.yml", content)
		if err != nil {
			t.Fatalf("%q: %v", content, err)
		}
		if !reflect.DeepEqual(parsed.Events, want) {
			t.Fatalf("%q: events = %v, want %v", content, parsed.Events, want)
		}
		if parsed.Name != "a" {
			t.Fatalf("default name = %q", parsed.Name)
		}
	}
}

func TestParseWorkflowRefusesWhatItCannotRunFaithfully(t *testing.T) {
	for name, content := range map[string]string{
		"empty":         "",
		"no steps":      "name: x\non: push\n",
		"only uses":     "steps:\n  - uses: a/b@v1\n",
		"not a mapping": "- a\n- b\n",
		"duplicate key": "name: a\nname: b\nsteps:\n  - run: true\n",
		"tab indent":    "steps:\n\t- run: true\n",
		"anchor":        "name: &a x\nsteps:\n  - run: true\n",
		"flow mapping":  "on: {push: {}}\nsteps:\n  - run: true\n",
		"nul":           "name: a\x00\nsteps:\n  - run: true\n",
		"too deep":      strings.Repeat("a:\n", 1) + deepYAML(40),
	} {
		if _, err := parseWorkflow(".forkmesh/a.yml", content); err == nil {
			t.Fatalf("%s: accepted", name)
		}
	}
	for _, path := range []string{"ci.yml", ".forkmesh/../x.yml", ".forkmesh/a/b.yml", ".forkmesh/a.json"} {
		if _, err := parseWorkflow(path, "steps:\n  - run: true\n"); err == nil {
			t.Fatalf("path %q accepted", path)
		}
	}
}

func deepYAML(depth int) string {
	var out strings.Builder
	for i := 1; i <= depth; i++ {
		out.WriteString(strings.Repeat(" ", i) + "k:\n")
	}
	out.WriteString(strings.Repeat(" ", depth+1) + "v: 1\n")
	return out.String()
}

func TestWorkflowPathFilterAndRunsOn(t *testing.T) {
	filtered := &workflow{PushPaths: []string{"app/**", "tools/*.py", "README.md"}}
	for file, want := range map[string]bool{
		"app/src/entry.py": true, "app/x": true, "tools/a.py": true,
		"tools/sub/a.py": false, "README.md": true, "docs/README.md": false,
		"server/main.go": false,
	} {
		if got := filtered.matchesPushPaths([]string{file}); got != want {
			t.Fatalf("%s: %v, want %v", file, got, want)
		}
	}
	if !(&workflow{}).matchesPushPaths(nil) {
		t.Fatal("a workflow without a path filter runs on every push")
	}
	labels := []string{"mirror38", "linux", "mirror"}
	if !(&workflow{}).runsOnThisNode(labels) ||
		!(&workflow{RunsOn: []string{"Linux"}}).runsOnThisNode(labels) ||
		(&workflow{RunsOn: []string{"macos", "deploy-box"}}).runsOnThisNode(labels) {
		t.Fatal("runs-on matching is wrong")
	}
}

func TestSubstituteWorkflowVarsBlanksWhatAMirrorDoesNotHold(t *testing.T) {
	out, used := substituteWorkflowVars(`curl -H "x: ${{ vars.TOKEN }}" ${{vars.URL}} $HOME`)
	if !used || out != `curl -H "x: " $HOME` && out != `curl -H "x: "  $HOME` {
		t.Fatalf("%v %q", used, out)
	}
	if out, used := substituteWorkflowVars("echo ${HOME}"); used || out != "echo ${HOME}" {
		t.Fatal("shell variables must be left alone")
	}
}

// The repository's own workflows are the real corpus this has to read.
func TestParseWorkflowReadsThisRepositorysWorkflows(t *testing.T) {
	files, _ := filepath.Glob("../.forkmesh/*.yml")
	if len(files) == 0 {
		t.Skip("no workflow corpus beside the module")
	}
	for _, file := range files {
		raw, err := os.ReadFile(file)
		if err != nil {
			t.Fatal(err)
		}
		parsed, err := parseWorkflow(".forkmesh/"+filepath.Base(file), string(raw))
		if err != nil {
			t.Fatalf("%s: %v", file, err)
		}
		if len(parsed.Events) == 0 || len(parsed.Steps) == 0 {
			t.Fatalf("%s: events=%v steps=%d", file, parsed.Events, len(parsed.Steps))
		}
		for _, step := range parsed.Steps {
			if strings.TrimSpace(step.Run) == "" || step.Name == "" {
				t.Fatalf("%s: empty step %#v", file, step)
			}
		}
		t.Logf("%-40s %-28q on=%v steps=%d", filepath.Base(file), parsed.Name, parsed.Events, len(parsed.Steps))
	}
}

func TestParseWorkflowRefusesAFlushLeftLineInsideARunBlock(t *testing.T) {
	content := `name: Boot
on: push
jobs:
  boot:
    steps:
      - name: Summarize
        run: |
          python3 - <<'PY'
import json, sys
PY
`
	_, err := parseWorkflow(".forkmesh/boot.yml", content)
	if !errors.Is(err, errWorkflowSyntax) || !strings.Contains(err.Error(), "(line ") {
		t.Fatalf("want a located syntax error, got %v", err)
	}
}

func TestParseWorkflowReadsInstanceTriggersAndStepOptions(t *testing.T) {
	content := `name: Refresh data
on:
  push:
    branches: [main]
    paths: ["bolt-jobs/**"]
  schedule:
    - cron: "17 3 * * *"
    - cron: "not a cron"
    - cron: "@daily"
  workflow_dispatch:
    inputs:
      project:
        description: Project to deploy
        required: true
        default: all
        type: choice
        options: [all, bolt-c]
      verbose:
        type: boolean
  issues:
    types: [opened, labeled, edited]
  issue_comment:
    types: [created]
  pull_request:
    branches: [main]
permissions:
  contents: write
env:
  TZ: UTC
jobs:
  refresh:
    runs-on: runner
    timeout-minutes: 30
    if: github.event_name != 'issues'
    defaults:
      run:
        working-directory: bolt-jobs
    env:
      NODE_ENV: production
      TZ: Europe/Paris
    steps:
      - name: Build
        run: npm ci
      - name: Deploy
        if: github.ref == 'refs/heads/main'
        working-directory: ./bolt-jobs/worker
        timeout-minutes: 5
        env:
          NODE_ENV: staging
          CLOUDFLARE_API_TOKEN: ${{ secrets.CLOUDFLARE_API_TOKEN }}
        run: npx wrangler deploy
  other:
    steps:
      - run: echo other
`
	wf, err := parseWorkflow(".forkmesh/refresh.yml", content)
	if err != nil {
		t.Fatal(err)
	}
	if !reflect.DeepEqual(wf.Events, []string{"push", "schedule", "workflow_dispatch", "issues", "issue_comment", "pull_request"}) {
		t.Fatalf("events %v", wf.Events)
	}
	if !reflect.DeepEqual(wf.Schedules, []string{"17 3 * * *", "@daily"}) || !wf.ContentsWrite ||
		wf.TimeoutMinutes != 30 || !reflect.DeepEqual(wf.IssueTypes, []string{"opened", "labeled", "edited"}) ||
		!reflect.DeepEqual(wf.CommentTypes, []string{"created"}) || !reflect.DeepEqual(wf.PullBranches, []string{"main"}) {
		t.Fatalf("workflow %+v", wf)
	}
	wantInputs := []workflowInput{
		{Name: "project", Description: "Project to deploy", Required: true, Default: "all", Type: "choice",
			Options: []string{"all", "bolt-c"}},
		{Name: "verbose", Type: "boolean"},
	}
	if !reflect.DeepEqual(wf.Inputs, wantInputs) {
		t.Fatalf("inputs %+v", wf.Inputs)
	}
	if len(wf.Steps) != 3 {
		t.Fatalf("steps %+v", wf.Steps)
	}
	build, deploy, other := wf.Steps[0], wf.Steps[1], wf.Steps[2]
	if build.Job != "refresh" || build.JobIf != "github.event_name != 'issues'" || build.WorkingDirectory != "bolt-jobs" ||
		!reflect.DeepEqual(build.Env, [][2]string{{"NODE_ENV", "production"}, {"TZ", "Europe/Paris"}}) {
		t.Fatalf("build %+v", build)
	}
	if deploy.If != "github.ref == 'refs/heads/main'" || deploy.WorkingDirectory != "bolt-jobs/worker" ||
		deploy.TimeoutMinutes != 5 || !reflect.DeepEqual(deploy.Env, [][2]string{{"NODE_ENV", "staging"},
		{"TZ", "Europe/Paris"}, {"CLOUDFLARE_API_TOKEN", "${{ secrets.CLOUDFLARE_API_TOKEN }}"}}) {
		t.Fatalf("deploy %+v", deploy)
	}
	if other.Job != "other" || other.JobIf != "" || other.WorkingDirectory != "" || other.Env != nil {
		t.Fatalf("other %+v", other)
	}
	if write, err := parseWorkflow(".forkmesh/a.yml", "permissions: write-all\nsteps:\n  - run: true\n"); err != nil || !write.ContentsWrite {
		t.Fatalf("write-all: %v", err)
	}
	if read, err := parseWorkflow(".forkmesh/a.yml", "permissions:\n  contents: read\nsteps:\n  - run: true\n"); err != nil || read.ContentsWrite {
		t.Fatalf("contents read: %v", err)
	}
	for name, bad := range map[string]string{
		"absolute dir":  "jobs:\n  a:\n    steps:\n      - run: true\n        working-directory: /etc\n",
		"parent dir":    "jobs:\n  a:\n    defaults:\n      run:\n        working-directory: a/../..\n    steps:\n      - run: true\n",
		"bad timeout":   "jobs:\n  a:\n    timeout-minutes: soon\n    steps:\n      - run: true\n",
		"step timeout":  "jobs:\n  a:\n    steps:\n      - run: true\n        timeout-minutes: 0\n",
		"input type":    "on:\n  workflow_dispatch:\n    inputs:\n      a:\n        type: number\nsteps:\n  - run: true\n",
		"empty choice":  "on:\n  workflow_dispatch:\n    inputs:\n      a:\n        type: choice\nsteps:\n  - run: true\n",
		"required word": "on:\n  workflow_dispatch:\n    inputs:\n      a:\n        required: maybe\nsteps:\n  - run: true\n",
	} {
		if _, err := parseWorkflow(".forkmesh/a.yml", bad); err == nil {
			t.Fatalf("%s accepted", name)
		}
	}
}
