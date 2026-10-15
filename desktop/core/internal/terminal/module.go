package terminal

import (
	"context"
	"encoding/base64"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strings"
	"sync"
	"time"

	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/rpc"
)

type process interface {
	Read([]byte) (int, error)
	Write([]byte) (int, error)
	Resize(cols, rows int) error
	Pid() int
	Exited() <-chan struct{}
	Code() int
	Stop()
	Close() error
}

type launch struct {
	program string
	args    []string
	dir     string
	env     []string
	cols    int
	rows    int
}

type viewer struct {
	peer   rpc.Peer
	box    outbox
	output string
	exit   string
}

type session struct {
	meta
	program string
	pid     int
	running bool
	cols    int
	rows    int
	proc    process
	replay  []byte
	viewers map[string]*viewer
	ended   bool
}

type Module struct {
	mu       sync.Mutex
	sendMu   sync.Mutex
	core     *daemon.Core
	ctx      context.Context
	cancel   context.CancelFunc
	sessions map[string]*session
	watched  map[rpc.Peer]bool
	wake     chan struct{}
	spawn    func(launch) (process, error)
	environ  func() []string
}

func New() *Module {
	return &Module{
		sessions: map[string]*session{},
		watched:  map[rpc.Peer]bool{},
		wake:     make(chan struct{}, 1),
		spawn:    startProcess,
		environ:  os.Environ,
	}
}

func (m *Module) Name() string { return "terminal" }

func (m *Module) Start(ctx context.Context, core *daemon.Core) error {
	m.core = core
	m.ctx, m.cancel = context.WithCancel(ctx)
	handlers := map[string]rpc.Handler{
		"core.terminal.start":  m.handleStart,
		"core.terminal.input":  m.handleInput,
		"core.terminal.resize": m.handleResize,
		"core.terminal.close":  m.handleClose,
		"core.terminal.list":   m.handleList,
		"core.terminal.attach": m.handleAttach,
		"core.terminal.detach": m.handleDetach,
	}
	for method, handler := range handlers {
		core.Registry.Handle(method, m.Name(), local(handler))
	}
	go m.flushLoop()
	return nil
}

func (m *Module) Busy() bool {
	m.mu.Lock()
	defer m.mu.Unlock()
	return len(m.sessions) > 0
}

func (m *Module) Stop() {
	if m.cancel != nil {
		m.cancel()
	}
	m.mu.Lock()
	all := make([]*session, 0, len(m.sessions))
	for _, s := range m.sessions {
		all = append(all, s)
	}
	m.mu.Unlock()
	var wg sync.WaitGroup
	for _, s := range all {
		wg.Add(1)
		go func(s *session) {
			defer wg.Done()
			m.terminate(s, -1)
		}(s)
	}
	wg.Wait()
}

func local(handler rpc.Handler) rpc.Handler {
	return func(ctx context.Context, req *rpc.Request) (any, error) {
		if req.Peer == nil || rpc.Remote(req.Peer) {
			return nil, rpc.Fail(errForbidden)
		}
		return handler(ctx, req)
	}
}

func environment(base []string, overrides map[string]string, strip bool) []string {
	credentials := []string{"ANTHROPIC_API_KEY=", "ANTHROPIC_AUTH_TOKEN=", "OPENAI_API_KEY="}
	out := make([]string, 0, len(base)+len(overrides)+2)
	for _, entry := range base {
		if equals := strings.IndexByte(entry, '='); equals > 0 {
			if _, ok := overrides[entry[:equals]]; ok {
				continue
			}
		}
		if strings.HasPrefix(entry, "TERM=") || strings.HasPrefix(entry, "COLORTERM=") {
			continue
		}
		skip := false
		if strip {
			for _, prefix := range credentials {
				if strings.HasPrefix(entry, prefix) {
					skip = true
					break
				}
			}
		}
		if !skip {
			out = append(out, entry)
		}
	}
	keys := make([]string, 0, len(overrides))
	for key := range overrides {
		keys = append(keys, key)
	}
	sort.Strings(keys)
	for _, key := range keys {
		out = append(out, key+"="+overrides[key])
	}
	return append(out, "TERM=xterm-256color", "COLORTERM=truecolor")
}

func stringList(value any) ([]string, bool) {
	if value == nil {
		return nil, true
	}
	items, ok := value.([]any)
	if !ok {
		return nil, false
	}
	out := make([]string, 0, len(items))
	for _, item := range items {
		text, ok := item.(string)
		if !ok {
			return nil, false
		}
		out = append(out, text)
	}
	return out, true
}

