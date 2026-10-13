package agentrun

import (
	"bytes"
	"path/filepath"
	"strconv"
	"sync"
	"time"

	"forkmesh.com/desktop-core/internal/qtjson"
	"forkmesh.com/desktop-core/internal/rpc"
)

const (
	RuntimeClaude = "claude"
	ackInterval   = 250 * time.Millisecond
	rawViewLimit  = 300000
)

type subscriber struct {
	peer   rpc.Peer
	stop   chan struct{}
	stderr bool
}

type usage struct {
	NumTurns     int     `json:"numTurns"`
	DurationMs   int64   `json:"durationMs"`
	CostUsd      float64 `json:"costUsd"`
	OutputTokens int64   `json:"outputTokens"`
	InputTokens  int64   `json:"inputTokens"`
}

type run struct {
	m       *Module
	key     string
	runtime string
	sid     int
	dir     string
	account string

	mu                 sync.Mutex
	framer             *Framer
	next               int64
	skipUntil          int64
	delivered          int64
	conversationOpened bool
	launchedMode       string
	permissionMode     string
	pendingID          string
	pendingMode        string
	lastSessionID      string
	idle               bool
	waiting            bool
	usage              usage
	seenMessages       map[string]int64
	subscribers        map[string]*subscriber
	exit               map[string]any
	exitPublished      bool
	ackPending         bool
	acked              int64
	codex              *codexState
	errNext            int64
	errSent            int64
	errAcked           int64
	errCarry           []byte
	memory             []storedEvent
	secrets            []string
	logCarry           []byte
}

func newRun(m *Module, key, runtime string, sid int, dir string, meta map[string]any, from int64) *run {
	r := &run{m: m, key: key, runtime: runtime, sid: sid, dir: dir, framer: NewFramer(MaxFrameBytes),
		next: from, skipUntil: from, delivered: from, acked: from, subscribers: map[string]*subscriber{},
		seenMessages: map[string]int64{}}
	r.account, _ = meta["accountId"].(string)
	r.launchedMode, _ = meta["launchedPermissionMode"].(string)
	r.permissionMode, _ = meta["permissionMode"].(string)
	r.conversationOpened, _ = meta["conversationOpened"].(bool)
	r.lastSessionID, _ = meta["lastSessionId"].(string)
	if runtime == RuntimeCodex {
		r.codex = newCodexState(r, meta)
	}
	r.secrets = runSecrets(meta)
	return r
}

func (r *run) eventsPath() string { return filepath.Join(r.dir, "events.jsonl") }

func isWhitespaceOnly(line []byte) bool {
	for _, c := range line {
		if c != ' ' && c != '\t' && c != '\r' {
			return false
		}
	}
	return true
}

func (r *run) Stdout(data []byte, offset int64) {
	r.mu.Lock()
	defer r.mu.Unlock()
	if offset > r.next {
		r.framer.Reset()
	} else if offset < r.next {
		skip := r.next - offset
		if skip >= int64(len(data)) {
			return
		}
		data = data[skip:]
		offset = r.next
	}
	r.next = offset + int64(len(data))
	if r.codex != nil {
		r.codex.ingest(data, offset)
		r.scheduleAck()
		return
	}
	if r.runtime == RuntimeLog {
		r.logLocked(data, offset)
		r.scheduleAck()
		return
	}
	from := 0
	for from < len(data) {
		newline := bytes.IndexByte(data[from:], '\n')
		to := len(data)
		if newline >= 0 {
			to = from + newline + 1
		}
		frames, errs := r.framer.Append(data[from:to], false)
		end := offset + int64(to)
		for _, message := range errs {
			r.emitLocked(qtjson.ObjectOf("type", "_runtime_protocol_error", "message", message), -1, message)
		}
		for _, frame := range frames {
			if len(frame) == 0 || isWhitespaceOnly(frame) {
				continue
			}
			r.lineLocked(frame, end)
		}
		from = to
	}
	r.scheduleAck()
}

func opensConversation(event qtjson.Value) bool {
	if event.Get("type").Str() != "system" {
		return false
	}
	subtype := event.Get("subtype").Str()
	return subtype == "init" ||
		(subtype == "" && event.Get("session_id").Str() != "" && event.Get("error").IsUndefined())
}

func (r *run) lineLocked(frame []byte, end int64) {
	if end <= r.skipUntil {
		return
	}
	event, ok := qtjson.ParseObject(frame)
	if !ok {
		r.sendLocked(map[string]any{"key": r.key, "so": end, "raw": rawView(frame)})
		r.delivered = max(r.delivered, end)
		return
	}
	r.observeLocked(event)
	event.Set("_so", qtjson.NewDouble(float64(end)))
	event.Set("_sk", qtjson.NewString(r.key))
	r.emitLocked(event, end, "")
}

