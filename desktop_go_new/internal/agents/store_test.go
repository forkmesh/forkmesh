package agents

import (
	"os"
	"path/filepath"
	"reflect"
	"regexp"
	"strings"
	"testing"
	"time"
)

func TestLoadAllSessionsOrderAndSkips(t *testing.T) {
	root := tempRoot(t)
	store := NewStore(root)
	tree := store.SessionsDir()
	writeMeta(t, tree, cppMeta(map[string]any{"id": 1, "createdAtMs": 10, "startedAtMs": 0, "finishedAtMs": 0}))
	writeMeta(t, tree, cppMeta(map[string]any{"id": 2, "createdAtMs": 10, "startedAtMs": 0, "finishedAtMs": 0}))
	writeMeta(t, tree, cppMeta(map[string]any{"id": 3, "owner": "bob", "createdAtMs": 5, "mergedAtMs": 50,
		"startedAtMs": 0, "finishedAtMs": 0}))
	writeFile(t, filepath.Join(tree, "alice-repo", "4", "meta.json"), []byte("{not json"))
	writeFile(t, filepath.Join(tree, "alice-repo", "5", "meta.json"), []byte("[1,2]"))
	writeFile(t, filepath.Join(tree, ".hidden", "6", "meta.json"), []byte(`{"id":6}`))
	writeFile(t, filepath.Join(tree, "alice-repo", ".7", "meta.json"), []byte(`{"id":7}`))
	writeFile(t, filepath.Join(tree, "alice-repo", "named", "meta.json"), append([]byte{0xEF, 0xBB, 0xBF}, []byte(`{"id":8,"createdAtMs":1}`)...))
	if err := os.MkdirAll(filepath.Join(tree, "alice-repo", "9"), 0o700); err != nil {
		t.Fatal(err)
	}
	var ids []int
	for _, s := range store.LoadAllSessions() {
		ids = append(ids, s.ID)
	}
	if !reflect.DeepEqual(ids, []int{3, 2, 1, 8}) {
		t.Fatalf("order %v", ids)
	}
	session, ok := store.FindSession(3)
	if !ok || session.Owner != "bob" {
		t.Fatalf("find %v %+v", ok, session)
	}
	if _, ok := store.FindSession(4); ok {
		t.Fatal("broken meta must not load")
	}
	writeMeta(t, tree, cppMeta(map[string]any{"id": 1, "createdAtMs": 99, "status": "running", "lastError": "changed"}))
	future := time.Now().Add(time.Hour)
	_ = os.Chtimes(filepath.Join(tree, "alice-repo", "1", "meta.json"), future, future)
	first := store.LoadAllSessions()[0]
	if first.ID != 1 || first.Status != "running" {
		t.Fatalf("cache did not refresh: %+v", first)
	}
}

func TestRecycledSessions(t *testing.T) {
	store := NewStore(tempRoot(t))
	bin := store.RecycleBinDir()
	a := writeMeta(t, bin, cppMeta(map[string]any{"id": 11}))
	b := writeMeta(t, bin, cppMeta(map[string]any{"id": 12, "owner": "bob"}))
	writeMeta(t, bin, cppMeta(map[string]any{"id": 13}))
	writeFile(t, filepath.Join(a, "deleted-at.json"), []byte(`{"deletedAtMs":2000}`))
	writeFile(t, filepath.Join(b, "deleted-at.json"), []byte(`{"deletedAtMs":3000}`))
	var got [][2]int64
	for _, s := range store.LoadRecycledSessions() {
		got = append(got, [2]int64{int64(s.ID), s.DeletedAtMs})
	}
	want := [][2]int64{{12, 3000}, {11, 2000}, {13, 0}}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("bin %v", got)
	}
	if dir := store.FindRecycledSessionDir(12); dir != b {
		t.Fatalf("find %q want %q", dir, b)
	}
	if store.FindRecycledSessionDir(99) != "" || store.FindRecycledSessionDir(0) != "" {
		t.Fatal("missing bin entry found")
	}
	if len(store.LoadAllSessions()) != 0 {
		t.Fatal("bin leaked into live sessions")
	}
}

