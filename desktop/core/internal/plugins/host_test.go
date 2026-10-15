package plugins

import (
	"bufio"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"net"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
	"testing"
	"time"

	"forkmesh.com/desktop-core/internal/coretest"
	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/identity"
	"forkmesh.com/desktop-core/internal/rpc"
)

func TestMain(m *testing.M) {
	if len(os.Args) > 1 {
		switch os.Args[1] {
		case "plugin-shim":
			os.Exit(ShimMain(os.Args[2:]))
		case "helper-plugin":
			os.Exit(helperPlugin())
		}
	}
	os.Exit(m.Run())
}

func helperPlugin() int {
	var writeMu sync.Mutex
	write := func(value any) {
		raw, _ := json.Marshal(value)
		writeMu.Lock()
		os.Stdout.Write(append(raw, '\n'))
		writeMu.Unlock()
	}
	var mu sync.Mutex
	cancels := map[int64]chan struct{}{}
	hostReplies := map[int64]chan json.RawMessage{}
	reader := bufio.NewReader(os.Stdin)
	for {
		line, err := reader.ReadBytes('\n')
		if err != nil {
			return 0
		}
		var message struct {
			ID     int64           `json:"id"`
			Method string          `json:"method"`
			Params json.RawMessage `json:"params"`
			OK     *bool           `json:"ok"`
			Result json.RawMessage `json:"result"`
			Error  string          `json:"error"`
		}
		if json.Unmarshal(line, &message) != nil {
			continue
		}
		if message.OK != nil {
			mu.Lock()
			wait := hostReplies[message.ID]
			mu.Unlock()
			if wait != nil {
				if *message.OK {
					wait <- message.Result
				} else {
					wait <- json.RawMessage(`{"error":"` + message.Error + `"}`)
				}
			}
			continue
		}
		reply := func(result any) { write(map[string]any{"id": message.ID, "ok": true, "result": result}) }
		switch message.Method {
		case "plugin.init":
			reply(map[string]any{"ready": true})
		case "plugin.shutdown":
			return 0
		case "$/cancel":
			var params struct {
				ID int64 `json:"id"`
			}
			_ = json.Unmarshal(message.Params, &params)
			mu.Lock()
			if stop := cancels[params.ID]; stop != nil {
				close(stop)
				delete(cancels, params.ID)
			}
			mu.Unlock()
		case "echo":
			reply(message.Params)
		case "env":
			reply(map[string]any{"env": os.Environ()})
		case "crash":
			os.Exit(3)
		case "dial":
			var params struct {
				Address string `json:"address"`
			}
			_ = json.Unmarshal(message.Params, &params)
			conn, err := net.DialTimeout("tcp", params.Address, 2*time.Second)
			if err == nil {
				conn.Close()
			}
			reply(map[string]any{"connected": err == nil})
		case "emit":
			write(map[string]any{"event": "secret", "data": map[string]any{"leak": true}})
			write(map[string]any{"event": "changed", "data": map[string]any{"n": 1}})
			reply(map[string]any{})
		case "slow":
			stop := make(chan struct{})
			mu.Lock()
			cancels[message.ID] = stop
			mu.Unlock()
			id := message.ID
			go func() {
				write(map[string]any{"event": "$/progress", "data": map[string]any{"id": id, "step": 1}})
				select {
				case <-stop:
					write(map[string]any{"id": id, "ok": false, "error": "cancelled"})
				case <-time.After(20 * time.Second):
					write(map[string]any{"id": id, "ok": true, "result": map[string]any{}})
				}
			}()
		case "host":
			go func() {
				results := map[string]json.RawMessage{}
				for i, method := range []string{"host.info", "host.secrets"} {
					id := int64(1000 + i)
					wait := make(chan json.RawMessage, 1)
					mu.Lock()
					hostReplies[id] = wait
					mu.Unlock()
					write(map[string]any{"id": id, "method": method, "params": map[string]any{}})
					select {
					case got := <-wait:
						results[method] = got
					case <-time.After(5 * time.Second):
					}
				}
				reply(results)
			}()
		default:
			write(map[string]any{"id": message.ID, "ok": false, "error": "unknown"})
		}
	}
}

type fixture struct {
	t        *testing.T
	core     *daemon.Core
	host     *Host
	ui       *coretest.Peer
	packages string
	signer   *identity.Identity
}

