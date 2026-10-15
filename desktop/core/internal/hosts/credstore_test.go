package hosts

import (
	"bytes"
	"context"
	"encoding/base64"
	"encoding/json"
	"fmt"
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
	"forkmesh.com/desktop-core/internal/vault"
)

func TestUpdatedEnvAssignment(t *testing.T) {
	cases := []struct{ in, name, value, want string }{
		{"A=1\r\nKEY=old\r\n  KEY =dup\nB=2\n", "KEY", "new", "A=1\r\nKEY=new\r\nB=2\n"},
		{"A=1\n\n\n", "KEY", "v", "A=1\nKEY=v\n"},
		{"", "KEY", "v", "KEY=v\n"},
		{"A=1", "bad-name", "v", "A=1"},
		{"A=1", "KEY", "two\nlines", "A=1"},
		{"KEYS=1\n", "KEY", "v", "KEYS=1\nKEY=v\n"},
	}
	for _, c := range cases {
		if got := updatedEnvAssignment(c.in, c.name, c.value); got != c.want {
			t.Fatalf("%q %s=%s: %q, want %q", c.in, c.name, c.value, got, c.want)
		}
	}
}

func TestWriteEnvAssignments(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, ".env.production")
	changed, failure := writeEnvAssignments(path, map[string]string{"B": "2", "A": "1"})
	if !changed || failure != "" {
		t.Fatalf("create %v %q", changed, failure)
	}
	data, _ := os.ReadFile(path)
	info, _ := os.Stat(path)
	if string(data) != "A=1\nB=2\n" || (runtime.GOOS != "windows" && info.Mode().Perm() != 0o600) {
		t.Fatalf("file %q %v", data, info.Mode())
	}
	if changed, failure = writeEnvAssignments(path, map[string]string{"A": "1"}); changed || failure != "" {
		t.Fatalf("unchanged %v %q", changed, failure)
	}
	entries, _ := os.ReadDir(dir)
	if len(entries) != 1 {
		t.Fatalf("temp files left: %v", entries)
	}
	if runtime.GOOS != "windows" {
		link := filepath.Join(dir, "link.env")
		_ = os.Symlink(path, link)
		if _, failure = writeEnvAssignments(link, map[string]string{"A": "2"}); failure != link+" is not a regular file." {
			t.Fatalf("symlink %q", failure)
		}
	}
	big := filepath.Join(dir, "big.env")
	_ = os.WriteFile(big, bytes.Repeat([]byte("x"), envFileCap+1), 0o600)
	if _, failure = writeEnvAssignments(big, map[string]string{"A": "2"}); failure != big+" is unexpectedly large." {
		t.Fatalf("large %q", failure)
	}
}

func fakeWorkerTree(t *testing.T) (string, string) {
	t.Helper()
	root := t.TempDir()
	for _, name := range append([]string{"tools/" + bootstrapScriptName}, func() []string {
		out := []string{}
		for _, file := range workerBundleFiles {
			out = append(out, "app/"+file)
		}
		return out
	}()...) {
		path := filepath.Join(root, name)
		_ = os.MkdirAll(filepath.Dir(path), 0o700)
		if err := os.WriteFile(path, []byte("x"), 0o600); err != nil {
			t.Fatal(err)
		}
	}
	for _, name := range workerBundleFolders {
		_ = os.MkdirAll(filepath.Join(root, "app", name), 0o700)
	}
	resolved, _ := filepath.EvalSymlinks(root)
	return filepath.Join(root, "desktop"), filepath.Join(resolved, "app", deployEnvFileName)
}

func TestSiteDeployEnvFilePath(t *testing.T) {
	t.Setenv("FORKMESH_CLOUDFLARE_BOOTSTRAP", "")
	t.Setenv("FORKMESH_RESOURCE_DIR", "")
	source, want := fakeWorkerTree(t)
	if got := siteDeployEnvFilePath(source, ""); got != want {
		t.Fatalf("got %q want %q", got, want)
	}
	if err := os.Remove(filepath.Join(filepath.Dir(want), "src", "entry.py")); err != nil {
		t.Fatal(err)
	}
	if got := siteDeployEnvFilePath(source, ""); got != "" {
		t.Fatalf("incomplete bundle accepted: %q", got)
	}
}

type credFixture struct {
	core   *daemon.Core
	mod    *Module
	ui     *coretest.Peer
	envOut string
	mu     sync.Mutex
	writes []daemon.SettingWrite
}

