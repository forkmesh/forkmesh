package rtp

import (
	"encoding/binary"
	"errors"
	"math"
)

const (
	TCCTickMicros      = 250
	TCCReferenceMicros = 64000
	tccMaxRunLength    = 0x1fff
	tccOneBitCapacity  = 14
	tccTwoBitCapacity  = 7
	tccNotReceived     = 0
	tccSmallDelta      = 1
	tccLargeDelta      = 2
	MaxTCCStatuses     = 0xffff
	MaxParsedStatuses  = 4096
)

var (
	errTCCNotFeedback = errors.New("rtcp: not a transport-wide feedback")
	errTCCShort       = errors.New("rtcp: transport-wide feedback too short")
	errTCCSymbol      = errors.New("rtcp: reserved transport-wide status symbol")
	errTCCDelta       = errors.New("rtcp: transport-wide delta out of range")
	errTCCEmpty       = errors.New("rtcp: transport-wide feedback without statuses")
	errTCCTooMany     = errors.New("rtcp: transport-wide feedback reports too many packets")
)

type FeedbackStatus struct {
	Received bool
	Delta    int32
}

type TransportFeedback struct {
	SenderSSRC    uint32
	MediaSSRC     uint32
	BaseSequence  uint16
	ReferenceTime uint32
	FeedbackCount uint8
	Statuses      []FeedbackStatus
}

func deltaSize(s FeedbackStatus) (int, error) {
	if !s.Received {
		return tccNotReceived, nil
	}
	if s.Delta >= 0 && s.Delta <= 0xff {
		return tccSmallDelta, nil
	}
	if s.Delta >= math.MinInt16 && s.Delta <= math.MaxInt16 {
		return tccLargeDelta, nil
	}
	return 0, errTCCDelta
}

type tccChunker struct {
	sizes   [tccOneBitCapacity]int
	size    int
	allSame bool
	large   bool
	out     []uint16
}

func (c *tccChunker) canAdd(d int) bool {
	if c.size < tccTwoBitCapacity {
		return true
	}
	if c.size < tccOneBitCapacity && !c.large && d != tccLargeDelta {
		return true
	}
	return c.size < tccMaxRunLength && c.allSame && c.sizes[0] == d
}

func (c *tccChunker) add(d int) {
	if !c.canAdd(d) {
		c.out = append(c.out, c.emit())
	}
	if c.size == 0 {
		c.allSame = true
		c.large = false
	}
	if c.size < tccOneBitCapacity {
		c.sizes[c.size] = d
	}
	c.size++
	c.allSame = c.allSame && d == c.sizes[0]
	c.large = c.large || d == tccLargeDelta
}

func (c *tccChunker) runLength() uint16 {
	return uint16(c.sizes[0])<<13 | uint16(c.size)
}

func (c *tccChunker) oneBit() uint16 {
	chunk := uint16(0x8000)
	for i := 0; i < c.size; i++ {
		chunk |= uint16(c.sizes[i]) << (tccOneBitCapacity - 1 - i)
	}
	return chunk
}

func (c *tccChunker) twoBit(n int) uint16 {
	chunk := uint16(0xc000)
	for i := 0; i < n; i++ {
		chunk |= uint16(c.sizes[i]) << (2 * (tccTwoBitCapacity - 1 - i))
	}
	return chunk
}

func (c *tccChunker) emit() uint16 {
	if c.allSame {
		chunk := c.runLength()
		c.size = 0
		return chunk
	}
	if c.size == tccOneBitCapacity {
		chunk := c.oneBit()
		c.size = 0
		return chunk
	}
	chunk := c.twoBit(tccTwoBitCapacity)
	c.size -= tccTwoBitCapacity
	c.allSame = true
	c.large = false
	for i := 0; i < c.size; i++ {
		d := c.sizes[tccTwoBitCapacity+i]
		c.sizes[i] = d
		c.allSame = c.allSame && d == c.sizes[0]
		c.large = c.large || d == tccLargeDelta
	}
	return chunk
}

func (c *tccChunker) last() uint16 {
	if c.allSame {
		return c.runLength()
	}
	if c.size <= tccTwoBitCapacity {
		return c.twoBit(c.size)
	}
	return c.oneBit()
}

