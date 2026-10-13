package mirrornode

import (
	"context"
	"crypto/ed25519"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"
)

// fakeAgentRelay serves the Worker's org-agent-jobs claim and result routes.
type fakeAgentRelay struct {
	t        *testing.T
	identity *Identity
	mu       sync.Mutex
	queue    []map[string]any
	reports  []map[string]any
	streams  []map[string]any
	live     []map[string]any
	watched  bool
	// Images served on org-agent-jobs/<id>/attachments/<attachmentId>, and
	// the lease each download presented.
	attachments      map[string][]byte
	attachmentLeases []string
}

func (f *fakeAgentRelay) ServeHTTP(w http.ResponseWriter, request *http.Request) {
	query := request.URL.Query()
	signature, _ := base64.RawURLEncoding.DecodeString(query.Get("sig"))
	public, _ := base64.RawURLEncoding.DecodeString(f.identity.PublicKey())
	canonical := "forkmesh-issues-pull-v1\n" + query.Get("owner") + "\n" + query.Get("ts")
	if query.Get("owner") != "mirror9" ||
		!ed25519.Verify(ed25519.PublicKey(public), []byte(canonical), signature) {
		http.Error(w, `{"error":"unauthorized"}`, http.StatusUnauthorized)
		return
	}
	f.mu.Lock()
	defer f.mu.Unlock()
	switch {
	case request.Method == http.MethodGet && request.URL.Path == "/api/repo/mirror9/proj/org-agent-jobs":
		jobs := []map[string]any{}
		if len(f.queue) > 0 {
			jobs, f.queue = append(jobs, f.queue[0]), f.queue[1:]
		}
		_ = json.NewEncoder(w).Encode(map[string]any{"ok": true, "jobs": jobs})
	case request.Method == http.MethodGet && strings.HasPrefix(request.URL.Path, "/api/repo/mirror9/proj/org-agent-jobs/") &&
		strings.Contains(request.URL.Path, "/attachments/"):
		f.attachmentLeases = append(f.attachmentLeases, query.Get("leaseId"))
		id := request.URL.Path[strings.LastIndex(request.URL.Path, "/")+1:]
		data, ok := f.attachments[id]
		if !ok {
			http.Error(w, `{"ok":false,"error":"attachment_unavailable"}`, http.StatusGone)
			return
		}
		_, _ = w.Write(data)
	case request.Method == http.MethodPost && strings.HasPrefix(request.URL.Path, "/api/repo/mirror9/proj/org-agent-jobs/") &&
		strings.HasSuffix(request.URL.Path, "/stream"):
		var body map[string]any
		_ = json.NewDecoder(request.Body).Decode(&body)
		body["path"] = request.URL.Path
		f.streams = append(f.streams, body)
		_, _ = w.Write([]byte(`{"ok":true,"watched":false}`))
	case request.Method == http.MethodPost && strings.HasPrefix(request.URL.Path, "/api/repo/mirror9/proj/org-agent-jobs/"):
		var body map[string]any
		if err := json.NewDecoder(request.Body).Decode(&body); err != nil {
			f.t.Errorf("result body: %v", err)
		}
		body["path"] = request.URL.Path
		if body["live"] == true {
			body["after"] = len(f.reports)
			f.live = append(f.live, body)
			_ = json.NewEncoder(w).Encode(map[string]any{"ok": true, "live": true, "watched": f.watched})
			return
		}
		f.reports = append(f.reports, body)
		_, _ = w.Write([]byte(`{"ok":true}`))
	default:
		http.NotFound(w, request)
	}
}

func (f *fakeAgentRelay) enqueue(job map[string]any) {
	f.mu.Lock()
	f.queue = append(f.queue, job)
	f.mu.Unlock()
}

func (f *fakeAgentRelay) snapshot() []map[string]any {
	f.mu.Lock()
	defer f.mu.Unlock()
	return append([]map[string]any(nil), f.reports...)
}

