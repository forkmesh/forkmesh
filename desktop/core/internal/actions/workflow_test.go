package actions

import (
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"
)

func TestSubstitute(t *testing.T) {
	vars := map[string]string{"TOKEN": "s3cr3t"}
	script := "channel=\"${RELEASE_CHANNEL:-latest}\"\nos=linux; arch=x86_64\ndest=\"releases/${channel}/forkmesh-${os}-${arch}\""
	if got := Substitute(script, vars); got != script {
		t.Fatalf("shell variables changed: %q", got)
	}
	if got := Substitute("auth ${{ vars.TOKEN }} and ${TOKEN}", vars); got != "auth s3cr3t and s3cr3t" {
		t.Fatalf("declared vars: %q", got)
	}
	if got := Substitute("x=${{ vars.MISSING }}-end", vars); got != "x=-end" {
		t.Fatalf("missing context: %q", got)
	}
}

func TestTriggers(t *testing.T) {
	rel := Parse(".forkmesh/release.yml", "name: Release\non: [release, workflow_dispatch]\njobs:\n  release:\n    steps:\n      - run: echo hi\n")
	if !rel.Valid || !rel.TriggersOnRelease() || rel.TriggersOnPush() || !rel.AllowsManualRun() {
		t.Fatalf("release: %+v", rel)
	}
	push := Parse(".forkmesh/ci.yml", "name: CI\non: [push]\njobs:\n  test:\n    steps:\n      - run: echo hi\n")
	if !push.TriggersOnPush() || push.TriggersOnRelease() || push.TriggersOnPullRequest() {
		t.Fatalf("push: %+v", push)
	}
	checks := Parse(".forkmesh/ci.yml", "name: CI\non: [push, pull_request]\njobs:\n  test:\n    steps:\n      - run: echo hi\n")
	if !checks.TriggersOnPullRequest() || !checks.TriggersOnPush() {
		t.Fatal("both events")
	}
	filtered := Parse(".forkmesh/ci.yml", "name: CI\non:\n  pull_request:\n    branches: [main]\n  push:\njobs:\n  test:\n    steps:\n      - run: echo hi\n")
	if !filtered.Valid || !filtered.TriggersOnPullRequest() || !filtered.TriggersOnPush() {
		t.Fatalf("mapping form: %+v", filtered)
	}
	if !reflect.DeepEqual(filtered.On, []string{"pull_request", "push"}) {
		t.Fatalf("mapping keys come out sorted like a QMap: %v", filtered.On)
	}
	paths := Parse(".forkmesh/android.yml", "name: Android\non:\n  push:\n    paths:\n      - mobile/**\njobs:\n  build:\n    steps:\n      - run: echo hi\n")
	if !paths.Valid || !paths.TriggersOnPush() || !paths.MatchesPushChangedPaths([]string{"mobile/main.dart"}) || paths.MatchesPushChangedPaths([]string{"app/main.dart"}) {
		t.Fatalf("paths: %+v", paths)
	}
	if paths.MatchesPushChangedPaths([]string{"mobile/lib/deep.dart"}) {
		t.Fatal("Qt's path wildcard does not let ** cross a slash")
	}
	if paths.MatchesPushChangedPaths(nil) {
		t.Fatal("no changed paths never match a filter")
	}
	nodes := Parse(".forkmesh/deploy-nodes.yml", "name: Deploy nodes\non:\n  push:\n    branches: [main]\n    paths:\n      - \"world/*\"\n      - \"world/*/*\"\n      - \"world/*/*/*\"\n      - \"world/*/*/*/*\"\njobs:\n  deploy:\n    steps:\n      - run: echo deploy\n")
	if !nodes.MatchesPushChangedPaths([]string{"world/public/world/scene.js"}) || nodes.MatchesPushChangedPaths([]string{"desktop/x"}) {
		t.Fatalf("world filters: %v", nodes.PushChangedPaths)
	}
}

