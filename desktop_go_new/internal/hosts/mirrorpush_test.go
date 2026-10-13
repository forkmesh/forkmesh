package hosts

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"log"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
	"testing"
	"time"

	"forkmesh.com/desktop-core/internal/coretest"
	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/rpc"
)

const mpFakeGit = `#!/bin/sh
printf 'git %s | %s | %s\n' "$*" "$GIT_SSH_COMMAND" "$GIT_TERMINAL_PROMPT" >> "$MP_LOG"
if [ "$1" = remote ] && [ "$2" = -v ]; then printf '%s\n' "$MP_REMOTES"; exit 0; fi
if [ "$1" = remote ]; then exit 0; fi
if [ -n "$MP_DOWN" ] && printf '%s' "$*" | grep -q "$MP_DOWN"; then
  echo "ssh: connect to host $MP_DOWN port 22: Connection timed out" >&2
  echo "fatal: Could not read from remote repository." >&2
  exit 128
fi
if [ -n "$MP_HELD" ] && printf '%s' "$*" | grep -q "$MP_HELD"; then
  printf 'To remote\n*\trefs/heads/main:refs/heads/main\t[new branch]\n!\trefs/heads/forkmesh/pulls:refs/heads/forkmesh/pulls\t[rejected] (non-fast-forward)\nDone\n'
  echo "hint: Updates were rejected because the tip is behind" >&2
  exit 1
fi
if [ -n "$MP_CURRENT" ]; then printf 'To remote\n=\trefs/heads/main:refs/heads/main\t[up to date]\nDone\n'; exit 0; fi
printf 'To remote\n*\trefs/heads/main:refs/heads/main\t[new branch]\nDone\n'
exit 0
`

const mpFakeSSH = `#!/bin/sh
printf 'ssh %s\n' "$*" >> "$MP_LOG"
case "$*" in
  *"for hash in"*) if [ -n "$MP_MISSING" ]; then printf '%s\n' "$MP_MISSING"; fi; exit 0 ;;
  *".seed.XXXXXX"*) cat > "$MP_SEEDS/seed.$$"; exit "${MP_SEED_EXIT:-0}" ;;
  *"control/sync"*) exit "${MP_WAKE_EXIT:-0}" ;;
esac
exit 0
`

type mpBuffer struct {
	mu sync.Mutex
	b  bytes.Buffer
}

func (l *mpBuffer) Write(p []byte) (int, error) {
	l.mu.Lock()
	defer l.mu.Unlock()
	return l.b.Write(p)
}

func (l *mpBuffer) String() string {
	l.mu.Lock()
	defer l.mu.Unlock()
	return l.b.String()
}

type mpFixture struct {
	t      *testing.T
	core   *daemon.Core
	mod    *Module
	ui     *coretest.Peer
	phone  *coretest.Peer
	logs   *mpBuffer
	calls  string
	seeds  string
	repo   mirrorRepo
	blobs  []string
	mu     sync.Mutex
	writes []daemon.SettingWrite
}

func mpHosts() []map[string]any {
	return []map[string]any{
		{"name": "mirror1", "ip": "203.0.113.5", "user": "root", "status": "installed"},
		{"name": "mirror2", "ip": "203.0.113.6", "user": "root", "status": "installing"},
		{"name": "box", "ip": "203.0.113.7", "user": "admin", "status": "installed"},
		{"name": "mirror3", "ip": "203.0.113.8", "user": "root"},
	}
}

func mpWriteConf(t *testing.T, core *daemon.Core, pending []string) {
	t.Helper()
	encoded, _ := json.Marshal(mpHosts())
	text := "[account]\nnodeName=alice\n\n[hosts]\nlist=" + quoteSetting(string(encoded)) + "\n"
	if len(pending) > 0 {
		quoted := []string{}
		for _, token := range pending {
			quoted = append(quoted, quoteSetting(token))
		}
		text += "\n[releases]\npendingMirrorPublications=" + strings.Join(quoted, ", ") + "\n"
	}
	path := core.Layout.SettingsFile()
	_ = os.MkdirAll(filepath.Dir(path), 0o700)
	if err := os.WriteFile(path, []byte(text), 0o600); err != nil {
		t.Fatal(err)
	}
	stamp := time.Now().Add(time.Duration(len(pending)+1) * time.Second)
	_ = os.Chtimes(path, stamp, stamp)
}

