package cc

import (
	"forkmesh.com/mirror-node/rtc/fmm/internal/seqnum"
	"forkmesh.com/mirror-node/rtc/fmm/wire"
)

const (
	TickUs          = 250
	recorderSlots   = 4096
	ReportInterval  = 50_000
	FinalizeHold    = 100_000
	reportMaxWindow = wire.MaxReportPackets
)

type recSlot struct {
	seq          int64
	has          bool
	reported     uint8
	arrival      int64
	missingSince int64
}

type Recorder struct {
	slots       [recorderSlots]recSlot
	started     bool
	base        int64
	highest     int64
	lastArrival int64
	lastReport  int64
	Repeat      int
	Packets     int
}

func (r *Recorder) OnPacket(tseq uint16, now int64) {
	r.Packets++
	if !r.started {
		r.started = true
		r.highest = int64(tseq)
		r.base = r.highest
		r.lastArrival = now
		r.lastReport = now - ReportInterval
		r.slots[r.highest%recorderSlots] = recSlot{seq: r.highest, has: true, arrival: now}
		return
	}
	s := seqnum.Near16(tseq, r.highest)
	if s < r.base {
		return
	}
	if s > r.highest+recorderSlots/2 {
		r.base = s
		r.highest = s
	}
	if s-r.base >= reportMaxWindow {
		r.base = s - reportMaxWindow + 1
	}
	sl := &r.slots[s%recorderSlots]
	if sl.seq == s && sl.has {
		return
	}
	*sl = recSlot{seq: s, has: true, arrival: now}
	if s > r.highest {
		r.highest = s
		r.lastArrival = now
	}
}

func (r *Recorder) Due(now int64) bool {
	return r.started && r.highest >= r.base && now-r.lastReport >= ReportInterval
}

func (r *Recorder) Report(now int64) *wire.Report {
	if !r.Due(now) {
		return nil
	}
	r.lastReport = now
	n := int(r.highest - r.base + 1)
	rep := &wire.Report{
		Base:     uint16(r.base),
		Received: make([]bool, n),
		Arrival:  make([]int64, n),
	}
	ref := int64(-1)
	prev := int64(0)
	for i := range n {
		s := r.base + int64(i)
		sl := &r.slots[s%recorderSlots]
		if sl.seq == s && sl.has {
			t := sl.arrival / TickUs
			if ref < 0 {
				ref = t
				rep.RefTime = uint32(t)
			}
			off := t - ref
			off = min(max(off, prev-32768), prev+32767)
			prev = off
			rep.Received[i] = true
			rep.Arrival[i] = off
			sl.reported++
			continue
		}
		if sl.seq != s {
			*sl = recSlot{seq: s, missingSince: now}
		}
	}
	rep.Delay = uint16(min((now-r.lastArrival)/TickUs, 0xFFFF))
	for r.base <= r.highest {
		sl := &r.slots[r.base%recorderSlots]
		if sl.seq == r.base && ((sl.has && int(sl.reported) > r.Repeat) || (!sl.has && now-sl.missingSince >= FinalizeHold)) {
			r.base++
			continue
		}
		break
	}
	return rep
}
