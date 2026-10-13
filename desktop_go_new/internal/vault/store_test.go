package vault

import (
	"bytes"
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"runtime"
	"testing"

	"forkmesh.com/desktop-core/internal/identity"
)

func testIdentity(t *testing.T, fill byte) *identity.Identity {
	t.Helper()
	id, err := identity.FromSeed(bytes.Repeat([]byte{fill}, 32))
	if err != nil {
		t.Fatal(err)
	}
	return id
}

func newStore(t *testing.T, signers ...Signer) *Store {
	t.Helper()
	list := signers
	return &Store{Path: filepath.Join(t.TempDir(), "vault", "secrets.json"), Signers: func() []Signer { return list }}
}

func put(t *testing.T, store *Store, values map[string]string) {
	t.Helper()
	if err := store.Update(func(tx *Tx) error {
		for name, value := range values {
			if err := tx.Set(name, []byte(value)); err != nil {
				return err
			}
		}
		return nil
	}); err != nil {
		t.Fatal(err)
	}
}

func get(t *testing.T, store *Store, name string) (string, error) {
	t.Helper()
	var out string
	err := store.View(func(tx *Tx) error {
		value, ok, err := tx.Get(name)
		if err != nil {
			return err
		}
		if !ok {
			return os.ErrNotExist
		}
		out = string(value)
		return nil
	})
	return out, err
}

func rewrite(t *testing.T, path string, edit func(doc map[string]any)) {
	t.Helper()
	raw, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	var doc map[string]any
	if err := json.Unmarshal(raw, &doc); err != nil {
		t.Fatal(err)
	}
	edit(doc)
	raw, _ = json.Marshal(doc)
	if err := os.WriteFile(path, raw, 0o600); err != nil {
		t.Fatal(err)
	}
}

func TestRoundTripAndFileMode(t *testing.T) {
	store := newStore(t, testIdentity(t, 1))
	put(t, store, map[string]string{"actions/A": "alpha", "actions/B": "", "other/x%20y": "zeta"})
	for name, want := range map[string]string{"actions/A": "alpha", "actions/B": "", "other/x%20y": "zeta"} {
		got, err := get(t, store, name)
		if err != nil || got != want {
			t.Fatalf("%s: %q %v", name, got, err)
		}
	}
	raw, _ := os.ReadFile(store.Path)
	for _, secret := range []string{"alpha", "zeta"} {
		if bytes.Contains(raw, []byte(secret)) {
			t.Fatalf("plaintext %q in the vault file", secret)
		}
	}
	if runtime.GOOS != "windows" {
		info, _ := os.Stat(store.Path)
		if info.Mode().Perm() != 0o600 {
			t.Fatalf("file mode %v", info.Mode().Perm())
		}
		dir, _ := os.Stat(filepath.Dir(store.Path))
		if dir.Mode().Perm() != 0o700 {
			t.Fatalf("dir mode %v", dir.Mode().Perm())
		}
	}
	status := store.Status()
	if status.State != "ready" || status.Count != 3 || status.Generation != 1 {
		t.Fatalf("status %+v", status)
	}
	entries, _ := os.ReadDir(filepath.Dir(store.Path))
	if len(entries) != 1 {
		t.Fatalf("leftover temp files: %d entries", len(entries))
	}
}

func TestWrongIdentityIsLocked(t *testing.T) {
	owner := testIdentity(t, 1)
	store := newStore(t, owner)
	put(t, store, map[string]string{"actions/A": "alpha"})
	stranger := &Store{Path: store.Path, Signers: func() []Signer { return []Signer{testIdentity(t, 2)} }}
	if _, err := get(t, stranger, "actions/A"); !errors.Is(err, ErrIdentityMismatch) {
		t.Fatalf("want identity mismatch, got %v", err)
	}
	if err := stranger.Update(func(tx *Tx) error { return tx.Set("actions/B", []byte("x")) }); !errors.Is(err, ErrIdentityMismatch) {
		t.Fatalf("a stranger must not write: %v", err)
	}
	if stranger.Status().State != "locked" {
		t.Fatal("status should be locked")
	}
	none := &Store{Path: store.Path, Signers: func() []Signer { return nil }}
	if _, err := get(t, none, "actions/A"); !errors.Is(err, ErrNoIdentity) {
		t.Fatalf("want no identity, got %v", err)
	}
}

