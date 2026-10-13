package mirrornode

import (
	"context"
	"encoding/base64"
	"encoding/json"
	"path/filepath"
	"strings"
	"testing"
)

func rulesPolicyJSON(t *testing.T, rules ...map[string]any) string {
	t.Helper()
	raw, err := json.Marshal(map[string]any{"schema": pullRulesSchema, "rules": rules})
	if err != nil {
		t.Fatal(err)
	}
	return string(raw)
}

func (f *mergeFixture) commitOnMain(t *testing.T, path, content, message string) {
	t.Helper()
	writeFile(t, filepath.Join(f.source, path), content)
	run(t, f.source, "git", "add", "-A")
	run(t, f.source, "git", "commit", "-q", "-m", message)
	run(t, f.source, "git", "push", "-q", f.bare, "main:refs/heads/main")
	f.mainOid = run(t, f.source, "git", "rev-parse", "main")
}

func (f *mergeFixture) commitOnFeature(t *testing.T, path, content, message string) {
	t.Helper()
	run(t, f.source, "git", "checkout", "-q", "feature")
	writeFile(t, filepath.Join(f.source, path), content)
	run(t, f.source, "git", "add", "-A")
	run(t, f.source, "git", "commit", "-q", "-m", message)
	run(t, f.source, "git", "push", "-q", "-f", f.bare, "feature:refs/heads/feature")
	f.featureOid = run(t, f.source, "git", "rev-parse", "feature")
	run(t, f.source, "git", "checkout", "-q", "main")
}

func (f *mergeFixture) evaluate(t *testing.T, changes map[string]any) PullRulesReport {
	t.Helper()
	value := map[string]any{
		"schemaVersion": 1, "type": pullRulesEvaluateType, "owner": "forkmesh", "repository": "forkmesh",
		"base": f.mainOid, "head": f.featureOid, "pullNumber": 7,
	}
	for key, item := range changes {
		value[key] = item
	}
	raw, _ := json.Marshal(value)
	result, err := f.executor.Handle(strings.NewReader(string(raw)))
	if err != nil {
		t.Fatalf("evaluate: %v", err)
	}
	encoded, _ := json.Marshal(result["rules"])
	var report PullRulesReport
	if err := json.Unmarshal(encoded, &report); err != nil {
		t.Fatal(err)
	}
	return report
}

func TestPullRulesPolicyRejectsMalformedRules(t *testing.T) {
	for name, raw := range map[string]string{
		"schema":    `{"schema":"other","rules":[]}`,
		"unknown":   `{"schema":"forkmesh-pull-rules-v1","rules":[],"extra":1}`,
		"kind":      `{"schema":"forkmesh-pull-rules-v1","rules":[{"id":"a","kind":"vibes"}]}`,
		"id":        `{"schema":"forkmesh-pull-rules-v1","rules":[{"id":"Bad Id","kind":"tests"}]}`,
		"duplicate": `{"schema":"forkmesh-pull-rules-v1","rules":[{"id":"a","kind":"tests"},{"id":"a","kind":"docs"}]}`,
		"size":      `{"schema":"forkmesh-pull-rules-v1","rules":[{"id":"a","kind":"size"}]}`,
		"paths":     `{"schema":"forkmesh-pull-rules-v1","rules":[{"id":"a","kind":"paths"}]}`,
		"regex":     `{"schema":"forkmesh-pull-rules-v1","rules":[{"id":"a","kind":"commits","subjectPattern":"("}]}`,
		"model":     `{"schema":"forkmesh-pull-rules-v1","rules":[{"id":"a","kind":"model","models":[]}]}`,
		"enforce":   `{"schema":"forkmesh-pull-rules-v1","rules":[{"id":"a","kind":"tests","enforcement":"maybe"}]}`,
		"branch":    `{"schema":"forkmesh-pull-rules-v1","rules":[{"id":"a","kind":"tests","branches":["../x y"]}]}`,
	} {
		if _, err := ParsePullRulesPolicy([]byte(raw)); err == nil {
			t.Errorf("%s: accepted %s", name, raw)
		}
	}
	if _, err := ParsePullRulesPolicy([]byte(`{"schema":"forkmesh-pull-rules-v1","rules":[]}`)); err != nil {
		t.Fatalf("empty policy: %v", err)
	}
}

