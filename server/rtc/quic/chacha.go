package quic

import (
	"crypto/subtle"
	"encoding/binary"
	"errors"
	"math/bits"
)

const (
	chachaKeySize   = 32
	chachaNonceSize = 12
	polyTagSize     = 16
)

var errChaChaOpen = errors.New("quic: chacha20-poly1305 authentication failed")

type chachaKey [8]uint32

func newChachaKey(key []byte) *chachaKey {
	var k chachaKey
	for i := range k {
		k[i] = binary.LittleEndian.Uint32(key[4*i:])
	}
	return &k
}

func quarterRound(a, b, c, d uint32) (uint32, uint32, uint32, uint32) {
	a += b
	d ^= a
	d = bits.RotateLeft32(d, 16)
	c += d
	b ^= c
	b = bits.RotateLeft32(b, 12)
	a += b
	d ^= a
	d = bits.RotateLeft32(d, 8)
	c += d
	b ^= c
	b = bits.RotateLeft32(b, 7)
	return a, b, c, d
}

func chachaBlock(out *[64]byte, k *chachaKey, counter uint32, nonce *[3]uint32) {
	s := [16]uint32{
		0x61707865, 0x3320646e, 0x79622d32, 0x6b206574,
		k[0], k[1], k[2], k[3], k[4], k[5], k[6], k[7],
		counter, nonce[0], nonce[1], nonce[2],
	}
	x := s
	for i := 0; i < 10; i++ {
		x[0], x[4], x[8], x[12] = quarterRound(x[0], x[4], x[8], x[12])
		x[1], x[5], x[9], x[13] = quarterRound(x[1], x[5], x[9], x[13])
		x[2], x[6], x[10], x[14] = quarterRound(x[2], x[6], x[10], x[14])
		x[3], x[7], x[11], x[15] = quarterRound(x[3], x[7], x[11], x[15])
		x[0], x[5], x[10], x[15] = quarterRound(x[0], x[5], x[10], x[15])
		x[1], x[6], x[11], x[12] = quarterRound(x[1], x[6], x[11], x[12])
		x[2], x[7], x[8], x[13] = quarterRound(x[2], x[7], x[8], x[13])
		x[3], x[4], x[9], x[14] = quarterRound(x[3], x[4], x[9], x[14])
	}
	for i := range x {
		binary.LittleEndian.PutUint32(out[4*i:], x[i]+s[i])
	}
}

func chachaXOR(dst, src []byte, k *chachaKey, counter uint32, nonce *[3]uint32) {
	var block [64]byte
	for len(src) > 0 {
		chachaBlock(&block, k, counter, nonce)
		counter++
		n := len(src)
		if n > 64 {
			n = 64
		}
		subtle.XORBytes(dst[:n], src[:n], block[:n])
		dst, src = dst[n:], src[n:]
	}
}

type poly1305 struct {
	r0, r1     uint64
	s0, s1     uint64
	h0, h1, h2 uint64
}

func newPoly1305(key *[32]byte) *poly1305 {
	return &poly1305{
		r0: binary.LittleEndian.Uint64(key[0:8]) & 0x0FFFFFFC0FFFFFFF,
		r1: binary.LittleEndian.Uint64(key[8:16]) & 0x0FFFFFFC0FFFFFFC,
		s0: binary.LittleEndian.Uint64(key[16:24]),
		s1: binary.LittleEndian.Uint64(key[24:32]),
	}
}

func (p *poly1305) block(m []byte) {
	var c uint64
	h0, h1, h2 := p.h0, p.h1, p.h2
	h0, c = bits.Add64(h0, binary.LittleEndian.Uint64(m[0:8]), 0)
	h1, c = bits.Add64(h1, binary.LittleEndian.Uint64(m[8:16]), c)
	h2 += c + 1
	r0, r1 := p.r0, p.r1
	h0r0hi, h0r0lo := bits.Mul64(h0, r0)
	h1r0hi, h1r0lo := bits.Mul64(h1, r0)
	h2r0hi, h2r0lo := bits.Mul64(h2, r0)
	h0r1hi, h0r1lo := bits.Mul64(h0, r1)
	h1r1hi, h1r1lo := bits.Mul64(h1, r1)
	_, h2r1lo := bits.Mul64(h2, r1)
	m1lo, c := bits.Add64(h1r0lo, h0r1lo, 0)
	m1hi, _ := bits.Add64(h1r0hi, h0r1hi, c)
	m2lo, c := bits.Add64(h2r0lo, h1r1lo, 0)
	m2hi, _ := bits.Add64(h2r0hi, h1r1hi, c)
	t0 := h0r0lo
	t1, c := bits.Add64(m1lo, h0r0hi, 0)
	t2, c := bits.Add64(m2lo, m1hi, c)
	t3, _ := bits.Add64(h2r1lo, m2hi, c)
	h0, h1, h2 = t0, t1, t2&3
	cLo, cHi := t2&^3, t3
	h0, c = bits.Add64(h0, cLo, 0)
	h1, c = bits.Add64(h1, cHi, c)
	h2 += c
	cLo = cLo>>2 | cHi<<62
	cHi >>= 2
	h0, c = bits.Add64(h0, cLo, 0)
	h1, c = bits.Add64(h1, cHi, c)
	h2 += c
	p.h0, p.h1, p.h2 = h0, h1, h2
}

