package agentproc

import (
	"bufio"
	"context"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"

	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/rpc"
)

const (
	errForbidden    = "forbidden"
	shutdownHardKey = "core.shutdown.hard"
	maxSocketPath   = 100
	writeChunk      = 1 << 20
)

type Module struct {
	mu           sync.Mutex
	core         *daemon.Core
	ctx          context.Context
	cancel       context.CancelFunc
	root         string
	procs        map[string]*proc
	starting     map[string]bool
	serial       int64
	shimCommand  func() (string, []string)
	environ      func() []string
	segmentBytes int64
	capBytes     int64
	orphanGrace  time.Duration
	retention    time.Duration
	handshake    time.Duration
	factory      ConsumerFactory
	useTCP       bool
}

func New() *Module {
	return &Module{
		procs:       map[string]*proc{},
		starting:    map[string]bool{},
		serial:      time.Now().UnixMilli() % 1_000_000_000,
		shimCommand: defaultShimCommand,
		environ:     os.Environ,
		orphanGrace: 60 * time.Second,
		retention:   7 * 24 * time.Hour,
		handshake:   20 * time.Second,
		useTCP:      defaultUseTCP,
	}
}

func defaultShimCommand() (string, []string) {
	exe, err := os.Executable()
	if err != nil {
		exe = os.Args[0]
	}
	return exe, []string{"agentproc-shim"}
}

func (m *Module) Name() string { return "agentproc" }

func Root(stateDir string) string { return filepath.Join(stateDir, "core", "agentproc") }

func (m *Module) Start(ctx context.Context, core *daemon.Core) error {
	m.core = core
	m.ctx, m.cancel = context.WithCancel(ctx)
	if m.root == "" {
		m.root = Root(core.Layout.State)
	}
	if err := os.MkdirAll(m.root, 0o700); err != nil {
		return err
	}
	handlers := map[string]rpc.Handler{
		"core.agentproc.start":         m.handleStart,
		"core.agentproc.write":         m.handleWrite,
		"core.agentproc.closeStdin":    m.handleCloseStdin,
		"core.agentproc.signal":        m.handleSignal,
		"core.agentproc.kill":          m.handleKill,
		"core.agentproc.stop":          m.handleStop,
		"core.agentproc.list":          m.handleList,
		"core.agentproc.attach":        m.handleAttach,
		"core.agentproc.detach":        m.handleDetach,
		"core.agentproc.ack":           m.handleAck,
		"core.agentproc.flow":          m.handleFlow,
		"core.agentproc.forget":        m.handleForget,
		"core.agentproc.setMeta":       m.handleSetMeta,
		"core.agentproc.setDetachable": m.handleSetDetachable,
	}
	for method, handler := range handlers {
		core.Registry.Handle(method, m.Name(), local(handler))
	}
	m.scan()
	go m.expireLoop()
	return nil
}

func (m *Module) Busy() bool {
	m.mu.Lock()
	defer m.mu.Unlock()
	for _, p := range m.procs {
		if p.running() {
			return true
		}
	}
	return false
}

func (m *Module) Stop() {
	hard := false
	if m.core != nil {
		hard, _ = m.core.Get(shutdownHardKey).(bool)
	}
	if m.cancel != nil {
		m.cancel()
	}
	m.mu.Lock()
	all := make([]*proc, 0, len(m.procs))
	for _, p := range m.procs {
		all = append(all, p)
	}
	m.mu.Unlock()
	if hard {
		for _, p := range all {
			_ = m.sendFrame(p, frameStop, int32Payload(1500))
		}
		deadline := time.Now().Add(3 * time.Second)
		for time.Now().Before(deadline) && m.Busy() {
			time.Sleep(50 * time.Millisecond)
		}
	}
	m.mu.Lock()
	for _, p := range all {
		for _, a := range p.attached {
			a.halt()
		}
		p.attached = nil
		if p.ownerTimer != nil {
			p.ownerTimer.Stop()
			p.ownerTimer = nil
		}
		if p.conn != nil {
			p.conn.Close()
		}
	}
	m.mu.Unlock()
}