func TestPullRulePathGlobs(t *testing.T) {
	cases := []struct {
		pattern, path string
		want          bool
	}{
		{"*.go", "server/merge.go", true},
		{"*.go", "merge.go", true},
		{"*.go", "merge.golang", false},
		{"docs/**", "docs/a/b.md", true},
		{"docs/**", "app/docs/a.md", false},
		{"**/docs/**", "app/docs/a.md", true},
		{"**/docs/**", "docs/a.md", true},
		{"src/*.c", "src/a/b.c", false},
		{".github/workflows/*", ".github/workflows/ci.yml", true},
	}
	for _, c := range cases {
		re, err := compilePathGlob(c.pattern, true)
		if err != nil {
			t.Fatal(err)
		}
		if got := re.MatchString(c.path); got != c.want {
			t.Errorf("%s ~ %s = %v", c.pattern, c.path, got)
		}
	}
	branch, _ := compilePathGlob("release/*", false)
	if !branch.MatchString("release/1.2") || branch.MatchString("release/1/2") || branch.MatchString("main") {
		t.Fatal("branch glob")
	}
}

func TestMergeExecutorEnforcesRepositoryPullRules(t *testing.T) {
	f := newMergeFixture(t)
	f.commitOnMain(t, pullRulesPolicyPath, rulesPolicyJSON(t,
		map[string]any{"id": "tests", "kind": "tests", "branches": []string{"main"}},
		map[string]any{"id": "small", "kind": "size", "maxFiles": 5, "enforcement": "warn"},
		map[string]any{"id": "release-docs", "kind": "docs", "branches": []string{"release/*"}},
	), "require tests")
	f.commitOnFeature(t, "lib/code.go", "package lib\n", "add code")

	report := f.evaluate(t, nil)
	if !report.Available || report.Branch != "main" || report.Passed || !report.Blocked || len(report.Rules) != 2 {
		t.Fatalf("report: %+v", report)
	}
	if report.Rules[0].ID != "tests" || report.Rules[0].Status != "failed" || report.Rules[0].Suggestion == "" ||
		!strings.Contains(report.Rules[0].Problems[0], "lib/code.go") {
		t.Fatalf("tests rule: %+v", report.Rules[0])
	}
	if report.Rules[1].Status != "passed" {
		t.Fatalf("size rule: %+v", report.Rules[1])
	}
	result := f.request(t, "execute", "merge", map[string]any{"requestId": "merge_rules_blocked", "overrideReview": true})
	if result["error"] != "rules_required" || f.ref(t, "refs/heads/main") != f.mainOid {
		t.Fatalf("a failing rule did not block the merge: %+v", result)
	}

	f.commitOnFeature(t, "lib/code_test.go", "package lib\n", "test code")
	report = f.evaluate(t, nil)
	if report.Blocked || !report.Passed {
		t.Fatalf("tests now present: %+v", report)
	}
	if result := f.request(t, "execute", "merge", map[string]any{"requestId": "merge_rules_passed"}); result["status"] != "merged" {
		t.Fatalf("passing rules did not merge: %+v", result)
	}
}

func TestPullRulesWarningsFlagWithoutBlocking(t *testing.T) {
	f := newMergeFixture(t)
	f.commitOnMain(t, pullRulesPolicyPath, rulesPolicyJSON(t,
		map[string]any{"id": "tiny", "kind": "size", "maxFiles": 1, "maxLines": 1, "enforcement": "warn",
			"suggestion": "Keep it small."},
	), "warn on size")
	f.commitOnFeature(t, "two.txt", "a\nb\nc\n", "second file")
	report := f.evaluate(t, nil)
	if report.Passed || report.Blocked || report.Rules[0].Suggestion != "Keep it small." || len(report.Rules[0].Problems) != 2 {
		t.Fatalf("warning: %+v", report)
	}
	if result := f.request(t, "execute", "merge", nil); result["status"] != "merged" {
		t.Fatalf("a warning blocked the merge: %+v", result)
	}
}

