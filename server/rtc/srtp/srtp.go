package srtp

import (
	"crypto/aes"
	"crypto/cipher"
	"crypto/hmac"
	"crypto/sha1"
	"errors"
	"hash"
	"sync"
)

type Profile uint16

const (
	ProfileAES128CMHMACSHA1_80 Profile = 0x0001
	ProfileAEADAES128GCM       Profile = 0x0007
)

const (
	labelRTPEncryption  byte = 0x00
	labelRTPAuth        byte = 0x01
	labelRTPSalt        byte = 0x02
	labelRTCPEncryption byte = 0x03
	labelRTCPAuth       byte = 0x04
	labelRTCPSalt       byte = 0x05

	masterKeyLength = 16
	cmSaltLength    = 14
	gcmSaltLength   = 12
	authKeyLength   = 20
	cmTagLength     = 10
	gcmTagLength    = 16
	srtcpIndexSize  = 4
	rtpHeaderMin    = 12
	rtcpHeaderMin   = 8
	maxStreams      = 4096
	maxROC          = 0xFFFFFFFF
	maxSRTCPIndex   = 0x7FFFFFFF
)

var (
	errUnknownProfile = errors.New("srtp: unknown protection profile")
	errKeyLength      = errors.New("srtp: wrong master key or salt length")
	errMaterialLength = errors.New("srtp: wrong keying material length")
	errShortPacket    = errors.New("srtp: packet too short")
	errVersion        = errors.New("srtp: not an RTP version 2 packet")
	errAuth           = errors.New("srtp: authentication failed")
	errReplay         = errors.New("srtp: replayed or too old packet")
	errExhausted      = errors.New("srtp: packet index exhausted, rekey required")
	errTooManyStreams = errors.New("srtp: too many SSRCs in one context")
)

func saltLength(p Profile) (int, error) {
	switch p {
	case ProfileAES128CMHMACSHA1_80:
		return cmSaltLength, nil
	case ProfileAEADAES128GCM:
		return gcmSaltLength, nil
	}
	return 0, errUnknownProfile
}

func KeyingMaterialLength(p Profile) int {
	s, err := saltLength(p)
	if err != nil {
		return 0
	}
	return 2 * (masterKeyLength + s)
}

func SplitKeyingMaterial(p Profile, material []byte) (clientKey, clientSalt, serverKey, serverSalt []byte, err error) {
	s, err := saltLength(p)
	if err != nil {
		return nil, nil, nil, nil, err
	}
	if len(material) != 2*(masterKeyLength+s) {
		return nil, nil, nil, nil, errMaterialLength
	}
	m := append([]byte(nil), material...)
	k := masterKeyLength
	clientKey = m[0:k:k]
	serverKey = m[k : 2*k : 2*k]
	clientSalt = m[2*k : 2*k+s : 2*k+s]
	serverSalt = m[2*k+s : 2*k+2*s : 2*k+2*s]
	return clientKey, clientSalt, serverKey, serverSalt, nil
}

type sessionKeys struct {
	rtpKey   []byte
	rtpSalt  []byte
	rtpAuth  []byte
	rtcpKey  []byte
	rtcpSalt []byte
	rtcpAuth []byte
}

type txRTPState struct {
	index uint64
}

type rxState struct {
	window replayWindow
}

type Context struct {
	mu       sync.Mutex
	profile  Profile
	rtpSalt  []byte
	rtcpSalt []byte
	rtpBlock cipher.Block
	rtcpBlk  cipher.Block
	rtpMAC   hash.Hash
	rtcpMAC  hash.Hash
	rtpGCM   cipher.AEAD
	rtcpGCM  cipher.AEAD
	txRTP    map[uint32]*txRTPState
	rxRTP    map[uint32]*rxState
	txRTCP   map[uint32]uint32
	rxRTCP   map[uint32]*rxState
	macBuf   [sha1.Size]byte
}

func NewContext(p Profile, masterKey, masterSalt []byte) (*Context, error) {
	s, err := saltLength(p)
	if err != nil {
		return nil, err
	}
	if len(masterKey) != masterKeyLength || len(masterSalt) != s {
		return nil, errKeyLength
	}
	keys, err := deriveSessionKeys(p, masterKey, masterSalt)
	if err != nil {
		return nil, err
	}
	return newContextFromSessionKeys(p, keys)
}

type derivation struct {
	out   *[]byte
	label byte
	n     int
}

