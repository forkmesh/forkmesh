//go:build !windows

package agentrun

import (
	"encoding/base64"
	"encoding/json"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
	"time"

	"forkmesh.com/desktop-core/internal/agentproc"
	"forkmesh.com/desktop-core/internal/coretest"
	"forkmesh.com/desktop-core/internal/qtjson"
	"forkmesh.com/desktop-core/internal/rpc"
)

func TestRunnerLogIsRedactedAndWrittenByTheCore(t *testing.T) {
	h := newRunHarness(t, nil, "")
	ui := coretest.NewPeer("ui:1", rpc.UI)
	created, _ := h.store.Writer().CreateRecord(qtjson.ObjectOf("schemaVersion", 2, "owner", "dave", "name", "z",
		"provider", "cloudflare-ai", "status", "running"))
	sid := created.Get("id").Int()
	dir, _ := h.store.Writer().DirFor("dave-z", sid)
	h.call(ui, "core.agentrun.credentials", map[string]any{"values": []string{}}, nil)
	key := "runner-" + strconv.Itoa(sid) + "-1"
	var started map[string]any
	h.call(ui, "core.agentproc.start", map[string]any{"key": key, "program": os.Args[0], "args": []string{},
		"env": append(os.Environ(), "FAKE_CLAUDE=1"), "cwd": t.TempDir(), "label": "runner", "detachable": true,
		"merge": true, "skipStdout": true, "secrets": []string{" runkey-123 "},
		"meta": map[string]any{"parse": "log", "runtime": "forkmesh-managed-runner", "sessionId": strconv.Itoa(sid),
			"storeKey": "dave-z"}}, &started)
	if started["parsed"] != true {
		t.Fatalf("start %v", started)
	}
	raw, _ := os.ReadFile(filepath.Join(agentproc.Root(h.core.Layout.State), key, "meta.json"))
	if strings.Contains(string(raw), "runkey") {
		t.Fatalf("the run secret reached meta.json: %s", raw)
	}
	h.call(ui, "core.agentrun.attach", map[string]any{"key": key, "after": 0}, nil)
	h.call(ui, "core.agentproc.write", map[string]any{"key": key,
		"data": base64.StdEncoding.EncodeToString([]byte(userLine("err token runkey-123 sk-abcdefghijklmnopqrstuvwx\n")))}, nil)
	texts := []string{}
	deadline := time.After(20 * time.Second)
	for !strings.Contains(strings.Join(texts, ""), `"type":"result"`) {
		select {
		case event := <-ui.Events:
			if event.Name != "core.agentrun.event" {
				continue
			}
			var payload struct {
				Text string `json:"text"`
				SO   int64  `json:"so"`
			}
			_ = json.Unmarshal(event.Data, &payload)
			if payload.Text == "" || payload.SO <= 0 {
				t.Fatalf("event %s", event.Data)
			}
			texts = append(texts, payload.Text)
		case <-deadline:
			t.Fatalf("timed out: %q", texts)
		}
	}
	joined := strings.Join(texts, "")
	if strings.Contains(joined, "runkey-123") || strings.Contains(joined, "sk-abcdefghij") ||
		!strings.Contains(joined, "token***sk-***") {
		t.Fatalf("not redacted: %q", joined)
	}
	h.call(ui, "core.agentproc.closeStdin", map[string]any{"key": key}, nil)
	ui.Next(t, "core.agentrun.exit", 20*time.Second)
	h.store.Writer().Flush()
	transcript, _ := os.ReadFile(filepath.Join(dir, "transcript.txt"))
	if !strings.Contains(string(transcript), "token***sk-***") || strings.Contains(string(transcript), "runkey") {
		t.Fatalf("transcript %q", transcript)
	}
	if _, err := os.Stat(filepath.Join(dir, "events.jsonl")); err == nil {
		raw, _ := os.ReadFile(filepath.Join(dir, "events.jsonl"))
		if strings.Contains(string(raw), key) {
			t.Fatalf("the runner log wrote events: %s", raw)
		}
	}
}

func TestRunnerLogKeepsSplitRunesTogether(t *testing.T) {
	m := New()
	r := newRun(m, "k", RuntimeLog, 0, "", map[string]any{}, 0)
	word := []byte("café €")
	r.Stdout(word[:4], 0)
	r.Stdout(word[4:7], 4)
	r.Stdout(word[7:], 7)
	r.mu.Lock()
	defer r.mu.Unlock()
	if len(r.logCarry) != 0 || r.next != int64(len(word)) || r.delivered != int64(len(word)) {
		t.Fatalf("carry %q next %d delivered %d", r.logCarry, r.next, r.delivered)
	}
}
