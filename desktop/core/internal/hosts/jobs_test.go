package hosts

import (
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"runtime"
	"sort"
	"strings"
	"sync"
	"testing"
	"time"

	"forkmesh.com/desktop-core/internal/coretest"
	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/identity"
	"forkmesh.com/desktop-core/internal/rpc"
)

const jobFakeSSH = `#!/bin/sh
printf '%s\n' "$*" >> "$FAKE_JOB_LOG"
cat > "$FAKE_JOB_STDIN.$$"
cat "$FAKE_JOB_STDIN.$$" >> "$FAKE_JOB_STDIN"
rm -f "$FAKE_JOB_STDIN.$$"
if [ -n "$FAKE_JOB_OUTPUT" ]; then printf '%s\n' "$FAKE_JOB_OUTPUT"; fi
exit "${FAKE_JOB_EXIT:-0}"
`

type fakeDesktop struct {
	mu       sync.Mutex
	settings map[string]string
	writes   []daemon.SettingWrite
	calls    []string
	env      hostEnv
	path     string
}

func renderSettings(values map[string]string) string {
	sections := map[string][]string{}
	for key, value := range values {
		section, name := "General", key
		if index := strings.LastIndex(key, "/"); index >= 0 {
			section, name = key[:index], key[index+1:]
		}
		sections[section] = append(sections[section], name+"="+quoteSetting(value))
	}
	names := make([]string, 0, len(sections))
	for name := range sections {
		names = append(names, name)
	}
	sort.Strings(names)
	var out strings.Builder
	for _, name := range names {
		lines := sections[name]
		sort.Strings(lines)
		out.WriteString("[" + name + "]\n" + strings.Join(lines, "\n") + "\n\n")
	}
	return out.String()
}

func (d *fakeDesktop) write(ctx context.Context, writes []daemon.SettingWrite) error {
	d.mu.Lock()
	defer d.mu.Unlock()
	for _, write := range writes {
		d.writes = append(d.writes, write)
		if write.Remove {
			delete(d.settings, write.Key)
			continue
		}
		d.settings[write.Key], _ = write.Value.(string)
	}
	time.Sleep(10 * time.Millisecond)
	return os.WriteFile(d.path, []byte(renderSettings(d.settings)), 0o600)
}

func (d *fakeDesktop) call(ctx context.Context, method string, params any) ([]byte, error) {
	d.mu.Lock()
	d.calls = append(d.calls, method)
	env := d.env
	d.mu.Unlock()
	switch method {
	case "ui.hosts.environment":
		return json.Marshal(env)
	case "ui.hosts.context":
		return []byte(`{"admin":true}`), nil
	}
	return []byte(`{}`), nil
}

func (d *fakeDesktop) savedHosts(t *testing.T) []map[string]any {
	d.mu.Lock()
	defer d.mu.Unlock()
	var hosts []map[string]any
	_ = json.Unmarshal([]byte(d.settings[hostsSetting]), &hosts)
	return hosts
}

type jobFixture struct {
	*fixture
	desktop  *fakeDesktop
	sshLog   string
	stdinLog string
}

