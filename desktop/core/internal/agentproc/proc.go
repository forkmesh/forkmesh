package agentproc

import (
	"bufio"
	"encoding/base64"
	"encoding/json"
	"net"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"sync"
	"time"

	"forkmesh.com/desktop-core/internal/rpc"
)

var keyPattern = regexp.MustCompile(`^[A-Za-z0-9][A-Za-z0-9._-]{0,95}$`)

func validKey(key string) bool { return keyPattern.MatchString(key) && !strings.Contains(key, "..") }

type spec struct {
	Key        string   `json:"key"`
	Program    string   `json:"program"`
	Args       []string `json:"args"`
	Cwd        string   `json:"cwd"`
	Label      string   `json:"label"`
	Detachable bool     `json:"detachable"`
	Merge      bool     `json:"merge"`
	Unit       string   `json:"unit,omitempty"`
	Contained  bool     `json:"contained"`
	Socket     string   `json:"socket"`
	Transport  string   `json:"transport,omitempty"`
	CreatedMs  int64    `json:"createdMs"`
	Boot       string   `json:"boot,omitempty"`
}

type ackState struct {
	Stdout int64 `json:"stdout"`
	Stderr int64 `json:"stderr"`
	Ms     int64 `json:"ms"`
}

type attachment struct {
	peer     rpc.Peer
	sink     Consumer
	skip     [2]bool
	pos      [2]int64
	consumed [2]int64
	window   int64
	wake     chan struct{}
	stop     chan struct{}
	stopOnce sync.Once
	exitSent bool
}

func (a *attachment) poke() {
	select {
	case a.wake <- struct{}{}:
	default:
	}
}

func (a *attachment) halt() { a.stopOnce.Do(func() { close(a.stop) }) }

type proc struct {
	key          string
	dir          string
	spec         spec
	meta         map[string]any
	run          runInfo
	known        [2]int64
	ack          ackState
	exit         *exitInfo
	exitHandled  bool
	conn         net.Conn
	writeMu      sync.Mutex
	metaMu       sync.Mutex
	attached     map[string]*attachment
	owner        rpc.Peer
	ownerTimer   *time.Timer
	forgetOnExit bool
	forgotten    bool
	consumer     Consumer
	secrets      []string
}

func (p *proc) running() bool { return p.exit == nil }

var streams = [2]string{streamOut, streamErr}
var streamEvents = [2]string{"core.agentproc.stdout", "core.agentproc.stderr"}

func (p *proc) info() map[string]any {
	out := map[string]any{
		"key": p.key, "pid": p.run.Pid, "shimPid": p.run.ShimPid, "pgid": p.run.Pgid,
		"running": p.running(), "startedMs": p.run.StartedMs, "createdMs": p.spec.CreatedMs,
		"label": p.spec.Label, "unit": p.spec.Unit, "contained": p.spec.Contained,
		"detachable": p.spec.Detachable, "merge": p.spec.Merge, "cwd": p.spec.Cwd,
		"program": p.spec.Program, "meta": p.meta, "stdout": p.known[0], "stderr": p.known[1],
		"ack":      map[string]any{"stdout": p.ack.Stdout, "stderr": p.ack.Stderr},
		"attached": len(p.attached), "owned": p.owner != nil, "parsed": p.consumer != nil,
		"parsedStderr": takesStderr(p.consumer),
	}
	if p.meta == nil {
		out["meta"] = map[string]any{}
	}
	if p.exit != nil {
		out["exit"] = exitEvent(p.key, p.exit)
	} else {
		out["exit"] = nil
	}
	return out
}

func exitEvent(key string, exit *exitInfo) map[string]any {
	return map[string]any{"key": key, "code": exit.Code, "crashed": exit.Crashed, "signal": exit.Signal,
		"lost": exit.Lost, "exitedMs": exit.ExitedMs, "stdout": exit.Stdout, "stderr": exit.Stderr}
}

func (m *Module) sendFrame(p *proc, kind byte, payload []byte) error {
	m.mu.Lock()
	conn := p.conn
	running := p.running()
	m.mu.Unlock()
	if conn == nil || !running {
		return rpc.Fail("not_running")
	}
	p.writeMu.Lock()
	defer p.writeMu.Unlock()
	_ = conn.SetWriteDeadline(time.Now().Add(10 * time.Second))
	if err := writeFrame(conn, kind, payload); err != nil {
		conn.Close()
		return rpc.Fail("not_running")
	}
	return nil
}

func (m *Module) follow(p *proc, conn net.Conn) {
	for conn != nil {
		m.followConn(p, conn)
		conn = m.reconnect(p, conn)
	}
	if m.stopping() {
		return
	}
	m.mu.Lock()
	if p.exit == nil && !p.forgotten {
		var exit exitInfo
		if readJSONFile(filepath.Join(p.dir, "exit.json"), &exit) {
			p.exit = &exit
		} else {
			p.exit = &exitInfo{Code: -1, Crashed: true, Lost: true, ExitedMs: time.Now().UnixMilli(),
				Stdout: maxInt64(p.known[0], spoolLength(p.dir, streamOut)),
				Stderr: maxInt64(p.known[1], spoolLength(p.dir, streamErr))}
			_ = writeJSONFile(filepath.Join(p.dir, "exit.json"), p.exit)
		}
		p.raise(p.exit.Stdout, p.exit.Stderr)
	}
	m.mu.Unlock()
	m.wakeAll(p)
	m.handleExit(p)
}