func newMpFixture(t *testing.T) *mpFixture {
	t.Helper()
	if runtime.GOOS == "windows" {
		t.Skip("fake tools are shell scripts")
	}
	bin := t.TempDir()
	for name, script := range map[string]string{"git": mpFakeGit, "ssh": mpFakeSSH, "sshpass": mpFakeSSH} {
		if err := os.WriteFile(filepath.Join(bin, name), []byte(script), 0o755); err != nil {
			t.Fatal(err)
		}
	}
	work := t.TempDir()
	f := &mpFixture{t: t, calls: filepath.Join(work, "calls.log"), seeds: filepath.Join(work, "seeds"), logs: &mpBuffer{}}
	_ = os.MkdirAll(f.seeds, 0o700)
	t.Setenv("PATH", bin+string(os.PathListSeparator)+os.Getenv("PATH"))
	t.Setenv("GIT_SSH_COMMAND", "")
	os.Unsetenv("GIT_SSH_COMMAND")
	for key, value := range map[string]string{"MP_LOG": f.calls, "MP_SEEDS": f.seeds, "MP_DOWN": "", "MP_HELD": "", "MP_CURRENT": "",
		"MP_MISSING": "", "MP_SEED_EXIT": "0", "MP_WAKE_EXIT": "0",
		"MP_REMOTES": "origin\thttps://example.org/forkmesh.git (push)\ngw1\tssh://git@gw1.example/forkmesh.git (fetch)\ngw1\tssh://git@gw1.example/forkmesh.git (push)\ngw2\tssh://git@gw2.example/forkmesh.git (push)\nnode\tssh://root@203.0.113.5/srv/forkmesh.git (push)"} {
		t.Setenv(key, value)
	}
	f.core = coretest.New(t)
	f.core.Logger = log.New(f.logs, "", 0)
	mpWriteConf(t, f.core, nil)
	local := filepath.Join(work, "checkout")
	mirror := filepath.Join(work, "mirror.git")
	_ = os.MkdirAll(local, 0o700)
	for _, content := range []string{"artifact one, the larger of the two", "artifact two"} {
		sum := sha256.Sum256([]byte(content))
		hash := hex.EncodeToString(sum[:])
		dir := filepath.Join(mirror, "forkmesh-releases", "sha256", hash[:2], hash)
		_ = os.MkdirAll(dir, 0o700)
		if err := os.WriteFile(filepath.Join(dir, "data"), []byte(content), 0o600); err != nil {
			t.Fatal(err)
		}
		f.blobs = append(f.blobs, hash)
	}
	f.repo = mirrorRepo{Owner: "forkmesh", Name: "forkmesh", LocalPath: local, MirrorPath: mirror}
	f.mod = New()
	f.mod.Owners = func() []string { return []string{"alice"} }
	f.mod.SSHDir = filepath.Join(work, "ssh")
	f.mod.Attached = func() bool { return true }
	f.mod.WriteSettings = func(ctx context.Context, writes []daemon.SettingWrite) error {
		f.mu.Lock()
		f.writes = append(f.writes, writes...)
		f.mu.Unlock()
		return nil
	}
	coretest.Start(t, f.core, f.mod)
	f.ui = coretest.NewPeer("ui", rpc.UI)
	f.phone = coretest.NewPeer("phone", rpc.Relay)
	f.phone.Dev.User = "alice"
	f.core.Hub.Add(f.ui)
	f.core.Hub.Add(f.phone)
	t.Cleanup(func() {
		deadline := time.Now().Add(10 * time.Second)
		for f.mod.Busy() && time.Now().Before(deadline) {
			time.Sleep(10 * time.Millisecond)
		}
	})
	return f
}

func (f *mpFixture) callLog() string {
	data, _ := os.ReadFile(f.calls)
	return string(data)
}