func newJobFixture(t *testing.T) *jobFixture {
	t.Helper()
	if runtime.GOOS == "windows" {
		t.Skip("fake ssh is a shell script")
	}
	f := newFixture(t)
	bin := t.TempDir()
	for _, name := range []string{"ssh", "sshpass"} {
		if err := os.WriteFile(filepath.Join(bin, name), []byte(jobFakeSSH), 0o755); err != nil {
			t.Fatal(err)
		}
	}
	dir := t.TempDir()
	j := &jobFixture{fixture: f, sshLog: filepath.Join(dir, "ssh.log"), stdinLog: filepath.Join(dir, "stdin")}
	t.Setenv("PATH", bin+string(os.PathListSeparator)+os.Getenv("PATH"))
	t.Setenv("FAKE_JOB_LOG", j.sshLog)
	t.Setenv("FAKE_JOB_STDIN", j.stdinLog)
	t.Setenv("FAKE_JOB_OUTPUT", "")
	t.Setenv("FAKE_JOB_EXIT", "0")
	encoded, _ := json.Marshal(defaultHosts())
	j.desktop = &fakeDesktop{settings: map[string]string{hostsSetting: string(encoded), "account/nodeName": "alice"},
		path: f.core.Layout.SettingsFile(), env: hostEnv{InstallURL: "https://relay.example/install.sh",
			UninstallURL: "https://relay.example/uninstall.sh", Version: "1.2.3", CatalogURL: "https://relay.example/api/catalog",
			LinkUser: "alice"}}
	if err := os.WriteFile(j.desktop.path, []byte(renderSettings(j.desktop.settings)), 0o600); err != nil {
		t.Fatal(err)
	}
	f.mod.CallUI = j.desktop.call
	f.mod.WriteSettings = j.desktop.write
	f.mod.Attached = func() bool { return true }
	f.mod.Sleep = func(ctx context.Context, d time.Duration) bool { return ctx.Err() == nil }
	return j
}

func waitFor(t *testing.T, what string, cond func() bool) {
	t.Helper()
	deadline := time.Now().Add(10 * time.Second)
	for time.Now().Before(deadline) {
		if cond() {
			return
		}
		time.Sleep(10 * time.Millisecond)
	}
	t.Fatalf("timed out waiting for %s", what)
}

func (j *jobFixture) hostStatus(name string) string {
	for _, host := range j.desktop.savedHosts(nil) {
		if host["name"] == name {
			return jsonString(host["status"])
		}
	}
	return ""
}

func TestQtArgAndShq(t *testing.T) {
	if got := qtArg("a %1 b %10 c %2 %3 %%1", "x", "y"); got != "a x b %10 c y %3 %x" {
		t.Fatalf("qtArg %q", got)
	}
	if got := shq("it's"); got != `'it'\''s'` {
		t.Fatalf("shq %q", got)
	}
	if got := redactRunnerToken("token fmrt_0123456789abcdef end"); got != "token fmrt_[hidden] end" {
		t.Fatalf("redact %q", got)
	}
}

