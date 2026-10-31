package main

import (
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"testing"

	"forkmesh.com/mirror-node/rtc/fmmserver"
	callsignal "forkmesh.com/mirror-node/rtc/signal"
)

func TestHealthAdvertisesFMMOnlyWhenItListens(t *testing.T) {
	webrtc := http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { w.WriteHeader(299) })
	off := httptest.NewRecorder()
	routes(webrtc, nil).ServeHTTP(off, httptest.NewRequest("GET", "/calls/health", nil))
	var body map[string]any
	if err := json.Unmarshal(off.Body.Bytes(), &body); err != nil || body["ok"] != true || body["fmm"] != nil {
		t.Fatalf("health without fm media: %s", off.Body.String())
	}
	passed := httptest.NewRecorder()
	routes(webrtc, nil).ServeHTTP(passed, httptest.NewRequest("GET", "/calls/ws", nil))
	if passed.Code != 299 {
		t.Fatalf("/calls/ws not handed to the WebRTC server: %d", passed.Code)
	}

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	if _, err := startFMM(ctx, "127.0.0.1:0", "", []byte("0123456789abcdef0123456789abcdef"), nil, nil); err == nil {
		t.Fatal("fm media started without a state directory")
	}
	state := t.TempDir()
	media, err := startFMM(ctx, "127.0.0.1:0", state, []byte("0123456789abcdef0123456789abcdef"),
		func(string) bool { return true }, callsignal.NewRoomEngines(10))
	if err != nil {
		t.Fatal(err)
	}
	on := httptest.NewRecorder()
	routes(webrtc, media).ServeHTTP(on, httptest.NewRequest("GET", "/calls/health", nil))
	var full struct {
		OK  bool `json:"ok"`
		FMM struct {
			Port   int      `json:"port"`
			Path   string   `json:"path"`
			ALPN   string   `json:"alpn"`
			Hashes []string `json:"hashes"`
		} `json:"fmm"`
	}
	if err := json.Unmarshal(on.Body.Bytes(), &full); err != nil {
		t.Fatal(err)
	}
	if !full.OK || full.FMM.Port != media.port || full.FMM.Path != fmmserver.Path || full.FMM.ALPN != fmmserver.ALPN || len(full.FMM.Hashes) != 2 {
		t.Fatalf("health %s", on.Body.String())
	}
	for _, name := range []string{"fmm-current.pem", "fmm-next.pem"} {
		info, err := os.Stat(filepath.Join(state, name))
		if err != nil || info.Mode().Perm() != 0o600 {
			t.Fatalf("%s: %v", name, err)
		}
	}
	if got := on.Header().Get("Cache-Control"); got != "no-store" {
		t.Fatalf("cache-control %q", got)
	}
}

func TestStateDirectoryComesFromSystemd(t *testing.T) {
	t.Setenv("FORKMESH_CALLS_FMM_STATE", "")
	t.Setenv("STATE_DIRECTORY", "/var/lib/fmnext-calls:/var/lib/other")
	if got := fmmStateDir(); got != "/var/lib/fmnext-calls" {
		t.Fatalf("state dir %q", got)
	}
	t.Setenv("FORKMESH_CALLS_FMM_STATE", "/srv/x")
	if got := fmmStateDir(); got != "/srv/x" {
		t.Fatalf("state dir %q", got)
	}
}
