package wire

import (
	"bytes"
	"encoding/hex"
	"encoding/json"
	"math/rand/v2"
	"os"
	"path/filepath"
	"testing"
	"unicode/utf8"

	"forkmesh.com/mirror-node/rtc/fmm/internal/seqnum"
)

const jsParityFile = "testdata/js-parity.json"

type jpObj = map[string]any

func jpHex(b []byte) string { return hex.EncodeToString(b) }

func jpErr(err error) string {
	if err == nil {
		return ""
	}
	return err.Error()
}

func jpMsg(m Message) jpObj {
	o := jpObj{"type": m.Type()}
	str := func(k, s string) {
		o[k] = jpHex([]byte(s))
		if !utf8.ValidString(s) {
			o["lossy"] = true
		}
	}
	switch v := m.(type) {
	case Join:
		str("ticket", v.Ticket)
	case Joined:
		str("peer", v.Peer)
	case Publish:
		o["stream"], o["kind"], o["layers"], o["track"], o["rank"] = v.Stream, v.Kind, v.Layers, v.Track, v.Rank
		str("name", v.Name)
	case Unpublish:
		o["stream"] = v.Stream
	case TrackAdded:
		o["stream"], o["kind"], o["layers"] = v.Stream, v.Kind, v.Layers
		str("peer", v.Peer)
		str("name", v.Name)
	case TrackRemoved:
		o["stream"] = v.Stream
	case Subscribe:
		o["stream"], o["maxLayer"] = v.Stream, v.MaxLayer
	case Unsubscribe:
		o["stream"] = v.Stream
	case KeyframeRequest:
		o["stream"] = v.Stream
	case *Report:
		o["base"], o["refTime"], o["delay"] = v.Base, v.RefTime, v.Delay
		o["received"], o["arrival"] = v.Received, v.Arrival
	case BitrateHint:
		o["stream"], o["bitrate"] = v.Stream, v.Bitrate
	case Leave:
		str("reason", v.Reason)
	case Error:
		o["code"] = v.Code
		str("text", v.Text)
	case Nack:
		o["stream"], o["seqs"] = v.Stream, v.Seqs
	case Ack:
		o["stream"], o["frames"] = v.Stream, v.Frames
	case Recovery:
		o["stream"], o["frame"] = v.Stream, v.Frame
	}
	return o
}

func jpMedia(m Media) jpObj {
	return jpObj{
		"stream": m.Stream, "transportSeq": m.TransportSeq, "seq": m.Seq, "frameID": m.FrameID,
		"fragIndex": m.FragIndex, "fragCount": m.FragCount, "keyframe": m.Keyframe, "fec": m.FEC,
		"recovery": m.Recovery, "longTerm": m.LongTerm, "face": m.Face, "layer": m.Layer,
		"refDelta": m.RefDelta, "timestamp": m.Timestamp, "payload": jpHex(m.Payload),
	}
}

func jpRepair(r Repair) jpObj {
	return jpObj{
		"stream": r.Stream, "transportSeq": r.TransportSeq, "baseSeq": r.BaseSeq, "scheme": r.Scheme,
		"k": r.K, "m": r.M, "index": r.Index, "offsets": jpHex(r.Offsets), "symbol": jpHex(r.Symbol),
	}
}

func jpBytes(rng *rand.Rand, n int) []byte {
	b := make([]byte, n)
	for i := range b {
		b[i] = byte(rng.Uint32())
	}
	return b
}

func jpString(rng *rand.Rand, max int) string {
	const alphabet = "abcdefghijklmnopqrstuvwxyz0123456789-_"
	n := rng.IntN(max + 1)
	b := make([]byte, n)
	for i := range b {
		b[i] = alphabet[rng.IntN(len(alphabet))]
	}
	s := string(b)
	if n > 2 && rng.IntN(5) == 0 {
		s = s[:n-2] + "é"
	}
	return s
}