func TestBuildHostInstallCommandShapes(t *testing.T) {
	m := New()
	env := hostEnv{InstallURL: "https://relay.example/install.sh", Version: "1.2.3", LinkUser: "alice",
		BuildCommit: strings.Repeat("a", 40), ManifestDigestEnv: strings.Repeat("b", 64), KernelType: "darwin", CPUArch: "arm64"}
	cmd, upload, err := m.buildHostInstallCommand(env, "1.2.3.4", "root", "node1", installOptions{})
	if err != nil || upload != nil {
		t.Fatal(err)
	}
	if !strings.HasPrefix(cmd, "( curl -fsSL 'https://relay.example/install.sh' | FORKMESH_NODE_NAME='node1' FORKMESH_OWNER='alice' bash ); fm_update_status=$?;") {
		t.Fatalf("plain %q", cmd[:120])
	}
	cmd, _, err = m.buildHostInstallCommand(env, "1.2.3.4", "deploy", "node1", installOptions{RequirePublish: true})
	if err != nil || !strings.HasPrefix(cmd, "sudo -S -p '' -- bash -c ") || !strings.Contains(cmd, "FORKMESH_EXPECTED_RELEASE_MANIFEST_SHA256='\\''"+strings.Repeat("b", 64)) ||
		!strings.Contains(cmd, "FORKMESH_RESTART=1") {
		t.Fatalf("published %v %q", err, cmd)
	}
	env.BuildCommit = "dev"
	if _, _, err := m.buildHostInstallCommand(env, "1.2.3.4", "root", "n", installOptions{RequirePublish: true}); err == nil ||
		!strings.Contains(err.Error(), "no exact source revision") {
		t.Fatalf("commit check %v", err)
	}
	binary := filepath.Join(t.TempDir(), "forkmesh")
	_ = os.WriteFile(binary, []byte("binary-bytes"), 0o755)
	env.AppBinary = binary
	cmd, upload, err = m.buildHostInstallCommand(env, "1.2.3.4", "root", "n", installOptions{UploadBinary: true})
	if err != nil || string(upload) != "binary-bytes" || !strings.Contains(cmd, "FORKMESH_LOCAL_OS='macos'") || !strings.Contains(cmd, "FORKMESH_LOCAL_ARCH='arm64'") {
		t.Fatalf("upload %v %q", err, cmd)
	}
	release := publishedRelease{Tag: "v9.8.7", Version: "9.8.7", BuildCommit: strings.Repeat("a", 40), ManifestSha256: strings.Repeat("b", 64),
		LinuxX86_64Sha256: strings.Repeat("c", 64), ArtifactSha256s: []string{strings.Repeat("c", 64)}}
	cmd, upload, err = m.buildHostInstallCommand(env, "1.2.3.4", "root", "n", installOptions{RequirePublish: true, Release: release})
	if err != nil || upload != nil || !strings.Contains(cmd, "blob_base='https://relay.example/api/repo/forkmesh/forkmesh/releases/blob/sha256/'") ||
		!strings.Contains(cmd, "x86_64|amd64) expected='"+strings.Repeat("c", 64)+"'") {
		t.Fatalf("native release %v %q", err, cmd)
	}
	if _, _, err := m.buildHostInstallCommand(hostEnv{}, "1", "root", "n", installOptions{}); err == nil || err.Error() != "Could not resolve the installer URL." {
		t.Fatalf("no url %v", err)
	}
}

func TestVultrMirrorInstallPayload(t *testing.T) {
	m := New()
	dir := t.TempDir()
	env := hostEnv{CatalogURL: "https://relay.example/api/catalog?x=1", Version: "1.2.3", ProfilePublicKey: "viewer"}
	for _, name := range []string{"forkmesh-mirror-node", "mirror_gateway.py", "mirror_replica.py", "public_snapshots.py", "portable_sessions.py",
		"direct_https_bootstrap.py", "node_operator_policy.py", "secret_gate.py", "forkmesh-mirror-node.service"} {
		path := filepath.Join(dir, name)
		_ = os.WriteFile(path, []byte("data-"+name), 0o644)
		env.MirrorPackage = append(env.MirrorPackage, packageFile{Name: name, Path: path})
	}
	cmd, payload, err := m.buildHostInstallCommand(env, "203.0.113.17", "root", "mirror17",
		installOptions{Vultr: &vultrInstallTarget{DNSHostname: "mirror17.forkmesh.com"}})
	if err != nil {
		t.Fatal(err)
	}
	if !strings.HasPrefix(string(payload), vultrMirrorUploadMarker+"\n9\nforkmesh-mirror-node\n25\n") {
		t.Fatalf("payload %q", payload[:60])
	}
	for _, want := range []string{"[ \"$marker\" = '__FORKMESH_GO_MIRROR_V1__' ]", "--node 'mirror17'", "--accounts-url 'https://relay.example/api/accounts'",
		"--zone 'forkmesh.com'", "--address '203.0.113.17'", "viewer_key='viewer'", "'https://relay.example/api/mirrors/https'"} {
		if !strings.Contains(cmd, want) {
			t.Fatalf("missing %q", want)
		}
	}
	env.MirrorPackage = env.MirrorPackage[1:]
	if _, _, err := m.buildHostInstallCommand(env, "1", "root", "n", installOptions{Vultr: &vultrInstallTarget{DNSHostname: "n.forkmesh.com"}}); err == nil ||
		!strings.Contains(err.Error(), "forkmesh-mirror-node is missing") {
		t.Fatalf("incomplete %v", err)
	}
}

