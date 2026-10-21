package wire

import (
	"encoding/binary"
	"errors"
	"unicode/utf8"
)

const (
	MsgJoin            = 0x01
	MsgJoined          = 0x02
	MsgPublish         = 0x03
	MsgUnpublish       = 0x04
	MsgTrackAdded      = 0x05
	MsgTrackRemoved    = 0x06
	MsgSubscribe       = 0x07
	MsgUnsubscribe     = 0x08
	MsgKeyframeRequest = 0x09
	MsgReport          = 0x0A
	MsgBitrateHint     = 0x0B
	MsgLeave           = 0x0C
	MsgError           = 0x0D
	MsgNack            = 0x0E

	ControlHeaderLen = 3
	MaxControlBody   = 8192
	MaxTicket        = 2048
	MaxName          = 64
	MaxText          = 256
	MaxReportPackets = 2048
	MaxReaderBuffer  = 1 << 20
	MaxNack          = 128

	TrackAudio = 1
	TrackVideo = 2

	AllStreams = 0xFF
)

var (
	ErrIncomplete = errors.New("fmm: control message incomplete")
	ErrType       = errors.New("fmm: unknown control message")
	ErrBody       = errors.New("fmm: malformed control message")
	ErrBound      = errors.New("fmm: control field out of bounds")
)

type Message interface{ Type() uint8 }

type Join struct{ Ticket string }

type Joined struct{ Peer string }

type Publish struct {
	Stream uint8
	Kind   uint8
	Layers uint8
	Name   string
}

type Unpublish struct{ Stream uint8 }

type TrackAdded struct {
	Stream uint8
	Kind   uint8
	Layers uint8
	Peer   string
	Name   string
}

type TrackRemoved struct{ Stream uint8 }

type Subscribe struct {
	Stream   uint8
	MaxLayer uint8
}

type Unsubscribe struct{ Stream uint8 }

type KeyframeRequest struct{ Stream uint8 }

type Report struct {
	Base     uint16
	RefTime  uint32
	Delay    uint16
	Received []bool
	Arrival  []int64
}

type BitrateHint struct {
	Stream  uint8
	Bitrate uint32
}

type Leave struct{ Reason string }

type Error struct {
	Code uint16
	Text string
}

type Nack struct {
	Stream uint8
	Seqs   []uint16
}

func (Join) Type() uint8            { return MsgJoin }
func (Joined) Type() uint8          { return MsgJoined }
func (Publish) Type() uint8         { return MsgPublish }
func (Unpublish) Type() uint8       { return MsgUnpublish }
func (TrackAdded) Type() uint8      { return MsgTrackAdded }
func (TrackRemoved) Type() uint8    { return MsgTrackRemoved }
func (Subscribe) Type() uint8       { return MsgSubscribe }
func (Unsubscribe) Type() uint8     { return MsgUnsubscribe }
func (KeyframeRequest) Type() uint8 { return MsgKeyframeRequest }
func (*Report) Type() uint8         { return MsgReport }
func (BitrateHint) Type() uint8     { return MsgBitrateHint }
func (Leave) Type() uint8           { return MsgLeave }
func (Error) Type() uint8           { return MsgError }
func (Nack) Type() uint8            { return MsgNack }

func validTrack(kind, layers uint8) bool {
	switch kind {
	case TrackAudio:
		return layers == 1
	case TrackVideo:
		return layers >= 1 && layers <= MaxLayer+1
	}
	return false
}

func appendString(b []byte, s string, limit int, allowEmpty bool) ([]byte, error) {
	if len(s) > limit || (!allowEmpty && s == "") || !utf8.ValidString(s) {
		return b, ErrBound
	}
	b = binary.BigEndian.AppendUint16(b, uint16(len(s)))
	return append(b, s...), nil
}

