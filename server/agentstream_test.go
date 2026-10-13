package mirrornode

import (
	"bytes"
	"context"
	"encoding/base64"
	"encoding/json"
	"io"
	"strings"
	"sync"
	"testing"
	"time"
)

type fakeStreamRelay struct {
	mu      sync.Mutex
	watched bool
	status  int
	fail    error
	seq     int64
	data    []byte
	posts   []map[string]any
}

func (f *fakeStreamRelay) post(_ context.Context, fields map[string]any) (int, []byte, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	copied := map[string]any{}
	for key, value := range fields {
		copied[key] = value
	}
	f.posts = append(f.posts, copied)
	if f.fail != nil {
		return 0, nil, f.fail
	}
	if f.status != 0 {
		return f.status, []byte(`{"ok":false}`), nil
	}
	seq, _ := fields["seq"].(int64)
	if encoded, ok := fields["data"].(string); ok {
		if seq != f.seq+1 {
			return 409, []byte(`{"ok":false,"error":"out_of_order"}`), nil
		}
		raw, _ := base64.StdEncoding.DecodeString(encoded)
		if len(raw) == 0 || len(raw) > agentStreamChunk {
			return 400, nil, nil
		}
		f.seq = seq
		f.data = append(f.data, raw...)
	}
	reply, _ := json.Marshal(map[string]any{"ok": true, "watched": f.watched, "seq": f.seq})
	return 200, reply, nil
}

func (f *fakeStreamRelay) snapshot() (int, []byte) {
	f.mu.Lock()
	defer f.mu.Unlock()
	return len(f.posts), append([]byte(nil), f.data...)
}

type fakeClock struct{ at time.Time }

func (c *fakeClock) now() time.Time              { return c.at }
func (c *fakeClock) advance(delta time.Duration) { c.at = c.at.Add(delta) }

func newTestStream(relay *fakeStreamRelay) (*agentStream, *fakeClock) {
	clock := &fakeClock{at: time.UnixMilli(2_000_000_000_000)}
	stream := newAgentStream(relay.post)
	stream.now = clock.now
	return stream, clock
}

func streamEvent(text string) string {
	raw, _ := json.Marshal(map[string]any{"type": "assistant", "message": map[string]any{
		"content": []any{map[string]any{"type": "text", "text": text}}}})
	return string(raw)
}

func TestAgentStreamSendsOnlyHeartbeatsWhileUnwatched(t *testing.T) {
	relay := &fakeStreamRelay{}
	stream, clock := newTestStream(relay)
	for i := 0; i < 50; i++ {
		stream.line([]byte(streamEvent(strings.Repeat("x", 500))))
	}
	stream.flush(context.Background(), false)
	posts, data := relay.snapshot()
	if posts != 1 || len(data) != 0 {
		t.Fatalf("posts=%d data=%d, want one heartbeat and no data", posts, len(data))
	}
	if _, ok := relay.posts[0]["data"]; ok || relay.posts[0]["seq"] != int64(0) {
		t.Fatalf("heartbeat carried %v", relay.posts[0])
	}
	stream.flush(context.Background(), false)
	if posts, _ := relay.snapshot(); posts != 1 {
		t.Fatalf("a second heartbeat went out inside the interval: %d posts", posts)
	}
	clock.advance(agentStreamHeartbeat)
	stream.flush(context.Background(), false)
	if posts, data := relay.snapshot(); posts != 2 || len(data) != 0 {
		t.Fatalf("posts=%d data=%d after the interval", posts, len(data))
	}
	stream.flush(context.Background(), true)
	if posts, _ := relay.snapshot(); posts != 2 {
		t.Fatal("the final flush of an unwatched run posted")
	}
}

func TestAgentStreamSendsTheBacklogInNumberedChunksOnceWatched(t *testing.T) {
	relay := &fakeStreamRelay{watched: true}
	stream, clock := newTestStream(relay)
	var want bytes.Buffer
	for i := 0; i < 40; i++ {
		line := streamEvent(strings.Repeat(string(rune('a'+i%26)), 900))
		stream.line([]byte(line + "\n"))
		want.WriteString(line + "\n")
	}
	stream.flush(context.Background(), false)
	_, data := relay.snapshot()
	if len(data) == 0 || len(data) >= want.Len() {
		t.Fatalf("first tick sent %d of %d bytes; want a bounded catch-up", len(data), want.Len())
	}
	for i := 0; i < 20; i++ {
		clock.advance(agentStreamTick)
		stream.flush(context.Background(), false)
	}
	_, data = relay.snapshot()
	if !bytes.Equal(data, want.Bytes()) {
		t.Fatalf("relayed %d bytes, want the exact %d-byte event stream", len(data), want.Len())
	}
	for _, post := range relay.posts {
		if encoded, ok := post["data"].(string); ok {
			raw, _ := base64.StdEncoding.DecodeString(encoded)
			if len(raw) > agentStreamChunk {
				t.Fatalf("chunk of %d bytes", len(raw))
			}
		}
	}
	if relay.seq < 4 {
		t.Fatalf("seq %d; want the stream split into numbered chunks", relay.seq)
	}
}

