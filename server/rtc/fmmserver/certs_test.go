package fmmserver

import (
	"crypto/ecdsa"
	"crypto/elliptic"
	"os"
	"path/filepath"
	"slices"
	"testing"
	"time"

	"forkmesh.com/mirror-node/rtc/fmmcall"
)

func TestCertificatesRotateWithAnOverlapAndPersist(t *testing.T) {
	dir := t.TempDir()
	now := time.Date(2026, 10, 21, 12, 0, 0, 0, time.UTC)
	clock := func() time.Time { return now }
	store, err := NewCertStore(dir, clock)
	if err != nil {
		t.Fatal(err)
	}
	check := func(c *Cert) {
		t.Helper()
		key, ok := c.Leaf.PublicKey.(*ecdsa.PublicKey)
		if !ok || key.Curve != elliptic.P256() {
			t.Fatal("not ECDSA P-256")
		}
		if span := c.Leaf.NotAfter.Sub(c.Leaf.NotBefore); span > 14*24*time.Hour {
			t.Fatalf("validity %v over 14 days", span)
		}
		pins, _ := fmmcall.DecodeHashes([]string{c.HashBase64()})
		if err := fmmcall.CheckPinned(c.Leaf, pins, now); err != nil {
			t.Fatal(err)
		}
	}
	first := store.Hashes()
	if len(first) != 2 || first[0] == first[1] {
		t.Fatalf("hashes %v", first)
	}
	check(store.Current())
	check(store.Next())
	for _, name := range []string{certFileCurrent, certFileNext} {
		info, err := os.Stat(filepath.Join(dir, name))
		if err != nil || info.Mode().Perm() != 0o600 {
			t.Fatalf("%s: %v %v", name, err, info)
		}
	}
	again, err := NewCertStore(dir, clock)
	if err != nil {
		t.Fatal(err)
	}
	if !slices.Equal(again.Hashes(), first) {
		t.Fatal("a restart changed the published hashes")
	}
	published := first
	for day := 1; day <= 40; day++ {
		now = now.Add(24 * time.Hour)
		if err := store.Rotate(); err != nil {
			t.Fatal(err)
		}
		presented := store.Current().HashBase64()
		check(store.Current())
		if !slices.Contains(published, presented) {
			t.Fatalf("day %d: presented certificate not in the hashes published the day before", day)
		}
		if day%6 == 0 && store.Current().HashBase64() != published[1] {
			t.Fatalf("day %d: the published next certificate did not take over", day)
		}
		published = store.Hashes()
	}
	if err := os.WriteFile(filepath.Join(dir, certFileCurrent), []byte("junk"), 0o600); err != nil {
		t.Fatal(err)
	}
	repaired, err := NewCertStore(dir, clock)
	if err != nil {
		t.Fatal(err)
	}
	if repaired.Current().HashBase64() != store.Next().HashBase64() {
		t.Fatal("a broken current certificate was not replaced by the published next one")
	}
	later := now.Add(30 * 24 * time.Hour)
	stale, err := NewCertStore(dir, func() time.Time { return later })
	if err != nil {
		t.Fatal(err)
	}
	if slices.Contains(published, stale.Current().HashBase64()) {
		t.Fatal("an expired certificate was kept after a long downtime")
	}
}
