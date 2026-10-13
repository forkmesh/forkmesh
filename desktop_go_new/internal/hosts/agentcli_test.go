package hosts

import (
	"context"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"forkmesh.com/desktop-core/internal/coretest"
	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/rpc"
)

const cliFakeSSH = `#!/bin/sh
printf '%s\n' "$*" >> "$FAKE_SSH_LOG"
if [ -n "$FAKE_SSH_STDIN" ]; then cat > "$FAKE_SSH_STDIN"; fi
case "$FAKE_SSH_MODE" in
  denied) echo "root@host: Permission denied (publickey)." >&2; exit 255 ;;
  timeout) echo "ssh: connect to host 203.0.113.9 port 22: Connection timed out" >&2; exit 255 ;;
  install) echo "Installing Claude Code from claude.ai..."; echo "Installation complete." ;;
  actions) printf 'helper output\nFORKMESH_ACTIONS_RESULT=%s\n' "$FAKE_ACTIONS_RESULT"; exit "${FAKE_ACTIONS_EXIT:-0}" ;;
  flood) i=0; while [ $i -lt 1100 ]; do printf '%064d\n' 0; i=$((i+1)); done ;;
  hang) exec sleep 30 ;;
  feed)
    printf 'L node hello from node\nJ 2026-09-17T15:12:52+0000 mirror1 sshd[12]: Accepted publickey\nM host a note\nK\nuntagged noise\n'
    printf 'warning: first\n\n' >&2
    if [ -n "$FAKE_FEED_EXIT" ]; then exit "$FAKE_FEED_EXIT"; fi
    exec sleep 30 ;;
esac
exit 0
`

type cliFixture struct {
	core     *daemon.Core
	mod      *Module
	ui       *coretest.Peer
	phone    *coretest.Peer
	log      string
	stdin    string
	attached atomic.Bool
	sleeps   chan time.Duration

	mu        sync.Mutex
	claudeDir string
	codexDir  string
	sleepHook func(d time.Duration) bool
}

func installCliFakeSSH(t *testing.T) (string, string) {
	t.Helper()
	if runtime.GOOS == "windows" {
		t.Skip("fake ssh is a shell script")
	}
	bin := t.TempDir()
	for _, name := range []string{"ssh", "sshpass"} {
		if err := os.WriteFile(filepath.Join(bin, name), []byte(cliFakeSSH), 0o755); err != nil {
			t.Fatal(err)
		}
	}
	scratch := t.TempDir()
	log := filepath.Join(scratch, "ssh.log")
	stdin := filepath.Join(scratch, "stdin")
	t.Setenv("PATH", bin+string(os.PathListSeparator)+os.Getenv("PATH"))
	t.Setenv("FAKE_SSH_LOG", log)
	t.Setenv("FAKE_SSH_STDIN", stdin)
	t.Setenv("FAKE_SSH_MODE", "")
	t.Setenv("FAKE_ACTIONS_RESULT", "")
	t.Setenv("FAKE_ACTIONS_EXIT", "")
	t.Setenv("FAKE_FEED_EXIT", "")
	return log, stdin
}

func newCliFixture(t *testing.T) *cliFixture {
	t.Helper()
	log, stdin := installCliFakeSSH(t)
	core := coretest.New(t)
	writeSettings(t, core, defaultHosts(), nil)
	f := &cliFixture{core: core, log: log, stdin: stdin, sleeps: make(chan time.Duration, 16)}
	f.attached.Store(true)
	f.mod = New()
	f.mod.Owners = func() []string { return []string{"alice"} }
	f.mod.Attached = f.attached.Load
	f.claudeDir = t.TempDir()
	f.codexDir = t.TempDir()
	t.Setenv("CLAUDE_CONFIG_DIR", f.claudeDir)
	t.Setenv("CODEX_HOME", f.codexDir)
	f.mod.CallUI = func(ctx context.Context, method string, params any) ([]byte, error) {
		return nil, rpc.Fail("desktop_unavailable")
	}
	f.mod.Sleep = func(ctx context.Context, d time.Duration) bool {
		select {
		case f.sleeps <- d:
		default:
		}
		f.mu.Lock()
		hook := f.sleepHook
		f.mu.Unlock()
		if hook != nil && hook(d) {
			return ctx.Err() == nil
		}
		<-ctx.Done()
		return false
	}
	coretest.Start(t, core, f.mod)
	f.mod.lastProbeAll = time.Now().UnixMilli()
	f.ui = coretest.NewPeer("ui", rpc.UI)
	f.phone = coretest.NewPeer("phone", rpc.Relay)
	f.phone.Dev.User = "alice"
	core.Hub.Add(f.ui)
	core.Hub.Add(f.phone)
	return f
}

