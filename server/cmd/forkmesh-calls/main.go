package main

import (
	"context"
	"errors"
	"log"
	"net/http"
	"net/netip"
	"os"
	"os/signal"
	"path/filepath"
	"strings"
	"syscall"
	"time"

	"forkmesh.com/mirror-node/rtc/ice"
	"forkmesh.com/mirror-node/rtc/sfu"
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
	return func(origin string) bool { return allowed[origin] }
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
	server, err := sfu.NewServer(sfu.Config{
		Secret:      []byte(secret),
		Addresses:   public,
		AllowOrigin: origins(env("FORKMESH_CALLS_ORIGINS", "https://app.forkmesh.com")),
	}, mux)
	if err != nil {
		log.Fatalf("forkmesh-calls: %v", err)
	}
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	go func() {
		if err := mux.Serve(); err != nil {
			log.Printf("forkmesh-calls: udp stopped: %v", err)
		}
	}()
	go server.Run(ctx)
	httpServer := &http.Server{
		Addr:              env("FORKMESH_CALLS_HTTP", "127.0.0.1:7881"),
		Handler:           server.Handler(),
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
	log.Printf("forkmesh-calls: http %s, udp port %d, %d public address(es)", httpServer.Addr, mux.Port(), len(public))
	if err := httpServer.ListenAndServe(); err != nil && !errors.Is(err, http.ErrServerClosed) {
		log.Fatalf("forkmesh-calls: http: %v", err)
	}
}