func Append(dst []byte, m Message) ([]byte, error) {
	start := len(dst)
	dst = append(dst, m.Type(), 0, 0)
	var err error
	switch v := m.(type) {
	case Join:
		if v.Ticket == "" || len(v.Ticket) > MaxTicket {
			return dst[:start], ErrBound
		}
		dst = binary.BigEndian.AppendUint16(dst, uint16(len(v.Ticket)))
		dst = append(dst, v.Ticket...)
	case Joined:
		dst, err = appendString(dst, v.Peer, MaxName, false)
	case Publish:
		if !validTrack(v.Kind, v.Layers) {
			return dst[:start], ErrBound
		}
		dst = append(dst, v.Stream, v.Kind, v.Layers)
		dst, err = appendString(dst, v.Name, MaxName, true)
	case Unpublish:
		dst = append(dst, v.Stream)
	case TrackAdded:
		if !validTrack(v.Kind, v.Layers) {
			return dst[:start], ErrBound
		}
		dst = append(dst, v.Stream, v.Kind, v.Layers)
		dst, err = appendString(dst, v.Peer, MaxName, false)
		if err == nil {
			dst, err = appendString(dst, v.Name, MaxName, true)
		}
	case TrackRemoved:
		dst = append(dst, v.Stream)
	case Subscribe:
		if v.MaxLayer > MaxLayer {
			return dst[:start], ErrBound
		}
		dst = append(dst, v.Stream, v.MaxLayer)
	case Unsubscribe:
		dst = append(dst, v.Stream)
	case KeyframeRequest:
		dst = append(dst, v.Stream)
	case *Report:
		dst, err = appendReport(dst, v)
	case BitrateHint:
		dst = append(dst, v.Stream)
		dst = binary.BigEndian.AppendUint32(dst, v.Bitrate)
	case Leave:
		dst, err = appendString(dst, v.Reason, MaxText, true)
	case Error:
		dst = binary.BigEndian.AppendUint16(dst, v.Code)
		dst, err = appendString(dst, v.Text, MaxText, true)
	case Nack:
		if len(v.Seqs) == 0 || len(v.Seqs) > MaxNack {
			return dst[:start], ErrBound
		}
		dst = append(dst, v.Stream, uint8(len(v.Seqs)))
		for _, q := range v.Seqs {
			dst = binary.BigEndian.AppendUint16(dst, q)
		}
	default:
		return dst[:start], ErrType
	}
	if err != nil {
		return dst[:start], err
	}
	body := len(dst) - start - ControlHeaderLen
	if body > MaxControlBody {
		return dst[:start], ErrBound
	}
	binary.BigEndian.PutUint16(dst[start+1:], uint16(body))
	return dst, nil
}

func appendReport(b []byte, r *Report) ([]byte, error) {
	n := len(r.Received)
	if n == 0 || n > MaxReportPackets || len(r.Arrival) != n {
		return b, ErrBound
	}
	b = binary.BigEndian.AppendUint16(b, r.Base)
	b = binary.BigEndian.AppendUint16(b, uint16(n))
	b = binary.BigEndian.AppendUint32(b, r.RefTime)
	b = binary.BigEndian.AppendUint16(b, r.Delay)
	mapStart := len(b)
	for i := 0; i < (n+7)/8; i++ {
		b = append(b, 0)
	}
	prev := int64(0)
	first := true
	for i, ok := range r.Received {
		if !ok {
			if r.Arrival[i] != 0 {
				return b, ErrBound
			}
			continue
		}
		b[mapStart+i/8] |= 0x80 >> (i % 8)
		if first {
			if r.Arrival[i] != 0 {
				return b, ErrBound
			}
			first = false
			continue
		}
		d := r.Arrival[i] - prev
		prev = r.Arrival[i]
		switch {
		case d >= 0 && d <= 254:
			b = append(b, byte(d))
		case d >= -32768 && d <= 32767:
			b = append(b, 0xFF)
			b = binary.BigEndian.AppendUint16(b, uint16(int16(d)))
		default:
			return b, ErrBound
		}
	}
	if first {
		return b, ErrBound
	}
	return b, nil
}

type reader struct {
	b   []byte
	err bool
}

func (r *reader) u8() uint8 {
	if len(r.b) < 1 {
		r.err = true
		return 0
	}
	v := r.b[0]
	r.b = r.b[1:]
	return v
}

func (r *reader) u16() uint16 {
	if len(r.b) < 2 {
		r.err = true
		return 0
	}
	v := binary.BigEndian.Uint16(r.b)
	r.b = r.b[2:]
	return v
}

func (r *reader) u32() uint32 {
	if len(r.b) < 4 {
		r.err = true
		return 0
	}
	v := binary.BigEndian.Uint32(r.b)
	r.b = r.b[4:]
	return v
}

func (r *reader) bytes(n int) []byte {
	if n > len(r.b) {
		r.err = true
		return nil
	}
	v := r.b[:n]
	r.b = r.b[n:]
	return v
}

func (r *reader) str(limit int, allowEmpty bool) string {
	n := int(r.u16())
	if r.err || n > limit || (!allowEmpty && n == 0) {
		r.err = true
		return ""
	}
	s := r.bytes(n)
	if r.err || !utf8.Valid(s) {
		r.err = true
		return ""
	}
	return string(s)
}

