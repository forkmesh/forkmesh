package agentmodel

import (
	"context"
	"encoding/json"
	"io"
	"os"
	"sync"
	"time"

	"forkmesh.com/desktop-core/internal/agentrun"
	"forkmesh.com/desktop-core/internal/agents"
	"forkmesh.com/desktop-core/internal/control"
	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/qtjson"
	"forkmesh.com/desktop-core/internal/relay"
	"forkmesh.com/desktop-core/internal/rpc"
)

const (
	maxTranscriptChars       = 64000
	maxTranscriptDeltaChars  = 16000
	maxFollowsPerConnection  = 8
	maxAttachmentEntries     = 50
	outsideFollowWatchPeriod = 200 * time.Millisecond
)

type logTail struct {
	bytes int64
	chars int64
}

type follow struct {
	peer     rpc.Peer
	conn     string
	session  int
	next     int64
	status   string
	active   bool
	busy     bool
	dirty    bool
	readAt   time.Time
	seenSize int64
	tail     logTail
}

type outsideTail struct {
	path     string
	kind     string
	offset   int64
	seenSize int64
	base     int64
	log      []uint16
}

type followPump struct {
	m       *Module
	mu      sync.Mutex
	list    []*follow
	timer   *time.Timer
	watch   *time.Ticker
	watched chan struct{}
	tails   map[int]outsideTail
	peers   map[string]bool
	closed  bool
}

type readResult struct {
	payload qtjson.Value
	err     string
	tail    logTail
}

func newFollowPump(m *Module) *followPump {
	return &followPump{m: m, tails: map[int]outsideTail{}, peers: map[string]bool{}}
}

func relayBacklog(core *daemon.Core) func() int {
	return func() int {
		if core == nil {
			return 0
		}
		if module, ok := core.Module("relay").(*relay.Module); ok {
			if client := module.Client(); client != nil {
				return client.ControlBacklog()
			}
		}
		return 0
	}
}

func (m *Module) registerFollow(core *daemon.Core) {
	core.Registry.Handle("agents.follow", m.Name(), m.handleFollow)
	core.Registry.Handle("agents.unfollow", m.Name(), m.handleFollow)
}

func (p *followPump) count() int {
	p.mu.Lock()
	defer p.mu.Unlock()
	return len(p.list)
}

func (p *followPump) stop() {
	p.mu.Lock()
	p.closed = true
	if p.timer != nil {
		p.timer.Stop()
	}
	if p.watch != nil {
		p.watch.Stop()
		close(p.watched)
		p.watch = nil
	}
	p.list = nil
	p.mu.Unlock()
}

func (p *followPump) find(conn string, session int) *follow {
	for _, f := range p.list {
		if f.conn == conn && f.session == session {
			return f
		}
	}
	return nil
}

func (p *followPump) remove(match func(*follow) bool) {
	kept := p.list[:0]
	for _, f := range p.list {
		if !match(f) {
			kept = append(kept, f)
		}
	}
	for i := len(kept); i < len(p.list); i++ {
		p.list[i] = nil
	}
	p.list = kept
}

func (p *followPump) dropConnection(conn string) {
	p.mu.Lock()
	p.remove(func(f *follow) bool { return f.conn == conn })
	p.mu.Unlock()
}

func (p *followPump) markDirty(session int) {
	p.mu.Lock()
	any := false
	for _, f := range p.list {
		if f.session == session {
			f.dirty = true
			any = true
		}
	}
	p.mu.Unlock()
	if any {
		p.schedule(0)
	}
}

func (p *followPump) markAllDirty() {
	p.mu.Lock()
	if len(p.list) == 0 {
		p.mu.Unlock()
		return
	}
	for _, f := range p.list {
		f.dirty = true
	}
	p.mu.Unlock()
	p.schedule(0)
}

func (p *followPump) schedule(delay time.Duration) {
	p.mu.Lock()
	defer p.mu.Unlock()
	if p.closed || p.timer != nil {
		return
	}
	p.timer = time.AfterFunc(delay, func() {
		p.mu.Lock()
		p.timer = nil
		p.mu.Unlock()
		p.pump()
	})
}

