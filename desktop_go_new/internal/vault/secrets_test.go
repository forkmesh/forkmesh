package vault

import (
	"bytes"
	"context"
	"encoding/json"
	"log"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"sort"
	"strings"
	"testing"
	"time"

	"forkmesh.com/desktop-core/internal/coretest"
	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/rpc"
)

const (
	testSealKey = "c2VhbC1rZXktc2VjcmV0LWJ5dGVzLTMyLWxvbmchIQ=="
	testGenie   = "genie-token-SECRET-42"
)

type secretsHarness struct {
	core   *daemon.Core
	module *Module
	logs   *lockedBuffer
	conf   map[string]string
	fail   error
	writes int
}

func (h *secretsHarness) writeConf(t *testing.T) {
	t.Helper()
	sections := map[string][]string{}
	for _, key := range SortedKeys(h.conf) {
		value := h.conf[key]
		section, name, _ := strings.Cut(key, "/")
		name = strings.ReplaceAll(name, "/", `\`)
		if key == SealKeySetting && !IsMarker(value) {
			value = "@ByteArray(" + value + ")"
		}
		sections[section] = append(sections[section], name+"="+quoteSetting(value))
	}
	names := SortedKeys(sections)
	sort.Strings(names)
	var b strings.Builder
	b.WriteString("[General]\nother=1\n")
	for _, section := range names {
		b.WriteString("\n[" + section + "]\n" + strings.Join(sections[section], "\n") + "\n")
	}
	path := h.core.Layout.SettingsFile()
	if err := os.MkdirAll(filepath.Dir(path), 0o700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, []byte(b.String()), 0o600); err != nil {
		t.Fatal(err)
	}
}

func newSecretsHarness(t *testing.T) *secretsHarness {
	t.Helper()
	core := coretest.New(t)
	logs := &lockedBuffer{}
	core.Logger = log.New(logs, "", 0)
	h := &secretsHarness{core: core, logs: logs, conf: map[string]string{}}
	id := testIdentity(t, 7)
	h.module = &Module{
		Finalize:   finalizeOn(),
		Identities: func() []Signer { return []Signer{id} },
		Interval:   time.Hour,
		WriteSettings: func(ctx context.Context, writes []daemon.SettingWrite) error {
			if h.fail != nil {
				return h.fail
			}
			h.writes++
			for _, write := range writes {
				if write.Type != "string" || write.Value != Marker {
					t.Errorf("unexpected write %s %s", write.Key, write.Type)
				}
				h.conf[write.Key] = Marker
			}
			h.writeConf(t)
			return nil
		},
	}
	coretest.Start(t, core, h.module)
	return h
}

func (h *secretsHarness) confText(t *testing.T) string {
	raw, err := os.ReadFile(h.core.Layout.SettingsFile())
	if err != nil {
		t.Fatal(err)
	}
	return string(raw)
}

func TestDesktopSecretsMoveIntoTheVaultOnce(t *testing.T) {
	h := newSecretsHarness(t)
	h.conf[SealKeySetting] = testSealKey
	h.conf[GenieTokenSetting] = testGenie
	h.conf["genie/org"] = "acme"
	h.writeConf(t)
	setAge(t, h.core.Layout.SettingsFile(), time.Now().Add(-time.Hour))
	if got := Secret(h.core, DesktopNamespace, SealKeySetting); got != testSealKey {
		t.Fatalf("before the migration the plaintext is read")
	}
	moved, err := h.module.MigrateDesktopSecrets(context.Background())
	if err != nil || moved != 2 {
		t.Fatalf("migrate: %d %v", moved, err)
	}
	text := h.confText(t)
	if strings.Contains(text, testSealKey) || strings.Contains(text, testGenie) || !strings.Contains(text, "acme") {
		t.Fatalf("ForkMesh.conf after the migration:\n%s", text)
	}
	if strings.Count(text, Marker) != 2 {
		t.Fatalf("both keys keep a marker:\n%s", text)
	}
	if Secret(h.core, DesktopNamespace, SealKeySetting) != testSealKey || Secret(h.core, DesktopNamespace, GenieTokenSetting) != testGenie {
		t.Fatal("the values come back from the vault")
	}
	raw, _ := os.ReadFile(h.module.store.Path)
	if bytes.Contains(raw, []byte(testGenie)) || bytes.Contains(raw, []byte(testSealKey)) {
		t.Fatal("plaintext in the vault file")
	}
	if logs := h.logs.String(); strings.Contains(logs, testGenie) || strings.Contains(logs, testSealKey) ||
		!strings.Contains(logs, "Moved 2 desktop secrets (chat/e2ee/x25519PrivateV2, genie/token)") {
		t.Fatalf("log: %s", logs)
	}
	if again, err := h.module.MigrateDesktopSecrets(context.Background()); err != nil || again != 0 || h.writes != 1 {
		t.Fatalf("second run is a no-op: %d %v writes=%d", again, err, h.writes)
	}
	ui := coretest.NewPeer("ui", rpc.UI)
	var got struct {
		Values map[string]string `json:"values"`
	}
	coretest.MustCall(t, h.core, ui, "core.vault.get", map[string]any{"namespace": DesktopNamespace, "names": []string{GenieTokenSetting}}, &got)
	if len(got.Values) != 1 || got.Values[GenieTokenSetting] != testGenie {
		t.Fatalf("core.vault.get desktop: %d values", len(got.Values))
	}
	if _, err := coretest.Call(t, h.core, coretest.NewPeer("cli", rpc.CLI), "core.vault.get", map[string]any{"namespace": DesktopNamespace}); rpc.Code(err) != "forbidden" {
		t.Fatalf("a shell reads desktop secrets: %v", err)
	}
	if _, err := coretest.Call(t, h.core, coretest.NewPeer("lan", rpc.LAN), "core.vault.get", map[string]any{"namespace": DesktopNamespace}); rpc.Code(err) != "forbidden" {
		t.Fatalf("a phone reads desktop secrets: %v", err)
	}
}

func TestDesktopSecretsResumeAndNewerPlaintextWins(t *testing.T) {
	h := newSecretsHarness(t)
	h.conf[GenieTokenSetting] = testGenie
	h.writeConf(t)
	setAge(t, h.core.Layout.SettingsFile(), time.Now().Add(-time.Hour))
	h.fail = rpc.Fail("desktop_unavailable")
	if _, err := h.module.MigrateDesktopSecrets(context.Background()); rpc.Code(err) != "desktop_unavailable" {
		t.Fatalf("no desktop: %v", err)
	}
	if !strings.Contains(h.confText(t), testGenie) || Secret(h.core, DesktopNamespace, GenieTokenSetting) != testGenie {
		t.Fatal("half way the settings are untouched and readers see the value")
	}
	h.fail = nil
	if moved, err := h.module.MigrateDesktopSecrets(context.Background()); err != nil || moved != 1 {
		t.Fatalf("resume: %d %v", moved, err)
	}
	h.conf[GenieTokenSetting] = "genie-rotated"
	h.writeConf(t)
	setAge(t, h.core.Layout.SettingsFile(), time.Now().Add(time.Minute))
	if got := Secret(h.core, DesktopNamespace, GenieTokenSetting); got != "genie-rotated" {
		t.Fatal("an older desktop's newer plaintext wins until it is migrated")
	}
	if moved, err := h.module.MigrateDesktopSecrets(context.Background()); err != nil || moved != 1 {
		t.Fatalf("second migration: %d %v", moved, err)
	}
	if got := Secret(h.core, DesktopNamespace, GenieTokenSetting); got != "genie-rotated" || strings.Contains(h.confText(t), "genie-rotated") {
		t.Fatal("the rotated value moved into the vault")
	}
	status, _ := coretest.Call(t, h.core, coretest.NewPeer("cli", rpc.CLI), "core.vault.status", nil)
	if !strings.Contains(string(status), `"pendingDesktop":0`) || !strings.Contains(string(status), `"pendingRelease":0`) {
		t.Fatalf("status: %s", status)
	}
}

func writeRelease(t *testing.T, h *secretsHarness, file, value string, mode os.FileMode) string {
	t.Helper()
	dir := ReleaseDir(h.core.Layout.Data)
	if err := os.MkdirAll(dir, 0o700); err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(dir, file)
	if err := os.WriteFile(path, []byte(value), mode); err != nil {
		t.Fatal(err)
	}
	if err := os.Chmod(path, mode); err != nil {
		t.Fatal(err)
	}
	setAge(t, path, time.Now().Add(-time.Hour))
	return path
}

func TestReleasePasswordsMoveIntoTheVault(t *testing.T) {
	h := newSecretsHarness(t)
	apk := writeRelease(t, h, ".apk-keystore-password", "apk-PASS-1\n", 0o600)
	win := writeRelease(t, h, ".windows-cert-password", "win-PASS-2", 0o600)
	pfx := writeRelease(t, h, "windows-authenticode.pfx", "binary-pfx", 0o600)
	moved, err := h.module.MigrateReleaseFiles()
	if err != nil || moved != 2 {
		t.Fatalf("migrate: %d %v", moved, err)
	}
	for _, path := range []string{apk, win} {
		raw, err := os.ReadFile(path)
		if err != nil || string(raw) != Marker+"\n" {
			t.Fatalf("%s keeps only the marker: %q %v", path, raw, err)
		}
		if info, _ := os.Stat(path); runtime.GOOS != "windows" && info.Mode().Perm() != 0o600 {
			t.Fatalf("%s mode %v", path, info.Mode().Perm())
		}
	}
	if raw, _ := os.ReadFile(pfx); string(raw) != "binary-pfx" {
		t.Fatal("the certificate container is not a password and stays a file")
	}
	if Secret(h.core, ReleaseNamespace, "apk-keystore-password") != "apk-PASS-1\n" || Secret(h.core, ReleaseNamespace, "windows-cert-password") != "win-PASS-2" {
		t.Fatal("the passwords come back from the vault")
	}
	raw, _ := os.ReadFile(h.module.store.Path)
	if bytes.Contains(raw, []byte("apk-PASS-1")) || strings.Contains(h.logs.String(), "PASS") {
		t.Fatal("a password reached the vault file or the log in plaintext")
	}
	if !strings.Contains(h.logs.String(), "Moved 2 release signing passwords (apk-keystore-password, windows-cert-password)") {
		t.Fatalf("log: %s", h.logs.String())
	}
	if again, err := h.module.MigrateReleaseFiles(); err != nil || again != 0 {
		t.Fatalf("second run: %d %v", again, err)
	}
	writeRelease(t, h, ".windows-cert-password", "win-NEW-3", 0o600)
	setAge(t, win, time.Now().Add(time.Minute))
	if got := Secret(h.core, ReleaseNamespace, "windows-cert-password"); got != "win-NEW-3" {
		t.Fatal("a password file the operator rewrote wins until it is migrated")
	}
	if moved, err := h.module.MigrateReleaseFiles(); err != nil || moved != 1 {
		t.Fatalf("re-migrate: %d %v", moved, err)
	}
	if got := Secret(h.core, ReleaseNamespace, "windows-cert-password"); got != "win-NEW-3" {
		t.Fatal("the rewritten password is in the vault")
	}
}

func TestReleaseMigrationLeavesLinksAlone(t *testing.T) {
	if runtime.GOOS == "windows" {
		t.Skip("symlinks")
	}
	h := newSecretsHarness(t)
	target := filepath.Join(t.TempDir(), "elsewhere")
	if err := os.WriteFile(target, []byte("linked-PASS"), 0o600); err != nil {
		t.Fatal(err)
	}
	dir := ReleaseDir(h.core.Layout.Data)
	if err := os.MkdirAll(dir, 0o700); err != nil {
		t.Fatal(err)
	}
	if err := os.Symlink(target, filepath.Join(dir, ".apk-keystore-password")); err != nil {
		t.Fatal(err)
	}
	if moved, err := h.module.MigrateReleaseFiles(); err != nil || moved != 0 {
		t.Fatalf("a symlinked password file is not migrated: %d %v", moved, err)
	}
	if raw, _ := os.ReadFile(target); string(raw) != "linked-PASS" {
		t.Fatal("the link target was rewritten")
	}
}

func TestPythonReleaseReaderReadsTheGoVault(t *testing.T) {
	python, err := exec.LookPath("python3")
	if err != nil {
		t.Skip("python3 is not installed")
	}
	if exec.Command(python, "-c", "import cryptography").Run() != nil {
		t.Skip("the Python cryptography package is not installed")
	}
	tools, err := filepath.Abs(filepath.Join("..", "..", "..", "tools"))
	if err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(filepath.Join(tools, "forkmesh_desktop_vault.py")); err != nil {
		t.Skip("tools/forkmesh_desktop_vault.py is not next to this module")
	}
	data := t.TempDir()
	pemPath, id := writePEM(t, t.TempDir(), 11)
	if err := os.MkdirAll(filepath.Join(data, "identity"), 0o700); err != nil {
		t.Fatal(err)
	}
	pemBytes, _ := os.ReadFile(pemPath)
	if err := os.WriteFile(filepath.Join(data, "identity", "ed25519.pem"), pemBytes, 0o600); err != nil {
		t.Fatal(err)
	}
	store := &Store{Path: StorePath(data), Signers: func() []Signer { return []Signer{id} }}
	put(t, store, map[string]string{
		EntryName(ReleaseNamespace, "apk-keystore-password"): "apk päss \"q\"\n",
		EntryName(DesktopNamespace, GenieTokenSetting):       testGenie,
	})
	release := filepath.Join(data, "release")
	if err := os.MkdirAll(release, 0o700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(release, ".apk-keystore-password"), []byte(Marker+"\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(release, ".windows-cert-password"), []byte(" plain-pass \n"), 0o600); err != nil {
		t.Fatal(err)
	}
	script := `
import json, sys
sys.path.insert(0, sys.argv[1])
import forkmesh_desktop_vault as v
from pathlib import Path
data = Path(sys.argv[2])
out = {
    "apk": v.release_secret(data / "release" / ".apk-keystore-password", "apk-keystore-password", data),
    "win": v.release_secret(data / "release" / ".windows-cert-password", "windows-cert-password", data),
    "desktop": sorted(v.read_namespace(v.vault_path(data), v.identity_path(data), "desktop")),
}
raw = Path(v.vault_path(data)).read_bytes()
doc = json.loads(raw)
doc["generation"] += 1
Path(sys.argv[3]).write_text(json.dumps(doc))
try:
    v.read_namespace(Path(sys.argv[3]), v.identity_path(data), "release")
    out["tamper"] = "opened"
except v.VaultError as error:
    out["tamper"] = error.code
print(json.dumps(out))
`
	out, err := exec.Command(python, "-c", script, tools, data, filepath.Join(t.TempDir(), "tampered.json")).CombinedOutput()
	if err != nil {
		t.Fatalf("python: %v\n%s", err, out)
	}
	var got map[string]any
	if err := json.Unmarshal(out, &got); err != nil {
		t.Fatalf("python output: %s", out)
	}
	if got["apk"] != "apk päss \"q\"" || got["win"] != "plain-pass" || got["tamper"] != "vault_tampered" {
		t.Fatalf("python reader: %v", got)
	}
	if names, _ := got["desktop"].([]any); len(names) != 1 || names[0] != GenieTokenSetting {
		t.Fatalf("python names: %v", got["desktop"])
	}
}
