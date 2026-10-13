package vault

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"log"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	"forkmesh.com/desktop-core/internal/coretest"
	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/rpc"
)

type lockedBuffer struct {
	mu sync.Mutex
	b  bytes.Buffer
}

func (l *lockedBuffer) Write(p []byte) (int, error) {
	l.mu.Lock()
	defer l.mu.Unlock()
	return l.b.Write(p)
}

func (l *lockedBuffer) String() string {
	l.mu.Lock()
	defer l.mu.Unlock()
	return l.b.String()
}

func quoteSetting(text string) string {
	return `"` + strings.ReplaceAll(strings.ReplaceAll(text, `\`, `\\`), `"`, `\"`) + `"`
}

func writeConf(t *testing.T, path string, variables map[string]any, extra string) {
	t.Helper()
	raw, err := json.Marshal(variables)
	if err != nil {
		t.Fatal(err)
	}
	text := "[General]\nother=1\n" + extra + "\n[actions]\nvariables=" + quoteSetting("@ByteArray("+string(raw)+")") + "\n"
	if err := os.MkdirAll(filepath.Dir(path), 0o700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, []byte(text), 0o600); err != nil {
		t.Fatal(err)
	}
}

func setAge(t *testing.T, path string, when time.Time) {
	t.Helper()
	if err := os.Chtimes(path, when, when); err != nil {
		t.Fatal(err)
	}
}

type harness struct {
	core   *daemon.Core
	module *Module
	logs   *lockedBuffer
	fail   error
	writes int
}

func newHarness(t *testing.T) *harness {
	t.Helper()
	core := coretest.New(t)
	logs := &lockedBuffer{}
	core.Logger = log.New(logs, "", 0)
	h := &harness{core: core, logs: logs}
	id := testIdentity(t, 9)
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
				if write.Key != VariablesSetting || write.Type != "json" {
					t.Errorf("unexpected write %s %s", write.Key, write.Type)
				}
				writeConf(t, core.Layout.SettingsFile(), write.Value.(map[string]any), "")
			}
			return nil
		},
	}
	coretest.Start(t, core, h.module)
	return h
}

func (h *harness) confText(t *testing.T) string {
	raw, err := os.ReadFile(h.core.Layout.SettingsFile())
	if err != nil {
		t.Fatal(err)
	}
	return string(raw)
}

var secrets = map[string]any{"CLOUDFLARE_API_TOKEN": "cf-secret-123", "ADMIN_PASS": "pa\"ss\\word", "MAC_BUILD_HOST": " mac.lan "}

func TestMigrationMovesEverySecretOnce(t *testing.T) {
	h := newHarness(t)
	conf := h.core.Layout.SettingsFile()
	writeConf(t, conf, secrets, "")
	setAge(t, conf, time.Now().Add(-time.Hour))
	if got := Variables(h.core); got["ADMIN_PASS"] != `pa"ss\word` || len(got) != 3 {
		t.Fatalf("before migration: %d names", len(got))
	}
	result, err := h.module.Migrate(context.Background())
	if err != nil || result["moved"] != 3 {
		t.Fatalf("migrate: %v %v", result, err)
	}
	text := h.confText(t)
	for _, value := range []string{"cf-secret-123", "ss", "mac.lan"} {
		if strings.Contains(text, value) {
			t.Fatalf("plaintext left in ForkMesh.conf")
		}
	}
	if !strings.Contains(text, Marker) || !strings.Contains(text, "CLOUDFLARE_API_TOKEN") {
		t.Fatal("the names stay in ForkMesh.conf with the marker")
	}
	got := Variables(h.core)
	if got["CLOUDFLARE_API_TOKEN"] != "cf-secret-123" || got["ADMIN_PASS"] != `pa"ss\word` || got["MAC_BUILD_HOST"] != " mac.lan " {
		t.Fatal("values must come back from the vault")
	}
	raw, _ := os.ReadFile(h.module.store.Path)
	if bytes.Contains(raw, []byte("cf-secret-123")) {
		t.Fatal("plaintext in the vault file")
	}
	again, err := h.module.Migrate(context.Background())
	if err != nil || again["moved"] != 0 || h.writes != 1 {
		t.Fatalf("second run must be a no-op: %v %v writes=%d", again, err, h.writes)
	}
	if strings.Contains(h.logs.String(), "cf-secret-123") || strings.Contains(h.logs.String(), "mac.lan") {
		t.Fatal("a value reached the log")
	}
	if !strings.Contains(h.logs.String(), "Moved 3 Actions variables") {
		t.Fatalf("missing log line: %s", h.logs.String())
	}
}

func TestMigrationResumesWhenTheDesktopIsAway(t *testing.T) {
	h := newHarness(t)
	conf := h.core.Layout.SettingsFile()
	writeConf(t, conf, secrets, "")
	setAge(t, conf, time.Now().Add(-time.Hour))
	h.fail = rpc.Fail("desktop_unavailable")
	if _, err := h.module.Migrate(context.Background()); err == nil {
		t.Fatal("migration must report the missing desktop")
	}
	if !h.module.store.Exists() || !strings.Contains(h.confText(t), "cf-secret-123") {
		t.Fatal("the vault holds the values and the settings are untouched")
	}
	if got := Variables(h.core); got["CLOUDFLARE_API_TOKEN"] != "cf-secret-123" || len(got) != 3 {
		t.Fatal("readers see the same values half way")
	}
	status, _ := coretest.Call(t, h.core, coretest.NewPeer("cli", rpc.CLI), "core.vault.status", nil)
	if !strings.Contains(string(status), `"pending":3`) || !strings.Contains(string(status), "desktop_unavailable") {
		t.Fatalf("status: %s", status)
	}
	generation := h.module.store.Status().Generation
	h.fail = nil
	if result, err := h.module.Migrate(context.Background()); err != nil || result["moved"] != 3 {
		t.Fatalf("resume: %v %v", result, err)
	}
	if h.module.store.Status().Generation != generation {
		t.Fatal("resuming must not rewrite identical values")
	}
	if strings.Contains(h.confText(t), "cf-secret-123") {
		t.Fatal("plaintext left after resume")
	}
}

func TestOlderDesktopWriteIsImportedAndNewerVaultWins(t *testing.T) {
	h := newHarness(t)
	conf := h.core.Layout.SettingsFile()
	writeConf(t, conf, secrets, "")
	setAge(t, conf, time.Now().Add(-time.Hour))
	if _, err := h.module.Migrate(context.Background()); err != nil {
		t.Fatal(err)
	}
	writeConf(t, conf, map[string]any{"CLOUDFLARE_API_TOKEN": "cf-rotated", "ADMIN_PASS": Marker, "MAC_BUILD_HOST": Marker}, "")
	setAge(t, conf, time.Now().Add(time.Minute))
	if got := Variables(h.core); got["CLOUDFLARE_API_TOKEN"] != "cf-rotated" || got["ADMIN_PASS"] != `pa"ss\word` {
		t.Fatal("a newer plaintext write wins until it is migrated")
	}
	if _, err := h.module.Migrate(context.Background()); err != nil {
		t.Fatal(err)
	}
	if got := Variables(h.core); got["CLOUDFLARE_API_TOKEN"] != "cf-rotated" || strings.Contains(h.confText(t), "cf-rotated") {
		t.Fatal("the older desktop's value moved into the vault")
	}
	if _, err := h.module.Apply(ActionsNamespace, map[string]string{"CLOUDFLARE_API_TOKEN": "cf-newest"}, nil, false); err != nil {
		t.Fatal(err)
	}
	writeConf(t, conf, map[string]any{"CLOUDFLARE_API_TOKEN": "cf-stale", "ADMIN_PASS": Marker, "MAC_BUILD_HOST": Marker}, "")
	setAge(t, conf, time.Now().Add(-time.Hour))
	if got := Variables(h.core); got["CLOUDFLARE_API_TOKEN"] != "cf-newest" {
		t.Fatal("a vault value newer than the settings file wins")
	}
	if _, err := h.module.Migrate(context.Background()); err != nil {
		t.Fatal(err)
	}
	if got := Variables(h.core); got["CLOUDFLARE_API_TOKEN"] != "cf-newest" || strings.Contains(h.confText(t), "cf-stale") {
		t.Fatal("migration must not overwrite a newer vault value, and still clears the plaintext")
	}
}

func TestDesktopMethodsAndPeers(t *testing.T) {
	h := newHarness(t)
	ui := coretest.NewPeer("ui", rpc.UI)
	cli := coretest.NewPeer("cli", rpc.CLI)
	h.core.Hub.Add(ui)
	for _, transport := range []rpc.Transport{rpc.LAN, rpc.Relay} {
		phone := coretest.NewPeer("phone", transport)
		for _, method := range []string{"core.vault.status", "core.vault.list", "core.vault.get", "core.vault.set", "core.vault.delete", "core.vault.migrate", "core.vault.rotate"} {
			if _, err := coretest.Call(t, h.core, phone, method, map[string]any{}); err == nil || err.Error() != "forbidden" {
				t.Fatalf("%s over %s: %v", method, transport, err)
			}
		}
	}
	for _, method := range []string{"core.vault.get", "core.vault.set", "core.vault.delete"} {
		if _, err := coretest.Call(t, h.core, cli, method, map[string]any{}); err == nil || err.Error() != "forbidden" {
			t.Fatalf("%s from an unverified local client: %v", method, err)
		}
	}
	var set struct {
		OK      bool     `json:"ok"`
		Changed []string `json:"changed"`
	}
	coretest.MustCall(t, h.core, ui, "core.vault.set", map[string]any{"namespace": "actions", "values": map[string]string{"A": "1", "B c": "2"}}, &set)
	if !set.OK || len(set.Changed) != 2 {
		t.Fatalf("set %+v", set)
	}
	event := ui.Next(t, "vault.changed", time.Second)
	if strings.Contains(string(event), `"1"`) || !strings.Contains(string(event), `"B c"`) {
		t.Fatalf("event carries names only: %s", event)
	}
	coretest.MustCall(t, h.core, ui, "core.vault.set", map[string]any{"values": map[string]string{"A": "1", "B c": "2"}}, &set)
	if len(set.Changed) != 0 {
		t.Fatal("an unchanged value is not rewritten")
	}
	var got struct {
		Values map[string]string `json:"values"`
	}
	coretest.MustCall(t, h.core, ui, "core.vault.get", map[string]any{"namespace": "actions", "names": []string{"B c", "missing"}}, &got)
	if len(got.Values) != 1 || got.Values["B c"] != "2" {
		t.Fatalf("get %v", got.Values)
	}
	var list struct {
		Names []string `json:"names"`
	}
	coretest.MustCall(t, h.core, cli, "core.vault.list", map[string]any{"namespace": "actions"}, &list)
	if strings.Join(list.Names, ",") != "A,B c" {
		t.Fatalf("list %v", list.Names)
	}
	coretest.MustCall(t, h.core, ui, "core.vault.set", map[string]any{"values": map[string]string{"C": "3"}, "replace": true}, &set)
	if strings.Join(set.Changed, ",") != "A,B c,C" {
		t.Fatalf("replace %v", set.Changed)
	}
	coretest.MustCall(t, h.core, ui, "core.vault.delete", map[string]any{"names": []string{"C"}}, &set)
	got.Values = nil
	coretest.MustCall(t, h.core, ui, "core.vault.get", map[string]any{}, &got)
	if len(got.Values) != 0 {
		t.Fatalf("after delete %v", got.Values)
	}
	if _, err := coretest.Call(t, h.core, ui, "core.vault.set", map[string]any{"namespace": "Bad NS"}); err == nil || err.Error() != "bad_request" {
		t.Fatalf("namespace check: %v", err)
	}
	if _, err := coretest.Call(t, h.core, ui, "core.vault.set", map[string]any{"values": map[string]string{"X": strings.Repeat("x", MaxValueBytes+1)}}); err == nil || err.Error() != "value_too_large" {
		t.Fatalf("size check: %v", err)
	}
	var rotated struct {
		KeyID string `json:"keyId"`
	}
	coretest.MustCall(t, h.core, cli, "core.vault.rotate", nil, &rotated)
	if rotated.KeyID == "" {
		t.Fatal("rotate returns the new key id")
	}
}

func TestLockedVaultFallsBackToPlaintextOnly(t *testing.T) {
	h := newHarness(t)
	if _, err := h.module.Apply(ActionsNamespace, map[string]string{"A": "1"}, nil, false); err != nil {
		t.Fatal(err)
	}
	stranger := testIdentity(t, 3)
	h.module.Identities = func() []Signer { return []Signer{stranger} }
	writeConf(t, h.core.Layout.SettingsFile(), map[string]any{"A": Marker, "B": "plain"}, "")
	got := Variables(h.core)
	if len(got) != 1 || got["B"] != "plain" {
		t.Fatalf("locked vault: %v", SortedKeys(got))
	}
	if _, err := coretest.Call(t, h.core, coretest.NewPeer("ui", rpc.UI), "core.vault.get", nil); err == nil || err.Error() != "vault_locked" {
		t.Fatalf("get on a locked vault: %v", err)
	}
	if _, err := h.module.Migrate(context.Background()); !errors.Is(err, ErrIdentityMismatch) {
		t.Fatalf("migration into a locked vault: %v", err)
	}
	if !strings.Contains(h.confText(t), "plain") {
		t.Fatal("a failed migration leaves the plaintext in place")
	}
}

func TestVariablesWithoutTheModule(t *testing.T) {
	core := coretest.New(t)
	writeConf(t, core.Layout.SettingsFile(), map[string]any{"A": Marker, "B": "2"}, "")
	got := Variables(core)
	if len(got) != 1 || got["B"] != "2" {
		t.Fatalf("plaintext reader drops markers: %v", SortedKeys(got))
	}
}
