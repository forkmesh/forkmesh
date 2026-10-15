package actions

import (
	"bufio"
	"encoding/json"
	"math/rand"
	"os"
	"os/exec"
	"reflect"
	"strconv"
	"strings"
	"testing"
)

type parityHarness struct {
	t      *testing.T
	stdin  *json.Encoder
	stdout *bufio.Scanner
}

func startParity(t *testing.T) *parityHarness {
	binary := os.Getenv("FORKMESH_ACTIONS_PARITY_BIN")
	if binary == "" {
		t.Skip("set FORKMESH_ACTIONS_PARITY_BIN to a build of the C++ ActionFile harness")
	}
	cmd := exec.Command(binary)
	in, err := cmd.StdinPipe()
	if err != nil {
		t.Fatal(err)
	}
	out, err := cmd.StdoutPipe()
	if err != nil {
		t.Fatal(err)
	}
	if err := cmd.Start(); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		_ = in.Close()
		_ = cmd.Wait()
	})
	scanner := bufio.NewScanner(out)
	scanner.Buffer(make([]byte, 1<<20), 64<<20)
	return &parityHarness{t: t, stdin: json.NewEncoder(in), stdout: scanner}
}

func (h *parityHarness) ask(request map[string]any) map[string]any {
	if err := h.stdin.Encode(request); err != nil {
		h.t.Fatal(err)
	}
	if !h.stdout.Scan() {
		h.t.Fatal("harness closed")
	}
	var reply map[string]any
	if err := json.Unmarshal(h.stdout.Bytes(), &reply); err != nil {
		h.t.Fatal(err)
	}
	return reply
}

func goParseView(wf Workflow) map[string]any {
	list := func(values []string) []any {
		out := []any{}
		for _, value := range values {
			out = append(out, value)
		}
		return out
	}
	env := map[string]any{}
	for key, value := range wf.Env {
		env[key] = value
	}
	steps := []any{}
	for _, step := range wf.Steps {
		steps = append(steps, map[string]any{"name": step.Name, "run": step.Run})
	}
	return map[string]any{"name": wf.Name, "on": list(wf.On), "pushChangedPaths": list(wf.PushChangedPaths), "env": env,
		"runsOn": list(wf.RunsOn), "needs": list(wf.Needs), "steps": steps, "checkout": wf.Checkout,
		"fetchDepth": float64(wf.FetchDepth), "valid": wf.Valid, "error": wf.Error}
}

var parityFragments = []string{
	"name: CI", "name: \"Quoted name\"", "name: 'single'", "name:", "on: push", "on: [push, pull_request]", "on:",
	"  push:", "    paths:", "      - mobile/**", "      - \"world/*\"", "  pull_request:", "    branches: [main]",
	"runs-on: Mac1", "runs-on: [a, B]", "runs-on:", "  - linux", "checkout: false", "checkout: no", "checkout: yes",
	"fetch-depth: 0", "fetch-depth: 7", "fetch-depth: -1", "fetch-depth: x", "needs: ci", "needs: [build, ci]",
	"env:", "  A: 1", "  B: \"two words\"", "  C: ${{ vars.TOKEN }}", "steps:", "  - run: echo hi", "  - name: Build",
	"    run: make", "  - name: Block", "    run: |", "      line one", "        nested", "      # comment", "      line two",
	"    run: >-", "  -", "    run: echo nested", "jobs:", "  build:", "    runs-on: linux", "    needs: [test]",
	"    steps:", "      - run: echo job", "      - name: Named", "        run: echo named", "  test:", "    steps:",
	"      - run: ctest", "# comment", "", "\t- run: tabbed", "key with: colon: inside", "- top seq", "  - deep",
	"- name: list map", "  run: echo x", "- -x: y", "é: unicode", "  résumé: café", "run: ☃",
	"  ", "on: \"release\"", "on: [ workflow_dispatch ]", "steps: [a, b]", "  - run: 'quoted run'", "  - run: \"x: y\"",
}

func randomWorkflow(random *rand.Rand) string {
	var lines []string
	count := 1 + random.Intn(18)
	for i := 0; i < count; i++ {
		line := parityFragments[random.Intn(len(parityFragments))]
		if random.Intn(4) == 0 {
			line = strings.Repeat(" ", random.Intn(9)) + line
		}
		if random.Intn(10) == 0 {
			line = "- " + line
		}
		if random.Intn(15) == 0 {
			line += "\r"
		}
		lines = append(lines, line)
	}
	return strings.Join(lines, "\n")
}