func (p *followPump) pump() {
	now := p.m.now()
	wait := time.Duration(-1)
	later := func(d time.Duration) {
		if wait < 0 || d < wait {
			wait = d
		}
	}
	type due struct {
		conn    string
		session int
	}
	var ready []due
	p.mu.Lock()
	if p.closed {
		p.mu.Unlock()
		return
	}
	for _, f := range p.list {
		if !f.dirty || f.busy {
			continue
		}
		relayed := f.peer.Transport() == rpc.Relay
		interval := time.Duration(control.FollowIntervalMs(relayed)) * time.Millisecond
		if remaining := f.readAt.Add(interval).Sub(now); remaining > 0 {
			later(remaining)
			continue
		}
		if relayed && p.m.backlog != nil && p.m.backlog() > 0 {
			later(interval)
			continue
		}
		f.dirty = false
		f.busy = true
		f.readAt = now
		ready = append(ready, due{f.conn, f.session})
	}
	p.mu.Unlock()
	for _, item := range ready {
		go p.read(item.conn, item.session, nil)
	}
	if wait >= 0 {
		p.schedule(wait)
	}
}

func (p *followPump) watchOutside() {
	p.mu.Lock()
	defer p.mu.Unlock()
	if p.closed || p.watch != nil {
		return
	}
	p.watch = time.NewTicker(outsideFollowWatchPeriod)
	p.watched = make(chan struct{})
	ticker, done := p.watch, p.watched
	go func() {
		for {
			select {
			case <-done:
				return
			case <-ticker.C:
				if !p.checkOutside() {
					return
				}
			}
		}
	}()
}

func (p *followPump) checkOutside() bool {
	snapshot, _ := p.m.snapshot()
	p.mu.Lock()
	watching := false
	grew := false
	for _, f := range p.list {
		if f.session >= 0 {
			continue
		}
		watching = true
		size := int64(-2)
		if source, ok := snapshot.OutsideSources[f.session]; ok && source.Transcript && source.Path != "" {
			size = fileSize(source.Path)
		}
		if size != f.seenSize {
			f.seenSize = size
			f.dirty = true
			grew = true
		}
	}
	if !watching && p.watch != nil {
		p.watch.Stop()
		close(p.watched)
		p.watch = nil
	}
	p.mu.Unlock()
	if grew {
		p.schedule(0)
	}
	return watching
}

func (m *Module) handleFollow(ctx context.Context, req *rpc.Request) (any, error) {
	if req.Peer == nil {
		return nil, rpc.Fail("forbidden")
	}
	params := paramsObject(req)
	session := params.Get("id").Int()
	conn := req.Peer.ID()
	p := m.follows
	if req.Method == "agents.unfollow" {
		p.mu.Lock()
		p.remove(func(f *follow) bool { return f.conn == conn && f.session == session })
		p.mu.Unlock()
		return map[string]any{}, nil
	}
	if session == 0 {
		return nil, rpc.Fail("unknown_session")
	}
	if !m.followServable(session) {
		return nil, rpc.ErrDelegate
	}
	since := max(int64(0), int64(params.Get("since").Float()))
	p.mu.Lock()
	p.remove(func(f *follow) bool { return f.conn == conn && f.session == session })
	held := 0
	for _, f := range p.list {
		if f.conn == conn {
			held++
		}
	}
	if held >= maxFollowsPerConnection {
		for i, f := range p.list {
			if f.conn == conn && !f.busy {
				p.list = append(p.list[:i], p.list[i+1:]...)
				break
			}
		}
	}
	first := !p.peers[conn]
	p.peers[conn] = true
	p.list = append(p.list, &follow{peer: req.Peer, conn: conn, session: session, next: since, busy: true,
		readAt: m.now(), seenSize: -1})
	p.mu.Unlock()
	if first {
		go p.watchPeer(req.Peer)
	}
	if session < 0 {
		p.watchOutside()
	}
	answer := make(chan readResult, 1)
	p.read(conn, session, answer)
	result := <-answer
	if result.err != "" {
		return nil, rpc.Fail(result.err)
	}
	return json.RawMessage(result.payload.Compact()), nil
}

