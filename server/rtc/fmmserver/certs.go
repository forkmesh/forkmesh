package fmmserver

import (
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/sha256"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/base64"
	"encoding/pem"
	"errors"
	"math/big"
	"os"
	"path/filepath"
	"sync"
	"time"
)

const (
	CertRotateEvery = 6 * 24 * time.Hour
	CertLifetime    = 13 * 24 * time.Hour
	CertBackdate    = time.Hour
	CertMaxSpan     = 14 * 24 * time.Hour
	certCheckEvery  = time.Hour
	certFileCurrent = "fmm-current.pem"
	certFileNext    = "fmm-next.pem"
)

var ErrNoCertificate = errors.New("fmmserver: no certificate")

type Cert struct {
	TLS     tls.Certificate
	Hash    [32]byte
	Created time.Time
	Leaf    *x509.Certificate
}

func (c *Cert) HashBase64() string { return base64.StdEncoding.EncodeToString(c.Hash[:]) }

type CertStore struct {
	dir string
	now func() time.Time

	mu      sync.Mutex
	current *Cert
	next    *Cert
}

func NewCertStore(dir string, now func() time.Time) (*CertStore, error) {
	if now == nil {
		now = time.Now
	}
	s := &CertStore{dir: dir, now: now}
	if dir != "" {
		s.current = loadCert(filepath.Join(dir, certFileCurrent))
		s.next = loadCert(filepath.Join(dir, certFileNext))
	}
	if err := s.Rotate(); err != nil {
		return nil, err
	}
	return s, nil
}

func (s *CertStore) usable(c *Cert, now time.Time) bool {
	return c != nil && now.After(c.Leaf.NotBefore) && c.Leaf.NotAfter.Sub(now) > CertLifetime-2*CertRotateEvery
}

func (s *CertStore) Rotate() error {
	s.mu.Lock()
	defer s.mu.Unlock()
	now := s.now()
	changed := false
	if !s.usable(s.next, now) {
		s.next = nil
	}
	if s.next != nil && now.Sub(s.next.Created) >= CertRotateEvery {
		s.current, s.next = s.next, nil
		changed = true
	}
	if !s.usable(s.current, now) {
		s.current = nil
	}
	if s.current == nil {
		if s.next != nil {
			s.current, s.next = s.next, nil
		} else {
			c, err := newCert(now)
			if err != nil {
				return err
			}
			s.current = c
		}
		changed = true
	}
	if s.next == nil {
		c, err := newCert(now)
		if err != nil {
			return err
		}
		s.next = c
		changed = true
	}
	if changed && s.dir != "" {
		if err := saveCert(filepath.Join(s.dir, certFileNext), s.next); err != nil {
			return err
		}
		if err := saveCert(filepath.Join(s.dir, certFileCurrent), s.current); err != nil {
			return err
		}
	}
	return nil
}

func (s *CertStore) Current() *Cert {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.current
}

func (s *CertStore) Next() *Cert {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.next
}

func (s *CertStore) Hashes() []string {
	s.mu.Lock()
	defer s.mu.Unlock()
	var out []string
	for _, c := range []*Cert{s.current, s.next} {
		if c != nil {
			out = append(out, c.HashBase64())
		}
	}
	return out
}

func (s *CertStore) GetCertificate(*tls.ClientHelloInfo) (*tls.Certificate, error) {
	c := s.Current()
	if c == nil {
		return nil, ErrNoCertificate
	}
	return &c.TLS, nil
}

func (s *CertStore) Run(stop <-chan struct{}, logf func(string, ...any)) {
	t := time.NewTicker(certCheckEvery)
	defer t.Stop()
	for {
		select {
		case <-stop:
			return
		case <-t.C:
			if err := s.Rotate(); err != nil && logf != nil {
				logf("fmm certificate rotation: %v", err)
			}
		}
	}
}

func newCert(now time.Time) (*Cert, error) {
	key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		return nil, err
	}
	serial, err := rand.Int(rand.Reader, new(big.Int).Lsh(big.NewInt(1), 120))
	if err != nil {
		return nil, err
	}
	tmpl := &x509.Certificate{
		SerialNumber:          serial,
		Subject:               pkix.Name{CommonName: "forkmesh fm media"},
		NotBefore:             now.Add(-CertBackdate),
		NotAfter:              now.Add(CertLifetime),
		KeyUsage:              x509.KeyUsageDigitalSignature,
		ExtKeyUsage:           []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth},
		BasicConstraintsValid: true,
	}
	der, err := x509.CreateCertificate(rand.Reader, tmpl, tmpl, &key.PublicKey, key)
	if err != nil {
		return nil, err
	}
	return certFrom(der, key)
}

func certFrom(der []byte, key *ecdsa.PrivateKey) (*Cert, error) {
	leaf, err := x509.ParseCertificate(der)
	if err != nil {
		return nil, err
	}
	pub, ok := leaf.PublicKey.(*ecdsa.PublicKey)
	if !ok || pub.Curve != elliptic.P256() || !pub.Equal(&key.PublicKey) {
		return nil, ErrNoCertificate
	}
	if leaf.NotAfter.Sub(leaf.NotBefore) > CertMaxSpan {
		return nil, ErrNoCertificate
	}
	return &Cert{
		TLS:     tls.Certificate{Certificate: [][]byte{der}, PrivateKey: key, Leaf: leaf},
		Hash:    sha256.Sum256(der),
		Created: leaf.NotBefore.Add(CertBackdate),
		Leaf:    leaf,
	}, nil
}

func loadCert(path string) *Cert {
	raw, err := os.ReadFile(path)
	if err != nil {
		return nil
	}
	var der []byte
	var key *ecdsa.PrivateKey
	for {
		var block *pem.Block
		block, raw = pem.Decode(raw)
		if block == nil {
			break
		}
		switch block.Type {
		case "CERTIFICATE":
			der = block.Bytes
		case "PRIVATE KEY":
			k, err := x509.ParsePKCS8PrivateKey(block.Bytes)
			if err != nil {
				return nil
			}
			key, _ = k.(*ecdsa.PrivateKey)
		}
	}
	if der == nil || key == nil {
		return nil
	}
	c, err := certFrom(der, key)
	if err != nil {
		return nil
	}
	return c
}

func saveCert(path string, c *Cert) error {
	key, err := x509.MarshalPKCS8PrivateKey(c.TLS.PrivateKey)
	if err != nil {
		return err
	}
	body := pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: c.TLS.Certificate[0]})
	body = append(body, pem.EncodeToMemory(&pem.Block{Type: "PRIVATE KEY", Bytes: key})...)
	tmp, err := os.CreateTemp(filepath.Dir(path), ".fmm-cert-*")
	if err != nil {
		return err
	}
	name := tmp.Name()
	if err := tmp.Chmod(0o600); err != nil {
		tmp.Close()
		os.Remove(name)
		return err
	}
	if _, err := tmp.Write(body); err != nil {
		tmp.Close()
		os.Remove(name)
		return err
	}
	if err := tmp.Sync(); err != nil {
		tmp.Close()
		os.Remove(name)
		return err
	}
	if err := tmp.Close(); err != nil {
		os.Remove(name)
		return err
	}
	return os.Rename(name, path)
}