func local(handler rpc.Handler) rpc.Handler {
	return func(ctx context.Context, req *rpc.Request) (any, error) {
		if req.Peer == nil || rpc.Remote(req.Peer) {
			return nil, rpc.Fail(errForbidden)
		}
		return handler(ctx, req)
	}
}

func (m *Module) socketPath(dir string) string {
	path := filepath.Join(dir, "s")
	if len(path) <= maxSocketPath {
		return path
	}
	sum := sha256.Sum256([]byte(dir))
	short := filepath.Join(os.TempDir(), "fmap-"+strconv.Itoa(os.Getuid()))
	_ = os.MkdirAll(short, 0o700)
	if !privateDir(short) {
		if fresh, err := os.MkdirTemp("", "fmap-"); err == nil {
			short = fresh
		}
	}
	return filepath.Join(short, hex.EncodeToString(sum[:8])+".sock")
}

func (m *Module) lookup(key string) (*proc, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	p := m.procs[key]
	if p == nil {
		return nil, rpc.Fail("unknown_key")
	}
	return p, nil
}

type startParams struct {
	Key        string         `json:"key"`
	Program    string         `json:"program"`
	Args       []string       `json:"args"`
	Env        []string       `json:"env"`
	Cwd        string         `json:"cwd"`
	Label      string         `json:"label"`
	Detachable bool           `json:"detachable"`
	Merge      bool           `json:"merge"`
	Scope      *Scope         `json:"scope"`
	Meta       map[string]any `json:"meta"`
	Attach     *bool          `json:"attach"`
	Window     int64          `json:"window"`
	SkipStdout bool           `json:"skipStdout"`
	SkipStderr bool           `json:"skipStderr"`
	Secrets    []string       `json:"secrets"`
}

