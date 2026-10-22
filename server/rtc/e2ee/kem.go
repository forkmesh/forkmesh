package e2ee

import (
	"crypto/aes"
	"crypto/cipher"
	"crypto/ecdh"
	"crypto/ed25519"
	"crypto/hkdf"
	"crypto/mlkem"
	"crypto/sha256"
	"crypto/subtle"
	"strconv"
	"strings"
)

type Sender struct {
	Peer  string
	IDK   []byte
	Epoch uint32
}

type FrameKey struct {
	Call     string
	Peer     string
	Epoch    uint32
	KeyID    byte
	Key      []byte
	Auth     ed25519.PublicKey
	AuthSeed []byte
}

type wrapTrace struct {
	SS1   []byte
	SS2   []byte
	KEK   []byte
	Nonce []byte
	Info  string
}

func callSalt(call string) []byte {
	h := sha256.Sum256([]byte(saltLabel + "\n" + call))
	return h[:]
}

func wrapInfo(call string, s Sender, r *Presence, eph, ct []byte) string {
	return strings.Join([]string{
		wrapLabel, call,
		s.Peer, enc(s.IDK), strconv.FormatUint(uint64(s.Epoch), 10),
		r.Peer, enc(r.IDK), strconv.FormatUint(uint64(r.Epoch), 10),
		enc(r.X), digest(r.PQ), enc(eph), digest(ct),
	}, "\n")
}

func wipe(bs ...[]byte) {
	for _, b := range bs {
		clear(b)
	}
}

func allZero(b []byte) bool {
	return subtle.ConstantTimeCompare(b, make([]byte, len(b))) == 1
}

func kekFrom(call, info string, ss1, ss2 []byte) (kek, nonce []byte, err error) {
	ikm := make([]byte, 0, len(ss1)+len(ss2))
	ikm = append(ikm, ss1...)
	ikm = append(ikm, ss2...)
	okm, err := hkdf.Key(sha256.New, ikm, callSalt(call), info, 32+12)
	wipe(ikm)
	if err != nil {
		return nil, nil, err
	}
	return okm[:32], okm[32:], nil
}

func seal(key, nonce, plain, aad []byte) ([]byte, error) {
	block, err := aes.NewCipher(key)
	if err != nil {
		return nil, err
	}
	aead, err := cipher.NewGCM(block)
	if err != nil {
		return nil, err
	}
	return aead.Seal(nil, nonce, plain, aad), nil
}

func open(key, nonce, sealed, aad []byte) ([]byte, error) {
	block, err := aes.NewCipher(key)
	if err != nil {
		return nil, err
	}
	aead, err := cipher.NewGCM(block)
	if err != nil {
		return nil, err
	}
	return aead.Open(nil, nonce, sealed, aad)
}

func wrapSecret(call string, s Sender, r *Presence, eph *ecdh.PrivateKey, secret []byte) (WrappedKey, *wrapTrace, error) {
	rx, err := ecdh.X25519().NewPublicKey(r.X)
	if err != nil {
		return WrappedKey{}, nil, ErrInvalid
	}
	ss1, err := eph.ECDH(rx)
	if err != nil || allZero(ss1) {
		return WrappedKey{}, nil, ErrInvalid
	}
	ek, err := mlkem.NewEncapsulationKey768(r.PQ)
	if err != nil {
		wipe(ss1)
		return WrappedKey{}, nil, ErrInvalid
	}
	ss2, ct := ek.Encapsulate()
	info := wrapInfo(call, s, r, eph.PublicKey().Bytes(), ct)
	kek, nonce, err := kekFrom(call, info, ss1, ss2)
	if err != nil {
		wipe(ss1, ss2)
		return WrappedKey{}, nil, err
	}
	key, err := seal(kek, nonce, secret, []byte(info))
	if err != nil {
		wipe(ss1, ss2, kek)
		return WrappedKey{}, nil, err
	}
	trace := &wrapTrace{SS1: ss1, SS2: ss2, KEK: kek, Nonce: nonce, Info: info}
	return WrappedKey{Peer: r.Peer, Epoch: r.Epoch, CT: ct, Key: key}, trace, nil
}

func unwrapSecret(call string, s Sender, eph []byte, w WrappedKey, r *Presence, x *ecdh.PrivateKey, pq *mlkem.DecapsulationKey768) ([]byte, error) {
	if w.Peer != r.Peer || w.Epoch != r.Epoch {
		return nil, ErrInvalid
	}
	pub, err := ecdh.X25519().NewPublicKey(eph)
	if err != nil {
		return nil, ErrInvalid
	}
	ss1, err := x.ECDH(pub)
	if err != nil || allZero(ss1) {
		return nil, ErrInvalid
	}
	ss2, err := pq.Decapsulate(w.CT)
	if err != nil {
		wipe(ss1)
		return nil, ErrInvalid
	}
	info := wrapInfo(call, s, r, eph, w.CT)
	kek, nonce, err := kekFrom(call, info, ss1, ss2)
	wipe(ss1, ss2)
	if err != nil {
		return nil, err
	}
	secret, err := open(kek, nonce, w.Key, []byte(info))
	wipe(kek)
	if err != nil || len(secret) != SecretSize {
		return nil, ErrInvalid
	}
	return secret, nil
}

func DeriveFrameKey(call string, s Sender, secret []byte) (FrameKey, error) {
	if len(secret) != SecretSize {
		return FrameKey{}, ErrInvalid
	}
	info := strings.Join([]string{frameLabel, call, s.Peer, enc(s.IDK), strconv.FormatUint(uint64(s.Epoch), 10)}, "\n")
	key, err := hkdf.Key(sha256.New, secret, callSalt(call), info, FrameKeySize)
	if err != nil {
		return FrameKey{}, err
	}
	return FrameKey{Call: call, Peer: s.Peer, Epoch: s.Epoch, KeyID: byte(s.Epoch), Key: key}, nil
}
