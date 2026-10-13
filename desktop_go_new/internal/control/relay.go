package control

import (
	"context"
	"encoding/base64"
	"encoding/json"
	"strings"
	"sync"
	"time"

	"forkmesh.com/desktop-core/internal/rpc"
)

const (
	maxQueuedRelayBytes = 16 * 1024 * 1024
	relaySendTimeout    = 15 * time.Second
)

func encodeStdBase64(raw []byte) string { return base64.StdEncoding.EncodeToString(raw) }

func (m *Module) relayFrame(frame map[string]any) {
	if m.node == "" || !m.ensureIdentity() {
		return
	}
	if jsonString(frame["from"]) != "browser" ||
		strings.ToLower(strings.TrimSpace(jsonString(frame["node"]))) != m.node {
		return
	}
	session := jsonString(frame["session"])
	action := jsonString(frame["action"])
	if size := utf16Len(session); size < 16 || size > 64 {
		return
	}
	now := m.now()
	forgetOpening := func() {
		delete(m.relayOpening, session)
		delete(m.relayOpeningSeq, session)
		delete(m.relayOpeningSince, session)
	}
	refuse := func(reason string) {
		forgetOpening()
		m.relayToldGone = noteBounded(m.relayToldGone, session)
		m.relay.enqueue(map[string]any{
			"type":    "control",
			"action":  "error",
			"session": session,
			"node":    m.node,
			"reason":  reason,
		}, session, "error", 256)
	}
	conn := m.connectionForRelay(session)
	if action == "close" {
		forgetOpening()
		m.relayUndelivered = removeString(m.relayUndelivered, session)
		m.relayToldGone = noteBounded(m.relayToldGone, session)
		if conn != nil {
			conn.relaySession = ""
			m.closeConnection(conn, "")
		}
		return
	}
	chunk := strictBase64(jsonString(frame["data"]))
	more := jsonBool(frame["more"])
	if len(chunk) == 0 {
		return
	}
	seqValue := int64(jsonDouble(frame["seq"], -1))
	if action == "open" {
		if conn != nil {
			return
		}
		if _, opening := m.relayOpening[session]; !opening {
			if len(m.relayOpening) >= maxRelayOpenings {
				refuse("This desktop has too many channels opening.")
				return
			}
			m.relayOpeningSince[session] = now
			m.relayOpening[session] = nil
		}
		if seqValue != int64(m.relayOpeningSeq[session]) {
			refuse("The hello arrived out of order.")
			delete(m.relayOpeningSeq, session)
			return
		}
		m.relayOpeningSeq[session] = uint32(seqValue + 1)
		pending := append(m.relayOpening[session], chunk...)
		m.relayOpening[session] = pending
		if len(pending) > 64*1024 {
			refuse("The hello is too large.")
			return
		}
		if more {
			return
		}
		hello := pending
		nextSeq := m.relayOpeningSeq[session]
		forgetOpening()
		if !IsControlHello(hello) {
			refuse("That is not a ForkMesh control hello.")
			return
		}
		if len(m.conns) >= maxConnections && !m.evictIdleRelayed(now) {
			refuse("This desktop has too many open channels.")
			return
		}
		created := m.newConnection(rpc.Relay)
		created.relaySession = session
		created.relayReceiveSeq = nextSeq
		created.lastHeardMs = now
		if !m.acceptHello(created, hello) {
			refuse("The hello did not verify.")
			created.relaySession = ""
			m.closeConnection(created, "bad hello")
			return
		}
		m.startHandshakeTimer(created)
		return
	}
	if action != "data" {
		return
	}
	if conn == nil {
		forgetOpening()
		m.tellRelayGone(session, "channel_gone")
		return
	}
	conn.lastHeardMs = now
	if seqValue != int64(conn.relayReceiveSeq) {
		m.closeConnection(conn, "relay chunk out of order")
		return
	}
	conn.relayReceiveSeq++
	conn.relayPending = append(conn.relayPending, chunk...)
	if len(conn.relayPending) > MaxRelayFrameBytes {
		m.closeConnection(conn, "oversized frame")
		return
	}
	if more {
		return
	}
	payload := conn.relayPending
	conn.relayPending = nil
	m.handleFrame(conn, payload)
}

func (m *Module) tellRelayGone(session, reason string) {
	if containsString(m.relayToldGone, session) {
		return
	}
	m.relayToldGone = noteBounded(m.relayToldGone, session)
	m.relay.enqueue(map[string]any{
		"type":    "control",
		"action":  "close",
		"session": session,
		"node":    m.node,
		"reason":  reason,
	}, session, "close", 256)
}

func (m *Module) relayRestored() {
	sessions := m.relayUndelivered
	m.relayUndelivered = nil
	for _, session := range sessions {
		m.relayToldGone = removeString(m.relayToldGone, session)
		m.tellRelayGone(session, "relay_reconnected")
	}
}

func (m *Module) relayDataFailed(session string) {
	if conn := m.connectionForRelay(session); conn != nil {
		m.closeConnection(conn, "relay unavailable")
	}
}

type relayItem struct {
	frame   map[string]any
	session string
	action  string
	size    int
	onFail  func()
}

type relayWriter struct {
	m      *Module
	mu     sync.Mutex
	signal chan struct{}
	queue  []relayItem
	bytes  int
	broken map[string]bool
	budget *SendBudget
	paced  bool
}