func TestReadLogEventsPatchAndStamp(t *testing.T) {
	store := NewStore(tempRoot(t))
	dir := writeMeta(t, store.SessionsDir(), cppMeta(map[string]any{"id": 1}))
	session, _ := store.FindSession(1)
	if store.TranscriptStamp(session) != "-|-" {
		t.Fatalf("stamp %q", store.TranscriptStamp(session))
	}
	writeFile(t, filepath.Join(dir, "transcript.txt"), []byte("one\r\ntwo\rthree\n"))
	writeFile(t, filepath.Join(dir, "changes.patch"), []byte("diff\r\n"))
	writeFile(t, filepath.Join(dir, "events.jsonl"), []byte("{\"type\":\"a\"}\n\n  \nnot json\n[1]\n{\"type\":\"b\",\"n\":1}\n{\"type\":\"c\"}"))
	if got := store.ReadLog(session); got != "one\ntwothree\n" {
		t.Fatalf("log %q", got)
	}
	if got := store.ReadPatch(session); got != "diff\n" {
		t.Fatalf("patch %q", got)
	}
	events := store.LoadEvents(session)
	if len(events) != 3 || string(events[1]) != `{"type":"b","n":1}` || string(events[2]) != `{"type":"c"}` {
		t.Fatalf("events %q", events)
	}
	stamp := store.TranscriptStamp(session)
	if !regexp.MustCompile(`^15:\d{13}\|\d+:\d{13}$`).MatchString(stamp) {
		t.Fatalf("stamp %q", stamp)
	}
}

func TestReadEventsOffsets(t *testing.T) {
	path := filepath.Join(t.TempDir(), "events.jsonl")
	writeFile(t, path, []byte("{\"a\":1}\n{\"a\":2}\n{\"a\":"))
	events, next, more := readEvents(path, 0, 0)
	if len(events) != 2 || next != 16 || more {
		t.Fatalf("first read %d %d %v", len(events), next, more)
	}
	f, _ := os.OpenFile(path, os.O_APPEND|os.O_WRONLY, 0o600)
	_, _ = f.WriteString("3}\n{\"a\":4}\n")
	f.Close()
	events, next, _ = readEvents(path, next, 0)
	if len(events) != 2 || string(events[0]) != `{"a":3}` || next != 32 {
		t.Fatalf("second read %q %d", events, next)
	}
	events, next, more = readEvents(path, 0, 10)
	if len(events) != 2 || next != 16 || !more {
		t.Fatalf("limited read %d %d %v", len(events), next, more)
	}
}

func TestSearchTranscript(t *testing.T) {
	store := NewStore(tempRoot(t))
	dir := writeMeta(t, store.SessionsDir(), cppMeta(map[string]any{"id": 1}))
	session, _ := store.FindSession(1)
	prefix := strings.Repeat("x", 100)
	writeFile(t, filepath.Join(dir, "transcript.txt"), []byte(prefix+" Needle   in\n the HAYSTACK needle "+strings.Repeat("y", 100)))
	writeFile(t, filepath.Join(dir, "events.jsonl"), []byte(`{"text":"NEEDLE"}`+"\n"))
	count, snippet := store.SearchTranscript(session, "  needle ")
	if count != 3 {
		t.Fatalf("count %d", count)
	}
	want := "…" + strings.Repeat("x", 59) + " Needle in the HAYSTACK needle " + strings.Repeat("y", 33) + "…"
	if snippet != want {
		t.Fatalf("snippet\n%q\nwant\n%q", snippet, want)
	}
	if count, _ := store.SearchTranscript(session, "   "); count != 0 {
		t.Fatal("blank needle must not match")
	}
	if count, _ := store.SearchTranscript(session, "nana"); count != 0 {
		t.Fatal("false hit")
	}
	writeFile(t, filepath.Join(dir, "events.jsonl"), []byte("aaaa\n"))
	if count, _ := store.SearchTranscript(session, "aa"); count != 2 {
		t.Fatalf("hits must not overlap, got %d", count)
	}
}

func TestSearchTailWindow(t *testing.T) {
	store := NewStore(tempRoot(t))
	dir := writeMeta(t, store.SessionsDir(), cppMeta(map[string]any{"id": 1}))
	session, _ := store.FindSession(1)
	body := "marker early\n" + strings.Repeat("filler line\n", TranscriptSearchTailBytes/12+10) + "partial marker"
	writeFile(t, filepath.Join(dir, "transcript.txt"), []byte(body))
	count, snippet := store.SearchTranscript(session, "MARKER")
	if count != 1 || !strings.HasSuffix(snippet, "partial marker") {
		t.Fatalf("tail search %d %q", count, snippet)
	}
}

