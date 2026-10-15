package agentmodel

import (
	"context"
	"encoding/base64"
	"encoding/json"
	"os"
	"regexp"
	"sort"
	"strconv"
	"strings"

	"forkmesh.com/desktop-core/internal/agentproc"
	"forkmesh.com/desktop-core/internal/agents"
	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/phonefamily"
	"forkmesh.com/desktop-core/internal/qtjson"
	"forkmesh.com/desktop-core/internal/rpc"
)

const (
	maxRawAttachmentBytes    = 2500 * 1024
	maxDecodeAttachmentBytes = 40 * 1024 * 1024
	stopGraceMs              = 1500
)

var attachmentIDRe = regexp.MustCompile(`^[0-9a-f]{32}$`)

type localPeer struct{ done chan struct{} }

func (p *localPeer) ID() string                       { return "agentmodel" }
func (p *localPeer) Transport() rpc.Transport         { return rpc.UI }
func (p *localPeer) Device() rpc.Device               { return rpc.Device{} }
func (p *localPeer) Send(event string, data any) bool { return false }
func (p *localPeer) Done() <-chan struct{}            { return p.done }

func (m *Module) registerActions(core *daemon.Core) {
	core.Registry.Handle("agents.attachment", m.Name(), m.handleAttachment)
	core.Registry.Handle("agents.stop", m.Name(), m.handleStop)
	core.Registry.Handle("agents.queue.move", m.Name(), m.handleQueueMove)
	for _, method := range []string{"agents.start", "agents.prompt", "agents.continue", "agents.takeover",
		"agents.handoff", "agents.queue.limit", "agents.markRead"} {
		core.Registry.Handle(method, m.Name(), m.handleDesktopAction)
	}
	core.Registry.Handle("diag.prepare", m.Name(), m.gate.Wrap(m.handleDiagPrepare))
	core.Registry.Handle("diag.run", m.Name(), m.gate.Wrap(m.handleDiagRun))
}

func (m *Module) handleDesktopAction(ctx context.Context, req *rpc.Request) (any, error) {
	if m.desktopAttached() {
		return nil, m.delegate(req, false)
	}
	return nil, rpc.Fail("desktop_unavailable")
}

func sniffImage(head []byte) string {
	text := string(head)
	switch {
	case strings.HasPrefix(text, "\xff\xd8\xff"):
		return "image/jpeg"
	case strings.HasPrefix(text, "\x89PNG\r\n\x1a\n"):
		return "image/png"
	case strings.HasPrefix(text, "GIF87a"), strings.HasPrefix(text, "GIF89a"):
		return "image/gif"
	case len(head) >= 12 && strings.HasPrefix(text, "RIFF") && text[8:12] == "WEBP":
		return "image/webp"
	}
	return ""
}

func (m *Module) handleAttachment(ctx context.Context, req *rpc.Request) (any, error) {
	params := paramsObject(req)
	id := params.Get("id").Int()
	wanted := strings.ToLower(trimmed(params.Get("attachment").Str()))
	session, store, found := m.ownSession(id)
	_, pushed := m.snapshot()
	if !pushed || m.salt() == nil {
		return nil, m.delegate(req, false)
	}
	if !found || !attachmentIDRe.MatchString(wanted) {
		return nil, rpc.Fail("unknown_attachment")
	}
	relayPreview := (req.Peer != nil && req.Peer.Transport() == rpc.Relay) || params.Get("preview").BoolValue()
	if writer := m.writer(); writer != nil {
		writer.FlushDir(store.SessionDir(session))
	}
	var entry *attachmentEntry
	for _, candidate := range m.attachmentEntries(m.sessionAttachmentPaths(store, session, m.knownImages(id)), id,
		maxAttachmentEntries) {
		if candidate.ID == wanted {
			copy := candidate
			entry = &copy
			break
		}
	}
	if entry == nil {
		return nil, rpc.Fail("unknown_attachment")
	}
	isImage := strings.HasPrefix(entry.MediaType, "image/")
	info, err := os.Stat(entry.Path)
	if err != nil {
		return nil, rpc.Fail("unreadable")
	}
	size := info.Size()
	if relayPreview {
		if size <= 0 {
			return nil, rpc.Fail("unreadable")
		}
		if !isImage {
			if size > maxRelayPreviewBytes {
				return nil, rpc.Fail("too_large_over_relay")
			}
			return nil, rpc.Fail("not_an_image")
		}
		if size > maxDecodeAttachmentBytes {
			return nil, rpc.Fail("too_large")
		}
		return nil, m.delegate(req, false)
	}
	if !isImage {
		return nil, rpc.Fail("not_an_image")
	}
	if size <= 0 {
		return nil, rpc.Fail("unreadable")
	}
	if size > maxDecodeAttachmentBytes {
		return nil, rpc.Fail("too_large")
	}
	data, err := os.ReadFile(entry.Path)
	if err != nil || int64(len(data)) != size {
		return nil, rpc.Fail("unreadable")
	}
	head := data
	if len(head) > 16 {
		head = head[:16]
	}
	sniffed := sniffImage(head)
	if sniffed == "" || size > maxRawAttachmentBytes {
		return nil, m.delegate(req, false)
	}
	return raw(qtjson.ObjectOf("name", entry.Name, "mediaType", sniffed, "size", float64(size),
		"data", base64.StdEncoding.EncodeToString(data), "preview", false)), nil
}

