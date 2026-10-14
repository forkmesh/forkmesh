package mirrornode

// Web sign-in of a runner's provider CLI (docs/RUNNERS.md, "Web sign-in").
//
// A runner has no node event channel, so the relayed login terminal that a
// mirror serves over its socket (terminal.go) reaches a runner through its
// signed polling instead: the claim/poll response names pending login
// sessions (app/src/runner_logins.py), and for each one the runner runs the
// provider's own login command on a pty, as the job user inside the same
// sandbox kind a job gets, posting the screen to POST /api/runners/login/output
// in sequence-numbered chunks and writing whatever input the response carries
// back to the pty.
//
// The credential the login creates stays in the job user's HOME, where job
// runs read it. What leaves the runner is the terminal, and every line of it
// passes tokenScrubber first, so a token the CLI prints is replaced before it
// is sent. The session ends when the CLI exits, the app says it was cancelled
// or expired, or its TTL (at most runnerLoginMaxTTL) runs out; the process
// group is killed in every case but a clean exit.

import (
	"context"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"regexp"
	"strings"
	"sync"
	"time"
)

const (
	runnerPollPath        = "/api/runners/poll"
	runnerLoginOutputPath = "/api/runners/login/output"
	runnerLoginMaxTTL     = 10 * time.Minute
	runnerLoginChunk      = 8192
	runnerLoginMaxInput   = 4096
	runnerLoginTick       = time.Second
	runnerLoginKillGrace  = 3 * time.Second
	runnerLoginMaxActive  = 4 // a sign-in and a terminal per provider
	runnerTerminalMaxTTL  = 60 * time.Minute
	runnerTerminalCols    = 100
	runnerTerminalRows    = 40
	runnerInputGap        = 30 * time.Millisecond
	// Wide enough that the provider's authorize URL is not wrapped, so the
	// page can turn it into one link.
	runnerLoginCols = 400
	runnerLoginRows = 40
)

// runnerLogin is one session the app hands a runner.
type runnerLogin struct {
	ID       string `json:"id"`
	Provider string `json:"provider"`
	TTLMs    int64  `json:"ttlMs"`
	Kind     string `json:"kind"`
	Cols     int    `json:"cols"`
	Rows     int    `json:"rows"`
}

func (l runnerLogin) terminal() bool { return l.Kind == "terminal" }

func (l runnerLogin) noun() string {
	if l.terminal() {
		return "terminal"
	}
	return "sign-in"
}

type loginProcess interface {
	Read([]byte) (int, error)
	Write([]byte) (int, error)
	Close() error
	Wait() int
	Kill()
}

// runnerLoginCommand is the whole command surface of a web sign-in. Claude
// Code's `auth login` stores the OAuth credential in ~/.claude/.credentials.json
// (what agents.go's availability and every `claude -p` job read), unlike
// `setup-token`, which only prints a token. Codex uses the device-code flow:
// its default flow waits for a browser callback on the runner's own
// localhost, which the admin's browser cannot reach.
func runnerLoginCommand(provider string) (string, []string, bool) {
	switch provider {
	case "claude-code":
		return "claude", []string{"auth", "login"}, true
	case "codex":
		return "codex", []string{"login", "--device-auth"}, true
	default:
		return "", nil, false
	}
}

func runnerTerminalCommand(provider string) (string, []string, bool) {
	switch provider {
	case "claude-code":
		return "claude", nil, true
	case "codex":
		return "codex", nil, true
	default:
		return "", nil, false
	}
}

func runnerTerminalSize(cols, rows int) (int, int) {
	if cols <= 0 || rows <= 0 {
		return runnerTerminalCols, runnerTerminalRows
	}
	return min(max(cols, 20), 300), min(max(rows, 5), 120)
}

type runnerLoginManager struct {
	post           func(ctx context.Context, path string, body []byte) (int, []byte, error)
	start          func(login runnerLogin) (loginProcess, error)
	tick           time.Duration
	maxTTL         time.Duration
	maxTerminalTTL time.Duration
	onDone         func()
	logf           func(string, ...any)

	mu     sync.Mutex
	active map[string]bool
	wg     sync.WaitGroup
}

func newRunnerLoginManager(post func(context.Context, string, []byte) (int, []byte, error),
	start func(runnerLogin) (loginProcess, error)) *runnerLoginManager {
	return &runnerLoginManager{post: post, start: start, tick: runnerLoginTick,
		maxTTL: runnerLoginMaxTTL, maxTerminalTTL: runnerTerminalMaxTTL,
		onDone: func() {}, logf: func(string, ...any) {}, active: map[string]bool{}}
}

