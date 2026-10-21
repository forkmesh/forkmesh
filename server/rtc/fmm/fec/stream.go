package fec

import (
	"encoding/binary"
	"slices"

	"forkmesh.com/mirror-node/rtc/fmm/wire"
)

type Params struct {
	K int
	M int
}

func (p Params) On() bool { return p.K > 0 && p.M > 0 }

func (p Params) Ratio() float64 {
	if !p.On() {
		return 0
	}
	return float64(p.M) / float64(p.K)
}

type member struct {
	seq   uint16
	block []byte
}

type openGroup struct {
	members  []member
	started  int64
	maxBlock int
	low      uint16
	high     uint16
}

type Encoder struct {
	stream      uint8
	params      Params
	other       Params
	unequal     bool
	maxDelay    int64
	maxDatagram int
	open        [2]openGroup
	Groups      int
	Repairs     int
}

func NewEncoder(stream uint8, maxDatagram int, maxDelayUs int64) *Encoder {
	return &Encoder{stream: stream, maxDatagram: min(maxDatagram, wire.MaxDatagramLen), maxDelay: maxDelayUs}
}

func (e *Encoder) SetUnequal(on bool) { e.unequal = on }

func (e *Encoder) Params() Params { return e.params }

func (e *Encoder) Ratio(importantShare float64) float64 {
	if !e.unequal {
		return e.params.Ratio()
	}
	return importantShare*e.params.Ratio() + (1-importantShare)*e.other.Ratio()
}

func clampParams(p Params) Params {
	p.K = min(max(p.K, 0), wire.MaxFECK)
	p.M = min(max(p.M, 0), wire.MaxFECM)
	return p
}

func (e *Encoder) SetParams(p Params) {
	e.params = clampParams(p)
	e.other = e.params
}

func (e *Encoder) SetClassParams(important, other Params) {
	e.params = clampParams(important)
	e.other = clampParams(other)
}

func (e *Encoder) Scale(f float64) {
	scale := func(p Params) Params {
		if !p.On() {
			return p
		}
		p.M = int(float64(p.M) * f)
		if p.M == 0 {
			return Params{}
		}
		return p
	}
	e.params, e.other = scale(e.params), scale(e.other)
}

func (e *Encoder) classParams(which int) Params {
	if which == 1 {
		return e.other
	}
	return e.params
}

func importance(pkt []byte) bool {
	return pkt[10]&wire.FlagKeyframe != 0 || pkt[10]&0x07 == 0
}

func (e *Encoder) Add(pkt []byte, now int64) [][]byte {
	if len(pkt) < wire.MediaHeaderLen {
		return nil
	}
	block := recoveryBlock(pkt)
	which := 0
	if e.unequal && !importance(pkt) {
		which = 1
	}
	params := e.classParams(which)
	if !params.On() || wire.RepairHeaderLen+1+len(block) > e.maxDatagram {
		wire.SetFECFlag(pkt, false)
		if !params.On() {
			return e.emit(which)
		}
		return nil
	}
	g := &e.open[which]
	seq := wire.SeqOf(pkt)
	var out [][]byte
	if len(g.members) > 0 {
		lo, hi := g.low, g.high
		if int16(seq-lo) < 0 {
			lo = seq
		}
		if int16(seq-hi) > 0 {
			hi = seq
		}
		dup := false
		for _, m := range g.members {
			if m.seq == seq {
				dup = true
			}
		}
		if dup || uint16(hi-lo) > wire.MaxRepairSpan || wire.RepairHeaderLen+len(g.members)+1+max(g.maxBlock, len(block)) > e.maxDatagram {
			out = e.emit(which)
		}
	}
	wire.SetFECFlag(pkt, true)
	block = recoveryBlock(pkt)
	if len(g.members) == 0 {
		g.started = now
		g.low, g.high = seq, seq
	}
	if int16(seq-g.low) < 0 {
		g.low = seq
	}
	if int16(seq-g.high) > 0 {
		g.high = seq
	}
	g.members = append(g.members, member{seq: seq, block: block})
	g.maxBlock = max(g.maxBlock, len(block))
	if len(g.members) >= params.K {
		out = append(out, e.emit(which)...)
	}
	return out
}

func (e *Encoder) Flush(now int64) [][]byte {
	var out [][]byte
	for i := range e.open {
		if len(e.open[i].members) > 0 && now-e.open[i].started >= e.maxDelay {
			out = append(out, e.emit(i)...)
		}
	}
	return out
}

func recoveryBlock(pkt []byte) []byte {
	body := pkt[wire.RecoveryOffset:]
	b := make([]byte, 2+len(body))
	binary.BigEndian.PutUint16(b, uint16(len(body)))
	copy(b[2:], body)
	return b
}

func (e *Encoder) emit(which int) [][]byte {
	g := &e.open[which]
	k := len(g.members)
	if k == 0 {
		return nil
	}
	m := 1
	if p := e.classParams(which); p.On() {
		m = max(1, (p.M*k+p.K-1)/p.K)
	}
	m = min(m, wire.MaxFECM)
	slices.SortFunc(g.members, func(a, b member) int { return int(int16(a.seq - b.seq)) })
	base := g.members[0].seq
	offsets := make([]uint8, k)
	blocks := make([][]byte, k)
	for i, mb := range g.members {
		offsets[i] = uint8(mb.seq - base)
		b := mb.block
		if len(b) < g.maxBlock {
			b = append(b, make([]byte, g.maxBlock-len(b))...)
		}
		blocks[i] = b
	}
	syms := Encode(blocks, m)
	scheme := uint8(wire.SchemeRS)
	if m == 1 {
		scheme = wire.SchemeXOR
	}
	out := make([][]byte, m)
	for j, sym := range syms {
		r := wire.Repair{Stream: e.stream, BaseSeq: base, Scheme: scheme, K: uint8(k), M: uint8(m), Index: uint8(j), Offsets: offsets, Symbol: sym}
		out[j] = r.Append(make([]byte, 0, wire.RepairHeaderLen+k+len(sym)))
	}
	*g = openGroup{members: g.members[:0]}
	e.Groups++
	e.Repairs += m
	return out
}