func (m *Module) handleStart(ctx context.Context, req *rpc.Request) (any, error) {
	var params startParams
	if err := req.Bind(&params); err != nil {
		return nil, err
	}
	if !validKey(params.Key) {
		return nil, rpc.Fail("bad_key")
	}
	if strings.TrimSpace(params.Program) == "" {
		return nil, rpc.Fail("bad_params")
	}
	m.mu.Lock()
	if m.starting[params.Key] {
		m.mu.Unlock()
		return nil, rpc.Fail("key_busy")
	}
	previous := m.procs[params.Key]
	if previous != nil && previous.running() {
		m.mu.Unlock()
		return nil, rpc.Fail("key_busy")
	}
	m.starting[params.Key] = true
	m.serial++
	serial := m.serial
	m.mu.Unlock()
	defer func() {
		m.mu.Lock()
		delete(m.starting, params.Key)
		m.mu.Unlock()
	}()
	if previous != nil {
		m.forget(previous)
	}
	dir := filepath.Join(m.root, params.Key)
	removeProcFiles(dir, "")
	if err := os.MkdirAll(dir, 0o700); err != nil {
		return nil, rpc.Fail("failed_to_start: " + err.Error())
	}
	sp := spec{Key: params.Key, Program: params.Program, Args: params.Args, Cwd: params.Cwd, Label: params.Label,
		Detachable: params.Detachable, Merge: params.Merge, CreatedMs: time.Now().UnixMilli(), Boot: bootID()}
	if m.useTCP {
		sp.Transport = transportTCP
	} else {
		sp.Socket = m.socketPath(dir)
	}
	env := params.Env
	if len(env) == 0 {
		env = m.environ()
	}
	launch := shimLaunch{Program: params.Program, Args: params.Args, Env: releasedEnv(env), Cwd: params.Cwd,
		Merge: params.Merge, Socket: sp.Socket, Transport: sp.Transport, SegmentBytes: m.segmentBytes, CapBytes: m.capBytes}
	var scope *Scope
	if params.Scope != nil && params.Scope.usable() {
		scope = params.Scope
		sp.Unit = unitName(params.Label, os.Getpid(), serial)
		sp.Contained = scope.Contain
	}
	if err := writeJSONFile(filepath.Join(dir, "spec.json"), sp); err != nil {
		return nil, rpc.Fail("failed_to_start: " + err.Error())
	}
	meta := params.Meta
	if meta == nil {
		meta = map[string]any{}
	}
	_ = writeJSONFile(filepath.Join(dir, "meta.json"), meta)
	reply, err := m.launchShim(dir, launch, scope, sp.Unit, params.Label)
	if err != nil && scope != nil {
		if m.core != nil {
			m.core.Logger.Printf("agentproc %s: scope %s failed (%v); starting without it", params.Key, sp.Unit, err)
		}
		removeProcFiles(dir, sp.Socket)
		_ = os.MkdirAll(dir, 0o700)
		sp.Unit, sp.Contained = "", false
		_ = writeJSONFile(filepath.Join(dir, "spec.json"), sp)
		_ = writeJSONFile(filepath.Join(dir, "meta.json"), meta)
		reply, err = m.launchShim(dir, launch, nil, "", params.Label)
	}
	if err != nil {
		removeProcFiles(dir, sp.Socket)
		return nil, rpc.Fail("failed_to_start: " + err.Error())
	}
	conn, err := dialShim(sp, dir, 3*time.Second)
	if err != nil {
		_ = signalPid(reply.Pid, sigKill)
		return nil, rpc.Fail("failed_to_start: " + err.Error())
	}
	p := &proc{key: params.Key, dir: dir, spec: sp, meta: meta, conn: conn, secrets: params.Secrets,
		run: runInfo{Protocol: shimProtocol, ShimPid: reply.ShimPid, Pid: reply.Pid, Pgid: reply.Pid, StartedMs: time.Now().UnixMilli()}}
	m.mu.Lock()
	m.procs[params.Key] = p
	if !params.Detachable {
		m.setOwner(p, req.Peer)
	}
	m.mu.Unlock()
	m.attachConsumer(p)
	m.mu.Lock()
	parsed := p.consumer != nil
	if params.Attach == nil || *params.Attach {
		a := m.addAttachment(p, req.Peer, 0, 0, params.Window)
		a.skip[0] = params.SkipStdout && parsed
		a.skip[1] = params.SkipStderr && takesStderr(p.consumer)
	}
	parsedStderr := takesStderr(p.consumer)
	m.mu.Unlock()
	go m.follow(p, conn)
	if m.core != nil {
		m.core.Logger.Printf("agentproc %s started pid %d (shim %d, unit %q, detachable %v)", params.Key, reply.Pid, reply.ShimPid, sp.Unit, params.Detachable)
	}
	return map[string]any{"key": params.Key, "pid": reply.Pid, "shimPid": reply.ShimPid, "unit": sp.Unit,
		"contained": sp.Contained, "parsed": parsed, "parsedStderr": parsedStderr}, nil
}

func (m *Module) launchShim(dir string, launch shimLaunch, scope *Scope, unit, label string) (handshake, error) {
	program, args := m.shimCommand()
	args = append(append([]string{}, args...), dir)
	if scope != nil {
		program, args = scopeCommand(*scope, unit, label, program, args)
	}
	cmd := exec.Command(program, args...)
	cmd.Env = releasedEnv(m.environ())
	cmd.Dir = dir
	cmd.SysProcAttr = shimAttr()
	logFile, err := os.OpenFile(filepath.Join(dir, "shim.log"), os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0o600)
	if err == nil {
		cmd.Stderr = logFile
		defer logFile.Close()
	}
	stdin, err := cmd.StdinPipe()
	if err != nil {
		return handshake{}, err
	}
	stdout, err := cmd.StdoutPipe()
	if err != nil {
		return handshake{}, err
	}
	if err := cmd.Start(); err != nil {
		return handshake{}, err
	}
	raw, _ := json.Marshal(launch)
	_, _ = stdin.Write(raw)
	_ = stdin.Close()
	lines := make(chan []byte, 1)
	go func() {
		line, _ := bufio.NewReader(stdout).ReadBytes('\n')
		lines <- line
	}()
	var reply handshake
	select {
	case line := <-lines:
		go func() { _ = cmd.Wait() }()
		if len(line) == 0 || json.Unmarshal(line, &reply) != nil {
			return handshake{}, errors.New("the shim exited before it started the agent")
		}
	case <-time.After(m.handshake):
		_ = cmd.Process.Kill()
		go func() { _ = cmd.Wait() }()
		return handshake{}, errors.New("the shim did not answer")
	}
	if reply.Error != "" {
		return handshake{}, errors.New(reply.Error)
	}
	if reply.Pid <= 0 {
		return handshake{}, errors.New("the shim reported no pid")
	}
	return reply, nil
}

