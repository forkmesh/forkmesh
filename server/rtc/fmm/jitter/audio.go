package jitter

import "slices"

const (
	StatusWaiting = 0
	StatusPlay    = 1
	StatusConceal = 2
	StatusInsert  = 3
	StatusDRED    = 4

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
	MaxExtraUs   int64
	NoLookAhead  bool
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
	if c.MaxExtraUs == 0 {
		c.MaxExtraUs = 100_000
	}
	return c
}

type delaySample struct {
	at    int64
	delay int64
}

type audioFrame struct {
	data  []byte
	depth int64
}

type lossRun struct {
	at    int64
	start int64
	n     int64
	need  int64
}

type AudioOut struct {
	Status int
	Data   []byte
	Offset int
}

type AudioStats struct {
	Played    int
	Concealed int
	FEC       int
	DRED      int
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
	arrived    int64
	runs       []lossRun
	depth      int64
	extra      int64
	base       int64
	samples    []delaySample
	minQ       []delaySample
	target     int64
	lastTarget int64
	lastAdjust int64
	Stats      AudioStats
}

const (
	minWindowUs     = 10_000_000
	maxBuffered     = 150
	resetAheadUs    = 3_000_000
	flushOverUs     = 200_000
	decayPerSec     = 100_000
	runWindowUs     = 15_000_000
	maxRuns         = 64
	minLossRuns     = 2
	settleUs        = 200_000
	lookAheadGoal   = 0.005
	lookAheadStepUs = 10_000
)

func NewAudio(cfg AudioConfig) *Audio {
	cfg = cfg.withDefaults()
	return &Audio{cfg: cfg, frames: map[int64]audioFrame{}, target: cfg.StartDelayUs}
}

func (a *Audio) TargetUs() int64 { return a.target }

func (a *Audio) tsOf(id int64) int64 { return a.firstTS + (id-a.firstID)*FrameUs }

func (a *Audio) ExtraUs() int64 { return a.extra }

func (a *Audio) Push(id int64, ts int64, data []byte, arrival int64) {
	a.PushDepth(id, ts, data, 0, arrival)
}