func (f *cliFixture) setLogins(t *testing.T, claude, codex string) {
	t.Helper()
	for path, content := range map[string]string{filepath.Join(f.claudeDir, ".credentials.json"): claude, filepath.Join(f.codexDir, "auth.json"): codex} {
		_ = os.Remove(path)
		if content == "" {
			continue
		}
		if err := os.WriteFile(path, []byte(content), 0o600); err != nil {
			t.Fatal(err)
		}
	}
}

func (f *cliFixture) call(t *testing.T, method string, params any) map[string]any {
	t.Helper()
	raw, err := coretest.Call(t, f.core, f.ui, method, params)
	if err != nil {
		t.Fatalf("%s: %v", method, err)
	}
	return decode(t, raw)
}

func (f *cliFixture) sshLog() string {
	data, _ := os.ReadFile(f.log)
	return string(data)
}

func (f *cliFixture) stdinBytes() []byte {
	data, _ := os.ReadFile(f.stdin)
	return data
}

func (f *cliFixture) waitJob(t *testing.T, id string) Job {
	t.Helper()
	deadline := time.Now().Add(10 * time.Second)
	for time.Now().Before(deadline) {
		for _, job := range f.mod.jobList() {
			if job.ID == id && !job.Running {
				return job
			}
		}
		time.Sleep(5 * time.Millisecond)
	}
	t.Fatalf("job %s did not finish", id)
	return Job{}
}

type cliEvents []coretest.Event

func (s cliEvents) named(name string) []map[string]any {
	out := []map[string]any{}
	for _, event := range s {
		if event.Name == name {
			var data map[string]any
			_ = json.Unmarshal(event.Data, &data)
			out = append(out, data)
		}
	}
	return out
}

func cliCollectUntil(t *testing.T, peer *coretest.Peer, name string, timeout time.Duration) cliEvents {
	t.Helper()
	out := cliEvents{}
	deadline := time.After(timeout)
	for {
		select {
		case event := <-peer.Events:
			out = append(out, event)
			if event.Name == name {
				return out
			}
		case <-deadline:
			t.Fatalf("no %s event within %s", name, timeout)
			return out
		}
	}
}

func cliDrain(peer *coretest.Peer, wait time.Duration) cliEvents {
	out := cliEvents{}
	deadline := time.After(wait)
	for {
		select {
		case event := <-peer.Events:
			out = append(out, event)
		case <-deadline:
			return out
		}
	}
}

func cliSHA(text string) string {
	sum := sha256.Sum256([]byte(text))
	return hex.EncodeToString(sum[:])
}

func TestAgentCliBootstrapRemoteCommandParity(t *testing.T) {
	cases := []struct {
		got  string
		sum  string
		size int
	}{
		{agentCliBootstrapRemoteCommand(true), "91669edc2efc14bdba01326722be90189288cd42233aa66f36f5776c129462a1", 2170},
		{agentCliBootstrapRemoteCommand(false), "3cbee3037856557435ad59974b0491b44b7d515e64683a169fabe9f3d279fad2", 1047},
		{agentCliServicePublishScript, "2073620e1cafe33c95d2932b0634d2c6ddd55c56ee9e955f81526b8425a38323", 308},
	}
	for i, c := range cases {
		if len(c.got) != c.size || cliSHA(c.got) != c.sum {
			t.Fatalf("case %d differs from the C++ bytes (%d bytes): %s", i, len(c.got), c.got)
		}
	}
	without := agentCliBootstrapRemoteCommand(false)
	if strings.Contains(without, "mktemp") || !strings.HasPrefix(without, "sh -lc 'set -eu; umask 077; home=${HOME:-/root}; echo ") ||
		!strings.HasSuffix(without, "Provider login is still required on this mirror.\"'") {
		t.Fatalf("without credentials: %s", without)
	}
}

