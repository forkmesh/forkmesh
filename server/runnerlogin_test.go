package mirrornode

import (
	"context"
	"encoding/base64"
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"testing"
	"time"
)

const testLoginID = "0123456789abcdef0123456789abcdef"

// installFakeLoginCLI puts a `claude` on PATH that behaves like
// `claude auth login`: it prints an authorize URL, reads the pasted code,
// stores a credential in $HOME/.claude and prints a token-looking line.
func installFakeLoginCLI(t *testing.T, body string) string {
	t.Helper()
	bin := t.TempDir()
	script := "#!/bin/sh\n" + body
	if err := os.WriteFile(filepath.Join(bin, "claude"), []byte(script), 0700); err != nil {
		t.Fatal(err)
	}
	home := t.TempDir()
	t.Setenv("PATH", bin+string(os.PathListSeparator)+os.Getenv("PATH"))
	t.Setenv("HOME", home)
	t.Setenv("CLAUDE_CONFIG_DIR", "")
	t.Setenv("ANTHROPIC_API_KEY", "")
	t.Setenv("ANTHROPIC_AUTH_TOKEN", "")
	t.Setenv("CLAUDE_CODE_OAUTH_TOKEN", "")
	return home
}

const fakeClaudeLogin = `case "$1 $2" in "auth login") ;; *) echo "unexpected: $*"; exit 3 ;; esac
echo "Browser didn't open? Use the url below to sign in:"
echo "https://claude.ai/oauth/authorize?code=true&client_id=abc&state=xyz"
printf "Paste code here if prompted > "
read code
mkdir -p "$HOME/.claude"
printf '{"claudeAiOauth":{"accessToken":"sk-ant-oat01-SECRETSECRETSECRET","refreshToken":"r"}}' > "$HOME/.claude/.credentials.json"  // forkmesh-secret-scan:ignore-line
echo "got:$code"
echo "Token sk-ant-oat01-SECRETSECRETSECRETXYZ saved"  // forkmesh-secret-scan:ignore-line
echo "Login successful."
`

// fakeLoginApp is the app's /api/runners/login/output: it checks sequence
// numbers, keeps the output, queues input, and records the final state.
type fakeLoginApp struct {
	mu        sync.Mutex
	output    []byte
	seq       int64
	input     []string
	delivered int64
	state     string // what the app answers; "" = running
	final     map[string]any
	done      chan struct{}
	onOutput  func(app *fakeLoginApp)
	cols      int
	rows      int
}

func newFakeLoginApp() *fakeLoginApp { return &fakeLoginApp{done: make(chan struct{})} }

func (a *fakeLoginApp) handle(fields map[string]any) (int, any) {
	a.mu.Lock()
	defer a.mu.Unlock()
	if fields["id"] != testLoginID {
		return 404, map[string]any{"ok": false}
	}
	seq := int64(fields["seq"].(float64))
	if seq > 0 {
		if seq > a.seq+1 {
			return 409, map[string]any{"ok": false, "error": "out_of_order"}
		}
		if seq == a.seq+1 {
			data, _ := base64.StdEncoding.DecodeString(fields["data"].(string))
			if len(data) > runnerLoginChunk {
				return 400, map[string]any{"ok": false}
			}
			a.output = append(a.output, data...)
			a.seq = seq
		}
	}
	if a.onOutput != nil {
		a.onOutput(a)
	}
	if state, _ := fields["state"].(string); state != "" && a.final == nil {
		a.final = fields
		close(a.done)
	}
	after := int64(fields["inputAfter"].(float64))
	var input []map[string]any
	for i, data := range a.input {
		if int64(i+1) > after {
			input = append(input, map[string]any{"seq": i + 1,
				"data": base64.StdEncoding.EncodeToString([]byte(data))})
		}
	}
	a.delivered = max(a.delivered, after)
	state := a.state
	if state == "" {
		state = "running"
	}
	reply := map[string]any{"ok": true, "state": state, "input": input}
	if a.cols > 0 {
		reply["cols"], reply["rows"] = a.cols, a.rows
	}
	return 200, reply
}

func (a *fakeLoginApp) text() string {
	a.mu.Lock()
	defer a.mu.Unlock()
	return string(a.output)
}

func (a *fakeLoginApp) waitFinal(t *testing.T, within time.Duration) map[string]any {
	t.Helper()
	select {
	case <-a.done:
	case <-time.After(within):
		t.Fatalf("no final state; output so far: %q", a.text())
	}
	a.mu.Lock()
	defer a.mu.Unlock()
	return a.final
}