func jpRandomMedia(rng *rand.Rand) Media {
	m := Media{
		Stream: uint8(rng.Uint32()), TransportSeq: uint16(rng.Uint32()), Seq: uint16(rng.Uint32()),
		FrameID: uint16(rng.Uint32()), Timestamp: rng.Uint32(), FEC: rng.IntN(2) == 0,
		LongTerm: rng.IntN(3) == 0, Face: rng.IntN(4) == 0,
		Payload: jpBytes(rng, rng.IntN(40)),
	}
	m.FragCount = uint8(1 + rng.IntN(255))
	m.FragIndex = uint8(rng.IntN(int(m.FragCount)))
	switch rng.IntN(4) {
	case 0:
		m.Keyframe = true
	case 1:
		m.Recovery = true
		m.RefDelta = uint8(1 + rng.IntN(255))
	default:
		m.Layer = uint8(rng.IntN(MaxLayer + 1))
		m.RefDelta = uint8(rng.Uint32())
	}
	return m
}

func jpRandomRepair(rng *rand.Rand) Repair {
	r := Repair{Stream: uint8(rng.Uint32()), TransportSeq: uint16(rng.Uint32()), BaseSeq: uint16(rng.Uint32())}
	r.K = uint8(1 + rng.IntN(MaxFECK))
	if rng.IntN(3) == 0 {
		r.Scheme, r.M = SchemeXOR, 1
	} else {
		r.Scheme, r.M = SchemeRS, uint8(1+rng.IntN(MaxFECM))
	}
	r.Index = uint8(rng.IntN(int(r.M)))
	off := 0
	for i := 0; i < int(r.K); i++ {
		if i > 0 {
			off += 1 + rng.IntN(4)
		}
		if off > 255 {
			r.K = uint8(i)
			break
		}
		r.Offsets = append(r.Offsets, uint8(off))
	}
	r.Symbol = jpBytes(rng, 12+rng.IntN(40))
	return r
}

func jpRandomReport(rng *rand.Rand) *Report {
	n := 1 + rng.IntN(70)
	if rng.IntN(20) == 0 {
		n = 1 + rng.IntN(MaxReportPackets)
	}
	r := &Report{Base: uint16(rng.Uint32()), RefTime: rng.Uint32(), Delay: uint16(rng.Uint32()), Received: make([]bool, n), Arrival: make([]int64, n)}
	first := true
	at := int64(0)
	for i := range n {
		if rng.IntN(5) == 0 && !(i == n-1 && first) {
			continue
		}
		r.Received[i] = true
		if first {
			first = false
			continue
		}
		switch rng.IntN(6) {
		case 0:
			at -= int64(1 + rng.IntN(32768))
		case 1:
			at += int64(255 + rng.IntN(32000))
		default:
			at += int64(rng.IntN(255))
		}
		r.Arrival[i] = at
	}
	return r
}

func jpRandomMessage(rng *rand.Rand) Message {
	switch rng.IntN(16) {
	case 0:
		return Join{Ticket: jpString(rng, 80) + "t"}
	case 1:
		return Joined{Peer: jpString(rng, 20) + "p"}
	case 2:
		v := Publish{Stream: uint8(rng.Uint32()), Name: jpString(rng, 30)}
		if rng.IntN(2) == 0 {
			v.Kind, v.Layers = uint8(TrackAudio+2*rng.IntN(2)), 1
		} else {
			v.Kind, v.Layers = TrackVideo, uint8(1+rng.IntN(8))
			if rng.IntN(2) == 0 {
				v.Track = uint8(1 + rng.IntN(255))
				v.Rank = uint8(rng.IntN(MaxEncodings))
				if v.Track == v.Stream && v.Rank == 0 {
					v.Rank = 1
				}
			}
		}
		return v
	case 3:
		return Unpublish{Stream: uint8(rng.Uint32())}
	case 4:
		v := TrackAdded{Stream: uint8(rng.Uint32()), Peer: jpString(rng, 30) + "x", Name: jpString(rng, 20)}
		v.Kind = uint8(1 + rng.IntN(3))
		v.Layers = 1
		if v.Kind == TrackVideo {
			v.Layers = uint8(1 + rng.IntN(8))
		}
		return v
	case 5:
		return TrackRemoved{Stream: uint8(rng.Uint32())}
	case 6:
		return Subscribe{Stream: uint8(rng.Uint32()), MaxLayer: uint8(rng.IntN(8))}
	case 7:
		return Unsubscribe{Stream: uint8(rng.Uint32())}
	case 8:
		return KeyframeRequest{Stream: uint8(rng.Uint32())}
	case 9:
		return jpRandomReport(rng)
	case 10:
		return BitrateHint{Stream: uint8(rng.Uint32()), Bitrate: rng.Uint32()}
	case 11:
		return Leave{Reason: jpString(rng, 40)}
	case 12:
		return Error{Code: uint16(rng.Uint32()), Text: jpString(rng, 40)}
	case 13:
		v := Nack{Stream: uint8(rng.Uint32())}
		for range 1 + rng.IntN(MaxNack) {
			v.Seqs = append(v.Seqs, uint16(rng.Uint32()))
		}
		return v
	case 14:
		v := Ack{Stream: uint8(rng.Uint32())}
		for range 1 + rng.IntN(MaxAck) {
			v.Frames = append(v.Frames, uint16(rng.Uint32()))
		}
		return v
	default:
		return Recovery{Stream: uint8(rng.Uint32()), Frame: uint16(rng.Uint32())}
	}
}