// installFakeClaude puts a stand-in `claude` first on PATH. A run appends to
// README.md and reports a JSON result. Every invocation's arguments are logged.
func installFakeClaude(t *testing.T) string {
	t.Helper()
	bin := t.TempDir()
	logPath := filepath.Join(bin, "calls.log")
	script := `#!/bin/sh
cat >/dev/null
echo "$*" >> "` + logPath + `"
case " $* " in
  *) echo "agent line" >> README.md
     echo '{"type":"system","subtype":"init","session_id":"sess-1"}'
     echo '{"type":"assistant","message":{"role":"assistant","content":[{"type":"text","text":"Appending a line to the README."},{"type":"tool_use","name":"Bash","input":{"command":"echo agent line >> README.md","description":"Append"}}]}}'
     echo '{"type":"user","message":{"role":"user","content":[{"type":"tool_result","content":""}]}}'
     echo '{"type":"assistant","message":{"role":"assistant","content":[{"type":"text","text":"Added a line."}]}}'
     echo '{"type":"result","result":"Added a line.","session_id":"sess-1","is_error":false,"num_turns":2,"duration_ms":1200,"total_cost_usd":0.25,"usage":{"input_tokens":10,"output_tokens":5}}' ;;
esac
`
	if err := os.WriteFile(filepath.Join(bin, "claude"), []byte(script), 0700); err != nil {
		t.Fatal(err)
	}
	t.Setenv("PATH", bin+string(os.PathListSeparator)+os.Getenv("PATH"))
	t.Setenv("HOME", t.TempDir())
	t.Setenv("CLAUDE_CONFIG_DIR", "")
	t.Setenv("ANTHROPIC_API_KEY", "test-key")
	return logPath
}

func newAgentTestDaemon(t *testing.T, agentOwner string) (*Daemon, *fakeAgentRelay, string) {
	t.Helper()
	_, bare := makeBareRepo(t)
	identity, _ := testIdentity(t)
	relay := &fakeAgentRelay{t: t, identity: identity}
	server := httptest.NewServer(relay)
	t.Cleanup(server.Close)
	var gateway GatewayConfig
	gateway.Node.Name = "mirror9"
	gateway.Node.PublicKey = identity.PublicKey()
	gateway.Repositories = []Repository{{Owner: "alice", Name: "proj",
		Visibility: "public", Enabled: true, GitDir: bare}}
	daemon := &Daemon{
		config: Config{
			GatewayConfig: filepath.Join(t.TempDir(), "mirror-gateway", "config.json"),
			CatalogURL:    server.URL + "/api/repositories",
			PublishOwner:  "alice",
			Version:       "test",
			AgentOwner:    agentOwner,
		},
		gateway:      gateway,
		identity:     identity,
		syncRequests: make(chan struct{}, 1),
	}
	daemon.agents = newAgentJobRunner(daemon)
	return daemon, relay, bare
}

func agentTestJob(id int64, kind string) map[string]any {
	return map[string]any{
		"jobId": id, "leaseId": "lease-" + kind, "kind": kind,
		"sessionId": "session-a", "organization": "jett",
		"repositoryOwner": "mirror9", "repositoryName": "proj",
		"provider": "claude-code", "prompt": "Add a line to the readme",
		"model": "", "issueNumber": 0, "createPr": true, "requestedBy": "jett",
	}
}

func drainAgents(t *testing.T, daemon *Daemon) {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	if err := daemon.agents.drain(ctx, daemon.gateway.Repositories); err != nil {
		t.Fatal(err)
	}
	daemon.agents.wait()
}

