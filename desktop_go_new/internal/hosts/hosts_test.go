package hosts

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
	"time"

	"forkmesh.com/desktop-core/internal/coretest"
	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/rpc"
)

const fakeSSH = `#!/bin/sh
printf '%s\n' "$0 $*" >> "$FAKE_SSH_LOG"
case "$FAKE_SSH_MODE" in
  denied) echo "root@host: Permission denied (publickey)." >&2; exit 255 ;;
  timeout) echo "ssh: connect to host 203.0.113.9 port 22: Connection timed out" >&2; exit 255 ;;
  slow) sleep 1 ;;
esac
case "$*" in
  *FORKMESH=*) echo "FORKMESH=0 MIRROR=1 MIRRORSVC=1 CLAUDE=1 CODEX=0" ;;
  *"du -x -k"*) printf '400\t/var/lib\n100\t/var/lib/forkmesh\n300\t/var/lib/docker\n12\t/elsewhere\n' ;;
  *"systemctl restart"*) echo "restart: done"; echo "Authorization: Bearer abcdefghijklmnop" ;;
  *"systemctl stop"*) echo "stop: done" ;;
esac
exit 0
`

type fixture struct {
	core  *daemon.Core
	mod   *Module
	ui    *coretest.Peer
	phone *coretest.Peer
	log   string
	now   time.Time
}