func (f *mpFixture) waitLog(want string) string {
	f.t.Helper()
	deadline := time.Now().Add(10 * time.Second)
	for time.Now().Before(deadline) {
		if text := f.logs.String(); strings.Contains(text, want) {
			return text
		}
		time.Sleep(10 * time.Millisecond)
	}
	f.t.Fatalf("log never said %q:\n%s", want, f.logs.String())
	return ""
}

func (f *mpFixture) waitIdle() {
	f.t.Helper()
	deadline := time.Now().Add(10 * time.Second)
	for f.mod.Busy() && time.Now().Before(deadline) {
		time.Sleep(10 * time.Millisecond)
	}
	if f.mod.Busy() {
		f.t.Fatal("the mirror push never finished")
	}
}

func (f *mpFixture) request(method string, params map[string]any) map[string]any {
	f.t.Helper()
	if _, ok := params["repo"]; !ok {
		params["repo"] = f.repo
	}
	raw, err := coretest.Call(f.t, f.core, f.ui, method, params)
	if err != nil {
		f.t.Fatalf("%s: %v", method, err)
	}
	return decode(f.t, raw)
}

func (f *mpFixture) finished(token string) map[string]any {
	f.t.Helper()
	deadline := time.Now().Add(10 * time.Second)
	for time.Now().Before(deadline) {
		event := decode(f.t, f.ui.Next(f.t, "core.mirror.finished", time.Until(deadline)))
		if event["token"] == token {
			return event
		}
	}
	f.t.Fatalf("no finish for %s", token)
	return nil
}

func (f *mpFixture) healthWrites() []string {
	f.mu.Lock()
	defer f.mu.Unlock()
	out := []string{}
	for _, write := range f.writes {
		if write.Key != gatewayHealthSetting {
			continue
		}
		if write.Remove {
			out = append(out, "<removed>")
			continue
		}
		out = append(out, strings.Join(write.Value.([]string), "|"))
	}
	return out
}

func TestMirrorPushRemotesHealthAndHeldBack(t *testing.T) {
	f := newMpFixture(t)
	t.Setenv("MP_DOWN", "gw2.example")
	answer := f.request("core.mirror.push", map[string]any{})
	if answer["targets"] != 3.0 || answer["idleReason"] != "" {
		t.Fatalf("answer %v", answer)
	}
	f.waitLog("Mirror: pushed forkmesh/forkmesh to SSH mirror gw1.example.")
	text := f.waitLog("Mirror: SSH gateway gw2.example did not answer for forkmesh/forkmesh: ssh: connect to host gw2.example port 22: Connection timed out fatal: Could not read from remote repository.. Skipping it for about 1 minute while the rest of the fleet keeps syncing.")
	f.waitIdle()
	if strings.Contains(text, "hint:") {
		t.Fatalf("hint leaked: %s", text)
	}
	calls := f.callLog()
	want := "git -C " + f.repo.LocalPath + " push --porcelain --no-force --no-prune ssh://git@gw1.example/forkmesh.git refs/heads/*:refs/heads/* refs/tags/*:refs/tags/* refs/pr/*:refs/pr/* | " + defaultGitSSHCommand + " | 0"
	if !strings.Contains(calls, want) || strings.Contains(calls, "https://example.org") {
		t.Fatalf("calls:\n%s", calls)
	}
	writes := f.healthWrites()
	if len(writes) != 1 || !strings.HasPrefix(writes[0], "gw2.example\t") || !strings.HasSuffix(writes[0], "\t0") {
		t.Fatalf("health writes %v", writes)
	}
	answer = f.request("core.mirror.push", map[string]any{})
	if answer["targets"] != 2.0 {
		t.Fatalf("cooling gateway was pushed: %v", answer)
	}
	f.waitIdle()
	summary, err := coretest.Call(t, f.core, f.phone, "fleet.summary", map[string]any{})
	if err != nil {
		t.Fatal(err)
	}
	gateways := decode(t, summary)["gateways"].([]any)
	if len(gateways) != 1 || gateways[0].(map[string]any)["host"] != "gw2.example" {
		t.Fatalf("gateways %v", gateways)
	}
	t.Setenv("MP_DOWN", "")
	t.Setenv("MP_HELD", "gw1.example")
	answer = f.request("core.mirror.push", map[string]any{"userInitiated": true, "pruneRefs": true})
	if answer["targets"] != 3.0 {
		t.Fatalf("user push %v", answer)
	}
	f.waitLog("Mirror: SSH mirror gw1.example is ahead of this node on forkmesh/pulls for forkmesh/forkmesh, so those branches were left alone")
	f.waitLog("Mirror: SSH gateway gw2.example is answering again.")
	f.waitIdle()
	if !strings.Contains(f.callLog(), "push --porcelain --no-force --prune ssh://git@gw2.example/forkmesh.git refs/heads/*:refs/heads/* refs/tags/*:refs/tags/* +refs/pr/*:refs/pr/*") {
		t.Fatalf("prune push:\n%s", f.callLog())
	}
	writes = f.healthWrites()
	if writes[len(writes)-1] != "<removed>" {
		t.Fatalf("health not cleared: %v", writes)
	}
}