// The whole path: poll hands the session over, the runner runs the login on
// a pty in the job sandbox, relays the scrubbed screen, writes the pasted
// code, reports success, and the next poll reports the provider available.
func TestRunnerWebSignInRoundTrip(t *testing.T) {
	f := newRunnerFixture(t, nil)
	home := installFakeLoginCLI(t, fakeClaudeLogin)
	backend := f.daemon.agents.backend.(*runnerBackend)
	backend.logins = newRunnerLoginManager(backend.signedPost, backend.startLogin)
	backend.logins.tick = 50 * time.Millisecond
	login := newFakeLoginApp()
	login.onOutput = func(app *fakeLoginApp) {
		if len(app.input) == 0 && strings.Contains(string(app.output), "Paste code here") {
			app.input = append(app.input, "the-code#123\r")
		}
	}
	f.app.mu.Lock()
	f.app.logins = []map[string]any{{"id": testLoginID, "provider": "claude-code", "ttlMs": 60000}}
	f.app.login = login.handle
	f.app.mu.Unlock()

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	if err := f.daemon.agents.runnerDrain(ctx); err != nil {
		t.Fatal(err)
	}
	final := login.waitFinal(t, 20*time.Second)
	backend.logins.wait()
	if final["state"] != "succeeded" || final["exitCode"] != float64(0) {
		t.Fatalf("final %#v; output %q", final, login.text())
	}
	output := login.text()
	for _, want := range []string{"https://claude.ai/oauth/authorize?code=true&client_id=abc&state=xyz",
		"got:the-code#123", "Token [redacted] saved", "Login successful."} {
		if !strings.Contains(output, want) {
			t.Fatalf("output lacks %q: %q", want, output)
		}
	}
	if strings.Contains(output, "SECRET") || strings.Contains(output, "sk-ant") {
		t.Fatalf("a token reached the app: %q", output)
	}
	if _, err := os.Stat(filepath.Join(home, ".claude", ".credentials.json")); err != nil {
		t.Fatalf("the credential was not stored in the job user's HOME: %v", err)
	}

	// No free provider before: the runner polled (no claim) and reported
	// Claude missing. After the sign-in it claims, reporting it available.
	claims, _ := f.app.snapshot()
	f.app.mu.Lock()
	polls := append([]map[string]any(nil), f.app.polls...)
	f.app.mu.Unlock()
	if len(polls) != 1 || len(claims) != 0 {
		t.Fatalf("polls %d claims %d", len(polls), len(claims))
	}
	claude := polls[0]["availability"].(map[string]any)["claude-code"].(map[string]any)
	if claude["binaryFound"] != true || claude["loginState"] != "missing" {
		t.Fatalf("availability before sign-in: %#v", claude)
	}
	if err := f.daemon.agents.runnerDrain(ctx); err != nil {
		t.Fatal(err)
	}
	claims, _ = f.app.snapshot()
	if len(claims) != 1 {
		t.Fatalf("no claim after sign-in: %d", len(claims))
	}
	claude = claims[0]["availability"].(map[string]any)["claude-code"].(map[string]any)
	if claude["loginState"] != "available" {
		t.Fatalf("availability after sign-in: %#v", claude)
	}
	if ids := claims[0]["activeLogins"].([]any); len(ids) != 0 {
		t.Fatalf("finished session still reported active: %v", ids)
	}
}

// startForTest runs the fake CLI the way the runner does (rlimit sandbox).
func loginStarter(t *testing.T) func(runnerLogin) (loginProcess, error) {
	backend := &runnerBackend{box: jobSandbox{memoryMax: "700M", cpuQuota: "90%"}}
	return backend.startLogin
}

func postTo(app *fakeLoginApp) func(context.Context, string, []byte) (int, []byte, error) {
	return func(_ context.Context, path string, body []byte) (int, []byte, error) {
		if path != runnerLoginOutputPath {
			return 404, nil, nil
		}
		var fields map[string]any
		if err := json.Unmarshal(body, &fields); err != nil {
			return 0, nil, err
		}
		status, reply := app.handle(fields)
		raw, _ := json.Marshal(reply)
		return status, raw, nil
	}
}

func processGone(pid int) bool {
	return errors.Is(syscall.Kill(pid, 0), syscall.ESRCH)
}

func waitPidFile(t *testing.T, path string) int {
	t.Helper()
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		if raw, err := os.ReadFile(path); err == nil {
			if pid, err := strconv.Atoi(strings.TrimSpace(string(raw))); err == nil && pid > 0 {
				return pid
			}
		}
		time.Sleep(20 * time.Millisecond)
	}
	t.Fatal("the fake CLI never started its helper")
	return 0
}

