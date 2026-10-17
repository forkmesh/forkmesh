package mirrornode

import (
	"strings"
	"testing"
)

func TestRunnerGrantActionsPushTakesAnyBranch(t *testing.T) {
	g := runnerGrant{Node: "core1", Owner: "alice", Repository: "proj", Access: "actions-push",
		Ref: "refs/heads/main", Runner: "runner1", GrantID: "grant-abcdefgh",
		IssuedAt: 1000, ExpiresAt: 2000}
	want := "forkmesh-runner-grant-v1\ncore1\nalice\nproj\nactions-push\nrefs/heads/main\nrunner1\ngrant-abcdefgh\n1000\n2000"
	if got := runnerGrantMessage(g); got != want {
		t.Fatalf("message = %q", got)
	}
	parsed, ok := parseRunnerGrantMessage(want)
	if !ok || parsed != g {
		t.Fatalf("parsed %+v %v", parsed, ok)
	}
	if !validateCapabilityPayload([]byte(want), "core1") {
		t.Fatal("the gateway verifier refuses an actions-push grant")
	}
	for _, ref := range []string{"refs/heads/release/2026.10", "refs/heads/agent/x", "refs/heads/a" + strings.Repeat("b", 199)} {
		g.Ref = ref
		if runnerGrantMessage(g) == "" {
			t.Fatalf("%s refused", ref)
		}
	}
	for _, ref := range []string{"", "main", "refs/heads/", "refs/tags/v1", "refs/heads/a..b", "refs/heads/a//b",
		"refs/heads/a@{1}", "refs/heads/a/", "refs/heads/a.lock", "refs/heads/a.", "refs/heads/-x",
		"refs/heads/.x", "refs/heads/a b", "refs/heads/a" + strings.Repeat("b", 200)} {
		g.Ref = ref
		if runnerGrantMessage(g) != "" {
			t.Fatalf("%q accepted", ref)
		}
	}
	g.Access = "push"
	g.Ref = "refs/heads/main"
	if runnerGrantMessage(g) != "" {
		t.Fatal("a plain push grant must stay under agent/")
	}
	g.Access, g.Ref = "actions-push", "refs/heads/main"
	g.ExpiresAt = g.IssuedAt + runnerGrantMaxLifetimeMs + 1
	if runnerGrantMessage(g) != "" {
		t.Fatal("lifetime cap must apply")
	}
}
