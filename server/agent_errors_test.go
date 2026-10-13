package mirrornode

import (
	"errors"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// The stream mirror35 printed on 2026-09-24 for a claude-opus-5-5 run on a
// frozen Claude Code 2.1.248: init, the API's refusal as an assistant
// message, then the result, and exit status 1 with nothing on stderr.
const outdatedClaudeStream = `{"type":"system","subtype":"init","cwd":"/var/lib/forkmesh/tmp/agent-59fe","session_id":"59fefbda-30d7-501e-fa82-6ba38a000001","tools":["Task","Bash","Edit","Read"],"mcp_servers":[],"model":"claude-opus-5-5","permissionMode":"bypassPermissions","slash_commands":[],"apiKeySource":"none","claude_code_version":"2.1.248","output_style":"default","agents":[],"skills":[],"plugins":[],"uuid":"a1"}
{"type":"assistant","message":{"id":"b2","container":null,"model":"<synthetic>","role":"assistant","stop_reason":"stop_sequence","stop_sequence":"","type":"message","usage":{"input_tokens":0,"output_tokens":0},"content":[{"type":"text","text":"API Error: 400 Claude Code 2.1.248 does not support this model; version 2.1.280 or newer is required. Run 'claude update'"}]},"parent_tool_use_id":null,"session_id":"59fefbda-30d7-501e-fa82-6ba38a000001","uuid":"b3","isApiErrorMessage":true,"apiErrorStatus":400,"error":"invalid_request"}
{"type":"result","subtype":"success","is_error":true,"duration_ms":1843,"duration_api_ms":0,"num_turns":1,"result":"API Error: 400 Claude Code 2.1.248 does not support this model; version 2.1.280 or newer is required. Run 'claude update'","stop_reason":"stop_sequence","session_id":"59fefbda-30d7-501e-fa82-6ba38a000001","total_cost_usd":0,"usage":{"input_tokens":0,"output_tokens":0},"permission_denials":[],"uuid":"c4"}
`

const outdatedClaudeMessage = "Claude Code on mirror35 is 2.1.248; claude-opus-5-5 needs 2.1.280 or newer. Update the host (Hosts → Update)."

func exitStatus(t *testing.T, code string) error {
	t.Helper()
	err := exec.Command("sh", "-c", "exit "+code).Run()
	if err == nil {
		t.Fatal("sh exit did not fail")
	}
	return err
}

func feedClaudeStream(text string) *claudeStream {
	stream := &claudeStream{}
	sink := &lineSink{fn: stream.line}
	_, _ = sink.Write([]byte(text))
	sink.flush()
	return stream
}

func TestClaudeRunErrorNamesTheOutdatedCLINotTheInitEvent(t *testing.T) {
	stream := feedClaudeStream(outdatedClaudeStream)
	err := claudeRunError(stream, nil, exitStatus(t, "1"), "mirror35", "claude-opus-5-5")
	if err == nil || err.Error() != outdatedClaudeMessage {
		t.Fatalf("error = %v", err)
	}
	var outdated *agentCLIOutdated
	if !errors.As(err, &outdated) || outdated.current != "2.1.248" || outdated.required != "2.1.280" {
		t.Fatalf("outdated = %#v", outdated)
	}
	// Without the result event the assistant's API error still says it.
	lines := strings.SplitN(outdatedClaudeStream, "\n", 3)
	partial := feedClaudeStream(lines[0] + "\n" + lines[1] + "\n")
	if err := claudeRunError(partial, nil, exitStatus(t, "1"), "mirror35", "claude-opus-5-5"); err == nil ||
		err.Error() != outdatedClaudeMessage {
		t.Fatalf("assistant-only error = %v", err)
	}
}

func TestClaudeRunErrorOrder(t *testing.T) {
	init := `{"type":"system","subtype":"init","session_id":"s","model":"claude-opus-5-5","claude_code_version":"2.1.281"}` + "\n"
	cases := []struct {
		name, stream, stderr, want string
	}{
		{"result text", init +
			`{"type":"assistant","message":{"content":[{"type":"text","text":"API Error: 529 Overloaded"}]},"isApiErrorMessage":true}` + "\n" +
			`{"type":"result","subtype":"success","is_error":true,"result":"API Error: 529 Overloaded (request req_1)"}` + "\n",
			"", "Claude Code exited with status 1: API Error: 529 Overloaded (request req_1)"},
		{"result errors list", init +
			`{"type":"result","subtype":"error_during_execution","is_error":true,"errors":["Tool permission stream closed"]}` + "\n",
			"", "Claude Code exited with status 1: Tool permission stream closed"},
		{"assistant API error text", init +
			`{"type":"assistant","message":{"content":[{"type":"text","text":"API Error: 401 authentication_error"}]}}` + "\n",
			"", "Claude Code exited with status 1: API Error: 401 authentication_error"},
		{"stderr last lines", init, "Loading...\n\nError: invalid session\nhint: sign in\n\n",
			"Claude Code exited with status 1: Loading... | Error: invalid session | hint: sign in"},
		{"bare exit", init, "", "Claude Code exited with status 1"},
	}
	for _, c := range cases {
		err := claudeRunError(feedClaudeStream(c.stream), []byte(c.stderr), exitStatus(t, "1"), "mirror35", "claude-opus-5-5")
		if err == nil || err.Error() != c.want {
			t.Errorf("%s: error = %v, want %q", c.name, err, c.want)
		}
		if err != nil && strings.Contains(err.Error(), "init") {
			t.Errorf("%s: quoted the init event: %v", c.name, err)
		}
	}
	// An error is capped.
	long := init + `{"type":"result","is_error":true,"result":"` + strings.Repeat("x", 5000) + `"}` + "\n"
	if err := claudeRunError(feedClaudeStream(long), nil, exitStatus(t, "1"), "m", ""); len([]rune(err.Error())) > agentErrorLimit {
		t.Fatalf("error not capped: %d runes", len([]rune(err.Error())))
	}
	// Exit 0 with an error result is still a failure; a clean run is not.
	if err := claudeRunError(feedClaudeStream(init+`{"type":"result","is_error":true,"result":"Prompt is too long"}`+"\n"),
		nil, nil, "m", ""); err == nil || err.Error() != "Claude Code failed: Prompt is too long" {
		t.Fatalf("is_error with exit 0 = %v", err)
	}
	if err := claudeRunError(feedClaudeStream(init+`{"type":"result","is_error":false,"result":"done"}`+"\n"),
		nil, nil, "m", ""); err != nil {
		t.Fatalf("clean run = %v", err)
	}
	if err := claudeRunError(feedClaudeStream(init), nil, nil, "m", ""); err == nil ||
		err.Error() != "Claude Code returned no result" {
		t.Fatalf("no result = %v", err)
	}
}

func TestCodexRunErrorUsesStderrTailAndNamesAnOutdatedCLI(t *testing.T) {
	err := codexRunError([]byte("banner\n"), []byte("OpenAI Codex v0.40.0\n--------\nERROR: stream disconnected before completion\n"),
		exitStatus(t, "2"), "mirror35", "gpt-5.6-sol", nil)
	if err == nil || err.Error() != "Codex exited with status 2: OpenAI Codex v0.40.0 | -------- | ERROR: stream disconnected before completion" {
		t.Fatalf("codex error = %v", err)
	}
	err = codexRunError(nil, []byte("ERROR: The 'gpt-5.6-sol' model requires a newer version of Codex. Please upgrade to the latest app or CLI and try again.\n"),
		exitStatus(t, "1"), "mirror35", "gpt-5.6-sol", func() string { return "0.40.0" })
	if err == nil || err.Error() != "Codex on mirror35 is 0.40.0; gpt-5.6-sol needs a newer version. Update the host (Hosts → Update)." {
		t.Fatalf("outdated codex = %v", err)
	}
	if err := codexRunError(nil, nil, exitStatus(t, "3"), "m", "", nil); err == nil || err.Error() != "Codex exited with status 3" {
		t.Fatalf("bare codex = %v", err)
	}
}

func TestVersionLess(t *testing.T) {
	for _, c := range []struct {
		a, b string
		less bool
	}{{"2.1.248", "2.1.280", true}, {"2.1.281", "2.1.280", false}, {"2.1.280", "2.1.280", false},
		{"2.1", "2.1.1", true}, {"10.0.0", "9.9.9", false}} {
		if versionLess(c.a, c.b) != c.less {
			t.Errorf("versionLess(%s, %s) != %v", c.a, c.b, c.less)
		}
	}
}

// installOutdatedClaude is a `claude` whose runs the API refuses for being
// older than the model needs, exactly as mirror35's 2.1.248 was. Writing a
// version to the returned file "updates" it.
func installOutdatedClaude(t *testing.T) (calls, version string) {
	t.Helper()
	bin := t.TempDir()
	calls = filepath.Join(bin, "calls.log")
	version = filepath.Join(bin, "version")
	if err := os.WriteFile(version, []byte("2.1.248"), 0600); err != nil {
		t.Fatal(err)
	}
	stream := filepath.Join(bin, "stream.jsonl")
	if err := os.WriteFile(stream, []byte(outdatedClaudeStream), 0600); err != nil {
		t.Fatal(err)
	}
	script := `#!/bin/sh
echo "$*" >> "` + calls + `"
if [ "$1" = "--version" ]; then echo "$(cat "` + version + `") (Claude Code)"; exit 0; fi
cat >/dev/null
if [ "$(cat "` + version + `")" = "2.1.248" ]; then cat "` + stream + `"; exit 1; fi
echo '{"type":"system","subtype":"init","session_id":"sess-2"}'
echo '{"type":"result","result":"Done on the new CLI.","session_id":"sess-2","is_error":false,"num_turns":1}'
`
	if err := os.WriteFile(filepath.Join(bin, "claude"), []byte(script), 0700); err != nil {
		t.Fatal(err)
	}
	t.Setenv("PATH", bin+string(os.PathListSeparator)+os.Getenv("PATH"))
	t.Setenv("HOME", t.TempDir())
	t.Setenv("CLAUDE_CONFIG_DIR", "")
	t.Setenv("ANTHROPIC_API_KEY", "test-key")
	return calls, version
}

func TestOutdatedClaudeRunReportsTheFixOnceAndFailsFastUntilUpdated(t *testing.T) {
	calls, version := installOutdatedClaude(t)
	daemon, relay, _ := newAgentTestDaemon(t, "")
	want := strings.Replace(outdatedClaudeMessage, "mirror35", daemon.gateway.Node.Name, 1)

	job := agentTestJob(61, "start")
	job["model"], job["createPr"] = "claude-opus-5-5", false
	relay.enqueue(job)
	drainAgents(t, daemon)
	reports := relay.snapshot()
	if len(reports) != 2 || reports[1]["status"] != "failed" {
		t.Fatalf("reports = %#v", reports)
	}
	done := reports[1]
	info, _ := done["agentInfo"].(map[string]any)
	if info["lastError"] != want {
		t.Fatalf("lastError = %q", info["lastError"])
	}
	// The error is in the transcript once - the API's own words - not again
	// as a trailer the clients would show a second time.
	transcript, _ := done["transcript"].(string)
	if strings.Contains(transcript, "subtype") || strings.Count(transcript, "does not support this model") != 1 ||
		strings.Contains(transcript, "Hosts → Update") {
		t.Fatalf("transcript = %q", transcript)
	}

	// The same CLI is not run again for that model: the next job fails at
	// once with the same message, having asked only for the version.
	before, _ := os.ReadFile(calls)
	job = agentTestJob(62, "start")
	job["sessionId"], job["model"], job["createPr"] = "session-b", "claude-opus-5-5", false
	relay.enqueue(job)
	daemon.agents.request()
	drainAgents(t, daemon)
	reports = relay.snapshot()
	last := reports[len(reports)-1]
	if last["status"] != "failed" || last["securityReason"] != want || last["result"] != "" {
		t.Fatalf("fast-fail report = %#v", last)
	}
	after, _ := os.ReadFile(calls)
	if added := strings.TrimPrefix(string(after), string(before)); added != "--version\n" {
		t.Fatalf("CLI calls after the refusal = %q", added)
	}

	// Once the host is updated the model runs again.
	if err := os.WriteFile(version, []byte("2.1.281"), 0600); err != nil {
		t.Fatal(err)
	}
	job = agentTestJob(63, "start")
	job["sessionId"], job["model"], job["createPr"] = "session-c", "claude-opus-5-5", false
	relay.enqueue(job)
	daemon.agents.request()
	drainAgents(t, daemon)
	reports = relay.snapshot()
	if last := reports[len(reports)-1]; last["status"] != "completed" || last["result"] != "Done on the new CLI." {
		t.Fatalf("after update = %#v", last)
	}
}
