package sfu

import (
	"context"
	"encoding/json"
	"net/http"
	"time"

	"forkmesh.com/mirror-node/rtc/signal"
)

const (
	joinTimeout = 10 * time.Second
	idleTimeout = 60 * time.Second
	tickEvery   = 100 * time.Millisecond
	MaxSockets  = 1000
	MaxWaiting  = 200
)

type clientMessage struct {
	Type   string `json:"type"`
	Ticket string `json:"ticket"`
	SDP    string `json:"sdp"`
	Rev    int    `json:"rev"`
}

func (s *Server) Handler() http.Handler {
	routes := http.NewServeMux()
	routes.HandleFunc("/calls/ws", s.serveSocket)
	routes.HandleFunc("/calls/health", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		w.Header().Set("Cache-Control", "no-store")
		_ = json.NewEncoder(w).Encode(map[string]any{"ok": true})
	})
	return routes
}

func (s *Server) Run(ctx context.Context) {
	ticker := time.NewTicker(tickEvery)
	defer ticker.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case now := <-ticker.C:
			s.Tick(now)
		}
	}
}

func (s *Server) serveSocket(w http.ResponseWriter, r *http.Request) {
	if s.sockets.Add(1) > MaxSockets {
		s.sockets.Add(-1)
		http.Error(w, "busy", http.StatusServiceUnavailable)
		return
	}
	defer s.sockets.Add(-1)
	if s.waiting.Add(1) > MaxWaiting {
		s.waiting.Add(-1)
		http.Error(w, "busy", http.StatusServiceUnavailable)
		return
	}
	waiting := true
	defer func() {
		if waiting {
			s.waiting.Add(-1)
		}
	}()
	conn, err := signal.Upgrade(w, r, s.cfg.AllowOrigin)
	if err != nil {
		return
	}
	defer conn.Close()
	_ = conn.SetReadDeadline(time.Now().Add(joinTimeout))
	var join clientMessage
	if err := conn.ReadJSON(&join); err != nil || join.Type != "join" {
		_ = conn.WriteJSON(map[string]any{"type": "error", "error": "join_required"})
		return
	}
	ticket, err := signal.VerifyTicket(s.cfg.Secret, join.Ticket, time.Now())
	if err != nil {
		_ = conn.WriteJSON(map[string]any{"type": "error", "error": "ticket_invalid"})
		return
	}
	room, participant, others, err := s.join(ticket.Room, ticket.Peer, ticket.Acct)
	if err != nil {
		_ = conn.WriteJSON(map[string]any{"type": "error", "error": "room_full"})
		return
	}
	defer room.leave(participant)
	waiting = false
	s.waiting.Add(-1)
	conn.SetLimit(signal.MaxMessageBytes)
	go func(out chan outgoing) {
		for message := range out {
			if err := conn.WriteJSON(message.message); err != nil {
				conn.Close()
				for range out {
				}
				return
			}
		}
		conn.Close()
	}(participant.out)
	room.mu.Lock()
	participant.send(map[string]any{"type": "joined", "peer": participant.id, "peers": others})
	room.mu.Unlock()
	for {
		_ = conn.SetReadDeadline(time.Now().Add(idleTimeout))
		var message clientMessage
		if err := conn.ReadJSON(&message); err != nil {
			return
		}
		switch message.Type {
		case "offer":
			if err := room.offer(participant, message.SDP); err != nil {
				room.mu.Lock()
				participant.send(map[string]any{"type": "error", "error": "offer_rejected"})
				room.mu.Unlock()
			}
		case "answer":
			_ = room.answered(participant, message.Rev, message.SDP)
		case "ping":
			room.mu.Lock()
			participant.send(map[string]any{"type": "pong"})
			room.mu.Unlock()
		case "leave":
			return
		}
	}
}