func TestParserShapesAndErrors(t *testing.T) {
	if wf := Parse(".forkmesh/empty.yml", "\n  # only a comment\n\n"); wf.Valid || wf.Error != "empty workflow file" || wf.Name != "empty.yml" {
		t.Fatalf("empty: %+v", wf)
	}
	if wf := Parse(".forkmesh/list.yml", "- a\n- b\n"); wf.Error != "workflow root must be a mapping" {
		t.Fatalf("root: %+v", wf)
	}
	if wf := Parse(".forkmesh/none.yml", "name: Nothing\non: push\n"); wf.Error != "workflow has no runnable steps" || wf.Name != "Nothing" {
		t.Fatalf("steps: %+v", wf)
	}
	content := "name: \"Quoted\"\non: push\ncheckout: off\nfetch-depth: 0\nenv:\n  A: one\n  B: 'two'\nsteps:\n  - name: First\n    run: |\n      echo one\n        indented\n      # dropped comment\n      echo two\n  - run: echo top\n  - name: no run\njobs:\n  zeta:\n    steps:\n      - run: echo zeta\n  alpha:\n    runs-on: Linux\n    steps:\n      - name:  Alpha step \n        run: echo alpha\n"
	wf := Parse(".forkmesh/shape.yml", content)
	if !wf.Valid || wf.Name != "Quoted" || wf.Checkout || wf.FetchDepth != 0 {
		t.Fatalf("shape: %+v", wf)
	}
	if wf.Env["A"] != "one" || wf.Env["B"] != "two" {
		t.Fatalf("env: %v", wf.Env)
	}
	want := []Step{{Name: "First", Run: "echo one\n  indented\necho two"}, {Run: "echo top"}, {Name: "Alpha step", Run: "echo alpha"}, {Run: "echo zeta"}}
	if !reflect.DeepEqual(wf.Steps, want) {
		t.Fatalf("steps: %#v", wf.Steps)
	}
	if !reflect.DeepEqual(wf.RunsOn, []string{"linux"}) {
		t.Fatalf("runs-on: %v", wf.RunsOn)
	}
	if wf.Content != content || wf.Path != ".forkmesh/shape.yml" {
		t.Fatal("content and path are kept")
	}
	tabs := Parse(".forkmesh/tabs.yml", "on: push\r\nsteps:\r\n\t- run: echo tab\r\n")
	if !tabs.Valid || len(tabs.Steps) != 1 || tabs.Steps[0].Run != "echo tab" {
		t.Fatalf("tabs and CRLF: %+v", tabs)
	}
	badDepth := Parse(".forkmesh/d.yml", "fetch-depth: -3\nsteps:\n  - run: x\n")
	if badDepth.FetchDepth != 2 {
		t.Fatalf("negative depth keeps the default: %d", badDepth.FetchDepth)
	}
	flow := Parse(".forkmesh/f.yml", "on: [ push , 'release', ]\nruns-on: [A, b, a]\nsteps:\n  - run: x\n")
	if !reflect.DeepEqual(flow.On, []string{"push", "release"}) || !reflect.DeepEqual(flow.RunsOn, []string{"a", "b"}) {
		t.Fatalf("flow: %v %v", flow.On, flow.RunsOn)
	}
}

func TestFetchDepthAndRunsOn(t *testing.T) {
	steps := "on: push\nsteps:\n  - run: x\n"
	if Parse(".forkmesh/ci.yml", steps).FetchDepth != 2 {
		t.Fatal("default depth")
	}
	if Parse(".forkmesh/x.yml", "fetch-depth: 50\n"+steps).FetchDepth != 50 {
		t.Fatal("depth 50")
	}
	anywhere := Parse(".forkmesh/ci.yml", "name: CI\non: [push]\njobs:\n  test:\n    steps:\n      - run: echo hi\n")
	if len(anywhere.RunsOn) != 0 || !RunsOnNode(anywhere.RunsOn, []string{"mirror2"}) {
		t.Fatal("undedicated")
	}
	tests := Parse(".forkmesh/tests.yml", "name: Tests\non: [push]\nruns-on: forkmesh\njobs:\n  test:\n    steps:\n      - run: ctest\n")
	if !reflect.DeepEqual(tests.RunsOn, []string{"forkmesh"}) || !RunsOnNode(tests.RunsOn, []string{"forkmesh", "linux"}) ||
		RunsOnNode(tests.RunsOn, []string{"mirror2", "linux"}) || !RunsOnNode(tests.RunsOn, []string{"ForkMesh"}) {
		t.Fatal("dedicated")
	}
	deploy := Parse(".forkmesh/deploy.yml", "name: Deploy\non: [push]\nruns-on: [mirror2, mirror3]\njobs:\n  deploy:\n    steps:\n      - run: wrangler deploy\n")
	if len(deploy.RunsOn) != 2 || !RunsOnNode(deploy.RunsOn, []string{"mirror3"}) || RunsOnNode(deploy.RunsOn, []string{"mac1"}) {
		t.Fatal("list")
	}
	ios := Parse(".forkmesh/ios.yml", "name: iOS\non: [workflow_dispatch]\njobs:\n  build:\n    runs-on:\n      - mac1\n    steps:\n      - run: flutter build ios\n")
	if !reflect.DeepEqual(ios.RunsOn, []string{"mac1"}) || RunsOnNode(ios.RunsOn, []string{"forkmesh"}) {
		t.Fatal("job level")
	}
	any := Parse(".forkmesh/any.yml", "name: Any\non: [push]\nruns-on: any\njobs:\n  j:\n    steps:\n      - run: echo hi\n")
	if !RunsOnNode(any.RunsOn, []string{"mirror2"}) {
		t.Fatal("any")
	}
}