const maxRelayPreviewBytes = 700 * 1024

func (m *Module) procs() *agentproc.Module {
	if m.core == nil {
		return nil
	}
	procs, _ := m.core.Module("agentproc").(*agentproc.Module)
	return procs
}

func sessionProcess(process agentproc.Process, id int) bool {
	sid := strconv.Itoa(id)
	if text, _ := process.Meta["sessionId"].(string); text == sid {
		return true
	}
	return process.Key == "agent-"+sid || strings.HasPrefix(process.Key, "agent-"+sid+"-")
}

func (m *Module) rawRecord(store *agents.Store, session agents.Session) (qtjson.Value, bool) {
	data, err := os.ReadFile(store.MetaPath(session))
	if err != nil {
		return qtjson.Value{}, false
	}
	return qtjson.ParseObject(data)
}

func (m *Module) stopDetached(id int) bool {
	session, store, ok := m.ownSession(id)
	if !ok || !stoppable(session) {
		return false
	}
	m.mu.Lock()
	queue := m.ctx.Queue[:0:0]
	for _, queued := range m.ctx.Queue {
		if queued != id {
			queue = append(queue, queued)
		}
	}
	m.ctx.Queue = queue
	m.mu.Unlock()
	if procs := m.procs(); procs != nil {
		for _, process := range procs.Processes() {
			if process.Running && sessionProcess(process, id) {
				_ = procs.StopProcess(process.Key, stopGraceMs)
			}
		}
	}
	if session.Status == agents.StatusQueued || session.Status == agents.StatusRunning ||
		session.Status == agents.StatusWaiting {
		record, ok := m.rawRecord(store, session)
		if ok {
			record.Set("status", qtjson.NewString(agents.StatusStopped))
			record.Set("finishedAtMs", qtjson.NewInt(m.nowMs()))
			if writer := m.writer(); writer != nil {
				_, _ = writer.SaveRecord(record)
			}
		}
	}
	m.ringAgents()
	return true
}

func (m *Module) handleStop(ctx context.Context, req *rpc.Request) (any, error) {
	if m.desktopAttached() {
		return nil, m.delegate(req, false)
	}
	id := paramsObject(req).Get("id").Int()
	if id < 0 {
		return nil, rpc.Fail("desktop_unavailable")
	}
	if _, credentialed := m.credentials(); !credentialed {
		return nil, rpc.Fail("desktop_unavailable")
	}
	if snapshot, _ := m.snapshot(); snapshot.Live[id].Terminal != "" {
		return nil, rpc.Fail("desktop_unavailable")
	}
	m.audit(req, "Stopping agent "+strconv.Itoa(id))
	if !m.stopDetached(id) {
		return nil, rpc.Fail("not_running")
	}
	return map[string]any{}, nil
}

