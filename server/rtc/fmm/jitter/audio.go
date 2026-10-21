package jitter

import "slices"

const (
	StatusWaiting = 0
	StatusPlay    = 1
	StatusConceal = 2
	StatusInsert  = 3

	FrameUs = 20_000
)

type AudioConfig struct {
	MinDelayUs   int64
	MaxDelayUs   int64
	StartDelayUs int64
	Percentile   float64
	WindowUs     int64
	MarginUs     int64
	HysteresisUs int64
	AdjustGapUs  int64
}

func (c AudioConfig) withDefaults() AudioConfig {
	if c.MinDelayUs == 0 {
		c.MinDelayUs = 20_000
	}
	if c.MaxDelayUs == 0 {
		c.MaxDelayUs = 400_000
	}
	if c.StartDelayUs == 0 {
		c.StartDelayUs = 60_000
	}
	if c.Percentile == 0 {
		c.Percentile = 0.98
	}
	if c.WindowUs == 0 {
		c.WindowUs = 3_000_000
	}
	if c.MarginUs == 0 {
		c.MarginUs = 5_000
	}
	if c.HysteresisUs == 0 {
		c.HysteresisUs = 15_000
	}
	if c.AdjustGapUs == 0 {
		c.AdjustGapUs = 100_000
	}
	return c
}

type delaySample struct {
	at    int64
	delay int64
}

type audioFrame struct {
	data []byte
	ok   bool
}

type AudioStats struct {
	Played    int
	Concealed int
	Inserted  int
	Dropped   int
	Late      int
	Early     int
}

type Audio struct {
	cfg        AudioConfig
	started    bool
	playing    bool
	firstID    int64
	firstTS    int64
	next       int64
	frames     map[int64]audioFrame
	newest     int64
	samples    []delaySample
	minQ       []delaySample
	target     int64
	lastTarget int64
	lastAdjust int64
	Stats      AudioStats
}

const (
	minWindowUs  = 10_000_000
	maxBuffered  = 150
	resetAheadUs = 3_000_000
	flushOverUs  = 200_000
	decayPerSec  = 100_000
)

func NewAudio(cfg AudioConfig) *Audio {
	cfg = cfg.withDefaults()
	return &Audio{cfg: cfg, frames: map[int64]audioFrame{}, target: cfg.StartDelayUs}
}

func (a *Audio) TargetUs() int64 { return a.target }

func (a *Audio) tsOf(id int64) int64 { return a.firstTS + (id-a.firstID)*FrameUs }

func (a *Audio) Push(id int64, ts int64, data []byte, arrival int64) {
	if !a.started {
		a.started = true
		a.firstID, a.firstTS, a.next, a.newest = id, ts, id, id
	}
	if (id-a.next)*FrameUs > resetAheadUs {
		a.Stats.Early++
		clear(a.frames)
		a.started, a.playing = true, false
		a.firstID, a.firstTS, a.next, a.newest = id, ts, id, id
		a.samples, a.minQ = a.samples[:0], a.minQ[:0]
	}
	if _, ok := a.frames[id]; ok {
		return
	}
	a.sampleDelay(arrival-a.tsOf(id), arrival)
	if id < a.next {
		a.Stats.Late++
		return
	}
	a.frames[id] = audioFrame{data: data, ok: true}
	a.newest = max(a.newest, id)
	for len(a.frames) > maxBuffered {
		delete(a.frames, a.next)
		a.next++
		a.Stats.Dropped++
	}
}

func (a *Audio) sampleDelay(d, arrival int64) {
	a.samples = append(a.samples, delaySample{at: arrival, delay: d})
	for len(a.minQ) > 0 && a.minQ[len(a.minQ)-1].delay >= d {
		a.minQ = a.minQ[:len(a.minQ)-1]
	}
	a.minQ = append(a.minQ, delaySample{at: arrival, delay: d})
	cut := 0
	for cut < len(a.samples) && a.samples[cut].at < arrival-a.cfg.WindowUs {
		cut++
	}
	if cut > 0 {
		a.samples = append(a.samples[:0], a.samples[cut:]...)
	}
	for len(a.minQ) > 1 && a.minQ[0].at < arrival-minWindowUs {
		a.minQ = a.minQ[1:]
	}
}

func (a *Audio) dmin() int64 { return a.minQ[0].delay }

func (a *Audio) updateTarget(now int64) {
	if len(a.samples) == 0 {
		return
	}
	d := make([]int64, len(a.samples))
	for i, s := range a.samples {
		d[i] = s.delay
	}
	slices.Sort(d)
	p := d[min(len(d)-1, int(a.cfg.Percentile*float64(len(d))))]
	want := min(max(p-a.dmin()+a.cfg.MarginUs, a.cfg.MinDelayUs), a.cfg.MaxDelayUs)
	if want >= a.target {
		a.target = want
	} else if a.lastTarget > 0 {
		step := decayPerSec * (now - a.lastTarget) / 1_000_000
		a.target = max(want, a.target-step)
	}
	a.lastTarget = now
}

func (a *Audio) Pull(now int64) ([]byte, int) {
	if !a.started {
		return nil, StatusWaiting
	}
	a.updateTarget(now)
	lag := now - (a.tsOf(a.next) + a.dmin())
	if !a.playing {
		if lag < a.target {
			return nil, StatusWaiting
		}
		a.playing = true
		a.lastAdjust = now
	}
	if lag > a.target+flushOverUs {
		for lag > a.target && a.next < a.newest {
			delete(a.frames, a.next)
			a.next++
			a.Stats.Dropped++
			lag -= FrameUs
		}
	} else if lag > a.target+a.cfg.HysteresisUs && now-a.lastAdjust >= a.cfg.AdjustGapUs && a.next < a.newest {
		delete(a.frames, a.next)
		a.next++
		a.Stats.Dropped++
		a.lastAdjust = now
	}
	f, ok := a.frames[a.next]
	if !ok && lag < a.target-a.cfg.HysteresisUs && now-a.lastAdjust >= a.cfg.AdjustGapUs/2 {
		a.lastAdjust = now
		a.Stats.Inserted++
		return nil, StatusInsert
	}
	if ok && lag < a.target-a.cfg.HysteresisUs && now-a.lastAdjust >= a.cfg.AdjustGapUs {
		a.lastAdjust = now
		a.Stats.Inserted++
		return nil, StatusInsert
	}
	delete(a.frames, a.next)
	a.next++
	if ok {
		a.Stats.Played++
		return f.data, StatusPlay
	}
	a.Stats.Concealed++
	if n, ok := a.frames[a.next]; ok {
		return n.data, StatusConceal
	}
	return nil, StatusConceal
}