func TestAgentStreamStopsWhenTheViewerLeaves(t *testing.T) {
	relay := &fakeStreamRelay{watched: true}
	stream, clock := newTestStream(relay)
	stream.line([]byte(streamEvent("first")))
	stream.flush(context.Background(), false)
	relay.mu.Lock()
	relay.watched = false
	relay.mu.Unlock()
	stream.line([]byte(streamEvent("second")))
	clock.advance(agentStreamTick)
	stream.flush(context.Background(), false)
	_, data := relay.snapshot()
	stream.line([]byte(streamEvent("third")))
	for i := 0; i < 3; i++ {
		clock.advance(agentStreamTick)
		stream.flush(context.Background(), false)
	}
	posts, after := relay.snapshot()
	if !bytes.Equal(data, after) || strings.Contains(string(after), "third") {
		t.Fatalf("an unwatched stream kept sending: %q", after)
	}
	if posts != 3 {
		t.Fatalf("posts=%d; want no heartbeat inside the interval", posts)
	}
	relay.mu.Lock()
	relay.watched = true
	relay.mu.Unlock()
	clock.advance(agentStreamHeartbeat)
	stream.flush(context.Background(), false)
	_, data = relay.snapshot()
	if !strings.Contains(string(data), "third") {
		t.Fatalf("the backlog was not sent when a viewer came back: %q", data)
	}
}

func TestAgentStreamBacklogIsBoundedAndStartsOnALine(t *testing.T) {
	relay := &fakeStreamRelay{}
	stream, _ := newTestStream(relay)
	for i := 0; i < 400; i++ {
		stream.line([]byte(streamEvent(strings.Repeat("y", 1000))))
	}
	stream.mu.Lock()
	size := len(stream.pending)
	stream.mu.Unlock()
	if size > agentStreamBacklog {
		t.Fatalf("backlog %d bytes, limit %d", size, agentStreamBacklog)
	}
	relay.watched = true
	stream.flush(context.Background(), false)
	for i := 0; i < 10; i++ {
		stream.flush(context.Background(), true)
	}
	_, data := relay.snapshot()
	lines := strings.Split(strings.TrimSuffix(string(data), "\n"), "\n")
	for _, line := range lines {
		if !json.Valid([]byte(line)) {
			t.Fatalf("relayed a partial event: %.60q", line)
		}
	}
}

func TestAgentStreamTrimAfterASplitLineTerminatesThePartial(t *testing.T) {
	relay := &fakeStreamRelay{watched: true}
	stream, _ := newTestStream(relay)
	stream.line([]byte(streamEvent(strings.Repeat("z", agentStreamChunk+100))))
	stream.flush(context.Background(), false)
	if !stream.midLine {
		t.Fatal("the first chunk should have ended inside the event")
	}
	relay.mu.Lock()
	relay.watched = false
	relay.mu.Unlock()
	stream.watched = false
	for i := 0; i < 400; i++ {
		stream.line([]byte(streamEvent(strings.Repeat("w", 1000))))
	}
	_, before := relay.snapshot()
	relay.mu.Lock()
	relay.watched = true
	relay.mu.Unlock()
	stream.lastPost = time.Time{}
	stream.flush(context.Background(), false)
	_, after := relay.snapshot()
	if len(after) <= len(before) || after[len(before)] != '\n' {
		t.Fatal("the relayed partial event was not closed before the next event")
	}
}

