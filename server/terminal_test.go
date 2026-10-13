package mirrornode

import (
	"encoding/base64"
	"io"
	"os/exec"
	"strings"
	"sync"
	"testing"
	"time"
)

// fakeTerminalProcess stands in for the pty so the relay's policy can be
// exercised without a provider CLI on the machine running the tests.
type fakeTerminalProcess struct {
	mu       sync.Mutex
	output   chan []byte
	written  []byte
	cols     int
	rows     int
	closed   bool
	exitCode int
}

func newFakeTerminalProcess() *fakeTerminalProcess {
	return &fakeTerminalProcess{output: make(chan []byte, 8)}
}

func (f *fakeTerminalProcess) Read(buffer []byte) (int, error) {
	chunk, ok := <-f.output
	if !ok {
		return 0, io.EOF
	}
	return copy(buffer, chunk), nil
}

func (f *fakeTerminalProcess) Write(buffer []byte) (int, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.written = append(f.written, buffer...)
	return len(buffer), nil
}

func (f *fakeTerminalProcess) Resize(cols, rows int) error {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.cols, f.rows = cols, rows
	return nil
}

func (f *fakeTerminalProcess) Close() error {
	f.mu.Lock()
	defer f.mu.Unlock()
	if !f.closed {
		f.closed = true
		close(f.output)
	}
	return nil
}

func (f *fakeTerminalProcess) Wait() int { return f.exitCode }

func (f *fakeTerminalProcess) wrote() string {
	f.mu.Lock()
	defer f.mu.Unlock()
	return string(f.written)
}

type recordedFrames struct {
	mu     sync.Mutex
	frames []map[string]any
}

func (r *recordedFrames) send(frame map[string]any) bool {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.frames = append(r.frames, frame)
	return true
}

func (r *recordedFrames) actions() []string {
	r.mu.Lock()
	defer r.mu.Unlock()
	actions := make([]string, 0, len(r.frames))
	for _, frame := range r.frames {
		actions = append(actions, frame["action"].(string))
	}
	return actions
}

func (r *recordedFrames) last() map[string]any {
	r.mu.Lock()
	defer r.mu.Unlock()
	if len(r.frames) == 0 {
		return nil
	}
	return r.frames[len(r.frames)-1]
}

func (r *recordedFrames) waitFor(t *testing.T, action string) map[string]any {
	t.Helper()
	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		r.mu.Lock()
		for _, frame := range r.frames {
			if frame["action"] == action {
				r.mu.Unlock()
				return frame
			}
		}
		r.mu.Unlock()
		time.Sleep(5 * time.Millisecond)
	}
	t.Fatalf("no %q frame arrived; got %v", action, r.actions())
	return nil
}

func newTestRelay(node string, disabled bool) (*TerminalRelay, *recordedFrames, *fakeTerminalProcess) {
	sent := &recordedFrames{}
	process := newFakeTerminalProcess()
	relay := NewTerminalRelay(node, disabled, sent.send)
	relay.lookPath = func(string) (string, error) { return "/usr/bin/claude", nil }
	relay.start = func(string, []string, int, int) (terminalProcess, error) {
		return process, nil
	}
	return relay, sent, process
}

func openFrame() TerminalFrame {
	return TerminalFrame{
		Action: "open", Session: "s1", Node: "mirror35",
		Provider: "claude-code", Cols: 100, Rows: 30,
	}
}

func TestTerminalRelayStreamsOneProviderLogin(t *testing.T) {
	relay, sent, process := newTestRelay("mirror35", false)
	relay.Handle(openFrame())
	sent.waitFor(t, "ready")

	process.output <- []byte("Paste the code: ")
	output := sent.waitFor(t, "output")
	decoded, err := base64.StdEncoding.DecodeString(output["data"].(string))
	if err != nil || string(decoded) != "Paste the code: " {
		t.Fatalf("unexpected output frame %v (%v)", output, err)
	}

	relay.Handle(TerminalFrame{
		Action: "input", Session: "s1", Node: "mirror35",
		Data: base64.StdEncoding.EncodeToString([]byte("code\r")),
	})
	if got := process.wrote(); got != "code\r" {
		t.Fatalf("input was not written to the pty: %q", got)
	}

	relay.Handle(TerminalFrame{
		Action: "resize", Session: "s1", Node: "mirror35", Cols: 120, Rows: 40,
	})
	process.mu.Lock()
	cols, rows := process.cols, process.rows
	process.mu.Unlock()
	if cols != 120 || rows != 40 {
		t.Fatalf("resize did not reach the pty: %dx%d", cols, rows)
	}

	relay.Handle(TerminalFrame{Action: "close", Session: "s1", Node: "mirror35"})
	sent.waitFor(t, "exit")
	relay.mu.Lock()
	defer relay.mu.Unlock()
	if relay.session != nil {
		t.Fatal("a closed session must not stay current")
	}
}

