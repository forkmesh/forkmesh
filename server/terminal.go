package mirrornode

import (
	"encoding/base64"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"strings"
	"sync"
	"syscall"
	"time"
	"unsafe"
)

// A provider sign-in terminal, streamed to the account owner's browser.
//
// A managed mirror has no desktop and no display, so signing its agent CLI in
// used to mean an SSH shell from the ForkMesh desktop app — and the website's
// "Sign in to Claude Code on <node>" button was a `forkmesh://` link that only
// worked on a machine with that app installed. This is the same terminal
// without the detour: the relay forwards keystrokes and screen bytes between
// the owner's browser and this daemon over the event socket both already hold
// (app/src/node_terminal.py), and the daemon runs the provider's own login
// command on a pty.
//
// The authority this adds to the socket is deliberately one command. There is
// no shell, no argument comes from the frame, and the only thing the browser
// can choose is which of the two provider CLIs — already advertised by this
// node because their binaries are installed — to sign in. Everything the
// session writes lands in the service account's own HOME, which is where the
// agent CLIs look for it.
const (
	// One frame per pty read, bounded so a full-screen redraw stays well
	// inside eventMaxFrameBytes once base64 and the JSON envelope are added.
	terminalMaxOutputChunk = 6144
	// Minimum spacing between output frames. The pty keeps buffering while we
	// wait, so this coalesces a burst into fewer, larger frames and keeps the
	// relay's terminal budget (NODE_TERMINAL_MSG_MAX_PER_WINDOW) intact.
	terminalOutputInterval = 40 * time.Millisecond
	// What a reconnecting browser is replayed. A sign-in screen is a few
	// kilobytes; this covers it without keeping a transcript.
	terminalReplayBytes = 48 << 10
	// A sign-in that nobody finishes must not leave a prompt open forever. The
	// browser ends its session on close; this is the backstop for a tab that
	// vanished.
	terminalMaxLifetime = 30 * time.Minute
)

// TerminalFrame is one relayed terminal message addressed to this node.
type TerminalFrame struct {
	Action   string
	Session  string
	Node     string
	Provider string
	Data     string
	Cols     int
	Rows     int
}

// terminalProcess is the pty half, so the relay's policy can be tested without
// spawning a provider CLI.
type terminalProcess interface {
	Read([]byte) (int, error)
	Write([]byte) (int, error)
	Resize(cols, rows int) error
	Close() error
	Wait() int
}

// TerminalRelay owns at most one streamed sign-in at a time.
type TerminalRelay struct {
	node     string
	disabled bool
	send     func(map[string]any) bool
	lookPath func(string) (string, error)
	start    func(name string, args []string, cols, rows int) (terminalProcess, error)
	logf     func(string, ...any)

	mu      sync.Mutex
	session *terminalSession
}

type terminalSession struct {
	id      string
	process terminalProcess
	replay  []byte
	done    bool
}

// NewTerminalRelay wires the relay to one node identity and outbound sender.
// `send` returns false when the socket cannot take the frame; a terminal
// stream has no queue, because bytes nobody is watching are worthless.
func NewTerminalRelay(node string, disabled bool, send func(map[string]any) bool) *TerminalRelay {
	return &TerminalRelay{
		node:     strings.ToLower(strings.TrimSpace(node)),
		disabled: disabled,
		send:     send,
		lookPath: exec.LookPath,
		start:    startPtyProcess,
		logf:     func(format string, args ...any) {},
	}
}

// terminalLoginCommand is the entire command surface this relay exposes.
func terminalLoginCommand(provider string) (string, []string, bool) {
	switch provider {
	case "claude-code":
		return "claude", []string{"auth", "login"}, true
	case "codex":
		return "codex", []string{"login"}, true
	default:
		return "", nil, false
	}
}

// Handle applies one relayed frame. Frames addressed to another of the
// account's nodes are ignored: the relay broadcasts to every node socket
// because it cannot tell which machine holds which one, so a node's own name
// is the address.
func (r *TerminalRelay) Handle(frame TerminalFrame) {
	if r == nil || frame.Node != r.node || frame.Session == "" {
		return
	}
	switch frame.Action {
	case "open":
		r.open(frame)
	case "input":
		r.input(frame)
	case "resize":
		r.resize(frame)
	case "close":
		r.Stop(frame.Session)
	}
}

