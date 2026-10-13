package actions

import (
	"bytes"
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"forkmesh.com/desktop-core/internal/coretest"
	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/identity"
	"forkmesh.com/desktop-core/internal/qsettings"
	"forkmesh.com/desktop-core/internal/rpc"
	"forkmesh.com/desktop-core/internal/vault"
)

const oldSecret = "old-secret-value-1234"

func vaultFixture(t *testing.T, seed byte) *vault.Store {
	t.Helper()
	id, err := identity.FromSeed(bytes.Repeat([]byte{seed}, 32))
	if err != nil {
		t.Fatal(err)
	}
	return &vault.Store{Path: filepath.Join(t.TempDir(), "vault", "secrets.json"), Signers: func() []vault.Signer { return []vault.Signer{id} }}
}

func vaultValues(t *testing.T, store *vault.Store, namespace string) map[string]string {
	t.Helper()
	out := map[string]string{}
	if err := store.View(func(tx *vault.Tx) error {
		for _, entry := range tx.Names(namespace + "/") {
			name, _ := vault.NameOf(namespace, entry)
			value, _, err := tx.Get(entry)
			if err != nil {
				return err
			}
			out[name] = string(value)
		}
		return nil
	}); err != nil {
		t.Fatal(err)
	}
	return out
}

func sealedMirrorFixture(t *testing.T) (MirrorConfigEnv, *iniStore, *vault.Store, *[][]string) {
	env, ini, _ := mirrorConfigFixture(t)
	ini.set(t, "actions/variables", iniQuote(`@ByteArray({"OLD":"`+oldSecret+`"})`))
	store := vaultFixture(t, 7)
	env.Vault = func() *vault.Store { return store }
	changed := &[][]string{}
	env.VaultChanged = func(names []string) { *changed = append(*changed, names) }
	return env, ini, store, changed
}

func assertNoPlaintext(t *testing.T, path string, secrets ...string) {
	t.Helper()
	raw, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	for _, secret := range secrets {
		if strings.Contains(string(raw), secret) {
			t.Fatalf("%s holds a plaintext secret:\n%s", path, raw)
		}
	}
}

func TestMirrorConfigurationSealsVariablesInTheVault(t *testing.T) {
	env, _, store, changed := sealedMirrorFixture(t)
	id := strings.Repeat("d", 32)
	got := ApplyMirrorConfiguration(mirrorRequest(id), env)
	if got["ok"] != true || got["variablesReplaced"] != true || got["variableCount"] != 1 {
		t.Fatal(got)
	}
	assertNoPlaintext(t, env.SettingsPath, "s3cret", oldSecret)
	settings, _ := qsettings.Load(env.SettingsPath)
	plain := vault.PlaintextVariables(settings)
	if len(plain) != 1 || plain["TOKEN"] != vault.Marker {
		t.Fatalf("settings keep %v", plain)
	}
	if values := vaultValues(t, store, vault.ActionsNamespace); len(values) != 1 || values["TOKEN"] != "s3cret" {
		t.Fatalf("vault holds %v", values)
	}
	for _, namespace := range []string{mirrorRollbackNamespace, mirrorTxnNamespace} {
		if left := vaultValues(t, store, namespace); len(left) != 0 {
			t.Fatalf("%s left after the commit: %v", namespace, left)
		}
	}
	if len(*changed) == 0 {
		t.Fatal("no vault.changed names")
	}
	raw, _ := os.ReadFile(store.Path)
	if bytes.Contains(raw, []byte("s3cret")) {
		t.Fatal("the vault file holds plaintext")
	}
}

func TestMirrorConfigurationVaultRollsBackWithTheSettings(t *testing.T) {
	env, _, store, _ := sealedMirrorFixture(t)
	healthy := env.Write
	calls := 0
	env.Write = func(ctx context.Context, writes []daemon.SettingWrite) error {
		calls++
		if calls == 1 {
			assertNoPlaintext(t, env.JournalPath(), "s3cret", oldSecret)
			if values := vaultValues(t, store, vault.ActionsNamespace); values["TOKEN"] != "s3cret" {
				t.Fatalf("not sealed before the settings write: %v", values)
			}
			return rpc.Fail("desktop_unavailable")
		}
		return healthy(ctx, writes)
	}
	got := ApplyMirrorConfiguration(mirrorRequest(strings.Repeat("e", 32)), env)
	if got["ok"] != false || got["errorCode"] != "settings_write_failed" {
		t.Fatal(got)
	}
	if values := vaultValues(t, store, vault.ActionsNamespace); len(values) != 1 || values["OLD"] != oldSecret {
		t.Fatalf("vault not rolled back: %v", values)
	}
	settings, _ := qsettings.Load(env.SettingsPath)
	if plain := vault.PlaintextVariables(settings); len(plain) != 1 || plain["OLD"] != vault.Marker {
		t.Fatalf("settings after rollback: %v", plain)
	}
	assertNoPlaintext(t, env.SettingsPath, "s3cret", oldSecret)
	if pathExists(env.JournalPath()) || len(vaultValues(t, store, mirrorTxnNamespace)) != 0 {
		t.Fatal("journal or transaction left behind")
	}
}