func quoteSetting(value string) string {
	value = strings.ReplaceAll(value, `\`, `\\`)
	value = strings.ReplaceAll(value, `"`, `\"`)
	return `"` + value + `"`
}

func writeSettings(t *testing.T, core *daemon.Core, hosts []map[string]any, variables map[string]string) {
	t.Helper()
	encoded, _ := json.Marshal(hosts)
	text := "[account]\nnodeName=alice\n\n[hosts]\nlist=" + quoteSetting(string(encoded)) + "\n"
	if variables != nil {
		vars, _ := json.Marshal(variables)
		text += "\n[actions]\nvariables=" + quoteSetting("@ByteArray("+string(vars)+")") + "\n"
	}
	text += "\n[mirrors]\nsshGatewayHealth=gw1.example\t1000\t0\t0, gw2.example\t5000\t0\t1\n"
	path := core.Layout.SettingsFile()
	if err := os.MkdirAll(filepath.Dir(path), 0o700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, []byte(text), 0o600); err != nil {
		t.Fatal(err)
	}
}

func installFakeSSH(t *testing.T) string {
	t.Helper()
	if runtime.GOOS == "windows" {
		t.Skip("fake ssh is a shell script")
	}
	bin := t.TempDir()
	for _, name := range []string{"ssh", "sshpass"} {
		if err := os.WriteFile(filepath.Join(bin, name), []byte(fakeSSH), 0o755); err != nil {
			t.Fatal(err)
		}
	}
	log := filepath.Join(t.TempDir(), "ssh.log")
	t.Setenv("PATH", bin+string(os.PathListSeparator)+os.Getenv("PATH"))
	t.Setenv("FAKE_SSH_LOG", log)
	t.Setenv("FAKE_SSH_MODE", "")
	return log
}

func defaultHosts() []map[string]any {
	return []map[string]any{
		{"name": "mirror1", "ip": "203.0.113.5", "user": "root", "provider": "Vultr", "region": "ewr", "planType": "vc2-1c-1gb", "monthlyCost": 5, "status": "installed", "password": "hunter22"},
		{"name": "box", "ip": "198.51.100.7", "user": "deploy", "status": "added"},
	}
}

func newFixture(t *testing.T) *fixture {
	t.Helper()
	log := installFakeSSH(t)
	core := coretest.New(t)
	writeSettings(t, core, defaultHosts(), nil)
	f := &fixture{core: core, log: log, now: time.UnixMilli(1_700_000_000_000)}
	f.mod = New()
	f.mod.Owners = func() []string { return []string{"alice"} }
	coretest.Start(t, core, f.mod)
	f.mod.lastProbeAll = time.Now().UnixMilli()
	f.ui = coretest.NewPeer("ui", rpc.UI)
	f.phone = coretest.NewPeer("phone", rpc.Relay)
	f.phone.Dev.User = "alice"
	core.Hub.Add(f.ui)
	core.Hub.Add(f.phone)
	return f
}

func (f *fixture) sshCalls(t *testing.T) []string {
	data, _ := os.ReadFile(f.log)
	return strings.Split(strings.TrimSpace(string(data)), "\n")
}

func decode(t *testing.T, raw json.RawMessage) map[string]any {
	t.Helper()
	out := map[string]any{}
	if err := json.Unmarshal(raw, &out); err != nil {
		t.Fatalf("decode %s: %v", raw, err)
	}
	return out
}

func call(t *testing.T, f *fixture, peer rpc.Peer, method string, params any) (map[string]any, error) {
	t.Helper()
	raw, err := coretest.Call(t, f.core, peer, method, params)
	if err != nil {
		return nil, err
	}
	return decode(t, raw), nil
}

func TestBuildHostSSHCommandParity(t *testing.T) {
	dir := filepath.Join(t.TempDir(), "ssh")
	command, err := buildHostSSHCommand(dir, " 203.0.113.5 ", "root", "", "uptime", "")
	if err != nil {
		t.Fatal(err)
	}
	known := filepath.Join(dir, "known_hosts")
	want := []string{"-o", "StrictHostKeyChecking=accept-new", "-o", "UserKnownHostsFile=" + known, "-o", "HashKnownHosts=yes",
		"-o", "UpdateHostKeys=yes", "-o", "ConnectTimeout=30", "-o", "BatchMode=yes", "-o", "PreferredAuthentications=publickey",
		"root@203.0.113.5", "uptime"}
	if command.Program != "ssh" || strings.Join(command.Args, "|") != strings.Join(want, "|") {
		t.Fatalf("got %s %q", command.Program, command.Args)
	}
	if info, err := os.Stat(known); err != nil || info.Mode().Perm() != 0o600 {
		t.Fatalf("known_hosts: %v %v", info, err)
	}
	if info, _ := os.Stat(dir); info.Mode().Perm() != 0o700 {
		t.Fatalf("ssh dir mode %v", info.Mode())
	}
	key := filepath.Join(dir, sharedKeyName)
	_ = os.WriteFile(key, []byte("k"), 0o600)
	t.Setenv("SSHPASS", "leak")
	command, err = buildHostSSHCommand(dir, "host.example", "deploy", "pw", "uptime", key)
	if err != nil {
		t.Fatal(err)
	}
	if command.Program != "sshpass" || command.Args[0] != "-e" || command.Args[1] != "ssh" {
		t.Fatalf("sshpass args %q", command.Args)
	}
	joined := strings.Join(command.Args, " ")
	if !strings.Contains(joined, "-i "+key+" -o IdentitiesOnly=yes") || !strings.Contains(joined, "PreferredAuthentications=publickey,password") || strings.Contains(joined, "BatchMode") {
		t.Fatalf("password args %q", command.Args)
	}
	passes := 0
	for _, entry := range command.Env {
		if strings.HasPrefix(entry, "SSHPASS=") {
			passes++
			if entry != "SSHPASS=pw" {
				t.Fatalf("inherited %s", entry)
			}
		}
	}
	if passes != 1 {
		t.Fatalf("SSHPASS entries %d", passes)
	}
	for _, bad := range [][2]string{{"a..b", "root"}, {"-oProxyCommand=x", "root"}, {"host", "1root"}, {"", "root"}} {
		if _, err := buildHostSSHCommand(dir, bad[0], bad[1], "", "uptime", ""); err == nil || err.Error() != "The saved SSH host or username is invalid." {
			t.Fatalf("%v accepted: %v", bad, err)
		}
	}
	if _, err := buildHostSSHCommand(dir, "host", "root", "", "  ", ""); err == nil || err.Error() != "The remote SSH command is invalid." {
		t.Fatalf("empty command: %v", err)
	}
	if _, err := buildHostSSHCommand(dir, "host", "root", "", "uptime", filepath.Join(dir, "missing")); err == nil || err.Error() != "The managed SSH key for this host is missing." {
		t.Fatalf("missing key: %v", err)
	}
}

func TestSharedKeyResolution(t *testing.T) {
	dir := t.TempDir()
	if got := existingSharedHostIdentityFile(dir); got != "" {
		t.Fatalf("no key yet: %s", got)
	}
	legacy := filepath.Join(dir, legacySharedKeyName)
	_ = os.WriteFile(legacy, []byte("k"), 0o600)
	if got := existingSharedHostIdentityFile(dir); got != legacy {
		t.Fatalf("legacy: %s", got)
	}
	shared := filepath.Join(dir, sharedKeyName)
	_ = os.WriteFile(shared, []byte("k"), 0o600)
	if got := existingSharedHostIdentityFile(dir); got != shared {
		t.Fatalf("shared: %s", got)
	}
	hosts := []map[string]any{{"name": "a", "ip": "1.2.3.4", "user": "root", "identityFile": " /own/key "}}
	if got := savedHostIdentityFile(hosts, dir, "a", "1.2.3.4", "root"); got != "/own/key" {
		t.Fatalf("own key: %s", got)
	}
	if got := savedHostIdentityFile(hosts, dir, "b", "1.2.3.4", "root"); got != shared {
		t.Fatalf("fallback: %s", got)
	}
}

func TestSavedHostsScrubAndCredentialKey(t *testing.T) {
	hosts, migrated := parseSavedHosts(`[{"name":"m <1>","ip":" 1.2.3.4 ","user":"root","sshPassword":"pw","pass":"other"},7]`)
	if len(hosts) != 2 || hosts[0]["sshPassword"] != nil || hosts[0]["pass"] != nil || len(hosts[1]) != 0 {
		t.Fatalf("hosts %v", hosts)
	}
	key := credentialKey("m <1>", " 1.2.3.4 ", "root")
	if key != `["m <1>","1.2.3.4","root"]` {
		t.Fatalf("key %s", key)
	}
	if migrated[key] != "other" && migrated[key] != "pw" {
		t.Fatalf("migrated %v", migrated)
	}
}

func TestOwnerGateAndForbidden(t *testing.T) {
	f := newFixture(t)
	stranger := coretest.NewPeer("stranger", rpc.LAN)
	stranger.Dev.User = "mallory"
	for _, method := range []string{"hosts.list", "hosts.prepare", "fleet.summary", "fleet.disk"} {
		if _, err := call(t, f, stranger, method, map[string]any{}); rpc.Code(err) != "not_owner" {
			t.Fatalf("%s: %v", method, err)
		}
	}
	keyless := coretest.NewPeer("keyless", rpc.LAN)
	keyless.Dev.Key = ""
	keyless.Dev.User = "alice"
	if _, err := call(t, f, keyless, "hosts.list", nil); rpc.Code(err) != "not_owner" {
		t.Fatalf("keyless: %v", err)
	}
	for _, method := range []string{"core.hosts.state", "core.hosts.probe", "core.hosts.exec", "core.hosts.lines", "core.hosts.context", "core.hosts.operations", "core.hosts.probeAll"} {
		if _, err := call(t, f, f.phone, method, map[string]any{}); rpc.Code(err) != "forbidden" {
			t.Fatalf("%s: %v", method, err)
		}
	}
}

func TestHostsListAndDetail(t *testing.T) {
	f := newFixture(t)
	push := map[string]any{
		"desktop":       "desk",
		"roster":        []map[string]any{{"name": "MIRROR1", "version": "1.2.3", "online": true}},
		"busy":          true,
		"installStatus": strings.Repeat("x", 700),
		"probes":        map[string]string{credentialKey("box", "198.51.100.7", "deploy"): "● Offline · unreachable"},
	}
	if _, err := call(t, f, f.ui, "core.hosts.context", push); err != nil {
		t.Fatal(err)
	}
	list, err := call(t, f, f.phone, "hosts.list", nil)
	if err != nil {
		t.Fatal(err)
	}
	rows := list["hosts"].([]any)
	if len(rows) != 2 || list["desktop"] != "desk" || list["busy"] != true || list["canCreate"] != false {
		t.Fatalf("list %v", list)
	}
	first := rows[0].(map[string]any)
	if first["health"] != "online" || first["version"] != "1.2.3" || first["plan"] != "vc2-1c-1gb" || first["monthlyCost"] != 5.0 {
		t.Fatalf("first %v", first)
	}
	actions := first["actions"].([]any)
	if len(actions) != 5 || actions[4] != "destroy" {
		t.Fatalf("actions %v", actions)
	}
	second := rows[1].(map[string]any)
	if second["health"] != "offline" || len(second["actions"].([]any)) != 1 {
		t.Fatalf("second %v", second)
	}
	if list["online"] != 1.0 || list["attention"] != 1.0 {
		t.Fatalf("counts %v %v", list["online"], list["attention"])
	}
	if _, err := call(t, f, f.phone, "hosts.detail", map[string]any{"host": "nope"}); rpc.Code(err) != "unknown_host" {
		t.Fatalf("unknown: %v", err)
	}
	f.mod.pushLine("Mirror1", "journal?", "hello")
	detail, err := call(t, f, f.phone, "hosts.detail", map[string]any{"host": "mirror1"})
	if err != nil {
		t.Fatal(err)
	}
	if len(detail["lastResult"].(string)) != 600 || detail["recentLog"].([]any)[0] != "[host] hello" {
		t.Fatalf("detail %v", detail)
	}
	if f.mod.password(credentialKey("mirror1", "203.0.113.5", "root")) != "hunter22" {
		t.Fatalf("migrated password not kept in memory")
	}
}

func TestProbeThroughFakeSSH(t *testing.T) {
	f := newFixture(t)
	out, err := call(t, f, f.ui, "core.hosts.probe", map[string]any{"name": "mirror1", "ip": "203.0.113.5", "user": "root", "status": "installed"})
	if err != nil {
		t.Fatal(err)
	}
	if out["status"] != "● Online · Mirror node" || out["claude"] != "✓ Installed" || out["codex"] != "Not installed" || out["color"] != colorGreen {
		t.Fatalf("probe %v", out)
	}
	coretest.MustCall(t, f.core, f.ui, "core.hosts.state", nil, nil)
	f.ui.Next(t, "core.hosts.probed", time.Second)
	calls := f.sshCalls(t)
	if !strings.Contains(calls[len(calls)-1], "/sshpass -e ssh ") {
		t.Fatalf("migrated password should use sshpass: %s", calls[len(calls)-1])
	}
	list, _ := call(t, f, f.phone, "hosts.list", nil)
	if list["hosts"].([]any)[0].(map[string]any)["healthLabel"] != "● Online · Mirror node" {
		t.Fatalf("probe label not used: %v", list)
	}
	t.Setenv("FAKE_SSH_MODE", "denied")
	out, _ = call(t, f, f.ui, "core.hosts.probe", map[string]any{"name": "box", "ip": "198.51.100.7", "user": "deploy", "status": "added"})
	if out["status"] != "● Attention · SSH key rejected" || out["color"] != colorRed {
		t.Fatalf("denied %v", out)
	}
	t.Setenv("FAKE_SSH_MODE", "timeout")
	out, _ = call(t, f, f.ui, "core.hosts.probe", map[string]any{"name": "box", "ip": "198.51.100.7", "user": "deploy", "status": "added"})
	if out["status"] != "◌ Provisioning · waiting for SSH" {
		t.Fatalf("timeout %v", out)
	}
	out, _ = call(t, f, f.ui, "core.hosts.probe", map[string]any{"name": "box", "ip": "bad..host", "user": "deploy"})
	if out["status"] != "● Attention" || out["tooltip"] != "The saved SSH host or username is invalid." {
		t.Fatalf("invalid %v", out)
	}
}

func TestServiceActionFlow(t *testing.T) {
	f := newFixture(t)
	if _, err := call(t, f, f.phone, "hosts.prepare", map[string]any{"action": "reboot", "host": "mirror1"}); rpc.Code(err) != "unsupported_action" {
		t.Fatalf("reboot: %v", err)
	}
	if _, err := call(t, f, f.phone, "hosts.prepare", map[string]any{"action": "restart", "host": "ghost"}); rpc.Code(err) != "unknown_host" {
		t.Fatalf("ghost: %v", err)
	}
	if _, err := call(t, f, f.phone, "hosts.prepare", map[string]any{"action": "install", "host": "fmnext-core-1"}); rpc.Code(err) != "protected_node" {
		t.Fatalf("fmnext: %v", err)
	}
	for _, action := range []string{"update", "install", "destroy"} {
		prepared, err := call(t, f, f.phone, "hosts.prepare", map[string]any{"action": action, "host": "mirror1"})
		if err != nil || len(jsonString(prepared["token"])) != 48 {
			t.Fatalf("%s: %v %v", action, prepared, err)
		}
		if action == "destroy" && prepared["typeToConfirm"] != "mirror1" {
			t.Fatalf("destroy confirm %v", prepared)
		}
	}
	if _, err := call(t, f, f.phone, "hosts.prepare", map[string]any{"action": "install", "host": "bad name"}); rpc.Code(err) != "bad_host" {
		t.Fatalf("bad host: %v", err)
	}
	prepared, err := call(t, f, f.phone, "hosts.prepare", map[string]any{"action": "restart", "host": "mirror1"})
	if err != nil {
		t.Fatal(err)
	}
	if prepared["expiresInMs"] != 60000.0 || !strings.Contains(prepared["summary"].(string), "Restart the mirror service on \"mirror1\"") || len(prepared["token"].(string)) != 48 {
		t.Fatalf("prepared %v", prepared)
	}
	if _, err := call(t, f, f.phone, "hosts.run", map[string]any{"action": "restart", "host": "mirror1", "token": "nope"}); rpc.Code(err) != "confirmation_required" {
		t.Fatalf("missing: %v", err)
	}
	if _, err := call(t, f, f.phone, "hosts.run", map[string]any{"action": "stop", "host": "mirror1", "token": prepared["token"]}); rpc.Code(err) != "confirmation_mismatch" {
		t.Fatalf("mismatch: %v", err)
	}
	prepared, _ = call(t, f, f.phone, "hosts.prepare", map[string]any{"action": "restart", "host": "mirror1"})
	started, err := call(t, f, f.phone, "hosts.run", map[string]any{"action": "restart", "host": "MIRROR1", "token": prepared["token"]})
	if err != nil {
		t.Fatal(err)
	}
	if started["started"] != true || started["message"] != "Asking the host's service manager." {
		t.Fatalf("started %v", started)
	}
	event := decode(t, f.phone.Next(t, "hosts.action", 5*time.Second))
	if event["ok"] != true || event["action"] != "restart" || !strings.Contains(event["message"].(string), "restart: done") || strings.Contains(event["message"].(string), "abcdefghijklmnop") {
		t.Fatalf("event %v", event)
	}
	f.phone.Next(t, "hosts.changed", time.Second)
	calls := f.sshCalls(t)
	last := calls[len(calls)-1]
	if !strings.Contains(last, "root@203.0.113.5") || !strings.Contains(last, "systemctl restart forkmesh-mirror-node") || strings.Contains(last, "sshpass") {
		t.Fatalf("ssh call %s", last)
	}
	ops := decode(t, mustRaw(t, f, f.ui, "core.hosts.operations", nil))
	list := ops["operations"].([]any)
	if len(list) != 1 || list[0].(map[string]any)["running"] != false || list[0].(map[string]any)["ok"] != true {
		t.Fatalf("operations %v", ops)
	}
	f.mod.Now = func() time.Time { return f.now }
	prepared, _ = call(t, f, f.phone, "hosts.prepare", map[string]any{"action": "stop", "host": "mirror1"})
	f.now = f.now.Add(2 * time.Minute)
	if _, err := call(t, f, f.phone, "hosts.run", map[string]any{"action": "stop", "host": "mirror1", "token": prepared["token"]}); rpc.Code(err) != "confirmation_expired" {
		t.Fatalf("expired: %v", err)
	}
}

func TestServiceActionRefusals(t *testing.T) {
	f := newFixture(t)
	prepared, _ := call(t, f, f.phone, "hosts.prepare", map[string]any{"action": "restart", "host": "box"})
	if _, err := call(t, f, f.phone, "hosts.run", map[string]any{"action": "restart", "host": "box", "token": prepared["token"]}); rpc.Code(err) != "not_a_mirror_node" {
		t.Fatalf("plain host: %v", err)
	}
	t.Setenv("FAKE_SSH_MODE", "slow")
	first, _ := call(t, f, f.phone, "hosts.prepare", map[string]any{"action": "stop", "host": "mirror1"})
	second, _ := call(t, f, f.phone, "hosts.prepare", map[string]any{"action": "stop", "host": "mirror1"})
	if _, err := call(t, f, f.phone, "hosts.run", map[string]any{"action": "stop", "host": "mirror1", "token": first["token"]}); err != nil {
		t.Fatal(err)
	}
	if !f.mod.Busy() {
		t.Fatalf("a running action keeps the core busy")
	}
	if _, err := call(t, f, f.phone, "hosts.run", map[string]any{"action": "stop", "host": "mirror1", "token": second["token"]}); rpc.Code(err) != "already_running" {
		t.Fatalf("second: %v", err)
	}
	f.phone.Next(t, "hosts.action", 5*time.Second)
}

func mustRaw(t *testing.T, f *fixture, peer rpc.Peer, method string, params any) json.RawMessage {
	t.Helper()
	raw, err := coretest.Call(t, f.core, peer, method, params)
	if err != nil {
		t.Fatalf("%s: %v", method, err)
	}
	return raw
}

func TestLogStreamWindowAndAcks(t *testing.T) {
	f := newFixture(t)
	f.mod.pushLine("mirror1", "node", "before")
	if _, err := call(t, f, f.phone, "hosts.logs.start", map[string]any{"host": "ghost"}); rpc.Code(err) != "unknown_host" {
		t.Fatalf("ghost: %v", err)
	}
	started, err := call(t, f, f.phone, "hosts.logs.start", map[string]any{"host": "mirror1"})
	if err != nil {
		t.Fatal(err)
	}
	stream := started["stream"].(string)
	if started["window"] != 4.0 || started["maxBuffered"] != 2000.0 || started["backlog"].([]any)[0] != "[node] before" || len(stream) != 32 {
		t.Fatalf("start %v", started)
	}
	for i := 0; i < 5; i++ {
		lines := []map[string]any{}
		for j := 0; j < 200; j++ {
			lines = append(lines, map[string]any{"node": "mirror1", "src": "mirror", "text": "line"})
		}
		coretest.MustCall(t, f.core, f.ui, "core.hosts.lines", map[string]any{"lines": lines}, nil)
		f.mod.flushStreams()
	}
	if got := len(f.phone.Events); got < 4 {
		t.Fatalf("events %d", got)
	}
	seqs := []float64{}
	for len(f.phone.Events) > 0 {
		event := <-f.phone.Events
		if event.Name == "hosts.log" {
			batch := decode(t, event.Data)
			if batch["stream"] != stream || batch["host"] != "mirror1" || len(batch["lines"].([]any)) != 200 {
				t.Fatalf("batch %v", batch)
			}
			seqs = append(seqs, batch["seq"].(float64))
		}
	}
	if len(seqs) != 4 {
		t.Fatalf("window not honoured: %v", seqs)
	}
	if _, err := call(t, f, f.phone, "hosts.logs.ack", map[string]any{"stream": stream, "seq": 2}); err != nil {
		t.Fatal(err)
	}
	batch := decode(t, f.phone.Next(t, "hosts.log", 2*time.Second))
	if batch["seq"] != 5.0 {
		t.Fatalf("after ack %v", batch)
	}
	other := coretest.NewPeer("other", rpc.LAN)
	other.Dev.User = "alice"
	if _, err := call(t, f, other, "hosts.logs.ack", map[string]any{"stream": stream, "seq": 3}); rpc.Code(err) != "unknown_stream" {
		t.Fatalf("other peer: %v", err)
	}
	for i := 0; i < 3; i++ {
		if _, err := call(t, f, f.phone, "hosts.logs.start", map[string]any{"host": "box"}); err != nil {
			t.Fatal(err)
		}
	}
	if _, err := call(t, f, f.phone, "hosts.logs.start", map[string]any{"host": "box"}); rpc.Code(err) != "too_many_streams" {
		t.Fatalf("fifth: %v", err)
	}
	if _, err := call(t, f, f.phone, "hosts.logs.stop", map[string]any{"stream": stream}); err != nil {
		t.Fatal(err)
	}
	if _, err := call(t, f, f.phone, "hosts.logs.stop", map[string]any{"stream": stream}); rpc.Code(err) != "unknown_stream" {
		t.Fatalf("stopped twice: %v", err)
	}
	f.phone.Close()
	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		f.mod.mu.Lock()
		left := len(f.mod.streams)
		f.mod.mu.Unlock()
		if left == 0 {
			return
		}
		time.Sleep(10 * time.Millisecond)
	}
	t.Fatalf("streams survive a closed peer")
}