func jpMutate(rng *rand.Rand, b []byte) []byte {
	out := append([]byte(nil), b...)
	switch rng.IntN(5) {
	case 0:
		if len(out) > 0 {
			out = out[:rng.IntN(len(out))]
		}
	case 1:
		out = append(out, jpBytes(rng, 1+rng.IntN(4))...)
	default:
		for range 1 + rng.IntN(3) {
			if len(out) > 0 {
				i := rng.IntN(min(len(out), 24))
				if rng.IntN(2) == 0 {
					out[i] = byte(rng.Uint32())
				} else {
					out[i] ^= 1 << rng.IntN(8)
				}
			}
		}
	}
	return out
}

func jpParse(b []byte) jpObj {
	o := jpObj{"bytes": jpHex(b)}
	k, err := Kind(b)
	o["kind"], o["kindErr"] = k, jpErr(err)
	if m, err := ParseMedia(b); err != nil {
		o["mediaErr"] = jpErr(err)
	} else {
		o["media"] = jpMedia(m)
	}
	if r, err := ParseRepair(b); err != nil {
		o["repairErr"] = jpErr(err)
	} else {
		o["repair"] = jpRepair(r)
	}
	if len(b) >= 4 {
		if s, err := ParsePadding(b); err != nil {
			o["paddingErr"] = jpErr(err)
		} else {
			o["padding"] = s
		}
	}
	if ms, err := ParseFeedback(b); err != nil {
		o["feedbackErr"] = jpErr(err)
	} else {
		list := []jpObj{}
		for _, m := range ms {
			list = append(list, jpMsg(m))
		}
		o["feedback"] = list
	}
	if key, ok := DedupKey(b); ok {
		o["dedup"] = key
	} else {
		o["dedup"] = nil
	}
	return o
}

func jpDecode(b []byte) jpObj {
	o := jpObj{"bytes": jpHex(b)}
	m, n, err := Decode(b)
	if err != nil {
		o["error"] = err.Error()
		return o
	}
	o["msg"], o["n"] = jpMsg(m), n
	return o
}

