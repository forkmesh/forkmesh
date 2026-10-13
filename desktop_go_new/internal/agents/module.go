package agents

import (
	"context"
	"io"
	"os"
	"path/filepath"
	"runtime"
	"sort"
	"sync"
	"unicode/utf8"

	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/rpc"
)

const (
	DefaultReadLimit = 4 << 20
	MaxReadLimit     = 32 << 20
	searchWorkers    = 8
)

type Module struct {
	mu     sync.Mutex
	store  *Store
	writer *Writer
	core   *daemon.Core
}

func New() *Module { return &Module{} }

func (m *Module) Name() string { return "agents" }

func (m *Module) Store() *Store {
	m.mu.Lock()
	defer m.mu.Unlock()
	return m.store
}

func (m *Module) Writer() *Writer {
	m.mu.Lock()
	defer m.mu.Unlock()
	return m.writer
}

func (m *Module) Start(ctx context.Context, core *daemon.Core) error {
	m.mu.Lock()
	m.core = core
	m.store = NewStore(filepath.Join(core.Layout.Data, "agents"))
	m.writer = NewWriter(m.store)
	m.mu.Unlock()
	m.registerWriter(core)
	core.Registry.Handle("core.agents.sessions", m.Name(), m.local(m.handleSessions))
	core.Registry.Handle("core.agents.transcript", m.Name(), m.local(m.handleTranscript))
	core.Registry.Handle("core.agents.events", m.Name(), m.local(m.handleEvents))
	core.Registry.Handle("core.agents.search", m.Name(), m.local(m.handleSearch))
	return nil
}

func (m *Module) local(handler rpc.Handler) rpc.Handler {
	return func(ctx context.Context, req *rpc.Request) (any, error) {
		if req.Peer == nil || rpc.Remote(req.Peer) {
			return nil, rpc.Fail("forbidden")
		}
		return handler(ctx, req)
	}
}

func sessionObjects(sessions []Session, recycled bool) []map[string]any {
	out := make([]map[string]any, 0, len(sessions))
	for _, session := range sessions {
		obj := session.Object()
		if recycled {
			obj["deletedAtMs"] = session.DeletedAtMs
		}
		out = append(out, obj)
	}
	return out
}

func (m *Module) handleSessions(ctx context.Context, req *rpc.Request) (any, error) {
	var params struct {
		Recycled bool `json:"recycled"`
		Outcomes bool `json:"outcomes"`
	}
	if err := req.Bind(&params); err != nil {
		return nil, err
	}
	store := m.Store()
	sessions := store.LoadAllSessions()
	result := map[string]any{
		"root":     store.Root,
		"sessions": sessionObjects(sessions, false),
		"queued":   QueuedSessionIDsOldestFirst(sessions),
	}
	if params.Recycled {
		result["recycled"] = sessionObjects(store.LoadRecycledSessions(), true)
	}
	if params.Outcomes {
		outcomes := []ModelOutcome{}
		for _, outcome := range store.RetiredModelOutcomes() {
			outcomes = append(outcomes, outcome)
		}
		sortOutcomes(outcomes)
		result["outcomes"] = outcomes
	}
	return result, nil
}

func sortOutcomes(outcomes []ModelOutcome) {
	sort.Slice(outcomes, func(i, j int) bool {
		if outcomes[i].Provider != outcomes[j].Provider {
			return outcomes[i].Provider < outcomes[j].Provider
		}
		return outcomes[i].Model < outcomes[j].Model
	})
}

type readParams struct {
	ID     int    `json:"id"`
	Offset *int64 `json:"offset"`
	Limit  int64  `json:"limit"`
}

func clampLimit(limit int64) int64 {
	if limit <= 0 {
		return DefaultReadLimit
	}
	return min(limit, MaxReadLimit)
}

func completeUTF8Prefix(data []byte) int {
	size := len(data)
	for back := 1; back <= 4 && back <= size; back++ {
		c := data[size-back]
		if c&0xC0 == 0x80 {
			continue
		}
		needed := 1
		switch {
		case c&0xE0 == 0xC0:
			needed = 2
		case c&0xF0 == 0xE0:
			needed = 3
		case c&0xF8 == 0xF0:
			needed = 4
		}
		if needed > back {
			return size - back
		}
		return size
	}
	return size
}

func leadingContinuation(data []byte) int {
	skip := 0
	for skip < len(data) && skip < 3 && !utf8.RuneStart(data[skip]) {
		skip++
	}
	return skip
}

