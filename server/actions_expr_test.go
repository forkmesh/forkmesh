package mirrornode

import (
	"strings"
	"testing"
)

func testExprContext() *exprContext {
	return &exprContext{EventName: "issues", Ref: "refs/heads/main", RefName: "main", SHA: "abc",
		Repository: "alice/proj", Actor: "bob",
		Event: map[string]any{"action": "labeled", "issue": map[string]any{
			"number": float64(12), "title": "Add `rm -rf /`; $(whoami)",
			"labels": []any{map[string]any{"name": "Hackathon"}, map[string]any{"name": "bug"}}}},
		Inputs:  map[string]string{"project": "bolt-c", "dry-run": "true"},
		Secrets: map[string]string{"TOKEN": "s3cret-value"},
		Env:     map[string]string{"MODE": "prod"}}
}

func TestExprConditions(t *testing.T) {
	ctx := testExprContext()
	for condition, want := range map[string]bool{
		"":                                   true,
		"github.event_name == 'issues'":      true,
		"${{ github.event_name == 'push' }}": false,
		"github.event_name != 'issues' || contains(github.event.issue.labels.*.name, 'hackathon')": true,
		"contains(github.event.issue.labels.*.name, 'feature')":                                    false,
		"github.ref == 'refs/heads/main' && !startsWith(github.ref_name, 'rel')":                   true,
		"endsWith(github.repository, '/PROJ')":                                                     true,
		"contains('Hello World', 'world')":                                                         true,
		"inputs.project == 'bolt-c' && inputs.dry-run == 'true'":                                 true,
		"inputs['project'] == 'bolt-c'":                                                          true,
		"github.event.issue.number == 12":                                                          true,
		"github.event.issue.number == '12'":                                                        true,
		"github.event.issue.labels[1].name == 'bug'":                                               true,
		"secrets.TOKEN != ''":                                                                      true,
		"secrets.MISSING":                                                                          false,
		"env.MODE == 'prod' && (1 == 2 || true)":                                                   true,
		"unknown.thing == ''":                                                                      true,
		"failure()":                                                                                false,
		"always()":                                                                                 true,
		"github.actor == 'BOB'":                                                                    true,
		"!(github.event.action == 'labeled')":                                                      false,
	} {
		got, err := evalCondition(condition, ctx)
		if err != nil || got != want {
			t.Fatalf("%q = %v (%v), want %v", condition, got, err, want)
		}
	}
	ctx.Failed = true
	for condition, want := range map[string]bool{"": false, "success()": false, "failure()": true,
		"always()": true, "github.event_name == 'issues'": false, "failure() && inputs.project != ''": true} {
		if got, err := evalCondition(condition, ctx); err != nil || got != want {
			t.Fatalf("after a failure %q = %v (%v)", condition, got, err)
		}
	}
	for _, bad := range []string{"github.ref ==", "nope(1)", "'open", "a @ b", "contains(1)", "(true"} {
		if _, err := evalCondition(bad, ctx); err == nil {
			t.Fatalf("%q accepted", bad)
		}
	}
}

func TestExprRunSubstitutionNeverInjectsShell(t *testing.T) {
	ctx := testExprContext()
	script := "echo ${{ github.event.issue.title }}\necho \"n=${{ github.event.issue.number }}\" '${{ inputs.project }}'\n" +
		"curl -H \"Authorization: ${{ secrets.TOKEN }}\" ${{ secrets.TOKEN }} ${{ secrets.OTHER }}\n"
	out, env, err := substituteRunExpr(script, ctx, func(name string) bool { return name == "TOKEN" })
	if err != nil {
		t.Fatal(err)
	}
	want := "echo \"$FM_EXPR_1\"\necho \"n=${FM_EXPR_2}\" ''\"$FM_EXPR_3\"''\n" +
		"curl -H \"Authorization: ${TOKEN}\" \"$TOKEN\" \"$FM_EXPR_4\"\n"
	if out != want {
		t.Fatalf("script\n%s\nwant\n%s", out, want)
	}
	if len(env) != 4 || env[0] != [2]string{"FM_EXPR_1", "Add `rm -rf /`; $(whoami)"} ||
		env[1][1] != "12" || env[2][1] != "bolt-c" || env[3][1] != "" {
		t.Fatalf("env %v", env)
	}
	if strings.Contains(out, "whoami") || strings.Contains(out, "s3cret") {
		t.Fatal("values must never reach the script text")
	}
	if _, _, err := substituteRunExpr("echo ${{ bad( }}", ctx, func(string) bool { return false }); err == nil {
		t.Fatal("a broken expression must fail the step")
	}
	if out, _, _ := substituteRunExpr("# it's fine\necho \"${{ env.MODE }}\"", ctx, func(string) bool { return false }); out != "# it's fine\necho \"${FM_EXPR_1}\"" {
		t.Fatalf("comment handling: %q", out)
	}
	value, err := interpolateExpr("mode-${{ env.MODE }}-${{ inputs.project }}", ctx)
	if err != nil || value != "mode-prod-bolt-c" {
		t.Fatalf("interpolate %q %v", value, err)
	}
}