func (m *Module) stopping() bool { return m.ctx != nil && m.ctx.Err() != nil }

func (m *Module) reconnect(p *proc, old net.Conn) net.Conn {
	if m.stopping() {
		return nil
	}
	m.mu.Lock()
	if p.conn == old {
		p.conn = nil
	}
	done := p.exit != nil || p.forgotten
	shimPid := p.run.ShimPid
	sp, dir := p.spec, p.dir
	m.mu.Unlock()
	if done || !pidAlive(shimPid) {
		return nil
	}
	if readJSONFile(filepath.Join(p.dir, "exit.json"), &exitInfo{}) {
		return nil
	}
	conn, err := dialShim(sp, dir, time.Second)
	if err != nil {
		return nil
	}
	m.mu.Lock()
	if p.forgotten || m.stopping() {
		m.mu.Unlock()
		conn.Close()
		return nil
	}
	p.conn = conn
	m.mu.Unlock()
	return conn
}

func (m *Module) followConn(p *proc, conn net.Conn) {
	reader := bufio.NewReaderSize(conn, 64<<10)
	for {
		kind, payload, err := readFrame(reader)
		if err != nil {
			break
		}
		m.mu.Lock()
		switch kind {
		case frameHello:
			var hello helloInfo
			if json.Unmarshal(payload, &hello) == nil {
				if hello.Pid > 0 {
					p.run = hello.runInfo
				}
				p.raise(hello.Stdout, hello.Stderr)
				if hello.Exit != nil && p.exit == nil {
					p.exit = hello.Exit
					p.raise(hello.Exit.Stdout, hello.Exit.Stderr)
				}
			}
		case frameData:
			out, errLen := payloadLengths(payload)
			p.raise(out, errLen)
		case frameExit:
			var exit exitInfo
			if json.Unmarshal(payload, &exit) == nil && p.exit == nil {
				p.exit = &exit
				p.raise(exit.Stdout, exit.Stderr)
			}
		}
		exited := p.exit != nil
		m.mu.Unlock()
		m.wakeAll(p)
		if exited {
			m.handleExit(p)
		}
	}
	conn.Close()
}

func maxInt64(a, b int64) int64 {
	if a > b {
		return a
	}
	return b
}

func (p *proc) raise(out, errLen int64) {
	if out > p.known[0] {
		p.known[0] = out
	}
	if errLen > p.known[1] {
		p.known[1] = errLen
	}
}

func (m *Module) wakeAll(p *proc) {
	m.mu.Lock()
	list := make([]*attachment, 0, len(p.attached))
	for _, a := range p.attached {
		list = append(list, a)
	}
	m.mu.Unlock()
	for _, a := range list {
		a.poke()
	}
}

func (m *Module) handleExit(p *proc) {
	m.mu.Lock()
	if p.exitHandled || p.exit == nil {
		m.mu.Unlock()
		return
	}
	p.exitHandled = true
	if p.ownerTimer != nil {
		p.ownerTimer.Stop()
		p.ownerTimer = nil
	}
	exit := *p.exit
	forget := p.forgetOnExit && len(p.attached) == 0
	m.mu.Unlock()
	if m.core != nil {
		m.core.Logger.Printf("agentproc %s exited (code %d, crashed %v, lost %v)", p.key, exit.Code, exit.Crashed, exit.Lost)
	}
	if forget {
		m.forget(p)
	}
}

const pumpChunk = 64 << 10

