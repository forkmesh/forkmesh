package jitter

import (
	"slices"

	"forkmesh.com/mirror-node/rtc/fmm/frag"
)

type VideoConfig struct {
	MinWaitUs          int64
	MaxWaitUs          int64
	KeyframeIntervalUs int64
	RefWaitUs          int64
	Percentile         float64
	WindowUs           int64
	RecoveryIntervalUs int64
	EscalateUs         int64
	NoRecovery         bool
}

func (c VideoConfig) withDefaults() VideoConfig {
	if c.MinWaitUs == 0 {
		c.MinWaitUs = 30_000
	}
	if c.MaxWaitUs == 0 {
		c.MaxWaitUs = 300_000
	}
	if c.KeyframeIntervalUs == 0 {
		c.KeyframeIntervalUs = 500_000
	}
	if c.RefWaitUs == 0 {
		c.RefWaitUs = 1_000_000
	}
	if c.Percentile == 0 {
		c.Percentile = 0.95
	}
	if c.WindowUs == 0 {
		c.WindowUs = 3_000_000
	}
	if c.RecoveryIntervalUs == 0 {
		c.RecoveryIntervalUs = 250_000
	}
	if c.EscalateUs == 0 {
		c.EscalateUs = 600_000
	}
	return c
}

type VideoStats struct {
	Released         int
	Undecodable      int
	Late             int
	GapsSkipped      int
	KeyframeRequests int
	RecoveryRequests int
	Recovered        int
	Escalated        int
}

type Video struct {
	cfg         VideoConfig
	pending     []*frag.Frame
	started     bool
	lastID      int64
	lastSeq     int64
	holes       [][2]int64
	decoded     map[int64]bool
	decodedList []int64
	broken      map[int64]bool
	needKey     bool
	lastKeyReq  int64
	needRec     bool
	needRecAt   int64
	lastRecReq  int64
	acks        []int64
	samples     []delaySample
	minQ        []delaySample
	wait        int64
	Stats       VideoStats
}

const (
	maxPending = 256
	maxBroken  = 1024
	maxHoles   = 64
)

func NewVideo(cfg VideoConfig) *Video {
	cfg = cfg.withDefaults()
	return &Video{cfg: cfg, decoded: map[int64]bool{}, broken: map[int64]bool{}, wait: cfg.MinWaitUs * 2}
}

func (v *Video) WaitUs() int64 { return v.wait }

func (v *Video) Push(f *frag.Frame, now int64) {
	if v.started && f.ID <= v.lastID {
		v.Stats.Late++
		return
	}
	for _, p := range v.pending {
		if p.ID == f.ID {
			return
		}
	}
	i, _ := slices.BinarySearchFunc(v.pending, f.ID, func(p *frag.Frame, id int64) int {
		switch {
		case p.ID < id:
			return -1
		case p.ID > id:
			return 1
		}
		return 0
	})
	v.pending = slices.Insert(v.pending, i, f)
	if len(v.pending) > maxPending {
		v.pending = v.pending[1:]
	}
	v.sample(f)
}

func (v *Video) sample(f *frag.Frame) {
	d := f.Complete - f.Timestamp
	v.samples = append(v.samples, delaySample{at: f.Complete, delay: d})
	for len(v.minQ) > 0 && v.minQ[len(v.minQ)-1].delay >= d {
		v.minQ = v.minQ[:len(v.minQ)-1]
	}
	v.minQ = append(v.minQ, delaySample{at: f.Complete, delay: d})
	cut := 0
	for cut < len(v.samples) && v.samples[cut].at < f.Complete-v.cfg.WindowUs {
		cut++
	}
	if cut > 0 {
		v.samples = append(v.samples[:0], v.samples[cut:]...)
	}
	for len(v.minQ) > 1 && v.minQ[0].at < f.Complete-minWindowUs {
		v.minQ = v.minQ[1:]
	}
	if len(v.samples) >= 10 {
		d := make([]int64, len(v.samples))
		for i, s := range v.samples {
			d[i] = s.delay
		}
		slices.Sort(d)
		p := d[min(len(d)-1, int(v.cfg.Percentile*float64(len(d))))]
		v.wait = min(max(p-v.minQ[0].delay+10_000, v.cfg.MinWaitUs), v.cfg.MaxWaitUs)
	}
}

func (v *Video) Lost(l frag.Lost, now int64) {
	if l.Keyframe || l.Layer == 0 {
		v.broken[l.ID] = true
	}
}

func (v *Video) markDecoded(id int64) {
	v.decoded[id] = true
	v.decodedList = append(v.decodedList, id)
	if len(v.decodedList) > 512 {
		delete(v.decoded, v.decodedList[0])
		v.decodedList = v.decodedList[1:]
	}
}

