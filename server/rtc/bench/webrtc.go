package bench

import (
	"context"
	"encoding/json"
	"io"
	"net/http"
	"net/netip"
	"strconv"
	"sync"
	"testing"
	"time"

	"forkmesh.com/mirror-node/rtc/ice"
	"forkmesh.com/mirror-node/rtc/netsim"
	"forkmesh.com/mirror-node/rtc/sfu"
	"forkmesh.com/mirror-node/rtc/signal"
)

func WebRTC() Engine {
	return Engine{
		Name:    "webrtc-sfu",
		Adapter: "/web/adapters/webrtc-sfu.js",
		Key:     netsim.ICEUfragKey,
		Start:   startWebRTC,
	}
}

func startWebRTC(t testing.TB, routes *http.ServeMux, relay func(media netip.AddrPort) netip.AddrPort) {
	secret := []byte("call-bench-secret-0123456789abcdef")
	mux, err := ice.Listen("127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { mux.Close() })
	go mux.Serve()
	advertised := relay(netip.AddrPortFrom(netip.MustParseAddr("127.0.0.1"), uint16(mux.Port())))
	if int(advertised.Port()) != mux.Port() {
		t.Fatalf("the SFU advertises its own port %d, the relay listens on %v", mux.Port(), advertised)
	}
	server, err := sfu.NewServer(sfu.Config{
		Secret:      secret,
		Addresses:   []netip.Addr{advertised.Addr()},
		AllowOrigin: func(string) bool { return true },
	}, mux)
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	go server.Run(ctx)
	t.Cleanup(func() {
		server.Close()
		cancel()
	})
	room := signal.RoomID(secret, "be0cbe0cbe0cbe0cbe0cbe0cbe0cbe0c")
	routes.Handle("/calls/", server.Handler())
	routes.HandleFunc("/engine/ticket", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		json.NewEncoder(w).Encode(map[string]string{
			"ticket": signal.MintTicket(secret, signal.Ticket{
				Room: room, Peer: signal.NewPeerID(), Acct: signal.NewPeerID(), Exp: time.Now().Add(signal.TicketLifetime).Unix(),
			}),
			"url": "ws://" + r.Host + "/calls/ws",
		})
	})
	var mu sync.Mutex
	var frames []json.RawMessage
	routes.HandleFunc("/engine/relay", func(w http.ResponseWriter, r *http.Request) {
		mu.Lock()
		defer mu.Unlock()
		if r.Method == http.MethodPost {
			body, _ := io.ReadAll(io.LimitReader(r.Body, 64<<10))
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
