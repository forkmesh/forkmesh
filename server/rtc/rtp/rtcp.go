package rtp

import (
	"encoding/binary"
	"errors"
	"slices"
)

const (
	TypeSR    = 200
	TypeRR    = 201
	TypeSDES  = 202
	TypeBYE   = 203
	TypeRTPFB = 205
	TypePSFB  = 206
	FmtNACK   = 1
	FmtPLI    = 1
	FmtFIR    = 4
	FmtREMB   = 15
	FmtTCC    = 15
)

var (
	errEmpty      = errors.New("rtcp: empty compound packet")
	errRTCPShort  = errors.New("rtcp: packet too short")
	errRTCPVer    = errors.New("rtcp: version is not 2")
	errRTCPLength = errors.New("rtcp: length field overruns packet")
	errNotNACK    = errors.New("rtcp: not a generic nack")
	errNACKFCI    = errors.New("rtcp: malformed nack fci")
	errNotReport  = errors.New("rtcp: not a sender or receiver report")
)

func SplitCompound(b []byte) ([][]byte, error) {
	if len(b) == 0 {
		return nil, errEmpty
	}
	var out [][]byte
	for len(b) > 0 {
		if len(b) < 4 {
			return nil, errRTCPShort
		}
		if b[0]>>6 != 2 {
			return nil, errRTCPVer
		}
		n := (int(binary.BigEndian.Uint16(b[2:4])) + 1) * 4
		if n > len(b) {
			return nil, errRTCPLength
		}
		out = append(out, b[:n:n])
		b = b[n:]
	}
	return out, nil
}

func PacketType(p []byte) uint8 {
	if len(p) < 2 {
		return 0
	}
	return p[1]
}

func Count(p []byte) uint8 {
	if len(p) < 1 {
		return 0
	}
	return p[0] & 0x1f
}

func SenderSSRC(p []byte) uint32 {
	if len(p) < 8 {
		return 0
	}
	return binary.BigEndian.Uint32(p[4:8])
}

func isFIR(p []byte) bool {
	return len(p) >= 20 && p[1] == TypePSFB && p[0]&0x1f == FmtFIR
}

func MediaSSRC(p []byte) uint32 {
	if len(p) < 12 {
		return 0
	}
	m := binary.BigEndian.Uint32(p[8:12])
	if m == 0 && isFIR(p) {
		return binary.BigEndian.Uint32(p[12:16])
	}
	return m
}

func SetMediaSSRC(p []byte, ssrc uint32) {
	if len(p) < 12 {
		return
	}
	if isFIR(p) && binary.BigEndian.Uint32(p[8:12]) == 0 {
		binary.BigEndian.PutUint32(p[12:16], ssrc)
		return
	}
	binary.BigEndian.PutUint32(p[8:12], ssrc)
}

func newPacket(count, typ uint8, size int) []byte {
	p := make([]byte, size)
	p[0] = 0x80 | count&0x1f
	p[1] = typ
	binary.BigEndian.PutUint16(p[2:4], uint16(size/4-1))
	return p
}

func NewPLI(sender, media uint32) []byte {
	p := newPacket(FmtPLI, TypePSFB, 12)
	binary.BigEndian.PutUint32(p[4:8], sender)
	binary.BigEndian.PutUint32(p[8:12], media)
	return p
}

func NewFIR(sender, media uint32, seq uint8) []byte {
	p := newPacket(FmtFIR, TypePSFB, 20)
	binary.BigEndian.PutUint32(p[4:8], sender)
	binary.BigEndian.PutUint32(p[12:16], media)
	p[16] = seq
	return p
}

func NewREMB(sender uint32, bitrate uint64, ssrcs []uint32) []byte {
	if len(ssrcs) > 255 {
		ssrcs = ssrcs[:255]
	}
	p := newPacket(FmtREMB, TypePSFB, 20+4*len(ssrcs))
	binary.BigEndian.PutUint32(p[4:8], sender)
	copy(p[12:16], "REMB")
	exp := uint32(0)
	for bitrate >= 1<<18 {
		bitrate >>= 1
		exp++
	}
	p[16] = uint8(len(ssrcs))
	v := exp<<18 | uint32(bitrate)
	p[17] = uint8(v >> 16)
	p[18] = uint8(v >> 8)
	p[19] = uint8(v)
	for i, s := range ssrcs {
		binary.BigEndian.PutUint32(p[20+4*i:24+4*i], s)
	}
	return p
}

type ReportBlock struct {
	SSRC         uint32
	FractionLost uint8
	Lost         int32
	HighestSeq   uint32
	Jitter       uint32
	LSR          uint32
	DLSR         uint32
}