func (a *Audio) PushDepth(id int64, ts int64, data []byte, depthUs int64, arrival int64) {
	if !a.started {
		a.started = true
		a.firstID, a.firstTS, a.next, a.newest, a.arrived = id, ts, id, id, id
	}
	if (id-a.next)*FrameUs > resetAheadUs {
		a.Stats.Early++
		clear(a.frames)
		a.started, a.playing = true, false
		a.firstID, a.firstTS, a.next, a.newest, a.arrived = id, ts, id, id, id
		a.samples, a.minQ, a.runs = a.samples[:0], a.minQ[:0], a.runs[:0]
	}
	if _, ok := a.frames[id]; ok {
		return
	}
	a.sampleDelay(arrival-a.tsOf(id), arrival)
	a.depth = depthUs
	a.sampleLoss(id, arrival)
	if id < a.next {
		a.Stats.Late++
		return
	}
	a.frames[id] = audioFrame{data: data, depth: depthUs}
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

func (a *Audio) sampleLoss(id, arrival int64) {
	if id > a.arrived+1 {
		r := lossRun{at: arrival, start: a.arrived + 1, n: id - a.arrived - 1, need: -1}
		if a.depth >= r.n*FrameUs || r.n == 1 {
			r.need = arrival - (a.tsOf(r.start) + a.dmin() + a.base)
		}
		a.runs = append(a.runs, r)
	}
	if id < a.arrived {
		for i := len(a.runs) - 1; i >= 0; i-- {
			r := &a.runs[i]
			if id < r.start || id >= r.start+r.n {
				continue
			}
			switch {
			case r.n == 1:
				a.runs = append(a.runs[:i], a.runs[i+1:]...)
			case id == r.start:
				r.start++
				r.n--
			case id == r.start+r.n-1:
				r.n--
			default:
				tail := lossRun{at: r.at, start: id + 1, n: r.start + r.n - id - 1}
				r.n = id - r.start
				a.runs = slices.Insert(a.runs, i+1, tail)
			}
			break
		}
	}
	a.arrived = max(a.arrived, id)
	cut := 0
	for cut < len(a.runs) && (a.runs[cut].at < arrival-runWindowUs || len(a.runs)-cut > maxRuns) {
		cut++
	}
	if cut > 0 {
		a.runs = append(a.runs[:0], a.runs[cut:]...)
	}
}

func (a *Audio) lookAhead(now int64) int64 {
	if a.cfg.NoLookAhead || len(a.runs) < minLossRuns {
		return 0
	}
	var settled []lossRun
	for _, r := range a.runs {
		if r.at <= now-settleUs {
			settled = append(settled, r)
		}
	}
	if len(settled) < minLossRuns {
		return 0
	}
	span := a.arrived - a.runs[0].start + 1
	if span <= 0 {
		return 0
	}
	goal := lookAheadGoal * float64(span)
	unrecovered := func(e int64) float64 {
		n := int64(0)
		for _, r := range settled {
			if r.need < 0 {
				continue
			}
			n += min(max((r.need-e+FrameUs-1)/FrameUs, 0), r.n)
		}
		return float64(n)
	}
	limit := a.cfg.MaxExtraUs
	if a.depth > 0 {
		limit = min(limit, a.depth)
	} else {
		limit = min(limit, FrameUs)
	}
	for e := int64(0); e <= limit; e += lookAheadStepUs {
		if unrecovered(e) <= goal {
			return e
		}
	}
	return limit
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
	a.extra = a.lookAhead(now)
	a.base = min(max(p-a.dmin()+a.cfg.MarginUs, a.cfg.MinDelayUs), a.cfg.MaxDelayUs)
	want := min(max(p-a.dmin()+a.cfg.MarginUs+a.extra, a.cfg.MinDelayUs), a.cfg.MaxDelayUs)
	if want >= a.target {
		a.target = want
	} else if a.lastTarget > 0 {
		step := decayPerSec * (now - a.lastTarget) / 1_000_000
		a.target = max(want, a.target-step)
	}
	a.lastTarget = now
}

func (a *Audio) Pull(now int64) ([]byte, int) {
	o := a.PullOut(now)
	return o.Data, o.Status
}

func (a *Audio) PullOut(now int64) AudioOut {
	if !a.started {
		return AudioOut{Status: StatusWaiting}
	}
	a.updateTarget(now)
	lag := now - (a.tsOf(a.next) + a.dmin())
	if !a.playing {
		if lag < a.target {
			return AudioOut{Status: StatusWaiting}
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
	if !ok && lag < a.target-a.cfg.HysteresisUs && now-a.lastAdjust >= a.cfg.AdjustGapUs/2 && a.recoverFrom(a.next) < 0 {
		a.lastAdjust = now
		a.Stats.Inserted++
		return AudioOut{Status: StatusInsert}
	}
	if ok && lag < a.target-a.cfg.HysteresisUs && now-a.lastAdjust >= a.cfg.AdjustGapUs {
		a.lastAdjust = now
		a.Stats.Inserted++
		return AudioOut{Status: StatusInsert}
	}
	id := a.next
	delete(a.frames, id)
	a.next++
	if ok {
		a.Stats.Played++
		return AudioOut{Status: StatusPlay, Data: f.data}
	}
	if m := a.recoverFrom(id); m >= 0 {
		a.Stats.DRED++
		return AudioOut{Status: StatusDRED, Data: a.frames[m].data, Offset: int(m - id)}
	}
	if n, ok := a.frames[id+1]; ok {
		a.Stats.FEC++
		a.Stats.Concealed++
		return AudioOut{Status: StatusConceal, Data: n.data}
	}
	a.Stats.Concealed++
	return AudioOut{Status: StatusConceal}
}

func (a *Audio) recoverFrom(id int64) int64 {
	for m := id + 1; m <= a.newest && (m-id)*FrameUs <= a.cfg.MaxDelayUs*4; m++ {
		f, ok := a.frames[m]
		if ok && f.depth >= (m-id)*FrameUs {
			return m
		}
	}
	return -1
}
