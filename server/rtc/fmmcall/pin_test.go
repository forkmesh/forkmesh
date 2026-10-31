package fmmcall

import (
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/rsa"
	"crypto/sha256"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/base64"
	"errors"
	"math/big"
	"testing"
	"time"
)

func leaf(t *testing.T, key any, pub any, from, to time.Time) *x509.Certificate {
	t.Helper()
	tmpl := &x509.Certificate{SerialNumber: big.NewInt(1), Subject: pkix.Name{CommonName: "x"}, NotBefore: from, NotAfter: to}
	der, err := x509.CreateCertificate(rand.Reader, tmpl, tmpl, pub, key)
	if err != nil {
		t.Fatal(err)
	}
	c, err := x509.ParseCertificate(der)
	if err != nil {
		t.Fatal(err)
	}
	return c
}

func pin(c *x509.Certificate) [][32]byte { return [][32]byte{sha256.Sum256(c.Raw)} }

func TestPinsAcceptOnlyShortLivedP256LeavesWithAKnownHash(t *testing.T) {
	now := time.Now()
	ec, _ := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	good := leaf(t, ec, &ec.PublicKey, now.Add(-time.Hour), now.Add(10*24*time.Hour))
	if err := CheckPinned(good, pin(good), now); err != nil {
		t.Fatal(err)
	}
	other := leaf(t, ec, &ec.PublicKey, now.Add(-time.Hour), now.Add(9*24*time.Hour))
	if err := CheckPinned(good, pin(other), now); !errors.Is(err, ErrCertPinned) {
		t.Fatalf("foreign pin: %v", err)
	}
	long := leaf(t, ec, &ec.PublicKey, now.Add(-time.Hour), now.Add(20*24*time.Hour))
	if err := CheckPinned(long, pin(long), now); !errors.Is(err, ErrCertExpired) {
		t.Fatalf("20-day certificate: %v", err)
	}
	if err := CheckPinned(good, pin(good), now.Add(11*24*time.Hour)); !errors.Is(err, ErrCertExpired) {
		t.Fatalf("expired certificate: %v", err)
	}
	p384, _ := ecdsa.GenerateKey(elliptic.P384(), rand.Reader)
	wide := leaf(t, p384, &p384.PublicKey, now.Add(-time.Hour), now.Add(24*time.Hour))
	if err := CheckPinned(wide, pin(wide), now); !errors.Is(err, ErrCertKind) {
		t.Fatalf("P-384: %v", err)
	}
	rk, _ := rsa.GenerateKey(rand.Reader, 2048)
	rsaLeaf := leaf(t, rk, &rk.PublicKey, now.Add(-time.Hour), now.Add(24*time.Hour))
	if err := CheckPinned(rsaLeaf, pin(rsaLeaf), now); !errors.Is(err, ErrCertKind) {
		t.Fatalf("RSA: %v", err)
	}
}

func TestHashListsMustNameAtLeastOneSHA256(t *testing.T) {
	if _, err := PinnedTLS("x", nil, nil); !errors.Is(err, ErrNoHashes) {
		t.Fatal(err)
	}
	if _, err := PinnedTLS("x", []string{"junk", base64.StdEncoding.EncodeToString(make([]byte, 31))}, nil); !errors.Is(err, ErrNoHashes) {
		t.Fatal(err)
	}
	conf, err := PinnedTLS("core.example", []string{"junk", base64.StdEncoding.EncodeToString(make([]byte, 32))}, nil)
	if err != nil || conf.NextProtos[0] != ALPN || conf.ServerName != "core.example" || conf.VerifyConnection == nil {
		t.Fatalf("%v %+v", err, conf)
	}
}