func TestLogStreamDropsOldest(t *testing.T) {
	s := newLogStream()
	for i := 0; i < logMaxBufferedLines+5; i++ {
		s.push(int64(i), "a-very-long-source-name", strings.Repeat("y", 2100))
	}
	batch := s.takeBatch()
	if batch["dropped"] != 5.0 || s.droppedTotal != 5 {
		t.Fatalf("dropped %v", batch["dropped"])
	}
	line := batch["lines"].([]map[string]any)[0]
	if line["src"] != "a-very-long-sour" || len(line["text"].(string)) != 2000 || line["t"] != 5.0 {
		t.Fatalf("line %v", line)
	}
	s.ack(10)
	if s.inFlight != 1 {
		t.Fatalf("future ack moved the window")
	}
	s.ack(1)
	if s.inFlight != 0 {
		t.Fatalf("ack did not open the window")
	}
}

func TestFleetDiskAndSummary(t *testing.T) {
	f := newFixture(t)
	if _, err := call(t, f, f.phone, "fleet.disk", map[string]any{"host": "ghost"}); rpc.Code(err) != "unknown_host" {
		t.Fatalf("ghost: %v", err)
	}
	if _, err := call(t, f, f.phone, "fleet.disk", map[string]any{"host": "mirror1", "path": "/var/../etc"}); rpc.Code(err) != "bad_path" {
		t.Fatalf("bad path: %v", err)
	}
	disk, err := call(t, f, f.phone, "fleet.disk", map[string]any{"host": "mirror1", "path": "/var/lib/"})
	if err != nil {
		t.Fatal(err)
	}
	entries := disk["entries"].([]any)
	if len(entries) != 3 || entries[0].(map[string]any)["name"] != "." || entries[1].(map[string]any)["name"] != "docker" || entries[1].(map[string]any)["bytes"] != 300.0*1024 {
		t.Fatalf("disk %v", disk)
	}
	t.Setenv("FAKE_SSH_MODE", "denied")
	if _, err := call(t, f, f.phone, "fleet.disk", map[string]any{"host": "mirror1"}); rpc.Code(err) != "host_unreachable" {
		t.Fatalf("unreachable: %v", err)
	}
	handler, _ := f.core.Registry.Resolve("fleet.summary")
	if _, err := handler(t.Context(), &rpc.Request{Peer: f.phone, Method: "fleet.summary"}); err != nil {
		t.Fatalf("summary without a desktop snapshot is native: %v", err)
	}
	if _, err := call(t, f, f.phone, "fleet.prepare", map[string]any{"action": "add", "host": "fmnext-run-1", "address": "1.2.3.4", "user": "root"}); rpc.Code(err) != "protected_node" {
		t.Fatalf("fmnext add: %v", err)
	}
	logDir := filepath.Join(f.core.Layout.Data, "deployments")
	_ = os.MkdirAll(logDir, 0o700)
	_ = os.WriteFile(filepath.Join(logDir, "vultr-mirror-latest.log"), []byte("one\r\ntoken=supersecretvalue\n\nthree\n"), 0o600)
	push := map[string]any{
		"provision":     map[string]any{"active": false, "node": "mirror2", "stage": 3},
		"mirrorClient":  map[string]any{"status": "<b>Running</b> &amp; healthy", "health": ""},
		"mirrorPush":    map[string]any{"pushing": 1, "pending": 2},
		"admin":         true,
		"installStatus": "<i>Done</i>",
		"fleetRunning":  []string{"agentClis:mirror1"},
		"repos":         []map[string]any{{"repo": "alice/app", "private": false, "published": true, "pushing": false}},
	}
	coretest.MustCall(t, f.core, f.ui, "core.hosts.context", push, nil)
	summary, err := call(t, f, f.phone, "fleet.summary", nil)
	if err != nil {
		t.Fatal(err)
	}
	costs := summary["costs"].(map[string]any)
	if costs["hosts"] != 2.0 || costs["vultr"] != 1.0 || costs["monthlyCost"] != 5.0 {
		t.Fatalf("costs %v", costs)
	}
	gateways := summary["gateways"].([]any)
	if len(gateways) != 2 || gateways[0].(map[string]any)["host"] != "gw2.example" || gateways[0].(map[string]any)["retired"] != true {
		t.Fatalf("gateways %v", gateways)
	}
	if summary["mirrorClient"].(map[string]any)["status"] != "Running & healthy" || summary["lastResult"] != "Done" || summary["admin"] != true {
		t.Fatalf("summary %v", summary)
	}
	provision, _ := call(t, f, f.phone, "fleet.provision", nil)
	logLines := provision["log"].([]any)
	if len(logLines) != 3 || logLines[0] != "one" || logLines[1] != "token=[redacted]" {
		t.Fatalf("provision %v", provision)
	}
	repos, _ := call(t, f, f.phone, "fleet.repos", nil)
	if len(repos["repos"].([]any)) != 1 {
		t.Fatalf("repos %v", repos)
	}
	run, _ := f.core.Registry.Resolve("fleet.run")
	if _, err := run(t.Context(), &rpc.Request{Peer: f.phone, Method: "fleet.run"}); rpc.Code(err) != "unsupported_action" {
		t.Fatalf("fleet.run without an action: %v", err)
	}
}