func (m *Module) setOwner(p *proc, peer rpc.Peer) {
	p.owner = peer
	if p.ownerTimer != nil {
		p.ownerTimer.Stop()
		p.ownerTimer = nil
	}
	ctx := m.ctx
	go func() {
		select {
		case <-peer.Done():
		case <-ctx.Done():
			return
		}
		m.mu.Lock()
		if p.owner != peer || !p.running() || p.forgotten {
			m.mu.Unlock()
			return
		}
		p.owner = nil
		p.forgetOnExit = true
		m.mu.Unlock()
		if m.core != nil {
			m.core.Logger.Printf("agentproc %s: its desktop connection closed; stopping it", p.key)
		}
		_ = m.sendFrame(p, frameStop, int32Payload(1500))
	}()
}

func (m *Module) armOrphanTimer(p *proc) {
	p.ownerTimer = time.AfterFunc(m.orphanGrace, func() {
		m.mu.Lock()
		if p.owner != nil || !p.running() || p.forgotten {
			m.mu.Unlock()
			return
		}
		p.forgetOnExit = true
		m.mu.Unlock()
		if m.core != nil {
			m.core.Logger.Printf("agentproc %s: no desktop claimed it after a core restart; stopping it", p.key)
		}
		_ = m.sendFrame(p, frameStop, int32Payload(1500))
	})
}

type keyParams struct {
	Key        string          `json:"key"`
	Data       string          `json:"data"`
	Signal     json.RawMessage `json:"signal"`
	Group      bool            `json:"group"`
	Grace      int             `json:"graceMs"`
	Forget     bool            `json:"forget"`
	Out        *int64          `json:"stdoutOffset"`
	Err        *int64          `json:"stderrOffset"`
	Window     int64           `json:"window"`
	Meta       map[string]any  `json:"meta"`
	Detach     *bool           `json:"detachable"`
	SkipStdout bool            `json:"skipStdout"`
	SkipStderr bool            `json:"skipStderr"`
}

func (m *Module) bindKey(req *rpc.Request) (keyParams, *proc, error) {
	var params keyParams
	if err := req.Bind(&params); err != nil {
		return params, nil, err
	}
	p, err := m.lookup(params.Key)
	return params, p, err
}

func (m *Module) handleWrite(ctx context.Context, req *rpc.Request) (any, error) {
	params, p, err := m.bindKey(req)
	if err != nil {
		return nil, err
	}
	data, err := base64.StdEncoding.DecodeString(params.Data)
	if err != nil {
		return nil, rpc.Fail("bad_params")
	}
	m.mu.Lock()
	consumer := p.consumer
	m.mu.Unlock()
	if consumer != nil {
		consumer.Stdin(data)
	}
	written := 0
	for len(data) > 0 {
		chunk := data
		if len(chunk) > writeChunk {
			chunk = data[:writeChunk]
		}
		if err := m.sendFrame(p, frameWrite, chunk); err != nil {
			return nil, err
		}
		written += len(chunk)
		data = data[len(chunk):]
	}
	return map[string]any{"written": written}, nil
}

func (m *Module) handleCloseStdin(ctx context.Context, req *rpc.Request) (any, error) {
	_, p, err := m.bindKey(req)
	if err != nil {
		return nil, err
	}
	if err := m.sendFrame(p, frameClose, nil); err != nil {
		return nil, err
	}
	return map[string]any{"closed": true}, nil
}