func TestTerminalRelayIgnoresFramesForAnotherNode(t *testing.T) {
	relay, sent, _ := newTestRelay("mirror35", false)
	frame := openFrame()
	frame.Node = "laptop"
	relay.Handle(frame)
	// Silence, not a refusal: the relay broadcasts to every node socket on the
	// account, so answering would mean every node replying to every frame.
	if got := sent.actions(); len(got) != 0 {
		t.Fatalf("expected no frames for another node's session, got %v", got)
	}
}

func TestTerminalRelayRefusesAnythingButAProviderLogin(t *testing.T) {
	relay, sent, _ := newTestRelay("mirror35", false)
	frame := openFrame()
	frame.Provider = "bash"
	relay.Handle(frame)
	if got := sent.last()["action"]; got != "error" {
		t.Fatalf("a non-provider open must be refused, got %v", got)
	}

	relay, sent, _ = newTestRelay("mirror35", false)
	relay.lookPath = func(string) (string, error) { return "", exec.ErrNotFound }
	relay.Handle(openFrame())
	if got := sent.last()["action"]; got != "error" {
		t.Fatalf("a missing CLI must be reported, got %v", got)
	}

	relay, sent, _ = newTestRelay("mirror35", true)
	relay.Handle(openFrame())
	if got := sent.last()["action"]; got != "error" {
		t.Fatalf("disableAgentLogin must refuse the session, got %v", got)
	}
}

func TestTerminalRelayReattachesAndRepaintsOneSession(t *testing.T) {
	relay, sent, process := newTestRelay("mirror35", false)
	relay.Handle(openFrame())
	sent.waitFor(t, "ready")
	process.output <- []byte("half-finished sign-in")
	sent.waitFor(t, "output")

	// The browser reconnected and re-announced the same session: the pty is
	// still running, so it must be repainted rather than restarted.
	started := 0
	relay.start = func(string, []string, int, int) (terminalProcess, error) {
		started++
		return newFakeTerminalProcess(), nil
	}
	relay.Handle(openFrame())
	if started != 0 {
		t.Fatal("re-announcing a live session restarted the sign-in")
	}
	deadline := time.Now().Add(time.Second)
	for time.Now().Before(deadline) {
		if len(sent.actions()) >= 4 {
			break
		}
		time.Sleep(5 * time.Millisecond)
	}
	replay := sent.last()
	decoded, err := base64.StdEncoding.DecodeString(replay["data"].(string))
	if replay["action"] != "output" || err != nil ||
		string(decoded) != "half-finished sign-in" {
		t.Fatalf("reconnect was not repainted: %v (%v)", replay, err)
	}
}

func TestTerminalRelayStartsANewSessionOverAnOldOne(t *testing.T) {
	relay, sent, first := newTestRelay("mirror35", false)
	relay.Handle(openFrame())
	sent.waitFor(t, "ready")

	second := newFakeTerminalProcess()
	relay.start = func(string, []string, int, int) (terminalProcess, error) {
		return second, nil
	}
	frame := openFrame()
	frame.Session = "s2"
	relay.Handle(frame)
	sent.waitFor(t, "exit") // the first session's pump reported it ended

	first.mu.Lock()
	closed := first.closed
	first.mu.Unlock()
	if !closed {
		t.Fatal("the previous session's pty was left running")
	}
	relay.mu.Lock()
	defer relay.mu.Unlock()
	if relay.session == nil || relay.session.id != "s2" {
		t.Fatalf("the new session did not take over: %v", relay.session)
	}
}

func TestTerminalEnvironmentDropsProviderApiKeys(t *testing.T) {
	t.Setenv("ANTHROPIC_API_KEY", "sk-should-not-be-inherited")
	t.Setenv("TERM", "dumb")
	var sawKey, sawTerm bool
	for _, entry := range terminalEnvironment() {
		if entry == "ANTHROPIC_API_KEY=sk-should-not-be-inherited" {
			sawKey = true
		}
		if entry == "TERM=xterm-256color" {
			sawTerm = true
		}
		if entry == "TERM=dumb" {
			t.Fatal("the daemon's own TERM leaked into the sign-in")
		}
	}
	if sawKey {
		t.Fatal("a provider API key must not authenticate the sign-in")
	}
	if !sawTerm {
		t.Fatal("the child needs a terminal-shaped TERM")
	}
}

func TestPtyProcessRunsARealCommandOnATerminal(t *testing.T) {
	// The one part that cannot be faked: the CLIs refuse a pipe, so prove the
	// child really is attached to a tty and that its size is what we set.
	process, err := startPtyProcess("/bin/sh", []string{
		"-c", "test -t 0 && stty size",
	}, 100, 30)
	if err != nil {
		t.Skipf("no pseudo-terminal available here: %v", err)
	}
	defer process.Close()
	var collected []byte
	buffer := make([]byte, 256)
	for {
		read, err := process.Read(buffer)
		collected = append(collected, buffer[:read]...)
		if err != nil {
			break
		}
	}
	if got := string(collected); !strings.Contains(got, "30 100") {
		t.Fatalf("the child did not see a 100x30 terminal: %q", got)
	}
	if code := process.Wait(); code != 0 {
		t.Fatalf("the child exited with %d", code)
	}
}