func TestHostOptionsFromStoredKey(t *testing.T) {
	f := newFixture(t)
	result, err := call(t, f, f.phone, "hosts.options", nil)
	if err != nil || result["vultr"] != false {
		t.Fatalf("no key: %v %v", result, err)
	}
	var auth string
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		auth = r.Header.Get("Authorization")
		if r.URL.Path != "/v2/plans" || r.URL.Query().Get("per_page") != "500" {
			w.WriteHeader(404)
			return
		}
		_, _ = w.Write([]byte(`{"plans":[{"id":"vc2-1c-1gb","monthly_cost":5,"ram":1024,"disk":25,"vcpu_count":1,"locations":["sea","ewr","fra"]},{"id":"vc2-1c-0.5gb-v6","monthly_cost":2.5,"ram":512,"locations":["ewr"]},{"id":"vc2-2c-2gb","monthly_cost":10,"ram":2048,"locations":["lax"]}]}`))
	}))
	defer server.Close()
	f.mod.VultrBase = server.URL
	writeSettings(t, f.core, append(defaultHosts(), map[string]any{"name": "mirror2"}), map[string]string{"VULTR_API_KEY": "ABCDEFGHIJKLMNOPQRSTUVWX"})
	coretest.MustCall(t, f.core, f.ui, "core.hosts.context", map[string]any{"linkedNodes": []string{"mirror3"}}, nil)
	result, err = call(t, f, f.phone, "hosts.options", nil)
	if err != nil {
		t.Fatal(err)
	}
	if auth != "Bearer ABCDEFGHIJKLMNOPQRSTUVWX" || result["vultr"] != true || result["suggestedName"] != "mirror4" || result["defaultPlan"] != "vc2-1c-1gb" || result["defaultRegion"] != "ewr" {
		t.Fatalf("options %v", result)
	}
	plans := result["plans"].([]any)
	if len(plans) != 2 || plans[0].(map[string]any)["regions"].([]any)[0] != "ewr" {
		t.Fatalf("plans %v", plans)
	}
	list, _ := call(t, f, f.phone, "hosts.list", nil)
	if list["canCreate"] != true {
		t.Fatalf("stored key should allow create")
	}
	coretest.MustCall(t, f.core, f.ui, "core.hosts.environment", map[string]any{"formVultrKey": "FORMKEYFORMKEYFORMKEY12"}, nil)
	if _, err := call(t, f, f.phone, "hosts.options", nil); err != nil || auth != "Bearer FORMKEYFORMKEYFORMKEY12" {
		t.Fatalf("the form key wins: %v %s", err, auth)
	}
	server.Close()
	coretest.MustCall(t, f.core, f.ui, "core.hosts.context", map[string]any{}, nil)
	if _, err := call(t, f, f.phone, "hosts.options", nil); rpc.Code(err) != "vultr_unreachable" {
		t.Fatalf("unreachable: %v", err)
	}
}

