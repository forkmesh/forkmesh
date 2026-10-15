package agentmodel

import (
	"encoding/json"
	"strings"
	"sync"
	"testing"
	"time"

	"forkmesh.com/desktop-core/internal/agents"
	"forkmesh.com/desktop-core/internal/coretest"
	"forkmesh.com/desktop-core/internal/qtjson"
	"forkmesh.com/desktop-core/internal/rpc"
)

type fakeProcs struct {
	mu     sync.Mutex
	table  processTable
	killed [][]int64
}

func (f *fakeProcs) scan() processTable {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.table
}

func (f *fakeProcs) set(entries map[int64]processEntry) {
	f.mu.Lock()
	defer f.mu.Unlock()
	table := processTable{entries: entries, children: map[int64][]int64{}}
	for pid, entry := range entries {
		table.children[entry.parent] = append(table.children[entry.parent], pid)
	}
	f.table = table
}

func (f *fakeProcs) kill(pids []int64) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.killed = append(f.killed, pids)
}

type fakeClock struct {
	mu  sync.Mutex
	now time.Time
}

func (c *fakeClock) Now() time.Time {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.now
}

func (c *fakeClock) advance(d time.Duration) {
	c.mu.Lock()
	c.now = c.now.Add(d)
	c.mu.Unlock()
}

func backgroundHarness(t *testing.T) (*harness, *fakeProcs, *fakeClock) {
	h := newHarness(t)
	procs := &fakeProcs{}
	procs.set(map[int64]processEntry{})
	clock := &fakeClock{now: time.Unix(1791100000, 0)}
	h.model.now = clock.Now
	h.model.background.scan = procs.scan
	h.model.background.kill = procs.kill
	return h, procs, clock
}

func uiEvents(t *testing.T, peer *coretest.Peer, count int) []qtjson.Value {
	t.Helper()
	out := []qtjson.Value{}
	for len(out) < count {
		out = append(out, decode(t, peer.Next(t, backgroundEventName, 3*time.Second)))
	}
	return out
}

func TestBackgroundWorkDrainsThenCompletesWithTheDesktop(t *testing.T) {
	h, procs, clock := backgroundHarness(t)
	h.attach(true)
	ui := coretest.NewPeer("ui:2", rpc.UI)
	h.core.Hub.Add(ui)
	session := h.create("owner", "alice", "name", "demo", "status", "running")
	procs.set(map[int64]processEntry{
		100: {parent: 1, command: "claude"},
		101: {parent: 100, command: "ninja", cpuTicks: 5},
		102: {parent: 101, command: "cc1plus", cpuTicks: 7},
		103: {parent: 100, command: "dead", zombie: true},
	})
	h.call(h.ui, "core.agents.background.watch", map[string]any{"id": session.ID, "pid": 100}, nil)
	first := uiEvents(t, ui, 1)[0]
	if first.Get("count").Int() != 2 || !first.Get("busy").BoolValue() || !first.Get("changed").BoolValue() ||
		!strings.HasPrefix(first.Get("notice").Str(), "The turn is over but 2 background tasks started here are still running (") ||
		!strings.HasSuffix(first.Get("notice").Str(), "). This session stays Running until they finish.") {
		t.Fatalf("first %s", first.Compact())
	}
	clock.advance(250 * time.Millisecond)
	second := uiEvents(t, ui, 1)[0]
	if second.Get("busy").BoolValue() || second.Get("changed").BoolValue() == false || second.Get("notice").Str() != "" {
		t.Fatalf("idle poll %s", second.Compact())
	}
	var adopted struct{ Adopted bool }
	h.call(h.ui, "core.agents.background.watch", map[string]any{"id": session.ID, "pid": 100}, &adopted)
	if !adopted.Adopted {
		t.Fatal("a re-attaching desktop adopts the running check")
	}
	procs.set(map[int64]processEntry{100: {parent: 1, command: "claude"}})
	var done qtjson.Value
	for i := 0; i < 20 && !done.Get("done").BoolValue(); i++ {
		done = uiEvents(t, ui, 1)[0]
	}
	if !done.Get("done").BoolValue() || done.Get("notice").Str() != "" {
		t.Fatalf("done %s", done.Compact())
	}
	if h.model.Busy() {
		t.Fatal("a finished check does not keep the core busy")
	}
	stored, _ := h.store.Writer().Store().FindSession(session.ID)
	if stored.Status != "running" {
		t.Fatalf("with a desktop attached the desktop makes the transition: %s", stored.Status)
	}
}

func TestBackgroundWorkKillsIdleStragglersAndCompletesWithoutTheDesktop(t *testing.T) {
	h, procs, clock := backgroundHarness(t)
	session := h.create("owner", "alice", "name", "demo", "status", "running")
	procs.set(map[int64]processEntry{200: {parent: 1}, 201: {parent: 200, command: "sleep", cpuTicks: 3}})
	h.call(h.ui, "core.agents.background.watch", map[string]any{"id": session.ID, "pid": 200}, nil)
	waitFor(t, "announced", func() bool {
		return h.model.Busy() && len(h.model.background.list()) == 1 &&
			h.model.background.list()[0]["polls"].(int) >= 1
	})
	clock.advance(9 * time.Second)
	waitFor(t, "stragglers killed", func() bool {
		procs.mu.Lock()
		defer procs.mu.Unlock()
		return len(procs.killed) == 1 && procs.killed[0][0] == 201
	})
	clock.advance(11 * time.Second)
	waitFor(t, "completed", func() bool {
		stored, _ := h.store.Writer().Store().FindSession(session.ID)
		return stored.Status == "success" && stored.FinishedAtMs > 0
	})
	h.store.Writer().Flush()
	text := h.store.Writer().Store().ReadLog(storedSession(h, session.ID))
	for _, want := range []string{
		"\n==> The turn is over but 1 background task started here is still running (sleep (PID 201)). This session stays Running until it finish.\n",
		"\n==> Stopping 1 background task that outlived the turn without using any CPU: sleep (PID 201).\n",
		"\n==> 1 background task ignored the stop signal and is still running (sleep (PID 201)); completing this session anyway.\n",
	} {
		if !strings.Contains(text, want) {
			t.Fatalf("transcript %q lacks %q", text, want)
		}
	}
	_ = json.RawMessage{}
}

func TestBackgroundWorkStopsWhenTheSessionLeavesRunning(t *testing.T) {
	h, procs, _ := backgroundHarness(t)
	session := h.create("owner", "alice", "name", "demo", "status", "running")
	procs.set(map[int64]processEntry{300: {parent: 1}, 301: {parent: 300, command: "x"}})
	h.call(h.ui, "core.agents.background.watch", map[string]any{"id": session.ID, "pid": 300}, nil)
	h.save(session, func(record *qtjson.Value) { record.Set("status", qtjson.NewString("waiting")) })
	waitFor(t, "check dropped", func() bool { return !h.model.Busy() })
	h.fails(h.phone, "core.agents.background.watch", map[string]any{"id": session.ID}, "forbidden")
	h.call(h.ui, "core.agents.background.watch", map[string]any{"id": session.ID, "pid": 300}, nil)
	var cancelled struct{ Cancelled bool }
	h.call(h.ui, "core.agents.background.cancel", map[string]any{"id": session.ID}, &cancelled)
	if !cancelled.Cancelled && h.model.Busy() {
		t.Fatal("cancel")
	}
}

func storedSession(h *harness, id int) agents.Session {
	session, _ := h.store.Writer().Store().FindSession(id)
	return session
}
