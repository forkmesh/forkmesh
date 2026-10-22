package quic

import (
	"crypto/rand"
	"encoding/binary"
	"errors"
)

const (
	Version1          uint32 = 0x00000001
	maxCIDLen                = 20
	localCIDLen              = 8
	minInitialSize           = 1200
	minDCIDFromClient        = 8
	maxPacketNumber          = 1<<62 - 1
)

type packetType uint8

const (
	packetInitial packetType = iota
	packet0RTT
	packetHandshake
	packetRetry
	packet1RTT
	packetVersionNegotiation
)

func (t packetType) String() string {
	switch t {
	case packetInitial:
		return "Initial"
	case packet0RTT:
		return "0-RTT"
	case packetHandshake:
		return "Handshake"
	case packetRetry:
		return "Retry"
	case packet1RTT:
		return "1-RTT"
	case packetVersionNegotiation:
		return "VersionNegotiation"
	}
	return "unknown"
}

var errMalformed = errors.New("quic: malformed packet")

func isLongHeader(b byte) bool { return b&0x80 != 0 }

type invariantHeader struct {
	long    bool
	version uint32
	dcid    []byte
	scid    []byte
}

func parseInvariant(b []byte, shortCIDLen int) (invariantHeader, bool) {
	var h invariantHeader
	if len(b) == 0 {
		return h, false
	}
	if !isLongHeader(b[0]) {
		if len(b) < 1+shortCIDLen {
			return h, false
		}
		h.dcid = b[1 : 1+shortCIDLen]
		return h, true
	}
	h.long = true
	if len(b) < 7 {
		return h, false
	}
	h.version = binary.BigEndian.Uint32(b[1:5])
	dl := int(b[5])
	p := 6
	if len(b) < p+dl+1 {
		return h, false
	}
	if h.version != 0 && dl > maxCIDLen {
		return h, false
	}
	if dl > 255 {
		return h, false
	}
	h.dcid = b[p : p+dl]
	p += dl
	sl := int(b[p])
	p++
	if len(b) < p+sl {
		return h, false
	}
	if h.version != 0 && sl > maxCIDLen {
		return h, false
	}
	h.scid = b[p : p+sl]
	return h, true
}

type longHeader struct {
	typ      packetType
	version  uint32
	dcid     []byte
	scid     []byte
	token    []byte
	pnOffset int
	end      int
}

func longPacketType(first byte) packetType {
	switch (first >> 4) & 0x03 {
	case 0:
		return packetInitial
	case 1:
		return packet0RTT
	case 2:
		return packetHandshake
	default:
		return packetRetry
	}
}

func parseLongHeader(b []byte) (longHeader, error) {
	var h longHeader
	if len(b) < 7 || !isLongHeader(b[0]) {
		return h, errMalformed
	}
	h.version = binary.BigEndian.Uint32(b[1:5])
	if h.version == 0 {
		h.typ = packetVersionNegotiation
		return h, nil
	}
	if h.version != Version1 {
		return h, errMalformed
	}
	if b[0]&0x40 == 0 {
		return h, errMalformed
	}
	h.typ = longPacketType(b[0])
	p := 5
	dl := int(b[p])
	p++
	if dl > maxCIDLen || len(b) < p+dl+1 {
		return h, errMalformed
	}
	h.dcid = b[p : p+dl]
	p += dl
	sl := int(b[p])
	p++
	if sl > maxCIDLen || len(b) < p+sl {
		return h, errMalformed
	}
	h.scid = b[p : p+sl]
	p += sl
	if h.typ == packetRetry {
		h.pnOffset = p
		h.end = len(b)
		return h, nil
	}
	if h.typ == packetInitial {
		tl, n := ReadVarint(b[p:])
		if n == 0 || tl > uint64(len(b)-p-n) {
			return h, errMalformed
		}
		p += n
		h.token = b[p : p+int(tl)]
		p += int(tl)
	}
	length, n := ReadVarint(b[p:])
	if n == 0 {
		return h, errMalformed
	}
	p += n
	if length > uint64(len(b)-p) {
		return h, errMalformed
	}
	h.pnOffset = p
	h.end = p + int(length)
	return h, nil
}

