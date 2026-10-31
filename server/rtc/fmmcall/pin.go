package fmmcall

import (
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/sha256"
	"crypto/subtle"
	"crypto/tls"
	"crypto/x509"
	"encoding/base64"
	"errors"
	"time"
)

const (
	ALPN        = "fmm/1"
	MaxCertSpan = 14 * 24 * time.Hour
)

var (
	ErrNoHashes    = errors.New("fmmcall: no certificate hashes")
	ErrCertPinned  = errors.New("fmmcall: server certificate matches no pinned hash")
	ErrCertKind    = errors.New("fmmcall: server certificate is not ECDSA P-256")
	ErrCertExpired = errors.New("fmmcall: server certificate outside its validity")
)

func DecodeHashes(hashes []string) ([][32]byte, error) {
	var out [][32]byte
	for _, h := range hashes {
		raw, err := base64.StdEncoding.DecodeString(h)
		if err != nil || len(raw) != sha256.Size {
			continue
		}
		var sum [32]byte
		copy(sum[:], raw)
		out = append(out, sum)
	}
	if len(out) == 0 {
		return nil, ErrNoHashes
	}
	return out, nil
}

func PinnedTLS(serverName string, hashes []string, now func() time.Time) (*tls.Config, error) {
	pins, err := DecodeHashes(hashes)
	if err != nil {
		return nil, err
	}
	if now == nil {
		now = time.Now
	}
	return &tls.Config{
		MinVersion:         tls.VersionTLS13,
		ServerName:         serverName,
		NextProtos:         []string{ALPN},
		InsecureSkipVerify: true,
		VerifyConnection: func(cs tls.ConnectionState) error {
			if len(cs.PeerCertificates) == 0 {
				return ErrCertPinned
			}
			return CheckPinned(cs.PeerCertificates[0], pins, now())
		},
	}, nil
}

func CheckPinned(leaf *x509.Certificate, pins [][32]byte, now time.Time) error {
	sum := sha256.Sum256(leaf.Raw)
	found := 0
	for _, p := range pins {
		found |= subtle.ConstantTimeCompare(sum[:], p[:])
	}
	if found != 1 {
		return ErrCertPinned
	}
	key, ok := leaf.PublicKey.(*ecdsa.PublicKey)
	if !ok || key.Curve != elliptic.P256() {
		return ErrCertKind
	}
	if now.Before(leaf.NotBefore) || now.After(leaf.NotAfter) || leaf.NotAfter.Sub(leaf.NotBefore) > MaxCertSpan {
		return ErrCertExpired
	}
	return nil
}