func parseSignal(raw json.RawMessage) (int, bool) {
	if len(raw) == 0 || string(raw) == "null" {
		return sigTerm, true
	}
	var number int
	if json.Unmarshal(raw, &number) == nil {
		return number, number > 0
	}
	var name string
	if json.Unmarshal(raw, &name) != nil {
		return 0, false
	}
	name = strings.TrimPrefix(strings.ToUpper(strings.TrimSpace(name)), "SIG")
	if value, ok := signalNames[name]; ok {
		return value, true
	}
	if number, err := strconv.Atoi(name); err == nil && number > 0 {
		return number, true
	}
	return 0, false
}

func (m *Module) handleSignal(ctx context.Context, req *rpc.Request) (any, error) {
	params, p, err := m.bindKey(req)
	if err != nil {
		return nil, err
	}
	sig, ok := parseSignal(params.Signal)
	if !ok {
		return nil, rpc.Fail("bad_signal")
	}
	kind := frameSignal
	if params.Group {
		kind = frameGroup
	}
	if err := m.sendFrame(p, kind, int32Payload(sig)); err != nil {
		return nil, err
	}
	return map[string]any{"signal": sig}, nil
}

func (m *Module) handleKill(ctx context.Context, req *rpc.Request) (any, error) {
	_, p, err := m.bindKey(req)
	if err != nil {
		return nil, err
	}
	if err := m.sendFrame(p, frameKill, nil); err != nil {
		return nil, err
	}
	return map[string]any{"killed": true}, nil
}

func (m *Module) handleStop(ctx context.Context, req *rpc.Request) (any, error) {
	params, p, err := m.bindKey(req)
	if err != nil {
		return nil, err
	}
	m.mu.Lock()
	running := p.running()
	if params.Forget {
		p.forgetOnExit = true
	}
	m.mu.Unlock()
	if !running {
		if params.Forget {
			m.forget(p)
		}
		return map[string]any{"running": false}, nil
	}
	grace := params.Grace
	if grace <= 0 {
		grace = 1500
	}
	if err := m.sendFrame(p, frameStop, int32Payload(grace)); err != nil {
		return nil, err
	}
	return map[string]any{"running": true, "graceMs": grace}, nil
}

func (m *Module) handleList(ctx context.Context, req *rpc.Request) (any, error) {
	m.mu.Lock()
	keys := make([]string, 0, len(m.procs))
	for key := range m.procs {
		keys = append(keys, key)
	}
	sort.Strings(keys)
	out := make([]any, 0, len(keys))
	for _, key := range keys {
		out = append(out, m.procs[key].info())
	}
	m.mu.Unlock()
	return map[string]any{"processes": out}, nil
}

func (m *Module) handleAttach(ctx context.Context, req *rpc.Request) (any, error) {
	params, p, err := m.bindKey(req)
	if err != nil {
		return nil, err
	}
	out, errOffset := int64(-1), int64(-1)
	if params.Out != nil {
		out = *params.Out
	}
	if params.Err != nil {
		errOffset = *params.Err
	}
	m.mu.Lock()
	defer m.mu.Unlock()
	if p.forgotten {
		return nil, rpc.Fail("unknown_key")
	}
	a := m.addAttachment(p, req.Peer, out, errOffset, params.Window)
	a.skip[0] = params.SkipStdout && p.consumer != nil
	a.skip[1] = params.SkipStderr && takesStderr(p.consumer)
	if !p.spec.Detachable && p.running() && (p.owner == nil || p.owner != req.Peer) {
		select {
		case <-ownerDone(p.owner):
			m.setOwner(p, req.Peer)
		default:
			if p.owner == nil {
				m.setOwner(p, req.Peer)
			}
		}
	}
	return p.info(), nil
}

func ownerDone(peer rpc.Peer) <-chan struct{} {
	if peer == nil {
		closed := make(chan struct{})
		close(closed)
		return closed
	}
	return peer.Done()
}

