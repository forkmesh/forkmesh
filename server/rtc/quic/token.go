package quic

import (
	"crypto/aes"
	"crypto/cipher"
	"crypto/hmac"
	"crypto/sha256"
	"encoding/binary"
	"net/netip"
	"sync"
	"time"
)

const (
	retryTokenLifetime = 10 * time.Second
	tokenKindRetry     = 1
)

type tokenSealer struct {
	aead cipher.AEAD
}

func newTokenSealer(key []byte) *tokenSealer {
	k := sha256.Sum256(append([]byte("forkmesh quic token key "), key...))
	block, err := aes.NewCipher(k[:])
	if err != nil {
		panic(err)
	}
	aead, err := cipher.NewGCM(block)
	if err != nil {
		panic(err)
	}
	return &tokenSealer{aead: aead}
}

func addrAAD(addr netip.AddrPort) []byte {
	a := addr.Addr().Unmap().As16()
	out := make([]byte, 0, 18)
	out = append(out, a[:]...)
	return binary.BigEndian.AppendUint16(out, addr.Port())
}

func (t *tokenSealer) retryToken(addr netip.AddrPort, odcid, rscid []byte, now time.Time) []byte {
	plain := make([]byte, 0, 1+8+2+len(odcid)+len(rscid))
	plain = append(plain, tokenKindRetry)
	plain = binary.BigEndian.AppendUint64(plain, uint64(now.UnixMilli()))
	plain = append(plain, byte(len(odcid)))
	plain = append(plain, odcid...)
	plain = append(plain, byte(len(rscid)))
	plain = append(plain, rscid...)
	nonce := randomBytes(t.aead.NonceSize())
	return t.aead.Seal(nonce, nonce, plain, addrAAD(addr))
}

func (t *tokenSealer) openRetryToken(token []byte, addr netip.AddrPort, now time.Time) (odcid, rscid []byte, ok bool) {
	ns := t.aead.NonceSize()
	if len(token) < ns+aeadTagLen+11 || len(token) > 256 {
		return nil, nil, false
	}
	plain, err := t.aead.Open(nil, token[:ns], token[ns:], addrAAD(addr))
	if err != nil || len(plain) < 11 || plain[0] != tokenKindRetry {
		return nil, nil, false
	}
	issued := time.UnixMilli(int64(binary.BigEndian.Uint64(plain[1:9])))
	if now.Sub(issued) > retryTokenLifetime || issued.Sub(now) > time.Second {
		return nil, nil, false
	}
	p := 9
	ol := int(plain[p])
	p++
	if ol > maxCIDLen || len(plain) < p+ol+1 {
		return nil, nil, false
	}
	odcid = plain[p : p+ol]
	p += ol
	rl := int(plain[p])
	p++
	if rl > maxCIDLen || len(plain) != p+rl {
		return nil, nil, false
	}
	return odcid, plain[p : p+rl], true
}

func statelessResetToken(key, cid []byte) [16]byte {
	m := hmac.New(sha256.New, key)
	m.Write(cid)
	var t [16]byte
	copy(t[:], m.Sum(nil))
	return t
}

type rateLimiter struct {
	mu     sync.Mutex
	rate   float64
	tokens float64
	last   time.Time
}

func (r *rateLimiter) allow(now time.Time) bool {
	r.mu.Lock()
	defer r.mu.Unlock()
	if r.last.IsZero() {
		r.last = now
		r.tokens = r.rate
	}
	r.tokens += now.Sub(r.last).Seconds() * r.rate
	r.last = now
	if r.tokens > r.rate {
		r.tokens = r.rate
	}
	if r.tokens < 1 {
		return false
	}
	r.tokens--
	return true
}