func moveInQueue(ctx *desktopContext, byID map[int]*agents.Session, id, to int) (int, bool, bool) {
	order := queueOrder(ctx, byID)
	from := -1
	for i, queued := range order {
		if queued == id {
			from = i
			break
		}
	}
	session := byID[id]
	if from < 0 || session == nil {
		return 0, false, false
	}
	order = append(order[:from], order[from+1:]...)
	to = min(max(to, 0), len(order))
	if to == from || len(order) == 0 {
		return session.Priority, false, true
	}
	var before, after *agents.Session
	if to > 0 {
		before = byID[order[to-1]]
	}
	if to < len(order) {
		after = byID[order[to]]
	}
	low, high := 0, 100
	if before != nil {
		low = before.Priority
	}
	if after != nil {
		high = after.Priority
	}
	priority := 0
	switch {
	case high-low >= 2:
		priority = min(max((low+high)/2, 1), 99)
	case after != nil:
		priority = after.Priority
	default:
		priority = before.Priority
	}
	session.Priority = priority
	queue := append([]int(nil), ctx.Queue...)
	sort.SliceStable(queue, func(i, j int) bool {
		left, right := byID[queue[i]], byID[queue[j]]
		return left != nil && right != nil && left.Priority < right.Priority
	})
	kept := queue[:0]
	for _, queued := range queue {
		if queued != id {
			kept = append(kept, queued)
		}
	}
	anchorID := 0
	if after != nil {
		anchorID = after.ID
	} else {
		anchorID = before.ID
	}
	anchor := -1
	for i, queued := range kept {
		if queued == anchorID {
			anchor = i
			break
		}
	}
	at := anchor
	if after == nil {
		at = anchor + 1
	}
	at = min(max(at, 0), len(kept))
	kept = append(kept, 0)
	copy(kept[at+1:], kept[at:])
	kept[at] = id
	ctx.Queue = kept
	return priority, true, true
}

func (m *Module) handleQueueMove(ctx context.Context, req *rpc.Request) (any, error) {
	if m.desktopAttached() {
		return nil, m.delegate(req, false)
	}
	params := paramsObject(req)
	id := params.Get("id").Int()
	to := params.Get("to").IntOr(-1)
	m.mu.Lock()
	pushed := m.pushed
	m.mu.Unlock()
	if !pushed {
		return nil, rpc.Fail("desktop_unavailable")
	}
	if _, _, found := m.ownSession(id); !found || to < 0 {
		return nil, rpc.Fail("not_queued")
	}
	m.audit(req, "Moving queued agent "+strconv.Itoa(id)+" to place "+strconv.Itoa(to+1))
	sessions, store := m.loadSessions()
	byID := indexSessions(sessions)
	m.mu.Lock()
	snapshot := m.ctx
	snapshot.Queue = append([]int(nil), m.ctx.Queue...)
	m.mu.Unlock()
	previous := 0
	if session := byID[id]; session != nil {
		previous = session.Priority
	}
	priority, changed, ok := moveInQueue(&snapshot, byID, id, to)
	if !ok {
		return nil, rpc.Fail("not_queued")
	}
	if changed && store != nil {
		session := byID[id]
		record, found := m.rawRecord(store, *session)
		saved := false
		if found {
			record.Set("priority", qtjson.NewInt(int64(priority)))
			if writer := m.writer(); writer != nil {
				saved, _ = writer.SaveRecord(record)
			}
		}
		if !saved {
			session.Priority = previous
			return nil, rpc.Fail("not_queued")
		}
		m.mu.Lock()
		m.ctx.Queue = snapshot.Queue
		m.mu.Unlock()
	}
	sessions, _ = m.loadSessions()
	current, _ := m.snapshot()
	m.ringAgents()
	return raw(m.queuePayload(&current, indexSessions(sessions))), nil
}

func (m *Module) callLocal(ctx context.Context, method string, params any) (qtjson.Value, error) {
	encoded, _ := json.Marshal(params)
	peer := &localPeer{done: make(chan struct{})}
	result, err := m.core.Registry.Dispatch(ctx, &rpc.Request{Peer: peer, ID: 1, Method: method, Params: encoded})
	if err != nil {
		return qtjson.Value{}, err
	}
	var data []byte
	if rawResult, ok := result.(json.RawMessage); ok {
		data = rawResult
	} else {
		data, _ = json.Marshal(result)
	}
	value, _ := qtjson.Parse(data)
	return value, nil
}

