package quic

import (
	"crypto/aes"
	"crypto/cipher"
	"crypto/hkdf"
	"crypto/sha256"
	"crypto/sha512"
	"crypto/subtle"
	"crypto/tls"
	"encoding/binary"
	"errors"
	"hash"
)

const (
	aeadTagLen   = 16
	hpSampleLen  = 16
	keyUpdateTag = "quic ku"
)

var initialSaltV1 = []byte{
	0x38, 0x76, 0x2c, 0xf7, 0xf5, 0x59, 0x34, 0xb3, 0x4d, 0x17,
	0x9a, 0xe6, 0xa4, 0xc8, 0x0c, 0xad, 0xcc, 0xbb, 0x7f, 0x0a,
}

var (
	retryKey   = []byte{0xbe, 0x0c, 0x69, 0x0b, 0x9f, 0x66, 0x57, 0x5a, 0x1d, 0x76, 0x6b, 0x54, 0xe3, 0x68, 0xc8, 0x4e}
	retryNonce = []byte{0x46, 0x15, 0x99, 0xd3, 0x5d, 0x63, 0x2b, 0xf2, 0x23, 0x98, 0x25, 0xbb}
)

var errDecrypt = errors.New("quic: packet decryption failed")

type cipherSuite struct {
	id     uint16
	hash   func() hash.Hash
	keyLen int
	chacha bool
}

var (
	suiteAES128   = &cipherSuite{id: tls.TLS_AES_128_GCM_SHA256, hash: sha256.New, keyLen: 16}
	suiteAES256   = &cipherSuite{id: tls.TLS_AES_256_GCM_SHA384, hash: sha512.New384, keyLen: 32}
	suiteChaCha20 = &cipherSuite{id: tls.TLS_CHACHA20_POLY1305_SHA256, hash: sha256.New, keyLen: 32, chacha: true}
)

func suiteByID(id uint16) *cipherSuite {
	switch id {
	case tls.TLS_AES_128_GCM_SHA256:
		return suiteAES128
	case tls.TLS_AES_256_GCM_SHA384:
		return suiteAES256
	case tls.TLS_CHACHA20_POLY1305_SHA256:
		return suiteChaCha20
	}
	return nil
}

func hkdfExpandLabel(h func() hash.Hash, secret []byte, label string, context []byte, length int) []byte {
	info := make([]byte, 0, 4+6+len(label)+len(context))
	info = append(info, byte(length>>8), byte(length), byte(6+len(label)))
	info = append(info, "tls13 "...)
	info = append(info, label...)
	info = append(info, byte(len(context)))
	info = append(info, context...)
	out, err := hkdf.Expand(h, secret, string(info), length)
	if err != nil {
		panic(err)
	}
	return out
}

func initialSecrets(dcid []byte) (client, server []byte) {
	initial, err := hkdf.Extract(sha256.New, dcid, initialSaltV1)
	if err != nil {
		panic(err)
	}
	client = hkdfExpandLabel(sha256.New, initial, "client in", nil, sha256.Size)
	server = hkdfExpandLabel(sha256.New, initial, "server in", nil, sha256.Size)
	return client, server
}

type packetKey struct {
	aead cipher.AEAD
	iv   [12]byte
}

func newPacketKey(suite *cipherSuite, secret []byte) *packetKey {
	key := hkdfExpandLabel(suite.hash, secret, "quic key", nil, suite.keyLen)
	iv := hkdfExpandLabel(suite.hash, secret, "quic iv", nil, 12)
	var aead cipher.AEAD
	if suite.chacha {
		aead = newChaCha20Poly1305(key)
	} else {
		block, err := aes.NewCipher(key)
		if err != nil {
			panic(err)
		}
		if aead, err = cipher.NewGCM(block); err != nil {
			panic(err)
		}
	}
	k := &packetKey{aead: aead}
	copy(k.iv[:], iv)
	return k
}

func (k *packetKey) nonce(pn int64) [12]byte {
	n := k.iv
	var p [8]byte
	binary.BigEndian.PutUint64(p[:], uint64(pn))
	for i := 0; i < 8; i++ {
		n[4+i] ^= p[i]
	}
	return n
}

func (k *packetKey) seal(packet []byte, headerLen int, pn int64) []byte {
	n := k.nonce(pn)
	return k.aead.Seal(packet[:headerLen], n[:], packet[headerLen:], packet[:headerLen])
}

