package stun

import (
	"crypto/hmac"
	"crypto/sha1"
	"encoding/binary"
	"errors"
	"hash/crc32"
	"net/netip"
	"unicode/utf8"
)

type Message struct {
	Type          uint16
	TransactionID [12]byte
	Attributes    []Attribute
}

type Attribute struct {
	Type  uint16
	Value []byte
}

const (
	BindingRequest       = 0x0001
	BindingSuccess       = 0x0101
	BindingError         = 0x0111
	AttrMappedAddress    = 0x0001
	AttrUsername         = 0x0006
	AttrMessageIntegrity = 0x0008
	AttrErrorCode        = 0x0009
	AttrXORMappedAddress = 0x0020
	AttrPriority         = 0x0024
	AttrUseCandidate     = 0x0025
	AttrFingerprint      = 0x8028
	AttrICEControlled    = 0x8029
	AttrICEControlling   = 0x802A
)

const (
	headerSize      = 20
	attrHeaderSize  = 4
	magicCookie     = 0x2112A442
	fingerprintXOR  = 0x5354554e
	integritySize   = 20
	fingerprintSize = 4
	maxBody         = 0xFFFF &^ 3
	maxReason       = 763
	familyIPv4      = 0x01
	familyIPv6      = 0x02
)

var (
	errShort        = errors.New("stun: message too short")
	errTopBits      = errors.New("stun: first two bits not zero")
	errCookie       = errors.New("stun: bad magic cookie")
	errLength       = errors.New("stun: length does not match message size")
	errAlignment    = errors.New("stun: length not a multiple of 4")
	errAttrOverrun  = errors.New("stun: attribute overruns message")
	errFingerprint  = errors.New("stun: attribute after FINGERPRINT")
	errAddrLength   = errors.New("stun: bad address length")
	errAddrFamily   = errors.New("stun: unknown address family")
	errAttrTruncate = errors.New("stun: truncated attribute header")
)

func pad4(n int) int {
	return (n + 3) &^ 3
}

func IsMessage(b []byte) bool {
	if len(b) < headerSize {
		return false
	}
	if b[0]&0xC0 != 0 {
		return false
	}
	if binary.BigEndian.Uint32(b[4:8]) != magicCookie {
		return false
	}
	length := int(binary.BigEndian.Uint16(b[2:4]))
	return length%4 == 0 && len(b) == headerSize+length
}

func checkHeader(b []byte) error {
	if len(b) < headerSize {
		return errShort
	}
	if b[0]&0xC0 != 0 {
		return errTopBits
	}
	if binary.BigEndian.Uint32(b[4:8]) != magicCookie {
		return errCookie
	}
	length := int(binary.BigEndian.Uint16(b[2:4]))
	if length%4 != 0 {
		return errAlignment
	}
	if len(b) != headerSize+length {
		return errLength
	}
	return nil
}

type rawAttr struct {
	typ   uint16
	start int
	vlen  int
	end   int
}

func walk(b []byte, fn func(a rawAttr) bool) error {
	if err := checkHeader(b); err != nil {
		return err
	}
	off := headerSize
	for off < len(b) {
		if len(b)-off < attrHeaderSize {
			return errAttrTruncate
		}
		typ := binary.BigEndian.Uint16(b[off : off+2])
		vlen := int(binary.BigEndian.Uint16(b[off+2 : off+4]))
		start := off + attrHeaderSize
		padded := pad4(vlen)
		if padded > len(b)-start {
			return errAttrOverrun
		}
		end := start + padded
		if !fn(rawAttr{typ: typ, start: start, vlen: vlen, end: end}) {
			return nil
		}
		off = end
	}
	return nil
}