func stringMap(value any) (map[string]string, bool) {
	out := map[string]string{}
	if value == nil {
		return out, true
	}
	items, ok := value.(map[string]any)
	if !ok {
		return nil, false
	}
	for key, item := range items {
		text, ok := item.(string)
		if !ok || key == "" || strings.ContainsAny(key, "=\x00") {
			return nil, false
		}
		out[key] = text
	}
	return out, true
}

func (m *Module) describe(s *session) map[string]any {
	out := terminalJSON(s.meta, s.running, s.cols, s.rows)
	out["pid"] = s.pid
	out["program"] = s.program
	return out
}

func (m *Module) handleStart(ctx context.Context, req *rpc.Request) (any, error) {
	if !supported {
		return nil, rpc.Fail(errUnsupported)
	}
	params := req.Object()
	id := jsonString(params["terminal"])
	if id != "" && !validTerminalID(id) {
		return nil, rpc.Fail(errBadRequest)
	}
	program := strings.TrimSpace(jsonString(params["program"]))
	args, okArgs := stringList(params["args"])
	env, okEnv := stringMap(params["env"])
	if program == "" || !okArgs || !okEnv {
		return nil, rpc.Fail(errBadRequest)
	}
	cwd := strings.TrimSpace(jsonString(params["cwd"]))
	if cwd == "" {
		home, err := os.UserHomeDir()
		if err != nil || home == "" {
			home = "/"
		}
		cwd = home
	}
	if !filepath.IsAbs(cwd) {
		return nil, rpc.Fail(errBadRequest)
	}
	cwd = filepath.Clean(cwd)
	resolved := program
	if !strings.ContainsRune(program, '/') {
		found, err := exec.LookPath(program)
		if err != nil {
			return nil, rpc.Fail(errCliMissing)
		}
		resolved = found
	}
	strip, _ := params["stripCredentials"].(bool)
	cols := clampCols(jsonInt(params["cols"], 80))
	rows := clampRows(jsonInt(params["rows"], 24))
	attach := true
	if value, ok := params["attach"].(bool); ok {
		attach = value
	}
	cli := strings.TrimSpace(jsonString(params["cli"]))
	if cli == "" {
		cli = filepath.Base(resolved)
	}
	device := strings.TrimSpace(jsonString(params["device"]))
	if device == "" {
		device = req.Peer.Device().Name
	}

	m.mu.Lock()
	if id == "" {
		for id == "" || m.sessions[id] != nil {
			id = newTerminalID()
		}
	} else if m.sessions[id] != nil {
		m.mu.Unlock()
		return nil, rpc.Fail(errBadRequest)
	}
	if len(m.sessions) >= maxCoreTerminals {
		m.mu.Unlock()
		return nil, rpc.Fail(errTerminalLimit)
	}
	s := &session{
		meta: meta{
			id:        id,
			provider:  strings.TrimSpace(jsonString(params["provider"])),
			cli:       cli,
			owner:     strings.TrimSpace(jsonString(params["owner"])),
			name:      strings.TrimSpace(jsonString(params["name"])),
			cwd:       cwd,
			device:    device,
			mode:      strings.TrimSpace(jsonString(params["mode"])),
			account:   strings.TrimSpace(jsonString(params["account"])),
			startedMs: time.Now().UnixMilli(),
			session:   jsonInt(params["session"], 0),
		},
		program: resolved,
		cols:    cols,
		rows:    rows,
		viewers: map[string]*viewer{},
	}
	m.sessions[id] = s
	m.mu.Unlock()

	proc, err := m.spawn(launch{program: resolved, args: args, dir: cwd,
		env: environment(m.environ(), env, strip), cols: cols, rows: rows})
	if err != nil {
		m.mu.Lock()
		delete(m.sessions, id)
		m.mu.Unlock()
		if m.core != nil {
			m.core.Logger.Printf("terminal: could not start %s in %s: %v", resolved, cwd, err)
		}
		if rpc.Code(err) == errUnsupported {
			return nil, err
		}
		return nil, rpc.Fail(errTerminalFailed)
	}
	m.mu.Lock()
	s.proc = proc
	if s.ended {
		m.mu.Unlock()
		proc.Stop()
		return nil, rpc.Fail(errTerminalFailed)
	}
	s.pid = proc.Pid()
	s.running = true
	if attach {
		m.addViewerLocked(s, req.Peer, time.Now())
	}
	out := m.describe(s)
	m.mu.Unlock()
	out["replay"] = ""
	if m.core != nil {
		m.core.Logger.Printf("terminal: started %s (pid %d) in %s for %s", resolved, s.pid, cwd, device)
	}
	go m.pump(s)
	return out, nil
}