func newCredFixture(t *testing.T, withVault bool, plaintext map[string]string) *credFixture {
	t.Helper()
	t.Setenv("FORKMESH_CLOUDFLARE_BOOTSTRAP", "")
	t.Setenv("FORKMESH_RESOURCE_DIR", "")
	core := coretest.New(t)
	writeSettings(t, core, defaultHosts(), plaintext)
	stamp := time.Now().Add(-time.Hour)
	_ = os.Chtimes(core.Layout.SettingsFile(), stamp, stamp)
	source, envPath := fakeWorkerTree(t)
	f := &credFixture{core: core, envOut: envPath}
	f.mod = New()
	f.mod.Owners = func() []string { return []string{"alice"} }
	f.mod.Attached = func() bool { return true }
	f.mod.WriteSettings = func(ctx context.Context, writes []daemon.SettingWrite) error {
		f.mu.Lock()
		f.writes = append(f.writes, writes...)
		f.mu.Unlock()
		return nil
	}
	f.mod.CallUI = func(ctx context.Context, method string, params any) ([]byte, error) {
		return nil, rpc.Fail("desktop_unavailable")
	}
	modules := []daemon.Module{}
	if withVault {
		id, err := identity.FromSeed(bytes.Repeat([]byte{7}, 32))
		if err != nil {
			t.Fatal(err)
		}
		modules = append(modules, &vault.Module{Identities: func() []vault.Signer { return []vault.Signer{id} }, Interval: time.Hour})
	}
	modules = append(modules, f.mod)
	coretest.Start(t, core, modules...)
	f.mod.storeEnv(hostEnv{SourceDir: source})
	f.ui = coretest.NewPeer("ui", rpc.UI)
	core.Hub.Add(f.ui)
	return f
}

func (f *credFixture) settingsText() string {
	f.mu.Lock()
	defer f.mu.Unlock()
	raw, _ := json.Marshal(f.writes)
	return string(raw)
}

func TestVultrKeyGoesToTheVaultAndDeployEnv(t *testing.T) {
	f := newCredFixture(t, true, map[string]string{"VULTR_TOKEN": "old-plaintext", "OTHER": "kept"})
	stores, failure := f.mod.storeVultrKey(" NEWKEY123 ")
	if failure != "" || len(stores) != 2 || stores[0] != "this device's VULTR_API_KEY variable" || stores[1] != f.envOut {
		t.Fatalf("stores %v %q", stores, failure)
	}
	values := vault.Variables(f.core)
	if values["VULTR_API_KEY"] != "NEWKEY123" || values["VULTR_TOKEN"] != "NEWKEY123" || values["OTHER"] != "kept" {
		t.Fatalf("vault %v", values)
	}
	written := f.settingsText()
	if strings.Contains(written, "NEWKEY123") || strings.Contains(written, "old-plaintext") || !strings.Contains(written, vault.Marker) || !strings.Contains(written, `"OTHER":"kept"`) {
		t.Fatalf("settings writes %s", written)
	}
	envFile, _ := os.ReadFile(f.envOut)
	if string(envFile) != "VULTR_API_KEY=NEWKEY123\n" {
		t.Fatalf("env %q", envFile)
	}
	event := decode(t, f.ui.Next(t, "core.hosts.variablesChanged", 5*time.Second))
	if fmt.Sprint(event["names"]) != "[VULTR_API_KEY VULTR_TOKEN]" {
		t.Fatalf("event %v", event)
	}
	if stores, _ = f.mod.storeVultrKey("NEWKEY123"); stores != nil {
		t.Fatalf("remembered key stored again: %v", stores)
	}
	stores, failure = f.mod.rememberCloudflareToken(context.Background(), "cf-token-new")
	if failure != "" || len(stores) != 2 || stores[0] != "this device's CLOUDFLARE_API_TOKEN variable" {
		t.Fatalf("cloudflare %v %q", stores, failure)
	}
	if vault.Variable(f.core, "CLOUDFLARE_API_TOKEN") != "cf-token-new" || strings.Contains(f.settingsText(), "cf-token-new") {
		t.Fatalf("cloudflare token not in the vault only")
	}
	envFile, _ = os.ReadFile(f.envOut)
	if string(envFile) != "VULTR_API_KEY=NEWKEY123\nCLOUDFLARE_API_TOKEN=cf-token-new\n" {
		t.Fatalf("env %q", envFile)
	}
}