// A helper that ignores SIGHUP still dies with the process group when the
// TTL runs out, and the session is reported expired.
func TestRunnerWebSignInTTLKillsTheProcessGroup(t *testing.T) {
	pidFile := filepath.Join(t.TempDir(), "helper.pid")
	installFakeLoginCLI(t, `trap '' HUP
echo "waiting for a code"
sleep 600 &
echo $! > `+pidFile+`
wait
`)
	app := newFakeLoginApp()
	m := newRunnerLoginManager(postTo(app), loginStarter(t))
	m.tick, m.maxTTL = 50*time.Millisecond, 700*time.Millisecond
	m.begin(context.Background(), []runnerLogin{{ID: testLoginID, Provider: "claude-code", TTLMs: 600000}})
	helper := waitPidFile(t, pidFile)
	final := app.waitFinal(t, 15*time.Second)
	m.wait()
	if final["state"] != "expired" {
		t.Fatalf("final %#v", final)
	}
	deadline := time.Now().Add(3 * time.Second)
	for !processGone(helper) && time.Now().Before(deadline) {
		time.Sleep(20 * time.Millisecond)
	}
	if !processGone(helper) {
		_ = syscall.Kill(helper, syscall.SIGKILL)
		t.Fatal("the login's helper process outlived the session")
	}
	if len(m.ids()) != 0 {
		t.Fatalf("session still active: %v", m.ids())
	}
}

// Cancel in the browser reaches the runner as the app's state; the runner
// kills the CLI and keeps the app's word for how it ended.
func TestRunnerWebSignInCancelFromTheApp(t *testing.T) {
	pidFile := filepath.Join(t.TempDir(), "cli.pid")
	installFakeLoginCLI(t, `echo $$ > `+pidFile+`
printf "Paste code here if prompted > "
read code
echo "should not get here"
`)
	app := newFakeLoginApp()
	app.onOutput = func(a *fakeLoginApp) {
		if strings.Contains(string(a.output), "Paste code") {
			a.state = "cancelled"
		}
	}
	m := newRunnerLoginManager(postTo(app), loginStarter(t))
	m.tick = 50 * time.Millisecond
	m.begin(context.Background(), []runnerLogin{{ID: testLoginID, Provider: "claude-code", TTLMs: 60000}})
	cli := waitPidFile(t, pidFile)
	final := app.waitFinal(t, 15*time.Second)
	m.wait()
	if final["state"] != "cancelled" {
		t.Fatalf("final %#v", final)
	}
	if _, ok := final["exitCode"]; ok {
		t.Fatalf("a killed login reported an exit code: %#v", final)
	}
	if !processGone(cli) {
		t.Fatal("the CLI survived the cancel")
	}
	if strings.Contains(app.text(), "should not get here") {
		t.Fatal("the CLI kept running after the cancel")
	}
}

// A missing CLI or unknown provider fails the session at once.
func TestRunnerWebSignInFailsWithoutTheCLI(t *testing.T) {
	t.Setenv("PATH", t.TempDir())
	app := newFakeLoginApp()
	m := newRunnerLoginManager(postTo(app), loginStarter(t))
	m.begin(context.Background(), []runnerLogin{{ID: testLoginID, Provider: "claude-code", TTLMs: 60000},
		{ID: "not-an-id", Provider: "codex", TTLMs: 60000}})
	final := app.waitFinal(t, 5*time.Second)
	m.wait()
	if final["state"] != "failed" || !strings.Contains(final["message"].(string), "not installed") {
		t.Fatalf("final %#v", final)
	}
	if name, args, _ := runnerLoginCommand("codex"); name != "codex" || strings.Join(args, " ") != "login --device-auth" {
		t.Fatalf("codex login command %s %v", name, args)
	}
	if _, _, ok := runnerLoginCommand("sh"); ok {
		t.Fatal("an unknown provider has a login command")
	}
}

// A terminal session runs the provider's CLI with no arguments, in the size
// the app asked for, follows a later size, and passes control keys through.
func TestRunnerTerminalRunsTheCLIAndFollowsTheSize(t *testing.T) {
	home := installFakeLoginCLI(t, `if [ $# -ne 0 ]; then echo "unexpected: $*"; exit 3; fi
echo "size:$(stty size)"
printf "ready > "
read line
echo "size:$(stty size)"
echo "got:$line"
`)
	app := newFakeLoginApp()
	app.onOutput = func(a *fakeLoginApp) {
		if len(a.input) == 0 && strings.Contains(string(a.output), "ready >") {
			a.cols, a.rows = 90, 30
			a.input = append(a.input, "\x1b[Ahello\r")
		}
	}
	m := newRunnerLoginManager(postTo(app), loginStarter(t))
	m.tick = 50 * time.Millisecond
	m.begin(context.Background(), []runnerLogin{{ID: testLoginID, Provider: "claude-code", TTLMs: 60000,
		Kind: "terminal", Cols: 70, Rows: 20}})
	final := app.waitFinal(t, 15*time.Second)
	m.wait()
	if final["state"] != "succeeded" {
		t.Fatalf("final %#v; output %q", final, app.text())
	}
	output := app.text()
	for _, want := range []string{"size:20 70", "size:30 90", "hello"} {
		if !strings.Contains(output, want) {
			t.Fatalf("output lacks %q: %q", want, output)
		}
	}
	if _, err := os.Stat(filepath.Join(home, ".claude", ".credentials.json")); err == nil {
		t.Fatal("a terminal session wrote a credential")
	}
	if name, args, _ := runnerTerminalCommand("codex"); name != "codex" || len(args) != 0 {
		t.Fatalf("codex terminal command %s %v", name, args)
	}
	if _, _, ok := runnerTerminalCommand("sh"); ok {
		t.Fatal("an unknown provider has a terminal command")
	}
	if cols, rows := runnerTerminalSize(5000, 1); cols != 300 || rows != 5 {
		t.Fatalf("size not clamped: %d x %d", cols, rows)
	}
}