func (m *Module) handleTranscript(ctx context.Context, req *rpc.Request) (any, error) {
	var params readParams
	if err := req.Bind(&params); err != nil {
		return nil, err
	}
	store := m.Store()
	session, ok := store.FindSession(params.ID)
	if !ok {
		return nil, rpc.Fail("unknown_session")
	}
	if writer := m.Writer(); writer != nil {
		writer.FlushDir(store.SessionDir(session))
	}
	path := store.LogPath(session)
	limit := clampLimit(params.Limit)
	result := map[string]any{"id": session.ID, "text": "", "from": int64(0), "next": int64(0),
		"size": int64(0), "more": false, "reset": false, "stamp": store.TranscriptStamp(session)}
	file, err := os.Open(path)
	if err != nil {
		return result, nil
	}
	defer file.Close()
	info, err := file.Stat()
	if err != nil {
		return result, nil
	}
	size := info.Size()
	from := int64(0)
	reset := false
	tail := false
	if params.Offset != nil {
		from = *params.Offset
	}
	switch {
	case from < 0:
		tail = true
		from = max(0, size-limit)
	case from > size:
		reset = true
		from = 0
	}
	if _, err := file.Seek(from, io.SeekStart); err != nil {
		return result, nil
	}
	data := make([]byte, min(limit, size-from))
	n, _ := io.ReadFull(file, data)
	data = data[:n]
	if tail || reset || from > 0 {
		skip := leadingContinuation(data)
		data = data[skip:]
		from += int64(skip)
	}
	data = data[:completeUTF8Prefix(data)]
	next := from + int64(len(data))
	result["text"] = string(stripCR(data))
	result["from"] = from
	result["next"] = next
	result["size"] = size
	result["more"] = next < size
	result["reset"] = reset
	return result, nil
}

func (m *Module) handleEvents(ctx context.Context, req *rpc.Request) (any, error) {
	var params readParams
	if err := req.Bind(&params); err != nil {
		return nil, err
	}
	store := m.Store()
	session, ok := store.FindSession(params.ID)
	if !ok {
		return nil, rpc.Fail("unknown_session")
	}
	offset := int64(0)
	if params.Offset != nil {
		offset = *params.Offset
	}
	if writer := m.Writer(); writer != nil {
		writer.FlushDir(store.SessionDir(session))
	}
	path := store.EventsPath(session)
	reset := false
	if info, err := os.Stat(path); err == nil && (offset < 0 || offset > info.Size()) {
		reset = true
		offset = 0
	}
	events, next, more := readEvents(path, offset, clampLimit(params.Limit))
	return map[string]any{"id": session.ID, "events": events, "from": offset, "next": next,
		"more": more, "reset": reset, "stamp": store.TranscriptStamp(session)}, nil
}

func (m *Module) handleSearch(ctx context.Context, req *rpc.Request) (any, error) {
	var params struct {
		Needle string `json:"needle"`
		IDs    []int  `json:"ids"`
	}
	if err := req.Bind(&params); err != nil {
		return nil, err
	}
	store := m.Store()
	if writer := m.Writer(); writer != nil {
		writer.Flush()
	}
	query := trimmed(params.Needle)
	hits := []SearchHit{}
	if query == "" {
		return map[string]any{"needle": query, "hits": hits, "searched": 0}, nil
	}
	sessions := store.LoadAllSessions()
	if params.IDs != nil {
		wanted := map[int]bool{}
		for _, id := range params.IDs {
			wanted[id] = true
		}
		filtered := sessions[:0]
		for _, session := range sessions {
			if wanted[session.ID] {
				filtered = append(filtered, session)
			}
		}
		sessions = filtered
	}
	found := store.SearchMany(ctx, sessions, query)
	for i, hit := range found {
		if hit.Count > 0 {
			hit.ID = sessions[i].ID
			hits = append(hits, hit)
		}
	}
	if err := ctx.Err(); err != nil {
		return nil, rpc.Fail("cancelled")
	}
	return map[string]any{"needle": query, "hits": hits, "searched": len(sessions)}, nil
}

func (s *Store) SearchMany(ctx context.Context, sessions []Session, needle string) []SearchHit {
	results := make([]SearchHit, len(sessions))
	workers := min(searchWorkers, max(1, runtime.NumCPU()), max(1, len(sessions)))
	jobs := make(chan int)
	var wg sync.WaitGroup
	for w := 0; w < workers; w++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for i := range jobs {
				count, snippet := s.SearchTranscript(sessions[i], needle)
				results[i] = SearchHit{ID: sessions[i].ID, Count: count, Snippet: snippet}
			}
		}()
	}
	for i := range sessions {
		if ctx.Err() != nil {
			break
		}
		jobs <- i
	}
	close(jobs)
	wg.Wait()
	return results
}