func (v *Video) advance(last int64) {
	v.lastSeq = max(v.lastSeq, last)
	for changed := true; changed; {
		changed = false
		keep := v.holes[:0]
		for _, h := range v.holes {
			switch {
			case h[0] <= v.lastSeq+1:
				v.lastSeq = max(v.lastSeq, h[1])
				changed = true
			default:
				keep = append(keep, h)
			}
		}
		v.holes = keep
	}
}

func (v *Video) discard(f *frag.Frame) {
	v.pending = v.pending[1:]
	if f.FirstSeq <= v.lastSeq+1 {
		v.advance(f.LastSeq)
	} else if len(v.holes) < maxHoles {
		v.holes = append(v.holes, [2]int64{f.FirstSeq, f.LastSeq})
	}
	v.Stats.Undecodable++
	if len(v.broken) > maxBroken {
		clear(v.broken)
	}
	v.broken[f.ID] = true
}

func (v *Video) release(f *frag.Frame) *frag.Frame {
	v.pending = v.pending[1:]
	v.started = true
	v.lastID = f.ID
	v.advance(f.LastSeq)
	v.markDecoded(f.ID)
	v.Stats.Released++
	if f.Layer == 0 {
		v.needKey = false
		v.needRec = false
	}
	if f.LongTerm && len(v.acks) < 64 {
		v.acks = append(v.acks, f.ID)
	}
	return f
}

func (v *Video) chainBroken(now int64) {
	if v.cfg.NoRecovery || !v.started {
		v.needKey = true
		return
	}
	if !v.needRec {
		v.needRec, v.needRecAt = true, now
	}
}

func (v *Video) recoveryAt() int {
	for i, p := range v.pending {
		if p.Recovery && v.decoded[p.Ref] {
			return i
		}
	}
	return -1
}

func (v *Video) independent(f *frag.Frame) bool { return !f.Keyframe && f.Ref < 0 }

func (v *Video) Pop(now int64) *frag.Frame {
	for len(v.pending) > 0 {
		f := v.pending[0]
		if f.Keyframe {
			clear(v.decoded)
			v.decodedList = v.decodedList[:0]
			clear(v.broken)
			v.holes = v.holes[:0]
			v.lastSeq = f.FirstSeq - 1
			return v.release(f)
		}
		if v.independent(f) {
			if f.FirstSeq > v.lastSeq+1 {
				v.Stats.GapsSkipped++
			}
			v.lastSeq = max(v.lastSeq, f.FirstSeq-1)
			return v.release(f)
		}
		if !v.started {
			v.discard(f)
			v.needKey = true
			continue
		}
		waited := now - f.Complete
		if !v.decoded[f.Ref] {
			if i := v.recoveryAt(); i > 0 {
				for range i {
					v.discard(v.pending[0])
				}
				continue
			}
			if f.Ref > v.lastID && !v.broken[f.Ref] {
				limit := v.wait
				if f.Layer == 0 {
					if waited >= v.wait {
						v.chainBroken(now)
					}
					limit = max(v.wait, v.cfg.RefWaitUs)
				}
				if waited < limit {
					return nil
				}
			}
			v.discard(f)
			if f.Layer == 0 {
				v.chainBroken(now)
			}
			continue
		}
		if f.FirstSeq > v.lastSeq+1 && !f.Recovery {
			if waited < v.wait {
				return nil
			}
			v.Stats.GapsSkipped++
		}
		if f.Recovery {
			v.Stats.Recovered++
			v.lastSeq = max(v.lastSeq, f.FirstSeq-1)
		}
		return v.release(f)
	}
	return nil
}

func (v *Video) TakeAcks() []int64 {
	a := v.acks
	v.acks = nil
	return a
}

func (v *Video) LastID() int64 {
	if !v.started {
		return -1
	}
	return v.lastID
}

func (v *Video) SetTiming(rttUs int64) {
	v.cfg.RecoveryIntervalUs = min(max(2*rttUs+100_000, 200_000), 1_000_000)
	v.cfg.EscalateUs = min(max(3*rttUs+400_000, 500_000), 2_000_000)
}

func (v *Video) RecoveryNeeded(now int64) (bool, int64) {
	if !v.needRec || v.needKey {
		return false, 0
	}
	if now-v.needRecAt >= v.cfg.EscalateUs {
		v.needRec = false
		v.needKey = true
		v.Stats.Escalated++
		return false, 0
	}
	if v.lastRecReq != 0 && now-v.lastRecReq < v.cfg.RecoveryIntervalUs {
		return false, 0
	}
	v.lastRecReq = now
	v.Stats.RecoveryRequests++
	stuck := v.lastID + 1
	if len(v.pending) > 0 {
		stuck = v.pending[0].ID
	}
	return true, stuck
}

func (v *Video) KeyframeNeeded(now int64) bool {
	if !v.needKey {
		return false
	}
	if v.lastKeyReq != 0 && now-v.lastKeyReq < v.cfg.KeyframeIntervalUs {
		return false
	}
	v.lastKeyReq = now
	v.Stats.KeyframeRequests++
	return true
}