func TestPullRulesFindSecretsProtectedPathsAndCommitFormat(t *testing.T) {
	f := newMergeFixture(t)
	f.commitOnFeature(t, "config/keys.env", "token=ghp_"+strings.Repeat("a", 36)+"\n", "wip")
	f.commitOnFeature(t, ".github/workflows/ci.yml", "on: push\n", "feat: ci")
	org := rulesPolicyJSON(t,
		map[string]any{"id": "secrets", "kind": "secrets"},
		map[string]any{"id": "workflows", "kind": "paths", "paths": []string{".github/workflows/**"}},
		map[string]any{"id": "conventional", "kind": "commits", "subjectPattern": `^(feat|fix|docs|chore): `},
	)
	report := f.evaluate(t, map[string]any{"orgRules": org})
	if !report.Blocked || len(report.Rules) != 3 {
		t.Fatalf("report: %+v", report)
	}
	for _, rule := range report.Rules {
		if rule.Source != pullRuleSourceOrg || rule.Status != "failed" {
			t.Fatalf("rule: %+v", rule)
		}
	}
	if !strings.Contains(report.Rules[0].Problems[0], "config/keys.env:1") ||
		strings.Contains(report.Rules[0].Problems[0], "ghp_") {
		t.Fatalf("secret finding: %+v", report.Rules[0])
	}
	if !strings.Contains(report.Rules[1].Problems[0], ".github/workflows/ci.yml") {
		t.Fatalf("paths finding: %+v", report.Rules[1])
	}
	if len(report.Rules[2].Problems) != 2 {
		t.Fatalf("commit format should flag 'add feature' and 'wip': %+v", report.Rules[2])
	}
	broken := f.evaluate(t, map[string]any{"orgRules": `{"schema":"forkmesh-pull-rules-v1","rules":[{"id":"x"}]}`})
	if !broken.Blocked || broken.Rules[0].ID != "policy" {
		t.Fatalf("an unreadable org policy must lock: %+v", broken)
	}
}

func TestPullRulesFailClosedOnAnInvalidRepositoryPolicy(t *testing.T) {
	f := newMergeFixture(t)
	f.commitOnMain(t, pullRulesPolicyPath, `{"schema":"forkmesh-pull-rules-v1"`, "broken rules")
	if result := f.request(t, "execute", "merge", nil); result["error"] != "rules_required" {
		t.Fatalf("invalid policy: %+v", result)
	}
}

func modelGrantToken(t *testing.T, router, runner *Identity, model string) (string, agentModelGrant) {
	t.Helper()
	grant := agentModelGrant{
		Runner: "pool1", RunnerKey: runner.PublicKey(), Owner: "forkmesh", Repository: "forkmesh",
		Ref: "refs/heads/agent/fix-1", Provider: "claude-code", Model: model, LeaseID: "lease_000001",
		IssuedAt: 1700000000000, ExpiresAt: 1700003600000,
	}
	message := agentModelGrantMessage(grant)
	return base64.RawURLEncoding.EncodeToString([]byte(message)) + "." + router.Sign([]byte(message)), grant
}

func (f *mergeFixture) attestFeature(t *testing.T, grantToken string, grant agentModelGrant, runner *Identity) {
	t.Helper()
	backend := &runnerBackend{cfg: &RunnerConfig{WorkRoot: t.TempDir()}, identity: runner, node: "pool1"}
	run(t, ".", "git", "--git-dir="+f.bare, "update-ref", "refs/fm/result", f.featureOid)
	if err := backend.attestCommits(context.Background(), f.bare, f.featureOid, []string{f.mainOid}, grantToken, grant); err != nil {
		t.Fatal(err)
	}
	f.featureOid = f.ref(t, "refs/fm/result")
	run(t, ".", "git", "--git-dir="+f.bare, "update-ref", "refs/heads/feature", f.featureOid)
	run(t, f.source, "git", "fetch", "-q", f.bare, "+refs/heads/feature:refs/heads/feature")
}

