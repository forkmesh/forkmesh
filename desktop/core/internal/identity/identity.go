package identity

import (
	"crypto/ed25519"
	"crypto/x509"
	"encoding/base64"
	"encoding/pem"
	"errors"
	"fmt"
	"os"
)

type Identity struct {
	private ed25519.PrivateKey
	public  ed25519.PublicKey
}

func Load(path string) (*Identity, error) {
	raw, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	return Parse(raw)
}

func Parse(raw []byte) (*Identity, error) {
	block, _ := pem.Decode(raw)
	if block == nil {
		return nil, errors.New("identity: no PEM block")
	}
	key, err := x509.ParsePKCS8PrivateKey(block.Bytes)
	if err != nil {
		return nil, fmt.Errorf("identity: %w", err)
	}
	private, ok := key.(ed25519.PrivateKey)
	if !ok {
		return nil, errors.New("identity: not an Ed25519 key")
	}
	return &Identity{private: private, public: private.Public().(ed25519.PublicKey)}, nil
}

func FromSeed(seed []byte) (*Identity, error) {
	if len(seed) != ed25519.SeedSize {
		return nil, errors.New("identity: seed must be 32 bytes")
	}
	private := ed25519.NewKeyFromSeed(seed)
	return &Identity{private: private, public: private.Public().(ed25519.PublicKey)}, nil
}

func (i *Identity) PublicKey() string { return Encode(i.public) }

func (i *Identity) PublicKeyBytes() []byte { return append([]byte(nil), i.public...) }

func (i *Identity) Sign(data []byte) string { return Encode(ed25519.Sign(i.private, data)) }

func (i *Identity) SignBytes(data []byte) []byte { return ed25519.Sign(i.private, data) }

func Verify(publicKey, signature string, data []byte) bool {
	key, err := Decode(publicKey, ed25519.PublicKeySize)
	if err != nil {
		return false
	}
	sig, err := Decode(signature, ed25519.SignatureSize)
	if err != nil {
		return false
	}
	return ed25519.Verify(key, data, sig)
}

func Encode(data []byte) string { return base64.RawURLEncoding.EncodeToString(data) }

func Decode(text string, size int) ([]byte, error) {
	trimmed := text
	for len(trimmed) > 0 && trimmed[len(trimmed)-1] == '=' {
		trimmed = trimmed[:len(trimmed)-1]
	}
	out, err := base64.RawURLEncoding.DecodeString(trimmed)
	if err != nil {
		return nil, err
	}
	if size > 0 && len(out) != size {
		return nil, fmt.Errorf("identity: want %d bytes, got %d", size, len(out))
	}
	return out, nil
}
