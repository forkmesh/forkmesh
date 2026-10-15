package insights

import (
	"context"
	"encoding/json"
	"strconv"
	"sync"
	"time"

	"forkmesh.com/desktop-core/internal/agentproc"
	"forkmesh.com/desktop-core/internal/agents"
	"forkmesh.com/desktop-core/internal/qtjson"
	"forkmesh.com/desktop-core/internal/rpc"
)

const contextRefresh = 30 * time.Second

type feedPeer struct {
	id   string
	key  string
	f    *feed
	done chan struct{}
	once sync.Once
}

func (p *feedPeer) ID() string               { return p.id }
func (p *feedPeer) Transport() rpc.Transport { return rpc.CLI }
func (p *feedPeer) Device() rpc.Device       { return rpc.Device{Name: "forkmesh-core insights"} }
func (p *feedPeer) Done() <-chan struct{}    { return p.done }
func (p *feedPeer) close()                   { p.once.Do(func() { close(p.done) }) }

func (p *feedPeer) Send(event string, data any) bool {
	if event != "core.agentrun.event" {
		return true
	}
	payload, ok := data.(map[string]any)
	if !ok {
		return true
	}
	line, _ := payload["line"].(string)
	if line == "" {
		return true
	}
	so := int64(-1)
	switch value := payload["so"].(type) {
	case int64:
		so = value
	case int:
		so = int64(value)
	case float64:
		so = int64(value)
	}
	p.f.enqueue(feedItem{key: p.key, line: line, so: so})
	return true
}

type feedItem struct {
	key  string
	line string
	so   int64
}

type cachedContext struct {
	context SessionContext
	at      time.Time
}

type feed struct {
	m        *Module
	mu       sync.Mutex
	cond     *sync.Cond
	queue    []feedItem
	attached map[string]*feedPeer
	sessions map[string]int
	contexts map[string]cachedContext
	lister   *feedPeer
	closed   bool
}

func newFeed(m *Module) *feed {
	f := &feed{m: m, attached: map[string]*feedPeer{}, sessions: map[string]int{}, contexts: map[string]cachedContext{}}
	f.cond = sync.NewCond(&f.mu)
	f.lister = &feedPeer{id: "insights:list", f: f, done: make(chan struct{})}
	return f
}

func (f *feed) enqueue(item feedItem) {
	f.mu.Lock()
	if !f.closed {
		f.queue = append(f.queue, item)
		f.cond.Signal()
	}
	f.mu.Unlock()
}

func (f *feed) run(ctx context.Context) {
	go f.work()
	every := f.m.FeedEvery
	if every <= 0 {
		every = time.Second
	}
	ticker := time.NewTicker(every)
	defer ticker.Stop()
	f.scan(ctx)
	for {
		select {
		case <-ctx.Done():
			f.shutdown()
			return
		case <-ticker.C:
			f.scan(ctx)
		}
	}
}

func (f *feed) shutdown() {
	f.mu.Lock()
	f.closed = true
	peers := f.attached
	f.attached = map[string]*feedPeer{}
	f.cond.Broadcast()
	f.mu.Unlock()
	for _, peer := range peers {
		peer.close()
	}
	f.lister.close()
}

func (f *feed) call(ctx context.Context, method string, peer rpc.Peer, params any) (any, error) {
	core := f.m.core
	if core == nil || core.Registry == nil {
		return nil, rpc.ErrUnsupported
	}
	handler, ok := core.Registry.Resolve(method)
	if !ok {
		return nil, rpc.ErrUnsupported
	}
	raw, _ := json.Marshal(params)
	return handler(ctx, &rpc.Request{Peer: peer, Method: method, Params: raw})
}

func runsOf(result any) []map[string]any {
	object, ok := result.(map[string]any)
	if !ok {
		return nil
	}
	switch runs := object["runs"].(type) {
	case []map[string]any:
		return runs
	case []any:
		out := []map[string]any{}
		for _, item := range runs {
			if run, ok := item.(map[string]any); ok {
				out = append(out, run)
			}
		}
		return out
	}
	return nil
}