func newFixture(t *testing.T) *fixture {
	if runtime.GOOS == "windows" {
		t.Skip("the helper plug-in uses /bin/sh")
	}
	seed := make([]byte, 32)
	seed[0] = 7
	signer, err := identity.FromSeed(seed)
	if err != nil {
		t.Fatal(err)
	}
	core := coretest.New(t)
	root := t.TempDir()
	host := NewWithOptions(Options{
		PackagesDir:  filepath.Join(root, "packages"),
		SignersFile:  filepath.Join(root, "signers.json"),
		ConsentFile:  filepath.Join(root, "consent.json"),
		DataDir:      filepath.Join(root, "data"),
		LogDir:       filepath.Join(root, "logs"),
		ShimBinary:   os.Args[0],
		TrustedKeys:  []string{signer.PublicKey()},
		ScanInterval: 100 * time.Millisecond,
		IdleTick:     100 * time.Millisecond,
	})
	ui := coretest.NewPeer("ui:1", rpc.UI)
	core.Hub.Add(ui)
	coretest.Start(t, core, host)
	return &fixture{t: t, core: core, host: host, ui: ui, packages: filepath.Join(root, "packages"), signer: signer}
}

func (f *fixture) install(id string, mutate func(manifest map[string]any)) string {
	dir := filepath.Join(f.packages, id)
	if err := os.MkdirAll(filepath.Join(dir, "content"), 0o755); err != nil {
		f.t.Fatal(err)
	}
	binary, _ := filepath.Abs(os.Args[0])
	script := fmt.Sprintf("exec '%s' helper-plugin\n", binary)
	if err := os.WriteFile(filepath.Join(dir, "content", "run.sh"), []byte(script), 0o644); err != nil {
		f.t.Fatal(err)
	}
	manifest := map[string]any{
		"schemaVersion": 1, "id": id, "name": "Helper " + id, "version": "1.0.0", "type": "service.development",
		"summary": "test plug-in", "permissions": []map[string]string{},
		"service": map[string]any{"api": 1, "exec": "content/run.sh", "interpreter": "sh",
			"methods": []string{"echo", "env", "crash", "dial", "emit", "slow", "host"}, "events": []string{"changed"}},
	}
	if mutate != nil {
		mutate(manifest)
	}
	raw, _ := json.MarshalIndent(manifest, "", "  ")
	if err := os.WriteFile(filepath.Join(dir, "manifest.json"), raw, 0o644); err != nil {
		f.t.Fatal(err)
	}
	if _, err := Sign(dir, f.signer); err != nil {
		f.t.Fatal(err)
	}
	f.host.Rescan()
	return dir
}

func (f *fixture) call(method string, params any) (json.RawMessage, error) {
	return coretest.Call(f.t, f.core, f.ui, method, params)
}

func (f *fixture) row(id string) map[string]any {
	for _, row := range f.host.List() {
		if row["id"] == id {
			return row
		}
	}
	f.t.Fatalf("no plug-in %s", id)
	return nil
}

func (f *fixture) waitState(id, state string) {
	deadline := time.Now().Add(10 * time.Second)
	for time.Now().Before(deadline) {
		if f.row(id)["state"] == state {
			return
		}
		time.Sleep(20 * time.Millisecond)
	}
	f.t.Fatalf("%s never reached %s: %+v", id, state, f.row(id))
}

func TestConsentGatesStartAndCallsWork(t *testing.T) {
	f := newFixture(t)
	f.install("echo", nil)
	if _, err := f.call("plugin.echo.echo", map[string]any{"a": 1}); rpc.Code(err) != "plugin_disabled" {
		t.Fatalf("disabled call: %v", err)
	}
	if _, err := f.call("core.plugins.enable", map[string]any{"id": "echo"}); err != nil {
		t.Fatal(err)
	}
	f.waitState("echo", StateStopped)
	got, err := f.call("plugin.echo.echo", map[string]any{"a": 1})
	if err != nil || string(got) != `{"a":1}` {
		t.Fatalf("echo %s %v", got, err)
	}
	if f.row("echo")["state"] != StateRunning {
		t.Fatalf("lazy plug-in not running after a call: %+v", f.row("echo"))
	}
	if _, err := f.call("plugin.echo.missing", nil); rpc.Code(err) != "unknown_method" {
		t.Fatalf("undeclared method: %v", err)
	}
	phone := coretest.NewPeer("lan:1", rpc.LAN)
	if _, err := coretest.Call(t, f.core, phone, "plugin.echo.echo", nil); rpc.Code(err) != "forbidden" {
		t.Fatalf("phone call: %v", err)
	}
	if _, err := f.call("core.plugins.disable", map[string]any{"id": "echo"}); err != nil {
		t.Fatal(err)
	}
	f.waitState("echo", StateDisabled)
}

