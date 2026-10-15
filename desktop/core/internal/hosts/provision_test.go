package hosts

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"net/url"
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

const pvFakeSSH = `#!/bin/sh
printf '%s\n' "$*" >> "$PV_LOG"
case "$*" in
  *"echo forkmesh-ssh-ready"*)
    if [ "$PV_SSH_MODE" = denied ]; then echo "root@host: Permission denied (publickey)." >&2; exit 255; fi
    echo forkmesh-ssh-ready; exit 0 ;;
  *FORKMESH_GO_MIRROR*)
    cat > "$PV_STDIN"
    echo "mirror installed"; exit 0 ;;
  *"systemctl is-active"*)
    echo "service: running"; exit 0 ;;
esac
exit 0
`

const pvFakeKeygen = `#!/bin/sh
printf 'ssh-keygen %s\n' "$*" >> "$PV_LOG"
while [ $# -gt 0 ]; do
  if [ "$1" = -f ]; then shift; printf 'PRIVATE\n' > "$1"; printf 'ssh-ed25519 AAAAPVFAKEKEY forkmesh-shared-host-key\n' > "$1.pub"; fi
  shift
done
exit 0
`

const pvFakeGit = `#!/bin/sh
printf 'git %s | %s\n' "$*" "$GIT_SSH_COMMAND" >> "$PV_LOG"
if [ "$1" = remote ] && [ "$2" = -v ] && [ -n "$PV_REMOTES" ]; then printf '%s\n' "$PV_REMOTES"; fi
exit 0
`

const pvRemotes = "gw-a\tssh://root@198.51.100.20/srv/forkmesh.git (fetch)\n" +
	"gw-a\tssh://root@198.51.100.20/srv/forkmesh.git (push)\n" +
	"vma\tssh://git@vm-a.example.net/forkmesh.git (push)\n" +
	"keep\tssh://root@mirror7.example.org/forkmesh.git (push)\n" +
	"origin\thttps://example.org/forkmesh.git (push)"

const (
	pvVultrKey     = "ABCDEFGHIJKLMNOPQRSTUVWX"
	pvInstance     = "1b2e4f6a-0000-4000-8000-00000000abcd"
	pvInstanceIP   = "203.0.113.50"
	pvDestroyA     = "aaaaaaaa-0000-4000-8000-000000000001"
	pvDestroyB     = "bbbbbbbb-0000-4000-8000-000000000002"
	pvDestroyC1    = "cccccccc-0000-4000-8000-000000000003"
	pvDestroyC2    = "cccccccc-0000-4000-8000-000000000004"
	pvTunnelToken  = "cf-token-123456"
	pvValidRefsSha = "1111111111111111111111111111111111111111111111111111111111111111"
)

type pvRecorder struct {
	mu     sync.Mutex
	events []coretest.Event
	done   chan struct{}
}

func (r *pvRecorder) ID() string               { return "pv-ui" }
func (r *pvRecorder) Transport() rpc.Transport { return rpc.UI }
func (r *pvRecorder) Device() rpc.Device       { return rpc.Device{} }
func (r *pvRecorder) Done() <-chan struct{}    { return r.done }
func (r *pvRecorder) Send(event string, data any) bool {
	raw, _ := json.Marshal(data)
	r.mu.Lock()
	r.events = append(r.events, coretest.Event{Name: event, Data: raw})
	r.mu.Unlock()
	return true
}

func (r *pvRecorder) named(name string) []map[string]any {
	r.mu.Lock()
	defer r.mu.Unlock()
	out := []map[string]any{}
	for _, event := range r.events {
		if event.Name == name {
			decoded := map[string]any{}
			_ = json.Unmarshal(event.Data, &decoded)
			out = append(out, decoded)
		}
	}
	return out
}

type pvStore struct {
	mu     sync.Mutex
	path   string
	values map[string]string
	stamp  time.Time
}

