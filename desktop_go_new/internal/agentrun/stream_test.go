package agentrun

import (
	"os"
	"path/filepath"
	"testing"
	"time"

	"forkmesh.com/desktop-core/internal/agents"
)

type streamGolden struct {
	ClaudeStream struct {
		Stream      string   `json:"stream"`
		Credentials []string `json:"credentials"`
		Account     string   `json:"account"`
		Key         string   `json:"key"`
		Ts          float64  `json:"ts"`
		Events      string   `json:"events"`
		Transcript  string   `json:"transcript"`
	} `json:"claudeStream"`
}

func testModule(t *testing.T, ts float64) (*Module, *agents.Writer, string) {
	t.Helper()
	root := filepath.Join(t.TempDir(), "agents")
	w := agents.NewWriter(agents.NewStore(root))
	m := New()
	m.w = w
	m.now = func() time.Time { return time.UnixMilli(int64(ts)) }
	dir, _ := w.DirFor("alice-demo", 5)
	return m, w, dir
}

func TestClaudeStreamMatchesCppTranscript(t *testing.T) {
	var g streamGolden
	loadInto(t, &g)
	c := g.ClaudeStream
	stream := b64(t, c.Stream)
	for _, chunk := range []int{1, 3, 17, 64, len(stream)} {
		m, w, dir := testModule(t, c.Ts)
		m.credentials, m.haveCreds = c.Credentials, true
		r := newRun(m, c.Key, RuntimeClaude, 5, dir, map[string]any{"accountId": c.Account}, 0)
		for at := 0; at < len(stream); at += chunk {
			end := min(len(stream), at+chunk)
			r.Stdout(stream[at:end], int64(at))
		}
		w.Flush()
		events, _ := os.ReadFile(filepath.Join(dir, "events.jsonl"))
		if want := string(b64(t, c.Events)); string(events) != want {
			t.Fatalf("chunk %d events:\n got %s\nwant %s", chunk, events, want)
		}
		transcript, _ := os.ReadFile(filepath.Join(dir, "transcript.txt"))
		if want := string(b64(t, c.Transcript)); string(transcript) != want {
			t.Fatalf("chunk %d transcript:\n got %q\nwant %q", chunk, transcript, want)
		}
		state := r.state()
		if state["conversationOpened"] != true || state["lastSessionId"] != "s-1" || state["idle"] != true {
			t.Fatalf("state %v", state)
		}
		if u := state["usage"].(usage); u.NumTurns != 2 || u.DurationMs != 1200 || u.CostUsd != 0.0123 || u.OutputTokens != 7 {
			t.Fatalf("usage %+v", u)
		}
	}
}

func TestStoredEventsAreNotWrittenTwice(t *testing.T) {
	var g streamGolden
	loadInto(t, &g)
	c := g.ClaudeStream
	stream := b64(t, c.Stream)
	m, w, dir := testModule(t, c.Ts)
	m.credentials, m.haveCreds = c.Credentials, true
	first := newRun(m, c.Key, RuntimeClaude, 5, dir, map[string]any{"accountId": c.Account}, 0)
	first.Stdout(stream[:500], 0)
	w.Flush()
	resume := lastStoredOffset(filepath.Join(dir, "events.jsonl"), c.Key)
	if resume != 499 {
		t.Fatalf("resume at %d", resume)
	}
	second := newRun(m, c.Key, RuntimeClaude, 5, dir, map[string]any{"accountId": c.Account}, resume)
	second.Stdout(stream[300:], 300)
	w.Flush()
	events, _ := os.ReadFile(filepath.Join(dir, "events.jsonl"))
	if want := string(b64(t, c.Events)); string(events) != want {
		t.Fatalf("events:\n got %s\nwant %s", events, want)
	}
	replay := storedSince(filepath.Join(dir, "events.jsonl"), c.Key, 205)
	if len(replay) != 4 || replay[0].so != 499 || replay[3].so != 923 {
		t.Fatalf("replay %+v", replay)
	}
}