func TestMirrorPushIdleReasons(t *testing.T) {
	f := newMpFixture(t)
	private := f.repo
	private.Private = true
	if answer := f.request("core.mirror.push", map[string]any{"repo": private}); answer["idleReason"] != "this repository has no source working copy to push from" {
		t.Fatalf("private %v", answer)
	}
	gone := f.repo
	gone.LocalPath = filepath.Join(t.TempDir(), "gone")
	if answer := f.request("core.mirror.push", map[string]any{"repo": gone}); answer["idleReason"] != "the working copy "+gone.LocalPath+" no longer exists" {
		t.Fatalf("gone %v", answer)
	}
	t.Setenv("MP_REMOTES", "origin\thttps://example.org/x.git (push)")
	if answer := f.request("core.mirror.push", map[string]any{}); answer["targets"] != 0.0 ||
		answer["idleReason"] != f.repo.LocalPath+" has no ssh:// push remote, so the SSH-fed mirrors receive nothing. Add the gateway's push remote under Remotes on this repository" {
		t.Fatalf("no remotes %v", answer)
	}
	mpWriteConf(t, f.core, []string{releaseRolloutToken("ForkMesh", "ForkMesh", "v1.0")})
	if answer := f.request("core.mirror.push", map[string]any{}); answer["idleReason"] != "release refs are being held until every signed artifact is present on the managed mirrors" {
		t.Fatalf("held %v", answer)
	}
	if answer := f.request("core.mirror.push", map[string]any{"releaseTag": "v2.0"}); answer["idleReason"] != "a newer release publication superseded this ref push" {
		t.Fatalf("superseded %v", answer)
	}
	if _, err := coretest.Call(t, f.core, f.ui, "core.mirror.push", map[string]any{"repo": map[string]any{"owner": "a/b", "name": "c"}}); rpc.Code(err) != "bad_repo" {
		t.Fatalf("bad repo %v", err)
	}
	if _, err := coretest.Call(t, f.core, f.phone, "core.mirror.push", map[string]any{"repo": f.repo}); rpc.Code(err) != "forbidden" {
		t.Fatalf("phone %v", err)
	}
}