func (m *Module) handleDetach(ctx context.Context, req *rpc.Request) (any, error) {
	var params keyParams
	if err := req.Bind(&params); err != nil {
		return nil, err
	}
	m.mu.Lock()
	targets := []*proc{}
	if params.Key == "" {
		for _, p := range m.procs {
			targets = append(targets, p)
		}
	} else if p := m.procs[params.Key]; p != nil {
		targets = append(targets, p)
	} else {
		m.mu.Unlock()
		return nil, rpc.Fail("unknown_key")
	}
	detached := 0
	for _, p := range targets {
		if a := p.attached[req.Peer.ID()]; a != nil {
			delete(p.attached, req.Peer.ID())
			a.halt()
			detached++
		}
	}
	m.mu.Unlock()
	return map[string]any{"detached": detached}, nil
}

func clampOffset(value *int64, current, limit int64) int64 {
	if value == nil || *value < current {
		return current
	}
	if *value > limit {
		return limit
	}
	return *value
}

func (m *Module) handleAck(ctx context.Context, req *rpc.Request) (any, error) {
	params, p, err := m.bindKey(req)
	if err != nil {
		return nil, err
	}
	m.mu.Lock()
	p.ack.Stdout = clampOffset(params.Out, p.ack.Stdout, p.known[0])
	p.ack.Stderr = clampOffset(params.Err, p.ack.Stderr, p.known[1])
	p.ack.Ms = time.Now().UnixMilli()
	ack := p.ack
	dir := p.dir
	if a := p.attached[req.Peer.ID()]; a != nil {
		a.consumed[0] = maxInt64(a.consumed[0], ack.Stdout)
		a.consumed[1] = maxInt64(a.consumed[1], ack.Stderr)
		a.poke()
	}
	m.mu.Unlock()
	if err := writeJSONFile(filepath.Join(dir, "ack.json"), ack); err != nil {
		return nil, rpc.Fail("ack_failed")
	}
	trimSpool(dir, streamOut, ack.Stdout)
	trimSpool(dir, streamErr, ack.Stderr)
	return map[string]any{"stdout": ack.Stdout, "stderr": ack.Stderr}, nil
}

func (m *Module) handleFlow(ctx context.Context, req *rpc.Request) (any, error) {
	params, p, err := m.bindKey(req)
	if err != nil {
		return nil, err
	}
	m.mu.Lock()
	defer m.mu.Unlock()
	a := p.attached[req.Peer.ID()]
	if a == nil {
		return nil, rpc.Fail("not_attached")
	}
	a.consumed[0] = clampOffset(params.Out, a.consumed[0], a.pos[0])
	a.consumed[1] = clampOffset(params.Err, a.consumed[1], a.pos[1])
	a.poke()
	return map[string]any{"stdout": a.consumed[0], "stderr": a.consumed[1]}, nil
}

func (m *Module) handleForget(ctx context.Context, req *rpc.Request) (any, error) {
	_, p, err := m.bindKey(req)
	if err != nil {
		return nil, err
	}
	m.mu.Lock()
	running := p.running()
	m.mu.Unlock()
	if running {
		return nil, rpc.Fail("running")
	}
	m.forget(p)
	return map[string]any{"forgotten": true}, nil
}

func (m *Module) handleSetDetachable(ctx context.Context, req *rpc.Request) (any, error) {
	params, p, err := m.bindKey(req)
	if err != nil {
		return nil, err
	}
	if params.Detach == nil {
		return nil, rpc.Fail("bad_params")
	}
	m.mu.Lock()
	p.spec.Detachable = *params.Detach
	if p.spec.Detachable {
		p.owner = nil
		if p.ownerTimer != nil {
			p.ownerTimer.Stop()
			p.ownerTimer = nil
		}
	} else if p.running() && req.Peer != nil {
		m.setOwner(p, req.Peer)
	}
	spec := p.spec
	dir := p.dir
	m.mu.Unlock()
	if err := writeJSONFile(filepath.Join(dir, "spec.json"), spec); err != nil {
		return nil, rpc.Fail("meta_failed")
	}
	return map[string]any{"detachable": spec.Detachable}, nil
}