func (f *TransportFeedback) Marshal() ([]byte, error) {
	if len(f.Statuses) == 0 {
		return nil, errTCCEmpty
	}
	if len(f.Statuses) > MaxTCCStatuses {
		return nil, errTCCDelta
	}
	var chunker tccChunker
	var deltas []byte
	for _, s := range f.Statuses {
		d, err := deltaSize(s)
		if err != nil {
			return nil, err
		}
		chunker.add(d)
		switch d {
		case tccSmallDelta:
			deltas = append(deltas, byte(s.Delta))
		case tccLargeDelta:
			deltas = binary.BigEndian.AppendUint16(deltas, uint16(int16(s.Delta)))
		}
	}
	chunks := append(chunker.out, chunker.last())
	body := 20 + 2*len(chunks) + len(deltas)
	padding := (4 - body%4) % 4
	p := newPacket(FmtTCC, TypeRTPFB, body+padding)
	if padding > 0 {
		p[0] |= 0x20
		p[len(p)-1] = byte(padding)
	}
	binary.BigEndian.PutUint32(p[4:8], f.SenderSSRC)
	binary.BigEndian.PutUint32(p[8:12], f.MediaSSRC)
	binary.BigEndian.PutUint16(p[12:14], f.BaseSequence)
	binary.BigEndian.PutUint16(p[14:16], uint16(len(f.Statuses)))
	binary.BigEndian.PutUint32(p[16:20], f.ReferenceTime<<8|uint32(f.FeedbackCount))
	o := 20
	for _, chunk := range chunks {
		binary.BigEndian.PutUint16(p[o:], chunk)
		o += 2
	}
	copy(p[o:], deltas)
	return p, nil
}

func ParseTransportFeedback(p []byte) (*TransportFeedback, error) {
	if len(p) < 20 {
		return nil, errTCCShort
	}
	if p[0]>>6 != 2 {
		return nil, errRTCPVer
	}
	if p[1] != TypeRTPFB || p[0]&0x1f != FmtTCC {
		return nil, errTCCNotFeedback
	}
	n := (int(binary.BigEndian.Uint16(p[2:4])) + 1) * 4
	if n > len(p) {
		return nil, errRTCPLength
	}
	end := n
	if p[0]&0x20 != 0 {
		pad := int(p[n-1])
		if pad == 0 || pad > n-20 {
			return nil, errTCCShort
		}
		end -= pad
	}
	f := &TransportFeedback{
		SenderSSRC:    binary.BigEndian.Uint32(p[4:8]),
		MediaSSRC:     binary.BigEndian.Uint32(p[8:12]),
		BaseSequence:  binary.BigEndian.Uint16(p[12:14]),
		ReferenceTime: binary.BigEndian.Uint32(p[16:20]) >> 8,
		FeedbackCount: p[19],
	}
	count := int(binary.BigEndian.Uint16(p[14:16]))
	if count == 0 {
		return nil, errTCCEmpty
	}
	if count > MaxParsedStatuses {
		return nil, errTCCTooMany
	}
	symbols := make([]int, 0, count)
	o := 20
	for len(symbols) < count {
		if o+2 > end {
			return nil, errTCCShort
		}
		chunk := binary.BigEndian.Uint16(p[o:])
		o += 2
		switch {
		case chunk&0x8000 == 0:
			symbol := int(chunk >> 13 & 3)
			for run := int(chunk & 0x1fff); run > 0 && len(symbols) < count; run-- {
				symbols = append(symbols, symbol)
			}
		case chunk&0x4000 == 0:
			for i := 0; i < tccOneBitCapacity && len(symbols) < count; i++ {
				symbols = append(symbols, int(chunk>>(tccOneBitCapacity-1-i)&1))
			}
		default:
			for i := 0; i < tccTwoBitCapacity && len(symbols) < count; i++ {
				symbols = append(symbols, int(chunk>>(2*(tccTwoBitCapacity-1-i))&3))
			}
		}
	}
	f.Statuses = make([]FeedbackStatus, count)
	for i, symbol := range symbols {
		switch symbol {
		case tccSmallDelta:
			if o+1 > end {
				return nil, errTCCShort
			}
			f.Statuses[i] = FeedbackStatus{Received: true, Delta: int32(p[o])}
			o++
		case tccLargeDelta:
			if o+2 > end {
				return nil, errTCCShort
			}
			f.Statuses[i] = FeedbackStatus{Received: true, Delta: int32(int16(binary.BigEndian.Uint16(p[o:])))}
			o += 2
		case tccNotReceived:
		default:
			return nil, errTCCSymbol
		}
	}
	return f, nil
}
