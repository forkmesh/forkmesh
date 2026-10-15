package hosts

import (
	"strings"
	"sync/atomic"
	"testing"
	"time"

	"forkmesh.com/desktop-core/internal/coretest"
)

func TestFeedScriptParity(t *testing.T) {
	if len(mainLogFeedScript) != 984 || cliSHA(mainLogFeedScript) != "8d152b6bf3d442f83e9b4aa9e3a3ba792656e99a556f7dbefb15916188fc7c0c" {
		t.Fatalf("kMainLogFeedScript differs from the C++ bytes")
	}
	remote := feedRemoteCommand()
	if len(remote) != 1017 || cliSHA(remote) != "cccd3db73e2fe8c5a35ca3f645fd855bf372b5416a58cb053e7d4cc032829137" {
		t.Fatalf("remote feed command differs from the C++ bytes: %s", remote)
	}
}

func TestFeedLineClassification(t *testing.T) {
	cases := []struct {
		line   string
		source int
		text   string
	}{
		{"J 2026-09-17T15:12:52+0000 mirror1 sshd[12]: Accepted publickey", 3, "15:12:52 sshd: Accepted publickey"},
		{"J 2026-09-17T15:12:52+0000 caddy[1]: GET /", 2, "15:12:52 caddy: GET /"},
		{"J 2026-09-17T15:12:52+0000 h ForkMesh-Mirror[3]: up  \r ", 1, "15:12:52 ForkMesh-Mirror: up"},
		{"J 2026-09-17T15:12:52+0000 h SSHD-session[3]: x", 3, "15:12:52 SSHD-session: x"},
		{"J 2026-09-17T15:12:52+0000 h systemd[1]:", 4, "15:12:52 systemd: "},
		{"J 2026 kernel: oops", 4, "kernel: oops"},
		{"J 2026-09-17T15:1 kernel: short", 4, "kernel: short"},
		{"J -- Boot abc --", 4, "-- Boot abc --"},
		{"J foo bar: x", 4, "foo bar: x"},
		{"J 2026-09-17T15:12:52 host noident", 4, "2026-09-17T15:12:52 host noident"},
		{"J 2026-09-17T15:12:52+0000 h [3]: x", 4, "15:12:52 [3]: x"},
		{"J 2026-09-17T15:12:52+0000 h :", 4, "2026-09-17T15:12:52+0000 h :"},
		{"J 1T\U0001D7D8\U0001D7D8\U0001D7D8\U0001D7D8 h nginx: y", 2, "\U0001D7D8\U0001D7D8\U0001D7D8\U0001D7D8 nginx: y"},
		{"J ", 4, ""},
		{"L node text here", 0, "text here"},
		{"L bogus text", 5, "bogus text"},
		{"L  x", 5, " x"},
		{"M host a note", 5, "a note"},
		{"M web", 5, "web"},
		{"K", -1, ""},
		{"N 1 2 3", -1, ""},
		{"untagged", -1, ""},
	}
	for _, c := range cases {
		source, text := feedLogLine(c.line)
		if source != c.source || (source >= 0 && text != c.text) {
			t.Fatalf("%q: got %d %q want %d %q", c.line, source, text, c.source, c.text)
		}
	}
	if logSourceID(-1) != "host" || logSourceID(9) != "host" || logSourceID(1) != "mirror" || logSourceIndex("system") != 4 {
		t.Fatal("source ids")
	}
}

func TestForwardedHostLinesDedupe(t *testing.T) {
	seen := newForwardedHostLines(3)
	if !seen.firstSighting("h", "0a") || seen.firstSighting("h", "0a") || !seen.firstSighting("other", "0a") {
		t.Fatal("feed sightings")
	}
	if seen.desktopSighting("h", "0a") {
		t.Fatal("the desktop echo of a feed line must be skipped")
	}
	if !seen.desktopSighting("h", "1b") || !seen.desktopSighting("h", "1b") || seen.firstSighting("h", "1b") {
		t.Fatal("a desktop line passes every time and the feed does not repeat it")
	}
	seen.firstSighting("h", "2c")
	seen.firstSighting("h", "3d")
	if !seen.firstSighting("h", "0a") {
		t.Fatal("the oldest line is forgotten past the per-host cap")
	}
}