const (
	slotCount = 1024
	maxGroups = 64
	groupLife = 1_000_000
)

type slot struct {
	seq   uint16
	valid bool
	block []byte
}

type group struct {
	base    uint16
	members []uint16
	k, m    int
	scheme  uint8
	size    int
	repairs [][]byte
	created int64
	done    bool
}

func (g *group) has(seq uint16) bool {
	return slices.Contains(g.members, seq)
}

type Decoder struct {
	stream    uint8
	slots     [slotCount]slot
	groups    []*group
	Recovered int
	Failed    int
}

func NewDecoder(stream uint8) *Decoder { return &Decoder{stream: stream} }

func (d *Decoder) AddMedia(pkt []byte, protected bool, now int64) [][]byte {
	seq := wire.SeqOf(pkt)
	s := &d.slots[seq%slotCount]
	if s.valid && s.seq == seq {
		return nil
	}
	if !protected {
		*s = slot{}
		return nil
	}
	*s = slot{seq: seq, valid: true, block: recoveryBlock(pkt)}
	var out [][]byte
	for _, g := range d.groups {
		if !g.done && g.has(seq) {
			out = append(out, d.try(g)...)
		}
	}
	d.prune(now)
	return out
}

func (d *Decoder) AddRepair(r wire.Repair, now int64) [][]byte {
	var g *group
	for _, x := range d.groups {
		if x.base == r.BaseSeq && x.k == int(r.K) && x.m == int(r.M) && x.scheme == r.Scheme && sameOffsets(x, r) {
			g = x
			break
		}
	}
	if g == nil {
		d.prune(now)
		if len(d.groups) >= maxGroups {
			d.groups = d.groups[1:]
		}
		g = &group{base: r.BaseSeq, members: r.Members(nil), k: int(r.K), m: int(r.M), scheme: r.Scheme, size: len(r.Symbol), repairs: make([][]byte, r.M), created: now}
		d.groups = append(d.groups, g)
	}
	if g.done || len(r.Symbol) != g.size || g.repairs[r.Index] != nil {
		return nil
	}
	g.repairs[r.Index] = append([]byte(nil), r.Symbol...)
	return d.try(g)
}

func sameOffsets(g *group, r wire.Repair) bool {
	for i, o := range r.Offsets {
		if g.members[i] != r.BaseSeq+uint16(o) {
			return false
		}
	}
	return true
}

func (d *Decoder) try(g *group) [][]byte {
	data := make([][]byte, g.k)
	have, missing := 0, 0
	for i := range g.k {
		seq := g.members[i]
		s := &d.slots[seq%slotCount]
		if s.valid && s.seq == seq {
			if len(s.block) > g.size {
				g.done = true
				d.Failed++
				return nil
			}
			b := s.block
			if len(b) < g.size {
				b = append(append(make([]byte, 0, g.size), b...), make([]byte, g.size-len(b))...)
			}
			data[i] = b
			have++
		} else {
			missing++
		}
	}
	if missing == 0 {
		g.done = true
		return nil
	}
	reps := 0
	for _, r := range g.repairs {
		if r != nil {
			reps++
		}
	}
	if reps < missing {
		return nil
	}
	if Reconstruct(data, g.repairs) != nil {
		g.done = true
		d.Failed++
		return nil
	}
	g.done = true
	var out [][]byte
	for i := range g.k {
		seq := g.members[i]
		s := &d.slots[seq%slotCount]
		if s.valid && s.seq == seq {
			continue
		}
		pkt, ok := rebuild(d.stream, seq, data[i])
		if !ok {
			d.Failed++
			continue
		}
		*s = slot{seq: seq, valid: true, block: recoveryBlock(pkt)}
		d.Recovered++
		out = append(out, pkt)
	}
	return out
}

func rebuild(stream uint8, seq uint16, block []byte) ([]byte, bool) {
	if len(block) < 2 {
		return nil, false
	}
	n := int(binary.BigEndian.Uint16(block))
	if n < wire.MediaHeaderLen-wire.RecoveryOffset || 2+n > len(block) {
		return nil, false
	}
	pkt := make([]byte, wire.RecoveryOffset, wire.RecoveryOffset+n)
	pkt[0] = wire.Version<<4 | wire.KindMedia
	pkt[1] = stream
	wire.SetSeq(pkt, seq)
	pkt = append(pkt, block[2:2+n]...)
	if _, err := wire.ParseMedia(pkt); err != nil {
		return nil, false
	}
	return pkt, true
}

func (d *Decoder) prune(now int64) {
	keep := d.groups[:0]
	for _, g := range d.groups {
		if !g.done && now-g.created < groupLife {
			keep = append(keep, g)
		}
	}
	for i := len(keep); i < len(d.groups); i++ {
		d.groups[i] = nil
	}
	d.groups = keep
}