func TestPinsAndLabels(t *testing.T) {
	var pins []string
	if PinnedNode(pins, ".forkmesh/ci.yml") != "" {
		t.Fatal("no pin")
	}
	pins = SetPinnedNode(pins, []string{".forkmesh/ci.yml"}, "Mac1")
	if PinnedNode(pins, ".forkmesh/ci.yml") != "mac1" || PinnedNode(pins, ".forkmesh/deploy.yml") != "" {
		t.Fatal("pin")
	}
	pins = SetPinnedNode(pins, []string{".forkmesh/ci.yml", ".forkmesh/deploy.yml"}, "mirror2")
	if len(pins) != 2 || PinnedNode(pins, ".forkmesh/ci.yml") != "mirror2" {
		t.Fatal("repin")
	}
	if pins[0] != ".forkmesh/ci.yml\tmirror2" || pins[1] != ".forkmesh/deploy.yml\tmirror2" {
		t.Fatalf("sorted: %q", pins)
	}
	pins = SetPinnedNode(pins, []string{".forkmesh/ci.yml"}, "")
	if len(pins) != 1 || PinnedNode(pins, ".forkmesh/ci.yml") != "" || PinnedNode(pins, ".forkmesh/deploy.yml") != "mirror2" {
		t.Fatal("clear")
	}
	wf := Workflow{Path: ".forkmesh/deploy.yml", RunsOn: []string{"mac1"}}
	if !WorkflowRunsOnThisNode(pins, wf, []string{"mirror2"}) || WorkflowRunsOnThisNode(pins, wf, []string{"mac1"}) {
		t.Fatal("pin decides")
	}
	labels := NodeLabels("Mac1", "", "ios, xcode  flutter,ios")
	for _, want := range []string{"mac1", "ios", "xcode", "flutter", osLabel()} {
		if !containsString(labels, want) {
			t.Fatalf("labels %v miss %s", labels, want)
		}
	}
	count := 0
	for _, label := range labels {
		if label == "ios" {
			count++
		}
	}
	if count != 1 || labels[0] != "mac1" {
		t.Fatalf("labels: %v", labels)
	}
	if !containsString(NodeLabels("", "mirror2", ""), "mirror2") || len(ParseLabelList("  ")) != 0 {
		t.Fatal("headless labels")
	}
}

