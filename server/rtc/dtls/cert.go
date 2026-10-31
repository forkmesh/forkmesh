package dtls

import (
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/sha256"
	"crypto/x509"
	"crypto/x509/pkix"
	"math/big"
	"strings"
	"time"
)

type Certificate struct {
	DER         []byte
	Key         *ecdsa.PrivateKey
	Fingerprint string
}

func GenerateCertificate() (*Certificate, error) {
	key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		return nil, err
	}
	serial, err := rand.Int(rand.Reader, new(big.Int).Lsh(big.NewInt(1), 63))
	if err != nil {
		return nil, err
	}
	now := time.Now()
	tmpl := &x509.Certificate{
		SerialNumber:          serial,
		Subject:               pkix.Name{CommonName: "forkmesh-calls"},
		Issuer:                pkix.Name{CommonName: "forkmesh-calls"},
		NotBefore:             now.Add(-time.Hour),
		NotAfter:              now.Add(30 * 24 * time.Hour),
		KeyUsage:              x509.KeyUsageDigitalSignature,
		ExtKeyUsage:           []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth, x509.ExtKeyUsageClientAuth},
		BasicConstraintsValid: true,
		SignatureAlgorithm:    x509.ECDSAWithSHA256,
	}
	der, err := x509.CreateCertificate(rand.Reader, tmpl, tmpl, &key.PublicKey, key)
	if err != nil {
		return nil, err
	}
	return &Certificate{DER: der, Key: key, Fingerprint: FingerprintOf(der)}, nil
}

func FingerprintOf(der []byte) string {
	sum := sha256.Sum256(der)
	const hexdigits = "0123456789ABCDEF"
	var b strings.Builder
	b.Grow(len("sha-256 ") + len(sum)*3)
	b.WriteString("sha-256 ")
	for i, c := range sum {
		if i > 0 {
			b.WriteByte(':')
		}
		b.WriteByte(hexdigits[c>>4])
		b.WriteByte(hexdigits[c&15])
	}
	return b.String()
}