func NewReceiverReport(sender uint32, blocks ...ReportBlock) []byte {
	if len(blocks) > 31 {
		blocks = blocks[:31]
	}
	p := newPacket(uint8(len(blocks)), TypeRR, 8+24*len(blocks))
	binary.BigEndian.PutUint32(p[4:8], sender)
	for i, b := range blocks {
		o := 8 + 24*i
		binary.BigEndian.PutUint32(p[o:], b.SSRC)
		lost := b.Lost
		if lost > 0x7fffff {
			lost = 0x7fffff
		}
		if lost < -0x800000 {
			lost = -0x800000
		}
		binary.BigEndian.PutUint32(p[o+4:], uint32(b.FractionLost)<<24|uint32(lost)&0xffffff)
		binary.BigEndian.PutUint32(p[o+8:], b.HighestSeq)
		binary.BigEndian.PutUint32(p[o+12:], b.Jitter)
		binary.BigEndian.PutUint32(p[o+16:], b.LSR)
		binary.BigEndian.PutUint32(p[o+20:], b.DLSR)
	}
	return p
}

func ParseReportBlocks(p []byte) ([]ReportBlock, error) {
	if len(p) < 8 {
		return nil, errRTCPShort
	}
	offset := 8
	switch PacketType(p) {
	case TypeSR:
		offset = 28
	case TypeRR:
	default:
		return nil, errNotReport
	}
	n := int(Count(p))
	if len(p) < offset+24*n {
		return nil, errRTCPLength
	}
	blocks := make([]ReportBlock, n)
	for i := range blocks {
		o := offset + 24*i
		word := binary.BigEndian.Uint32(p[o+4:])
		lost := int32(word&0xffffff) << 8 >> 8
		blocks[i] = ReportBlock{
			SSRC:         binary.BigEndian.Uint32(p[o:]),
			FractionLost: uint8(word >> 24),
			Lost:         lost,
			HighestSeq:   binary.BigEndian.Uint32(p[o+8:]),
			Jitter:       binary.BigEndian.Uint32(p[o+12:]),
			LSR:          binary.BigEndian.Uint32(p[o+16:]),
			DLSR:         binary.BigEndian.Uint32(p[o+20:]),
		}
	}
	return blocks, nil
}

func SenderReportNTP(p []byte) (uint64, bool) {
	if len(p) < 28 || PacketType(p) != TypeSR {
		return 0, false
	}
	return binary.BigEndian.Uint64(p[8:16]), true
}

func ParseNACK(p []byte) (uint32, []uint16, error) {
	if len(p) < 12 {
		return 0, nil, errRTCPShort
	}
	if p[0]>>6 != 2 {
		return 0, nil, errRTCPVer
	}
	if p[1] != TypeRTPFB || p[0]&0x1f != FmtNACK {
		return 0, nil, errNotNACK
	}
	n := (int(binary.BigEndian.Uint16(p[2:4])) + 1) * 4
	if n > len(p) {
		return 0, nil, errRTCPLength
	}
	fci := p[12:n]
	if len(fci) == 0 || len(fci)%4 != 0 {
		return 0, nil, errNACKFCI
	}
	media := binary.BigEndian.Uint32(p[8:12])
	var lost []uint16
	for i := 0; i+4 <= len(fci); i += 4 {
		pid := binary.BigEndian.Uint16(fci[i : i+2])
		blp := binary.BigEndian.Uint16(fci[i+2 : i+4])
		lost = append(lost, pid)
		for bit := 0; bit < 16; bit++ {
			if blp&(1<<bit) != 0 {
				lost = append(lost, pid+uint16(bit)+1)
			}
		}
	}
	return media, lost, nil
}

func circularOrder(lost []uint16) []uint16 {
	s := slices.Clone(lost)
	slices.Sort(s)
	s = slices.Compact(s)
	if len(s) < 2 {
		return s
	}
	start := 0
	best := int(s[0]) + 65536 - int(s[len(s)-1])
	for i := 1; i < len(s); i++ {
		if g := int(s[i]) - int(s[i-1]); g > best {
			best = g
			start = i
		}
	}
	return append(s[start:], s[:start]...)
}

func NewNACK(sender, media uint32, lost []uint16) []byte {
	seqs := circularOrder(lost)
	if len(seqs) == 0 {
		return nil
	}
	var fci []byte
	for i := 0; i < len(seqs); {
		pid := seqs[i]
		var blp uint16
		i++
		for i < len(seqs) {
			d := seqs[i] - pid
			if d < 1 || d > 16 {
				break
			}
			blp |= 1 << (d - 1)
			i++
		}
		fci = binary.BigEndian.AppendUint16(fci, pid)
		fci = binary.BigEndian.AppendUint16(fci, blp)
	}
	p := newPacket(FmtNACK, TypeRTPFB, 12+len(fci))
	binary.BigEndian.PutUint32(p[4:8], sender)
	binary.BigEndian.PutUint32(p[8:12], media)
	copy(p[12:], fci)
	return p
}
