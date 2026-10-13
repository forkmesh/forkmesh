package hosts

import (
	"context"
	"strings"
	"time"

	"forkmesh.com/desktop-core/internal/rpc"
)

func (m *Module) logsStart(req *rpc.Request, host string) (any, error) {
	if req.Peer == nil {
		return nil, rpc.Fail("unsupported")
	}
	if m.hostDetail(m.desktopContext(context.Background(), false), host) == nil {
		return nil, rpc.Fail("unknown_host")
	}
	peer := req.Peer
	m.mu.Lock()
	open := 0
	for _, sub := range m.streams {
		if sub.peer.ID() == peer.ID() {
			open++
		}
	}
	if open >= maxStreamsPerPeer {
		m.mu.Unlock()
		return nil, rpc.Fail("too_many_streams")
	}
	sub := &subscription{id: newStreamID(), peer: peer, host: lowerHost(host), stream: newLogStream()}
	m.streams[sub.id] = sub
	recent := m.recent[sub.host]
	if len(recent) > 120 {
		recent = recent[len(recent)-120:]
	}
	backlog := append([]string{}, recent...)
	watch := !m.watched[peer.ID()]
	m.watched[peer.ID()] = true
	m.mu.Unlock()
	if watch {
		go m.watchPeer(peer)
	}
	m.kick()
	return map[string]any{"stream": sub.id, "backlog": backlog, "window": logWindow, "maxBuffered": logMaxBufferedLines}, nil
}

func (m *Module) watchPeer(peer rpc.Peer) {
	select {
	case <-peer.Done():
	case <-m.ctx.Done():
		return
	}
	m.mu.Lock()
	for id, sub := range m.streams {
		if sub.peer.ID() == peer.ID() && sub.peer == peer {
			delete(m.streams, id)
		}
	}
	delete(m.watched, peer.ID())
	for key, owner := range m.running {
		if owner == peer.ID() {
			m.running[key] = ""
		}
	}
	m.mu.Unlock()
}

func (m *Module) logsAckStop(req *rpc.Request, params map[string]any) (any, error) {
	id := jsonString(params["stream"])
	m.mu.Lock()
	sub, ok := m.streams[id]
	if !ok || req.Peer == nil || sub.peer.ID() != req.Peer.ID() {
		m.mu.Unlock()
		return nil, rpc.Fail("unknown_stream")
	}
	if req.Method == "hosts.logs.stop" {
		delete(m.streams, id)
		m.mu.Unlock()
		return map[string]any{}, nil
	}
	sub.stream.ack(int64(jsonDouble(params["seq"])))
	m.mu.Unlock()
	m.kick()
	return map[string]any{}, nil
}

func (m *Module) kick() {
	select {
	case m.wake <- struct{}{}:
	default:
	}
}

func (m *Module) flushLoop(ctx context.Context) {
	for {
		m.mu.Lock()
		idle := len(m.streams) == 0
		m.mu.Unlock()
		if idle {
			select {
			case <-ctx.Done():
				return
			case <-m.wake:
			}
		} else {
			timer := time.NewTimer(logFlushInterval)
			select {
			case <-ctx.Done():
				timer.Stop()
				return
			case <-timer.C:
			case <-m.wake:
				timer.Stop()
			}
		}
		m.flushStreams()
	}
}

func (m *Module) flushStreams() {
	type delivery struct {
		peer rpc.Peer
		data map[string]any
	}
	m.mu.Lock()
	deliveries := []delivery{}
	for id, sub := range m.streams {
		batch := sub.stream.takeBatch()
		if batch == nil {
			continue
		}
		batch["stream"] = id
		batch["host"] = sub.host
		deliveries = append(deliveries, delivery{peer: sub.peer, data: batch})
	}
	m.mu.Unlock()
	for _, d := range deliveries {
		d.peer.Send("hosts.log", d.data)
	}
}

type incomingLine struct {
	Node   string `json:"node"`
	Source string `json:"src"`
	Text   string `json:"text"`
}

func (m *Module) pushLine(node, source, text string) {
	m.pushLines([]incomingLine{{Node: node, Source: source, Text: text}})
}

func (m *Module) pushLines(lines []incomingLine) {
	now := m.nowMs()
	m.mu.Lock()
	defer m.mu.Unlock()
	for _, line := range lines {
		if strings.TrimSpace(line.Text) == "" {
			continue
		}
		host := lowerHost(line.Node)
		src := knownLogSource(line.Source)
		recent := append(m.recent[host], "["+src+"] "+leftUTF16(line.Text, logMaxLineChars))
		if len(recent) > recentLinesPerHost {
			recent = append([]string(nil), recent[len(recent)-recentLinesPerHost:]...)
		}
		m.recent[host] = recent
		for _, sub := range m.streams {
			if sub.host == host {
				sub.stream.push(now, src, line.Text)
			}
		}
	}
}

func (m *Module) handleLines(ctx context.Context, req *rpc.Request) (any, error) {
	var params struct {
		Lines []incomingLine `json:"lines"`
	}
	if err := req.Bind(&params); err != nil {
		return nil, err
	}
	lines := params.Lines
	if len(lines) > maxLinesPerNotify {
		lines = lines[len(lines)-maxLinesPerNotify:]
	}
	lines = m.skipFeedEchoes(lines)
	m.pushLines(lines)
	return map[string]any{"accepted": len(lines)}, nil
}
