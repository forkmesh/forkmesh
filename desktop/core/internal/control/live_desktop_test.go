package control

import (
	"crypto/ed25519"
	"encoding/hex"
	"encoding/json"
	"os"
	"testing"
	"time"
)

func TestLiveDesktop(t *testing.T) {
	addr := os.Getenv("FORKMESH_LIVE_MESH")
	seedHex := os.Getenv("FORKMESH_LIVE_SEED")
	if addr == "" || seedHex == "" {
		t.Skip("set FORKMESH_LIVE_MESH and FORKMESH_LIVE_SEED to drive a running desktop")
	}
	seed, err := hex.DecodeString(seedHex)
	if err != nil || len(seed) != ed25519.SeedSize {
		t.Fatal("FORKMESH_LIVE_SEED must be 64 hex characters")
	}
	private := ed25519.NewKeyFromSeed(seed)
	identity := &signer{private: private, public: ToBase64Url(private.Public().(ed25519.PublicKey))}
	phone := dialPhone(t, addr, identity, false)
	select {
	case <-phone.welcomed:
	case <-time.After(10 * time.Second):
		t.Fatal("no welcome from the desktop")
	}
	type call struct {
		Method string         `json:"method"`
		Params map[string]any `json:"params"`
	}
	calls := []call{}
	if raw := os.Getenv("FORKMESH_LIVE_CALLS"); raw != "" {
		if err := json.Unmarshal([]byte(raw), &calls); err != nil {
			t.Fatalf("FORKMESH_LIVE_CALLS: %v", err)
		}
	} else {
		for _, method := range []string{"ping", "files.browse.roots", "diag.log", "repos.list", "agents.list", "core.status"} {
			calls = append(calls, call{Method: method})
		}
	}
	methods := make([]string, len(calls))
	for i, c := range calls {
		methods[i] = c.Method
		if c.Params == nil {
			c.Params = map[string]any{}
		}
		phone.send(map[string]any{"id": i + 1, "method": c.Method, "params": c.Params})
	}
	limit := 300
	if os.Getenv("FORKMESH_LIVE_FULL") != "" {
		limit = 1 << 20
	}
	for i, method := range methods {
		reply := phone.wait(func(m map[string]any) bool {
			id, ok := m["id"].(float64)
			return ok && int(id) == i+1
		})
		raw, _ := json.Marshal(reply)
		if len(raw) > limit {
			raw = append(raw[:limit], []byte("…")...)
		}
		t.Logf("%s -> %s", method, raw)
	}
}
