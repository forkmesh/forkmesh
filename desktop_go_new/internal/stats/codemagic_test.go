package stats

import (
	"encoding/json"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"
)

type fakeTransport struct {
	calls  atomic.Int32
	status int
	body   string
	gate   chan struct{}
	seen   http.Header
	mu     sync.Mutex
}

func (f *fakeTransport) RoundTrip(req *http.Request) (*http.Response, error) {
	f.calls.Add(1)
	f.mu.Lock()
	f.seen = req.Header.Clone()
	status, body := f.status, f.body
	f.mu.Unlock()
	if f.gate != nil {
		<-f.gate
	}
	return &http.Response{StatusCode: status, Body: io.NopCloser(strings.NewReader(body)), Header: http.Header{}, Request: req}, nil
}

func (f *fakeTransport) set(status int, body string) {
	f.mu.Lock()
	f.status, f.body = status, body
	f.mu.Unlock()
}

const codemagicReply = `{"user":{"buildTimes":{"monthlyFreeBuildTimeLimit":30000,"currentPeriod":{"free":1200},"previousPeriod":{}},
"billing":{"usage":{"currentPeriod":{"buildTime":{"mac_mini_m2_free":1200,"mac_mini_m2":300,"linux":-5}},
"previousPeriod":{"buildTime":{"mac_mini_m2_free":700,"linux_free":50}}}}}}`

func TestCodemagicUsageParity(t *testing.T) {
	now := time.Date(2026, 12, 15, 10, 0, 0, 0, time.UTC).UnixMilli()
	var reply map[string]any
	if err := jsonUnmarshal(codemagicReply, &reply); err != nil {
		t.Fatal(err)
	}
	assertJSON(t, codemagicUsage(reply, now), map[string]any{
		"limitSeconds": 30000.0, "usedSeconds": 1200.0, "remainingSeconds": 28800.0, "previousSeconds": 750.0,
		"paidSeconds": 300.0, "resetsAt": float64(time.Date(2027, 1, 1, 0, 0, 0, 0, time.UTC).UnixMilli()),
		"fetchedAt": float64(now),
	})
	var flat map[string]any
	_ = jsonUnmarshal(`{"billing":{"usage":{"freeLimit":{"buildTime":500},"currentPeriod":{"buildTime":{"a_free":600}}}}}`, &flat)
	got := codemagicUsage(flat, now)
	if got["usedSeconds"] != 600.0 || got["remainingSeconds"] != 0.0 || got["limitSeconds"] != 500.0 {
		t.Fatalf("fallback limit: %v", got)
	}
	if len(codemagicUsage(map[string]any{"user": "nope"}, now)) != 0 {
		t.Fatal("no limit should give an empty answer")
	}
	for status, want := range map[int]string{401: "codemagic_unauthorized", 403: "codemagic_unauthorized", 200: "bad_answer", 0: "codemagic_unreachable", 500: "codemagic_unreachable"} {
		if got := codemagicError(status); got != want {
			t.Fatalf("codemagicError(%d)=%q", status, got)
		}
	}
}

func TestCodemagicMethod(t *testing.T) {
	f := newFixture(t)
	writeSettings(t, f.core, repoSettings("alice", nil, nil))
	fake := &fakeTransport{status: 200, body: codemagicReply}
	f.module.HTTPClient = &http.Client{Transport: fake}
	f.module.CodemagicURL = "https://codemagic.invalid/user"
	f.expectError("stats.codemagic", nil, "codemagic_not_configured")

	writeSettings(t, f.core, repoSettings("alice", nil, map[string][]string{
		"actions": {`variables="@ByteArray({\"OTHER\":\"1\",\"CODEMAGIC_API_TOKEN\":\"  tok-1  \"})"`},
	}))
	got := f.mustCall("stats.codemagic", nil)
	if got["usedSeconds"] != 1200.0 || got["fetchedAt"] != float64(f.now.UnixMilli()) {
		t.Fatalf("usage %v", got)
	}
	if fake.seen.Get("x-auth-token") != "tok-1" || fake.seen.Get("Accept") != "application/json" {
		t.Fatalf("headers %v", fake.seen)
	}
	f.now = f.now.Add(4 * time.Minute)
	f.mustCall("stats.codemagic", nil)
	if fake.calls.Load() != 1 {
		t.Fatalf("cached answer refetched: %d", fake.calls.Load())
	}
	f.now = f.now.Add(2 * time.Minute)
	fake.set(401, "{}")
	f.expectError("stats.codemagic", nil, "codemagic_unauthorized")
	fake.set(200, "[1,2]")
	f.expectError("stats.codemagic", nil, "bad_answer")
	fake.set(502, "")
	f.expectError("stats.codemagic", nil, "codemagic_unreachable")

	fake.set(200, codemagicReply)
	fake.gate = make(chan struct{})
	before := fake.calls.Load()
	var wg sync.WaitGroup
	results := make([]string, 4)
	for i := range results {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			_, code := f.call("stats.codemagic", nil)
			results[i] = code
		}(i)
	}
	time.Sleep(100 * time.Millisecond)
	close(fake.gate)
	wg.Wait()
	if n := fake.calls.Load() - before; n != 1 {
		t.Fatalf("concurrent requests should share one fetch, got %d", n)
	}
	for _, code := range results {
		if code != "" {
			t.Fatalf("coalesced request failed: %v", results)
		}
	}
	if f.module.Busy() {
		t.Fatal("module still busy after fetch")
	}
}