func (r *TerminalRelay) open(frame TerminalFrame) {
	if r.disabled {
		r.fail(frame.Session, "This node is configured not to accept web sign-ins.")
		return
	}
	name, args, ok := terminalLoginCommand(frame.Provider)
	if !ok {
		r.fail(frame.Session, "That agent provider has no sign-in on this node.")
		return
	}
	if _, err := r.lookPath(name); err != nil {
		r.fail(frame.Session, fmt.Sprintf("%s is not installed on this node.", name))
		return
	}

	r.mu.Lock()
	// A browser re-announces its session after a reconnect. The pty outlived
	// the socket, so re-attach to it and repaint rather than restarting a
	// sign-in the operator was halfway through.
	if r.session != nil && r.session.id == frame.Session && !r.session.done {
		existing := r.session
		replay := append([]byte(nil), existing.replay...)
		process := existing.process
		r.mu.Unlock()
		if frame.Cols > 0 && frame.Rows > 0 {
			_ = process.Resize(frame.Cols, frame.Rows)
		}
		r.emit(frame.Session, map[string]any{"action": "ready"})
		r.emitOutput(frame.Session, replay)
		return
	}
	previous := r.session
	r.session = nil
	r.mu.Unlock()
	if previous != nil {
		_ = previous.process.Close()
	}

	process, err := r.start(name, args, frame.Cols, frame.Rows)
	if err != nil {
		r.fail(frame.Session, fmt.Sprintf("Could not start %s: %v", name, err))
		return
	}
	session := &terminalSession{id: frame.Session, process: process}
	r.mu.Lock()
	r.session = session
	r.mu.Unlock()
	r.logf("agent sign-in terminal opened for %s on this node", frame.Provider)
	r.emit(frame.Session, map[string]any{"action": "ready"})
	go r.pump(session)
	go func() {
		timer := time.NewTimer(terminalMaxLifetime)
		defer timer.Stop()
		<-timer.C
		r.Stop(session.id)
	}()
}

func (r *TerminalRelay) pump(session *terminalSession) {
	buffer := make([]byte, terminalMaxOutputChunk)
	for {
		read, err := session.process.Read(buffer)
		if read > 0 {
			chunk := append([]byte(nil), buffer[:read]...)
			r.remember(session, chunk)
			r.emitOutput(session.id, chunk)
			time.Sleep(terminalOutputInterval)
		}
		if err != nil {
			break
		}
	}
	code := session.process.Wait()
	r.mu.Lock()
	if r.session == session {
		r.session.done = true
		r.session = nil
	}
	r.mu.Unlock()
	_ = session.process.Close()
	r.emit(session.id, map[string]any{"action": "exit", "code": code})
}

func (r *TerminalRelay) remember(session *terminalSession, chunk []byte) {
	r.mu.Lock()
	defer r.mu.Unlock()
	session.replay = append(session.replay, chunk...)
	if len(session.replay) > terminalReplayBytes {
		session.replay = append(
			[]byte(nil), session.replay[len(session.replay)-terminalReplayBytes:]...)
	}
}

func (r *TerminalRelay) input(frame TerminalFrame) {
	decoded, err := base64.StdEncoding.DecodeString(frame.Data)
	if err != nil || len(decoded) == 0 {
		return
	}
	r.mu.Lock()
	session := r.session
	r.mu.Unlock()
	if session == nil || session.id != frame.Session {
		return
	}
	_, _ = session.process.Write(decoded)
}

func (r *TerminalRelay) resize(frame TerminalFrame) {
	r.mu.Lock()
	session := r.session
	r.mu.Unlock()
	if session == nil || session.id != frame.Session {
		return
	}
	_ = session.process.Resize(frame.Cols, frame.Rows)
}

// Stop ends the named session; an empty id ends whatever is running, which is
// what a shutting-down daemon wants.
func (r *TerminalRelay) Stop(session string) {
	if r == nil {
		return
	}
	r.mu.Lock()
	current := r.session
	if current == nil || (session != "" && current.id != session) {
		r.mu.Unlock()
		return
	}
	r.session = nil
	r.mu.Unlock()
	_ = current.process.Close()
}

func (r *TerminalRelay) fail(session, message string) {
	r.emit(session, map[string]any{"action": "error", "message": message})
}

func (r *TerminalRelay) emit(session string, fields map[string]any) {
	if r.send == nil {
		return
	}
	frame := map[string]any{
		"type": "terminal", "session": session, "node": r.node,
	}
	for key, value := range fields {
		frame[key] = value
	}
	r.send(frame)
}

func (r *TerminalRelay) emitOutput(session string, data []byte) {
	for start := 0; start < len(data); start += terminalMaxOutputChunk {
		end := start + terminalMaxOutputChunk
		if end > len(data) {
			end = len(data)
		}
		r.emit(session, map[string]any{
			"action": "output",
			"data":   base64.StdEncoding.EncodeToString(data[start:end]),
		})
	}
}

// --- pty -------------------------------------------------------------------

