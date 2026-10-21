package wire

import (
	"encoding/binary"
	"errors"
)

const (
	Version = 1

	KindMedia    = 0
	KindRepair   = 1
	KindPadding  = 2
	KindFeedback = 3

	MediaHeaderLen    = 16
	RepairHeaderLen   = 10
	PaddingHeaderLen  = 4
	FeedbackHeaderLen = 4

	RecoveryOffset  = 6
	RepairExpansion = RepairHeaderLen + 2 - RecoveryOffset
	FECReserveGroup = 16
	FECReserve      = RepairExpansion + FECReserveGroup
	MaxRepairSpan   = 255

	MaxDatagramLen = 1500
	MaxFragments   = 255
	MaxLayer       = 7

	SchemeXOR = 0
	SchemeRS  = 1
	MaxFECK   = 48
	MaxFECM   = 16

	FlagKeyframe = 0x80
	FlagFEC      = 0x40
	FlagRecovery = 0x20
	FlagLongTerm = 0x10
	FlagFace     = 0x08
	layerMask    = 0x07

	DepthUnitUs = 10_000
)

var (
	ErrShort    = errors.New("fmm: datagram too short")
	ErrLong     = errors.New("fmm: datagram too long")
	ErrVersion  = errors.New("fmm: unknown version")
	ErrKind     = errors.New("fmm: unknown datagram kind")
	ErrFragment = errors.New("fmm: bad fragment index or count")
	ErrFlags    = errors.New("fmm: bad frame flags")
	ErrRepair   = errors.New("fmm: bad repair header")
	ErrPadding  = errors.New("fmm: bad padding header")
	ErrFeedback = errors.New("fmm: bad feedback datagram")
)

type Media struct {
	Stream       uint8
	TransportSeq uint16
	Seq          uint16
	FrameID      uint16
	FragIndex    uint8
	FragCount    uint8
	Keyframe     bool
	FEC          bool
	Recovery     bool
	LongTerm     bool
	Face         bool
	Layer        uint8
	RefDelta     uint8
	Timestamp    uint32
	Payload      []byte
}

type Repair struct {
	Stream       uint8
	TransportSeq uint16
	BaseSeq      uint16
	Scheme       uint8
	K            uint8
	M            uint8
	Index        uint8
	Offsets      []uint8
	Symbol       []byte
}

func Kind(b []byte) (int, error) {
	if len(b) < PaddingHeaderLen {
		return 0, ErrShort
	}
	if len(b) > MaxDatagramLen {
		return 0, ErrLong
	}
	if b[0]>>4 != Version {
		return 0, ErrVersion
	}
	k := int(b[0] & 0x0F)
	if k > KindFeedback {
		return 0, ErrKind
	}
	return k, nil
}

func (m *Media) Validate() error {
	if m.FragCount == 0 || m.FragIndex >= m.FragCount {
		return ErrFragment
	}
	if m.Layer > MaxLayer {
		return ErrFlags
	}
	if m.Keyframe && (m.Layer != 0 || m.RefDelta != 0) {
		return ErrFlags
	}
	if m.Recovery && (m.Keyframe || m.Layer != 0 || m.RefDelta == 0) {
		return ErrFlags
	}
	if MediaHeaderLen+len(m.Payload) > MaxDatagramLen {
		return ErrLong
	}
	return nil
}

func (m *Media) Append(b []byte) []byte {
	flags := m.Layer & layerMask
	if m.Keyframe {
		flags |= FlagKeyframe
	}
	if m.FEC {
		flags |= FlagFEC
	}
	if m.Recovery {
		flags |= FlagRecovery
	}
	if m.LongTerm {
		flags |= FlagLongTerm
	}
	if m.Face {
		flags |= FlagFace
	}
	b = append(b, Version<<4|KindMedia, m.Stream)
	b = binary.BigEndian.AppendUint16(b, m.TransportSeq)
	b = binary.BigEndian.AppendUint16(b, m.Seq)
	b = binary.BigEndian.AppendUint16(b, m.FrameID)
	b = append(b, m.FragIndex, m.FragCount, flags, m.RefDelta)
	b = binary.BigEndian.AppendUint32(b, m.Timestamp)
	return append(b, m.Payload...)
}

func ParseMedia(b []byte) (Media, error) {
	var m Media
	k, err := Kind(b)
	if err != nil {
		return m, err
	}
	if k != KindMedia {
		return m, ErrKind
	}
	if len(b) < MediaHeaderLen {
		return m, ErrShort
	}
	flags := b[10]
	m = Media{
		Stream:       b[1],
		TransportSeq: binary.BigEndian.Uint16(b[2:]),
		Seq:          binary.BigEndian.Uint16(b[4:]),
		FrameID:      binary.BigEndian.Uint16(b[6:]),
		FragIndex:    b[8],
		FragCount:    b[9],
		Keyframe:     flags&FlagKeyframe != 0,
		FEC:          flags&FlagFEC != 0,
		Recovery:     flags&FlagRecovery != 0,
		LongTerm:     flags&FlagLongTerm != 0,
		Face:         flags&FlagFace != 0,
		Layer:        flags & layerMask,
		RefDelta:     b[11],
		Timestamp:    binary.BigEndian.Uint32(b[12:]),
		Payload:      b[MediaHeaderLen:],
	}
	if err := m.Validate(); err != nil {
		return Media{}, err
	}
	return m, nil
}