func TestAgentStreamScrubsTokensAndShortensHugeEvents(t *testing.T) {
	relay := &fakeStreamRelay{watched: true}
	stream, _ := newTestStream(relay)
	stream.line([]byte(streamEvent("key sk-ant-api03-SECRETSECRETSECRET here"))) // forkmesh-secret-scan:ignore-line
	huge, _ := json.Marshal(map[string]any{"type": "user", "message": map[string]any{
		"content": []any{map[string]any{"type": "tool_result", "tool_use_id": "t1",
			"content": strings.Repeat("<b>", agentStreamEventLimit)}}}})
	stream.line(huge)
	stream.line([]byte("not json"))
	stream.line([]byte(`{"type":"assistant"`))
	stream.flush(context.Background(), false)
	_, data := relay.snapshot()
	text := string(data)
	if strings.Contains(text, "SECRET") || !strings.Contains(text, "[redacted]") {
		t.Fatalf("token not scrubbed: %q", text)
	}
	lines := strings.Split(strings.TrimSuffix(text, "\n"), "\n")
	if len(lines) != 2 {
		t.Fatalf("%d events relayed, want the two valid ones", len(lines))
	}
	if len(lines[1]) > agentStreamEventLimit || !strings.Contains(lines[1], "tool_use_id") ||
		!strings.Contains(lines[1], "<b>") || !json.Valid([]byte(lines[1])) {
		t.Fatalf("huge event not shortened into valid JSON: %d bytes", len(lines[1]))
	}
}

func TestAgentStreamRetriesTheSameChunkAndGivesUpOnARefusal(t *testing.T) {
	relay := &fakeStreamRelay{watched: true, fail: io.ErrUnexpectedEOF}
	stream, _ := newTestStream(relay)
	stream.watched = true
	stream.line([]byte(streamEvent("one")))
	stream.flush(context.Background(), false)
	relay.mu.Lock()
	relay.fail = nil
	relay.mu.Unlock()
	stream.flush(context.Background(), false)
	if relay.seq != 1 || relay.posts[0]["seq"] != int64(1) || relay.posts[1]["seq"] != int64(1) {
		t.Fatalf("seq %d; posts %v", relay.seq, relay.posts)
	}
	relay.mu.Lock()
	relay.status = 409
	relay.mu.Unlock()
	stream.line([]byte(streamEvent("two")))
	stream.flush(context.Background(), false)
	stream.line([]byte(streamEvent("three")))
	stream.flush(context.Background(), true)
	if posts, _ := relay.snapshot(); posts != 3 {
		t.Fatalf("posts=%d; a refused stream must stop posting", posts)
	}
}

type streamBackend struct {
	mirrorAgentBackend
	relay *fakeStreamRelay
	paths []string
}

func (b *streamBackend) postStream(ctx context.Context, work agentWork, body []byte) (int, []byte, error) {
	var fields map[string]any
	_ = json.Unmarshal(body, &fields)
	if fields["jobId"] != float64(work.job.JobID) || fields["leaseId"] != "lease-2" {
		return 409, nil, nil
	}
	if seq, ok := fields["seq"].(float64); ok {
		fields["seq"] = int64(seq)
	}
	return b.relay.post(ctx, fields)
}

func TestClaudeRunTapsEveryEventIntoTheWatchedStream(t *testing.T) {
	relay := &fakeStreamRelay{watched: true}
	r := &agentJobRunner{state: agentJobState{Jobs: map[string]*agentJobRecord{
		"7": {JobID: 7, LeaseID: "lease-2"}}}, streamTick: 10 * time.Millisecond}
	r.backend = &streamBackend{relay: relay}
	events := streamEvent("hello") + "\n" + `{"type":"result","result":"done","session_id":"s"}` + "\n"
	command := func(ctx context.Context, dir, stdin string, stdout io.Writer,
		name string, args ...string) ([]byte, error) {
		_, _ = stdout.Write([]byte(events))
		return nil, nil
	}
	tap, stop := r.startStream(context.Background(), agentWork{job: agentJob{JobID: 7, LeaseID: "lease-1"}})
	outcome, err := runClaudeAgent(withAgentStreamTap(context.Background(), tap), command, "",
		&agentSession{}, t.TempDir(), "hi", "", nil)
	stop()
	if err != nil || outcome.text != "done" {
		t.Fatalf("outcome %+v err %v", outcome, err)
	}
	if _, data := relay.snapshot(); string(data) != events {
		t.Fatalf("relayed %q, want %q", data, events)
	}
}

func TestRunnersWithoutAStreamRouteRunUntapped(t *testing.T) {
	r := &agentJobRunner{backend: nil}
	tap, stop := r.startStream(context.Background(), agentWork{job: agentJob{JobID: 0}})
	stop()
	if tap != nil || agentStreamTapFrom(withAgentStreamTap(context.Background(), nil)) != nil {
		t.Fatal("a job with no id must not stream")
	}
}
