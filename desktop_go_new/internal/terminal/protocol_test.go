package terminal

import (
	"bytes"
	"encoding/base64"
	"strings"
	"testing"

	"forkmesh.com/desktop-core/internal/rpc"
)

func TestDecodeInputMatchesQt(t *testing.T) {
	cases := []struct {
		in   any
		want string
		ok   bool
	}{
		{"aGk=", "hi", true},
		{"aGk", "hi", true},
		{"aGVsbG8K", "hello\n", true},
		{"YQ==", "a", true},
		{"YQ", "a", true},
		{"YQ=", "", false},
		{"Y===", "", false},
		{"Y=Q=", "", false},
		{"aGk\n", "", false},
		{"a Gk", "", false},
		{"", "", false},
		{"Q", "", false},
		{42.0, "", false},
		{nil, "", false},
		{"aGkñ", "", false},
		{"-_8=", "", false},
	}
	for _, c := range cases {
		got, err := decodeInput(c.in)
		if c.ok != (err == nil) {
			t.Fatalf("%q: err %v", c.in, err)
		}
		if err != nil && rpc.Code(err) != "bad_input" {
			t.Fatalf("%q: code %s", c.in, rpc.Code(err))
		}
		if c.ok && string(got) != c.want {
			t.Fatalf("%q: got %q", c.in, got)
		}
	}
	big := base64.StdEncoding.EncodeToString(bytes.Repeat([]byte{'x'}, maxInputBytes))
	if _, err := decodeInput(big); err != nil {
		t.Fatalf("max input refused: %v", err)
	}
	over := base64.StdEncoding.EncodeToString(bytes.Repeat([]byte{'x'}, maxInputBytes+1))
	if _, err := decodeInput(over); err == nil {
		t.Fatal("oversized input accepted")
	}
}

func TestAppendReplayCutsAtLineOrRune(t *testing.T) {
	replay := appendReplay(nil, []byte("aaaa\nbbbbbb"), 8)
	if string(replay) != "bbbbbb" {
		t.Fatalf("line cut: %q", replay)
	}
	replay = appendReplay(nil, []byte("xé"+strings.Repeat("y", 6)), 7)
	if string(replay) != "yyyyyy" {
		t.Fatalf("rune cut: %q", replay)
	}
	replay = appendReplay(nil, []byte("abc"), 8)
	replay = appendReplay(replay, []byte("defghij"), 8)
	if string(replay) != "cdefghij" {
		t.Fatalf("plain cut: %q", replay)
	}
	long := append(bytes.Repeat([]byte{'z'}, replayBytes+replayLineSearchBytes+10), '\n', 'k')
	replay = appendReplay(nil, long, replayBytes)
	if len(replay) != replayBytes {
		t.Fatalf("far newline should not be used: %d", len(replay))
	}
}

func TestOutboxTakeAndReset(t *testing.T) {
	var box outbox
	chunk := []byte(strings.Repeat("a", maxEventBytes-1) + "é" + "tail")
	box.queue(chunk, maxPendingBytes)
	first := box.take("id", nil, maxEventBytes)
	data, _ := base64.StdEncoding.DecodeString(first["data"].(string))
	if len(data) != maxEventBytes-1 || first["reset"] != false || first["terminal"] != "id" {
		t.Fatalf("first event split inside a rune: %d %v", len(data), first["reset"])
	}
	second := box.take("id", nil, maxEventBytes)
	data, _ = base64.StdEncoding.DecodeString(second["data"].(string))
	if string(data) != "étail" || box.hasOutput() {
		t.Fatalf("second: %q", data)
	}
	box.queue(bytes.Repeat([]byte{'b'}, 10), 16)
	box.queue(bytes.Repeat([]byte{'b'}, 10), 16)
	if !box.reset || len(box.pending) != 0 || !box.hasOutput() {
		t.Fatal("overflow did not reset")
	}
	box.queue([]byte("ignored"), 16)
	reset := box.take("id", []byte("REPLAY"), maxEventBytes)
	data, _ = base64.StdEncoding.DecodeString(reset["data"].(string))
	if string(data) != "REPLAY" || reset["reset"] != true || box.hasOutput() {
		t.Fatalf("reset: %q %v", data, reset["reset"])
	}
}

func TestHelpers(t *testing.T) {
	if clampCols(1) != 20 || clampCols(900) != 500 || clampRows(1) != 5 || clampRows(999) != 200 {
		t.Fatal("clamp")
	}
	if jsonInt(80.0, 1) != 80 || jsonInt(80.5, 7) != 7 || jsonInt("80", 7) != 7 || jsonInt(nil, 24) != 24 || jsonInt(true, 3) != 3 {
		t.Fatal("jsonInt")
	}
	id := newTerminalID()
	if !validTerminalID(id) || validTerminalID(strings.ToUpper(id)) || validTerminalID(id[1:]) {
		t.Fatalf("id %s", id)
	}
	if followInterval(rpc.Relay) != relayedInterval || followInterval(rpc.LAN) != directInterval || followInterval(rpc.UI) != directInterval {
		t.Fatal("interval")
	}
	m := meta{id: id, provider: "codex", cli: "codex", startedMs: 5}
	out := terminalJSON(m, true, 80, 24)
	if out["label"] != "Codex" || out["running"] != true || out["session"] != nil || out["mode"] != nil || out["account"] != nil {
		t.Fatalf("json %v", out)
	}
	m.session, m.mode, m.account = 3, "login", "work"
	out = terminalJSON(m, false, 80, 24)
	if out["session"] != 3 || out["mode"] != "login" || out["account"] != "work" {
		t.Fatalf("json %v", out)
	}
	if cliLabel("claude-code") != "Claude Code" || cliProgram("claude-code") != "claude" || cliProgram("x") != "" {
		t.Fatal("cli")
	}
}

func TestEnvironment(t *testing.T) {
	base := []string{"PATH=/bin", "TERM=dumb", "COLORTERM=no", "ANTHROPIC_API_KEY=k", "OPENAI_API_KEY=o", "CODEX_HOME=/old", "KEEP=1"}
	got := environment(base, map[string]string{"CODEX_HOME": "/new", "A": "b"}, true)
	want := []string{"PATH=/bin", "KEEP=1", "A=b", "CODEX_HOME=/new", "TERM=xterm-256color", "COLORTERM=truecolor"}
	if strings.Join(got, ",") != strings.Join(want, ",") {
		t.Fatalf("strip: %v", got)
	}
	got = environment(base, nil, false)
	if !strings.Contains(strings.Join(got, ","), "ANTHROPIC_API_KEY=k") || strings.Contains(strings.Join(got, ","), "TERM=dumb") {
		t.Fatalf("keep: %v", got)
	}
}