func (p *poly1305) padded(b []byte) {
	for len(b) >= 16 {
		p.block(b[:16])
		b = b[16:]
	}
	if len(b) > 0 {
		var last [16]byte
		copy(last[:], b)
		p.block(last[:])
	}
}

func (p *poly1305) sum(out []byte) {
	h0, h1, h2 := p.h0, p.h1, p.h2
	t0, b := bits.Sub64(h0, 0xFFFFFFFFFFFFFFFB, 0)
	t1, b := bits.Sub64(h1, 0xFFFFFFFFFFFFFFFF, b)
	_, b = bits.Sub64(h2, 3, b)
	keep := -b
	h0 = h0&keep | t0&^keep
	h1 = h1&keep | t1&^keep
	var c uint64
	h0, c = bits.Add64(h0, p.s0, 0)
	h1, _ = bits.Add64(h1, p.s1, c)
	binary.LittleEndian.PutUint64(out[0:8], h0)
	binary.LittleEndian.PutUint64(out[8:16], h1)
}

func poly1305Sum(out []byte, msg []byte, key *[32]byte) {
	p := newPoly1305(key)
	for len(msg) >= 16 {
		p.block(msg[:16])
		msg = msg[16:]
	}
	if len(msg) > 0 {
		var last [17]byte
		copy(last[:], msg)
		last[len(msg)] = 1
		p.lastPartial(last[:16])
	}
	p.sum(out)
}

func (p *poly1305) lastPartial(m []byte) {
	h2 := p.h2
	p.h2 = h2 - 1
	p.block(m)
}

type chacha20poly1305 struct {
	key *chachaKey
}

func newChaCha20Poly1305(key []byte) *chacha20poly1305 {
	return &chacha20poly1305{key: newChachaKey(key)}
}

func (a *chacha20poly1305) NonceSize() int { return chachaNonceSize }

func (a *chacha20poly1305) Overhead() int { return polyTagSize }

func (a *chacha20poly1305) tag(out []byte, nonce *[3]uint32, ad, ciphertext []byte) {
	var block [64]byte
	chachaBlock(&block, a.key, 0, nonce)
	var pk [32]byte
	copy(pk[:], block[:32])
	p := newPoly1305(&pk)
	p.padded(ad)
	p.padded(ciphertext)
	var lens [16]byte
	binary.LittleEndian.PutUint64(lens[0:8], uint64(len(ad)))
	binary.LittleEndian.PutUint64(lens[8:16], uint64(len(ciphertext)))
	p.block(lens[:])
	p.sum(out)
}

func nonceWords(nonce []byte) [3]uint32 {
	return [3]uint32{
		binary.LittleEndian.Uint32(nonce[0:4]),
		binary.LittleEndian.Uint32(nonce[4:8]),
		binary.LittleEndian.Uint32(nonce[8:12]),
	}
}

func sliceForAppend(in []byte, n int) (head, tail []byte) {
	if total := len(in) + n; cap(in) >= total {
		head = in[:total]
	} else {
		head = make([]byte, total)
		copy(head, in)
	}
	return head, head[len(in):]
}

func (a *chacha20poly1305) Seal(dst, nonce, plaintext, ad []byte) []byte {
	if len(nonce) != chachaNonceSize {
		panic("quic: bad chacha20-poly1305 nonce length")
	}
	n := nonceWords(nonce)
	ret, out := sliceForAppend(dst, len(plaintext)+polyTagSize)
	chachaXOR(out[:len(plaintext)], plaintext, a.key, 1, &n)
	a.tag(out[len(plaintext):], &n, ad, out[:len(plaintext)])
	return ret
}

func (a *chacha20poly1305) Open(dst, nonce, ciphertext, ad []byte) ([]byte, error) {
	if len(nonce) != chachaNonceSize {
		panic("quic: bad chacha20-poly1305 nonce length")
	}
	if len(ciphertext) < polyTagSize {
		return nil, errChaChaOpen
	}
	n := nonceWords(nonce)
	body := ciphertext[:len(ciphertext)-polyTagSize]
	var want [16]byte
	a.tag(want[:], &n, ad, body)
	if subtle.ConstantTimeCompare(want[:], ciphertext[len(body):]) != 1 {
		return nil, errChaChaOpen
	}
	ret, out := sliceForAppend(dst, len(body))
	chachaXOR(out, body, a.key, 1, &n)
	return ret, nil
}