func TestExecRecordsOperation(t *testing.T) {
	f := newFixture(t)
	result, err := call(t, f, f.ui, "core.hosts.exec", map[string]any{"node": "mirror1", "command": "sh -lc 'systemctl stop forkmesh-mirror-node'", "timeoutMs": 5000})
	if err != nil {
		t.Fatal(err)
	}
	if result["ok"] != true || result["output"] != "stop: done" {
		t.Fatalf("exec %v", result)
	}
	result, _ = call(t, f, f.ui, "core.hosts.exec", map[string]any{"node": "ghost", "command": "true"})
	if result["ok"] != false || result["output"] != "" {
		t.Fatalf("ghost %v", result)
	}
	result, _ = call(t, f, f.ui, "core.hosts.exec", map[string]any{"node": "x", "ip": "198.51.100.9", "command": "uptime", "timeoutMs": 5000})
	calls := f.sshCalls(t)
	if result["ok"] != true || !strings.Contains(calls[len(calls)-1], "root@198.51.100.9 uptime") {
		t.Fatalf("provisioning address: %v %v", result, calls)
	}
	ops := decode(t, mustRaw(t, f, f.ui, "core.hosts.operations", nil))["operations"].([]any)
	if len(ops) != 3 {
		t.Fatalf("ops %v", ops)
	}
}

