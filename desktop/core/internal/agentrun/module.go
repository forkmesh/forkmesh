package agentrun

import (
	"context"
	"os"
	"path/filepath"
	"strconv"
	"sync"
	"time"

	"forkmesh.com/desktop-core/internal/agentproc"
	"forkmesh.com/desktop-core/internal/agents"
	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/phonefamily"
	"forkmesh.com/desktop-core/internal/rpc"
)

type Module struct {
	mu          sync.Mutex
	core        *daemon.Core
	procs       *agentproc.Module
	w           *agents.Writer
	runs        map[string]*run
	credentials []string
	haveCreds   bool
	now         func() time.Time
	salt        []byte
	gate        *phonefamily.Gate
	metaMu      sync.Mutex
	metaQueue   chan metaWrite
}

func New() *Module {
	return &Module{runs: map[string]*run{}, now: time.Now}
}

func (m *Module) Name() string { return "agentrun" }

func (m *Module) Start(ctx context.Context, core *daemon.Core) error {
	m.core = core
	m.procs, _ = core.Module("agentproc").(*agentproc.Module)
	if store, ok := core.Module("agents").(*agents.Module); ok {
		m.w = store.Writer()
	}
	handlers := map[string]rpc.Handler{
		"core.agentrun.credentials": m.handleCredentials,
		"core.agentrun.attach":      m.handleAttach,
		"core.agentrun.detach":      m.handleDetach,
		"core.agentrun.list":        m.handleList,
	}
	for method, handler := range handlers {
		core.Registry.Handle(method, m.Name(), local(handler))
	}
	m.registerCodex(core)
	m.registerPhone(core)
	if m.procs != nil {
		m.procs.SetConsumerFactory(m.consumer)
	}
	return nil
}

func local(handler rpc.Handler) rpc.Handler {
	return func(ctx context.Context, req *rpc.Request) (any, error) {
		if req.Peer == nil || rpc.Remote(req.Peer) {
			return nil, rpc.Fail("forbidden")
		}
		return handler(ctx, req)
	}
}

func (m *Module) writer() *agents.Writer {
	return m.w
}

func (m *Module) credentialValues() []string {
	m.mu.Lock()
	defer m.mu.Unlock()
	return m.credentials
}

func (m *Module) mergeMeta(key string, meta map[string]any) {
	if m.procs == nil {
		return
	}
	m.metaMu.Lock()
	if m.metaQueue == nil {
		m.metaQueue = make(chan metaWrite, 256)
		go func(queue chan metaWrite) {
			for write := range queue {
				m.procs.MergeMeta(write.key, write.meta)
			}
		}(m.metaQueue)
	}
	queue := m.metaQueue
	m.metaMu.Unlock()
	select {
	case queue <- metaWrite{key: key, meta: meta}:
	default:
		go m.procs.MergeMeta(key, meta)
	}
}

type metaWrite struct {
	key  string
	meta map[string]any
}

func (m *Module) ack(key string, offset int64) {
	if m.procs != nil && offset > 0 {
		m.procs.Ack(key, offset)
	}
}

func runtimeOf(meta map[string]any) string {
	parse, _ := meta["parse"].(string)
	switch parse {
	case RuntimeClaude, RuntimeCodex, RuntimeLog:
		return parse
	}
	return ""
}

func (m *Module) sessionDir(meta map[string]any) (string, int) {
	text, _ := meta["sessionId"].(string)
	sid, err := strconv.Atoi(text)
	if err != nil || sid <= 0 {
		return "", 0
	}
	writer := m.writer()
	if writer == nil {
		return "", sid
	}
	if key, _ := meta["storeKey"].(string); key != "" {
		if dir, ok := writer.DirFor(key, sid); ok {
			return dir, sid
		}
	}
	root := writer.Store().SessionsDir()
	entries, _ := os.ReadDir(root)
	for _, entry := range entries {
		candidate := filepath.Join(root, entry.Name(), text)
		if info, err := os.Stat(candidate); err == nil && info.IsDir() {
			return candidate, sid
		}
	}
	return "", sid
}