func TestMirrorNodeClaimsRunsPublishesAndReportsWebsiteAgentJob(t *testing.T) {
	calls := installFakeClaude(t)
	daemon, relay, bare := newAgentTestDaemon(t, "")
	relay.enqueue(agentTestJob(41, "start"))
	drainAgents(t, daemon)

	reports := relay.snapshot()
	if len(reports) != 2 {
		t.Fatalf("reports = %#v, want running then completed", reports)
	}
	running, done := reports[0], reports[1]
	if running["status"] != "running" || running["securityVerdict"] != "approved" ||
		running["leaseId"] != "lease-start" || running["localAgentId"] != float64(41) ||
		running["path"] != "/api/repo/mirror9/proj/org-agent-jobs/41/result" {
		t.Fatalf("running report = %#v", running)
	}
	if done["status"] != "completed" || done["result"] != "Added a line." {
		t.Fatalf("completion report = %#v", done)
	}
	relay.mu.Lock()
	streams := append([]map[string]any(nil), relay.streams...)
	relay.mu.Unlock()
	if len(streams) == 0 || streams[0]["path"] != "/api/repo/mirror9/proj/org-agent-jobs/41/stream" ||
		streams[0]["leaseId"] != "lease-start" || streams[0]["seq"] != float64(0) || streams[0]["data"] != nil {
		t.Fatalf("stream posts = %#v, want an unwatched heartbeat on the lease", streams)
	}
	// The run is narrated, not just concluded: the website's session page
	// shows what the agent said and which tools it ran.
	if transcript, _ := done["transcript"].(string); transcript !=
		"Appending a line to the README.\n\n\u25b8 Bash: echo agent line >> README.md\n\nAdded a line." {
		t.Fatalf("completion transcript = %q", transcript)
	}
	info, _ := done["agentInfo"].(map[string]any)
	branch, _ := info["branchName"].(string)
	if !strings.HasPrefix(branch, "agent/web-41-add-a-line-to-the-readme") ||
		info["prNumber"] != float64(1) || info["mode"] != "headless" ||
		info["numTurns"] != float64(2) {
		t.Fatalf("agentInfo = %#v", info)
	}
	if readme := gitValue(context.Background(), bare, "show", "refs/heads/"+branch+":README.md"); !strings.Contains(readme, "agent line") {
		t.Fatalf("published branch README = %q", readme)
	}
	if main := gitValue(context.Background(), bare, "show", "refs/heads/main:README.md"); strings.Contains(main, "agent line") {
		t.Fatal("the agent wrote to main instead of its branch")
	}
	pull := gitValue(context.Background(), bare, "show", "refs/heads/forkmesh/pulls:.forkmesh/pulls/1/pull.md")
	if !strings.Contains(pull, "\nhead: "+branch+"\n") || !strings.Contains(pull, "\nbase: main\n") {
		t.Fatalf("pull ledger = %q", pull)
	}

	// A follow-up prompt resumes the same conversation on the same branch.
	steer := agentTestJob(42, "steer")
	delete(steer, "createPr")
	relay.enqueue(steer)
	daemon.agents.request()
	drainAgents(t, daemon)
	reports = relay.snapshot()
	if len(reports) != 4 || reports[3]["status"] != "completed" ||
		reports[3]["path"] != "/api/repo/mirror9/proj/org-agent-jobs/42/result" {
		t.Fatalf("follow-up reports = %#v", reports)
	}
	log, _ := os.ReadFile(calls)
	if !strings.Contains(string(log), "--output-format stream-json --verbose") {
		t.Fatalf("the run was not streamed: %s", log)
	}
	if !strings.Contains(string(log), "--resume sess-1") {
		t.Fatalf("follow-up did not resume the conversation: %s", log)
	}
	readme := gitValue(context.Background(), bare, "show", "refs/heads/"+branch+":README.md")
	if strings.Count(readme, "agent line") != 2 {
		t.Fatalf("follow-up did not build on the branch: %q", readme)
	}
}

func TestMirrorNodeRejectsUnsafeAgentJobWithoutRunningIt(t *testing.T) {
	calls := installFakeClaude(t)
	daemon, relay, _ := newAgentTestDaemon(t, "")
	job := agentTestJob(7, "start")
	job["provider"] = "bash"
	relay.enqueue(job)
	drainAgents(t, daemon)
	reports := relay.snapshot()
	if len(reports) != 1 || reports[0]["status"] != "rejected" ||
		reports[0]["securityVerdict"] != "rejected" {
		t.Fatalf("reports = %#v", reports)
	}
	if _, err := os.Stat(calls); err == nil {
		t.Fatal("an unsafe envelope reached the provider CLI")
	}
}