func newRelayWriter(m *Module) *relayWriter {
	return &relayWriter{m: m, signal: make(chan struct{}, 1), broken: map[string]bool{}, budget: NewSendBudget()}
}

func (w *relayWriter) enqueue(frame map[string]any, session, action string, size int) bool {
	return w.push(relayItem{frame: frame, session: session, action: action, size: size})
}

func (w *relayWriter) enqueueClose(frame map[string]any, session string, onFail func()) {
	if !w.push(relayItem{frame: frame, session: session, action: "close", size: 256, onFail: onFail}) && onFail != nil {
		onFail()
	}
}

func (w *relayWriter) push(item relayItem) bool {
	w.mu.Lock()
	if w.bytes+item.size > maxQueuedRelayBytes {
		w.mu.Unlock()
		return false
	}
	w.queue = append(w.queue, item)
	w.bytes += item.size
	w.mu.Unlock()
	select {
	case w.signal <- struct{}{}:
	default:
	}
	return true
}

func (w *relayWriter) configureFrom(params map[string]any) {
	raw, ok := params["controlBudget"]
	if !ok {
		return
	}
	budget := jsonObject(raw)
	windowMs := int64(jsonDouble(budget["windowMs"], 0))
	maximum, _ := jsonInt(budget["max"])
	w.mu.Lock()
	defer w.mu.Unlock()
	w.paced = true
	if windowMs > 0 && maximum > 0 {
		w.budget.Configure(windowMs, maximum)
	} else {
		w.budget.Configure(DefaultRelayControlWindowMs, DefaultRelayControlMax)
	}
}

func (w *relayWriter) restored(params map[string]any) {
	w.mu.Lock()
	w.budget.Reset()
	w.mu.Unlock()
	w.configureFrom(params)
}

func (w *relayWriter) next() (relayItem, bool) {
	w.mu.Lock()
	defer w.mu.Unlock()
	if len(w.queue) == 0 {
		return relayItem{}, false
	}
	item := w.queue[0]
	w.queue[0] = relayItem{}
	w.queue = w.queue[1:]
	w.bytes -= item.size
	return item, true
}

func (w *relayWriter) pace(ctx context.Context) bool {
	for {
		w.mu.Lock()
		if !w.paced {
			w.mu.Unlock()
			return true
		}
		now := time.Now().UnixMilli()
		wait := w.budget.WaitMs(now)
		if wait == 0 {
			w.budget.Spend(now)
			w.mu.Unlock()
			return true
		}
		w.mu.Unlock()
		timer := time.NewTimer(time.Duration(min(wait, 60000)) * time.Millisecond)
		select {
		case <-ctx.Done():
			timer.Stop()
			return false
		case <-timer.C:
		}
	}
}

func (w *relayWriter) run(ctx context.Context) {
	for {
		item, ok := w.next()
		if !ok {
			select {
			case <-ctx.Done():
				return
			case <-w.signal:
			}
			continue
		}
		if item.action == "data" && w.broken[item.session] {
			continue
		}
		if item.action == "close" {
			delete(w.broken, item.session)
		}
		if !w.pace(ctx) {
			return
		}
		if w.m.sendRelay(ctx, item.frame) {
			continue
		}
		switch item.action {
		case "data":
			w.broken[item.session] = true
			session := item.session
			w.m.post(func() { w.m.relayDataFailed(session) })
		case "close":
			if item.onFail != nil {
				item.onFail()
			}
		}
	}
}

func (m *Module) sendRelay(ctx context.Context, frame map[string]any) bool {
	if m.relayHook != nil {
		return m.relayHook(frame)
	}
	if sender := m.relaySender.Load(); sender != nil {
		return (*sender)(frame)
	}
	if m.core.UI() == nil {
		return false
	}
	callCtx, cancel := context.WithTimeout(ctx, relaySendTimeout)
	defer cancel()
	raw, err := m.core.CallUI(callCtx, "ui.relay.send", map[string]any{"frame": frame})
	if err != nil {
		return false
	}
	var result struct {
		Sent bool `json:"sent"`
	}
	if json.Unmarshal(raw, &result) != nil {
		return false
	}
	return result.Sent
}

type notification struct {
	method string
	params any
	onFail func()
}

type notifier struct {
	m      *Module
	mu     sync.Mutex
	signal chan struct{}
	queue  []notification
}

func newNotifier(m *Module) *notifier {
	return &notifier{m: m, signal: make(chan struct{}, 1)}
}

func (n *notifier) push(method string, params any, onFail func()) {
	n.mu.Lock()
	n.queue = append(n.queue, notification{method: method, params: params, onFail: onFail})
	n.mu.Unlock()
	select {
	case n.signal <- struct{}{}:
	default:
	}
}

func (n *notifier) run(ctx context.Context) {
	for {
		n.mu.Lock()
		batch := n.queue
		n.queue = nil
		n.mu.Unlock()
		if len(batch) == 0 {
			select {
			case <-ctx.Done():
				return
			case <-n.signal:
			}
			continue
		}
		for _, item := range batch {
			if !n.m.core.NotifyUI(item.method, item.params) && item.onFail != nil {
				item.onFail()
			}
		}
	}
}