// ids names the sessions running here, sent with every claim/poll so the app
// re-offers only a session this runner lost.
func (m *runnerLoginManager) ids() []string {
	if m == nil {
		return []string{}
	}
	m.mu.Lock()
	defer m.mu.Unlock()
	ids := make([]string, 0, len(m.active))
	for id := range m.active {
		ids = append(ids, id)
	}
	return ids
}

func (m *runnerLoginManager) begin(ctx context.Context, logins []runnerLogin) {
	if m == nil {
		return
	}
	for _, login := range logins {
		if !validLoginID(login.ID) {
			continue
		}
		m.mu.Lock()
		if m.active[login.ID] || len(m.active) >= runnerLoginMaxActive {
			m.mu.Unlock()
			continue
		}
		m.active[login.ID] = true
		m.mu.Unlock()
		m.wg.Add(1)
		go func(login runnerLogin) {
			defer m.wg.Done()
			m.run(ctx, login)
			m.mu.Lock()
			delete(m.active, login.ID)
			m.mu.Unlock()
			m.onDone()
		}(login)
	}
}

func (m *runnerLoginManager) wait() {
	if m != nil {
		m.wg.Wait()
	}
}

func validLoginID(id string) bool {
	if len(id) != 32 {
		return false
	}
	for _, c := range id {
		if !(c >= '0' && c <= '9' || c >= 'a' && c <= 'f') {
			return false
		}
	}
	return true
}

type loginReply struct {
	State string `json:"state"`
	Cols  int    `json:"cols"`
	Rows  int    `json:"rows"`
	Input []struct {
		Seq  int64  `json:"seq"`
		Data string `json:"data"`
	} `json:"input"`
}

// loginRelay is one session's outbound half: the scrubbed bytes not yet
// accepted by the app, the next sequence number, and the input cursor.
type loginRelay struct {
	m          *runnerLoginManager
	id         string
	seq        int64
	unsent     []byte // chunk `seq` failed to post; retried as the same seq
	pending    []byte
	inputAfter int64
	cols, rows int
}

type resizableProcess interface {
	Resize(cols, rows int) error
}

func (l *loginRelay) send(ctx context.Context, fields map[string]any, process loginProcess) (loginReply, error) {
	body := map[string]any{"id": l.id, "seq": 0, "inputAfter": l.inputAfter}
	if l.unsent == nil && len(l.pending) > 0 {
		size := min(len(l.pending), runnerLoginChunk)
		l.unsent = append([]byte(nil), l.pending[:size]...)
		l.pending = l.pending[size:]
		l.seq++
	}
	if l.unsent != nil {
		body["seq"] = l.seq
		body["data"] = base64.StdEncoding.EncodeToString(l.unsent)
	}
	for key, value := range fields {
		body[key] = value
	}
	raw, _ := json.Marshal(body)
	status, reply, err := l.m.post(ctx, runnerLoginOutputPath, raw)
	if err != nil {
		return loginReply{}, err
	}
	if status < 200 || status >= 300 {
		if status == 404 || status == 409 {
			// The session is gone, or the app holds a different sequence:
			// nothing this runner sends will be accepted any more.
			return loginReply{State: "failed"}, nil
		}
		return loginReply{}, fmt.Errorf("login output returned HTTP %d", status)
	}
	l.unsent = nil
	var parsed loginReply
	if err := json.Unmarshal(reply, &parsed); err != nil {
		return loginReply{}, errors.New("login output returned invalid JSON")
	}
	if parsed.Cols > 0 && parsed.Rows > 0 && (parsed.Cols != l.cols || parsed.Rows != l.rows) {
		if sized, ok := process.(resizableProcess); ok {
			if sized.Resize(runnerTerminalSize(parsed.Cols, parsed.Rows)) == nil {
				l.cols, l.rows = parsed.Cols, parsed.Rows
			}
		}
	}
	wrote := false
	for _, input := range parsed.Input {
		if input.Seq <= l.inputAfter {
			continue
		}
		l.inputAfter = input.Seq
		decoded, err := base64.StdEncoding.DecodeString(input.Data)
		if err != nil || len(decoded) == 0 || len(decoded) > runnerLoginMaxInput || process == nil {
			continue
		}
		if wrote {
			time.Sleep(runnerInputGap)
		}
		_, _ = process.Write(decoded)
		wrote = true
	}
	return parsed, nil
}