// Jobs run without a model safety preflight whatever security envelope the
// relay attaches: none, the retired Haiku gate, or an owner waiver.
func TestAgentJobsRunWithoutAHaikuPreflight(t *testing.T) {
	for i, check := range []map[string]any{
		nil,
		{"provider": "claude-code", "model": "haiku", "tools": false, "failClosed": true},
		{"required": false, "waiver": "repository_owner", "waivedFor": "jett", "tools": false, "failClosed": true},
		{"required": false},
	} {
		calls := installFakeClaude(t)
		daemon, relay, _ := newAgentTestDaemon(t, "")
		job := agentTestJob(int64(20+i), "start")
		job["createPr"] = false
		job["requestedBy"] = "someone-else"
		if check != nil {
			job["securityCheck"] = check
		}
		relay.enqueue(job)
		drainAgents(t, daemon)
		reports := relay.snapshot()
		if len(reports) != 2 || reports[0]["securityVerdict"] != "approved" ||
			reports[1]["status"] != "completed" {
			t.Fatalf("envelope %d reports = %#v", i, reports)
		}
		log, _ := os.ReadFile(calls)
		if strings.Contains(string(log), "--model haiku") {
			t.Fatalf("envelope %d ran a Haiku preflight: %s", i, log)
		}
	}
}

func TestAgentJobInterruptedByRestartIsReportedFailed(t *testing.T) {
	installFakeClaude(t)
	daemon, relay, _ := newAgentTestDaemon(t, "")
	state := agentJobState{
		Sessions: map[string]*agentSession{"session-a": {
			SessionID: "session-a", LocalAgentID: 5, RepositoryName: "proj",
			Provider: "claude-code", Status: "running",
		}},
		Jobs: map[string]*agentJobRecord{"5": {
			JobID: 5, LeaseID: "lease-5", RepositoryName: "proj", SessionID: "session-a",
			Provider: "claude-code", Verdict: "approved", Status: "running",
		}},
	}
	if err := os.MkdirAll(daemon.agents.dir, 0700); err != nil {
		t.Fatal(err)
	}
	if err := writePrivateJSON(daemon.agents.statePath(), state); err != nil {
		t.Fatal(err)
	}
	drainAgents(t, daemon)
	reports := relay.snapshot()
	if len(reports) != 1 || reports[0]["status"] != "failed" ||
		reports[0]["leaseId"] != "lease-5" ||
		!strings.Contains(reports[0]["securityReason"].(string), "restarted") {
		t.Fatalf("reports = %#v", reports)
	}
	// Accepted once; the next safety read does not repeat it.
	drainAgents(t, daemon)
	if len(relay.snapshot()) != 1 {
		t.Fatal("an accepted terminal result was reported again")
	}
}

func TestOrgAgentsEventRequestsAClaim(t *testing.T) {
	daemon := &Daemon{syncRequests: make(chan struct{}, 1)}
	daemon.agents = &agentJobRunner{daemon: daemon}
	if !daemon.agents.due(time.Now()) {
		t.Fatal("a fresh runner should claim once")
	}
	if daemon.agents.due(time.Now()) {
		t.Fatal("no event: the safety interval has not elapsed")
	}
	daemon.handleNodeEvent(Event{Topic: "org-agents", Repo: "mirror9/proj"})
	if !daemon.agents.due(time.Now()) {
		t.Fatal("an org-agents push must trigger a claim")
	}
	select {
	case <-daemon.syncRequests:
	default:
		t.Fatal("an org-agents push must wake the sync loop")
	}
}

func TestCatalogOmitsAgentProvidersWhenAgentJobsAreDisabled(t *testing.T) {
	publisher := &CatalogPublisher{
		LookPath:      func(string) (string, error) { return "/usr/local/bin/claude", nil },
		DisableAgents: true,
	}
	if providers := publisher.agentProviders(); len(providers) != 0 {
		t.Fatalf("providers = %v", providers)
	}
}

