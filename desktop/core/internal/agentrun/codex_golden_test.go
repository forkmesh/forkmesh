package agentrun

import (
	"encoding/json"
	"os"
	"path/filepath"
	"testing"

	"forkmesh.com/desktop-core/internal/qtjson"
	"forkmesh.com/desktop-core/internal/rpc"
)

type codexScenario struct {
	Name   string         `json:"name"`
	Launch map[string]any `json:"launch"`
	Ops    []struct {
		Feed      *string  `json:"feed"`
		Send      *string  `json:"send"`
		Respond   []string `json:"respond"`
		Options   []string `json:"options"`
		Interrupt bool     `json:"interrupt"`
	} `json:"ops"`
	Steps []struct {
		OK     *bool             `json:"ok"`
		Writes []string          `json:"writes"`
		Events []string          `json:"events"`
		Models []json.RawMessage `json:"models"`
	} `json:"steps"`
}

type capturePeer struct {
	payloads []map[string]any
	done     chan struct{}
}

func (p *capturePeer) ID() string               { return "capture" }
func (p *capturePeer) Transport() rpc.Transport { return rpc.UI }
func (p *capturePeer) Device() rpc.Device       { return rpc.Device{} }
func (p *capturePeer) Done() <-chan struct{}    { return p.done }
func (p *capturePeer) Send(event string, data any) bool {
	if payload, ok := data.(map[string]any); ok {
		p.payloads = append(p.payloads, payload)
	}
	return true
}

func stripCore(line string) string {
	event, ok := qtjson.ParseObject([]byte(line))
	if !ok {
		return line
	}
	for _, key := range []string{"_so", "_sk", "_ts", "_forkmesh_account"} {
		event.Remove(key)
	}
	return string(event.Compact())
}

func TestCodexProtocolMatchesCpp(t *testing.T) {
	data, err := os.ReadFile(filepath.Join("..", "qtjson", "testdata", "codex_golden.json"))
	if err != nil {
		t.Fatal(err)
	}
	var scenarios []codexScenario
	if err := json.Unmarshal(data, &scenarios); err != nil {
		t.Fatal(err)
	}
	if len(scenarios) < 2 {
		t.Fatal("missing scenarios")
	}
	for _, sc := range scenarios {
		m, _, dir := testModule(t, 1759512345678)
		m.haveCreds = true
		meta := map[string]any{"parse": "codex"}
		for key, value := range sc.Launch {
			meta[key] = value
		}
		r := newRun(m, "agent-5-codex", RuntimeCodex, 5, dir, meta, 0)
		writes := []string{}
		r.codex.writeHook = func(line []byte) bool { writes = append(writes, string(line)); return true }
		r.codex.runningHook = func() bool { return true }
		peer := &capturePeer{done: make(chan struct{})}
		r.subscribers[peer.ID()] = &subscriber{peer: peer, stop: make(chan struct{})}
		r.mu.Lock()
		r.codex.start()
		r.mu.Unlock()
		offset := int64(0)
		compared := 0
		check := func(step int, ok *bool, got bool) {
			want := sc.Steps[step]
			if ok != nil && *ok != got {
				t.Errorf("%s step %d: ok %v, C++ %v", sc.Name, step, got, *ok)
			}
			if len(writes) != len(want.Writes) {
				t.Fatalf("%s step %d writes:\n got %q\nwant %q", sc.Name, step, writes, want.Writes)
			}
			for i := range writes {
				if writes[i] != want.Writes[i] {
					t.Fatalf("%s step %d write %d:\n got %s\nwant %s", sc.Name, step, i, writes[i], want.Writes[i])
				}
			}
			events := []string{}
			models := 0
			for _, payload := range peer.payloads {
				if line, ok := payload["line"].(string); ok {
					events = append(events, stripCore(line))
				}
				if _, ok := payload["models"]; ok {
					models++
				}
			}
			if len(events) != len(want.Events) {
				t.Fatalf("%s step %d events:\n got %q\nwant %q", sc.Name, step, events, want.Events)
			}
			for i := range events {
				if events[i] != want.Events[i] {
					t.Fatalf("%s step %d event %d:\n got %s\nwant %s", sc.Name, step, i, events[i], want.Events[i])
				}
			}
			if models != len(want.Models) {
				t.Fatalf("%s step %d models %d, C++ %d", sc.Name, step, models, len(want.Models))
			}
			compared += len(events) + len(want.Writes)
			writes = writes[:0]
			peer.payloads = nil
		}
		check(0, nil, true)
		for i, op := range sc.Ops {
			ok := true
			switch {
			case op.Feed != nil:
				line := []byte(*op.Feed + "\n")
				r.Stdout(line, offset)
				offset += int64(len(line))
			case op.Send != nil:
				result, _ := r.codexOp(codexOp{Op: "send", Text: *op.Send})
				ok = result.(map[string]any)["ok"].(bool)
			case op.Respond != nil:
				result, _ := r.codexOp(codexOp{Op: "respond", Token: op.Respond[0], Answer: op.Respond[1]})
				ok = result.(map[string]any)["ok"].(bool)
			case op.Options != nil:
				_, _ = r.codexOp(codexOp{Op: "options", Model: op.Options[0], Mode: op.Options[1], Effort: op.Options[2]})
			case op.Interrupt:
				_, _ = r.codexOp(codexOp{Op: "interrupt"})
			}
			check(i+1, sc.Steps[i+1].OK, ok)
		}
		if compared < 10 {
			t.Fatalf("%s compared only %d items", sc.Name, compared)
		}
		t.Logf("%s: %d events and writes match the C++ session", sc.Name, compared)
	}
}
