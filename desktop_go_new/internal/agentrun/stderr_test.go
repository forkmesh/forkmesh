//go:build !windows

package agentrun

import (
	"encoding/base64"
	"encoding/json"
	"os"
	"strconv"
	"strings"
	"testing"
	"time"

	"forkmesh.com/desktop-core/internal/coretest"
	"forkmesh.com/desktop-core/internal/qtjson"
	"forkmesh.com/desktop-core/internal/rpc"
)

type stderrEvent struct {
	Stderr string `json:"stderr"`
	EO     int64  `json:"eo"`
	Replay bool   `json:"replay"`
	Line   string `json:"line"`
}

func nextStderr(t *testing.T, peer *coretest.Peer, timeout time.Duration) (stderrEvent, bool) {
	t.Helper()
	deadline := time.After(timeout)
	for {
		select {
		case event := <-peer.Events:
			if event.Name == "core.agentproc.stderr" {
				t.Fatalf("the process attachment delivered stderr despite skipStderr: %s", event.Data)
			}
			if event.Name != "core.agentrun.event" {
				continue
			}
			var payload stderrEvent
			_ = json.Unmarshal(event.Data, &payload)
			if payload.Stderr != "" && payload.Line == "" {
				return payload, true
			}
		case <-deadline:
			return stderrEvent{}, false
		}
	}
}

func TestParsedRunStderrIsDeliveredByTheCoreAndReplayed(t *testing.T) {
	h := newRunHarness(t, nil, "")
	ui := coretest.NewPeer("ui:1", rpc.UI)
	created, _ := h.store.Writer().CreateRecord(qtjson.ObjectOf("schemaVersion", 2, "owner", "carol", "name", "y",
		"provider", "claude-code", "status", "running"))
	sid := created.Get("id").Int()
	h.call(ui, "core.agentrun.credentials", map[string]any{"values": []string{"hushhushvalue"}}, nil)
	key := "agent-" + strconv.Itoa(sid) + "-3"
	var started map[string]any
	h.call(ui, "core.agentproc.start", map[string]any{"key": key, "program": os.Args[0], "args": []string{},
		"env": append(os.Environ(), "FAKE_CLAUDE=1"), "cwd": t.TempDir(), "label": "claude", "detachable": true,
		"meta":       map[string]any{"parse": "claude", "sessionId": strconv.Itoa(sid), "storeKey": "carol-y"},
		"skipStdout": true, "skipStderr": true}, &started)
	if started["parsedStderr"] != true {
		t.Fatalf("start %v", started)
	}
	var state map[string]any
	h.call(ui, "core.agentrun.attach", map[string]any{"key": key, "after": 0, "stderr": true}, &state)
	if state["stderr"] != true {
		t.Fatalf("attach %v", state)
	}
	send := func(text string) {
		h.call(ui, "core.agentproc.write", map[string]any{"key": key,
			"data": base64.StdEncoding.EncodeToString([]byte(userLine(text)))}, nil)
	}
	send("err warning: token hushhushvalue café\n")
	first, ok := nextStderr(t, ui, 20*time.Second)
	if !ok || first.Stderr != "warning: token *** café\n" || first.Replay {
		t.Fatalf("first stderr %+v", first)
	}
	send("err second line\n")
	second, ok := nextStderr(t, ui, 20*time.Second)
	if !ok || second.Stderr != "second line\n" || second.EO <= first.EO {
		t.Fatalf("second stderr %+v", second)
	}
	h.call(ui, "core.agentrun.detach", map[string]any{"key": key}, nil)

	resumed := coretest.NewPeer("ui:2", rpc.UI)
	h.call(resumed, "core.agentrun.attach", map[string]any{"key": key, "after": 0, "stderr": true,
		"stderrAfter": first.EO}, &state)
	replay, ok := nextStderr(t, resumed, 5*time.Second)
	if !ok || !replay.Replay || replay.Stderr != "second line\n" || replay.EO != second.EO {
		t.Fatalf("replay after %d: %+v", first.EO, replay)
	}
	if extra, more := nextStderr(t, resumed, 300*time.Millisecond); more {
		t.Fatalf("stderr delivered twice: %+v", extra)
	}

	fresh := coretest.NewPeer("ui:3", rpc.UI)
	h.call(fresh, "core.agentrun.attach", map[string]any{"key": key, "after": 0, "stderr": true}, &state)
	all, ok := nextStderr(t, fresh, 5*time.Second)
	if !ok || !strings.HasPrefix(all.Stderr, "warning: token ***") || !strings.HasSuffix(all.Stderr, "second line\n") {
		t.Fatalf("fresh replay %+v", all)
	}

	legacy := coretest.NewPeer("ui:4", rpc.UI)
	h.call(legacy, "core.agentrun.attach", map[string]any{"key": key, "after": 0}, nil)
	send("err for the new desktop only\n")
	if got, ok := nextStderr(t, fresh, 20*time.Second); !ok || got.Stderr != "for the new desktop only\n" {
		t.Fatalf("fresh %+v", got)
	}
	if got, ok := nextStderr(t, legacy, 500*time.Millisecond); ok {
		t.Fatalf("a subscriber that did not ask for stderr got %+v", got)
	}
	h.call(fresh, "core.agentproc.closeStdin", map[string]any{"key": key}, nil)
	fresh.Next(t, "core.agentrun.exit", 20*time.Second)
}

func TestCompleteUTF8KeepsSplitRunes(t *testing.T) {
	word := []byte("abé€")
	cases := map[int]int{len(word): len(word), len(word) - 1: 4, len(word) - 2: 4, 4: 4, 3: 2, 2: 2}
	for size, want := range cases {
		if got := completeUTF8(word[:size]); got != want {
			t.Fatalf("completeUTF8(%q) = %d, want %d", word[:size], got, want)
		}
	}
}