func decodePacketNumber(largest int64, truncated uint64, pnLen int) int64 {
	expected := largest + 1
	win := int64(1) << (uint(pnLen) * 8)
	hwin := win / 2
	mask := win - 1
	candidate := (expected &^ mask) | int64(truncated)
	if candidate <= expected-hwin && candidate < (1<<62)-win {
		return candidate + win
	}
	if candidate > expected+hwin && candidate >= win {
		return candidate - win
	}
	return candidate
}

func packetNumberLen(pn, largestAcked int64) int {
	var unacked uint64
	if largestAcked < 0 {
		unacked = uint64(pn) + 1
	} else {
		unacked = uint64(pn - largestAcked)
	}
	switch {
	case unacked < 1<<7:
		return 1
	case unacked < 1<<15:
		return 2
	case unacked < 1<<23:
		return 3
	default:
		return 4
	}
}

func appendPacketNumber(b []byte, pn int64, n int) []byte {
	for i := n - 1; i >= 0; i-- {
		b = append(b, byte(pn>>(8*uint(i))))
	}
	return b
}

func readTruncatedPN(b []byte, n int) uint64 {
	var v uint64
	for i := 0; i < n; i++ {
		v = v<<8 | uint64(b[i])
	}
	return v
}

func longTypeBits(t packetType) byte {
	switch t {
	case packetInitial:
		return 0x00
	case packet0RTT:
		return 0x10
	case packetHandshake:
		return 0x20
	default:
		return 0x30
	}
}

func appendLongHeader(b []byte, t packetType, dcid, scid, token []byte, pnLen int) (out []byte, lengthAt int) {
	b = append(b, 0xc0|longTypeBits(t)|byte(pnLen-1))
	b = binary.BigEndian.AppendUint32(b, Version1)
	b = append(b, byte(len(dcid)))
	b = append(b, dcid...)
	b = append(b, byte(len(scid)))
	b = append(b, scid...)
	if t == packetInitial {
		b = AppendVarint(b, uint64(len(token)))
		b = append(b, token...)
	}
	lengthAt = len(b)
	b = append(b, 0x40, 0x00)
	return b, lengthAt
}

func appendShortHeader(b []byte, dcid []byte, pnLen int, keyPhase bool) []byte {
	first := byte(0x40) | byte(pnLen-1)
	if keyPhase {
		first |= 0x04
	}
	b = append(b, first)
	return append(b, dcid...)
}

func appendVersionNegotiation(b []byte, dcid, scid []byte, versions []uint32) []byte {
	var r [1]byte
	rand.Read(r[:])
	b = append(b, 0x80|r[0]|0x40)
	b = append(b, 0, 0, 0, 0)
	b = append(b, byte(len(dcid)))
	b = append(b, dcid...)
	b = append(b, byte(len(scid)))
	b = append(b, scid...)
	for _, v := range versions {
		b = binary.BigEndian.AppendUint32(b, v)
	}
	return b
}

func parseVersionNegotiation(b []byte) (dcid, scid []byte, versions []uint32, ok bool) {
	h, good := parseInvariant(b, 0)
	if !good || !h.long || h.version != 0 {
		return nil, nil, nil, false
	}
	p := 7 + len(h.dcid) + len(h.scid)
	rest := b[p:]
	if len(rest) == 0 || len(rest)%4 != 0 {
		return nil, nil, nil, false
	}
	for i := 0; i+4 <= len(rest) && len(versions) < 64; i += 4 {
		versions = append(versions, binary.BigEndian.Uint32(rest[i:]))
	}
	return h.dcid, h.scid, versions, true
}

func appendRetry(b []byte, dcid, scid, odcid, token []byte) []byte {
	var r [1]byte
	rand.Read(r[:])
	start := len(b)
	b = append(b, 0xf0|(r[0]&0x0f))
	b = binary.BigEndian.AppendUint32(b, Version1)
	b = append(b, byte(len(dcid)))
	b = append(b, dcid...)
	b = append(b, byte(len(scid)))
	b = append(b, scid...)
	b = append(b, token...)
	tag := retryIntegrityTag(odcid, b[start:])
	return append(b, tag...)
}

func greaseVersion() uint32 {
	var r [4]byte
	rand.Read(r[:])
	return binary.BigEndian.Uint32(r[:])&0xf0f0f0f0 | 0x0a0a0a0a
}