func TestShortcutMethod(t *testing.T) {
	f := newFixture(t)
	checkout := t.TempDir()
	writeSettings(t, f.core, repoSettings("alice", []repoEntry{
		{owner: "Alice", name: "Tool", localPath: checkout, cloneURL: "https://app.forkmesh.com/alice/tool"},
		{owner: "bob", name: "remote", cloneURL: "https://app.forkmesh.com/bob/remote"},
		{owner: "We Are/Org", name: "kit", localPath: checkout, cloneURL: "https://x.example/a"},
	}, nil))
	writeFile(t, filepath.Join(checkout, ".forkmesh/shortcuts/deploy.sh"), "#!/bin/sh\r\n# name: Deploy\r\necho hi\r\n")
	writeFile(t, filepath.Join(checkout, ".forkmesh/skills/review.md"), "# Review\nLook closely.\n")
	writeFile(t, filepath.Join(f.core.Layout.Data, "skills/global.skill"), strings.Repeat("a", maxShortcutBytes+10))
	writeFile(t, filepath.Join(f.core.Layout.Data, "organization-skills/We-Are-Org/org.md"), "org skill")

	got := f.mustCall("insights.shortcut", map[string]any{"repo": " alice/tool ", "scope": "shortcut", "file": "deploy.sh"})
	assertJSON(t, got, map[string]any{"file": "deploy.sh", "scope": "shortcut", "text": "#!/bin/sh\n# name: Deploy\necho hi\n", "truncated": false})
	got = f.mustCall("insights.shortcut", map[string]any{"repo": "ALICE/TOOL", "scope": " repository ", "file": "review.md"})
	if got["text"] != "# Review\nLook closely.\n" || got["scope"] != "repository" {
		t.Fatalf("repository skill %v", got)
	}
	got = f.mustCall("insights.shortcut", map[string]any{"scope": "app", "file": "global.skill"})
	if got["truncated"] != true || len(got["text"].(string)) != maxShortcutBytes {
		t.Fatalf("app skill truncation: %v %d", got["truncated"], len(got["text"].(string)))
	}
	got = f.mustCall("insights.shortcut", map[string]any{"repo": "We Are/Org/kit", "scope": "organization", "file": "org.md"})
	if got["text"] != "org skill" {
		t.Fatalf("organization skill %v", got)
	}
	for _, params := range []map[string]any{
		{"repo": "alice/tool", "scope": "shortcut", "file": "../deploy.sh"},
		{"repo": "alice/tool", "scope": "shortcut", "file": ".hidden"},
		{"repo": "alice/tool", "scope": "shortcut", "file": "deploy.sh "},
		{"repo": "alice/tool", "scope": "elsewhere", "file": "deploy.sh"},
		{"repo": "alice/tool", "scope": "shortcut", "file": "missing.sh"},
		{"repo": "bob/remote", "scope": "shortcut", "file": "deploy.sh"},
		{"repo": "carol/none", "scope": "repository", "file": "review.md"},
		{"repo": "alice/tool", "scope": "shortcut", "file": "deploy.sh/"},
	} {
		f.expectError("insights.shortcut", params, "unknown_shortcut")
	}
	if err := os.MkdirAll(filepath.Join(checkout, ".forkmesh/shortcuts/folder"), 0o755); err != nil {
		t.Fatal(err)
	}
	f.expectError("insights.shortcut", map[string]any{"repo": "alice/tool", "scope": "shortcut", "file": "folder"}, "unknown_shortcut")
}

func jsonUnmarshal(text string, into any) error {
	return json.Unmarshal([]byte(text), into)
}