func TestParityWithCppActionFile(t *testing.T) {
	h := startParity(t)
	random := rand.New(rand.NewSource(1336))
	corpus := []string{
		"name: CI\non: [push]\njobs:\n  test:\n    steps:\n      - run: echo hi\n",
		"name: Deploy\non:\n  push:\n    branches: [main]\n    paths:\n      - \"world/*\"\nneeds: [.forkmesh/ci.yml]\nruns-on: [mirror2, mirror3]\nenv:\n  A: x\njobs:\n  deploy:\n    steps:\n      - name: Go\n        run: |\n          ./deploy.sh\n          echo done\n",
	}
	count := 4000
	if n, err := strconv.Atoi(os.Getenv("FORKMESH_ACTIONS_PARITY_COUNT")); err == nil && n > 0 {
		count = n
	}
	for i := 0; i < count; i++ {
		corpus = append(corpus, randomWorkflow(random))
	}
	for _, content := range corpus {
		want := h.ask(map[string]any{"kind": "parse", "path": ".forkmesh/p.yml", "content": content})
		got := goParseView(Parse(".forkmesh/p.yml", content))
		if !reflect.DeepEqual(want, got) {
			t.Fatalf("parse mismatch for %q\nC++: %v\nGo:  %v", content, want, got)
		}
	}
	patterns := []string{"*.md", "docs/*", "mobile/**", "[ab]*", "[!a]*", "a?c", "x.y", "a+b", "(z)", "[a/b]", "[]x]", "{a,b}", "a$", "^a", "a|b", "\\d", "[", "]"}
	paths := []string{"README.md", "docs/a", "docs/a/b", "mobile/x", "mobile/x/y", "apple", "banana", "abc", "x.y", "xzy", "a+b", "(z)", "a", "]x", "{a,b}", "a$", "^a", "a|b", "\\d", "d", "[", "]"}
	for _, pattern := range patterns {
		for _, p := range paths {
			want := h.ask(map[string]any{"kind": "match", "pattern": pattern, "path": p})["match"]
			wf := Workflow{PushChangedPaths: []string{pattern}}
			if got := wf.MatchesPushChangedPaths([]string{p}); got != want {
				t.Errorf("match %q %q: C++ %v Go %v", pattern, p, want, got)
			}
		}
	}
	vars := map[string]any{"TOKEN": "s3cr3t", "A_1": "x"}
	goVars := map[string]string{"TOKEN": "s3cr3t", "A_1": "x"}
	for _, text := range []string{"${{ vars.TOKEN }}", "${{vars.A_1}}${A_1}${B}", "$TOKEN ${TOKEN} ${{  vars.MISSING  }}", "${{ vars.TOKEN}", "\t${{\tvars.TOKEN\t}}"} {
		want := h.ask(map[string]any{"kind": "subst", "text": text, "vars": vars})["out"]
		if got := Substitute(text, goVars); got != want {
			t.Errorf("substitute %q: C++ %q Go %q", text, want, got)
		}
	}
	for _, text := range []string{"a, b;c\td\ne", "  ", "A,a,,B", "x y"} {
		want := h.ask(map[string]any{"kind": "labels", "text": text})["labels"]
		got := []any{}
		for _, label := range ParseLabelList(text) {
			got = append(got, label)
		}
		if !reflect.DeepEqual(want, got) {
			t.Errorf("labels %q: C++ %v Go %v", text, want, got)
		}
	}
}

func TestParityStoreAndPhoneHelpers(t *testing.T) {
	h := startParity(t)
	runs := []map[string]any{
		{"id": 7, "owner": "owner", "name": "repo", "workflowPath": ".forkmesh/ci.yml", "workflowName": "CI \"x\" <a&b> \u00e9\u2603\U0001F680",
			"workflowContent": "on: push\n\tsteps:\r\n  - run: echo \\\\ \u0001\u001f\u007f /", "commit": "abc", "ref": "refs/heads/main",
			"status": "running", "stepStatuses": []any{"success", " running ", ""}, "currentStepIndex": 1, "pullNumber": 32,
			"pullAuthor": "atomic", "firstTimePullAuthor": true, "createdAtMs": 1700000000123.0, "startedAtMs": 0, "finishedAtMs": 1.5},
		{"id": 9, "owner": "\u00c9cole", "name": "na\u00efve \U0001F600-x.y_z", "stepStatuses": []any{}},
		{"id": "8", "status": 3, "currentStepIndex": 2.5, "createdAtMs": "123"},
		{},
	}
	for _, record := range runs {
		want := h.ask(map[string]any{"kind": "run", "run": record})
		raw, _ := json.Marshal(record)
		object, _ := decodeJSONObject(raw)
		run := RunFromObject(object)
		if got := string(run.Encode()); got != want["json"] {
			t.Errorf("meta.json bytes differ\nC++: %q\nGo:  %q", want["json"], got)
		}
		if run.LegacyRepoKey() != want["legacy"] || run.RepoKey() != want["key"] {
			t.Errorf("keys: C++ %v %v Go %v %v", want["legacy"], want["key"], run.LegacyRepoKey(), run.RepoKey())
		}
	}
	logs := []string{"", "hello", "a\U0001F680b\U0001F680c", strings.Repeat("x", 50)}
	for _, log := range logs {
		for _, since := range []int64{-1, 0, 1, 2, 3, 5, 49, 50, 51, 100} {
			for _, max := range []int64{2, 3, 10, 128 * 1024} {
				want := h.ask(map[string]any{"kind": "slice", "log": log, "since": since, "max": max})
				got := LogSliceOf(log, since, max)
				if got.Text != want["text"] || float64(got.From) != want["from"] || float64(got.Next) != want["next"] || got.Reset != want["reset"] {
					t.Errorf("slice %q since %d max %d: C++ %v Go %+v", log, since, max, want, got)
				}
			}
		}
	}
	for _, action := range append(append([]string{}, allActions...), "bogus") {
		for _, name := range []string{"", "Deploy  \n now", "\u2603"} {
			intent := Intent{Action: action, Repo: "o/n", Path: ".forkmesh/ci.yml", Branch: "main", Run: 12}
			want := h.ask(map[string]any{"kind": "summary", "action": action, "repo": "o/n", "path": intent.Path, "branch": "main", "run": 12, "name": name})
			if got := ActionSummary(intent, name); got != want["summary"] {
				t.Errorf("summary %s %q:\nC++: %q\nGo:  %q", action, name, want["summary"], got)
			}
			if got := IntentSubject(intent); got != want["subject"] {
				t.Errorf("subject %s: %q vs %q", action, want["subject"], got)
			}
		}
	}
}