func (k *packetKey) open(packet []byte, headerLen int, pn int64) ([]byte, error) {
	n := k.nonce(pn)
	out, err := k.aead.Open(packet[headerLen:headerLen], n[:], packet[headerLen:], packet[:headerLen])
	if err != nil {
		return nil, errDecrypt
	}
	return out, nil
}

type headerKey struct {
	block  cipher.Block
	chacha *chachaKey
}

func newHeaderKey(suite *cipherSuite, secret []byte) headerKey {
	hp := hkdfExpandLabel(suite.hash, secret, "quic hp", nil, suite.keyLen)
	if suite.chacha {
		return headerKey{chacha: newChachaKey(hp)}
	}
	block, err := aes.NewCipher(hp)
	if err != nil {
		panic(err)
	}
	return headerKey{block: block}
}

func (h headerKey) mask(sample []byte) [16]byte {
	var m [16]byte
	if h.chacha != nil {
		counter := binary.LittleEndian.Uint32(sample[0:4])
		nonce := nonceWords(sample[4:16])
		var block [64]byte
		chachaBlock(&block, h.chacha, counter, &nonce)
		copy(m[:], block[:5])
		return m
	}
	h.block.Encrypt(m[:], sample[:hpSampleLen])
	return m
}

func (h headerKey) protect(packet []byte, pnOffset, pnLen int) {
	m := h.mask(packet[pnOffset+4 : pnOffset+4+hpSampleLen])
	if packet[0]&0x80 != 0 {
		packet[0] ^= m[0] & 0x0f
	} else {
		packet[0] ^= m[0] & 0x1f
	}
	for i := 0; i < pnLen; i++ {
		packet[pnOffset+i] ^= m[1+i]
	}
}

func (h headerKey) unprotect(packet []byte, pnOffset int) (pnLen int, ok bool) {
	if pnOffset+4+hpSampleLen > len(packet) {
		return 0, false
	}
	m := h.mask(packet[pnOffset+4 : pnOffset+4+hpSampleLen])
	if packet[0]&0x80 != 0 {
		packet[0] ^= m[0] & 0x0f
	} else {
		packet[0] ^= m[0] & 0x1f
	}
	pnLen = int(packet[0]&0x03) + 1
	for i := 0; i < pnLen; i++ {
		packet[pnOffset+i] ^= m[1+i]
	}
	return pnLen, true
}

type directionKeys struct {
	suite  *cipherSuite
	secret []byte
	key    *packetKey
	hp     headerKey
}

func newDirectionKeys(suite *cipherSuite, secret []byte) *directionKeys {
	return &directionKeys{
		suite:  suite,
		secret: append([]byte(nil), secret...),
		key:    newPacketKey(suite, secret),
		hp:     newHeaderKey(suite, secret),
	}
}

func (d *directionKeys) nextGeneration() *directionKeys {
	secret := hkdfExpandLabel(d.suite.hash, d.secret, keyUpdateTag, nil, len(d.secret))
	return &directionKeys{
		suite:  d.suite,
		secret: secret,
		key:    newPacketKey(d.suite, secret),
		hp:     d.hp,
	}
}

func initialKeys(dcid []byte, isClient bool) (seal, open *directionKeys) {
	client, server := initialSecrets(dcid)
	c := newDirectionKeys(suiteAES128, client)
	s := newDirectionKeys(suiteAES128, server)
	if isClient {
		return c, s
	}
	return s, c
}

var retryAEAD = func() cipher.AEAD {
	block, err := aes.NewCipher(retryKey)
	if err != nil {
		panic(err)
	}
	aead, err := cipher.NewGCM(block)
	if err != nil {
		panic(err)
	}
	return aead
}()

func retryIntegrityTag(odcid, retryWithoutTag []byte) []byte {
	pseudo := make([]byte, 0, 1+len(odcid)+len(retryWithoutTag))
	pseudo = append(pseudo, byte(len(odcid)))
	pseudo = append(pseudo, odcid...)
	pseudo = append(pseudo, retryWithoutTag...)
	return retryAEAD.Seal(nil, retryNonce, nil, pseudo)
}

func validRetryTag(odcid, retry []byte) bool {
	if len(retry) < aeadTagLen {
		return false
	}
	want := retryIntegrityTag(odcid, retry[:len(retry)-aeadTagLen])
	return subtle.ConstantTimeCompare(want, retry[len(retry)-aeadTagLen:]) == 1
}