func rawView(line []byte) string {
	if len(line) <= rawViewLimit {
		return string(line)
	}
	return "[stream-json line omitted from raw view: " + strconv.Itoa(len(line)) + " bytes; parsed event retained]"
}

func (r *run) observeLocked(event qtjson.Value) {
	kind := event.Get("type").Str()
	if opensConversation(event) && !r.conversationOpened {
		r.conversationOpened = true
		r.m.mergeMeta(r.key, map[string]any{"conversationOpened": true})
	}
	if kind == "control_response" && r.pendingID != "" {
		response := event.Get("response")
		if response.Get("request_id").Str() == r.pendingID {
			if response.Get("subtype").Str() == "error" {
				r.permissionMode = r.launchedMode
			}
			r.pendingID, r.pendingMode = "", ""
			r.m.mergeMeta(r.key, map[string]any{"permissionMode": r.permissionMode})
		}
	}
	switch kind {
	case "result":
		r.idle = true
		r.waiting = false
		if turns := event.Get("num_turns").Int(); turns > 0 {
			r.usage.NumTurns = turns
		}
		if duration := int64(event.Get("duration_ms").Float()); duration > 0 {
			r.usage.DurationMs = duration
		}
		if cost := event.Get("total_cost_usd").Float(); cost > 0 {
			r.usage.CostUsd = cost
		}
	case "control_request":
		r.waiting = true
	case "assistant", "user", "stream_event":
		r.waiting = false
		if kind == "assistant" {
			message := event.Get("message")
			id := message.Get("id").Str()
			tokens := message.Get("usage").Get("output_tokens").Int64()
			if previous, seen := r.seenMessages[id]; id == "" || !seen || tokens > previous {
				if id != "" {
					r.usage.OutputTokens += tokens - r.seenMessages[id]
					r.seenMessages[id] = tokens
				} else {
					r.usage.OutputTokens += tokens
				}
			}
			r.usage.InputTokens = max(r.usage.InputTokens, message.Get("usage").Get("input_tokens").Int64())
		}
	}
	if id := event.Get("session_id").Str(); id != "" && event.Get("thread_id").Str() == "" &&
		event.Get("provider").Str() != "codex" && id != r.lastSessionID {
		r.lastSessionID = id
		r.m.mergeMeta(r.key, map[string]any{"lastSessionId": id})
	}
}

func (r *run) Stdin(data []byte) {
	r.mu.Lock()
	defer r.mu.Unlock()
	if r.codex != nil {
		return
	}
	for _, line := range bytes.Split(data, []byte("\n")) {
		message, ok := qtjson.ParseObject(line)
		if !ok {
			continue
		}
		switch message.Get("type").Str() {
		case "user":
			r.idle = false
		case "control_request":
			request := message.Get("request")
			if request.Get("subtype").Str() == "set_permission_mode" {
				r.pendingID = message.Get("request_id").Str()
				r.pendingMode = request.Get("mode").Str()
				r.permissionMode = r.pendingMode
			}
		}
	}
}

func (r *run) normalise(event qtjson.Value) qtjson.Value {
	event = Redact(event, r.m.credentialValues())
	if r.account != "" && (event.Get("session_id").Str() != "" || event.Get("thread_id").Str() != "") {
		event.Set("_forkmesh_account", qtjson.NewString(r.account))
	}
	switch event.Get("type").Str() {
	case "_local_user", "assistant", "_codex_agent_delta", "_codex_agent_complete":
		if !event.Has("_ts") {
			event.Set("_ts", qtjson.NewDouble(float64(r.m.now().UnixMilli())))
		}
	}
	return event
}

func (r *run) emitLocked(event qtjson.Value, so int64, stderr string) {
	r.emitEventLocked(event, so, true, stderr)
}

func (r *run) emitEventLocked(event qtjson.Value, so int64, store bool, stderr string) {
	event = r.normalise(event)
	line := event.Compact()
	if writer := r.m.writer(); store && writer != nil && r.dir != "" {
		writer.AppendEventLine(r.dir, r.sid, line)
		if text := LogText(event); text != "" {
			writer.AppendLog(r.dir, r.sid, QtUTF8(text))
		}
	}
	payload := map[string]any{"key": r.key, "line": string(line)}
	if !store {
		payload["transient"] = true
	}
	if so >= 0 {
		payload["so"] = so
		r.delivered = max(r.delivered, so)
		if store && r.dir == "" {
			r.rememberLocked(so, line)
		}
	}
	if stderr != "" {
		payload["stderr"] = stderr
	}
	r.sendLocked(payload)
}