func TestPolicyHelpers(t *testing.T) {
	if got := plainText("<p>a&nbsp;&lt;b&gt;   c</p>", 400); got != "a <b> c" {
		t.Fatalf("plain %q", got)
	}
	if got := plainText(strings.Repeat("z", 10), 5); got != "zzzz…" {
		t.Fatalf("cut %q", got)
	}
	if got := redactSecrets(`password: "hunter22" sk-abcdefghijklmnop ghp_abcdefghijklmnopqrstuv`); got != `password: "[redacted]" [redacted] [redacted]` {
		t.Fatalf("redact %q", got)
	}
	if got := redactProcessOutput("\x1b[31mred\x1b[0m password=abc Bearer abcdefghij"); got != "red password=<redacted> Bearer <redacted>" {
		t.Fatalf("process redact %q", got)
	}
	if got := nextMirrorNodeName([]string{"mirror1", "Mirror-3", "box"}); got != "mirror4" {
		t.Fatalf("next %s", got)
	}
	if healthFromProbeLabel("◌ Checking…") != "unknown" || healthFromProbeLabel("weird") != "attention" || healthFromProbeLabel("● Online · ForkMesh missing") != "attention" {
		t.Fatalf("health")
	}
	if !validHostName("mirror-1.a") || validHostName("-x") || validHostName(strings.Repeat("a", 64)) {
		t.Fatalf("host names")
	}
	if validRemotePath("/a/./b") || !validRemotePath("/") || validRemotePath("rel") || validRemotePath("/a b") {
		t.Fatalf("remote paths")
	}
	rows := parseDiskUsage("10\t/\n4\t/var\n3\t/var/lib\n", "/", 60)
	if len(rows) != 2 || rows[1]["name"] != "var" {
		t.Fatalf("root disk %v", rows)
	}
}

