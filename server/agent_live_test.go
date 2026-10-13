package mirrornode

import (
	"context"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"
)

func installSlowFakeClaude(t *testing.T) {
	t.Helper()
	bin := t.TempDir()
	script := `#!/bin/sh
cat >/dev/null
echo "agent line" >> README.md
echo '{"type":"system","subtype":"init","session_id":"sess-1"}'
echo '{"type":"assistant","message":{"role":"assistant","content":[{"type":"text","text":"Reading the README first."}]}}'
sleep 0.4
echo '{"type":"assistant","message":{"role":"assistant","content":[{"type":"tool_use","name":"Bash","input":{"command":"echo agent line >> README.md"}}]}}'
sleep 0.4
echo '{"type":"assistant","message":{"role":"assistant","content":[{"type":"text","text":"Added a line."}]}}'
echo '{"type":"result","result":"Added a line.","session_id":"sess-1","is_error":false,"num_turns":2,"duration_ms":900,"total_cost_usd":0.1,"usage":{"input_tokens":10,"output_tokens":5}}'
`
	if err := os.WriteFile(filepath.Join(bin, "claude"), []byte(script), 0700); err != nil {
		t.Fatal(err)
	}
	t.Setenv("PATH", bin+string(os.PathListSeparator)+os.Getenv("PATH"))
	t.Setenv("HOME", t.TempDir())
	t.Setenv("CLAUDE_CONFIG_DIR", "")
	t.Setenv("ANTHROPIC_API_KEY", "test-key")
}

func TestMirrorNodeStreamsTheTranscriptWhileTheAgentRuns(t *testing.T) {
	previous := agentLiveReportInterval
	agentLiveReportInterval = 50 * time.Millisecond
	t.Cleanup(func() { agentLiveReportInterval = previous })
	installSlowFakeClaude(t)
	daemon, relay, _ := newAgentTestDaemon(t, "")
	relay.enqueue(agentTestJob(61, "start"))
	drainAgents(t, daemon)

	reports := relay.snapshot()
	if len(reports) != 2 || reports[0]["status"] != "running" || reports[1]["status"] != "completed" {
		t.Fatalf("reports = %#v", reports)
	}
	relay.mu.Lock()
	live := append([]map[string]any(nil), relay.live...)
	relay.mu.Unlock()
	if len(live) < 2 {
		t.Fatalf("live reports = %#v, want one per step", live)
	}
	for _, report := range live {
		if report["status"] != "running" || report["securityVerdict"] != "approved" ||
			report["leaseId"] != "lease-start" || report["after"] != 1 ||
			report["path"] != "/api/repo/mirror9/proj/org-agent-jobs/61/result" {
			t.Fatalf("live report = %#v", report)
		}
	}
	first, _ := live[0]["transcript"].(string)
	if first != "Reading the README first." {
		t.Fatalf("first live transcript = %q", first)
	}
	last, _ := live[len(live)-1]["transcript"].(string)
	if !strings.Contains(last, "▸ Bash: echo agent line >> README.md") {
		t.Fatalf("last live transcript = %q", last)
	}
}

func TestAgentLiveReporterSendsOnlyTheNewestTextAtTheInterval(t *testing.T) {
	previous := agentLiveReportInterval
	agentLiveReportInterval = 200 * time.Millisecond
	t.Cleanup(func() { agentLiveReportInterval = previous })
	var mu sync.Mutex
	var sent []string
	reporter := newAgentLiveReporter(context.Background(), func(_ context.Context, text string) bool {
		mu.Lock()
		sent = append(sent, text)
		mu.Unlock()
		return false
	})
	reporter.update("one")
	time.Sleep(50 * time.Millisecond)
	reporter.update("two")
	reporter.update("three")
	time.Sleep(300 * time.Millisecond)
	reporter.update("three")
	time.Sleep(250 * time.Millisecond)
	reporter.stop()
	mu.Lock()
	defer mu.Unlock()
	if strings.Join(sent, ",") != "one,three" {
		t.Fatalf("sent = %q", sent)
	}
}

func TestAgentLiveReporterSpeedsUpWhileSomeoneWatches(t *testing.T) {
	previous, previousWatched := agentLiveReportInterval, agentLiveWatchedInterval
	agentLiveReportInterval, agentLiveWatchedInterval = 2*time.Second, 20*time.Millisecond
	t.Cleanup(func() { agentLiveReportInterval, agentLiveWatchedInterval = previous, previousWatched })
	var mu sync.Mutex
	var sent []string
	reporter := newAgentLiveReporter(context.Background(), func(_ context.Context, text string) bool {
		mu.Lock()
		sent = append(sent, text)
		mu.Unlock()
		return text != "three"
	})
	reporter.update("one")
	time.Sleep(60 * time.Millisecond)
	reporter.update("two")
	time.Sleep(60 * time.Millisecond)
	reporter.update("three")
	time.Sleep(60 * time.Millisecond)
	reporter.update("four")
	time.Sleep(200 * time.Millisecond)
	reporter.stop()
	mu.Lock()
	defer mu.Unlock()
	if strings.Join(sent, ",") != "one,two,three" {
		t.Fatalf("sent = %q", sent)
	}
}

func TestMirrorNodeLiveReportsFollowTheWatchedAnswer(t *testing.T) {
	previous, previousWatched := agentLiveReportInterval, agentLiveWatchedInterval
	agentLiveReportInterval, agentLiveWatchedInterval = time.Hour, 10*time.Millisecond
	t.Cleanup(func() { agentLiveReportInterval, agentLiveWatchedInterval = previous, previousWatched })
	installSlowFakeClaude(t)
	daemon, relay, _ := newAgentTestDaemon(t, "")
	relay.mu.Lock()
	relay.watched = true
	relay.mu.Unlock()
	relay.enqueue(agentTestJob(62, "start"))
	drainAgents(t, daemon)
	relay.mu.Lock()
	live := len(relay.live)
	relay.mu.Unlock()
	if live < 2 {
		t.Fatalf("live reports = %d, want the watched interval to allow one per step", live)
	}
}