// A desktop's "Move to cloud" is queued as soon as it published the branch;
// the branch can reach this node after the claim. The run waits for it and
// then continues on that branch instead of failing the move.
func TestMirrorNodeContinuesAHandoffWhoseBranchArrivesAfterTheClaim(t *testing.T) {
	installFakeClaude(t)
	wait, poll := agentHandoffBranchWait, agentHandoffBranchPoll
	agentHandoffBranchWait, agentHandoffBranchPoll = 10*time.Second, 20*time.Millisecond
	t.Cleanup(func() { agentHandoffBranchWait, agentHandoffBranchPoll = wait, poll })
	daemon, relay, bare := newAgentTestDaemon(t, "")
	job := agentTestJob(51, "continue")
	job["branch"] = "agent/adhoc-7-moved"
	job["baseBranch"] = "main"
	job["createPr"] = false
	relay.enqueue(job)
	go func() {
		time.Sleep(150 * time.Millisecond)
		if _, err := agentGit(context.Background(), "", "--git-dir="+bare,
			"branch", "agent/adhoc-7-moved", "main"); err != nil {
			t.Errorf("create branch: %v", err)
		}
	}()
	drainAgents(t, daemon)

	reports := relay.snapshot()
	if len(reports) != 2 || reports[1]["status"] != "completed" {
		t.Fatalf("reports = %#v, want running then completed", reports)
	}
	info, _ := reports[1]["agentInfo"].(map[string]any)
	if info["branchName"] != "agent/adhoc-7-moved" {
		t.Fatalf("agentInfo = %#v, want the moved branch", info)
	}
	readme := gitValue(context.Background(), bare, "show", "refs/heads/agent/adhoc-7-moved:README.md")
	if !strings.Contains(readme, "agent line") {
		t.Fatalf("the continuation did not build on the moved branch: %q", readme)
	}
}

func TestMirrorNodeFailsAHandoffWhoseBranchNeverArrives(t *testing.T) {
	installFakeClaude(t)
	wait, poll := agentHandoffBranchWait, agentHandoffBranchPoll
	agentHandoffBranchWait, agentHandoffBranchPoll = 100*time.Millisecond, 20*time.Millisecond
	t.Cleanup(func() { agentHandoffBranchWait, agentHandoffBranchPoll = wait, poll })
	daemon, relay, _ := newAgentTestDaemon(t, "")
	job := agentTestJob(52, "continue")
	job["branch"] = "agent/adhoc-8-lost"
	relay.enqueue(job)
	drainAgents(t, daemon)
	reports := relay.snapshot()
	if len(reports) != 1 || reports[0]["status"] != "failed" ||
		!strings.Contains(fmt.Sprint(reports[0]["securityReason"]), "has not reached this node") {
		t.Fatalf("reports = %#v, want one failed report naming the missing branch", reports)
	}
}

// A "stop" job the owner queued from another device cancels the session's run
// on this node and reports at once; it never reaches the provider CLI itself.
func TestAgentStopJobCancelsTheSessionsRunWithoutStartingAnything(t *testing.T) {
	calls := installFakeClaude(t)
	daemon, relay, _ := newAgentTestDaemon(t, "")
	cancelled := false
	daemon.agents.mu.Lock()
	daemon.agents.stops[agentTestJob(1, "start")["sessionId"].(string)] = func() { cancelled = true }
	daemon.agents.mu.Unlock()
	stop := agentTestJob(51, "stop")
	stop["localAgentId"] = 3
	relay.enqueue(stop)
	drainAgents(t, daemon)
	if !cancelled {
		t.Fatal("the running provider was not cancelled")
	}
	reports := relay.snapshot()
	if len(reports) != 1 || reports[0]["status"] != "failed" ||
		reports[0]["securityVerdict"] != "approved" ||
		reports[0]["path"] != "/api/repo/mirror9/proj/org-agent-jobs/51/result" {
		t.Fatalf("reports = %#v", reports)
	}
	if _, err := os.Stat(calls); err == nil {
		t.Fatal("a stop job reached the provider CLI")
	}

	// With nothing running it still answers, so the website hears back.
	relay.enqueue(agentTestJob(52, "stop"))
	daemon.agents.request()
	drainAgents(t, daemon)
	reports = relay.snapshot()
	if len(reports) != 2 || reports[1]["status"] != "failed" {
		t.Fatalf("idle stop reports = %#v", reports)
	}
}
