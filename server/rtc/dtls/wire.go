package dtls

import (
	"encoding/binary"
	"fmt"
)

const (
	contentChangeCipherSpec = 20
	contentAlert            = 21
	contentHandshake        = 22
	contentApplicationData  = 23
)

const (
	hsClientHello        = 1
	hsServerHello        = 2
	hsHelloVerifyRequest = 3
	hsCertificate        = 11
	hsServerKeyExchange  = 12
	hsCertificateRequest = 13
	hsServerHelloDone    = 14
	hsCertificateVerify  = 15
	hsClientKeyExchange  = 16
	hsFinished           = 20
)

const (
	versionDTLS10 = 0xfeff
	versionDTLS12 = 0xfefd
)

const (
	suiteECDHEECDSAAES128GCMSHA256 = 0xc02b
	suiteRenegotiationSCSV         = 0x00ff
)

const (
	extSupportedGroups     = 0x000a
	extECPointFormats      = 0x000b
	extSignatureAlgorithms = 0x000d
	extUseSRTP             = 0x000e
	extExtendedMasterSec   = 0x0017
	extRenegotiationInfo   = 0xff01
)

const (
	curveSecp256r1 = 23

	sigECDSASecp256r1SHA256 = 0x0403
	sigECDSASecp384r1SHA384 = 0x0503
	sigRSAPKCS1SHA256       = 0x0401

	certTypeRSASign   = 1
	certTypeECDSASign = 64
)

const (
	alertLevelWarning = 1
	alertLevelFatal   = 2

	alertCloseNotify            = 0
	alertUnexpectedMessage      = 10
	alertHandshakeFailure       = 40
	alertBadCertificate         = 42
	alertUnsupportedCertificate = 43
	alertIllegalParameter       = 47
	alertDecodeError            = 50
	alertDecryptError           = 51
	alertProtocolVersion        = 70
	alertInternalError          = 80
)

const (
	recordHeaderLen    = 13
	handshakeHeaderLen = 12
	gcmExplicitLen     = 8
	gcmTagLen          = 16
	gcmOverhead        = gcmExplicitLen + gcmTagLen
	maxPlaintext       = 16384
	maxCiphertext      = maxPlaintext + 2048
	maxMessageLen      = 16384
	maxPendingMessages = 8
	maxEarlyRecords    = 8
	maxEarlyRecordLen  = 2048
	maxSeq             = 1<<48 - 1
	defaultMTU         = 1200
	minMTU             = 128
	minFragment        = 64
)

type alertError struct {
	desc uint8
	msg  string
}

func (e *alertError) Error() string {
	return "dtls: " + e.msg
}

func fatal(desc uint8, format string, args ...any) error {
	return &alertError{desc: desc, msg: fmt.Sprintf(format, args...)}
}

type peerAlertError struct {
	desc uint8
}

func (e *peerAlertError) Error() string {
	return fmt.Sprintf("dtls: peer sent fatal alert %d", e.desc)
}

type reader struct {
	b  []byte
	ok bool
}

func newReader(b []byte) *reader {
	return &reader{b: b, ok: true}
}

func (r *reader) take(n int) []byte {
	if !r.ok || n < 0 || n > len(r.b) {
		r.ok = false
		return nil
	}
	v := r.b[:n:n]
	r.b = r.b[n:]
	return v
}

func (r *reader) u8() uint8 {
	v := r.take(1)
	if v == nil {
		return 0
	}
	return v[0]
}

func (r *reader) u16() uint16 {
	v := r.take(2)
	if v == nil {
		return 0
	}
	return binary.BigEndian.Uint16(v)
}

func (r *reader) u24() int {
	v := r.take(3)
	if v == nil {
		return 0
	}
	return int(v[0])<<16 | int(v[1])<<8 | int(v[2])
}

func (r *reader) vec8() []byte {
	return r.take(int(r.u8()))
}

func (r *reader) vec16() []byte {
	return r.take(int(r.u16()))
}

func (r *reader) vec24() []byte {
	return r.take(r.u24())
}

func (r *reader) empty() bool {
	return r.ok && len(r.b) == 0
}

func appendU16(b []byte, v uint16) []byte {
	return append(b, byte(v>>8), byte(v))
}

func appendU24(b []byte, v int) []byte {
	return append(b, byte(v>>16), byte(v>>8), byte(v))
}

func appendU48(b []byte, v uint64) []byte {
	return append(b, byte(v>>40), byte(v>>32), byte(v>>24), byte(v>>16), byte(v>>8), byte(v))
}

func appendVec8(b, v []byte) []byte {
	return append(append(b, byte(len(v))), v...)
}

func appendVec16(b, v []byte) []byte {
	return append(appendU16(b, uint16(len(v))), v...)
}

func appendVec24(b, v []byte) []byte {
	return append(appendU24(b, len(v)), v...)
}

func appendHandshakeHeader(b []byte, typ uint8, length int, seq uint16, offset, fragLen int) []byte {
	b = append(b, typ)
	b = appendU24(b, length)
	b = appendU16(b, seq)
	b = appendU24(b, offset)
	return appendU24(b, fragLen)
}

func marshalHandshake(typ uint8, seq uint16, body []byte) []byte {
	b := make([]byte, 0, handshakeHeaderLen+len(body))
	b = appendHandshakeHeader(b, typ, len(body), seq, 0, len(body))
	return append(b, body...)
}

type record struct {
	typ     uint8
	version uint16
	epoch   uint16
	seq     uint64
	payload []byte
}

func nextRecord(b []byte) (record, []byte, bool) {
	if len(b) < recordHeaderLen {
		return record{}, nil, false
	}
	n := int(binary.BigEndian.Uint16(b[11:13]))
	if recordHeaderLen+n > len(b) {
		return record{}, nil, false
	}
	rec := record{
		typ:     b[0],
		version: binary.BigEndian.Uint16(b[1:3]),
		epoch:   binary.BigEndian.Uint16(b[3:5]),
		seq:     uint64(b[5])<<40 | uint64(b[6])<<32 | uint64(b[7])<<24 | uint64(b[8])<<16 | uint64(b[9])<<8 | uint64(b[10]),
		payload: b[recordHeaderLen : recordHeaderLen+n : recordHeaderLen+n],
	}
	return rec, b[recordHeaderLen+n:], true
}

type fragment struct {
	typ    uint8
	length int
	seq    uint16
	offset int
	data   []byte
}

func parseFragments(b []byte) ([]fragment, bool) {
	var out []fragment
	for len(b) > 0 {
		r := newReader(b)
		f := fragment{typ: r.u8(), length: r.u24(), seq: r.u16(), offset: r.u24()}
		f.data = r.vec24()
		if !r.ok || f.offset+len(f.data) > f.length {
			return nil, false
		}
		out = append(out, f)
		b = r.b
	}
	return out, len(out) > 0
}