func TestKeysAreNotWrittenInPlaintextWithoutTheVault(t *testing.T) {
	f := newCredFixture(t, false, nil)
	stores, failure := f.mod.storeVultrKey("KEY-ONE")
	if len(stores) != 1 || stores[0] != f.envOut || !strings.Contains(failure, "(vault_unavailable)") {
		t.Fatalf("stores %v %q", stores, failure)
	}
	if strings.Contains(f.settingsText(), "KEY-ONE") {
		t.Fatalf("plaintext write %s", f.settingsText())
	}
	if stores, _ = f.mod.storeVultrKey("KEY-ONE"); len(stores) != 0 {
		t.Fatalf("env rewritten: %v", stores)
	}
	f.mod.mu.Lock()
	kept := f.mod.vultrKept
	f.mod.mu.Unlock()
	if kept != "" {
		t.Fatalf("a key the vault refused was remembered")
	}
	f.mod.storeEnv(hostEnv{SourceDir: filepath.Join(t.TempDir(), "nowhere")})
	if _, failure = f.mod.rememberCloudflareToken(context.Background(), "cf"); !strings.Contains(failure, deployEnvMissing) {
		t.Fatalf("missing env %q", failure)
	}
}

func TestAgentLoginsReadByTheCore(t *testing.T) {
	core := coretest.New(t)
	claudeDefault := t.TempDir()
	profile := t.TempDir()
	codex := t.TempDir()
	t.Setenv("CLAUDE_CONFIG_DIR", claudeDefault)
	t.Setenv("CODEX_HOME", codex)
	writeSettings(t, core, defaultHosts(), nil)
	conf, _ := os.ReadFile(core.Layout.SettingsFile())
	conf = append(conf, []byte("\n[agents]\nactiveAccount\\claude=work\naccountProfiles\\claude\\work\\configDir="+quoteSetting(profile)+"\nclaudeApiKey=sk-ant\n")...)
	_ = os.WriteFile(core.Layout.SettingsFile(), conf, 0o600)
	mod := New()
	coretest.Start(t, core, mod)
	_ = os.WriteFile(filepath.Join(claudeDefault, ".credentials.json"), []byte(`{"who":"default"}`), 0o600)
	if got := mod.agentCredentials(); got.Summary != "ANTHROPIC_API_KEY" || strings.Contains(string(got.Payload), base64.StdEncoding.EncodeToString([]byte(`{"who":"default"}`))) {
		t.Fatalf("the default account was read while %q is active: %+v", "work", got.Summary)
	}
	_ = os.WriteFile(filepath.Join(profile, ".credentials.json"), []byte(`{"who":"work"}`+"\n"), 0o600)
	_ = os.WriteFile(filepath.Join(codex, "auth.json"), []byte(`["not","an","object"]`), 0o600)
	got := mod.agentCredentials()
	if got.Empty || got.Error != "This device's Codex login file is not readable as a credential document." || got.Payload != nil {
		t.Fatalf("bad codex %+v", got)
	}
	_ = os.WriteFile(filepath.Join(codex, "auth.json"), []byte(`{"tokens":{}}`), 0o600)
	got = mod.agentCredentials()
	want := "claude " + base64.StdEncoding.EncodeToString([]byte(`{"who":"work"}`)) + "\ncodex " + base64.StdEncoding.EncodeToString([]byte(`{"tokens":{}}`)) + "\n"
	if got.Error != "" || string(got.Payload) != want || got.Summary != "Claude Code login, Codex login" {
		t.Fatalf("payload %q %+v", got.Payload, got.Summary)
	}
	_ = os.Remove(filepath.Join(profile, ".credentials.json"))
	got = mod.agentCredentials()
	if got.Summary != "Codex login, ANTHROPIC_API_KEY" || !strings.Contains(string(got.Payload), "env "+base64.StdEncoding.EncodeToString([]byte("export ANTHROPIC_API_KEY='sk-ant'\n"))) {
		t.Fatalf("api key fallback %q %q", got.Summary, got.Payload)
	}
	if usable := agentEnvEntryUsable("lower", "x") || agentEnvEntryUsable("OK", "tab\there") || agentEnvEntryUsable("OK", strings.Repeat("x", 4097)); usable {
		t.Fatal("env entry validation")
	}
	access, summary := mod.agentAccess()
	if !access || summary != "Codex login, ANTHROPIC_API_KEY" {
		t.Fatalf("access %v %q", access, summary)
	}
}