func intOf(value any) int {
	switch v := value.(type) {
	case int:
		return v
	case int64:
		return int(v)
	case float64:
		return int(v)
	case string:
		n, _ := strconv.Atoi(v)
		return n
	}
	return 0
}

func (f *feed) scan(ctx context.Context) {
	result, err := f.call(ctx, "core.agentrun.list", f.lister, map[string]any{})
	if err != nil {
		return
	}
	present := map[string]bool{}
	for _, run := range runsOf(result) {
		key, _ := run["key"].(string)
		if key == "" {
			continue
		}
		present[key] = true
		f.mu.Lock()
		_, known := f.attached[key]
		f.mu.Unlock()
		if known {
			continue
		}
		peer := &feedPeer{id: "insights:" + key, key: key, f: f, done: make(chan struct{})}
		f.mu.Lock()
		f.sessions[key] = intOf(run["sessionId"])
		f.mu.Unlock()
		after := f.m.offsetFor(key)
		if _, err := f.call(ctx, "core.agentrun.attach", peer, map[string]any{"key": key, "after": after}); err != nil {
			continue
		}
		f.mu.Lock()
		f.attached[key] = peer
		f.mu.Unlock()
	}
	f.mu.Lock()
	stale := []*feedPeer{}
	for key, peer := range f.attached {
		if !present[key] {
			stale = append(stale, peer)
			delete(f.attached, key)
			delete(f.sessions, key)
			delete(f.contexts, key)
		}
	}
	f.mu.Unlock()
	for _, peer := range stale {
		peer.close()
	}
}

func (f *feed) work() {
	for {
		f.mu.Lock()
		for len(f.queue) == 0 && !f.closed {
			f.cond.Wait()
		}
		if f.closed && len(f.queue) == 0 {
			f.mu.Unlock()
			return
		}
		item := f.queue[0]
		f.queue = f.queue[1:]
		f.mu.Unlock()
		f.handle(item)
	}
}

func (f *feed) handle(item feedItem) {
	event, ok := qtjson.ParseObject([]byte(item.line))
	if !ok {
		return
	}
	kind := event.Get("type").Str()
	if kind != "assistant" && kind != "user" && kind != "result" && kind != "_codex_agent_complete" {
		f.m.noteOffset(item.key, item.so)
		return
	}
	context := f.contextFor(item.key)
	f.m.ingestFeed(item.key, item.so, context, event)
}

func (f *feed) contextFor(key string) SessionContext {
	f.mu.Lock()
	cached, ok := f.contexts[key]
	sid := f.sessions[key]
	f.mu.Unlock()
	if ok && time.Since(cached.at) < contextRefresh {
		return cached.context
	}
	context := SessionContext{SessionID: sid}
	if core := f.m.core; core != nil {
		if store, ok := core.Module("agents").(*agents.Module); ok && store.Store() != nil {
			if session, found := store.Store().FindSession(sid); found {
				context.Provider = session.Provider
				context.Model = session.Model
			}
		}
		if procs, ok := core.Module("agentproc").(*agentproc.Module); ok {
			if info, found := procs.Info(key); found {
				if cwd, _ := info["cwd"].(string); cwd != "" {
					context.Workdir = cwd
				}
			}
			if context.Workdir == "" {
				for _, process := range procs.Processes() {
					if process.Key == key {
						context.Workdir = process.Cwd
						break
					}
				}
			}
		}
	}
	f.mu.Lock()
	f.contexts[key] = cachedContext{context: context, at: time.Now()}
	f.mu.Unlock()
	return context
}

func (m *Module) offsetFor(key string) int64 {
	m.mu.Lock()
	defer m.mu.Unlock()
	return m.offsets[key]
}

func (m *Module) noteOffset(key string, so int64) {
	if so <= 0 {
		return
	}
	m.mu.Lock()
	if so > m.offsets[key] {
		m.offsets[key] = so
	}
	m.mu.Unlock()
}

func (m *Module) ingestFeed(key string, so int64, context SessionContext, event qtjson.Value) {
	m.mu.Lock()
	defer m.mu.Unlock()
	if so > 0 {
		if so <= m.offsets[key] {
			return
		}
		m.offsets[key] = so
	}
	m.ingestLocked(context, event)
}