func TestSearchFoldsUnicode(t *testing.T) {
	store := NewStore(tempRoot(t))
	dir := writeMeta(t, store.SessionsDir(), cppMeta(map[string]any{"id": 1}))
	session, _ := store.FindSession(1)
	writeFile(t, filepath.Join(dir, "transcript.txt"), []byte("\U0001F600 ÉCOLE école"))
	count, snippet := store.SearchTranscript(session, "École")
	if count != 2 || snippet != "\U0001F600 ÉCOLE école" {
		t.Fatalf("%d %q", count, snippet)
	}
}

func TestAttachmentPaths(t *testing.T) {
	got := AttachmentPathsIn("Attached image:   /tmp/a.png\nAttached video:\t/tmp/b.mp4  \r\n" +
		`{"text":"Attached image: /tmp/c.jpg\"}` + "\nAttached image: /tmp/a.png\nAttached file: /tmp/x")
	want := []string{"/tmp/a.png", "/tmp/b.mp4", "/tmp/c.jpg"}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("paths %v", got)
	}
	if len(AttachmentPathsIn("nothing here")) != 0 {
		t.Fatal("no markers")
	}

	store := NewStore(tempRoot(t))
	dir := writeMeta(t, store.SessionsDir(), cppMeta(map[string]any{"id": 1, "prompt": "Attached image: /p/prompt.png"}))
	session, _ := store.FindSession(1)
	filler := strings.Repeat("z", 99) + "\n"
	body := "Attached image: /p/head.png\n" + strings.Repeat(filler, AttachmentScanBytes/100+5) +
		"Attached image: /p/middle.png\n" + strings.Repeat(filler, AttachmentScanBytes/100+5) +
		"Attached image: /p/tail.png\n"
	writeFile(t, filepath.Join(dir, "transcript.txt"), []byte(body))
	writeFile(t, filepath.Join(dir, "events.jsonl"), []byte(`{"t":"Attached video: /p/clip.mov"}`+"\n"))
	got = store.AttachmentPaths(session)
	want = []string{"/p/prompt.png", "/p/head.png", "/p/tail.png", "/p/clip.mov"}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("session paths %v", got)
	}
}

func TestRetiredModelOutcomes(t *testing.T) {
	store := NewStore(tempRoot(t))
	writeFile(t, store.ModelOutcomesPath(), []byte(`{"version":1,"models":[
		{"provider":"claude-code","model":"Opus ","runs":3,"merged":1},
		{"provider":"claude-code","model":"opus","runs":2,"merged":-4},
		{"provider":"codex","model":"","runs":9,"merged":9},
		{"provider":"codex","model":"gpt-5","runs":1,"merged":1}]}`))
	got := store.RetiredModelOutcomes()
	if len(got) != 2 {
		t.Fatalf("outcomes %v", got)
	}
	if o := got[ModelOutcomeKey("claude-code", "OPUS")]; o.Runs != 5 || o.Merged != 1 {
		t.Fatalf("opus %+v", o)
	}
	if len(NewStore(tempRoot(t)).RetiredModelOutcomes()) != 0 {
		t.Fatal("missing file")
	}
}

func TestQueuedOldestFirst(t *testing.T) {
	mk := func(id, priority int, created int64, status string, held bool) Session {
		s := NewSession()
		s.ID, s.Priority, s.CreatedAtMs, s.Status, s.Held = id, priority, created, status, held
		return s
	}
	ids := QueuedSessionIDsOldestFirst([]Session{
		mk(1, 50, 30, StatusQueued, false), mk(2, 10, 40, StatusQueued, false),
		mk(3, 50, 20, StatusQueued, false), mk(4, 1, 1, StatusQueued, true),
		mk(5, 1, 1, StatusRunning, false), mk(6, 50, 20, StatusQueued, false),
	})
	if !reflect.DeepEqual(ids, []int{2, 3, 6, 1}) {
		t.Fatalf("queue %v", ids)
	}
}