func (m *runnerLoginManager) run(parent context.Context, login runnerLogin) {
	relay := &loginRelay{m: m, id: login.ID, cols: login.Cols, rows: login.Rows}
	limit := m.maxTTL
	if login.terminal() {
		limit = m.maxTerminalTTL
	}
	ttl := min(time.Duration(login.TTLMs)*time.Millisecond, limit)
	finish := func(state string, code any, message string) {
		fields := map[string]any{"state": state, "message": message}
		if code != nil {
			fields["exitCode"] = code
		}
		ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
		defer cancel()
		for attempt := 0; attempt < 5 && ctx.Err() == nil; attempt++ {
			if len(relay.pending) > 0 || relay.unsent != nil {
				if _, err := relay.send(ctx, nil, nil); err != nil {
					time.Sleep(time.Second)
				}
				continue
			}
			if _, err := relay.send(ctx, fields, nil); err == nil {
				return
			}
			time.Sleep(time.Second)
		}
	}
	if ttl <= 0 {
		finish("expired", nil, "The "+login.noun()+" expired before this runner picked it up.")
		return
	}
	process, err := m.start(login)
	if err != nil {
		m.logf("runner: web %s for %s could not start: %v", login.noun(), login.Provider, err)
		finish("failed", nil, boundedText([]byte(err.Error()), 200))
		return
	}
	m.logf("runner: web %s for %s started", login.noun(), login.Provider)
	ctx, cancel := context.WithTimeout(parent, ttl)
	defer cancel()

	reads := make(chan []byte, 64)
	go func() {
		defer close(reads)
		buffer := make([]byte, 4096)
		for {
			count, err := process.Read(buffer)
			if count > 0 {
				reads <- append([]byte(nil), buffer[:count]...)
			}
			if err != nil {
				return
			}
		}
	}()
	scrubber := &tokenScrubber{}
	ticker := time.NewTicker(m.tick)
	defer ticker.Stop()
	grew := false
	state, message := "", ""
	var code any
	killed := false
	kill := func() {
		if killed {
			return
		}
		killed = true
		_ = process.Close()
		go func() {
			time.Sleep(runnerLoginKillGrace)
			process.Kill()
		}()
	}
loop:
	for {
		select {
		case chunk, ok := <-reads:
			if !ok {
				relay.pending = append(relay.pending, scrubber.flush()...)
				exit := process.Wait()
				if state == "" {
					code = exit
					state = "failed"
					if exit == 0 {
						state = "succeeded"
					}
				}
				break loop
			}
			grew = true
			relay.pending = append(relay.pending, scrubber.write(chunk)...)
			if len(relay.pending) < runnerLoginChunk {
				continue
			}
		case <-ticker.C:
			if !grew {
				relay.pending = append(relay.pending, scrubber.flush()...)
			}
			grew = false
		case <-ctx.Done():
			if state == "" {
				state, message = "expired", "The "+login.noun()+" took longer than it is allowed to."
				if parent.Err() != nil {
					state, message = "failed", "The runner stopped."
				}
			}
			kill()
			// Keep reading until the pty closes, so the exit is collected.
			ctx = context.Background()
			continue
		}
		if killed {
			continue
		}
		reply, err := relay.send(ctx, nil, process)
		if err != nil {
			continue
		}
		if reply.State != "" && reply.State != "running" {
			if state == "" {
				state = reply.State
			}
			kill()
		}
	}
	if killed {
		code = nil
	}
	finish(state, code, message)
	m.logf("runner: web %s for %s ended: %s", login.noun(), login.Provider, state)
}

// --- token scrubbing ---------------------------------------------------------

var runnerTokenPattern = regexp.MustCompile(
	`sk-ant-[A-Za-z0-9_-]{6,}|sk-(?:proj-)?[A-Za-z0-9_-]{20,}|` +
		`eyJ[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{8,}`)

// tokenScrubber replaces anything shaped like a provider token before the
// runner relays it. A token can arrive split across pty reads, so a trailing
// run of token characters that could still grow into one is held back until
// the next read, or until a quiet tick (flush) shows the CLI is waiting.
type tokenScrubber struct {
	held []byte
}

func tokenByte(c byte) bool {
	return c >= 'a' && c <= 'z' || c >= 'A' && c <= 'Z' || c >= '0' && c <= '9' ||
		c == '-' || c == '_' || c == '.'
}

func (s *tokenScrubber) write(p []byte) []byte {
	data := append(s.held, p...)
	s.held = nil
	cut := len(data)
	for cut > 0 && tokenByte(data[cut-1]) {
		cut--
	}
	tail := string(data[cut:])
	if tail != "" && len(tail) < 8192 && (strings.Contains(tail, "sk") ||
		strings.Contains(tail, "eyJ") || strings.HasSuffix(tail, "s") ||
		strings.HasSuffix(tail, "e") || strings.HasSuffix(tail, "ey")) {
		s.held = append([]byte(nil), data[cut:]...)
		data = data[:cut]
	}
	return scrubTokens(data)
}

func (s *tokenScrubber) flush() []byte {
	data := s.held
	s.held = nil
	return scrubTokens(data)
}

