package dtls

import (
	"crypto/aes"
	"crypto/cipher"
)

type replayWindow struct {
	top    uint64
	bits   uint64
	inited bool
}

func (w *replayWindow) fresh(seq uint64) bool {
	if !w.inited || seq > w.top {
		return true
	}
	d := w.top - seq
	if d >= 64 {
		return false
	}
	return w.bits&(1<<d) == 0
}

func (w *replayWindow) mark(seq uint64) {
	if !w.inited {
		w.inited = true
		w.top = seq
		w.bits = 1
		return
	}
	if seq > w.top {
		d := seq - w.top
		if d >= 64 {
			w.bits = 1
		} else {
			w.bits = w.bits<<d | 1
		}
		w.top = seq
		return
	}
	d := w.top - seq
	if d < 64 {
		w.bits |= 1 << d
	}
}

type cipherState struct {
	aead cipher.AEAD
	iv   [4]byte
}

func newCipherState(key, iv []byte) (*cipherState, error) {
	block, err := aes.NewCipher(key)
	if err != nil {
		return nil, err
	}
	aead, err := cipher.NewGCM(block)
	if err != nil {
		return nil, err
	}
	c := &cipherState{aead: aead}
	copy(c.iv[:], iv)
	return c, nil
}

func recordAAD(typ uint8, version, epoch uint16, seq uint64, n int) []byte {
	aad := make([]byte, 0, 13)
	aad = appendU16(aad, epoch)
	aad = appendU48(aad, seq)
	aad = append(aad, typ)
	aad = appendU16(aad, version)
	return appendU16(aad, uint16(n))
}

func (c *cipherState) seal(dst []byte, typ uint8, epoch uint16, seq uint64, plaintext []byte) []byte {
	var nonce [12]byte
	copy(nonce[:4], c.iv[:])
	explicit := appendU48(appendU16(nil, epoch), seq)
	copy(nonce[4:], explicit)
	dst = append(dst, typ)
	dst = appendU16(dst, versionDTLS12)
	dst = append(dst, explicit...)
	dst = appendU16(dst, uint16(gcmOverhead+len(plaintext)))
	dst = append(dst, explicit...)
	return c.aead.Seal(dst, nonce[:], plaintext, recordAAD(typ, versionDTLS12, epoch, seq, len(plaintext)))
}

func (c *cipherState) open(rec record) ([]byte, bool) {
	if len(rec.payload) < gcmOverhead {
		return nil, false
	}
	var nonce [12]byte
	copy(nonce[:4], c.iv[:])
	copy(nonce[4:], rec.payload[:gcmExplicitLen])
	n := len(rec.payload) - gcmOverhead
	pt, err := c.aead.Open(nil, nonce[:], rec.payload[gcmExplicitLen:], recordAAD(rec.typ, rec.version, rec.epoch, rec.seq, n))
	if err != nil {
		return nil, false
	}
	return pt, true
}

func plainRecord(dst []byte, typ uint8, epoch uint16, seq uint64, payload []byte) []byte {
	dst = append(dst, typ)
	dst = appendU16(dst, versionDTLS12)
	dst = appendU16(dst, epoch)
	dst = appendU48(dst, seq)
	dst = appendU16(dst, uint16(len(payload)))
	return append(dst, payload...)
}
