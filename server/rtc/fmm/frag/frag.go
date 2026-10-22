package frag

import (
	"errors"

	"forkmesh.com/mirror-node/rtc/fmm/internal/seqnum"
	"forkmesh.com/mirror-node/rtc/fmm/wire"
)

var (
	ErrTooLarge = errors.New("frag: frame needs more than 255 fragments")
	ErrRef      = errors.New("frag: reference frame too far back, send a keyframe")
	ErrLayer    = errors.New("frag: bad layer")
)

type Packetizer struct {
	stream        uint8
	maxPayload    int
	seq           uint16
	frameID       int64
	lastAtOrBelow [wire.MaxLayer + 1]int64
	haveLayer     [wire.MaxLayer + 1]bool
}

type Opts struct {
	Keyframe    bool
	Layer       int
	Independent bool
	Ref         int64
	LongTerm    bool
	Face        bool
	Aux         uint8
}

func NewPacketizer(stream uint8, maxPayload int) *Packetizer {
	return &Packetizer{stream: stream, maxPayload: max(maxPayload, 1), frameID: -1}
}

func (p *Packetizer) MaxFrame() int { return p.maxPayload * wire.MaxFragments }

func (p *Packetizer) NextID() int64 { return p.frameID + 1 }

func (p *Packetizer) LastID() int64 { return p.frameID }

func (p *Packetizer) Packetize(data []byte, keyframe bool, layer int, independent bool, ts uint32) ([][]byte, error) {
	return p.PacketizeFrame(data, Opts{Keyframe: keyframe, Layer: layer, Independent: independent, Ref: -1}, ts)
}

func (p *Packetizer) PacketizeFrame(data []byte, o Opts, ts uint32) ([][]byte, error) {
	keyframe, layer := o.Keyframe, o.Layer
	if layer < 0 || layer > wire.MaxLayer || (keyframe && layer != 0) {
		return nil, ErrLayer
	}
	if o.Ref >= 0 && (keyframe || o.Independent || layer != 0) {
		return nil, ErrLayer
	}
	n := max(1, (len(data)+p.maxPayload-1)/p.maxPayload)
	if n > wire.MaxFragments {
		return nil, ErrTooLarge
	}
	id := p.frameID + 1
	ref := o.Aux
	recovery := false
	switch {
	case keyframe:
		ref = 0
	case o.Ref >= 0:
		if !p.haveLayer[0] {
			return nil, ErrRef
		}
		d := id - o.Ref
		if d <= 0 || d > 255 {
			return nil, ErrRef
		}
		ref = uint8(d)
		recovery = true
	case !o.Independent:
		lower := 0
		if layer > 0 {
			lower = layer - 1
		}
		if !p.haveLayer[lower] {
			return nil, ErrRef
		}
		d := id - p.lastAtOrBelow[lower]
		if d <= 0 || d > 255 {
			return nil, ErrRef
		}
		ref = uint8(d)
	}
	p.frameID = id
	if keyframe {
		for l := range p.haveLayer {
			p.haveLayer[l] = true
			p.lastAtOrBelow[l] = id
		}
	} else if !o.Independent {
		for l := layer; l <= wire.MaxLayer; l++ {
			p.haveLayer[l] = true
			p.lastAtOrBelow[l] = id
		}
	}
	out := make([][]byte, n)
	off := 0
	for i := range n {
		size := (len(data) - off) / (n - i)
		m := wire.Media{
			Stream: p.stream, Seq: p.seq, FrameID: uint16(id), FragIndex: uint8(i), FragCount: uint8(n),
			Keyframe: keyframe, Recovery: recovery, LongTerm: o.LongTerm, Face: o.Face,
			Layer: uint8(layer), RefDelta: ref, Timestamp: ts, Payload: data[off : off+size],
		}
		off += size
		p.seq++
		out[i] = m.Append(make([]byte, 0, wire.MediaHeaderLen+size+wire.FECReserve))
	}
	return out, nil
}

type Frame struct {
	ID           int64
	Keyframe     bool
	Recovery     bool
	LongTerm     bool
	Face         bool
	Layer        int
	Ref          int64
	Aux          int
	Timestamp    int64
	FirstSeq     int64
	LastSeq      int64
	Data         []byte
	FirstArrival int64
	Complete     int64
	Recovered    int
}

type Lost struct {
	ID       int64
	Keyframe bool
	Layer    int
}

type partial struct {
	id        int64
	count     int
	have      int
	frags     [][]byte
	keyframe  bool
	flags     uint8
	layer     int
	ref       int64
	ts        int64
	firstSeq  int64
	first     int64
	recovered int
	bytes     int
}

const (
	maxPartials  = 64
	maxBytes     = 4 << 20
	doneRing     = 512
	DefaultLimit = 1_000_000
)

