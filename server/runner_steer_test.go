package mirrornode

import (
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func installSteerRecordingClaude(t *testing.T) string {
	t.Helper()
	bin := t.TempDir()
	promptLog := filepath.Join(bin, "prompts.log")
	script := `#!/bin/sh
cat >> "` + promptLog + `"
echo "---" >> "` + promptLog + `"
echo "agent line" >> README.md
echo '{"type":"system","subtype":"init","session_id":"sess-1"}'
echo '{"type":"result","result":"Added a line.","session_id":"sess-1","is_error":false,"num_turns":1,"duration_ms":10,"total_cost_usd":0.01,"usage":{"input_tokens":1,"output_tokens":1}}'
`
	if err := os.WriteFile(filepath.Join(bin, "claude"), []byte(script), 0700); err != nil {
		t.Fatal(err)
	}
	t.Setenv("PATH", bin+string(os.PathListSeparator)+os.Getenv("PATH"))
	t.Setenv("HOME", t.TempDir())
	t.Setenv("CLAUDE_CONFIG_DIR", "")
	t.Setenv("ANTHROPIC_API_KEY", "test-key")
	return promptLog
}

func TestRunnerResumesAFollowUpItNeverRanOnTheSameBranch(t *testing.T) {
	promptLog := installSteerRecordingClaude(t)
	f := newRunnerFixture(t, nil)
	f.app.queue = append(f.app.queue, f.job(t, 7, "start", "refs/heads/agent/7-fix-login"))
	f.drain(t)
	_, reports := f.app.snapshot()
	if len(reports) != 2 || reports[1]["status"] != "completed" {
		t.Fatalf("start reports = %#v", reports)
	}
	info, _ := reports[1]["agentInfo"].(map[string]any)
	stats, _ := info["diffStats"].(map[string]any)
	if stats["files"] != float64(1) || stats["added"] != float64(1) || stats["ahead"] != float64(1) {
		t.Fatalf("diff stats = %#v", info["diffStats"])
	}
	first := gitValue(context.Background(), f.bare, "rev-parse", "refs/heads/agent/7-fix-login")

	f.daemon.agents.mu.Lock()
	f.daemon.agents.state.Sessions = map[string]*agentSession{}
	f.daemon.agents.mu.Unlock()

	f.app.queue = append(f.app.queue, f.job(t, 9, "steer", "refs/heads/agent/7-fix-login"))
	f.drain(t)
	_, reports = f.app.snapshot()
	if len(reports) != 4 || reports[3]["status"] != "completed" {
		t.Fatalf("follow-up reports = %#v", reports)
	}
	after := gitValue(context.Background(), f.bare, "rev-parse", "refs/heads/agent/7-fix-login")
	if after == first || !gitIsAncestor(context.Background(), f.bare, first, after) {
		t.Fatal("the follow-up did not build on the branch")
	}
	if readme := gitValue(context.Background(), f.bare, "show", "refs/heads/agent/7-fix-login:README.md"); strings.Count(readme, "agent line") != 2 {
		t.Fatalf("README after the follow-up = %q", readme)
	}
	raw, err := os.ReadFile(promptLog)
	if err != nil {
		t.Fatal(err)
	}
	prompts := strings.Split(string(raw), "---\n")
	if len(prompts) < 2 || !strings.Contains(prompts[1], "continuing earlier work on branch agent/7-fix-login") ||
		strings.Contains(prompts[0], "continuing earlier work") {
		t.Fatalf("prompts = %q", prompts)
	}
}

func TestParseNumstatCountsBinaryFilesWithoutLines(t *testing.T) {
	stats := parseNumstat("3\t1\ta.txt\n-\t-\timage.png\n\n10\t0\tb.go\n")
	if stats.Files != 3 || stats.Added != 13 || stats.Removed != 1 {
		t.Fatalf("stats = %+v", stats)
	}
}