func (m *Module) consumer(key string, meta map[string]any, ack int64) (agentproc.Consumer, int64, bool) {
	runtime := runtimeOf(meta)
	if runtime == "" {
		return nil, 0, false
	}
	m.mu.Lock()
	ready := m.haveCreds
	existing := m.runs[key]
	m.mu.Unlock()
	if existing != nil {
		return existing, existing.next, true
	}
	if !ready {
		return nil, 0, false
	}
	dir, sid := m.sessionDir(meta)
	transient, _ := meta["transient"].(bool)
	if dir == "" && !transient {
		if m.core != nil {
			m.core.Logger.Printf("agentrun %s: no session directory for %v; the desktop parses it", key, meta["sessionId"])
		}
		return nil, 0, false
	}
	from := ack
	if dir != "" {
		if writer := m.writer(); writer != nil {
			writer.FlushDir(dir)
		}
		from = max(ack, lastStoredOffset(filepath.Join(dir, "events.jsonl"), key))
	}
	r := newRun(m, key, runtime, sid, dir, meta, from)
	m.mu.Lock()
	m.runs[key] = r
	m.mu.Unlock()
	if r.codex != nil {
		r.mu.Lock()
		if from > 0 {
			r.codex.fresh = false
		}
		r.codex.start()
		r.mu.Unlock()
	}
	if m.core != nil {
		if dir == "" {
			m.core.Logger.Printf("agentrun %s: parsing %s output for a transient run from byte %d", key, runtime, from)
		} else {
			m.core.Logger.Printf("agentrun %s: parsing %s output for session %d from byte %d", key, runtime, sid, from)
		}
	}
	return r, from, true
}

func (m *Module) lookup(key string) *run {
	m.mu.Lock()
	defer m.mu.Unlock()
	return m.runs[key]
}

func (m *Module) handleCredentials(ctx context.Context, req *rpc.Request) (any, error) {
	var params struct {
		Values []string `json:"values"`
	}
	if err := req.Bind(&params); err != nil {
		return nil, err
	}
	values := []string{}
	seen := map[string]bool{}
	for _, value := range params.Values {
		if value != "" && !seen[value] {
			seen[value] = true
			values = append(values, value)
		}
	}
	m.mu.Lock()
	m.credentials = values
	first := !m.haveCreds
	m.haveCreds = true
	m.mu.Unlock()
	if first && m.procs != nil {
		m.procs.SetConsumerFactory(m.consumer)
	}
	return map[string]any{"count": len(values)}, nil
}

func (m *Module) handleAttach(ctx context.Context, req *rpc.Request) (any, error) {
	var params struct {
		Key         string `json:"key"`
		After       int64  `json:"after"`
		Stderr      bool   `json:"stderr"`
		StderrAfter int64  `json:"stderrAfter"`
	}
	if err := req.Bind(&params); err != nil {
		return nil, err
	}
	r := m.lookup(params.Key)
	if r == nil {
		return nil, rpc.Fail("not_parsed")
	}
	return r.subscribe(req.Peer, subscribeOptions{after: params.After, stderr: params.Stderr,
		stderrAfter: params.StderrAfter}), nil
}

func (m *Module) handleDetach(ctx context.Context, req *rpc.Request) (any, error) {
	var params struct {
		Key string `json:"key"`
	}
	if err := req.Bind(&params); err != nil {
		return nil, err
	}
	r := m.lookup(params.Key)
	if r == nil {
		return nil, rpc.Fail("not_parsed")
	}
	return map[string]any{"detached": r.unsubscribe(req.Peer)}, nil
}

func (m *Module) handleList(ctx context.Context, req *rpc.Request) (any, error) {
	m.mu.Lock()
	runs := make([]*run, 0, len(m.runs))
	for _, r := range m.runs {
		runs = append(runs, r)
	}
	m.mu.Unlock()
	out := make([]map[string]any, 0, len(runs))
	for _, r := range runs {
		out = append(out, r.state())
	}
	return map[string]any{"runs": out, "credentials": m.haveCredentials()}, nil
}

func (m *Module) haveCredentials() bool {
	m.mu.Lock()
	defer m.mu.Unlock()
	return m.haveCreds
}

func (m *Module) SessionRuns(sid int) []map[string]any {
	m.mu.Lock()
	runs := []*run{}
	for _, r := range m.runs {
		if r.sid == sid {
			runs = append(runs, r)
		}
	}
	m.mu.Unlock()
	out := []map[string]any{}
	for _, r := range runs {
		out = append(out, r.state())
	}
	return out
}

func (m *Module) Stop() {
	m.mu.Lock()
	runs := make([]*run, 0, len(m.runs))
	for _, r := range m.runs {
		runs = append(runs, r)
	}
	m.mu.Unlock()
	for _, r := range runs {
		r.mu.Lock()
		for id, sub := range r.subscribers {
			delete(r.subscribers, id)
			close(sub.stop)
		}
		r.mu.Unlock()
	}
}