func TestMirrorVaultTransactionSettlesByGeneration(t *testing.T) {
	env, ini, store, _ := sealedMirrorFixture(t)
	ini.set(t, mirrorGenerationSetting, iniQuote(strings.Repeat("1", 32)))
	settings, _ := qsettings.Load(env.SettingsPath)
	interrupted := strings.Repeat("2", 32)
	if _, err := env.sealMirrorVariables(settings, interrupted, map[string]string{"NEW": "n"}); err != nil {
		t.Fatal(err)
	}
	if code := env.RecoverPending(); code != "" {
		t.Fatal(code)
	}
	if values := vaultValues(t, store, vault.ActionsNamespace); len(values) != 1 || values["OLD"] != oldSecret {
		t.Fatalf("an apply whose settings never landed was kept: %v", values)
	}
	settings, _ = qsettings.Load(env.SettingsPath)
	committed := strings.Repeat("3", 32)
	if _, err := env.sealMirrorVariables(settings, committed, map[string]string{"NEW": "n"}); err != nil {
		t.Fatal(err)
	}
	ini.set(t, mirrorGenerationSetting, iniQuote(committed))
	if code := env.RecoverPending(); code != "" {
		t.Fatal(code)
	}
	if values := vaultValues(t, store, vault.ActionsNamespace); len(values) != 1 || values["NEW"] != "n" {
		t.Fatalf("a committed apply was rolled back: %v", values)
	}
	if len(vaultValues(t, store, mirrorRollbackNamespace)) != 0 {
		t.Fatal("rollback copy kept after settling")
	}
}

func TestMirrorConfigurationRefusesPlaintextWhenTheVaultIsLocked(t *testing.T) {
	env, _, store, _ := sealedMirrorFixture(t)
	if err := store.Update(func(tx *vault.Tx) error { return tx.Set(vault.EntryName(vault.ActionsNamespace, "X"), []byte("x")) }); err != nil {
		t.Fatal(err)
	}
	other := vaultFixture(t, 9)
	other.Path = store.Path
	env.Vault = func() *vault.Store { return other }
	before, _ := os.ReadFile(env.SettingsPath)
	got := ApplyMirrorConfiguration(mirrorRequest(strings.Repeat("f", 32)), env)
	if got["ok"] != false || got["errorCode"] != "variables_vault_unavailable" {
		t.Fatal(got)
	}
	after, _ := os.ReadFile(env.SettingsPath)
	if !bytes.Equal(before, after) || pathExists(env.JournalPath()) {
		t.Fatal("a refused apply changed the settings or left a journal")
	}
	noVariables := mirrorRequest(strings.Repeat("a", 32))
	delete(noVariables, "variables")
	if got := ApplyMirrorConfiguration(noVariables, env); got["ok"] != true {
		t.Fatalf("an apply without variables needs no vault: %v", got)
	}
}

func TestMirrorConfigurationWaitsForADesktopToWriteSettings(t *testing.T) {
	core := coretest.New(t)
	module := New()
	module.Root = filepath.Join(t.TempDir(), "actions")
	coretest.Start(t, core, module)
	for deadline := time.Now().Add(5 * time.Second); !dirExists(filepath.Join(module.Root, "sandbox")) && time.Now().Before(deadline); {
		time.Sleep(10 * time.Millisecond)
	}
	cli := coretest.NewPeer("cli", rpc.CLI)
	for _, method := range []string{"core.actions.mirrorConfig.apply", "core.actions.mirrorConfig.recover"} {
		if _, err := coretest.Call(t, core, cli, method, mirrorRequest(strings.Repeat("a", 32))); rpc.Code(err) != "desktop_unavailable" {
			t.Fatalf("%s without a desktop: %v", method, err)
		}
	}
	phone := coretest.NewPeer("phone", rpc.LAN)
	if _, err := coretest.Call(t, core, phone, "core.actions.mirrorConfig.apply", mirrorRequest(strings.Repeat("a", 32))); rpc.Code(err) != "forbidden" {
		t.Fatalf("phone: %v", err)
	}
}