func TestEnvironmentIsMinimal(t *testing.T) {
	t.Setenv("FORKMESH_SECRET_TOKEN", "do-not-leak")
	f := newFixture(t)
	f.install("envcheck", nil)
	f.call("core.plugins.enable", map[string]any{"id": "envcheck"})
	got, err := f.call("plugin.envcheck.env", nil)
	if err != nil {
		t.Fatal(err)
	}
	text := string(got)
	if strings.Contains(text, "do-not-leak") || !strings.Contains(text, "FORKMESH_PLUGIN_ID=envcheck") {
		t.Fatalf("environment %s", text)
	}
}

func TestUnsignedTamperedAndUntrustedPackagesNeverRun(t *testing.T) {
	f := newFixture(t)
	dir := f.install("tampered", nil)
	if err := os.WriteFile(filepath.Join(dir, "content", "run.sh"), []byte("echo pwned\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	f.host.Rescan()
	if row := f.row("tampered"); row["state"] != StateInvalid {
		t.Fatalf("tampered %+v", row)
	}
	if _, err := f.call("core.plugins.enable", map[string]any{"id": "tampered"}); rpc.Code(err) != "plugin_invalid" {
		t.Fatalf("enable tampered: %v", err)
	}
	dir = f.install("unsigned", nil)
	os.Remove(filepath.Join(dir, SignatureFile))
	f.host.Rescan()
	if row := f.row("unsigned"); row["state"] != StateInvalid || !strings.Contains(fmt.Sprint(row["problem"]), "not signed") {
		t.Fatalf("unsigned %+v", row)
	}
	other, _ := identity.FromSeed(make([]byte, 32))
	dir = f.install("stranger", nil)
	if _, err := Sign(dir, other); err != nil {
		t.Fatal(err)
	}
	f.host.Rescan()
	if row := f.row("stranger"); row["state"] != StateUntrusted {
		t.Fatalf("stranger %+v", row)
	}
	if _, err := f.call("core.plugins.enable", map[string]any{"id": "stranger"}); rpc.Code(err) != "plugin_untrusted" {
		t.Fatalf("enable stranger: %v", err)
	}
}

func TestNewPermissionNeedsConsentAgain(t *testing.T) {
	f := newFixture(t)
	f.install("grow", nil)
	f.call("core.plugins.enable", map[string]any{"id": "grow"})
	f.waitState("grow", StateStopped)
	f.install("grow", func(manifest map[string]any) {
		manifest["version"] = "1.1.0"
		manifest["permissions"] = []map[string]string{{"id": PermissionNetwork, "label": "Network", "risk": "medium"}}
	})
	f.waitState("grow", StateNeedsConsent)
	if _, err := f.call("plugin.grow.echo", nil); rpc.Code(err) != "plugin_needs_consent" {
		t.Fatalf("call after growth: %v", err)
	}
	if _, err := f.call("core.plugins.enable", map[string]any{"id": "grow", "grants": []string{}}); rpc.Code(err) != "grants_incomplete" {
		t.Fatalf("partial grant: %v", err)
	}
	if _, err := f.call("core.plugins.enable", map[string]any{"id": "grow"}); err != nil {
		t.Fatal(err)
	}
	if _, err := f.call("plugin.grow.echo", nil); err != nil {
		t.Fatal(err)
	}
}

func TestEventsProgressAndCancel(t *testing.T) {
	f := newFixture(t)
	f.install("events", nil)
	f.call("core.plugins.enable", map[string]any{"id": "events"})
	if _, err := f.call("plugin.events.emit", nil); err != nil {
		t.Fatal(err)
	}
	if data := f.ui.Next(t, "plugin.events.changed", 5*time.Second); string(data) != `{"n":1}` {
		t.Fatalf("event %s", data)
	}
	ctx, cancel := context.WithCancel(context.Background())
	progress := make(chan json.RawMessage, 4)
	request := &rpc.Request{Peer: f.ui, ID: 9, Method: "plugin.events.slow", Params: json.RawMessage(`{}`)}
	request.SetProgress(func(data any) bool {
		raw, _ := json.Marshal(data)
		progress <- raw
		return true
	})
	done := make(chan error, 1)
	go func() {
		_, err := f.core.Registry.Dispatch(ctx, request)
		done <- err
	}()
	select {
	case got := <-progress:
		if !strings.Contains(string(got), `"step":1`) {
			t.Fatalf("progress %s", got)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("no progress")
	}
	cancel()
	select {
	case err := <-done:
		if !errors.Is(err, context.Canceled) {
			t.Fatalf("cancel: %v", err)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("cancel did not return")
	}
	if got, err := f.call("plugin.events.echo", map[string]any{"after": "cancel"}); err != nil || !strings.Contains(string(got), "cancel") {
		t.Fatalf("plug-in unusable after cancel: %s %v", got, err)
	}
}

func TestHostRequestsAreRestricted(t *testing.T) {
	f := newFixture(t)
	f.install("hostreq", nil)
	f.call("core.plugins.enable", map[string]any{"id": "hostreq"})
	got, err := f.call("plugin.hostreq.host", nil)
	if err != nil {
		t.Fatal(err)
	}
	var results map[string]map[string]any
	_ = json.Unmarshal(got, &results)
	if results["host.info"]["id"] != "hostreq" || results["host.secrets"]["error"] != "forbidden" {
		t.Fatalf("host replies %s", got)
	}
}

func TestBootPluginRestartsAfterCrashThenGivesUp(t *testing.T) {
	f := newFixture(t)
	f.install("boot", func(manifest map[string]any) {
		manifest["service"].(map[string]any)["start"] = "boot"
	})
	f.call("core.plugins.enable", map[string]any{"id": "boot"})
	f.waitState("boot", StateRunning)
	first := f.row("boot")["pid"]
	f.call("plugin.boot.crash", nil)
	f.waitState("boot", StateStopped)
	f.waitState("boot", StateRunning)
	if f.row("boot")["pid"] == first {
		t.Fatal("restarted with the same pid")
	}
	for i := 0; i < crashLimit; i++ {
		f.waitState("boot", StateRunning)
		f.call("plugin.boot.crash", nil)
		if f.row("boot")["state"] == StateFailed {
			break
		}
		time.Sleep(50 * time.Millisecond)
		if f.row("boot")["state"] == StateFailed {
			break
		}
		f.host.mu.Lock()
		if p := f.host.plugins["boot"]; p != nil {
			p.restartAt = time.Now()
		}
		f.host.mu.Unlock()
	}
	f.waitState("boot", StateFailed)
	if _, err := f.call("core.plugins.restart", map[string]any{"id": "boot"}); err != nil {
		t.Fatal(err)
	}
	f.waitState("boot", StateRunning)
}

func TestLazyPluginStopsWhenIdle(t *testing.T) {
	f := newFixture(t)
	f.install("idle", func(manifest map[string]any) {
		manifest["service"].(map[string]any)["idleExitSeconds"] = 10
	})
	f.call("core.plugins.enable", map[string]any{"id": "idle"})
	f.call("plugin.idle.echo", nil)
	f.host.mu.Lock()
	f.host.plugins["idle"].lastUsed = time.Now().Add(-time.Minute)
	f.host.mu.Unlock()
	f.waitState("idle", StateStopped)
}

func TestRemovedPackageStops(t *testing.T) {
	f := newFixture(t)
	dir := f.install("gone", nil)
	f.call("core.plugins.enable", map[string]any{"id": "gone"})
	f.call("plugin.gone.echo", nil)
	f.waitState("gone", StateRunning)
	pid := int(f.row("gone")["pid"].(int))
	if err := os.RemoveAll(dir); err != nil {
		t.Fatal(err)
	}
	deadline := time.Now().Add(10 * time.Second)
	for time.Now().Before(deadline) {
		if len(f.host.List()) == 0 {
			break
		}
		time.Sleep(50 * time.Millisecond)
	}
	if len(f.host.List()) != 0 {
		t.Fatal("removed package still listed")
	}
	process, err := os.FindProcess(pid)
	if err != nil {
		return
	}
	for deadline := time.Now().Add(8 * time.Second); time.Now().Before(deadline); time.Sleep(50 * time.Millisecond) {
		if process.Signal(nilSignal()) != nil {
			return
		}
	}
	t.Fatalf("pid %d still alive", pid)
}

func TestNetworkIsolationWithoutGrant(t *testing.T) {
	if !networkIsolationSupported() {
		t.Skip("no network namespaces here")
	}
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer listener.Close()
	go func() {
		for {
			conn, err := listener.Accept()
			if err != nil {
				return
			}
			conn.Close()
		}
	}()
	f := newFixture(t)
	f.install("offline", nil)
	f.install("online", func(manifest map[string]any) {
		manifest["permissions"] = []map[string]string{{"id": PermissionNetwork, "label": "Network", "risk": "medium"}}
	})
	f.call("core.plugins.enable", map[string]any{"id": "offline"})
	f.call("core.plugins.enable", map[string]any{"id": "online"})
	params := map[string]any{"address": listener.Addr().String()}
	online, err := f.call("plugin.online.dial", params)
	if err != nil || string(online) != `{"connected":true}` {
		t.Fatalf("online %s %v", online, err)
	}
	offline, err := f.call("plugin.offline.dial", params)
	if err != nil {
		t.Fatal(err)
	}
	if f.row("offline")["networkIsolation"] != true {
		t.Skipf("user namespaces are not available here; ran without isolation (%s)", offline)
	}
	if string(offline) != `{"connected":false}` {
		t.Fatalf("isolated plug-in reached the host: %s", offline)
	}
}

func TestPhonesReachOnlyRemoteMethodsAsOwner(t *testing.T) {
	f := newFixture(t)
	f.install("phoneapp", func(manifest map[string]any) {
		service := manifest["service"].(map[string]any)
		service["remoteMethods"] = []string{"echo"}
		service["remoteEvents"] = []string{"changed"}
	})
	f.call("core.plugins.enable", map[string]any{"id": "phoneapp"})
	f.core.Set("control.owners", []string{"jett"})
	owner := coretest.NewPeer("lan:owner", rpc.LAN)
	owner.Dev = rpc.Device{Key: "k1", User: "jett", Platform: "ios"}
	stranger := coretest.NewPeer("lan:other", rpc.LAN)
	stranger.Dev = rpc.Device{Key: "k2", User: "mallory", Platform: "ios"}
	f.core.Hub.Add(owner)
	f.core.Hub.Add(stranger)
	if _, err := coretest.Call(t, f.core, stranger, "plugin.phoneapp.echo", nil); rpc.Code(err) != "not_owner" {
		t.Fatalf("stranger: %v", err)
	}
	if _, err := coretest.Call(t, f.core, owner, "plugin.phoneapp.env", nil); rpc.Code(err) != "forbidden" {
		t.Fatalf("owner, desktop-only method: %v", err)
	}
	got, err := coretest.Call(t, f.core, owner, "plugin.phoneapp.echo", map[string]any{"from": "phone"})
	if err != nil || !strings.Contains(string(got), "phone") {
		t.Fatalf("owner echo %s %v", got, err)
	}
	if _, err := coretest.Call(t, f.core, stranger, "plugins.list", nil); rpc.Code(err) != "not_owner" {
		t.Fatalf("stranger list: %v", err)
	}
	listed, err := coretest.Call(t, f.core, owner, "plugins.list", nil)
	if err != nil || !strings.Contains(string(listed), `"methods":["echo"]`) || strings.Contains(string(listed), "env") {
		t.Fatalf("owner list %s %v", listed, err)
	}
	if _, err := coretest.Call(t, f.core, owner, "core.plugins.list", nil); rpc.Code(err) != "forbidden" {
		t.Fatalf("phone reached core.plugins.list: %v", err)
	}
	f.call("plugin.phoneapp.emit", nil)
	if data := owner.Next(t, "plugin.phoneapp.changed", 5*time.Second); string(data) != `{"n":1}` {
		t.Fatalf("owner event %s", data)
	}
	select {
	case event := <-stranger.Events:
		t.Fatalf("stranger got %s", event.Name)
	case <-time.After(200 * time.Millisecond):
	}
}

func TestManifestRejectsUndeclaredRemoteMethod(t *testing.T) {
	service := Service{API: 1, Exec: "content/run.sh", Methods: []string{"a"}, RemoteMethods: []string{"b"}}
	if err := service.normalize(); err == nil || !errors.Is(err, ErrBadService) {
		t.Fatalf("got %v", err)
	}
	for _, exec := range []string{"../x", "/bin/sh", "content/../../x", "a\\b", ""} {
		bad := Service{API: 1, Exec: exec, Methods: []string{"a"}}
		if err := bad.normalize(); err == nil {
			t.Fatalf("exec %q accepted", exec)
		}
	}
	if err := (&Service{API: 2, Exec: "x", Methods: []string{"a"}}).normalize(); err == nil {
		t.Fatal("api 2 accepted")
	}
}