func TestModelRuleAcceptsOnlyRouterGrantedRunnerAttestations(t *testing.T) {
	f := newMergeFixture(t)
	f.commitOnMain(t, pullRulesPolicyPath, rulesPolicyJSON(t,
		map[string]any{"id": "opus", "kind": "model", "models": []string{"claude-opus-5-5"}},
	), "require opus")
	report := f.evaluate(t, nil)
	if !report.Blocked || !strings.Contains(report.Rules[0].Problems[0], "no verifiable model attestation") {
		t.Fatalf("unattested commit: %+v", report)
	}

	router, runner, outsider := f.identity, newRunnerIdentity(t), newRunnerIdentity(t)
	forged, forgedGrant := modelGrantToken(t, outsider, runner, "claude-opus-5-5")
	f.attestFeature(t, forged, forgedGrant, runner)
	if report := f.evaluate(t, nil); !report.Blocked {
		t.Fatalf("a grant the router never signed was accepted: %+v", report)
	}

	f.featureOid = run(t, f.source, "git", "rev-parse", "feature~0")
	sonnet, sonnetGrant := modelGrantToken(t, router, runner, "claude-sonnet-5")
	f.attestFeature(t, sonnet, sonnetGrant, runner)
	if report := f.evaluate(t, nil); !report.Blocked || !strings.Contains(report.Rules[0].Problems[0], "claude-sonnet-5") {
		t.Fatalf("a disallowed model passed: %+v", report)
	}

	run(t, f.source, "git", "reset", "-q", "--hard", f.mainOid)
	run(t, f.source, "git", "checkout", "-q", "-B", "feature", run(t, f.source, "git", "rev-parse", "HEAD"))
	writeFile(t, filepath.Join(f.source, "feature.txt"), "feature\n")
	run(t, f.source, "git", "add", "-A")
	run(t, f.source, "git", "commit", "-q", "-m", "add feature")
	run(t, f.source, "git", "push", "-q", "-f", f.bare, "feature:refs/heads/feature")
	run(t, f.source, "git", "checkout", "-q", "main")
	f.featureOid = f.ref(t, "refs/heads/feature")
	opus, opusGrant := modelGrantToken(t, router, runner, "claude-opus-5-5")
	f.attestFeature(t, opus, opusGrant, runner)
	message := run(t, ".", "git", "--git-dir="+f.bare, "log", "-1", "--format=%B", f.featureOid)
	if !strings.Contains(message, "ForkMesh-Agent: claude-code/claude-opus-5-5") || !strings.Contains(message, agentAttestationTrailer) {
		t.Fatalf("attested message: %q", message)
	}
	if report := f.evaluate(t, nil); report.Blocked || !report.Passed {
		t.Fatalf("a genuine attestation was refused: %+v", report)
	}
	attested := f.featureOid

	f.commitOnMain(t, "other.txt", "base moved\n", "base work")
	run(t, f.source, "git", "checkout", "-q", "feature")
	run(t, f.source, "git", "merge", "-q", "--no-edit", "main")
	run(t, f.source, "git", "push", "-q", f.bare, "feature:refs/heads/feature")
	f.featureOid = run(t, f.source, "git", "rev-parse", "feature")
	run(t, f.source, "git", "checkout", "-q", "main")
	if report := f.evaluate(t, nil); report.Blocked {
		t.Fatalf("a clean update merge from the base was refused: %+v", report)
	}

	run(t, f.source, "git", "checkout", "-q", "feature")
	writeFile(t, filepath.Join(f.source, "feature.txt"), "edited by hand\n")
	run(t, f.source, "git", "commit", "-q", "-a", "--amend", "--no-edit")
	run(t, f.source, "git", "push", "-q", "-f", f.bare, "feature:refs/heads/feature")
	f.featureOid = run(t, f.source, "git", "rev-parse", "feature")
	run(t, f.source, "git", "checkout", "-q", "main")
	if report := f.evaluate(t, nil); !report.Blocked {
		t.Fatalf("an evil merge commit passed: %+v", report)
	}

	run(t, f.source, "git", "checkout", "-q", "feature")
	run(t, f.source, "git", "reset", "-q", "--hard", attested)
	writeFile(t, filepath.Join(f.source, "feature.txt"), "tampered\n")
	run(t, f.source, "git", "commit", "-q", "-a", "--amend", "--no-edit")
	run(t, f.source, "git", "push", "-q", "-f", f.bare, "feature:refs/heads/feature")
	f.featureOid = run(t, f.source, "git", "rev-parse", "feature")
	run(t, f.source, "git", "checkout", "-q", "main")
	tampered := run(t, ".", "git", "--git-dir="+f.bare, "log", "-1", "--format=%B", f.featureOid)
	if !strings.Contains(tampered, agentAttestationTrailer) {
		t.Fatal("the amend dropped the trailer; the test would prove nothing")
	}
	if report := f.evaluate(t, nil); !report.Blocked {
		t.Fatalf("an amended commit kept its attestation: %+v", report)
	}
	if result := f.request(t, "execute", "merge", map[string]any{"requestId": "merge_model_tampered"}); result["error"] != "rules_required" {
		t.Fatalf("tampered commit merged: %+v", result)
	}
}

