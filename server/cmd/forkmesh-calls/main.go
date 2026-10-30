package main

import (
	"context"
	"encoding/json"
	"errors"
	"log"
	"net"
	"net/http"
	"net/netip"
	"os"
	"os/signal"
	"path/filepath"
	"strings"
	"syscall"
	"time"

	"forkmesh.com/mirror-node/rtc/fmmserver"
	"forkmesh.com/mirror-node/rtc/ice"
	"forkmesh.com/mirror-node/rtc/quic"
	"forkmesh.com/mirror-node/rtc/sfu"
	callsignal "forkmesh.com/mirror-node/rtc/signal"
)

func env(name, fallback string) string {
	if value := strings.TrimSpace(os.Getenv(name)); value != "" {
		return value
	}
	return fallback
}

func addresses(list string) ([]netip.Addr, error) {
	var result []netip.Addr
	for _, item := range strings.Split(list, ",") {
		item = strings.TrimSpace(item)
		if item == "" {
			continue
		}
		address, err := netip.ParseAddr(item)
		if err != nil {
			return nil, err
		}
		result = append(result, address)
	}
	if len(result) == 0 {
		return nil, errors.New("FORKMESH_CALLS_ADDRESSES names no public address")
	}
	return result, nil
}

func origins(list string) func(string) bool {
	allowed := map[string]bool{}
	for _, item := range strings.Split(list, ",") {
		if item = strings.TrimRight(strings.TrimSpace(item), "/"); item != "" {
			allowed[item] = true
		}
	}
	return func(origin string) bool { return origin == "" || allowed[origin] }
}

func readSecret() string {
	if value := strings.TrimSpace(os.Getenv("FORKMESH_CALLS_SECRET")); value != "" {
		return value
	}
	directory := os.Getenv("CREDENTIALS_DIRECTORY")
	if directory == "" {
		return ""
	}
	raw, err := os.ReadFile(filepath.Join(directory, "FORKMESH_CALLS_SECRET"))
	if err != nil {
		return ""
	}
	return strings.TrimSpace(string(raw))
}

func main() {
	secret := readSecret()
	if len(secret) < 32 {
		log.Fatal("forkmesh-calls: FORKMESH_CALLS_SECRET must be at least 32 characters")
	}
	public, err := addresses(env("FORKMESH_CALLS_ADDRESSES", ""))
	if err != nil {
		log.Fatalf("forkmesh-calls: %v", err)
	}
	mux, err := ice.Listen(env("FORKMESH_CALLS_UDP", ":3479"))
	if err != nil {
		log.Fatalf("forkmesh-calls: udp: %v", err)
	}
	allow := origins(env("FORKMESH_CALLS_ORIGINS", "https://app.forkmesh.com"))
	engines := callsignal.NewRoomEngines(sfu.MaxServerPeople)
	server, err := sfu.NewServer(sfu.Config{
		Secret:      []byte(secret),
		Addresses:   public,
		AllowOrigin: allow,
		Engines:     engines,
	}, mux)
	if err != nil {
		log.Fatalf("forkmesh-calls: %v", err)
	}
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	media, err := startFMM(ctx, env("FORKMESH_CALLS_FMM", ""), fmmStateDir(), []byte(secret), allow, engines)
	if err != nil {
		log.Fatalf("forkmesh-calls: fm media: %v", err)
	}
	go func() {
		if err := mux.Serve(); err != nil {
			log.Printf("forkmesh-calls: udp stopped: %v", err)
		}
	}()
	go server.Run(ctx)
	httpServer := &http.Server{
		Addr:              env("FORKMESH_CALLS_HTTP", "127.0.0.1:7881"),
		Handler:           routes(server.Handler(), media),
		ReadHeaderTimeout: 10 * time.Second,
		MaxHeaderBytes:    16 << 10,
	}
	go func() {
		<-ctx.Done()
		shutdown, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		_ = httpServer.Shutdown(shutdown)
		_ = mux.Close()
	}()
	log.Printf("forkmesh-calls: http %s, udp port %d, %d public address(es), fm media %s", httpServer.Addr, mux.Port(), len(public), media.describe())
	if err := httpServer.ListenAndServe(); err != nil && !errors.Is(err, http.ErrServerClosed) {
		log.Fatalf("forkmesh-calls: http: %v", err)
	}
}

type fmm struct {
	server *fmmserver.Server
	certs  *fmmserver.CertStore
	ep     *quic.Endpoint
	port   int
}

func fmmStateDir() string {
	if dir := env("FORKMESH_CALLS_FMM_STATE", ""); dir != "" {
		return dir
	}
	first, _, _ := strings.Cut(os.Getenv("STATE_DIRECTORY"), ":")
	return first
}

func startFMM(ctx context.Context, address, state string, secret []byte, allow func(string) bool, engines *callsignal.RoomEngines) (*fmm, error) {
	if address == "" {
		return nil, nil
	}
	if state == "" {
		return nil, errors.New("FORKMESH_CALLS_FMM needs a state directory for its certificates")
	}
	certs, err := fmmserver.NewCertStore(state, nil)
	if err != nil {
		return nil, err
	}
	server, err := fmmserver.New(fmmserver.Config{
		Secret: secret, AllowOrigin: allow, Engines: engines, Certs: certs,
	})
	if err != nil {
		return nil, err
	}
	ep, err := quic.Listen("udp", address, server.QUICConfig())
	if err != nil {
		return nil, err
	}
	media := &fmm{server: server, certs: certs, ep: ep, port: ep.LocalAddr().(*net.UDPAddr).Port}
	go certs.Run(ctx.Done(), log.Printf)
	go func() {
		if err := server.Serve(ctx, ep); err != nil && ctx.Err() == nil {
			log.Printf("forkmesh-calls: fm media stopped: %v", err)
		}
	}()
	go func() {
		<-ctx.Done()
		server.Close()
		_ = ep.Close()
	}()
	return media, nil
}

func (m *fmm) describe() string {
	if m == nil {
		return "off"
	}
	return "udp port " + strings.TrimPrefix(m.ep.LocalAddr().String(), "[::]")
}

func (m *fmm) health() map[string]any {
	if m == nil {
		return nil
	}
	return map[string]any{
		"port":   m.port,
		"path":   fmmserver.Path,
		"alpn":   fmmserver.ALPN,
		"hashes": m.certs.Hashes(),
	}
}

func routes(webrtc http.Handler, media *fmm) http.Handler {
	mux := http.NewServeMux()
	mux.Handle("/", webrtc)
	mux.HandleFunc("/calls/health", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		w.Header().Set("Cache-Control", "no-store")
		body := map[string]any{"ok": true}
		if info := media.health(); info != nil {
			body["fmm"] = info
		}
		_ = json.NewEncoder(w).Encode(body)
	})
	return mux
}