type feedFixture struct {
	*cliFixture
	sleepCalls atomic.Int32
}

func newFeedFixture(t *testing.T, hook func(call int32, d time.Duration) bool) *feedFixture {
	t.Helper()
	f := &feedFixture{cliFixture: newCliFixture(t)}
	t.Setenv("FAKE_SSH_MODE", "feed")
	t.Setenv("FAKE_SSH_STDIN", "")
	f.mu.Lock()
	f.sleepHook = func(d time.Duration) bool {
		call := f.sleepCalls.Add(1)
		return hook != nil && hook(call, d)
	}
	f.mu.Unlock()
	return f
}

func (f *feedFixture) start(t *testing.T, untilMs int64) map[string]any {
	return f.call(t, "core.hosts.feed.start", map[string]any{"key": "k1", "node": "mirror1", "ip": "203.0.113.5", "user": "root",
		"password": "", "identityFile": "", "sshUntilMs": float64(untilMs)})
}

type feedEvents struct {
	lines map[string]int
	ended []map[string]any
}

func (f *feedFixture) watch(t *testing.T, done func(feedEvents) bool) feedEvents {
	t.Helper()
	out := feedEvents{lines: map[string]int{}}
	deadline := time.After(10 * time.Second)
	for !done(out) {
		select {
		case event := <-f.ui.Events:
			data := decode(t, event.Data)
			switch event.Name {
			case "core.hosts.feedLines":
				if data["key"] != "k1" || data["node"] != "mirror1" {
					t.Fatalf("feed lines %v", data)
				}
				for _, raw := range data["lines"].([]any) {
					line := raw.(map[string]any)
					out.lines[line["text"].(string)] = int(line["source"].(float64))
				}
			case "core.hosts.feedEnded":
				out.ended = append(out.ended, data)
			}
		case <-deadline:
			t.Fatalf("feed events so far: %+v", out)
		}
	}
	return out
}

var feedExpected = map[string]int{
	"hello from node":                   0,
	"15:12:52 sshd: Accepted publickey": 3,
	"a note":                            5,
	"[ssh] warning: first":              5,
}

func hasFeedLines(lines map[string]int) bool {
	for text, source := range feedExpected {
		if got, ok := lines[text]; !ok || got != source {
			return false
		}
	}
	return true
}