type Reassembler struct {
	ids, seqs, ts seqnum.Unwrapper16
	tsw           seqnum.Unwrapper32
	partial       map[int64]*partial
	order         []int64
	bytes         int
	done          [doneRing]int64
	doneOK        [doneRing]bool
	Timeout       int64
	Completed     int
	Duplicates    int
	Corrupt       int
	Expired       int
}

func NewReassembler() *Reassembler {
	return &Reassembler{partial: map[int64]*partial{}, Timeout: DefaultLimit}
}

func (r *Reassembler) isDone(id int64) bool {
	i := id % doneRing
	if i < 0 {
		i += doneRing
	}
	return r.doneOK[i] && r.done[i] == id
}

func (r *Reassembler) markDone(id int64) {
	i := id % doneRing
	if i < 0 {
		i += doneRing
	}
	r.done[i], r.doneOK[i] = id, true
}

func (r *Reassembler) Add(m *wire.Media, recovered bool, now int64) *Frame {
	id := r.ids.Unwrap(m.FrameID)
	seq := r.seqs.Unwrap(m.Seq)
	ts := r.tsw.Unwrap(m.Timestamp)
	if r.isDone(id) {
		r.Duplicates++
		return nil
	}
	ref := int64(-1)
	if m.RefDelta != 0 {
		ref = id - int64(m.RefDelta)
	}
	flags := frameFlags(m)
	if m.FragCount == 1 {
		r.markDone(id)
		r.Completed++
		f := &Frame{ID: id, Keyframe: m.Keyframe, Layer: int(m.Layer), Ref: ref, Timestamp: ts,
			FirstSeq: seq, LastSeq: seq, Data: append([]byte(nil), m.Payload...), FirstArrival: now, Complete: now}
		f.setFlags(flags, m.RefDelta)
		if recovered {
			f.Recovered = 1
		}
		return f
	}
	p := r.partial[id]
	if p == nil {
		r.evict(now)
		p = &partial{id: id, count: int(m.FragCount), frags: make([][]byte, m.FragCount), keyframe: m.Keyframe,
			flags: flags, layer: int(m.Layer), ref: ref, ts: ts, firstSeq: seq - int64(m.FragIndex), first: now}
		r.partial[id] = p
		r.order = append(r.order, id)
	}
	if p.count != int(m.FragCount) || p.keyframe != m.Keyframe || p.flags != flags || p.layer != int(m.Layer) || p.ref != ref || p.firstSeq != seq-int64(m.FragIndex) {
		r.Corrupt++
		r.drop(id)
		return nil
	}
	if p.frags[m.FragIndex] != nil {
		r.Duplicates++
		return nil
	}
	p.frags[m.FragIndex] = append([]byte(nil), m.Payload...)
	p.have++
	p.bytes += len(m.Payload)
	r.bytes += len(m.Payload)
	if recovered {
		p.recovered++
	}
	if p.have < p.count {
		return nil
	}
	data := make([]byte, 0, p.bytes)
	for _, f := range p.frags {
		data = append(data, f...)
	}
	r.drop(id)
	r.markDone(id)
	r.Completed++
	f := &Frame{ID: id, Keyframe: p.keyframe, Layer: p.layer, Ref: p.ref, Timestamp: p.ts,
		FirstSeq: p.firstSeq, LastSeq: p.firstSeq + int64(p.count) - 1, Data: data,
		FirstArrival: p.first, Complete: now, Recovered: p.recovered}
	f.setFlags(p.flags, m.RefDelta)
	return f
}

func frameFlags(m *wire.Media) uint8 {
	var f uint8
	if m.Recovery {
		f |= wire.FlagRecovery
	}
	if m.LongTerm {
		f |= wire.FlagLongTerm
	}
	if m.Face {
		f |= wire.FlagFace
	}
	return f
}

func (f *Frame) setFlags(flags, aux uint8) {
	f.Recovery = flags&wire.FlagRecovery != 0
	f.LongTerm = flags&wire.FlagLongTerm != 0
	f.Face = flags&wire.FlagFace != 0
	f.Aux = int(aux)
}

func (r *Reassembler) drop(id int64) {
	p := r.partial[id]
	if p == nil {
		return
	}
	r.bytes -= p.bytes
	delete(r.partial, id)
	for i, x := range r.order {
		if x == id {
			r.order = append(r.order[:i], r.order[i+1:]...)
			break
		}
	}
}

func (r *Reassembler) evict(now int64) {
	for len(r.order) > 0 && (len(r.order) >= maxPartials || r.bytes > maxBytes) {
		r.Expired++
		r.drop(r.order[0])
	}
}

func (r *Reassembler) Expire(now int64) []Lost {
	var out []Lost
	for len(r.order) > 0 {
		p := r.partial[r.order[0]]
		if now-p.first < r.Timeout {
			break
		}
		out = append(out, Lost{ID: p.id, Keyframe: p.keyframe, Layer: p.layer})
		r.Expired++
		r.markDone(p.id)
		r.drop(p.id)
	}
	return out
}