func (m *Module) addViewerLocked(s *session, peer rpc.Peer, sentAt time.Time) {
	s.viewers[peer.ID()] = &viewer{peer: peer, box: outbox{sentAt: sentAt},
		output: coreOutputEvent, exit: coreExitEvent}
	if m.watched[peer] {
		return
	}
	m.watched[peer] = true
	ctx := m.ctx
	go func() {
		select {
		case <-peer.Done():
		case <-ctx.Done():
			return
		}
		m.mu.Lock()
		delete(m.watched, peer)
		for _, s := range m.sessions {
			if v := s.viewers[peer.ID()]; v != nil && v.peer == peer {
				delete(s.viewers, peer.ID())
			}
		}
		m.mu.Unlock()
	}()
}

func (m *Module) lookupLocked(params map[string]any) *session {
	id := jsonString(params["terminal"])
	if !validTerminalID(id) {
		return nil
	}
	s := m.sessions[id]
	if s == nil || s.proc == nil || s.ended {
		return nil
	}
	return s
}

func (m *Module) lookup(params map[string]any) *session {
	m.mu.Lock()
	defer m.mu.Unlock()
	return m.lookupLocked(params)
}

func (m *Module) handleInput(ctx context.Context, req *rpc.Request) (any, error) {
	params := req.Object()
	s := m.lookup(params)
	if s == nil {
		return nil, rpc.Fail(errUnknownTerminal)
	}
	data, err := decodeInput(params["data"])
	if err != nil {
		return nil, err
	}
	writeAll(s.proc, data)
	return map[string]any{}, nil
}

func writeAll(proc process, data []byte) {
	if file, ok := proc.(interface{ SetWriteDeadline(time.Time) error }); ok {
		_ = file.SetWriteDeadline(time.Now().Add(2 * time.Second))
	}
	for len(data) > 0 {
		n, err := proc.Write(data)
		if n > 0 {
			data = data[n:]
		}
		if err != nil {
			return
		}
	}
}

func (m *Module) handleResize(ctx context.Context, req *rpc.Request) (any, error) {
	params := req.Object()
	m.mu.Lock()
	defer m.mu.Unlock()
	s := m.lookupLocked(params)
	if s == nil {
		return nil, rpc.Fail(errUnknownTerminal)
	}
	cols := jsonInt(params["cols"], 0)
	rows := jsonInt(params["rows"], 0)
	if cols <= 0 || rows <= 0 {
		return nil, rpc.Fail(errBadRequest)
	}
	s.cols = clampCols(cols)
	s.rows = clampRows(rows)
	_ = s.proc.Resize(s.cols, s.rows)
	return map[string]any{}, nil
}

func (m *Module) handleAttach(ctx context.Context, req *rpc.Request) (any, error) {
	params := req.Object()
	m.mu.Lock()
	defer m.mu.Unlock()
	s := m.lookupLocked(params)
	if s == nil {
		return nil, rpc.Fail(errUnknownTerminal)
	}
	cols := jsonInt(params["cols"], 0)
	rows := jsonInt(params["rows"], 0)
	if cols > 0 && rows > 0 {
		s.cols = clampCols(cols)
		s.rows = clampRows(rows)
		_ = s.proc.Resize(s.cols, s.rows)
	}
	m.addViewerLocked(s, req.Peer, time.Time{})
	out := m.describe(s)
	out["replay"] = base64.StdEncoding.EncodeToString(s.replay)
	return out, nil
}

func (m *Module) handleDetach(ctx context.Context, req *rpc.Request) (any, error) {
	params := req.Object()
	m.mu.Lock()
	defer m.mu.Unlock()
	s := m.lookupLocked(params)
	if s == nil {
		return nil, rpc.Fail(errUnknownTerminal)
	}
	delete(s.viewers, req.Peer.ID())
	return map[string]any{}, nil
}

func (m *Module) handleClose(ctx context.Context, req *rpc.Request) (any, error) {
	params := req.Object()
	s := m.lookup(params)
	if s == nil {
		return nil, rpc.Fail(errUnknownTerminal)
	}
	if m.core != nil {
		m.core.Logger.Printf("terminal: closing %s (pid %d) in %s for %s", s.program, s.pid, s.cwd, req.Peer.Device().Name)
	}
	go m.terminate(s, -1)
	return map[string]any{}, nil
}

func (m *Module) handleList(ctx context.Context, req *rpc.Request) (any, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	all := make([]*session, 0, len(m.sessions))
	for _, s := range m.sessions {
		if s.proc != nil {
			all = append(all, s)
		}
	}
	sort.Slice(all, func(i, j int) bool {
		if all[i].startedMs != all[j].startedMs {
			return all[i].startedMs < all[j].startedMs
		}
		return all[i].id < all[j].id
	})
	list := make([]any, 0, len(all))
	for _, s := range all {
		list = append(list, m.describe(s))
	}
	return map[string]any{"terminals": list}, nil
}