func TestManagedPushTargetsAndWake(t *testing.T) {
	f := newMpFixture(t)
	answer := f.request("core.mirror.managed", map[string]any{"reconcileArtifacts": false, "token": "t1"})
	if answer["targets"] != 2.0 {
		t.Fatalf("answer %v", answer)
	}
	event := f.finished("t1")
	if event["succeeded"] != 2.0 || event["failed"] != 0.0 {
		t.Fatalf("event %v", event)
	}
	f.waitLog("Mirror: pushed forkmesh/forkmesh to managed host mirror1 (203.0.113.5) and woke its daemon.")
	f.waitLog("Mirror: managed-host ref fan-out for forkmesh/forkmesh finished (2 updated, 0 failed).")
	calls := f.callLog()
	for _, want := range []string{
		"push --porcelain --no-force --no-prune --receive-pack=runuser -u forkmesh-node -- git-receive-pack ssh://root@203.0.113.5/var/lib/forkmesh/repositories/forkmesh-forkmesh.git refs/heads/*:refs/heads/* refs/tags/*:refs/tags/* refs/pr/*:refs/pr/* | 'ssh' '-o' 'StrictHostKeyChecking=accept-new'",
		"'-o' 'ServerAliveInterval=15' '-o' 'ServerAliveCountMax=2' | 0",
		"ssh://root@203.0.113.8/var/lib/forkmesh/repositories/forkmesh-forkmesh.git",
		"root@203.0.113.5 " + managedWakeCommand,
	} {
		if !strings.Contains(calls, want) {
			t.Fatalf("calls lack %q:\n%s", want, calls)
		}
	}
	if strings.Contains(calls, "203.0.113.6") || strings.Contains(calls, "203.0.113.7") || strings.Contains(calls, "'true'") {
		t.Fatalf("wrong targets:\n%s", calls)
	}
	_ = os.Remove(f.calls)
	t.Setenv("MP_CURRENT", "1")
	f.request("core.mirror.managed", map[string]any{"reconcileArtifacts": false, "wakeIfUnchanged": false, "token": "t2"})
	if event := f.finished("t2"); event["succeeded"] != 2.0 {
		t.Fatalf("current %v", event)
	}
	if strings.Contains(f.callLog(), "control/sync") {
		t.Fatalf("an unchanged safety pass woke the daemons:\n%s", f.callLog())
	}
	other := f.repo
	other.Owner = "alice"
	if answer := f.request("core.mirror.managed", map[string]any{"repo": other, "token": "t3"}); answer["targets"] != 0.0 {
		t.Fatalf("other repo %v", answer)
	}
	if event := f.finished("t3"); event["succeeded"] != 0.0 || event["failed"] != 0.0 {
		t.Fatalf("other repo event %v", event)
	}
}

func TestReleaseSeedsArtifactsBeforeRefs(t *testing.T) {
	f := newMpFixture(t)
	mpWriteConf(t, f.core, []string{releaseRolloutToken("forkmesh", "forkmesh", "v1.0")})
	answer := f.request("core.mirror.release", map[string]any{"releaseTag": "v1.0", "artifactHashes": []string{strings.ToUpper(f.blobs[1])}, "token": "r1"})
	if answer["targets"] != 2.0 {
		t.Fatalf("answer %v", answer)
	}
	event := f.finished("r1")
	if event["succeeded"] != 2.0 || event["failed"] != 0.0 {
		t.Fatalf("event %v", event)
	}
	f.waitLog("Release: seeded 1 signed artifact on every managed host before advancing refs.")
	f.waitLog("Release: managed-host ref fan-out for v1.0 finished (2 succeeded, 0 failed).")
	calls := f.callLog()
	lastSeed := strings.LastIndex(calls, ".seed.XXXXXX")
	firstPush := strings.Index(calls, "push --porcelain")
	if lastSeed < 0 || firstPush < 0 || lastSeed > firstPush {
		t.Fatalf("refs moved before the artifacts:\n%s", calls)
	}
	if !strings.Contains(calls, "sha256/"+f.blobs[1][:2]+"/"+f.blobs[1]+"; install -d -o forkmesh-node") {
		t.Fatalf("seed command:\n%s", calls)
	}
	entries, _ := os.ReadDir(f.seeds)
	if len(entries) != 2 {
		t.Fatalf("seeds %d", len(entries))
	}
	for _, entry := range entries {
		data, _ := os.ReadFile(filepath.Join(f.seeds, entry.Name()))
		if string(data) != "artifact two" {
			t.Fatalf("seeded %q", data)
		}
	}
	cases := []struct {
		tag, want string
		hashes    []string
	}{
		{"v2.0", "Release: v2.0 refs were held back because a newer publication superseded this fan-out.", f.blobs[:1]},
		{"v1.0", "Release: v1.0 refs were held back because its signed artifact list contains an invalid digest.", []string{"abc"}},
		{"v1.0", "Release: v1.0 refs were held back because artifact " + strings.Repeat("e", 12) + " is not present in the local served CAS.", []string{strings.Repeat("e", 64)}},
		{"v1.0", "Release: managed-host refs were held back because no authenticated artifact set was supplied.", nil},
	}
	for i, c := range cases {
		token := "bad" + itoa(i)
		if answer := f.request("core.mirror.release", map[string]any{"releaseTag": c.tag, "artifactHashes": c.hashes, "token": token}); answer["targets"] != 0.0 {
			t.Fatalf("%s: %v", c.want, answer)
		}
		if event := f.finished(token); event["failed"] != 1.0 {
			t.Fatalf("%s: %v", c.want, event)
		}
		f.waitLog(c.want)
	}
	t.Setenv("MP_SEED_EXIT", "74")
	f.request("core.mirror.release", map[string]any{"releaseTag": "v1.0", "artifactHashes": f.blobs[:1], "token": "r2"})
	if event := f.finished("r2"); event["succeeded"] != 0.0 || event["failed"] != 2.0 {
		t.Fatalf("seed failure %v", event)
	}
	f.waitLog("Release: v1.0 artifact seeding failed on 2 managed hosts; refs and deployment remain pending.")
	f.waitLog("Release: could not seed artifact " + f.blobs[0][:12] + " on managed host mirror1 (203.0.113.5): SSH artifact transfer failed")
}