func (r *Repair) Validate() error {
	if r.K == 0 || r.K > MaxFECK || r.M == 0 || r.M > MaxFECM || r.Index >= r.M {
		return ErrRepair
	}
	switch r.Scheme {
	case SchemeXOR:
		if r.M != 1 {
			return ErrRepair
		}
	case SchemeRS:
	default:
		return ErrRepair
	}
	if len(r.Offsets) != int(r.K) || r.Offsets[0] != 0 {
		return ErrRepair
	}
	for i := 1; i < len(r.Offsets); i++ {
		if r.Offsets[i] <= r.Offsets[i-1] {
			return ErrRepair
		}
	}
	if len(r.Symbol) < 2+MediaHeaderLen-RecoveryOffset {
		return ErrShort
	}
	if RepairHeaderLen+len(r.Offsets)+len(r.Symbol) > MaxDatagramLen {
		return ErrLong
	}
	return nil
}

func (r *Repair) Members(dst []uint16) []uint16 {
	for _, o := range r.Offsets {
		dst = append(dst, r.BaseSeq+uint16(o))
	}
	return dst
}

func (r *Repair) Append(b []byte) []byte {
	b = append(b, Version<<4|KindRepair, r.Stream)
	b = binary.BigEndian.AppendUint16(b, r.TransportSeq)
	b = binary.BigEndian.AppendUint16(b, r.BaseSeq)
	b = append(b, r.Scheme, r.K, r.M, r.Index)
	b = append(b, r.Offsets...)
	return append(b, r.Symbol...)
}

func ParseRepair(b []byte) (Repair, error) {
	var r Repair
	k, err := Kind(b)
	if err != nil {
		return r, err
	}
	if k != KindRepair {
		return r, ErrKind
	}
	if len(b) < RepairHeaderLen {
		return r, ErrShort
	}
	n := int(b[7])
	if len(b) < RepairHeaderLen+n {
		return r, ErrShort
	}
	r = Repair{
		Stream:       b[1],
		TransportSeq: binary.BigEndian.Uint16(b[2:]),
		BaseSeq:      binary.BigEndian.Uint16(b[4:]),
		Scheme:       b[6],
		K:            b[7],
		M:            b[8],
		Index:        b[9],
		Offsets:      b[RepairHeaderLen : RepairHeaderLen+n],
		Symbol:       b[RepairHeaderLen+n:],
	}
	if err := r.Validate(); err != nil {
		return Repair{}, err
	}
	return r, nil
}

func AppendPadding(b []byte, transportSeq uint16, size int) []byte {
	size = max(size, PaddingHeaderLen)
	b = append(b, Version<<4|KindPadding, 0)
	b = binary.BigEndian.AppendUint16(b, transportSeq)
	for i := PaddingHeaderLen; i < size; i++ {
		b = append(b, 0)
	}
	return b
}

func ParsePadding(b []byte) (uint16, error) {
	k, err := Kind(b)
	if err != nil {
		return 0, err
	}
	if k != KindPadding {
		return 0, ErrKind
	}
	if b[1] != 0 {
		return 0, ErrPadding
	}
	return binary.BigEndian.Uint16(b[2:]), nil
}

func TransportSeqOf(b []byte) uint16 { return binary.BigEndian.Uint16(b[2:]) }

func SetTransportSeq(b []byte, s uint16) { binary.BigEndian.PutUint16(b[2:], s) }

func StreamOf(b []byte) uint8 { return b[1] }

func SetStream(b []byte, id uint8) { b[1] = id }

func SeqOf(b []byte) uint16 { return binary.BigEndian.Uint16(b[4:]) }

func SetSeq(b []byte, s uint16) { binary.BigEndian.PutUint16(b[4:], s) }

func SetFECFlag(b []byte, on bool) {
	if on {
		b[10] |= FlagFEC
	} else {
		b[10] &^= FlagFEC
	}
}

func FlagsOf(b []byte) uint8 { return b[10] }

func FrameIDOf(b []byte) uint16 { return binary.BigEndian.Uint16(b[6:]) }

func SetFrameID(b []byte, id uint16) { binary.BigEndian.PutUint16(b[6:], id) }

func AuxOf(b []byte) uint8 { return b[11] }

func AppendFeedback(b []byte, msgs []byte) []byte {
	b = append(b, Version<<4|KindFeedback, 0, 0, 0)
	return append(b, msgs...)
}

func ParseFeedback(b []byte) ([]Message, error) {
	k, err := Kind(b)
	if err != nil {
		return nil, err
	}
	if k != KindFeedback || b[1] != 0 || b[2] != 0 || b[3] != 0 || len(b) == FeedbackHeaderLen {
		return nil, ErrFeedback
	}
	var out []Message
	rest := b[FeedbackHeaderLen:]
	for len(rest) > 0 {
		m, n, err := Decode(rest)
		if err != nil {
			return nil, ErrFeedback
		}
		if !FeedbackType(m.Type()) {
			return nil, ErrFeedback
		}
		out = append(out, m)
		rest = rest[n:]
	}
	return out, nil
}

func FeedbackType(t uint8) bool {
	return t == MsgReport || t == MsgNack || t == MsgAck || t == MsgRecovery
}
