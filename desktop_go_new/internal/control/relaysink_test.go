package control

import (
	"testing"
	"time"

	"forkmesh.com/desktop-core/internal/coretest"
	"forkmesh.com/desktop-core/internal/rpc"
)

func TestRelaySinkSenderReplacesUIPath(t *testing.T) {
	h := newHarness(t, nil)
	if got, _ := h.core.Get(RelaySinkKey).(*Module); got != h.module {
		t.Fatalf("control did not publish itself as the relay sink")
	}
	ui := coretest.NewPeer("ui", rpc.UI)
	coretest.MustCall(t, h.core, ui, "control.info", map[string]any{"name": "Box", "user": "alice", "node": "Box-1"}, nil)
	sent := make(chan map[string]any, 16)
	h.module.SetRelaySender(func(frame map[string]any) bool {
		sent <- frame
		return true
	})
	if !h.module.RelayOwned() {
		t.Fatal("sender not recorded")
	}
	session := "abcdefghijklmnop0123"
	h.module.RelayIn(map[string]any{"type": "control", "from": "browser", "node": "box-1", "session": session,
		"action": "data", "seq": float64(0), "data": "aGVsbG8="})
	select {
	case frame := <-sent:
		if frame["action"] != "close" || frame["reason"] != "channel_gone" || frame["session"] != session || frame["node"] != "box-1" {
			t.Fatalf("unexpected frame %v", frame)
		}
	case <-time.After(waitTimeout):
		t.Fatal("no close through the in-core sender")
	}
	h.module.RelayRestored()
	h.module.SetRelaySender(nil)
	if h.module.RelayOwned() {
		t.Fatal("sender not cleared")
	}
}

func TestRelaySinkIgnoredBeforeStart(t *testing.T) {
	m := New()
	m.RelayIn(map[string]any{"type": "control"})
	m.RelayRestored()
	m.queueMu.Lock()
	defer m.queueMu.Unlock()
	if len(m.queue) != 0 {
		t.Fatalf("queued %d tasks on a module that never started", len(m.queue))
	}
}