func (m *Module) pump(s *session) {
	buf := make([]byte, readChunkBytes)
	for {
		n, err := s.proc.Read(buf)
		if n > 0 {
			m.output(s, append([]byte(nil), buf[:n]...))
		}
		if err != nil {
			break
		}
	}
	code := s.proc.Code()
	m.mu.Lock()
	if s.ended {
		m.mu.Unlock()
		return
	}
	s.ended = true
	s.running = false
	s.pid = 0
	delete(m.sessions, s.id)
	m.mu.Unlock()
	_ = s.proc.Close()
	if m.core != nil {
		m.core.Logger.Printf("terminal: %s in %s ended with status %d", s.program, s.cwd, code)
	}
	m.deliverEnd(s, code)
}

func (m *Module) output(s *session, chunk []byte) {
	m.mu.Lock()
	if s.ended {
		m.mu.Unlock()
		return
	}
	s.replay = appendReplay(s.replay, chunk, replayBytes)
	for _, v := range s.viewers {
		v.box.queue(chunk, maxPendingBytes)
	}
	watching := len(s.viewers) > 0
	m.mu.Unlock()
	if watching {
		m.poke()
	}
}

func (m *Module) poke() {
	select {
	case m.wake <- struct{}{}:
	default:
	}
}

func (m *Module) terminate(s *session, code int) {
	m.mu.Lock()
	if s.ended {
		m.mu.Unlock()
		return
	}
	s.ended = true
	s.running = false
	delete(m.sessions, s.id)
	m.mu.Unlock()
	if s.proc != nil {
		s.proc.Stop()
	}
	m.deliverEnd(s, code)
}

type delivery struct {
	peer   rpc.Peer
	events []string
	data   []any
}

func (m *Module) deliverEnd(s *session, code int) {
	m.sendMu.Lock()
	defer m.sendMu.Unlock()
	m.mu.Lock()
	ids := make([]string, 0, len(s.viewers))
	for id := range s.viewers {
		ids = append(ids, id)
	}
	sort.Strings(ids)
	batch := make([]delivery, 0, len(ids))
	for _, id := range ids {
		v := s.viewers[id]
		d := delivery{peer: v.peer}
		for v.box.hasOutput() {
			d.events = append(d.events, v.output)
			d.data = append(d.data, v.box.take(s.id, s.replay, maxEventBytes))
		}
		d.events = append(d.events, v.exit)
		d.data = append(d.data, map[string]any{"terminal": s.id, "code": code})
		batch = append(batch, d)
	}
	s.viewers = map[string]*viewer{}
	m.mu.Unlock()
	for _, d := range batch {
		for i, event := range d.events {
			if !d.peer.Send(event, d.data[i]) {
				break
			}
		}
	}
}

type frame struct {
	peer  rpc.Peer
	event string
	data  map[string]any
}

func (m *Module) flush() time.Duration {
	m.sendMu.Lock()
	defer m.sendMu.Unlock()
	m.mu.Lock()
	now := time.Now()
	wait := time.Duration(-1)
	later := func(d time.Duration) {
		if wait < 0 || d < wait {
			wait = d
		}
	}
	var batch []frame
	for _, s := range m.sessions {
		for _, v := range s.viewers {
			if !v.box.hasOutput() {
				continue
			}
			interval := followInterval(v.peer.Transport())
			if remaining := v.box.sentAt.Add(interval).Sub(now); remaining > 0 {
				later(remaining)
				continue
			}
			v.box.sentAt = now
			batch = append(batch, frame{peer: v.peer, event: v.output, data: v.box.take(s.id, s.replay, maxEventBytes)})
			if v.box.hasOutput() {
				later(interval)
			}
		}
	}
	m.mu.Unlock()
	var gone []rpc.Peer
	for _, f := range batch {
		if !f.peer.Send(f.event, f.data) {
			gone = append(gone, f.peer)
		}
	}
	if len(gone) > 0 {
		m.mu.Lock()
		for _, peer := range gone {
			for _, s := range m.sessions {
				if v := s.viewers[peer.ID()]; v != nil && v.peer == peer {
					delete(s.viewers, peer.ID())
				}
			}
		}
		m.mu.Unlock()
	}
	return wait
}

func (m *Module) flushLoop() {
	timer := time.NewTimer(time.Hour)
	timer.Stop()
	defer timer.Stop()
	for {
		select {
		case <-m.ctx.Done():
			return
		case <-m.wake:
		case <-timer.C:
		}
		if wait := m.flush(); wait >= 0 {
			timer.Reset(wait)
		}
	}
}
