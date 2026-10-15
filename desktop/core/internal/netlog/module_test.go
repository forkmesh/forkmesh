package netlog

import (
	"os"
	"path/filepath"
	"testing"
	"time"

	"forkmesh.com/desktop-core/internal/coretest"
	"forkmesh.com/desktop-core/internal/rpc"
)

func TestModuleRoutesAndGates(t *testing.T) {
	core := coretest.New(t)
	now := time.Now()
	module := New()
	module.Owners = func() []string { return []string{"alice"} }
	coretest.Start(t, core, module)
	live := LivePath(core.Layout.Data)
	if err := os.MkdirAll(filepath.Dir(live), 0o700); err != nil {
		t.Fatal(err)
	}
	stamp := now.Format(storedFormat)
	if err := os.WriteFile(live, []byte(stamp+"  Could not reach the relay\n"), 0o600); err != nil {
		t.Fatal(err)
	}

	owner := coretest.NewPeer("phone", rpc.LAN)
	owner.Dev.User = "alice"
	stranger := coretest.NewPeer("other", rpc.Relay)
	stranger.Dev.User = "mallory"
	ui := coretest.NewPeer("ui", rpc.UI)

	if _, err := coretest.Call(t, core, stranger, "stats.timeline", map[string]any{}); rpc.Code(err) != "not_owner" {
		t.Fatalf("stranger = %v", err)
	}
	if _, err := coretest.Call(t, core, owner, "core.log.append", map[string]any{"line": stamp + "  x"}); rpc.Code(err) != "forbidden" {
		t.Fatalf("phone append = %v", err)
	}
	var reply struct {
		Days []struct {
			DayTs    float64 `json:"dayTs"`
			Errors   int     `json:"errors"`
			Warnings int     `json:"warnings"`
			Info     int     `json:"info"`
		} `json:"days"`
	}
	coretest.MustCall(t, core, owner, "stats.timeline", map[string]any{"days": 2}, &reply)
	if len(reply.Days) != 2 || reply.Days[1].Errors != 1 || reply.Days[1].DayTs != float64(LocalDayStartMs(now.UnixMilli())) {
		t.Fatalf("timeline = %+v", reply)
	}
	var accepted struct {
		Accepted int `json:"accepted"`
	}
	coretest.MustCall(t, core, ui, "core.log.append", map[string]any{"lines": []map[string]string{
		{"line": stamp + "  UI stalled for 900 ms", "badge": "STALL"},
		{"line": "no stamp", "badge": "INFO"},
	}}, &accepted)
	if accepted.Accepted != 1 {
		t.Fatalf("accepted = %d", accepted.Accepted)
	}
	coretest.MustCall(t, core, owner, "stats.timeline", map[string]any{"days": 1}, &reply)
	if len(reply.Days) != 1 || reply.Days[0].Errors != 1 || reply.Days[0].Warnings != 1 {
		t.Fatalf("after append = %+v", reply)
	}
}