func (m *Module) handleSetMeta(ctx context.Context, req *rpc.Request) (any, error) {
	params, p, err := m.bindKey(req)
	if err != nil {
		return nil, err
	}
	p.metaMu.Lock()
	defer p.metaMu.Unlock()
	m.mu.Lock()
	if p.meta == nil {
		p.meta = map[string]any{}
	}
	for key, value := range params.Meta {
		if value == nil {
			delete(p.meta, key)
		} else {
			p.meta[key] = value
		}
	}
	snapshot := make(map[string]any, len(p.meta))
	for key, value := range p.meta {
		snapshot[key] = value
	}
	dir := p.dir
	m.mu.Unlock()
	if err := writeJSONFile(filepath.Join(dir, "meta.json"), snapshot); err != nil {
		return nil, rpc.Fail("meta_failed")
	}
	return map[string]any{"meta": snapshot}, nil
}

func (m *Module) scan() {
	entries, err := os.ReadDir(m.root)
	if err != nil {
		return
	}
	for _, entry := range entries {
		if !entry.IsDir() || !validKey(entry.Name()) {
			continue
		}
		dir := filepath.Join(m.root, entry.Name())
		var sp spec
		if !readJSONFile(filepath.Join(dir, "spec.json"), &sp) || sp.Key != entry.Name() {
			continue
		}
		p := &proc{key: sp.Key, dir: dir, spec: sp, meta: map[string]any{}}
		readJSONFile(filepath.Join(dir, "meta.json"), &p.meta)
		readJSONFile(filepath.Join(dir, "ack.json"), &p.ack)
		readJSONFile(filepath.Join(dir, "run.json"), &p.run)
		p.known = [2]int64{spoolLength(dir, streamOut), spoolLength(dir, streamErr)}
		var exit exitInfo
		hasExit := readJSONFile(filepath.Join(dir, "exit.json"), &exit)
		var conn net.Conn
		if !hasExit && (sp.Socket != "" || sp.Transport == transportTCP) {
			conn, _ = dialShimOnce(sp, dir, 500*time.Millisecond)
		}
		switch {
		case conn != nil:
			p.conn = conn
		case hasExit:
			p.exit = &exit
			p.exitHandled = true
			p.raise(exit.Stdout, exit.Stderr)
		default:
			p.exit = &exitInfo{Code: -1, Crashed: true, Lost: true, ExitedMs: time.Now().UnixMilli(),
				Stdout: p.known[0], Stderr: p.known[1]}
			p.exitHandled = true
			_ = writeJSONFile(filepath.Join(dir, "exit.json"), p.exit)
		}
		if p.exit != nil && m.retention > 0 && time.Since(time.UnixMilli(p.exit.ExitedMs)) > m.retention {
			removeProcFiles(dir, sp.Socket)
			continue
		}
		m.mu.Lock()
		m.procs[p.key] = p
		if p.conn != nil && !sp.Detachable {
			m.armOrphanTimer(p)
		}
		conn = p.conn
		pid := p.run.Pid
		m.mu.Unlock()
		if m.core != nil {
			state := "exited"
			if conn != nil {
				state = "running"
			}
			m.core.Logger.Printf("agentproc: adopted %s (%s, pid %d)", p.key, state, pid)
		}
		if conn != nil {
			go m.follow(p, conn)
		}
	}
}

func (m *Module) expireLoop() {
	ticker := time.NewTicker(time.Hour)
	defer ticker.Stop()
	for {
		select {
		case <-m.ctx.Done():
			return
		case <-ticker.C:
			m.expire()
		}
	}
}

func (m *Module) expire() {
	if m.retention <= 0 {
		return
	}
	m.mu.Lock()
	old := []*proc{}
	for _, p := range m.procs {
		if p.exit != nil && len(p.attached) == 0 && time.Since(time.UnixMilli(p.exit.ExitedMs)) > m.retention {
			old = append(old, p)
		}
	}
	m.mu.Unlock()
	for _, p := range old {
		m.forget(p)
	}
}