func Decode(b []byte) (Message, int, error) {
	if len(b) < ControlHeaderLen {
		return nil, 0, ErrIncomplete
	}
	t := b[0]
	if t < MsgJoin || t > MsgNack {
		return nil, 0, ErrType
	}
	n := int(binary.BigEndian.Uint16(b[1:]))
	if n > MaxControlBody {
		return nil, 0, ErrBound
	}
	if len(b) < ControlHeaderLen+n {
		return nil, 0, ErrIncomplete
	}
	r := &reader{b: b[ControlHeaderLen : ControlHeaderLen+n]}
	var m Message
	switch t {
	case MsgJoin:
		l := int(r.u16())
		if r.err || l == 0 || l > MaxTicket {
			return nil, 0, ErrBody
		}
		m = Join{Ticket: string(r.bytes(l))}
	case MsgJoined:
		m = Joined{Peer: r.str(MaxName, false)}
	case MsgPublish:
		v := Publish{Stream: r.u8(), Kind: r.u8(), Layers: r.u8()}
		v.Name = r.str(MaxName, true)
		if !validTrack(v.Kind, v.Layers) {
			return nil, 0, ErrBody
		}
		m = v
	case MsgUnpublish:
		m = Unpublish{Stream: r.u8()}
	case MsgTrackAdded:
		v := TrackAdded{Stream: r.u8(), Kind: r.u8(), Layers: r.u8()}
		v.Peer = r.str(MaxName, false)
		v.Name = r.str(MaxName, true)
		if !validTrack(v.Kind, v.Layers) {
			return nil, 0, ErrBody
		}
		m = v
	case MsgTrackRemoved:
		m = TrackRemoved{Stream: r.u8()}
	case MsgSubscribe:
		v := Subscribe{Stream: r.u8(), MaxLayer: r.u8()}
		if v.MaxLayer > MaxLayer {
			return nil, 0, ErrBody
		}
		m = v
	case MsgUnsubscribe:
		m = Unsubscribe{Stream: r.u8()}
	case MsgKeyframeRequest:
		m = KeyframeRequest{Stream: r.u8()}
	case MsgReport:
		rep, ok := decodeReport(r)
		if !ok {
			return nil, 0, ErrBody
		}
		m = rep
	case MsgBitrateHint:
		m = BitrateHint{Stream: r.u8(), Bitrate: r.u32()}
	case MsgLeave:
		m = Leave{Reason: r.str(MaxText, true)}
	case MsgError:
		v := Error{Code: r.u16()}
		v.Text = r.str(MaxText, true)
		m = v
	case MsgNack:
		v := Nack{Stream: r.u8()}
		n := int(r.u8())
		if r.err || n == 0 || n > MaxNack {
			return nil, 0, ErrBody
		}
		v.Seqs = make([]uint16, n)
		for i := range v.Seqs {
			v.Seqs[i] = r.u16()
		}
		m = v
	}
	if r.err || len(r.b) != 0 {
		return nil, 0, ErrBody
	}
	return m, ControlHeaderLen + n, nil
}

func decodeReport(r *reader) (*Report, bool) {
	rep := &Report{Base: r.u16()}
	n := int(r.u16())
	rep.RefTime = r.u32()
	rep.Delay = r.u16()
	if r.err || n == 0 || n > MaxReportPackets {
		return nil, false
	}
	bitmap := r.bytes((n + 7) / 8)
	if r.err {
		return nil, false
	}
	if n%8 != 0 && bitmap[len(bitmap)-1]&(0xFF>>(n%8)) != 0 {
		return nil, false
	}
	rep.Received = make([]bool, n)
	rep.Arrival = make([]int64, n)
	first := true
	prev := int64(0)
	for i := range n {
		if bitmap[i/8]&(0x80>>(i%8)) == 0 {
			continue
		}
		rep.Received[i] = true
		if first {
			first = false
			continue
		}
		c := r.u8()
		if r.err {
			return nil, false
		}
		d := int64(c)
		if c == 0xFF {
			d = int64(int16(r.u16()))
			if r.err || (d >= 0 && d <= 254) {
				return nil, false
			}
		}
		prev += d
		rep.Arrival[i] = prev
	}
	if first {
		return nil, false
	}
	return rep, true
}

type Reader struct {
	buf    []byte
	broken error
}

func (r *Reader) Write(p []byte) error {
	if r.broken != nil {
		return r.broken
	}
	if len(r.buf)+len(p) > MaxReaderBuffer {
		r.broken = ErrBound
		return r.broken
	}
	r.buf = append(r.buf, p...)
	return nil
}

func (r *Reader) Next() (Message, error) {
	if r.broken != nil {
		return nil, r.broken
	}
	m, n, err := Decode(r.buf)
	if err == ErrIncomplete {
		return nil, nil
	}
	if err != nil {
		r.broken = err
		return nil, err
	}
	rest := copy(r.buf, r.buf[n:])
	r.buf = r.buf[:rest]
	return m, nil
}
