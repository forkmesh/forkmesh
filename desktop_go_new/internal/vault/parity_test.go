package vault

import (
	"bytes"
	"crypto/ed25519"
	"crypto/x509"
	"encoding/json"
	"encoding/pem"
	"os"
	"os/exec"
	"path/filepath"
	"testing"

	"forkmesh.com/desktop-core/internal/identity"
)

func parityBinary(t *testing.T) string {
	binary := os.Getenv("FORKMESH_VAULT_PARITY_BIN")
	if binary == "" {
		t.Skip("set FORKMESH_VAULT_PARITY_BIN to a build of testdata/parity_harness.cpp with desktop/src/VaultFile.cpp")
	}
	return binary
}

func writePEM(t *testing.T, dir string, fill byte) (string, *identity.Identity) {
	t.Helper()
	seed := bytes.Repeat([]byte{fill}, 32)
	der, err := x509.MarshalPKCS8PrivateKey(ed25519.NewKeyFromSeed(seed))
	if err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(dir, "ed25519.pem")
	if err := os.WriteFile(path, pem.EncodeToMemory(&pem.Block{Type: "PRIVATE KEY", Bytes: der}), 0o600); err != nil {
		t.Fatal(err)
	}
	id, err := identity.FromSeed(seed)
	if err != nil {
		t.Fatal(err)
	}
	return path, id
}

func runHarness(t *testing.T, binary string, args ...string) map[string]any {
	t.Helper()
	out, err := exec.Command(binary, args...).Output()
	if err != nil {
		t.Fatalf("harness: %v", err)
	}
	reply := map[string]any{}
	if err := json.Unmarshal(out, &reply); err != nil {
		t.Fatalf("harness output: %v", err)
	}
	return reply
}

func TestCppReaderParity(t *testing.T) {
	binary := parityBinary(t)
	dir := t.TempDir()
	pemPath, id := writePEM(t, dir, 5)
	store := &Store{Path: filepath.Join(dir, "vault", "secrets.json"), Signers: func() []Signer { return []Signer{id} }}
	values := map[string]string{"CLOUDFLARE_API_TOKEN": "cf", "EMPTY": "", "my var/é": "ü\"\\\n", "BIG": string(bytes.Repeat([]byte("x"), 70000))}
	if err := store.Update(func(tx *Tx) error {
		for name, value := range values {
			if err := tx.Set(EntryName(ActionsNamespace, name), []byte(value)); err != nil {
				return err
			}
		}
		return tx.Set(EntryName("other", "X"), []byte("y"))
	}); err != nil {
		t.Fatal(err)
	}
	reply := runHarness(t, binary, store.Path, pemPath, ActionsNamespace)
	got, _ := reply["values"].(map[string]any)
	if len(got) != len(values) {
		t.Fatalf("C++ read %d values (%v), want %d", len(got), reply["error"], len(values))
	}
	var at int64
	_ = store.View(func(tx *Tx) error { at = tx.At(EntryName(ActionsNamespace, "EMPTY")); return nil })
	for name, want := range values {
		entry, _ := got[name].(map[string]any)
		if entry == nil || entry["value"] != want {
			t.Fatalf("C++ value for %q differs", name)
		}
		if int64(entry["at"].(float64)) != at {
			t.Fatalf("C++ timestamp for %q differs", name)
		}
	}
	for _, name := range []string{"plain", "my var/é", "a~b.c-d_e", "100%"} {
		out, err := exec.Command(binary, "-", "-", ActionsNamespace, name).Output()
		if err != nil || string(out) != EntryName(ActionsNamespace, name) {
			t.Fatalf("entry name for %q: C++ %q, Go %q", name, out, EntryName(ActionsNamespace, name))
		}
	}
	if _, err := store.Rotate(); err != nil {
		t.Fatal(err)
	}
	if reply := runHarness(t, binary, store.Path, pemPath, ActionsNamespace); len(reply["values"].(map[string]any)) != len(values) {
		t.Fatal("C++ reads a rotated vault")
	}
	otherPEM, _ := writePEM(t, t.TempDir(), 6)
	if reply := runHarness(t, binary, store.Path, otherPEM, ActionsNamespace); reply["error"] != "vault_locked" {
		t.Fatalf("wrong identity: %v", reply["error"])
	}
	rewrite(t, store.Path, func(doc map[string]any) {
		doc["generation"] = 99
	})
	if reply := runHarness(t, binary, store.Path, pemPath, ActionsNamespace); reply["error"] != "vault_tampered" {
		t.Fatalf("tampered: %v", reply["error"])
	}
}