func TestRunnerKeepsCommitsUnattestedWhenTheGrantDoesNotMatch(t *testing.T) {
	router, runner := newRunnerIdentity(t), newRunnerIdentity(t)
	token, _ := modelGrantToken(t, router, runner, "claude-opus-5-5")
	backend := &runnerBackend{identity: runner, node: "pool1"}
	work := agentWork{job: agentJob{ModelGrant: token, PushRef: "refs/heads/agent/fix-1"}}
	if _, ok := backend.modelGrantFor(work, &agentSession{Provider: "claude-code"}); !ok {
		t.Fatal("the default Claude model should match an Opus 5.5 grant")
	}
	for name, session := range map[string]*agentSession{
		"model":    {Provider: "claude-code", Model: "claude-sonnet-5"},
		"provider": {Provider: "codex", Model: "claude-opus-5-5"},
	} {
		if _, ok := backend.modelGrantFor(work, session); ok {
			t.Errorf("%s mismatch was attested", name)
		}
	}
	other := &runnerBackend{identity: newRunnerIdentity(t), node: "pool1"}
	if _, ok := other.modelGrantFor(work, &agentSession{Provider: "claude-code"}); ok {
		t.Fatal("a grant for another runner key was used")
	}
}

func TestAgentModelGrantMatchesTheRelayFormat(t *testing.T) {
	key := newRunnerIdentity(t).PublicKey()
	message := strings.Join([]string{"forkmesh-agent-model-grant-v1", "pool-1", key, "core-a", "proj",
		"refs/heads/agent/s1", "claude-code", "claude-opus-5-5", "lease-0001", "10", "20"}, "\n")
	grant, ok := parseAgentModelGrantMessage(message)
	if !ok || grant.Model != "claude-opus-5-5" || grant.IssuedAt != 10 || agentModelGrantMessage(grant) != message {
		t.Fatalf("relay grant: %+v %v", grant, ok)
	}
	for _, bad := range []string{
		strings.Replace(message, "refs/heads/agent/s1", "refs/heads/main", 1),
		strings.Replace(message, "\n10\n", "\n010\n", 1),
		strings.Replace(message, "claude-code", "gemini", 1),
		message + "\nextra",
	} {
		if _, ok := parseAgentModelGrantMessage(bad); ok {
			t.Errorf("accepted %q", bad)
		}
	}
}