func TestInstallJobStreamsAndSavesHost(t *testing.T) {
	j := newJobFixture(t)
	t.Setenv("FAKE_JOB_OUTPUT", "installing\nFORKMESH LINK CODE: 123456")
	raw, err := coretest.Call(t, j.core, j.ui, "core.hosts.install", map[string]any{"node": "box", "ip": "198.51.100.7", "user": "deploy", "password": "pw"})
	if err != nil {
		t.Fatal(err)
	}
	result := decode(t, raw)
	if result["started"] != true {
		t.Fatalf("not started %v", result)
	}
	if !j.mod.Busy() {
		t.Fatal("install must keep the core busy")
	}
	waitFor(t, "install finished", func() bool { return !j.mod.jobRunning("install") })
	text := j.mod.transcriptText(installChannel)
	for _, want := range []string{"$ ssh deploy@198.51.100.7 sudo -S -p '' -- bash -c", "Connecting to 198.51.100.7 as deploy and running https://relay.example/install.sh ...",
		"installing", "[warning] Not auto-linking this host", "✔ Install finished. Node \"box\" will join the network"} {
		if !strings.Contains(text, want) {
			t.Fatalf("transcript missing %q:\n%s", want, text)
		}
	}
	if got := j.mod.status("install"); got != "✔ Installed on 198.51.100.7 as node \"box\"." {
		t.Fatalf("status %q", got)
	}
	stdin, _ := os.ReadFile(j.stdinLog)
	if string(stdin) != "pw\n" {
		t.Fatalf("stdin %q", stdin)
	}
	waitFor(t, "saved", func() bool { return j.hostStatus("box") == "installed" })
	for _, host := range j.desktop.savedHosts(t) {
		for _, secret := range secretHostFields {
			if _, ok := host[secret]; ok {
				t.Fatalf("password written: %v", host)
			}
		}
	}
	if j.mod.Busy() {
		t.Fatal("still busy")
	}
	if j.ui.Next(t, "core.hosts.linkPrompt", time.Second) == nil {
		t.Fatal("no link prompt")
	}
}

func TestInstallFailureHintAndBusy(t *testing.T) {
	j := newJobFixture(t)
	t.Setenv("FAKE_JOB_OUTPUT", "ssh: connect to host 10.0.0.5 port 22: Connection timed out")
	t.Setenv("FAKE_JOB_EXIT", "255")
	var finished struct {
		sync.Mutex
		ok   bool
		fail string
		done bool
	}
	job, why := j.mod.runHostInstall(context.Background(), installRequest{Node: "lan", IP: "10.0.0.5", User: "root"}, func(ok bool, failure string) {
		finished.Lock()
		finished.ok, finished.fail, finished.done = ok, failure, true
		finished.Unlock()
	})
	if job == nil {
		t.Fatal(why)
	}
	if _, why := j.mod.runHostInstall(context.Background(), installRequest{Node: "x", IP: "1.2.3.4", User: "root"}, nil); why != "An install is already running." {
		t.Fatalf("busy %q", why)
	}
	waitFor(t, "failure", func() bool { finished.Lock(); defer finished.Unlock(); return finished.done })
	if finished.ok || !strings.HasPrefix(finished.fail, "exit 255 — ssh: connect to host") {
		t.Fatalf("failure %+v", finished.fail)
	}
	text := j.mod.transcriptText(installChannel)
	if !strings.Contains(text, "✘ Install failed (exit 255).") || !strings.Contains(text, "10.0.0.5 is in the private range 10.0.0.0/8 (RFC 1918)") {
		t.Fatalf("transcript %s", text)
	}
	if j.mod.status("install") != "✘ Install failed — see the output above." {
		t.Fatalf("status %q", j.mod.status("install"))
	}
	waitFor(t, "saved failure", func() bool { return j.hostStatus("lan") == "install failed" })
}