func TestAgentCliInstallWithoutCredentials(t *testing.T) {
	f := newCliFixture(t)
	t.Setenv("FAKE_SSH_MODE", "install")
	started := f.call(t, "core.hosts.agentClis", map[string]any{"node": "mirror1", "ip": "203.0.113.5", "user": "root",
		"password": "", "identityFile": "", "copyCredentials": false})
	if started["started"] != true || started["message"] != "" {
		t.Fatalf("started %v", started)
	}
	job := f.waitJob(t, started["job"].(string))
	want := "Claude Code and Codex are installed on mirror1. Finish the provider sign-ins in the terminal that just opened."
	if !job.OK || job.Kind != "agentClis" || job.Message != want || job.Origin != "desktop" || job.Host != "mirror1" {
		t.Fatalf("job %+v", job)
	}
	events := cliCollectUntil(t, f.ui, "core.hosts.loginTerminal", 5*time.Second)
	login := events.named("core.hosts.loginTerminal")[0]
	if login["node"] != "mirror1" || login["ip"] != "203.0.113.5" || login["user"] != "root" {
		t.Fatalf("login %v", login)
	}
	if len(events.named("core.hosts.probeRequested")) != 1 {
		t.Fatal("no probe request before the login terminal")
	}
	if f.mod.status("install") != want {
		t.Fatalf("status %q", f.mod.status("install"))
	}
	transcript := f.mod.transcriptText(installChannel)
	for _, line := range []string{
		"Installing Claude Code and Codex on mirror1 (root@203.0.113.5)...\n",
		"The shared ForkMesh SSH key has not been generated on this device; using the current session credential or the system SSH agent.\n",
		"Installing Claude Code from claude.ai...\n",
		"\nAgent CLI installation finished.\n",
	} {
		if !strings.Contains(transcript, line) {
			t.Fatalf("transcript lacks %q:\n%s", line, transcript)
		}
	}
	if strings.Contains(transcript, "Copying this device") {
		t.Fatalf("copied without credentials:\n%s", transcript)
	}
	log := f.sshLog()
	if !strings.Contains(log, "BatchMode=yes") || !strings.Contains(log, "root@203.0.113.5 "+agentCliBootstrapRemoteCommand(false)) {
		t.Fatalf("ssh log %s", log)
	}
	if len(f.stdinBytes()) != 0 {
		t.Fatalf("stdin %q", f.stdinBytes())
	}
}

func TestAgentCliInstallCopiesCredentialsOverStdin(t *testing.T) {
	f := newCliFixture(t)
	t.Setenv("FAKE_SSH_MODE", "install")
	f.setLogins(t, "  {\"token\":\"secret\"}\n", "")
	conf, _ := os.ReadFile(f.core.Layout.SettingsFile())
	conf = append(conf, []byte("\n[agents]\ncodexApiKey=sk-it's\n")...)
	if err := os.WriteFile(f.core.Layout.SettingsFile(), conf, 0o600); err != nil {
		t.Fatal(err)
	}
	payload := "claude " + base64.StdEncoding.EncodeToString([]byte(`{"token":"secret"}`)) + "\n" +
		"env " + base64.StdEncoding.EncodeToString([]byte("export OPENAI_API_KEY='sk-it'\\''s'\n")) + "\n"
	started := f.call(t, "core.hosts.agentClis", map[string]any{"node": "mirror1", "ip": "203.0.113.5", "user": "root",
		"password": "pw", "copyCredentials": true})
	job := f.waitJob(t, started["job"].(string))
	if !job.OK || job.Message != "Claude Code and Codex are installed on mirror1 and signed in with this device's access." {
		t.Fatalf("job %+v", job)
	}
	if got := string(f.stdinBytes()); got != payload {
		t.Fatalf("stdin %q", got)
	}
	events := cliCollectUntil(t, f.ui, "core.hosts.probeRequested", 5*time.Second)
	events = append(events, cliDrain(f.ui, 150*time.Millisecond)...)
	if len(events.named("core.hosts.loginTerminal")) != 0 {
		t.Fatal("a signed-in install opened a login terminal")
	}
	transcript := f.mod.transcriptText(installChannel)
	if !strings.Contains(transcript, "Copying this device's agent access (Claude Code login, OPENAI_API_KEY) over the SSH session's stdin.\n") {
		t.Fatalf("transcript:\n%s", transcript)
	}
	log := f.sshLog()
	if !strings.HasPrefix(log, "-e ssh ") || !strings.Contains(log, agentCliBootstrapRemoteCommand(true)) || strings.Contains(log, "secret") {
		t.Fatalf("ssh log %s", log)
	}
}