func (p *followPump) watchPeer(peer rpc.Peer) {
	<-peer.Done()
	p.dropConnection(peer.ID())
	p.mu.Lock()
	delete(p.peers, peer.ID())
	p.mu.Unlock()
}

func (m *Module) followServable(session int) bool {
	_, pushed := m.snapshot()
	_, credentialed := m.credentials()
	return pushed && credentialed && m.writer() != nil
}

func (p *followPump) read(conn string, session int, answer chan<- readResult) {
	p.mu.Lock()
	since := int64(0)
	tail := logTail{}
	for _, f := range p.list {
		if f.conn == conn && f.session == session {
			since = f.next
			tail = f.tail
		}
	}
	p.mu.Unlock()
	var result readResult
	if session < 0 {
		result = p.m.readOutside(session, since)
	} else {
		result = p.m.readOwn(session, since, tail)
	}
	p.finish(conn, session, answer, result)
}

func (p *followPump) finish(conn string, session int, answer chan<- readResult, result readResult) {
	p.mu.Lock()
	f := p.find(conn, session)
	if result.err != "" {
		if f != nil {
			p.remove(func(other *follow) bool { return other == f })
		}
		var peer rpc.Peer
		if f != nil {
			peer = f.peer
		}
		p.mu.Unlock()
		if answer != nil {
			answer <- result
		} else if peer != nil {
			peer.Send("agents.output", json.RawMessage(qtjson.ObjectOf("id", session, "error", result.err).Compact()))
		}
		return
	}
	if f == nil {
		p.mu.Unlock()
		if answer != nil {
			answer <- result
		}
		return
	}
	f.busy = false
	f.tail = result.tail
	payload := result.payload
	next := int64(payload.Get("next").Float())
	status := payload.Get("status").Str()
	active := payload.Get("active").BoolValue()
	changed := answer != nil || payload.Get("reset").BoolValue() || next != f.next || status != f.status ||
		active != f.active
	f.next = next
	f.status = status
	f.active = active
	dirty := f.dirty
	peer := f.peer
	p.mu.Unlock()
	if answer != nil {
		payload.Set("streaming", qtjson.NewBool(true))
		answer <- readResult{payload: payload}
	} else if changed && !peer.Send("agents.output", json.RawMessage(payload.Compact())) {
		p.dropConnection(conn)
		return
	}
	if dirty {
		p.schedule(0)
	}
}

func readLogGrowth(path string, tail *logTail) ([]uint16, bool) {
	rewritten := false
	file, err := os.Open(path)
	if err != nil {
		if tail.bytes > 0 {
			rewritten = true
		}
		*tail = logTail{}
		return nil, rewritten
	}
	defer file.Close()
	info, err := file.Stat()
	if err != nil {
		return nil, rewritten
	}
	if info.Size() < tail.bytes {
		*tail = logTail{}
		rewritten = true
	}
	if _, err := file.Seek(tail.bytes, io.SeekStart); err != nil {
		return nil, rewritten
	}
	data, _ := io.ReadAll(file)
	keep := completeUTF8Prefix(data)
	data = data[:keep]
	tail.bytes += int64(keep)
	text := qtDecodeUTF8(withoutCR(data))
	tail.chars += int64(len(text))
	return text, rewritten
}

func (m *Module) ownSession(id int) (agents.Session, *agents.Store, bool) {
	writer := m.writer()
	snapshot, _ := m.snapshot()
	if id <= 0 || snapshot.external(id) || writer == nil {
		return agents.Session{}, nil, false
	}
	session, _, ok := m.findSession(id)
	if !ok || session.DeletedAtMs != 0 {
		return agents.Session{}, nil, false
	}
	return session, writer.Store(), true
}

func (m *Module) knownImages(id int) []string {
	snapshot, _ := m.snapshot()
	return snapshot.Live[id].Images
}