func scrubTokens(data []byte) []byte {
	if len(data) == 0 {
		return nil
	}
	return runnerTokenPattern.ReplaceAll(append([]byte(nil), data...), []byte("[redacted]"))
}

// --- runner wiring -------------------------------------------------------------

// startLogin runs the provider's login command on a pty inside the job
// sandbox, as the job user and with the job user's HOME, so the credential
// lands where job runs read it. Provider API keys are removed from the
// environment: the sign-in must authenticate the account being entered.
func (b *runnerBackend) startLogin(login runnerLogin) (loginProcess, error) {
	provider := login.Provider
	name, args, ok := runnerLoginCommand(provider)
	cols, rows := runnerLoginCols, runnerLoginRows
	switch login.Kind {
	case "", "login":
	case "terminal":
		name, args, ok = runnerTerminalCommand(provider)
		cols, rows = runnerTerminalSize(login.Cols, login.Rows)
	default:
		return nil, errors.New("this runner does not open that kind of session")
	}
	if !ok {
		return nil, fmt.Errorf("that provider has no %s on this runner", login.noun())
	}
	if _, err := exec.LookPath(name); err != nil {
		return nil, fmt.Errorf("%s is not installed on this runner", name)
	}
	program, argv := b.box.argv(name, args)
	dir := b.box.home
	if dir == "" {
		dir = terminalWorkingDirectory()
	}
	if _, err := os.Stat(dir); err != nil {
		dir = "/"
	}
	env := terminalEnvironmentFrom(b.box.environment())
	signIn := !login.terminal()
	if signIn {
		b.box.accounts.beginLogin(provider)
	}
	b.box.begin()
	process, err := startPtyCommand(program, argv, env, dir, cols, rows, b.box.credential())
	if err != nil {
		b.box.end()
		if signIn {
			b.box.accounts.endLogin(provider, false)
		}
		return nil, err
	}
	return &sandboxLogin{loginProcess: process, box: b.box, provider: provider, signIn: signIn}, nil
}

type sandboxLogin struct {
	loginProcess
	box      jobSandbox
	provider string
	signIn   bool
	once     sync.Once
}

func (l *sandboxLogin) Wait() int {
	code := l.loginProcess.Wait()
	l.once.Do(func() {
		l.box.end()
		if l.signIn {
			l.box.accounts.endLogin(l.provider, code == 0)
		}
	})
	return code
}

func (l *sandboxLogin) Resize(cols, rows int) error {
	sized, ok := l.loginProcess.(resizableProcess)
	if !ok {
		return errors.New("this session cannot be resized")
	}
	return sized.Resize(cols, rows)
}

// availabilitySnapshot is what a claim/poll reports for the runners page.
func (r *agentJobRunner) availabilitySnapshot(accounts *runnerAccounts) map[string]any {
	accounts.sync()
	snapshot := map[string]any{}
	for _, provider := range []string{"claude-code", "codex"} {
		availability := r.availability(provider)
		snapshot[provider] = map[string]any{
			"binaryFound": availability["binaryFound"] == true,
			"loginState":  availability["loginState"],
			"source":      availability["credentialSource"],
			"accounts":    accounts.snapshot(provider),
			"auth":        r.authStatus(provider),
		}
	}
	return snapshot
}

// poll is claim's no-job twin: availability and login sessions only, for a
// runner with no free slot or no signed-in provider (which is exactly the
// runner an admin is trying to sign in).
func (b *runnerBackend) poll(ctx context.Context) (runnerInbox, error) {
	body, err := json.Marshal(b.statusFields())
	if err != nil {
		return runnerInbox{}, err
	}
	status, reply, err := b.signedPost(ctx, runnerPollPath, body)
	if err != nil {
		return runnerInbox{}, err
	}
	if status < 200 || status >= 300 {
		return runnerInbox{}, fmt.Errorf("runner poll returned HTTP %d: %s", status, boundedText(reply, 240))
	}
	var inbox runnerInbox
	if err := json.Unmarshal(reply, &inbox); err != nil {
		return runnerInbox{}, errors.New("runner poll returned invalid JSON")
	}
	return inbox, nil
}

func (b *runnerBackend) statusFields() map[string]any {
	return map[string]any{
		"availability":   b.r.availabilitySnapshot(b.box.accounts),
		"auth":           b.r.authSnapshot(),
		"activeLogins":   b.logins.ids(),
		"features":       []string{"terminal"},
		"accountResults": b.box.accounts.pendingResults(),
		"version":        b.version,
	}
}

// fastPollDelay is the app's requested poll interval while a sign-in is
// open, or 0.
func (b *runnerBackend) fastPollDelay() time.Duration {
	ms := b.fastPollMs.Load()
	if ms <= 0 {
		return 0
	}
	return time.Duration(max(ms, 250)) * time.Millisecond
}