func Parse(b []byte) (*Message, error) {
	if err := checkHeader(b); err != nil {
		return nil, err
	}
	body := make([]byte, len(b)-headerSize)
	copy(body, b[headerSize:])
	m := &Message{Type: binary.BigEndian.Uint16(b[0:2])}
	copy(m.TransactionID[:], b[8:20])
	seenIntegrity := false
	seenFingerprint := false
	var failure error
	err := walk(b, func(a rawAttr) bool {
		if seenFingerprint {
			failure = errFingerprint
			return false
		}
		if a.typ == AttrFingerprint {
			seenFingerprint = true
		} else if seenIntegrity {
			return true
		}
		if a.typ == AttrMessageIntegrity {
			seenIntegrity = true
		}
		s := a.start - headerSize
		v := body[s : s+a.vlen : s+a.vlen]
		m.Attributes = append(m.Attributes, Attribute{Type: a.typ, Value: v})
		return true
	})
	if err != nil {
		return nil, err
	}
	if failure != nil {
		return nil, failure
	}
	return m, nil
}

func (m *Message) Get(t uint16) ([]byte, bool) {
	for _, a := range m.Attributes {
		if a.Type == t {
			return a.Value, true
		}
	}
	return nil, false
}

func (m *Message) Add(t uint16, v []byte) {
	c := make([]byte, len(v))
	copy(c, v)
	m.Attributes = append(m.Attributes, Attribute{Type: t, Value: c})
}

func (m *Message) AddXORMappedAddress(addr netip.AddrPort) {
	ip := addr.Addr().Unmap()
	if !ip.IsValid() {
		return
	}
	port := addr.Port() ^ uint16(magicCookie>>16)
	var key [16]byte
	binary.BigEndian.PutUint32(key[0:4], magicCookie)
	copy(key[4:], m.TransactionID[:])
	if ip.Is4() {
		a := ip.As4()
		v := make([]byte, 8)
		v[1] = familyIPv4
		binary.BigEndian.PutUint16(v[2:4], port)
		for i := 0; i < 4; i++ {
			v[4+i] = a[i] ^ key[i]
		}
		m.Attributes = append(m.Attributes, Attribute{Type: AttrXORMappedAddress, Value: v})
		return
	}
	a := ip.As16()
	v := make([]byte, 20)
	v[1] = familyIPv6
	binary.BigEndian.PutUint16(v[2:4], port)
	for i := 0; i < 16; i++ {
		v[4+i] = a[i] ^ key[i]
	}
	m.Attributes = append(m.Attributes, Attribute{Type: AttrXORMappedAddress, Value: v})
}

func (m *Message) AddErrorCode(code int, reason string) {
	if code < 300 || code > 699 {
		code = 500
	}
	if len(reason) > maxReason {
		cut := maxReason
		for cut > 0 && !utf8.RuneStart(reason[cut]) {
			cut--
		}
		reason = reason[:cut]
	}
	v := make([]byte, 4+len(reason))
	v[2] = byte(code / 100)
	v[3] = byte(code % 100)
	copy(v[4:], reason)
	m.Attributes = append(m.Attributes, Attribute{Type: AttrErrorCode, Value: v})
}

func (m *Message) Encode(integrityKey []byte, fingerprint bool) []byte {
	reserve := 0
	if integrityKey != nil {
		reserve += attrHeaderSize + integritySize
	}
	if fingerprint {
		reserve += attrHeaderSize + fingerprintSize
	}
	size := 0
	for _, a := range m.Attributes {
		if a.Type == AttrMessageIntegrity || a.Type == AttrFingerprint || len(a.Value) > 0xFFFF {
			continue
		}
		n := attrHeaderSize + pad4(len(a.Value))
		if size+n+reserve > maxBody {
			continue
		}
		size += n
	}
	buf := make([]byte, headerSize, headerSize+size+reserve)
	binary.BigEndian.PutUint16(buf[0:2], m.Type&0x3FFF)
	binary.BigEndian.PutUint32(buf[4:8], magicCookie)
	copy(buf[8:20], m.TransactionID[:])
	body := 0
	for _, a := range m.Attributes {
		if a.Type == AttrMessageIntegrity || a.Type == AttrFingerprint || len(a.Value) > 0xFFFF {
			continue
		}
		n := attrHeaderSize + pad4(len(a.Value))
		if body+n+reserve > maxBody {
			continue
		}
		body += n
		buf = appendAttr(buf, a.Type, a.Value)
	}
	if integrityKey != nil {
		setLength(buf, len(buf)-headerSize+attrHeaderSize+integritySize)
		mac := hmac.New(sha1.New, integrityKey)
		mac.Write(buf)
		buf = appendAttr(buf, AttrMessageIntegrity, mac.Sum(nil))
	}
	if fingerprint {
		setLength(buf, len(buf)-headerSize+attrHeaderSize+fingerprintSize)
		var v [4]byte
		binary.BigEndian.PutUint32(v[:], crc32.ChecksumIEEE(buf)^fingerprintXOR)
		buf = appendAttr(buf, AttrFingerprint, v[:])
	}
	setLength(buf, len(buf)-headerSize)
	return buf
}