type ptyProcess struct {
	master *os.File
	cmd    *exec.Cmd

	once sync.Once
	code int
}

func ioctl(fd uintptr, request uintptr, argument uintptr) error {
	if _, _, errno := syscall.Syscall(
		syscall.SYS_IOCTL, fd, request, argument); errno != 0 {
		return errno
	}
	return nil
}

// startPtyProcess runs one command on a new pseudo-terminal. A pty, not a
// pipe: the provider CLIs draw a full-screen prompt and refuse to read a
// secret from something that is not a terminal.
func startPtyProcess(name string, args []string, cols, rows int) (terminalProcess, error) {
	process, err := startPtyCommand(name, args, terminalEnvironment(), terminalWorkingDirectory(), cols, rows, nil)
	if err != nil {
		return nil, err
	}
	return process, nil
}

// startPtyCommand is startPtyProcess with the program, environment and
// working directory chosen by the caller: a runner's web sign-in runs the
// login inside the job sandbox (runnerlogin.go).
func startPtyCommand(name string, args, env []string, dir string, cols, rows int, credential *syscall.Credential) (*ptyProcess, error) {
	master, follower, err := openPTY()
	if err != nil {
		return nil, err
	}
	defer follower.Close()

	cmd := exec.Command(name, args...)
	cmd.Dir = dir
	cmd.Env = env
	cmd.Stdin, cmd.Stdout, cmd.Stderr = follower, follower, follower
	// Ctty indexes the child's own descriptors, where the pty follower is 0.
	cmd.SysProcAttr = &syscall.SysProcAttr{
		Setsid: true, Setctty: true, Ctty: 0, Credential: credential,
	}
	if err := cmd.Start(); err != nil {
		master.Close()
		return nil, err
	}
	process := &ptyProcess{master: master, cmd: cmd}
	_ = process.Resize(cols, rows)
	return process, nil
}

func terminalWorkingDirectory() string {
	if home, err := os.UserHomeDir(); err == nil && home != "" {
		return home
	}
	return "/"
}

// terminalEnvironment hands the child a terminal-shaped environment with any
// provider API key removed: a sign-in has to authenticate the account the
// operator is entering, not whatever key happens to be exported into the
// daemon.
func terminalEnvironment() []string {
	return terminalEnvironmentFrom(os.Environ())
}

// terminalEnvironmentFrom applies terminalEnvironment's rules to base.
func terminalEnvironmentFrom(base []string) []string {
	stripped := []string{
		"ANTHROPIC_API_KEY=", "ANTHROPIC_AUTH_TOKEN=", "OPENAI_API_KEY=",
	}
	environment := make([]string, 0, len(base)+2)
	for _, entry := range base {
		skip := false
		for _, prefix := range stripped {
			if strings.HasPrefix(entry, prefix) {
				skip = true
				break
			}
		}
		if !skip && !strings.HasPrefix(entry, "TERM=") &&
			!strings.HasPrefix(entry, "COLORTERM=") {
			environment = append(environment, entry)
		}
	}
	return append(environment, "TERM=xterm-256color", "COLORTERM=truecolor")
}

func (p *ptyProcess) Read(buffer []byte) (int, error) {
	return p.master.Read(buffer)
}

func (p *ptyProcess) Write(buffer []byte) (int, error) {
	return p.master.Write(buffer)
}

func (p *ptyProcess) Resize(cols, rows int) error {
	if cols <= 0 || rows <= 0 {
		return errors.New("terminal size must be positive")
	}
	size := struct{ rows, cols, xpixel, ypixel uint16 }{
		rows: uint16(rows), cols: uint16(cols),
	}
	return ioctl(p.master.Fd(), syscall.TIOCSWINSZ, uintptr(unsafe.Pointer(&size)))
}

func (p *ptyProcess) Close() error {
	if p.cmd.Process != nil {
		// SIGHUP is what a closing terminal sends; the CLI cleans up its own
		// half-written credential state rather than being killed mid-write.
		_ = p.cmd.Process.Signal(syscall.SIGHUP)
	}
	return p.master.Close()
}

// Kill ends the whole process group the pty session leads (Setsid made the
// child its leader), so a CLI's helpers cannot outlive a cancelled sign-in.
func (p *ptyProcess) Kill() {
	if p.cmd.Process != nil {
		_ = syscall.Kill(-p.cmd.Process.Pid, syscall.SIGKILL)
	}
}

func (p *ptyProcess) Wait() int {
	p.once.Do(func() {
		err := p.cmd.Wait()
		if err == nil {
			p.code = 0
			return
		}
		var exit *exec.ExitError
		if errors.As(err, &exit) {
			p.code = exit.ExitCode()
			return
		}
		p.code = -1
	})
	return p.code
}
