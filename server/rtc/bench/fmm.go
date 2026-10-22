package bench

import (
	"context"
	"encoding/json"
	"io"
	"net"
	"net/http"
	"net/netip"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"

	"forkmesh.com/mirror-node/rtc/fmmserver"
	"forkmesh.com/mirror-node/rtc/quic"
	"forkmesh.com/mirror-node/rtc/signal"
)

type fmmKeys struct {
	mu      sync.Mutex
	pending []string
}

func (k *fmmKeys) reset() {
	k.mu.Lock()
	k.pending = nil
	k.mu.Unlock()
}

func (k *fmmKeys) expect(name string) {
	k.mu.Lock()
	k.pending = append(k.pending, name)
	k.mu.Unlock()
}

func (k *fmmKeys) key(packet []byte) (string, bool) {
	if len(packet) == 0 || packet[0]&0x80 == 0 {
		return "", false
	}
	k.mu.Lock()
	defer k.mu.Unlock()
	if len(k.pending) == 0 {
		return "", false
	}
	name := k.pending[0]
	k.pending = k.pending[1:]
	return name, true
}

func FMM() Engine {
	keys := &fmmKeys{}
	return Engine{
		Name:    "fm-media",
		Adapter: "/web/adapters/fmm-media.js",
		Key:     keys.key,
		Start: func(t testing.TB, routes *http.ServeMux, relay func(media netip.AddrPort) netip.AddrPort) {
			startFMM(t, routes, relay, keys)
		},
	}
}

func EngineNamed(name string) (Engine, bool) {
	switch strings.TrimSpace(name) {
	case "", "webrtc", "webrtc-sfu":
		return WebRTC(), true
	case "fmm", "fm-media":
		return FMM(), true
	}
	return Engine{}, false
}

func startFMM(t testing.TB, routes *http.ServeMux, relay func(media netip.AddrPort) netip.AddrPort, keys *fmmKeys) {
	keys.reset()
	secret := []byte("call-bench-secret-0123456789abcdef")
	certs, err := fmmserver.NewCertStore("", nil)
	if err != nil {
		t.Fatal(err)
	}
	srv, err := fmmserver.New(fmmserver.Config{Secret: secret, Certs: certs, AllowOrigin: func(string) bool { return true }})
	if err != nil {
		t.Fatal(err)
	}
	ep, err := quic.Listen("udp4", "127.0.0.1:0", srv.QUICConfig())
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	go srv.Serve(ctx, ep)
	t.Cleanup(func() {
		cancel()
		srv.Close()
		ep.Close()
	})
	advertised := relay(ep.LocalAddr().(*net.UDPAddr).AddrPort())
	room := signal.RoomID(secret, "be0cbe0cbe0cbe0cbe0cbe0cbe0cbe0c")
	url := "https://" + advertised.String() + fmmserver.Path
	routes.HandleFunc("/engine/expect", func(w http.ResponseWriter, r *http.Request) {
		name := strings.TrimSpace(r.URL.Query().Get("name"))
		if name == "" {
			http.Error(w, "no name", http.StatusBadRequest)
			return
		}
		keys.expect(name)
	})
	routes.HandleFunc("/engine/ticket", func(w http.ResponseWriter, r *http.Request) {
		peer := signal.NewPeerID()
		acct := signal.NewPeerID()
		ticket := signal.MintTicket(secret, signal.Ticket{
			Room: room, Peer: peer, Acct: acct, Exp: time.Now().Add(signal.TicketLifetime).Unix(), Engine: signal.EngineFMM,
		})
		w.Header().Set("Content-Type", "application/json")
		json.NewEncoder(w).Encode(map[string]any{
			"ticket": "webrtc-unused",
			"url":    "ws://127.0.0.1:1/calls/ws",
			"room":   room,
			"peer":   peer,
			"fmm": map[string]any{
				"engine": "fmm",
				"url":    url,
				"addr":   advertised.String(),
				"alpn":   "fmm/1",
				"hashes": certs.Hashes(),
				"ticket": ticket,
			},
		})
	})
	var mu sync.Mutex
	var frames []json.RawMessage
	routes.HandleFunc("/engine/relay", func(w http.ResponseWriter, r *http.Request) {
		mu.Lock()
		defer mu.Unlock()
		if r.Method == http.MethodPost {
			body, _ := io.ReadAll(io.LimitReader(r.Body, 256<<10))
			frames = append(frames, json.RawMessage(body))
			return
		}
		since, _ := strconv.Atoi(r.URL.Query().Get("since"))
		if since < 0 || since > len(frames) {
			since = len(frames)
		}
		w.Header().Set("Content-Type", "application/json")
		json.NewEncoder(w).Encode(map[string]any{"frames": append([]json.RawMessage{}, frames[since:]...), "next": len(frames)})
	})
}