func TestArtifactReconciliationRestoresMissingObjects(t *testing.T) {
	f := newMpFixture(t)
	t.Setenv("MP_MISSING", f.blobs[1])
	f.request("core.mirror.managed", map[string]any{"token": "a1"})
	f.finished("a1")
	f.waitLog("Artifacts: restored 2 retained CAS objects across the managed mirror fleet.")
	f.waitIdle()
	calls := f.callLog()
	if !strings.Contains(calls, "for hash in "+f.blobs[0]+" "+f.blobs[1]+"; do") {
		t.Fatalf("inventory:\n%s", calls)
	}
	entries, _ := os.ReadDir(f.seeds)
	if len(entries) != 2 {
		t.Fatalf("restored %d", len(entries))
	}
	t.Setenv("MP_MISSING", "not-a-hash")
	f.request("core.mirror.managed", map[string]any{"token": "a2"})
	f.finished("a2")
	f.waitLog("Artifacts: managed host mirror1 (203.0.113.5) returned an invalid inventory.")
	f.waitLog("Artifacts: managed-host reconciliation failed on 2 hosts and will retry; normal ref fan-out continued independently.")
}

func TestUnwireRemovesDeadNodeRemotes(t *testing.T) {
	f := newMpFixture(t)
	other := filepath.Join(t.TempDir(), "other")
	_ = os.MkdirAll(other, 0o700)
	result := f.request("core.mirror.repos", map[string]any{"repos": []mirrorRepo{f.repo, {Owner: "alice", Name: "app", LocalPath: other}, {Owner: "bad/owner", Name: "x"}}})
	if result["count"] != 2.0 {
		t.Fatalf("repos %v", result)
	}
	answer := f.request("core.mirror.unwire", map[string]any{"node": "mirror1"})
	if answer["removed"] != 3.0 {
		t.Fatalf("unwire %v\n%s", answer, f.callLog())
	}
	event := decode(t, f.ui.Next(t, "core.mirror.unwired", 5*time.Second))
	if event["node"] != "mirror1" || event["removed"] != 3.0 {
		t.Fatalf("event %v", event)
	}
	calls := f.callLog()
	if strings.Count(calls, "git remote remove node |") != 3 || strings.Contains(calls, "remote remove gw") {
		t.Fatalf("calls:\n%s", calls)
	}
	f.waitLog("Mirror: removed git remote node from " + f.repo.LocalPath + " — node mirror1 was deleted.")
	if answer := f.request("core.mirror.unwire", map[string]any{"node": "gw1"}); answer["removed"] != 3.0 {
		t.Fatalf("by name %v", answer)
	}
	if answer := f.request("core.mirror.unwire", map[string]any{"node": "gw"}); answer["removed"] != 0.0 {
		t.Fatalf("partial segment matched %v", answer)
	}
	raw, _ := os.ReadFile(filepath.Join(f.core.Layout.CoreState(), mirrorReposFile))
	if !strings.Contains(string(raw), "alice") {
		t.Fatalf("repo table not kept: %s", raw)
	}
}