func TestNeeds(t *testing.T) {
	deploy := Parse(".forkmesh/deploy.yml", "name: Deploy\non: [push]\nneeds: [.forkmesh/ci.yml]\njobs:\n  deploy:\n    steps:\n      - run: ./deploy.sh\n")
	if !deploy.Valid || !reflect.DeepEqual(deploy.Needs, []string{".forkmesh/ci.yml"}) {
		t.Fatal(deploy.Needs)
	}
	job := Parse(".forkmesh/publish.yml", "name: Publish\non: [push]\njobs:\n  publish:\n    needs:\n      - CI tests\n    steps:\n      - run: echo hi\n")
	if !reflect.DeepEqual(job.Needs, []string{"CI tests"}) {
		t.Fatal(job.Needs)
	}
	siblings := Parse(".forkmesh/build.yml", "name: Build\non: [push]\njobs:\n  compile:\n    steps:\n      - run: make\n  test:\n    needs: compile\n    steps:\n      - run: make test\n")
	if len(siblings.Needs) != 0 {
		t.Fatal(siblings.Needs)
	}
	if !NeedsMatches("ci", "CI tests", ".forkmesh/ci.yml") || !NeedsMatches("CI TESTS", "CI tests", ".forkmesh/ci.yml") ||
		!NeedsMatches(".forkmesh/ci.yml", "CI tests", ".forkmesh/ci.yml") || NeedsMatches("ci-qt", "CI tests", ".forkmesh/ci.yml") {
		t.Fatal("matches")
	}
	dependency := Run{ID: 1, Owner: "forkmesh", Name: "forkmesh", WorkflowPath: ".forkmesh/ci.yml", WorkflowName: "CI tests", Commit: "abc123"}
	dependent := dependency
	dependent.ID = 2
	dependent.WorkflowPath = ".forkmesh/deploy.yml"
	needs := []string{".forkmesh/ci.yml"}
	for status, want := range map[string]NeedsState{StatusRunning: NeedsWaiting, StatusAwaitingApproval: NeedsWaiting, StatusSuccess: NeedsReady, StatusFailed: NeedsBlocked} {
		dependency.Status = status
		state, detail := ResolveNeeds(dependent, needs, []Run{dependency})
		if state != want {
			t.Fatalf("%s: %v", status, state)
		}
		if status == StatusFailed && detail != "\"CI tests\" failed" {
			t.Fatalf("detail %q", detail)
		}
	}
	other := dependency
	other.Commit = "def456"
	other.Status = StatusFailed
	if state, _ := ResolveNeeds(dependent, needs, []Run{other}); state != NeedsReady {
		t.Fatal("other commit")
	}
}

func TestParseDirAndRetire(t *testing.T) {
	dir := t.TempDir()
	if err := os.MkdirAll(filepath.Join(dir, ".forkmesh", "issues"), 0o700); err != nil {
		t.Fatal(err)
	}
	write := func(name, text string) {
		if err := os.WriteFile(filepath.Join(dir, ".forkmesh", name), []byte(text), 0o600); err != nil {
			t.Fatal(err)
		}
	}
	write("b.yaml", "name: B\r\non: push\r\nsteps:\r\n  - run: echo b\r\n")
	write("a.yml", "on: push\nsteps:\n  - run: echo a\n")
	write(".hidden.yml", "on: push\nsteps:\n  - run: x\n")
	write("notes.txt", "x")
	workflows := ParseDir(dir)
	if len(workflows) != 2 || workflows[0].Path != ".forkmesh/a.yml" || workflows[1].Path != ".forkmesh/b.yaml" {
		t.Fatalf("dir: %+v", workflows)
	}
	if strings.Contains(workflows[1].Content, "\r") {
		t.Fatal("text-mode read strips carriage returns")
	}
	if !DefaultBranchRetiresPush(".forkmesh/a.yml", "on: [workflow_dispatch]\nsteps:\n  - run: x\n") {
		t.Fatal("retired")
	}
	if DefaultBranchRetiresPush(".forkmesh/a.yml", "on: push\nsteps:\n  - run: x\n") || DefaultBranchRetiresPush(".forkmesh/a.yml", "") ||
		DefaultBranchRetiresPush(".forkmesh/a.yml", "on: [workflow_dispatch]\n") {
		t.Fatal("not retired")
	}
}

func TestWildcard(t *testing.T) {
	cases := []struct {
		pattern, path string
		match         bool
	}{
		{"*.md", "README.md", true},
		{"*.md", "docs/README.md", false},
		{"docs/*", "docs/a", true},
		{"docs/?", "docs/ab", false},
		{"[ab]*", "apple", true},
		{"[!ab]*", "apple", false},
		{"a.b", "axb", false},
		{"a+b", "a+b", true},
		{"(x)", "(x)", true},
		{"[a/b]", "a", false},
	}
	for _, c := range cases {
		re, ok := wildcardRegexp(c.pattern)
		got := ok && re.MatchString(c.path)
		if got != c.match {
			t.Errorf("%q vs %q: %v", c.pattern, c.path, got)
		}
	}
}