func TestCoreProbesWhenNoDesktop(t *testing.T) {
	f := newFixture(t)
	f.mod.lastProbeAll = 0
	stale := map[string]string{credentialKey("mirror1", "203.0.113.5", "root"): "● Offline · unreachable"}
	coretest.MustCall(t, f.core, f.ui, "core.hosts.context", map[string]any{"probes": stale}, nil)
	if _, err := call(t, f, f.phone, "hosts.list", nil); err != nil {
		t.Fatal(err)
	}
	f.phone.Next(t, "hosts.changed", 5*time.Second)
	list, _ := call(t, f, f.phone, "hosts.list", nil)
	row := list["hosts"].([]any)[0].(map[string]any)
	if row["healthLabel"] != "● Online · Mirror node" || row["health"] != "online" {
		t.Fatalf("a newer core probe should win over the desktop's last label: %v", row)
	}
	if got := len(f.sshCalls(t)); got != 2 {
		t.Fatalf("one probe per saved host, then none within five minutes: %d", got)
	}
	coretest.MustCall(t, f.core, f.ui, "core.hosts.context", map[string]any{"probes": stale}, nil)
	list, _ = call(t, f, f.phone, "hosts.list", nil)
	if list["hosts"].([]any)[0].(map[string]any)["health"] != "offline" {
		t.Fatalf("a newer desktop label should win")
	}
}