func TestTamperDetection(t *testing.T) {
	cases := map[string]func(doc map[string]any){
		"flipped ciphertext": func(doc map[string]any) {
			entry := doc["entries"].(map[string]any)["actions/A"].(map[string]any)
			data := []byte(entry["data"].(string))
			if data[0] == 'A' {
				data[0] = 'B'
			} else {
				data[0] = 'A'
			}
			entry["data"] = string(data)
		},
		"swapped entries": func(doc map[string]any) {
			entries := doc["entries"].(map[string]any)
			entries["actions/A"], entries["actions/B"] = entries["actions/B"], entries["actions/A"]
		},
		"deleted entry": func(doc map[string]any) {
			delete(doc["entries"].(map[string]any), "actions/B")
		},
		"rolled generation": func(doc map[string]any) {
			doc["generation"] = 7
		},
		"changed timestamp": func(doc map[string]any) {
			doc["entries"].(map[string]any)["actions/A"].(map[string]any)["at"] = 1
		},
		"wrapped key edited": func(doc map[string]any) {
			wrap := doc["wraps"].([]any)[0].(map[string]any)
			key := []byte(wrap["key"].(string))
			if key[2] == 'A' {
				key[2] = 'B'
			} else {
				key[2] = 'A'
			}
			wrap["key"] = string(key)
		},
	}
	for name, edit := range cases {
		t.Run(name, func(t *testing.T) {
			store := newStore(t, testIdentity(t, 1))
			put(t, store, map[string]string{"actions/A": "alpha", "actions/B": "beta"})
			rewrite(t, store.Path, edit)
			if _, err := get(t, store, "actions/A"); !errors.Is(err, ErrTampered) {
				t.Fatalf("want tampered, got %v", err)
			}
			if store.Status().State != "tampered" {
				t.Fatal("status should be tampered")
			}
		})
	}
	store := newStore(t, testIdentity(t, 1))
	if err := os.MkdirAll(filepath.Dir(store.Path), 0o700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(store.Path, []byte(`{"kind":"forkmesh.vault","v":2}`), 0o600); err != nil {
		t.Fatal(err)
	}
	if _, err := get(t, store, "x"); !errors.Is(err, ErrFormat) {
		t.Fatalf("unknown version: %v", err)
	}
}

func TestRotation(t *testing.T) {
	store := newStore(t, testIdentity(t, 1))
	put(t, store, map[string]string{"actions/A": "alpha", "actions/B": "beta"})
	before := store.Status()
	beforeRaw, _ := os.ReadFile(store.Path)
	keyID, err := store.Rotate()
	if err != nil || keyID == "" || keyID == before.KeyID {
		t.Fatalf("rotate: %q %v", keyID, err)
	}
	after := store.Status()
	if after.KeyID != keyID || after.Generation != before.Generation+1 || after.Count != 2 {
		t.Fatalf("after %+v", after)
	}
	for name, want := range map[string]string{"actions/A": "alpha", "actions/B": "beta"} {
		if got, err := get(t, store, name); err != nil || got != want {
			t.Fatalf("%s after rotation: %v", name, err)
		}
	}
	var old, fresh document
	_ = json.Unmarshal(beforeRaw, &old)
	raw, _ := os.ReadFile(store.Path)
	_ = json.Unmarshal(raw, &fresh)
	if old.Entries["actions/A"].Data == fresh.Entries["actions/A"].Data {
		t.Fatal("rotation must re-encrypt every entry")
	}
	if old.Entries["actions/A"].At != fresh.Entries["actions/A"].At {
		t.Fatal("rotation keeps the entry timestamps")
	}
	if err := os.WriteFile(store.Path, beforeRaw, 0o600); err != nil {
		t.Fatal(err)
	}
	if got, err := get(t, store, "actions/A"); err != nil || got != "alpha" {
		t.Fatal("an old copy still opens with the identity that wrapped it")
	}
}

func TestIdentityChangeRewraps(t *testing.T) {
	oldID, newID := testIdentity(t, 1), testIdentity(t, 2)
	store := newStore(t, oldID)
	put(t, store, map[string]string{"actions/A": "alpha"})
	both := &Store{Path: store.Path, Signers: func() []Signer { return []Signer{newID, oldID} }}
	if err := both.Update(func(tx *Tx) error { return nil }); err != nil {
		t.Fatal(err)
	}
	if ids := both.Status().Identities; len(ids) != 2 || ids[0] != newID.PublicKey() {
		t.Fatalf("wraps %v", ids)
	}
	onlyNew := &Store{Path: store.Path, Signers: func() []Signer { return []Signer{newID} }}
	if got, err := get(t, onlyNew, "actions/A"); err != nil || got != "alpha" {
		t.Fatalf("new identity: %v", err)
	}
	if got, err := get(t, store, "actions/A"); err != nil || got != "alpha" {
		t.Fatalf("old identity keeps its wrap: %v", err)
	}
}

func TestLimitsAndNames(t *testing.T) {
	store := newStore(t, testIdentity(t, 1))
	err := store.Update(func(tx *Tx) error { return tx.Set("actions/bad name", []byte("x")) })
	if !errors.Is(err, ErrInvalidName) {
		t.Fatalf("space in name: %v", err)
	}
	err = store.Update(func(tx *Tx) error { return tx.Set("actions/BIG", make([]byte, MaxValueBytes+1)) })
	if !errors.Is(err, ErrTooLarge) {
		t.Fatalf("too large: %v", err)
	}
	if store.Exists() {
		t.Fatal("a failed first write must not create the file")
	}
	if EntryName("actions", "my var/é") != "actions/my%20var%2F%C3%A9" {
		t.Fatal(EntryName("actions", "my var/é"))
	}
	if name, ok := NameOf("actions", "actions/my%20var%2F%C3%A9"); !ok || name != "my var/é" {
		t.Fatal(name)
	}
	if err := os.MkdirAll(filepath.Dir(store.Path), 0o700); err != nil {
		t.Fatal(err)
	}
	if runtime.GOOS != "windows" {
		target := filepath.Join(t.TempDir(), "elsewhere")
		if err := os.Symlink(target, store.Path); err != nil {
			t.Fatal(err)
		}
		if err := store.Update(func(tx *Tx) error { return tx.Set("actions/A", []byte("x")) }); err == nil {
			t.Fatal("a symlinked vault path must be refused")
		}
		if _, err := os.Stat(target); err == nil {
			t.Fatal("wrote through a symlink")
		}
	}
}