func (r *run) sendLocked(payload map[string]any) {
	for id, sub := range r.subscribers {
		if !sub.peer.Send("core.agentrun.event", payload) {
			delete(r.subscribers, id)
			close(sub.stop)
		}
	}
}

func (r *run) scheduleAck() {
	if r.ackPending || r.delivered <= r.acked {
		return
	}
	r.ackPending = true
	time.AfterFunc(ackInterval, func() {
		r.mu.Lock()
		target := r.delivered
		r.mu.Unlock()
		done := func() {
			r.m.ack(r.key, target)
			r.mu.Lock()
			r.ackPending = false
			if target > r.acked {
				r.acked = target
			}
			r.scheduleAck()
			r.mu.Unlock()
		}
		if writer := r.m.writer(); writer != nil {
			writer.WhenDrained(done)
		} else {
			done()
		}
	})
}

func (r *run) Exit(exit map[string]any) {
	r.mu.Lock()
	if r.codex != nil {
		r.codex.exited()
	}
	r.flushStderrLocked()
	if r.runtime == RuntimeLog {
		r.flushLogLocked()
	}
	r.framer.Reset()
	target := r.delivered
	r.mu.Unlock()
	finish := func() {
		r.m.ack(r.key, target)
		r.mu.Lock()
		r.exit = exit
		r.exitPublished = true
		r.idle = true
		for id, sub := range r.subscribers {
			if !sub.peer.Send("core.agentrun.exit", exit) {
				delete(r.subscribers, id)
				close(sub.stop)
			}
		}
		r.mu.Unlock()
	}
	if writer := r.m.writer(); writer != nil {
		writer.WhenDrained(finish)
	} else {
		finish()
	}
}

func (r *run) subscribe(peer rpc.Peer, options subscribeOptions) map[string]any {
	after := options.after
	r.mu.Lock()
	defer r.mu.Unlock()
	if previous := r.subscribers[peer.ID()]; previous != nil {
		close(previous.stop)
		delete(r.subscribers, peer.ID())
	}
	if writer := r.m.writer(); writer != nil && r.dir != "" {
		writer.FlushDir(r.dir)
	}
	replayed := 0
	for _, stored := range r.storedSinceLocked(after) {
		peer.Send("core.agentrun.event", map[string]any{"key": r.key, "so": stored.so, "line": string(stored.line),
			"replay": true})
		replayed++
	}
	if options.stderr {
		if payload, ok := r.replayStderrLocked(options.stderrAfter); ok {
			peer.Send("core.agentrun.event", payload)
		}
	}
	sub := &subscriber{peer: peer, stop: make(chan struct{}), stderr: options.stderr}
	r.subscribers[peer.ID()] = sub
	go func() {
		select {
		case <-peer.Done():
			r.mu.Lock()
			if r.subscribers[peer.ID()] == sub {
				delete(r.subscribers, peer.ID())
			}
			r.mu.Unlock()
		case <-sub.stop:
		}
	}()
	if r.codex != nil {
		r.codex.resendPermissions(func(payload map[string]any) { peer.Send("core.agentrun.event", payload) })
	}
	if r.exitPublished {
		peer.Send("core.agentrun.exit", r.exit)
	}
	state := r.stateLocked()
	state["replayed"] = replayed
	if options.stderr {
		state["stderr"] = true
		state["stderrEnd"] = r.errSent
	}
	return state
}

func (r *run) unsubscribe(peer rpc.Peer) bool {
	r.mu.Lock()
	defer r.mu.Unlock()
	sub := r.subscribers[peer.ID()]
	if sub == nil {
		return false
	}
	delete(r.subscribers, peer.ID())
	close(sub.stop)
	return true
}

func (r *run) stateLocked() map[string]any {
	state := map[string]any{"key": r.key, "runtime": r.runtime, "sessionId": r.sid,
		"conversationOpened": r.conversationOpened, "permissionMode": r.permissionMode,
		"lastSessionId": r.lastSessionID, "idle": r.idle, "waiting": r.waiting, "usage": r.usage,
		"delivered": r.delivered, "acked": r.acked, "subscribers": len(r.subscribers), "exited": r.exitPublished}
	if r.exitPublished {
		state["exit"] = r.exit
	}
	if r.codex != nil {
		r.codex.describe(state)
	}
	return state
}

func (r *run) state() map[string]any {
	r.mu.Lock()
	defer r.mu.Unlock()
	return r.stateLocked()
}