func (m *Module) pump(p *proc, a *attachment) {
	for {
		select {
		case <-a.stop:
			return
		case <-a.peer.Done():
			m.removeAttachment(p, a)
			return
		case <-a.wake:
		}
		for {
			m.mu.Lock()
			if p.forgotten || p.attached[a.peer.ID()] != a {
				m.mu.Unlock()
				return
			}
			known, exit, dir := p.known, p.exit, p.dir
			pos, consumed, window := a.pos, a.consumed, a.window
			m.mu.Unlock()
			sent := false
			for i := range streams {
				if a.skip[i] {
					if pos[i] < known[i] {
						m.mu.Lock()
						a.pos[i] = known[i]
						a.consumed[i] = known[i]
						m.mu.Unlock()
						pos[i] = known[i]
					}
					continue
				}
				if pos[i] >= known[i] {
					continue
				}
				limit := known[i] - pos[i]
				if window > 0 {
					inflight := (pos[0] - consumed[0]) + (pos[1] - consumed[1])
					if inflight >= window {
						break
					}
					if room := window - inflight; limit > room {
						limit = room
					}
				}
				if limit > pumpChunk {
					limit = pumpChunk
				}
				data, from, err := readSpool(dir, streams[i], pos[i], limit)
				if err != nil || len(data) == 0 {
					m.mu.Lock()
					if a.pos[i] < known[i] {
						a.pos[i] = known[i]
					}
					m.mu.Unlock()
					pos[i] = known[i]
					continue
				}
				if a.sink != nil {
					if i == 1 {
						if errSink, ok := a.sink.(StderrConsumer); ok {
							errSink.Stderr(data, from)
						}
					} else {
						a.sink.Stdout(data, from)
					}
					m.mu.Lock()
					a.pos[i] = from + int64(len(data))
					a.consumed[i] = a.pos[i]
					pos[i] = a.pos[i]
					m.mu.Unlock()
					sent = true
					continue
				}
				event := map[string]any{"key": p.key, "data": base64.StdEncoding.EncodeToString(data),
					"offset": from, "end": from + int64(len(data))}
				if from > pos[i] {
					event["gap"] = from - pos[i]
				}
				if !a.peer.Send(streamEvents[i], event) {
					m.removeAttachment(p, a)
					return
				}
				m.mu.Lock()
				a.pos[i] = from + int64(len(data))
				pos[i] = a.pos[i]
				m.mu.Unlock()
				sent = true
			}
			if sent {
				continue
			}
			if exit != nil && !a.exitSent && pos[0] >= known[0] && pos[1] >= known[1] {
				if a.sink != nil {
					a.sink.Exit(exitEvent(p.key, exit))
				} else if !a.peer.Send("core.agentproc.exit", exitEvent(p.key, exit)) {
					m.removeAttachment(p, a)
					return
				}
				m.mu.Lock()
				a.exitSent = true
				if a.sink != nil && p.attached[a.peer.ID()] == a {
					delete(p.attached, a.peer.ID())
				}
				forget := p.forgetOnExit && allExitSent(p)
				m.mu.Unlock()
				if a.sink != nil {
					a.halt()
				}
				if forget {
					m.forget(p)
					return
				}
			}
			break
		}
	}
}

func (m *Module) addAttachment(p *proc, peer rpc.Peer, out, errOffset, window int64) *attachment {
	a := &attachment{peer: peer, window: window, wake: make(chan struct{}, 1), stop: make(chan struct{})}
	if out < 0 {
		out = p.ack.Stdout
	}
	if errOffset < 0 {
		errOffset = p.ack.Stderr
	}
	a.pos = [2]int64{out, errOffset}
	a.consumed = a.pos
	if previous := p.attached[peer.ID()]; previous != nil {
		previous.halt()
	}
	if p.attached == nil {
		p.attached = map[string]*attachment{}
	}
	p.attached[peer.ID()] = a
	go m.pump(p, a)
	a.poke()
	return a
}

func (m *Module) removeAttachment(p *proc, a *attachment) {
	m.mu.Lock()
	if p.attached[a.peer.ID()] == a {
		delete(p.attached, a.peer.ID())
	}
	m.mu.Unlock()
	a.halt()
}

func (m *Module) forget(p *proc) {
	m.mu.Lock()
	if p.forgotten {
		m.mu.Unlock()
		return
	}
	p.forgotten = true
	if m.procs[p.key] == p {
		delete(m.procs, p.key)
	}
	for _, a := range p.attached {
		a.halt()
	}
	p.attached = nil
	conn := p.conn
	p.conn = nil
	if p.ownerTimer != nil {
		p.ownerTimer.Stop()
		p.ownerTimer = nil
	}
	m.mu.Unlock()
	if conn != nil {
		conn.Close()
	}
	removeProcFiles(p.dir, p.spec.Socket)
}

func removeProcFiles(dir, socket string) {
	entries, err := os.ReadDir(dir)
	if err != nil {
		return
	}
	known := map[string]bool{"spec.json": true, "meta.json": true, "ack.json": true, "run.json": true,
		"exit.json": true, "shim.log": true, "s": true, tcpFile: true}
	for _, entry := range entries {
		name := entry.Name()
		if entry.IsDir() {
			continue
		}
		base := strings.TrimSuffix(name, ".tmp")
		if known[base] || len(listSegmentName(base)) > 0 {
			_ = os.Remove(filepath.Join(dir, name))
		}
	}
	if socket != "" && filepath.Dir(socket) != dir {
		if info, err := os.Lstat(socket); err == nil && info.Mode()&os.ModeSocket != 0 {
			_ = os.Remove(socket)
		}
	}
	_ = os.Remove(dir)
}

func listSegmentName(name string) string {
	for _, stream := range streams {
		prefix := stream + "."
		if strings.HasPrefix(name, prefix) && len(name) == len(prefix)+16 {
			digits := name[len(prefix):]
			if strings.Trim(digits, "0123456789") == "" {
				return stream
			}
		}
	}
	return ""
}