func TestAgentCliInstallCredentialFailures(t *testing.T) {
	f := newCliFixture(t)
	result := f.call(t, "core.hosts.agentClis", map[string]any{"node": "mirror1", "ip": "203.0.113.5", "user": "root", "copyCredentials": true})
	want := "This device has no Claude Code or Codex login to copy to mirror1. Sign in here first, then use Install Claude + Codex on that host."
	if result["started"] != false || result["job"] != "" || result["message"] != want || f.mod.status("install") != want {
		t.Fatalf("empty: %v / %q", result, f.mod.status("install"))
	}
	bad := "This device's Codex login file is not readable as a credential document."
	f.setLogins(t, "", "not a document")
	result = f.call(t, "core.hosts.agentClis", map[string]any{"node": "mirror1", "ip": "203.0.113.5", "user": "root", "copyCredentials": true})
	if result["started"] != false || result["message"] != bad {
		t.Fatalf("unreadable: %v", result)
	}
	f.setLogins(t, "", "")
	f.attached.Store(false)
	result = f.call(t, "core.hosts.agentClis", map[string]any{"node": "mirror1", "ip": "203.0.113.5", "user": "root", "copyCredentials": true})
	if result["started"] != false || result["message"] != want {
		t.Fatalf("no desktop: %v", result)
	}
	result = f.call(t, "core.hosts.agentClis", map[string]any{"node": "mirror1", "ip": "bad..host", "user": "root"})
	if result["started"] != false || result["message"] != "The saved SSH host or username is invalid." {
		t.Fatalf("bad host: %v", result)
	}
	if f.sshLog() != "" {
		t.Fatalf("ssh ran: %s", f.sshLog())
	}
}

func TestAgentCliInstallFailureMessages(t *testing.T) {
	f := newCliFixture(t)
	key := filepath.Join(t.TempDir(), "forkmesh_shared_ed25519")
	if err := os.WriteFile(key, []byte("k"), 0o600); err != nil {
		t.Fatal(err)
	}
	cases := []struct {
		mode     string
		identity string
		want     string
	}{
		{"timeout", key, "SSH could not reach 203.0.113.5 on port 22. The saved key was not reached; use the host's reachable public/stable address or connect this device to the private network, then retry."},
		{"denied", key, "The mirror rejected the shared ForkMesh SSH key. Add that key to mirror1's authorized_keys, then retry."},
		{"denied", "", "Agent CLI installation failed on mirror1; see Live output."},
	}
	for _, c := range cases {
		t.Setenv("FAKE_SSH_MODE", c.mode)
		started := f.call(t, "core.hosts.agentClis", map[string]any{"node": "mirror1", "ip": "203.0.113.5", "user": "root", "identityFile": c.identity})
		job := f.waitJob(t, started["job"].(string))
		if job.OK || job.Message != c.want || job.Result["exitCode"] != 255 {
			t.Fatalf("%s: %+v", c.mode, job)
		}
		transcript := f.mod.transcriptText(installChannel)
		if !strings.Contains(transcript, "\nAgent CLI installation failed (exit 255).\n") {
			t.Fatalf("%s transcript:\n%s", c.mode, transcript)
		}
		if c.identity != "" && !strings.Contains(transcript, "Using the shared ForkMesh SSH identity ("+key+").\n") {
			t.Fatalf("%s identity line:\n%s", c.mode, transcript)
		}
		events := cliCollectUntil(t, f.ui, "core.hosts.probeRequested", 5*time.Second)
		events = append(events, cliDrain(f.ui, 100*time.Millisecond)...)
		if len(events.named("core.hosts.loginTerminal")) != 0 {
			t.Fatalf("%s opened a login terminal", c.mode)
		}
	}
}

func TestAgentCliInstallBusyAndUnstartable(t *testing.T) {
	f := newCliFixture(t)
	t.Setenv("FAKE_SSH_MODE", "hang")
	started := f.call(t, "core.hosts.agentClis", map[string]any{"node": "mirror1", "ip": "203.0.113.5", "user": "root"})
	second := f.call(t, "core.hosts.agentClis", map[string]any{"node": "box", "ip": "198.51.100.7", "user": "deploy"})
	if second["started"] != false || second["message"] != agentCliBusyMessage || f.mod.status("install") != agentCliBusyMessage {
		t.Fatalf("busy: %v", second)
	}
	for deadline := time.Now().Add(10 * time.Second); f.sshLog() == ""; time.Sleep(5 * time.Millisecond) {
		if time.Now().After(deadline) {
			t.Fatal("the fake ssh never started")
		}
	}
	f.call(t, "core.hosts.cancel", map[string]any{"id": started["job"]})
	job := f.waitJob(t, started["job"].(string))
	if job.OK || job.Message != "Agent CLI installation failed on mirror1; see Live output." {
		t.Fatalf("cancelled %+v", job)
	}
	t.Setenv("PATH", t.TempDir())
	started = f.call(t, "core.hosts.agentClis", map[string]any{"node": "mirror1", "ip": "203.0.113.5", "user": "root"})
	job = f.waitJob(t, started["job"].(string))
	for deadline := time.Now().Add(5 * time.Second); f.mod.status("install") != job.Message && time.Now().Before(deadline); {
		time.Sleep(5 * time.Millisecond)
	}
	if job.OK || job.Message != "Could not start the pinned SSH installer." || f.mod.status("install") != job.Message {
		t.Fatalf("unstartable %+v", job)
	}
}