func buildJSParity() ([]byte, error) {
	rng := rand.New(rand.NewPCG(20261021, 6))
	fx := jpObj{}

	type seqCase struct {
		Inputs  []uint32 `json:"inputs"`
		Outputs []int64  `json:"outputs"`
		Last    *int64   `json:"last,omitempty"`
	}
	var u16, u32 []seqCase
	for c := range 5 {
		var u seqnum.Unwrapper16
		var w seqnum.Unwrapper32
		var a, b seqCase
		v16 := uint16(rng.Uint32())
		v32 := rng.Uint32()
		if c == 0 {
			v16, v32 = 65530, 0xFFFFFFF0
		}
		for range 200 {
			a.Inputs = append(a.Inputs, uint32(v16))
			a.Outputs = append(a.Outputs, u.Unwrap(v16))
			b.Inputs = append(b.Inputs, v32)
			b.Outputs = append(b.Outputs, w.Unwrap(v32))
			switch rng.IntN(6) {
			case 0:
				v16 -= uint16(rng.IntN(200))
				v32 -= uint32(rng.IntN(1 << 20))
			case 1:
				v16 += uint16(rng.IntN(40000))
				v32 += uint32(rng.Uint32())
			default:
				v16 += uint16(rng.IntN(20))
				v32 += uint32(rng.IntN(1 << 22))
			}
		}
		last, _ := u.Last()
		a.Last = &last
		u16, u32 = append(u16, a), append(u32, b)
	}
	var near []jpObj
	for range 100 {
		ref := int64(rng.Uint64()>>12) - int64(1<<50)
		if rng.IntN(3) == 0 {
			ref = int64(rng.IntN(200000)) - 100000
		}
		v16, v32 := uint16(rng.Uint32()), rng.Uint32()
		near = append(near, jpObj{"ref": ref, "v16": v16, "near16": seqnum.Near16(v16, ref), "v32": v32, "near32": seqnum.Near32(v32, ref)})
	}
	fx["seqnum"] = jpObj{"unwrap16": u16, "unwrap32": u32, "near": near}

	var datagrams [][]byte
	var media []jpObj
	fixed := []Media{
		{Stream: 3, TransportSeq: 0x0102, Seq: 0x0304, FrameID: 0x0506, FragIndex: 1, FragCount: 4, FEC: true, Layer: 2, RefDelta: 1, Timestamp: 0x0A0B0C0D, Payload: []byte{0xEE}},
		{FragCount: 1},
		{Stream: 255, FragCount: 255, FragIndex: 254, Keyframe: true, LongTerm: true, Face: true, Timestamp: 0xFFFFFFFF, Payload: make([]byte, MaxDatagramLen-MediaHeaderLen)},
		{FragCount: 1, Recovery: true, RefDelta: 9, LongTerm: true},
	}
	for i := range 160 {
		var m Media
		if i < len(fixed) {
			m = fixed[i]
		} else {
			m = jpRandomMedia(rng)
		}
		b := m.Append(nil)
		datagrams = append(datagrams, b)
		media = append(media, jpObj{"in": jpMedia(m), "bytes": jpHex(b), "validate": jpErr(m.Validate())})
	}
	invalidMedia := []Media{
		{FragCount: 0}, {FragCount: 2, FragIndex: 2}, {FragCount: 1, Layer: 8}, {FragCount: 1, Keyframe: true, Layer: 1},
		{FragCount: 1, Keyframe: true, RefDelta: 1}, {FragCount: 1, Recovery: true}, {FragCount: 1, Recovery: true, Keyframe: true, RefDelta: 1},
		{FragCount: 1, Recovery: true, Layer: 1, RefDelta: 2}, {FragCount: 1, Payload: make([]byte, MaxDatagramLen-MediaHeaderLen+1)},
	}
	var mediaInvalid []jpObj
	for _, m := range invalidMedia {
		mediaInvalid = append(mediaInvalid, jpObj{"in": jpMedia(m), "validate": jpErr(m.Validate())})
	}
	fx["media"], fx["mediaInvalid"] = media, mediaInvalid

	var repairs []jpObj
	fixedRepairs := []Repair{
		{Stream: 9, TransportSeq: 7, BaseSeq: 0xFFFE, Scheme: SchemeRS, K: 3, M: 2, Index: 1, Offsets: []uint8{0, 2, 9}, Symbol: make([]byte, 12)},
		{K: 1, M: 1, Offsets: []uint8{0}, Symbol: make([]byte, 12)},
	}
	for i := range 80 {
		var r Repair
		if i < len(fixedRepairs) {
			r = fixedRepairs[i]
		} else {
			r = jpRandomRepair(rng)
		}
		b := r.Append(nil)
		datagrams = append(datagrams, b)
		repairs = append(repairs, jpObj{"in": jpRepair(r), "bytes": jpHex(b), "validate": jpErr(r.Validate()), "members": r.Members(nil)})
	}
	four := []uint8{0, 1, 2, 3}
	badRepairs := []Repair{
		{K: 0, M: 1, Symbol: make([]byte, 12)},
		{K: 49, M: 1, Offsets: make([]uint8, 49), Symbol: make([]byte, 12)},
		{K: 4, M: 2, Scheme: SchemeXOR, Offsets: four, Symbol: make([]byte, 12)},
		{K: 4, M: 2, Index: 2, Scheme: SchemeRS, Offsets: four, Symbol: make([]byte, 12)},
		{K: 4, M: 1, Scheme: 2, Offsets: four, Symbol: make([]byte, 12)},
		{K: 4, M: 17, Scheme: SchemeRS, Offsets: four, Symbol: make([]byte, 12)},
		{K: 4, M: 1, Offsets: []uint8{0, 1, 1, 3}, Symbol: make([]byte, 12)},
		{K: 4, M: 1, Offsets: []uint8{1, 2, 3, 4}, Symbol: make([]byte, 12)},
		{K: 3, M: 1, Offsets: four, Symbol: make([]byte, 12)},
		{K: 4, M: 1, Offsets: four, Symbol: make([]byte, 11)},
		{K: 4, M: 1, Offsets: four, Symbol: make([]byte, MaxDatagramLen)},
	}
	var repairInvalid []jpObj
	for _, r := range badRepairs {
		repairInvalid = append(repairInvalid, jpObj{"in": jpRepair(r), "validate": jpErr(r.Validate()), "bytes": jpHex(r.Append(nil))})
	}
	fx["repair"], fx["repairInvalid"] = repairs, repairInvalid

	var padding []jpObj
	for _, size := range []int{0, 3, 4, 5, 20, 1200, 1500} {
		seq := uint16(rng.Uint32())
		b := AppendPadding([]byte{0xAA, 0xBB}, seq, size)
		datagrams = append(datagrams, b[2:])
		padding = append(padding, jpObj{"seq": seq, "size": size, "prefix": "aabb", "bytes": jpHex(b)})
	}
	fx["padding"] = padding

	var controls []jpObj
	var encoded [][]byte
	fixedMsgs := []Message{
		Join{Ticket: "ticket"}, Joined{Peer: "p1"},
		Publish{Stream: 1, Kind: TrackAudio, Layers: 1, Name: "mic"},
		Publish{Stream: 2, Kind: TrackVideo, Layers: 3, Name: ""},
		Publish{Stream: 3, Kind: TrackVideo, Layers: 1, Name: "cam", Track: 2, Rank: 0},
		Publish{Stream: 4, Kind: TrackVideo, Layers: 3, Name: "cam", Track: 4, Rank: 3},
		Publish{Stream: 5, Kind: TrackAuth, Layers: 1, Name: "auth"},
		Unpublish{Stream: 9},
		TrackAdded{Stream: 7, Kind: TrackVideo, Layers: 3, Peer: "p2", Name: "cam"},
		TrackAdded{Stream: 8, Kind: TrackAuth, Layers: 1, Peer: "p2"},
		TrackRemoved{Stream: 7}, Subscribe{Stream: 7, MaxLayer: 7}, Unsubscribe{Stream: 7}, KeyframeRequest{Stream: 0xFF},
		&Report{Base: 65530, RefTime: 0xFFFFFFFF, Delay: 3, Received: []bool{true, false, true, true, true, false, false, true, true}, Arrival: []int64{0, 0, 5, -300, 2000, 0, 0, 2254, 2000}},
		&Report{Base: 1, Received: []bool{true}, Arrival: []int64{0}},
		BitrateHint{Stream: AllStreams, Bitrate: 0xFFFFFFFF}, Leave{Reason: ""}, Leave{Reason: "bye é"},
		Error{Code: 409, Text: "engine webrtc"}, Nack{Stream: 2, Seqs: []uint16{1, 65535}}, Ack{Stream: 3, Frames: []uint16{7}},
		Recovery{Stream: 4, Frame: 0xBEEF},
	}
	for i := range 260 {
		var m Message
		if i < len(fixedMsgs) {
			m = fixedMsgs[i]
		} else {
			m = jpRandomMessage(rng)
		}
		b, err := Append([]byte{0x55}, m)
		if err != nil {
			return nil, err
		}
		encoded = append(encoded, b[1:])
		controls = append(controls, jpObj{"msg": jpMsg(m), "prefix": "55", "bytes": jpHex(b), "decoded": jpDecode(b[1:])})
	}
	fx["control"] = controls

	long := string(bytes.Repeat([]byte("x"), MaxTicket+1))
	badMsgs := []Message{
		Join{}, Join{Ticket: long}, Joined{}, Joined{Peer: string(bytes.Repeat([]byte("p"), MaxName+1))}, Joined{Peer: "\xff"},
		Publish{Kind: TrackAudio, Layers: 2}, Publish{Kind: TrackVideo, Layers: 9}, Publish{Kind: TrackVideo, Layers: 0}, Publish{Kind: 4, Layers: 1},
		Publish{Kind: TrackAuth, Layers: 2}, Publish{Stream: 3, Kind: TrackAudio, Layers: 1, Track: 3, Rank: 1},
		Publish{Stream: 3, Kind: TrackVideo, Layers: 1, Track: 2, Rank: MaxEncodings}, Publish{Stream: 3, Kind: TrackVideo, Layers: 1, Rank: 1},
		Publish{Stream: 3, Kind: TrackVideo, Layers: 1, Track: 3, Rank: 0},
		TrackAdded{Kind: TrackVideo, Layers: 1}, TrackAdded{Kind: 0, Layers: 1, Peer: "p"},
		Subscribe{MaxLayer: 8}, &Report{}, &Report{Received: []bool{false}, Arrival: []int64{0}},
		&Report{Received: []bool{true}, Arrival: []int64{1}}, &Report{Received: []bool{true, false}, Arrival: []int64{0, 3}},
		&Report{Received: []bool{true, true}, Arrival: []int64{0, 40000}}, &Report{Received: []bool{true}, Arrival: []int64{0, 0}},
		&Report{Received: make([]bool, MaxReportPackets+1), Arrival: make([]int64, MaxReportPackets+1)},
		Leave{Reason: string(bytes.Repeat([]byte("r"), MaxText+1))}, Error{Text: string(bytes.Repeat([]byte("t"), MaxText+1))},
		Nack{}, Nack{Seqs: make([]uint16, MaxNack+1)}, Ack{}, Ack{Frames: make([]uint16, MaxAck+1)},
	}
	var appendErrors []jpObj
	for _, m := range badMsgs {
		_, err := Append(nil, m)
		if err == nil {
			return nil, ErrType
		}
		o := jpObj{"msg": jpMsg(m), "error": err.Error()}
		if r, ok := m.(*Report); ok {
			o["msg"].(jpObj)["received"], o["msg"].(jpObj)["arrival"] = r.Received, r.Arrival
		}
		appendErrors = append(appendErrors, o)
	}
	fx["appendErrors"] = appendErrors

	var decodes []jpObj
	rawDecodes := [][]byte{
		{}, {1}, {1, 0}, {0, 0, 0}, {0x11, 0, 0}, {1, 0x20, 1}, {2, 0, 2, 0, 0},
		{3, 0, 7, 1, 1, 1, 0, 0, 0, 1}, {3, 0, 7, 1, 2, 1, 0, 0, 1, 0},
		{0x0A, 0, 11, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0x80},
		{0x0A, 0, 13, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0xC0, 0xFF, 0},
		{0x0A, 0, 14, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0xC0, 0xFF, 0, 5},
		{0x0A, 0, 11, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0x81},
	}
	for _, b := range rawDecodes {
		decodes = append(decodes, jpDecode(b))
	}
	for range 420 {
		decodes = append(decodes, jpDecode(jpMutate(rng, encoded[rng.IntN(len(encoded))])))
	}
	for range 100 {
		b := jpBytes(rng, rng.IntN(20))
		if len(b) > 0 {
			b[0] = byte(1 + rng.IntN(17))
		}
		if len(b) > 2 {
			b[1], b[2] = 0, byte(len(b)-3)
		}
		decodes = append(decodes, jpDecode(b))
	}
	fx["decode"] = decodes

	var feedback []jpObj
	for range 60 {
		var body []byte
		for range 1 + rng.IntN(4) {
			var m Message
			switch rng.IntN(4) {
			case 0:
				m = jpRandomReport(rng)
			case 1:
				m = Nack{Stream: 1, Seqs: []uint16{uint16(rng.Uint32()), uint16(rng.Uint32())}}
			case 2:
				m = Ack{Stream: 2, Frames: []uint16{uint16(rng.Uint32())}}
			default:
				m = Recovery{Stream: 3, Frame: uint16(rng.Uint32())}
			}
			body, _ = Append(body, m)
		}
		if len(body) > MaxDatagramLen-FeedbackHeaderLen {
			continue
		}
		b := AppendFeedback(nil, body)
		datagrams = append(datagrams, b)
		feedback = append(feedback, jpObj{"body": jpHex(body), "bytes": jpHex(b)})
	}
	join, _ := Append(nil, Join{Ticket: "t"})
	datagrams = append(datagrams, AppendFeedback(nil, nil), AppendFeedback(nil, join), []byte{0x13, 0, 1, 0, 0x10, 0, 3, 1, 0, 1}, AppendFeedback(nil, []byte{0x10, 0, 3, 1}))
	fx["feedback"] = feedback

	var parses []jpObj
	for _, b := range datagrams {
		parses = append(parses, jpParse(b))
	}
	for range 300 {
		b := datagrams[rng.IntN(len(datagrams))]
		if len(b) > 200 {
			b = b[:200]
		}
		parses = append(parses, jpParse(jpMutate(rng, b)))
	}
	for _, b := range [][]byte{{}, {0x10}, {0x10, 0, 0}, {0x20, 0, 0, 0}, {0x14, 0, 0, 0}, {0x12, 1, 0, 0}, make([]byte, MaxDatagramLen+1)} {
		parses = append(parses, jpParse(b))
	}
	fx["parse"] = parses

	var access []jpObj
	for i := range 20 {
		b := append([]byte(nil), datagrams[i]...)
		o := jpObj{"bytes": jpHex(b), "transportSeq": TransportSeqOf(b), "stream": StreamOf(b), "seq": SeqOf(b), "flags": FlagsOf(b), "frameID": FrameIDOf(b), "aux": AuxOf(b)}
		ts, st, sq, fid, fec := uint16(rng.Uint32()), uint8(rng.Uint32()), uint16(rng.Uint32()), uint16(rng.Uint32()), rng.IntN(2) == 0
		SetTransportSeq(b, ts)
		SetStream(b, st)
		SetSeq(b, sq)
		SetFrameID(b, fid)
		SetFECFlag(b, fec)
		o["set"] = jpObj{"transportSeq": ts, "stream": st, "seq": sq, "frameID": fid, "fec": fec}
		o["after"] = jpHex(b)
		access = append(access, o)
	}
	fx["access"] = access

	var readers []jpObj
	stream := []byte{}
	for i := range 12 {
		stream = append(stream, encoded[i*7%len(encoded)]...)
	}
	addReader := func(chunks [][]byte) {
		var r Reader
		var out []jpObj
		errText := ""
		for _, c := range chunks {
			if err := r.Write(c); err != nil {
				errText = err.Error()
				break
			}
			for {
				m, err := r.Next()
				if err != nil {
					errText = err.Error()
					break
				}
				if m == nil {
					break
				}
				out = append(out, jpMsg(m))
			}
			if errText != "" {
				break
			}
		}
		hx := make([]any, len(chunks))
		for i, c := range chunks {
			if len(c) > 4096 && len(bytes.Trim(c, "\x00")) == 0 {
				hx[i] = jpObj{"zeros": len(c)}
			} else {
				hx[i] = jpHex(c)
			}
		}
		if out == nil {
			out = []jpObj{}
		}
		readers = append(readers, jpObj{"chunks": hx, "messages": out, "error": errText})
	}
	addReader([][]byte{stream})
	var bytewise [][]byte
	for _, c := range stream {
		bytewise = append(bytewise, []byte{c})
	}
	addReader(bytewise)
	for range 10 {
		var chunks [][]byte
		for at := 0; at < len(stream); {
			n := min(1+rng.IntN(40), len(stream)-at)
			chunks = append(chunks, stream[at:at+n])
			at += n
		}
		addReader(chunks)
	}
	addReader([][]byte{stream[:5], {0x7F, 0, 0}, stream})
	addReader([][]byte{{0x01, 0x20, 0x01}})
	addReader([][]byte{make([]byte, MaxReaderBuffer+1)})
	addReader([][]byte{{0x01, 0, 4, 0, 0}})
	fx["reader"] = readers

	return json.Marshal(fx)
}

func TestJSParity(t *testing.T) {
	got, err := buildJSParity()
	if err != nil {
		t.Fatal(err)
	}
	again, err := buildJSParity()
	if err != nil || !bytes.Equal(got, again) {
		t.Fatal("the JS parity fixture is not deterministic")
	}
	got = append(got, '\n')
	if os.Getenv("FMM_JS_PARITY_WRITE") == "1" {
		if err := os.MkdirAll(filepath.Dir(jsParityFile), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(jsParityFile, got, 0o644); err != nil {
			t.Fatal(err)
		}
		return
	}
	stored, err := os.ReadFile(jsParityFile)
	if err != nil {
		t.Fatalf("%v (write it with FMM_JS_PARITY_WRITE=1)", err)
	}
	if !bytes.Equal(stored, got) {
		t.Fatal("testdata/js-parity.json is stale; regenerate with FMM_JS_PARITY_WRITE=1")
	}
}