func diagSubject(action string, params qtjson.Value) (string, string) {
	switch action {
	case "stopAgent":
		session := params.Get("session").IntOr(0)
		if session <= 0 {
			return "", "bad_session"
		}
		return strconv.Itoa(session), ""
	case "sendStall":
		index := params.Get("index").IntOr(-1)
		if index < 0 || index > 1000 {
			return "", "bad_index"
		}
		return strconv.Itoa(index), ""
	case "sendStalls":
		return "all", ""
	}
	return "", "unsupported_action"
}

func diagSummary(action, subject, label string) string {
	switch action {
	case "stopAgent":
		if label == "" {
			label = "#" + subject
		}
		return "Stop the agent run \"" + label + "\". Everything it started goes down with it; its worktree and branch are kept."
	case "sendStalls":
		return "Start an agent on the desktop to fix the recorded UI stalls."
	case "sendStall":
		if label != "" {
			return "Start an agent on the desktop to fix this UI stall (" + label + ")."
		}
		return "Start an agent on the desktop to fix this UI stall."
	}
	return ""
}

func (m *Module) handleDiagPrepare(ctx context.Context, req *rpc.Request) (any, error) {
	params := paramsObject(req)
	action := params.Get("action").Str()
	subject, why := diagSubject(action, params)
	if subject == "" {
		return nil, rpc.Fail(why)
	}
	label := ""
	if action == "stopAgent" {
		session, _, found := m.ownSessionAny(qtToInt(subject))
		if !found || !stoppable(session) {
			return nil, rpc.Fail("not_running")
		}
		if title := trimmed(session.IssueTitle); title != "" {
			label = left16(title, 120)
		} else {
			label = left16(trimmed(section0(session.Prompt)), 120)
		}
	} else {
		stalls, err := m.callLocal(ctx, "diag.stalls", map[string]any{})
		if err != nil || stalls.Get("stalls").Len() == 0 || !stalls.Get("canSend").BoolValue() {
			return nil, rpc.Fail("nothing_to_send")
		}
		if action == "sendStall" {
			index := qtToInt(subject)
			if index >= stalls.Get("stalls").Len() {
				return nil, rpc.Fail("unknown_stall")
			}
			detail, err := m.callLocal(ctx, "diag.stall", map[string]any{"index": index})
			if err != nil {
				return nil, rpc.Fail("unknown_stall")
			}
			label = detail.Get("when").Str()
		}
	}
	token := m.confirms.diag.Issue(phonefamily.DeviceOf(req).Key, action, subject, m.nowMs())
	return raw(qtjson.ObjectOf("token", token, "action", action, "subject", subject,
		"summary", diagSummary(action, subject, label), "expiresInMs", float64(phonefamily.ConfirmationLifetimeMs))), nil
}

func (m *Module) ownSessionAny(id int) (agents.Session, *agents.Store, bool) {
	writer := m.writer()
	if id <= 0 || writer == nil {
		return agents.Session{}, nil, false
	}
	session, _, ok := m.findSession(id)
	return session, writer.Store(), ok
}

func (m *Module) handleDiagRun(ctx context.Context, req *rpc.Request) (any, error) {
	params := paramsObject(req)
	action := params.Get("action").Str()
	subject, why := diagSubject(action, params)
	verdict := m.confirms.diag.Consume(params.Get("token").Str(), phonefamily.DeviceOf(req).Key, action, subject, m.nowMs())
	if subject == "" || verdict != phonefamily.Ok {
		if subject != "" {
			why = phonefamily.ConfirmationError(verdict)
		}
		m.logUI("Refused a diagnostics %s from %s: %s.", action, phonefamily.DeviceLabel(req), why)
		return nil, rpc.Fail(why)
	}
	if m.desktopAttached() {
		return nil, m.delegate(req, true)
	}
	if action != "stopAgent" {
		return nil, rpc.Fail("desktop_unavailable")
	}
	if _, credentialed := m.credentials(); !credentialed {
		return nil, rpc.Fail("desktop_unavailable")
	}
	session := qtToInt(subject)
	if !m.stopDetached(session) {
		return nil, rpc.Fail("not_running")
	}
	m.logUI("Agent run #%d stopped from the memory panel by %s.", session, phonefamily.DeviceLabel(req))
	phonefamily.BroadcastPhones(m.core, "diag.changed", nil)
	return raw(qtjson.ObjectOf("started", true, "action", action, "subject", subject, "message", "Stop requested.")), nil
}