// A session kind this runner does not know fails instead of running a sign-in.
func TestRunnerLoginRefusesAnUnknownKind(t *testing.T) {
	installFakeLoginCLI(t, fakeClaudeLogin)
	app := newFakeLoginApp()
	m := newRunnerLoginManager(postTo(app), loginStarter(t))
	m.begin(context.Background(), []runnerLogin{{ID: testLoginID, Provider: "claude-code", TTLMs: 60000,
		Kind: "shell"}})
	final := app.waitFinal(t, 5*time.Second)
	m.wait()
	if final["state"] != "failed" || app.text() != "" {
		t.Fatalf("final %#v; output %q", final, app.text())
	}
}

type recordingProcess struct {
	writes [][]byte
}

func (p *recordingProcess) Read([]byte) (int, error) { return 0, errors.New("unused") }
func (p *recordingProcess) Write(b []byte) (int, error) {
	p.writes = append(p.writes, append([]byte(nil), b...))
	return len(b), nil
}
func (p *recordingProcess) Close() error { return nil }
func (p *recordingProcess) Wait() int    { return 0 }
func (p *recordingProcess) Kill()        {}

// Input is written once, in order, and never beyond runnerLoginMaxInput;
// output goes in chunks of at most runnerLoginChunk with increasing seq.
func TestRunnerLoginRelayInputBoundsAndChunking(t *testing.T) {
	app := newFakeLoginApp()
	app.input = []string{strings.Repeat("x", runnerLoginMaxInput+1), "code\r", "again\n"}
	m := newRunnerLoginManager(postTo(app), nil)
	relay := &loginRelay{m: m, id: testLoginID}
	relay.pending = []byte(strings.Repeat("a", runnerLoginChunk+10))
	process := &recordingProcess{}
	for i := 0; i < 3; i++ {
		if _, err := relay.send(context.Background(), nil, process); err != nil {
			t.Fatal(err)
		}
	}
	if len(process.writes) != 2 || string(process.writes[0]) != "code\r" || string(process.writes[1]) != "again\n" {
		t.Fatalf("writes %q", process.writes)
	}
	if relay.inputAfter != 3 || relay.seq != 2 || len(app.text()) != runnerLoginChunk+10 {
		t.Fatalf("inputAfter %d seq %d output %d", relay.inputAfter, relay.seq, len(app.text()))
	}
}

func TestTokenScrubberHandlesSplitTokens(t *testing.T) {
	s := &tokenScrubber{}
	got := string(s.write([]byte("Your token: sk-an")))
	got += string(s.write([]byte("t-oat01-ABCDEFGHIJKL")))
	got += string(s.write([]byte("MNOP\r\nPaste code here")))
	got += string(s.flush())
	if got != "Your token: [redacted]\r\nPaste code here" {
		t.Fatalf("scrubbed %q", got)
	}
	jwt := "eyJhbGciOiJSUzI1NiJ9.eyJzdWIiOiIxMjM0NTY3ODkwIn0.c2lnbmF0dXJlLXZhbHVl"
	if out := string(s.write([]byte("id " + jwt + " ok\n"))); strings.Contains(out, "eyJ") {
		t.Fatalf("jwt leaked: %q", out)
	}
	if out := string(s.write([]byte("key sk-proj-abcdefghijklmnopqrstuvwxyz0123 end\n"))); strings.Contains(out, "abcdefghij") {  // forkmesh-secret-scan:ignore-line
		t.Fatalf("openai key leaked: %q", out)
	}
	url := "https://claude.ai/oauth/authorize?code=true&client_id=9d1c250a-e61b&state=AbCdEf\n"
	if out := string(s.write([]byte(url))); out != url {
		t.Fatalf("url changed: %q", out)
	}
}