func TestPhoneMirrorPushRunsInTheCore(t *testing.T) {
	f := newMpFixture(t)
	fixture := &fixture{core: f.core, mod: f.mod, ui: f.ui, phone: f.phone}
	j := &jobFixture{fixture: fixture}
	if _, err := j.fleet(t, map[string]any{"action": "mirrorPush", "repo": "ForkMesh/ForkMesh"}); rpc.Code(err) != "unknown_repo" {
		t.Fatalf("unknown %v", err)
	}
	f.request("core.mirror.repos", map[string]any{"repos": []mirrorRepo{f.repo}})
	started, err := j.fleet(t, map[string]any{"action": "mirrorPush", "repo": "ForkMesh/ForkMesh"})
	if err != nil || started["message"] != "Pushing to 3 SSH remote(s) and 2 managed host(s)." || started["remotes"] != 3.0 || started["managedHosts"] != 2.0 {
		t.Fatalf("push %v %v", started, err)
	}
	event := decode(t, f.phone.Next(t, "fleet.action", 10*time.Second))
	if event["ok"] != true || event["action"] != "mirrorPush" || event["subject"] != "forkmesh/forkmesh" || event["message"] != "Managed hosts: 2 updated, 0 failed." {
		t.Fatalf("event %v", event)
	}
	f.waitLog("Mirror push of forkmesh/forkmesh requested by")
	t.Setenv("MP_REMOTES", "")
	alone := mirrorRepo{Owner: "alice", Name: "app", LocalPath: f.repo.LocalPath}
	f.request("core.mirror.repos", map[string]any{"repos": []mirrorRepo{alone}})
	if _, err := j.fleet(t, map[string]any{"action": "mirrorPush", "repo": "alice/app"}); rpc.Code(err) != alone.LocalPath+" has no ssh:// push remote, so the SSH-fed mirrors receive nothing. Add the gateway's push remote under Remotes on this repository" {
		t.Fatalf("idle %v", err)
	}
}

func TestMirrorHelpersParity(t *testing.T) {
	outcome := classifyMirrorPush("To ssh://x\n*\trefs/heads/main:refs/heads/main\t[new branch]\n=\trefs/tags/v1:refs/tags/v1\t[up to date]\n!\trefs/heads/a:refs/heads/a\t[rejected] (fetch first)\n!\trefs/heads/b:refs/heads/b\t[rejected] (stale info)\nDone\n")
	if strings.Join(outcome.updated, ",") != "main" || strings.Join(outcome.heldBack, ",") != "a,b" || !outcome.onlyHeldBack {
		t.Fatalf("outcome %+v", outcome)
	}
	if hard := classifyMirrorPush("!\trefs/heads/a:refs/heads/a\t[remote rejected] (hook declined)\n"); hard.onlyHeldBack {
		t.Fatalf("hard %+v", hard)
	}
	if !mirrorGatewayHostBelongsToNode("forkmesh-mirror10-sync", "mirror10") || mirrorGatewayHostBelongsToNode("mirror10.forkmesh.com", "mirror1") ||
		!mirrorGatewayHostBelongsToNode("mirror10.forkmesh.com", "Mirror10") {
		t.Fatal("node match")
	}
	if !mirrorGatewayUnreachable("ssh: Could not resolve hostname x: Name or service not known") || mirrorGatewayUnreachable("remote: permission denied") {
		t.Fatal("unreachable")
	}
	health := gatewayHealth{backoff: networkBackoff{jitter: func(int64) int64 { return 0 }}}
	now := int64(10_000_000_000)
	notice := health.noteUnreachable("gw", now)
	if !notice.announce || !notice.persist || notice.cooldownMs != gatewayBaseCooldownMs || health.readyToPush("gw", now) {
		t.Fatalf("first %+v", notice)
	}
	if notice = health.noteUnreachable("gw", now+1); notice.announce || notice.cooldownMs != 2*gatewayBaseCooldownMs {
		t.Fatalf("second %+v", notice)
	}
	if delay := mirrorPushRetryDelayMs(&health, []string{"gw"}, now+1, 60_000); delay != 2*gatewayBaseCooldownMs+250 {
		t.Fatalf("delay %d", delay)
	}
	if notice = health.noteUnreachable("gw", now+gatewayRetiredAfterMs); !notice.retired || !notice.parked || notice.cooldownMs != gatewayRetiredCooldownMs {
		t.Fatalf("retired %+v", notice)
	}
	state := health.persistedState()
	if len(state) != 1 || state[0] != "gw\t10000000000\t10000000000\t1" {
		t.Fatalf("state %v", state)
	}
	restored := gatewayHealth{}
	restored.restoreState(append(state, "future\t99999999999999\t0\t0", "short\t1"), now+gatewayRetiredAfterMs)
	if got := restored.persistedState(); len(got) != 1 || got[0] != state[0] || !restored.readyToPush("gw", now) {
		t.Fatalf("restored %v", got)
	}
	gate := refPushGate{}
	first := gate.reserve("ForkMesh/ForkMesh", []string{"A", "b", "a"})
	if len(first.readyHosts) != 2 || first.replayQueued {
		t.Fatalf("first %+v", first)
	}
	second := gate.reserve("forkmesh/forkmesh", []string{"a"})
	if len(second.readyHosts) != 0 || !second.replayQueued || !gate.busy("forkmesh/forkmesh", []string{"B"}) {
		t.Fatalf("second %+v", second)
	}
	if gate.finish("forkmesh/forkmesh", "a") || !gate.finish("forkmesh/forkmesh", "b") || gate.finish("forkmesh/forkmesh", "b") {
		t.Fatal("gate replay")
	}
	if token := releaseRolloutToken(" ForkMesh ", "ForkMesh", " v1.0 "); token != `["forkmesh","forkmesh","v1.0"]` {
		t.Fatalf("token %s", token)
	}
	if managedReleaseArtifactInventoryCommand([]string{f64("a"), f64("a")}) != "" || managedReleaseArtifactSeedCommand("xyz") != "" {
		t.Fatal("digest validation")
	}
	if managedPushURL("root", "2001:DB8::1") != "ssh://root@[2001:db8::1]/var/lib/forkmesh/repositories/forkmesh-forkmesh.git" {
		t.Fatal(managedPushURL("root", "2001:DB8::1"))
	}
	command := sshCommand{Program: "sshpass", Args: []string{"-e", "ssh", "it's"}}
	if hostSSHCommandLine(command) != `'sshpass' '-e' 'ssh' 'it'\''s'` {
		t.Fatal(hostSSHCommandLine(command))
	}
}