func TestHostFeedLinesReachPhoneAndDesktop(t *testing.T) {
	f := newFeedFixture(t, nil)
	stream, err := call(t, &fixture{core: f.core}, f.phone, "hosts.logs.start", map[string]any{"host": "mirror1"})
	if err != nil {
		t.Fatal(err)
	}
	started := f.start(t, 0)
	if started["started"] != true || started["kept"] != false {
		t.Fatalf("start %v", started)
	}
	got := f.watch(t, func(e feedEvents) bool { return hasFeedLines(e.lines) })
	for _, unwanted := range []string{"untagged noise", "K", "[ssh] "} {
		if _, ok := got.lines[unwanted]; ok {
			t.Fatalf("forwarded %q", unwanted)
		}
	}
	phoneLines := map[string]string{}
	deadline := time.After(5 * time.Second)
	for len(phoneLines) < len(feedExpected) {
		select {
		case event := <-f.phone.Events:
			if event.Name != "hosts.log" {
				continue
			}
			batch := decode(t, event.Data)
			if batch["stream"] != stream["stream"] || batch["host"] != "mirror1" {
				t.Fatalf("batch %v", batch)
			}
			for _, raw := range batch["lines"].([]any) {
				line := raw.(map[string]any)
				phoneLines[line["text"].(string)] = line["src"].(string)
			}
		case <-deadline:
			t.Fatalf("phone lines %v", phoneLines)
		}
	}
	for text, source := range feedExpected {
		if phoneLines[text] != logSourceID(source) {
			t.Fatalf("phone %q: %q", text, phoneLines[text])
		}
	}
	feeds := f.call(t, "core.hosts.feeds", map[string]any{})["feeds"].([]any)
	if len(feeds) != 1 || feeds[0].(map[string]any)["running"] != true || feeds[0].(map[string]any)["gotData"] != true {
		t.Fatalf("feeds %v", feeds)
	}
	if !strings.Contains(f.sshLog(), "root@203.0.113.5 "+feedRemoteCommand()) {
		t.Fatalf("ssh log %s", f.sshLog())
	}

	lines := []map[string]any{{"node": "mirror1", "src": "node", "text": "hello from node"}, {"node": "mirror1", "src": "web", "text": "desktop only"}}
	if accepted := f.call(t, "core.hosts.lines", map[string]any{"lines": lines})["accepted"]; accepted != 1.0 {
		t.Fatalf("accepted %v", accepted)
	}
	if accepted := f.call(t, "core.hosts.lines", map[string]any{"lines": lines})["accepted"]; accepted != 1.0 {
		t.Fatalf("accepted again %v", accepted)
	}
	f.mod.mu.Lock()
	recent := strings.Join(f.mod.recent["mirror1"], "\n")
	f.mod.mu.Unlock()
	if strings.Count(recent, "[node] hello from node") != 1 || strings.Count(recent, "[web] desktop only") != 2 {
		t.Fatalf("recent:\n%s", recent)
	}

	again := f.start(t, 0)
	if again["started"] != false || again["kept"] != true {
		t.Fatalf("restart %v", again)
	}
	if stopped := f.call(t, "core.hosts.feed.stop", map[string]any{"key": "k1"}); stopped["stopped"] != true {
		t.Fatalf("stop %v", stopped)
	}
	if stopped := f.call(t, "core.hosts.feed.stop", map[string]any{"key": "k1"}); stopped["stopped"] != false {
		t.Fatalf("stop twice %v", stopped)
	}
	if feeds := f.call(t, "core.hosts.feeds", map[string]any{})["feeds"].([]any); len(feeds) != 0 {
		t.Fatalf("feeds after stop %v", feeds)
	}
	if ended := cliDrain(f.ui, 300*time.Millisecond).named("core.hosts.feedEnded"); len(ended) != 0 {
		t.Fatalf("a stopped feed reported an end: %v", ended)
	}
	if f.sleepCalls.Load() != 0 {
		t.Fatal("a stopped feed redialled")
	}
}

func TestHostFeedReconnectBackoff(t *testing.T) {
	f := newFeedFixture(t, func(call int32, d time.Duration) bool { return call == 1 })
	t.Setenv("FAKE_FEED_EXIT", "3")
	f.start(t, 0)
	first := "[log feed] SSH connection closed (exit 3); reconnecting in 60 s"
	second := "[log feed] SSH connection closed (exit 3); reconnecting in 120 s"
	got := f.watch(t, func(e feedEvents) bool {
		_, a := e.lines[first]
		_, b := e.lines[second]
		return a && b && len(e.ended) >= 2
	})
	if got.lines[first] != fallbackLogSource || got.lines[second] != fallbackLogSource || !hasFeedLines(got.lines) {
		t.Fatalf("lines %v", got.lines)
	}
	for _, ended := range got.ended[:2] {
		if ended["how"] != "exit 3" || ended["gotData"] != true || ended["recycling"] != false || ended["key"] != "k1" {
			t.Fatalf("ended %v", ended)
		}
	}
	if d1, d2 := <-f.sleeps, <-f.sleeps; d1 != 60*time.Second || d2 != 120*time.Second {
		t.Fatalf("backoff %v %v", d1, d2)
	}
	feeds := f.call(t, "core.hosts.feeds", map[string]any{})["feeds"].([]any)
	if len(feeds) != 1 || feeds[0].(map[string]any)["failures"] != 2.0 || feeds[0].(map[string]any)["lastEnd"] != "exit 3" {
		t.Fatalf("feeds %v", feeds)
	}
	if !strings.Contains(strings.Join(f.recentLines(), "\n"), "[host] "+first) {
		t.Fatalf("phones miss the reconnect note: %v", f.recentLines())
	}
}