func TestUninstallJob(t *testing.T) {
	j := newJobFixture(t)
	raw, err := coretest.Call(t, j.core, j.ui, "core.hosts.uninstall", map[string]any{"node": "mirror1", "ip": "203.0.113.5", "user": "root"})
	if err != nil || decode(t, raw)["started"] != true {
		t.Fatalf("%v %s", err, raw)
	}
	waitFor(t, "uninstalled", func() bool { return j.hostStatus("mirror1") == "uninstalled" })
	logged, _ := os.ReadFile(j.sshLog)
	if !strings.Contains(string(logged), "curl -fsSL 'https://relay.example/uninstall.sh' | FORKMESH_ASSUME_YES=1 FORKMESH_PURGE=1 bash") {
		t.Fatalf("command %s", logged)
	}
	if j.mod.status("install") != "✔ Uninstalled from 203.0.113.5." {
		t.Fatalf("status %q", j.mod.status("install"))
	}
}

func TestDeployAllSessions(t *testing.T) {
	j := newJobFixture(t)
	j.desktop.env.BuildCommit = strings.Repeat("a", 40)
	j.desktop.env.ManifestDigestEnv = strings.Repeat("b", 64)
	count, why := j.mod.runHostDeployAll(context.Background(), deployRequest{Mode: "install", Origin: "desktop"}, nil)
	if count != 2 {
		t.Fatalf("count %d %s", count, why)
	}
	waitFor(t, "deploy done", func() bool { return j.mod.deployRemaining() == 0 && !j.mod.jobRunning("deploy") })
	if got := j.mod.status("install"); got != "✔ Finished deploying to all 2 host(s)." {
		t.Fatalf("status %q", got)
	}
	snapshot := j.mod.deploySnapshot()
	sessions := snapshot["sessions"].([]any)
	first := sessions[0].(map[string]any)
	if !strings.HasPrefix(jsonString(first["header"]), "✔ ") {
		t.Fatalf("header %v", first)
	}
	channel := jsonString(first["channel"])
	if !strings.Contains(j.mod.transcriptText(channel), "Installing the published ForkMesh v1.2.3; the host will verify") {
		t.Fatalf("pane %q", j.mod.transcriptText(channel))
	}
	waitFor(t, "statuses", func() bool { return j.hostStatus("box") == "installed" && j.hostStatus("mirror1") == "installed" })
}

func TestSettingsQueueWaitsForDesktop(t *testing.T) {
	j := newJobFixture(t)
	attached := false
	var lock sync.Mutex
	j.mod.Attached = func() bool { lock.Lock(); defer lock.Unlock(); return attached }
	j.mod.rememberHost("queued", "192.0.2.1", "root", "secret", "added", "", nil)
	if j.mod.pendingSettingsCount() == 0 {
		t.Fatal("write should be queued")
	}
	if _, index := j.mod.savedHostByName("queued"); index < 0 {
		t.Fatal("overlay must show the queued host")
	}
	if j.mod.password(credentialKey("queued", "192.0.2.1", "root")) != "secret" {
		t.Fatal("password kept in memory")
	}
	lock.Lock()
	attached = true
	lock.Unlock()
	waitFor(t, "flush", func() bool { return j.mod.pendingSettingsCount() == 0 })
	if j.hostStatus("queued") != "added" {
		t.Fatal("not written")
	}
	if strings.Contains(j.desktop.settings[hostsSetting], "secret") {
		t.Fatal("password written")
	}
	if !j.mod.forgetHostNamed("QUEUED") {
		t.Fatal("forget")
	}
	waitFor(t, "ledger", func() bool {
		j.desktop.mu.Lock()
		defer j.desktop.mu.Unlock()
		return strings.Contains(j.desktop.settings[decommissionedNodesSetting], "queued")
	})
	j.mod.rememberHost("queued", "192.0.2.1", "root", "", "added", "", nil)
	waitFor(t, "ledger cleared", func() bool {
		j.desktop.mu.Lock()
		defer j.desktop.mu.Unlock()
		return !strings.Contains(j.desktop.settings[decommissionedNodesSetting], "queued")
	})
	if j.mod.rememberHost("fmnext-core-1", "192.0.2.9", "root", "", "added", "", nil) {
		t.Fatal("fleet instance stored")
	}
	j.desktop.env.FleetAddresses = []string{"192.0.2.10"}
	j.mod.environment(context.Background())
	if j.mod.rememberHost("other", "192.0.2.10", "root", "", "added", "", nil) {
		t.Fatal("fleet address stored")
	}
}