func f64(c string) string { return strings.Repeat(c, 64) }

func cppFunctionLiteral(t *testing.T, source, signature string) string {
	t.Helper()
	start := strings.Index(source, signature)
	if start < 0 {
		t.Fatalf("%s not found", signature)
	}
	body := source[start:]
	open := strings.Index(body, "return QStringLiteral(")
	if open < 0 {
		t.Fatalf("no literal in %s", signature)
	}
	rest := body[open+len("return QStringLiteral("):]
	var out strings.Builder
	for i := 0; i < len(rest); {
		c := rest[i]
		switch {
		case c == ' ' || c == '\n' || c == '\t' || c == '\r':
			i++
		case c == '"':
			i++
			for i < len(rest) && rest[i] != '"' {
				if rest[i] == '\\' && i+1 < len(rest) {
					switch rest[i+1] {
					case 'n':
						out.WriteByte('\n')
					default:
						out.WriteByte(rest[i+1])
					}
					i += 2
					continue
				}
				out.WriteByte(rest[i])
				i++
			}
			i++
		default:
			return out.String()
		}
	}
	return out.String()
}

func TestManagedArtifactCommandsMatchTheDesktop(t *testing.T) {
	raw, err := os.ReadFile(filepath.Join("..", "..", "..", "desktop", "src", "ControlNode.cpp"))
	if err != nil {
		t.Skip("desktop sources not present")
	}
	source := string(raw)
	if got := cppFunctionLiteral(t, source, "QString managedReleaseArtifactSeedCommand("); got != managedArtifactSeedTemplate {
		t.Fatalf("seed command differs:\n%s\n%s", got, managedArtifactSeedTemplate)
	}
	if got := cppFunctionLiteral(t, source, "QString managedReleaseArtifactInventoryCommand("); got != managedArtifactInventoryTemplate {
		t.Fatalf("inventory command differs:\n%s\n%s", got, managedArtifactInventoryTemplate)
	}
}