func (f *feedFixture) recentLines() []string {
	f.mod.mu.Lock()
	defer f.mod.mu.Unlock()
	return append([]string(nil), f.mod.recent["mirror1"]...)
}

func TestHostFeedRecyclesAtSSHUntil(t *testing.T) {
	f := newFeedFixture(t, nil)
	f.start(t, time.Now().UnixMilli()+400)
	got := f.watch(t, func(e feedEvents) bool { return len(e.ended) >= 1 })
	ended := got.ended[0]
	if ended["recycling"] != true || ended["how"] != "exit 15" || ended["gotData"] != true || ended["node"] != "mirror1" {
		t.Fatalf("ended %v", ended)
	}
	if feeds := f.call(t, "core.hosts.feeds", map[string]any{})["feeds"].([]any); len(feeds) != 0 {
		t.Fatalf("a recycled feed stays in the core: %v", feeds)
	}
	if f.sleepCalls.Load() != 0 {
		t.Fatal("a recycled feed redialled itself")
	}
}

func TestHostFeedRecycleWaitsForDesktop(t *testing.T) {
	var f *feedFixture
	f = newFeedFixture(t, func(call int32, d time.Duration) bool {
		switch call {
		case 1:
			time.Sleep(300 * time.Millisecond)
			return true
		case 2:
			f.attached.Store(true)
			return true
		}
		return false
	})
	f.attached.Store(false)
	t.Setenv("FAKE_FEED_EXIT", "3")
	f.start(t, time.Now().UnixMilli()+150)
	got := f.watch(t, func(e feedEvents) bool { return len(e.ended) >= 3 })
	if got.ended[0]["recycling"] != false || got.ended[1]["recycling"] != false {
		t.Fatalf("recycled without a desktop: %v", got.ended)
	}
	last := got.ended[2]
	if last["recycling"] != true || last["how"] != "recycling" || last["gotData"] != false {
		t.Fatalf("recycle once attached: %v", last)
	}
	if feeds := f.call(t, "core.hosts.feeds", map[string]any{})["feeds"].([]any); len(feeds) != 0 {
		t.Fatalf("feeds %v", feeds)
	}
}

func TestHostFeedRefusals(t *testing.T) {
	f := newFeedFixture(t, nil)
	if _, err := coretest.Call(t, f.core, f.ui, "core.hosts.feed.start", map[string]any{"key": "k", "ip": "", "user": "root"}); err == nil {
		t.Fatal("feed without an address")
	}
	if _, err := coretest.Call(t, f.core, f.phone, "core.hosts.feed.start", map[string]any{"key": "k", "ip": "203.0.113.5", "user": "root"}); err == nil {
		t.Fatal("a phone started a feed")
	}
	result := f.call(t, "core.hosts.feed.start", map[string]any{"key": "bad", "node": "bad", "ip": "bad..host", "user": "root"})
	if result["started"] != true {
		t.Fatalf("start %v", result)
	}
	deadline := time.After(5 * time.Second)
	for {
		select {
		case event := <-f.ui.Events:
			if event.Name != "core.hosts.feedLines" {
				continue
			}
			data := decode(t, event.Data)
			line := data["lines"].([]any)[0].(map[string]any)
			if data["key"] != "bad" || line["text"] != "[error] Cannot follow this host's logs: The saved SSH host or username is invalid." || line["source"] != 5.0 {
				t.Fatalf("error line %v", data)
			}
			feeds := f.call(t, "core.hosts.feeds", map[string]any{})["feeds"].([]any)
			if len(feeds) != 1 || feeds[0].(map[string]any)["failed"] != true || feeds[0].(map[string]any)["running"] != false {
				t.Fatalf("feeds %v", feeds)
			}
			if f.sshLog() != "" || f.sleepCalls.Load() != 0 {
				t.Fatal("an invalid feed dialled")
			}
			return
		case <-deadline:
			t.Fatal("no error line")
		}
	}
}