func pvQuote(value string) string {
	value = strings.ReplaceAll(value, `\`, `\\`)
	value = strings.ReplaceAll(value, `"`, `\"`)
	return `"` + value + `"`
}

func (s *pvStore) flushLocked(t *testing.T) {
	keys := make([]string, 0, len(s.values))
	for key := range s.values {
		keys = append(keys, key)
	}
	sort.Strings(keys)
	var text strings.Builder
	for _, key := range keys {
		text.WriteString(strings.ReplaceAll(key, "/", `\`) + "=" + pvQuote(s.values[key]) + "\n")
	}
	if err := os.MkdirAll(filepath.Dir(s.path), 0o700); err != nil {
		t.Error(err)
	}
	if err := os.WriteFile(s.path, []byte(text.String()), 0o600); err != nil {
		t.Error(err)
	}
	s.stamp = s.stamp.Add(time.Second)
	_ = os.Chtimes(s.path, s.stamp, s.stamp)
}

func (s *pvStore) get(key string) string {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.values[key]
}

func (s *pvStore) has(key string) bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	_, ok := s.values[key]
	return ok
}

type pvVultr struct {
	mu        sync.Mutex
	requests  []string
	created   map[string]any
	instances []map[string]any
	polls     int
	ready     map[string]any
	block     chan struct{}
	deleted   []string
}

func (v *pvVultr) handler(t *testing.T) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("Authorization") != "Bearer "+pvVultrKey {
			w.WriteHeader(401)
			_, _ = w.Write([]byte(`{"error":"bad key"}`))
			return
		}
		body, _ := io.ReadAll(r.Body)
		v.mu.Lock()
		v.requests = append(v.requests, r.Method+" "+r.URL.RequestURI())
		v.mu.Unlock()
		reply := func(value any) {
			raw, _ := json.Marshal(value)
			_, _ = w.Write(raw)
		}
		switch {
		case r.Method == http.MethodGet && r.URL.Path == "/v2/ssh-keys":
			reply(map[string]any{"ssh_keys": []any{map[string]any{"id": "other", "ssh_key": "ssh-ed25519 OTHER x"}}})
		case r.Method == http.MethodPost && r.URL.Path == "/v2/ssh-keys":
			reply(map[string]any{"ssh_key": map[string]any{"id": "key-1"}})
		case r.URL.Path == "/v2/plans":
			reply(map[string]any{"plans": []any{
				map[string]any{"id": "vc2-1c-1gb", "monthly_cost": 5, "ram": 1024, "disk": 25, "locations": []any{"ewr", "atl"}},
				map[string]any{"id": "vc2-1c-2gb", "monthly_cost": 10, "ram": 2048, "disk": 55, "locations": []any{"ewr", "atl"}},
			}})
		case r.URL.Path == "/v2/os":
			reply(map[string]any{"os": []any{
				map[string]any{"id": 477, "name": "Debian 11 x64 (bullseye)", "arch": "x64", "family": "debian"},
				map[string]any{"id": 2136, "name": "Debian 12 x64 (bookworm)", "arch": "x64", "family": "debian"},
				map[string]any{"id": 9, "name": "Ubuntu 24.04 x64", "arch": "x64", "family": "ubuntu"},
			}})
		case r.Method == http.MethodGet && r.URL.Path == "/v2/instances":
			v.mu.Lock()
			list := v.instances
			v.mu.Unlock()
			reply(map[string]any{"instances": list})
		case r.Method == http.MethodPost && r.URL.Path == "/v2/instances":
			created := map[string]any{}
			_ = json.Unmarshal(body, &created)
			v.mu.Lock()
			v.created = created
			v.mu.Unlock()
			reply(map[string]any{"instance": map[string]any{"id": pvInstance}})
		case r.Method == http.MethodGet && strings.HasPrefix(r.URL.Path, "/v2/instances/"):
			v.mu.Lock()
			v.polls++
			polls, ready, block := v.polls, v.ready, v.block
			v.mu.Unlock()
			if block != nil {
				select {
				case <-block:
				case <-r.Context().Done():
				}
			}
			if polls == 1 || ready == nil {
				reply(map[string]any{"instance": map[string]any{"status": "pending", "power_status": "stopped", "main_ip": "0.0.0.0"}})
				return
			}
			reply(map[string]any{"instance": ready})
		case r.Method == http.MethodDelete && strings.HasPrefix(r.URL.Path, "/v2/instances/"):
			v.mu.Lock()
			v.deleted = append(v.deleted, strings.TrimPrefix(r.URL.Path, "/v2/instances/"))
			v.mu.Unlock()
			w.WriteHeader(204)
		default:
			w.WriteHeader(404)
		}
	}
}

type pvRewrite struct {
	health string
	host   string
}

func (rw *pvRewrite) RoundTrip(r *http.Request) (*http.Response, error) {
	if r.URL.Hostname() == rw.host && rw.health != "" {
		target, _ := url.Parse(rw.health)
		clone := r.Clone(r.Context())
		clone.URL.Scheme = target.Scheme
		clone.URL.Host = target.Host
		clone.Host = target.Host
		return http.DefaultTransport.RoundTrip(clone)
	}
	return http.DefaultTransport.RoundTrip(r)
}

type pvFixture struct {
	t        *testing.T
	core     *daemon.Core
	mod      *Module
	ui       *pvRecorder
	store    *pvStore
	vultr    *pvVultr
	rewrite  *pvRewrite
	log      string
	stdin    string
	logFile  string
	keyPath  string
	mirror   string
	envMu    sync.Mutex
	env      map[string]any
	cfServer *httptest.Server
}

func (f *pvFixture) setEnv(key string, value any) {
	f.envMu.Lock()
	f.env[key] = value
	f.envMu.Unlock()
}

func pvInstall(t *testing.T) (string, string) {
	t.Helper()
	if runtime.GOOS == "windows" {
		t.Skip("fake tools are shell scripts")
	}
	bin := t.TempDir()
	for name, script := range map[string]string{"ssh": pvFakeSSH, "sshpass": pvFakeSSH, "ssh-keygen": pvFakeKeygen, "git": pvFakeGit} {
		if err := os.WriteFile(filepath.Join(bin, name), []byte(script), 0o755); err != nil {
			t.Fatal(err)
		}
	}
	dir := t.TempDir()
	log := filepath.Join(dir, "calls.log")
	stdin := filepath.Join(dir, "stdin.bin")
	t.Setenv("PATH", bin+string(os.PathListSeparator)+os.Getenv("PATH"))
	t.Setenv("PV_LOG", log)
	t.Setenv("PV_STDIN", stdin)
	t.Setenv("PV_SSH_MODE", "")
	t.Setenv("PV_REMOTES", pvRemotes)
	return log, stdin
}

func newPvFixture(t *testing.T, hosts []map[string]any, extra map[string]string, prepare func(f *pvFixture)) *pvFixture {
	t.Helper()
	log, stdin := pvInstall(t)
	core := coretest.New(t)
	f := &pvFixture{t: t, core: core, log: log, stdin: stdin, ui: &pvRecorder{done: make(chan struct{})}, vultr: &pvVultr{}}
	encoded, _ := json.Marshal(hosts)
	f.store = &pvStore{path: core.Layout.SettingsFile(), values: map[string]string{"hosts/list": string(encoded)}, stamp: time.Now()}
	for key, value := range extra {
		f.store.values[key] = value
	}
	f.store.mu.Lock()
	f.store.flushLocked(t)
	f.store.mu.Unlock()
	vultrServer := httptest.NewServer(f.vultr.handler(t))
	t.Cleanup(vultrServer.Close)
	work := t.TempDir()
	f.logFile = filepath.Join(work, "deployments", "provision.log")
	mirrorRoot := filepath.Join(work, "mirrors")
	if err := os.MkdirAll(filepath.Join(mirrorRoot, "forkmesh-forkmesh.git"), 0o700); err != nil {
		t.Fatal(err)
	}
	pkg := []map[string]any{}
	for _, name := range []string{"forkmesh-mirror-node", "mirror_gateway.py", "mirror_replica.py", "public_snapshots.py", "portable_sessions.py",
		"direct_https_bootstrap.py", "node_operator_policy.py", "secret_gate.py", "forkmesh-mirror-node.service"} {
		path := filepath.Join(work, name)
		_ = os.WriteFile(path, []byte("content of "+name), 0o600)
		pkg = append(pkg, map[string]any{"name": name, "path": path})
	}
	f.env = map[string]any{
		"installUrl":       "https://relay.example/install.sh",
		"version":          "1.2.3",
		"catalogUrl":       "https://relay.example/api/catalog",
		"profilePublicKey": "pk-profile",
		"mirrorRoot":       mirrorRoot,
		"mirrorPackage":    pkg,
		"provisionLog":     f.logFile,
		"kernelType":       "darwin",
		"cpuArch":          "arm64",
		"admin":            true,
		"accountOwner":     " Alice ",
	}
	f.mod = New()
	f.mod.Owners = func() []string { return []string{"alice"} }
	f.mod.VultrBase = vultrServer.URL
	f.rewrite = &pvRewrite{}
	f.mod.HTTPClient = &http.Client{Transport: f.rewrite}
	f.mod.SSHDir = filepath.Join(work, "ssh")
	f.mod.Sleep = func(ctx context.Context, d time.Duration) bool { return ctx.Err() == nil }
	f.mod.Attached = func() bool { return true }
	f.mod.Variables = func() map[string]string {
		return map[string]string{"VULTR_API_KEY": pvVultrKey, "CLOUDFLARE_API_TOKEN": pvTunnelToken, "CLOUDFLARE_ZONE": "example.net"}
	}
	f.mod.WriteSettings = func(ctx context.Context, writes []daemon.SettingWrite) error {
		f.store.mu.Lock()
		defer f.store.mu.Unlock()
		for _, write := range writes {
			if write.Remove {
				delete(f.store.values, write.Key)
				continue
			}
			f.store.values[write.Key] = fmt.Sprint(write.Value)
		}
		f.store.flushLocked(t)
		return nil
	}
	f.mod.CallUI = func(ctx context.Context, method string, params any) ([]byte, error) {
		switch method {
		case "ui.hosts.environment":
			f.envMu.Lock()
			defer f.envMu.Unlock()
			return json.Marshal(f.env)
		}
		return nil, rpc.Fail("unknown_method")
	}
	if prepare != nil {
		prepare(f)
	}
	core.Hub.Add(f.ui)
	coretest.Start(t, core, f.mod)
	checkout := filepath.Join(work, "checkout")
	_ = os.MkdirAll(checkout, 0o700)
	f.mod.mirror().setRepos([]mirrorRepo{{Owner: "forkmesh", Name: "forkmesh", LocalPath: checkout}})
	t.Cleanup(func() {
		deadline := time.Now().Add(5 * time.Second)
		for f.mod.Busy() && time.Now().Before(deadline) {
			time.Sleep(10 * time.Millisecond)
		}
		time.Sleep(50 * time.Millisecond)
	})
	return f
}

func (f *pvFixture) healthServer(validAfter int) {
	id, err := identity.FromSeed(make([]byte, 32))
	if err != nil {
		f.t.Fatal(err)
	}
	var mu sync.Mutex
	calls := 0
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		mu.Lock()
		calls++
		n := calls
		mu.Unlock()
		query := r.URL.Query()
		if r.URL.Path != "/health" || query.Get("owner") != "forkmesh" || query.Get("repo") != "forkmesh" || r.Header.Get("accept") != "application/json" {
			w.WriteHeader(400)
			return
		}
		nonce := query.Get("nonce")
		var issuedAt int64
		fmt.Sscan(query.Get("issuedAt"), &issuedAt)
		refs := pvValidRefsSha
		if n < validAfter {
			refs = emptyRefsSha256
		}
		node := strings.SplitN(r.Host, ".", 2)[0]
		if f.rewrite.host != "" {
			node = strings.SplitN(f.rewrite.host, ".", 2)[0]
		}
		proof := map[string]any{"owner": "forkmesh", "repository": "forkmesh", "available": true, "integrity": "ok", "refsSha256": refs, "operationsSha256": "ops"}
		message := flagshipHealthMessage(node, nonce, issuedAt, proof)
		digest := sha256.Sum256(message)
		raw, _ := json.Marshal(map[string]any{
			"node": node, "publicKey": id.PublicKey(), "repositoryProof": proof,
			"challenge": map[string]any{"messageType": "forkmesh-https-health-repository-v1", "nonce": nonce, "issuedAt": issuedAt,
				"messageSha256": hex.EncodeToString(digest[:]), "signature": id.Sign(message)},
		})
		_, _ = w.Write(raw)
	}))
	f.t.Cleanup(server.Close)
	f.rewrite.health = server.URL
}

func (f *pvFixture) waitState(state string) map[string]any {
	f.t.Helper()
	deadline := time.Now().Add(10 * time.Second)
	for time.Now().Before(deadline) {
		snapshot := f.mod.provisionSnapshot()
		if snapshot["state"] == state && snapshot["active"] == false {
			for f.mod.Busy() && time.Now().Before(deadline) {
				time.Sleep(5 * time.Millisecond)
			}
			return snapshot
		}
		time.Sleep(5 * time.Millisecond)
	}
	f.t.Fatalf("provisioning never reached %s: %v\n%s", state, f.mod.provisionSnapshot(), f.mod.transcriptText(installChannel))
	return nil
}

func (f *pvFixture) calls() string {
	data, _ := os.ReadFile(f.log)
	return string(data)
}

func (f *pvFixture) savedHost(name string) map[string]any {
	var hosts []map[string]any
	_ = json.Unmarshal([]byte(f.store.get("hosts/list")), &hosts)
	for _, host := range hosts {
		if jsonString(host["name"]) == name {
			return host
		}
	}
	return nil
}

func pvHosts() []map[string]any {
	return []map[string]any{
		{"name": "mirror1", "ip": "203.0.113.5", "user": "root", "status": "installed"},
		{"name": "box", "ip": "198.51.100.7", "user": "deploy", "status": "added"},
	}
}

func TestProvisionCreateToSucceeded(t *testing.T) {
	f := newPvFixture(t, pvHosts(), nil, nil)
	f.vultr.ready = map[string]any{"status": "active", "power_status": "running", "main_ip": pvInstanceIP}
	f.rewrite.host = "mirror2.example.net"
	f.healthServer(2)
	result := map[string]any{}
	coretest.MustCall(t, f.core, coretest.NewPeer("desk", rpc.UI), "core.hosts.provision.create",
		map[string]any{"node": "", "apiKey": "", "installAgentClis": false, "plan": "vc2-1c-2gb", "region": "atl"}, &result)
	if result["started"] != true || result["node"] != "mirror2" {
		t.Fatalf("create %v", result)
	}
	if !f.mod.Busy() && f.mod.provisionSnapshot()["state"] != "succeeded" {
		t.Fatalf("a running provision keeps the core busy")
	}
	snapshot := f.waitState("succeeded")
	if snapshot["stage"] != 6 || snapshot["resumeRequested"] != false || snapshot["instanceId"] != pvInstance || snapshot["ip"] != pvInstanceIP ||
		snapshot["dnsHostname"] != "mirror2.example.net" || snapshot["publicationGap"] != "its repository is still empty — no refs to serve yet" {
		t.Fatalf("snapshot %v", snapshot)
	}
	details := snapshot["stageDetails"].([]string)
	if len(details) != 6 || !strings.HasPrefix(details[5], "ForkMesh is running on mirror2 — verifying healthy public traffic (2/105)… Still missing:") {
		t.Fatalf("details %q", details)
	}
	status := snapshot["status"].(map[string]any)
	if status["state"] != "succeeded" || status["resumable"] != false || status["attempts"] != 1 || status["stageCount"] != 6 {
		t.Fatalf("status %v", status)
	}
	f.vultr.mu.Lock()
	created, requests := f.vultr.created, append([]string{}, f.vultr.requests...)
	f.vultr.mu.Unlock()
	if created["plan"] != "vc2-1c-2gb" || created["region"] != "atl" || created["os_id"] != 2136.0 || created["label"] != "mirror2" ||
		created["hostname"] != "mirror2" || created["backups"] != "disabled" || created["enable_ipv6"] != false ||
		fmt.Sprint(created["sshkey_id"]) != "[key-1]" || fmt.Sprint(created["tags"]) != "[forkmesh-mirror]" {
		t.Fatalf("create payload %v", created)
	}
	wantOrder := []string{"GET /v2/ssh-keys?per_page=500", "POST /v2/ssh-keys", "GET /v2/plans?per_page=500", "GET /v2/os?per_page=500",
		"GET /v2/instances?per_page=500", "POST /v2/instances", "GET /v2/instances/" + pvInstance, "GET /v2/instances/" + pvInstance}
	if strings.Join(requests, "\n") != strings.Join(wantOrder, "\n") {
		t.Fatalf("vultr requests %q", requests)
	}
	stdin, _ := os.ReadFile(f.stdin)
	if !strings.HasPrefix(string(stdin), pvTunnelToken+"\n"+vultrMirrorUploadMarker+"\n9\n") {
		t.Fatalf("install stdin %q", leftUTF16(string(stdin), 80))
	}
	calls := f.calls()
	for _, want := range []string{"ssh-keygen -q -t ed25519 -N  -C forkmesh-shared-host-key -f " + filepath.Join(f.mod.SSHDir, sharedKeyName),
		"root@" + pvInstanceIP + " echo forkmesh-ssh-ready",
		"git --git-dir=" + f.env["mirrorRoot"].(string) + "/forkmesh-forkmesh.git push --prune --receive-pack=runuser -u forkmesh-node -- git-receive-pack ssh://root@" + pvInstanceIP + "/var/lib/forkmesh/repositories/forkmesh-forkmesh.git",
		"| ssh -i '" + filepath.Join(f.mod.SSHDir, sharedKeyName) + "' -o StrictHostKeyChecking=accept-new -o ConnectTimeout=10"} {
		if !strings.Contains(calls, want) {
			t.Fatalf("missing call %q in\n%s", want, calls)
		}
	}
	host := f.savedHost("mirror2")
	if host == nil || host["status"] != "installed" || host["provider"] != "Vultr" || host["instanceId"] != pvInstance || host["planType"] != "vc2-1c-2gb" ||
		host["region"] != "atl" || host["monthlyCost"] != 10.0 || host["ramMb"] != 2048.0 || host["diskGb"] != 55.0 ||
		host["publicOrigin"] != "https://mirror2.example.net" || host["identityFile"] != filepath.Join(f.mod.SSHDir, sharedKeyName) || host["ip"] != pvInstanceIP {
		t.Fatalf("saved host %v", host)
	}
	checkpoint := map[string]any{}
	if err := json.Unmarshal([]byte(f.store.get(provisionSetting)), &checkpoint); err != nil {
		t.Fatal(err)
	}
	if checkpoint["version"] != 1.0 || checkpoint["state"] != "succeeded" || checkpoint["node"] != "mirror2" || checkpoint["installAttempts"] != 1.0 ||
		!strings.HasPrefix(checkpoint["message"].(string), "Vultr mirror \"mirror2\" (mirror2.example.net, "+pvInstanceIP+") is installed, linked, healthy") {
		t.Fatalf("checkpoint %v", checkpoint)
	}
	transcript := f.mod.transcriptText(installChannel)
	for _, want := range []string{"Creating Vultr mirror \"mirror2\" — cheapest supported US plan", "Shared ForkMesh SSH key: ",
		"Cheapest supported plan: vc2-1c-2gb ($10/month, 2048 MB RAM, 55 GB disk) in region atl", "Operating system: Debian 12 x64 (bookworm)",
		"Instance " + pvInstance + " created — waiting for it to boot…", "Instance is up at " + pvInstanceIP + ".", "SSH answered on " + pvInstanceIP + " after 1 knock.",
		"──── Attempt 1 of 1 started at ", "Install attempts this run:\n  • Attempt 1 of 1 started at ", "— installed",
		"✔ Repository seed push completed.", "mirror2 is not a healthy public mirror yet: its repository is still empty — no refs to serve yet.",
		"✔ Verified mirror2 directly", "✔ Vultr mirror \"mirror2\""} {
		if !strings.Contains(transcript, want) {
			t.Fatalf("transcript lacks %q:\n%s", want, transcript)
		}
	}
	if strings.Contains(transcript, "Cloudflare credential kept") {
		t.Fatalf("a fresh run clears the log after the credential note")
	}
	logged, _ := os.ReadFile(f.logFile)
	if !strings.Contains(string(logged), "✔ Verified mirror2 directly") {
		t.Fatalf("provision log not written: %q", logged)
	}
	notes := f.ui.named("core.hosts.notify")
	if len(notes) != 6 || notes[0]["title"] != "Vultr mirror deployment · Credentials succeeded" ||
		notes[5]["title"] != "Vultr mirror deployment · Live traffic succeeded" || notes[5]["error"] != false ||
		!strings.HasPrefix(notes[5]["body"].(string), "mirror2 · step 6 of 6 succeeded. Vultr mirror") {
		t.Fatalf("notifications %v", notes)
	}
	provisions := f.ui.named("core.hosts.provision")
	if len(provisions) == 0 || provisions[len(provisions)-1]["state"] != "succeeded" {
		t.Fatalf("provision events %d", len(provisions))
	}
	if f.mod.status("vultr") != "" {
		t.Fatalf("a successful run clears the status: %q", f.mod.status("vultr"))
	}
	jobs := f.mod.jobList()
	found := false
	for _, job := range jobs {
		if job.Kind == "provision" && job.Host == "mirror2" && !job.Running && job.OK {
			found = true
		}
	}
	if !found {
		t.Fatalf("no finished provision job: %v", jobs)
	}
}

func TestProvisionRefusals(t *testing.T) {
	f := newPvFixture(t, pvHosts(), nil, nil)
	started, _, message := f.mod.createProvision(t.Context(), "mirror1", "", "", "", false, "desktop")
	if started || message != "A saved host named \"mirror1\" already exists — choose a different node name." || f.mod.status("vultr") != message {
		t.Fatalf("duplicate: %v %q", started, message)
	}
	started, _, message = f.mod.createProvision(t.Context(), "Bad_Name", "", "", "", false, "desktop")
	if started || !strings.HasPrefix(message, "The node name must start with a letter") {
		t.Fatalf("bad name: %q", message)
	}
	f.mod.Variables = func() map[string]string { return map[string]string{"VULTR_API_KEY": pvVultrKey} }
	started, _, message = f.mod.createProvision(t.Context(), "mirror9", "", "", "", false, "desktop")
	if started || !strings.HasPrefix(message, "A Cloudflare API token and valid zone are required") {
		t.Fatalf("no tunnel: %q", message)
	}
	started, _, message = f.mod.createVultrMirror(t.Context(), "mirror9", "", "nope-plan", "", "phone")
	if started || f.mod.Busy() {
		t.Fatalf("phone create without a tunnel token: %v %q", started, message)
	}
}

func TestProvisionIPv6OnlyFails(t *testing.T) {
	f := newPvFixture(t, pvHosts(), nil, nil)
	f.vultr.ready = map[string]any{"status": "active", "power_status": "running", "main_ip": "0.0.0.0", "v6_main_ip": "2001:db8::5"}
	started, node, _ := f.mod.createVultrMirror(t.Context(), "mirror7", "", "", "", "phone")
	if !started || node != "mirror7" {
		t.Fatalf("not started")
	}
	snapshot := f.waitState("failed")
	want := "Vultr gave this instance an IPv6 address only, which the mesh cannot reach. Destroy instance " + pvInstance + " in the Vultr panel and retry — ForkMesh only deploys IPv4 plans."
	if snapshot["message"] != want || snapshot["resumeRequested"] != true || snapshot["stage"] != 4 {
		t.Fatalf("snapshot %v", snapshot)
	}
	if f.mod.status("vultr") != "✘ "+want {
		t.Fatalf("status %q", f.mod.status("vultr"))
	}
	if details := snapshot["stageDetails"].([]string); details[3] != want {
		t.Fatalf("failed stage detail %q", details[3])
	}
	if status := snapshot["status"].(map[string]any); status["resumable"] != true {
		t.Fatalf("a failed run is resumable: %v", status)
	}
	notes := f.ui.named("core.hosts.notify")
	last := notes[len(notes)-1]
	if last["title"] != "Vultr mirror deployment · Boot + connect failed" || last["error"] != true {
		t.Fatalf("failure ping %v", last)
	}
	f.vultr.mu.Lock()
	created := f.vultr.created
	f.vultr.mu.Unlock()
	if created["plan"] != "vc2-1c-1gb" || created["region"] != "ewr" {
		t.Fatalf("default plan %v", created)
	}
}

func TestProvisionKeyRejectedAfterThreeKnocks(t *testing.T) {
	f := newPvFixture(t, pvHosts(), nil, nil)
	t.Setenv("PV_SSH_MODE", "denied")
	f.vultr.ready = map[string]any{"status": "active", "power_status": "running", "main_ip": pvInstanceIP}
	if started, _, _ := f.mod.createVultrMirror(t.Context(), "mirror7", "", "", "", "desktop"); !started {
		t.Fatalf("not started")
	}
	snapshot := f.waitState("failed")
	if snapshot["message"] != "The new instance refused the managed SSH key. It is saved under Hosts — fix its credentials there and click Update to finish the install." {
		t.Fatalf("snapshot %v", snapshot)
	}
	if got := strings.Count(f.calls(), "echo forkmesh-ssh-ready"); got != 3 {
		t.Fatalf("knocks %d", got)
	}
	transcript := f.mod.transcriptText(installChannel)
	if !strings.Contains(transcript, "SSH probe: exit 255 — root@host: Permission denied (publickey).") {
		t.Fatalf("transcript %s", transcript)
	}
	if host := f.savedHost("mirror7"); host == nil || host["status"] != "vultr booting" || host["instanceId"] != pvInstance {
		t.Fatalf("saved %v", host)
	}
}

func TestProvisionResumesActiveCheckpoint(t *testing.T) {
	keyDir := t.TempDir()
	key := filepath.Join(keyDir, "id")
	_ = os.WriteFile(key, []byte("k"), 0o600)
	checkpoint := compactJSON(map[string]any{
		"version": 1, "state": "active", "stage": 4, "detail": "Waiting for the server to boot and accept SSH…", "message": "",
		"node": "mirror5", "dnsHostname": "mirror5.example.net", "instanceId": pvInstance, "ip": "", "identityFile": key,
		"installAgentClis": false, "installUseLocalBinary": false, "pollCount": 3, "installAttempts": 0,
		"hostMetadata": map[string]any{"provider": "Vultr", "instanceId": pvInstance, "dnsHostname": "mirror5.example.net"}, "updatedAt": 1,
	})
	f := newPvFixture(t, pvHosts(), map[string]string{provisionSetting: checkpoint}, func(f *pvFixture) {
		f.vultr.ready = map[string]any{"status": "active", "power_status": "running", "main_ip": pvInstanceIP}
		f.vultr.polls = 1
		f.rewrite.host = "mirror5.example.net"
		f.healthServer(1)
		_ = os.MkdirAll(filepath.Dir(f.logFile), 0o700)
		_ = os.WriteFile(f.logFile, []byte("earlier output\n"), 0o600)
	})
	snapshot := f.waitState("succeeded")
	if snapshot["node"] != "mirror5" || snapshot["publicationGap"] != "" {
		t.Fatalf("snapshot %v", snapshot)
	}
	transcript := f.mod.transcriptText(installChannel)
	if !strings.HasPrefix(transcript, "earlier output\n") || !strings.Contains(transcript, "\n↻ Desktop restarted — resuming Vultr deployment at stage 4.\n") ||
		!strings.Contains(transcript, "Instance is up at "+pvInstanceIP) || !strings.Contains(transcript, "is installed, linked, healthy, and serving repository traffic.") {
		t.Fatalf("transcript %s", transcript)
	}
	f.vultr.mu.Lock()
	requests := strings.Join(f.vultr.requests, "\n")
	f.vultr.mu.Unlock()
	if strings.Contains(requests, "POST") || strings.Contains(requests, "/v2/plans") {
		t.Fatalf("a stage-4 resume only polls: %s", requests)
	}
	if host := f.savedHost("mirror5"); host == nil || host["status"] != "installed" || host["identityFile"] != key {
		t.Fatalf("saved %v", host)
	}
}

func TestProvisionEndStopsTheRun(t *testing.T) {
	f := newPvFixture(t, pvHosts(), nil, nil)
	release := make(chan struct{})
	f.vultr.block = release
	if started, _, _ := f.mod.createVultrMirror(t.Context(), "mirror7", "", "", "", "desktop"); !started {
		t.Fatalf("not started")
	}
	deadline := time.Now().Add(5 * time.Second)
	for {
		f.vultr.mu.Lock()
		polls := f.vultr.polls
		f.vultr.mu.Unlock()
		if polls > 0 {
			break
		}
		if time.Now().After(deadline) {
			t.Fatalf("never polled: %s", f.mod.transcriptText(installChannel))
		}
		time.Sleep(5 * time.Millisecond)
	}
	if !f.mod.Busy() || !f.mod.provisionActive() {
		t.Fatalf("running provision is not busy")
	}
	if !f.store.has(provisionSetting) {
		t.Fatalf("no checkpoint while running")
	}
	coretest.MustCall(t, f.core, coretest.NewPeer("desk", rpc.UI), "core.hosts.provision.end", nil, nil)
	close(release)
	time.Sleep(50 * time.Millisecond)
	snapshot := f.mod.provisionSnapshot()
	if snapshot["active"] != false || snapshot["state"] != "" || snapshot["node"] != "" || snapshot["stage"] != 0 {
		t.Fatalf("snapshot %v", snapshot)
	}
	if f.store.has(provisionSetting) {
		t.Fatalf("checkpoint kept after end")
	}
	if !strings.Contains(f.mod.transcriptText(installChannel), "\n⚠ Deployment ended by request (mirror7).\n") {
		t.Fatalf("transcript %s", f.mod.transcriptText(installChannel))
	}
	if f.mod.Busy() {
		t.Fatalf("still busy after end")
	}
	if _, err := coretest.Call(t, f.core, coretest.NewPeer("phone", rpc.Relay), "core.hosts.provision.end", nil); rpc.Code(err) != "forbidden" {
		t.Fatalf("phones may not end: %v", err)
	}
	resumed := map[string]any{}
	coretest.MustCall(t, f.core, coretest.NewPeer("desk", rpc.UI), "core.hosts.provision.resume", map[string]any{"apiKey": ""}, &resumed)
	if resumed["started"] != false {
		t.Fatalf("nothing to resume: %v", resumed)
	}
}

func pvDestroyHosts() []map[string]any {
	return []map[string]any{
		{"name": "vm-a", "ip": "198.51.100.20", "user": "root", "provider": "Vultr", "instanceId": pvDestroyA, "status": "installed"},
		{"name": "vm-b", "ip": "198.51.100.21", "user": "root", "provider": "Vultr", "status": "installed"},
		{"name": "vm-c", "ip": "198.51.100.22", "user": "root", "provider": "Vultr", "status": "installed"},
	}
}

func (f *pvFixture) destroy(name string) (bool, string) {
	type outcome struct {
		ok      bool
		message string
	}
	done := make(chan outcome, 1)
	f.mod.destroyVultrHost(f.t.Context(), name, "", "phone", func(ok bool, message string) { done <- outcome{ok, message} })
	select {
	case got := <-done:
		return got.ok, got.message
	case <-time.After(10 * time.Second):
		f.t.Fatalf("destroy of %s never finished", name)
	}
	return false, ""
}

func TestDestroyVultrHost(t *testing.T) {
	f := newPvFixture(t, pvDestroyHosts(), nil, nil)
	f.vultr.instances = []map[string]any{
		{"id": pvDestroyB, "main_ip": "198.51.100.21", "label": "vm-b"},
		{"id": pvDestroyC1, "main_ip": "198.51.100.22"},
		{"id": pvDestroyC2, "main_ip": "198.51.100.22"},
	}
	ok, message := f.destroy("vm-a")
	if !ok || message != "Destroyed \"vm-a\" on Vultr and removed it from this list." {
		t.Fatalf("recorded: %v %q", ok, message)
	}
	ok, message = f.destroy("vm-b")
	if !ok || f.savedHost("vm-b") != nil || f.savedHost("vm-a") != nil {
		t.Fatalf("address lookup: %v %q", ok, message)
	}
	ok, message = f.destroy("vm-c")
	if ok || message != "No single Vultr instance matches \"vm-c\" (198.51.100.22), so nothing was destroyed. Delete it from the Vultr panel instead." || f.savedHost("vm-c") == nil {
		t.Fatalf("ambiguous: %v %q", ok, message)
	}
	f.vultr.mu.Lock()
	deleted := strings.Join(f.vultr.deleted, ",")
	f.vultr.mu.Unlock()
	if deleted != pvDestroyA+","+pvDestroyB {
		t.Fatalf("deleted %s", deleted)
	}
	transcript := f.mod.transcriptText(installChannel)
	for _, want := range []string{"Destroying Vultr instance " + pvDestroyA + " (\"vm-a\")…\n", "Vultr instance " + pvDestroyA + " destroyed.\n",
		"Removed 2 git remotes that pointed at \"vm-a\".\n"} {
		if !strings.Contains(transcript, want) {
			t.Fatalf("transcript lacks %q: %s", want, transcript)
		}
	}
	ledger := map[string]any{}
	_ = json.Unmarshal([]byte(f.store.get(decommissionedNodesSetting)), &ledger)
	if ledger["vm-a"] == nil || ledger["vm-b"] == nil {
		t.Fatalf("ledger %v", ledger)
	}
	if ok, message = f.destroy("ghost"); ok || message != "No saved host is named \"ghost\"." {
		t.Fatalf("ghost %q", message)
	}
	answer := map[string]any{}
	coretest.MustCall(t, f.core, coretest.NewPeer("desk", rpc.UI), "core.hosts.destroy", map[string]any{"name": "ghost", "apiKey": ""}, &answer)
	if answer["started"] != false || answer["message"] != "No saved host is named \"ghost\"." {
		t.Fatalf("desktop destroy %v", answer)
	}
}

type pvRelay struct {
	mu       sync.Mutex
	auth     string
	query    url.Values
	body     map[string]any
	response string
}

func (f *pvFixture) relay(response string) *pvRelay {
	relay := &pvRelay{response: response}
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		raw, _ := io.ReadAll(r.Body)
		relay.mu.Lock()
		relay.auth = r.Header.Get("Authorization")
		relay.query = r.URL.Query()
		relay.body = map[string]any{}
		_ = json.Unmarshal(raw, &relay.body)
		body := relay.response
		relay.mu.Unlock()
		if r.URL.Path != "/api/world/admin/nodes/delete" {
			w.WriteHeader(404)
			return
		}
		_, _ = w.Write([]byte(body))
	}))
	f.t.Cleanup(server.Close)
	f.setEnv("nodeDeleteUrl", server.URL+"/api/world/admin/nodes/delete")
	cf := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch {
		case r.URL.Path == "/zones":
			_, _ = w.Write([]byte(`{"success":true,"result":[{"id":"zone1","name":"example.net"}]}`))
		case r.Method == http.MethodGet && r.URL.Path == "/zones/zone1/dns_records":
			name := r.URL.Query().Get("name")
			_, _ = w.Write([]byte(`{"success":true,"result":[{"id":"rec1","name":"` + name + `","type":"A"}]}`))
		case r.Method == http.MethodDelete && r.URL.Path == "/zones/zone1/dns_records/rec1":
			_, _ = w.Write([]byte(`{"success":true,"result":{"id":"rec1"}}`))
		default:
			w.WriteHeader(404)
		}
	}))
	f.t.Cleanup(cf.Close)
	f.mod.CloudflareBase = cf.URL
	return relay
}

func (f *pvFixture) waitNodeDeleted() map[string]any {
	f.t.Helper()
	deadline := time.Now().Add(10 * time.Second)
	for time.Now().Before(deadline) {
		if events := f.ui.named("core.hosts.nodeDeleted"); len(events) > 0 {
			for f.mod.Busy() && time.Now().Before(deadline) {
				time.Sleep(5 * time.Millisecond)
			}
			return events[len(events)-1]
		}
		time.Sleep(5 * time.Millisecond)
	}
	f.t.Fatalf("node delete never finished")
	return nil
}

func TestDeleteNodeWithSessionToken(t *testing.T) {
	f := newPvFixture(t, pvDestroyHosts(), nil, nil)
	relay := f.relay(`{"ok":true,"nodeDeleted":"vm-a","identifiers":["VM-A","vm-a-alias",""]}`)
	f.setEnv("sessionToken", "sess-1")
	answer := map[string]any{}
	coretest.MustCall(t, f.core, coretest.NewPeer("desk", rpc.UI), "core.hosts.deleteNode", map[string]any{"node": " VM-A ", "nodeId": "id-1", "origin": "desktop"}, &answer)
	if answer["started"] != true {
		t.Fatalf("answer %v", answer)
	}
	event := f.waitNodeDeleted()
	message := "3/3 complete — Relay confirmed deletion of \"vm-a\"; it has been removed from this list."
	if event["ok"] != true || event["node"] != "vm-a" || event["message"] != message || fmt.Sprint(event["deletedNames"]) != "[vm-a vm-a-alias]" || event["alreadyAbsent"] != false {
		t.Fatalf("event %v", event)
	}
	relay.mu.Lock()
	auth, query, body := relay.auth, relay.query, relay.body
	relay.mu.Unlock()
	if auth != "Bearer sess-1" || len(query) != 0 || body["nodeName"] != "vm-a" || body["nodeId"] != "id-1" || body["confirmation"] != "DELETE vm-a" {
		t.Fatalf("request %q %v %v", auth, query, body)
	}
	f.vultr.mu.Lock()
	deleted := strings.Join(f.vultr.deleted, ",")
	f.vultr.mu.Unlock()
	if deleted != pvDestroyA || f.savedHost("vm-a") != nil {
		t.Fatalf("server %s, host %v", deleted, f.savedHost("vm-a"))
	}
	for _, want := range []string{"git remote remove gw-a |", "git remote remove vma |"} {
		if !strings.Contains(f.calls(), want) {
			t.Fatalf("unwired: %s", f.calls())
		}
	}
	if strings.Contains(f.calls(), "remote remove keep") || strings.Contains(f.calls(), "remote remove origin") {
		t.Fatalf("unwired too much: %s", f.calls())
	}
	for _, setting := range []string{deletedMeshNodesSetting, decommissionedNodesSetting} {
		ledger := map[string]any{}
		_ = json.Unmarshal([]byte(f.store.get(setting)), &ledger)
		if ledger["vm-a"] == nil || ledger["vm-a-alias"] == nil {
			t.Fatalf("%s %v", setting, ledger)
		}
	}
	if f.mod.status("nodes") != message {
		t.Fatalf("status %q", f.mod.status("nodes"))
	}
	if busy, _ := f.mod.nodeDeleteState(); busy {
		t.Fatalf("still in progress")
	}
	statuses := []string{}
	for _, status := range f.ui.named("core.hosts.status") {
		if status["target"] == "nodes" {
			statuses = append(statuses, status["text"].(string))
		}
	}
	want := []string{"1/3 — Destroying the server for \"vm-a\"…", "1/3 — Destroying the Vultr server behind \"vm-a\"…",
		"Destroyed Vultr instance " + pvDestroyA + " (\"vm-a\").", "2/3 — Removing DNS for \"vm-a\"…", "Removed the vm-a.example.net DNS record.",
		"3/3 — Removing \"vm-a\" from the mesh…", "3/3 — Removing \"vm-a\" from the mesh…", message}
	if strings.Join(statuses, "\n") != strings.Join(want, "\n") {
		t.Fatalf("statuses\n%s", strings.Join(statuses, "\n"))
	}
}

func TestDeleteNodeSignedFallbackAndRelayRefusal(t *testing.T) {
	f := newPvFixture(t, pvDestroyHosts(), nil, nil)
	id, _ := identity.FromSeed([]byte(strings.Repeat("s", 32)))
	f.core.Identity = id
	relay := f.relay(`{"ok":true,"nodeDeleted":"vm-b","alreadyAbsent":true}`)
	f.setEnv("canSign", true)
	if started, _ := f.mod.deleteNode(t.Context(), "vm-b", "", "Pixel (alice), through the relay"); !started {
		t.Fatalf("not started")
	}
	event := f.waitNodeDeleted()
	if event["ok"] != true || event["message"] != "Relay confirmed \"vm-b\" was already absent; removed its stale local entry." {
		t.Fatalf("event %v", event)
	}
	relay.mu.Lock()
	query, auth := relay.query, relay.auth
	relay.mu.Unlock()
	canonical := "forkmesh-world-node-delete-v1\nalice\nvm-b\n" + query.Get("ts")
	if auth != "" || query.Get("node") != "alice" || !identity.Verify(id.PublicKey(), query.Get("sig"), []byte(canonical)) {
		t.Fatalf("signed query %v", query)
	}
	found := false
	for _, status := range f.ui.named("core.hosts.status") {
		if status["text"] == "1/3: destroying the server for \"vm-b\" (from Pixel (alice), through the relay)" {
			found = true
		}
	}
	if !found {
		t.Fatalf("phone wording missing")
	}

	relay.mu.Lock()
	relay.response = `{"ok":true,"nodeDeleted":"something-else"}`
	relay.mu.Unlock()
	before := len(f.ui.named("core.hosts.nodeDeleted"))
	if started, _ := f.mod.deleteNode(t.Context(), "vm-c", "", "desktop"); !started {
		t.Fatalf("not started")
	}
	deadline := time.Now().Add(10 * time.Second)
	for len(f.ui.named("core.hosts.nodeDeleted")) == before && time.Now().Before(deadline) {
		time.Sleep(5 * time.Millisecond)
	}
	events := f.ui.named("core.hosts.nodeDeleted")
	refusal := events[len(events)-1]
	message := "The relay did not confirm deletion of \"vm-c\"; it remains visible."
	if refusal["ok"] != false || refusal["message"] != message || f.savedHost("vm-c") == nil {
		t.Fatalf("refusal %v", refusal)
	}
	flashes := f.ui.named("core.hosts.flash")
	if len(flashes) == 0 || flashes[len(flashes)-1]["text"] != message || flashes[len(flashes)-1]["error"] != true {
		t.Fatalf("flash %v", flashes)
	}
	for f.mod.Busy() && time.Now().Before(deadline) {
		time.Sleep(5 * time.Millisecond)
	}
	f.setEnv("canSign", false)
	if started, why := f.mod.deleteNode(t.Context(), "vm-c", "", "desktop"); started || !strings.HasPrefix(why, "Sign in to your admin account") {
		t.Fatalf("unsigned: %v %q", started, why)
	}
	f.setEnv("admin", false)
	if started, why := f.mod.deleteNode(t.Context(), "vm-c", "", "desktop"); started || why != "Only a platform admin can delete a node from the mesh." {
		t.Fatalf("not admin: %q", why)
	}
	if started, _ := f.mod.deleteNode(t.Context(), "ForkMesh", "", "desktop"); started {
		t.Fatalf("protected node")
	}
}