func deriveSessionKeys(p Profile, masterKey, masterSalt []byte) (sessionKeys, error) {
	s, err := saltLength(p)
	if err != nil {
		return sessionKeys{}, err
	}
	var k sessionKeys
	steps := []derivation{
		{&k.rtpKey, labelRTPEncryption, masterKeyLength},
		{&k.rtpSalt, labelRTPSalt, s},
		{&k.rtcpKey, labelRTCPEncryption, masterKeyLength},
		{&k.rtcpSalt, labelRTCPSalt, s},
	}
	if p == ProfileAES128CMHMACSHA1_80 {
		steps = append(steps,
			derivation{&k.rtpAuth, labelRTPAuth, authKeyLength},
			derivation{&k.rtcpAuth, labelRTCPAuth, authKeyLength},
		)
	}
	for _, st := range steps {
		v, err := prf(masterKey, masterSalt, st.label, st.n)
		if err != nil {
			return sessionKeys{}, err
		}
		*st.out = v
	}
	return k, nil
}

func prf(masterKey, masterSalt []byte, label byte, n int) ([]byte, error) {
	block, err := aes.NewCipher(masterKey)
	if err != nil {
		return nil, err
	}
	if len(masterSalt) > 14 {
		return nil, errKeyLength
	}
	var iv [aes.BlockSize]byte
	copy(iv[:], masterSalt)
	iv[7] ^= label
	out := make([]byte, n)
	cipher.NewCTR(block, iv[:]).XORKeyStream(out, out)
	return out, nil
}

func newContextFromSessionKeys(p Profile, k sessionKeys) (*Context, error) {
	c := &Context{
		profile:  p,
		rtpSalt:  append([]byte(nil), k.rtpSalt...),
		rtcpSalt: append([]byte(nil), k.rtcpSalt...),
		txRTP:    map[uint32]*txRTPState{},
		rxRTP:    map[uint32]*rxState{},
		txRTCP:   map[uint32]uint32{},
		rxRTCP:   map[uint32]*rxState{},
	}
	var err error
	if c.rtpBlock, err = aes.NewCipher(k.rtpKey); err != nil {
		return nil, err
	}
	if c.rtcpBlk, err = aes.NewCipher(k.rtcpKey); err != nil {
		return nil, err
	}
	switch p {
	case ProfileAES128CMHMACSHA1_80:
		if len(c.rtpSalt) != cmSaltLength || len(c.rtcpSalt) != cmSaltLength || len(k.rtpAuth) == 0 || len(k.rtcpAuth) == 0 {
			return nil, errKeyLength
		}
		c.rtpMAC = hmac.New(sha1.New, k.rtpAuth)
		c.rtcpMAC = hmac.New(sha1.New, k.rtcpAuth)
	case ProfileAEADAES128GCM:
		if len(c.rtpSalt) != gcmSaltLength || len(c.rtcpSalt) != gcmSaltLength {
			return nil, errKeyLength
		}
		if c.rtpGCM, err = cipher.NewGCM(c.rtpBlock); err != nil {
			return nil, err
		}
		if c.rtcpGCM, err = cipher.NewGCM(c.rtcpBlk); err != nil {
			return nil, err
		}
	default:
		return nil, errUnknownProfile
	}
	return c, nil
}

func grow(dst []byte, n int) []byte {
	if cap(dst) >= n {
		return dst[:n]
	}
	return make([]byte, n)
}

func cmIV(salt []byte, ssrc uint32, index uint64) [aes.BlockSize]byte {
	var iv [aes.BlockSize]byte
	copy(iv[:], salt)
	iv[4] ^= byte(ssrc >> 24)
	iv[5] ^= byte(ssrc >> 16)
	iv[6] ^= byte(ssrc >> 8)
	iv[7] ^= byte(ssrc)
	iv[8] ^= byte(index >> 40)
	iv[9] ^= byte(index >> 32)
	iv[10] ^= byte(index >> 24)
	iv[11] ^= byte(index >> 16)
	iv[12] ^= byte(index >> 8)
	iv[13] ^= byte(index)
	return iv
}

func cmXOR(block cipher.Block, salt []byte, ssrc uint32, index uint64, dst, src []byte) {
	if len(src) == 0 {
		return
	}
	iv := cmIV(salt, ssrc, index)
	cipher.NewCTR(block, iv[:]).XORKeyStream(dst, src)
}

func gcmNonce(salt []byte, ssrc uint32, mid uint32, low uint16) [gcmSaltLength]byte {
	var n [gcmSaltLength]byte
	n[2] = byte(ssrc >> 24)
	n[3] = byte(ssrc >> 16)
	n[4] = byte(ssrc >> 8)
	n[5] = byte(ssrc)
	n[6] = byte(mid >> 24)
	n[7] = byte(mid >> 16)
	n[8] = byte(mid >> 8)
	n[9] = byte(mid)
	n[10] = byte(low >> 8)
	n[11] = byte(low)
	for i := range n {
		n[i] ^= salt[i]
	}
	return n
}

func be32(b []byte) uint32 {
	return uint32(b[0])<<24 | uint32(b[1])<<16 | uint32(b[2])<<8 | uint32(b[3])
}

func put32(b []byte, v uint32) {
	b[0] = byte(v >> 24)
	b[1] = byte(v >> 16)
	b[2] = byte(v >> 8)
	b[3] = byte(v)
}