func TestMergeSavedHostRecordKeepsProviderFacts(t *testing.T) {
	previous := map[string]any{"name": "m", "provider": "Vultr", "instanceId": "abc", "identityFile": "/k", "statusChangedAt": 5.0}
	entry := mergeSavedHostRecord(previous, map[string]any{"name": "m", "status": "installed"}, map[string]any{"region": "ewr"})
	if entry["provider"] != "Vultr" || entry["instanceId"] != "abc" || entry["identityFile"] != "/k" || entry["region"] != "ewr" {
		t.Fatalf("merged %v", entry)
	}
	ledger := recordLedger(map[string]any{"old": 1.0}, " New ", 10_000)
	if _, ok := ledger["old"]; !ok || ledger["new"] != 10000.0 {
		t.Fatalf("ledger %v", ledger)
	}
	if pruned := recordLedger(map[string]any{"old": 1.0}, "x", decommissionedNodeTTLMs+10); len(pruned) != 1 {
		t.Fatalf("prune %v", pruned)
	}
}

func TestLinkCodeSignedPost(t *testing.T) {
	j := newJobFixture(t)
	seed := make([]byte, 32)
	id, _ := identity.FromSeed(seed)
	j.core.Identity = id
	var got map[string]any
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		_ = json.NewDecoder(r.Body).Decode(&got)
		_, _ = w.Write([]byte(`{"linked":true,"node":"box","user":"alice"}`))
	}))
	defer server.Close()
	j.desktop.env.AccountsLinkURL = server.URL + "/api/accounts/link-node"
	j.desktop.env.LinkSigner = "alice"
	ok, message := j.mod.submitHostLinkCode(context.Background(), "654321")
	if !ok || message != "✔ Node \"box\" is now linked to user \"alice\"." {
		t.Fatalf("%v %q", ok, message)
	}
	canonical := "forkmesh-link-v1\nalice\n654321\n" + jsonString(got["ts"])
	if !identity.Verify(id.PublicKey(), jsonString(got["sig"]), []byte(canonical)) || got["nodeName"] != "alice" {
		t.Fatalf("body %v", got)
	}
	if _, err := coretest.Call(t, j.core, j.ui, "core.hosts.linkCode", map[string]any{"code": "12"}); rpc.Code(err) != "bad_code" {
		t.Fatalf("bad code %v", err)
	}
}

func TestJobMethodsRefusePhones(t *testing.T) {
	j := newJobFixture(t)
	for _, method := range []string{"core.hosts.install", "core.hosts.uninstall", "core.hosts.deployAll", "core.hosts.linkCode", "core.hosts.jobs",
		"core.hosts.transcript", "core.hosts.cancel", "core.hosts.environment"} {
		if _, err := coretest.Call(t, j.core, j.phone, method, map[string]any{}); rpc.Code(err) != "forbidden" {
			t.Fatalf("%s: %v", method, err)
		}
	}
	j.mod.appendOutput(installChannel, "hello ")
	j.mod.appendOutput(installChannel, "world")
	raw, err := coretest.Call(t, j.core, j.ui, "core.hosts.transcript", map[string]any{"channel": installChannel, "from": 6})
	if err != nil || decode(t, raw)["text"] != "world" {
		t.Fatalf("%v %s", err, raw)
	}
}