func appendAttr(buf []byte, t uint16, v []byte) []byte {
	var h [4]byte
	binary.BigEndian.PutUint16(h[0:2], t)
	binary.BigEndian.PutUint16(h[2:4], uint16(len(v)))
	buf = append(buf, h[:]...)
	buf = append(buf, v...)
	for i := len(v); i%4 != 0; i++ {
		buf = append(buf, 0)
	}
	return buf
}

func setLength(buf []byte, n int) {
	binary.BigEndian.PutUint16(buf[2:4], uint16(n))
}

func CheckIntegrity(raw []byte, key []byte) bool {
	if len(key) == 0 {
		return false
	}
	found := false
	var mi rawAttr
	trailing := 0
	ok := true
	err := walk(raw, func(a rawAttr) bool {
		if !found {
			if a.typ == AttrMessageIntegrity {
				found = true
				mi = a
			}
			return true
		}
		trailing++
		if trailing > 1 || a.typ != AttrFingerprint || a.vlen != fingerprintSize {
			ok = false
			return false
		}
		return true
	})
	if err != nil || !found || !ok || mi.vlen != integritySize {
		return false
	}
	hdrStart := mi.start - attrHeaderSize
	var hdr [4]byte
	copy(hdr[:], raw[0:4])
	binary.BigEndian.PutUint16(hdr[2:4], uint16(mi.end-headerSize))
	mac := hmac.New(sha1.New, key)
	mac.Write(hdr[:])
	mac.Write(raw[4:hdrStart])
	return hmac.Equal(mac.Sum(nil), raw[mi.start:mi.start+integritySize])
}

func CheckFingerprint(raw []byte) bool {
	var last rawAttr
	count := 0
	err := walk(raw, func(a rawAttr) bool {
		last = a
		count++
		return true
	})
	if err != nil || count == 0 {
		return false
	}
	if last.typ != AttrFingerprint || last.vlen != fingerprintSize || last.end != len(raw) {
		return false
	}
	hdrStart := last.start - attrHeaderSize
	want := binary.BigEndian.Uint32(raw[last.start : last.start+fingerprintSize])
	return crc32.ChecksumIEEE(raw[:hdrStart])^fingerprintXOR == want
}

func DecodeXORAddress(value []byte, tid [12]byte) (netip.AddrPort, error) {
	if len(value) < 4 {
		return netip.AddrPort{}, errAddrLength
	}
	var key [16]byte
	binary.BigEndian.PutUint32(key[0:4], magicCookie)
	copy(key[4:], tid[:])
	port := binary.BigEndian.Uint16(value[2:4]) ^ uint16(magicCookie>>16)
	switch value[1] {
	case familyIPv4:
		if len(value) != 8 {
			return netip.AddrPort{}, errAddrLength
		}
		var a [4]byte
		for i := 0; i < 4; i++ {
			a[i] = value[4+i] ^ key[i]
		}
		return netip.AddrPortFrom(netip.AddrFrom4(a), port), nil
	case familyIPv6:
		if len(value) != 20 {
			return netip.AddrPort{}, errAddrLength
		}
		var a [16]byte
		for i := 0; i < 16; i++ {
			a[i] = value[4+i] ^ key[i]
		}
		return netip.AddrPortFrom(netip.AddrFrom16(a), port), nil
	}
	return netip.AddrPort{}, errAddrFamily
}