func (m *Module) readOwn(id int, since int64, tail logTail) readResult {
	session, store, ok := m.ownSession(id)
	if !ok {
		return readResult{err: "unknown_session"}
	}
	active := sessionActive(session.Status)
	status := session.Status
	known := m.knownImages(id)
	if writer := m.writer(); writer != nil {
		writer.FlushDir(store.SessionDir(session))
	}
	path := store.LogPath(session)
	var text []uint16
	base := int64(0)
	if tail.bytes > 0 && since == tail.chars {
		var rewritten bool
		text, rewritten = readLogGrowth(path, &tail)
		if !rewritten {
			base = since
		}
	} else {
		tail = logTail{}
		text, _ = readLogGrowth(path, &tail)
	}
	listed := base == 0 || len(agents.AttachmentPathsIn(fromU16(text))) > 0
	total := tail.chars
	from := base
	reset := false
	if since >= base && since <= total {
		from = since
		if total-from > maxTranscriptChars {
			from = total - maxTranscriptChars
			reset = true
		}
	} else {
		reset = true
		from = max(base, total-maxTranscriptChars)
	}
	credentials, _ := m.credentials()
	payload := qtjson.ObjectOf(
		"id", id,
		"transcript", agentrun.RedactText(fromU16(text[from-base:]), credentials),
		"from", float64(from),
		"next", float64(total),
		"reset", reset,
		"active", active,
		"status", status,
	)
	if listed {
		payload.Set("attachments", attachmentsJSON(m.attachmentEntries(
			m.sessionAttachmentPaths(store, session, known), id, maxAttachmentEntries)))
	}
	return readResult{payload: payload, tail: tail}
}

func (m *Module) readOutside(id int, since int64) readResult {
	snapshot, _ := m.snapshot()
	source, ok := snapshot.OutsideSources[id]
	if ok && source.Terminal {
		return readResult{err: "no_transcript"}
	}
	if !ok || !source.Transcript {
		return readResult{err: "unknown_session"}
	}
	if source.Path == "" || !readsTranscript(source.Kind) {
		return readResult{err: "no_transcript"}
	}
	header := toU16("==> " + outsideLabel(source.Kind) + " started outside ForkMesh, in " + source.SourceCwd + "\n")
	p := m.follows
	p.mu.Lock()
	t, kept := p.tails[id]
	p.mu.Unlock()
	fresh := !kept || t.path != source.Path
	if fresh {
		t = outsideTail{path: source.Path, kind: source.Kind, offset: -1, log: append([]uint16(nil), header...)}
	} else {
		t.log = append([]uint16(nil), t.log...)
	}
	rebuilt := false
	if t.offset > fileSize(t.path) {
		t = outsideTail{path: t.path, kind: t.kind, offset: -1, log: append([]uint16(nil), header...)}
		rebuilt = true
	}
	if t.offset < 0 {
		t.offset = tailStartOffset(t.path, maxOutsideTranscriptBytes)
	}
	entries, next := readJSONLines(t.path, t.offset)
	for _, entry := range entries {
		t.log = append(t.log, toU16(outsideEntry(t.kind, entry))...)
	}
	t.offset = next
	t.seenSize = fileSize(t.path)
	if len(t.log) > 4*maxTranscriptChars {
		drop := len(t.log) - 2*maxTranscriptChars
		t.log = append([]uint16(nil), t.log[drop:]...)
		t.base += int64(drop)
	}
	p.mu.Lock()
	if stored, ok := p.tails[id]; !ok || stored.path != t.path || stored.offset <= t.offset {
		p.tails[id] = t
	}
	p.mu.Unlock()
	total := t.base + int64(len(t.log))
	restarted := (fresh || rebuilt) && since > 0
	from := t.base
	reset := false
	if since >= 0 && since <= total && !restarted {
		from = max(since, total-maxTranscriptDeltaChars)
	} else {
		reset = since >= 0
		from = max(t.base, total-maxTranscriptChars)
	}
	credentials, _ := m.credentials()
	payload := qtjson.ObjectOf(
		"id", id,
		"transcript", agentrun.RedactText(fromU16(t.log[from-t.base:]), credentials),
		"from", float64(from),
		"next", float64(total),
		"reset", reset,
		"active", true,
		"status", "running",
		"outside", true,
		"attachments", qtjson.NewArray(),
	)
	return readResult{payload: payload}
}
