package link

import (
	"container/heap"
	"errors"
	"math/rand/v2"
	"slices"
)

type Transport interface {
	SendDatagram(p []byte) error
	MaxDatagramSize() int
	SendControl(p []byte) error
}

type Handler interface {
	HandleDatagram(p []byte, now int64)
	HandleControl(p []byte, now int64)
}

var (
	ErrTooLarge = errors.New("link: datagram larger than the maximum")
	ErrClosed   = errors.New("link: closed")
)

type event struct {
	at  int64
	seq uint64
	fn  func()
}

type events []event

func (h events) Len() int { return len(h) }
func (h events) Less(i, j int) bool {
	if h[i].at != h[j].at {
		return h[i].at < h[j].at
	}
	return h[i].seq < h[j].seq
}
func (h events) Swap(i, j int) { h[i], h[j] = h[j], h[i] }
func (h *events) Push(x any)   { *h = append(*h, x.(event)) }
func (h *events) Pop() any {
	old := *h
	e := old[len(old)-1]
	*h = old[:len(old)-1]
	return e
}

type Sim struct {
	now  int64
	seq  uint64
	q    events
	rng  *rand.Rand
	seed uint64
}

func NewSim(seed uint64) *Sim {
	return &Sim{rng: rand.New(rand.NewPCG(seed, seed^0x9E3779B97F4A7C15)), seed: seed}
}

func (s *Sim) Now() int64 { return s.now }

func (s *Sim) At(t int64, fn func()) {
	s.seq++
	heap.Push(&s.q, event{at: max(t, s.now), seq: s.seq, fn: fn})
}

func (s *Sim) Every(start, period int64, fn func()) {
	var tick func()
	next := start
	tick = func() {
		fn()
		next += period
		s.At(next, tick)
	}
	s.At(start, tick)
}

func (s *Sim) RunUntil(t int64) {
	for len(s.q) > 0 && s.q[0].at <= t {
		e := heap.Pop(&s.q).(event)
		s.now = e.at
		e.fn()
	}
	s.now = max(s.now, t)
}

type Params struct {
	DelayUs     int64
	JitterUs    int64
	InOrder     bool
	Loss        float64
	BurstEnter  float64
	BurstLeave  float64
	BurstLoss   float64
	ReorderRate float64
	ReorderUs   int64
	RateBps     int
	QueueUs     int64
	OverheadB   int
}

type Stats struct {
	Sent        int
	Delivered   int
	QueueDrops  int
	RandomDrops int
	SentBytes   int64
	queueDelays []int64
}

func (s *Stats) QueueDelayPercentile(p float64) int64 {
	if len(s.queueDelays) == 0 {
		return 0
	}
	d := slices.Clone(s.queueDelays)
	slices.Sort(d)
	return d[min(len(d)-1, int(p*float64(len(d))))]
}

func (s *Stats) QueueDelaysSince(i int) []int64 { return s.queueDelays[min(i, len(s.queueDelays)):] }

func (s *Stats) Samples() int { return len(s.queueDelays) }

type Link struct {
	sim         *Sim
	p           Params
	rng         *rand.Rand
	bad         bool
	busyUntil   int64
	lastArrival int64
	lastControl int64
	dst         *Endpoint
	Stats       Stats
}

func (l *Link) SetParams(p Params) { l.p = p }

func (l *Link) Params() Params { return l.p }

func (l *Link) QueueDelay(now int64) int64 { return max(0, l.busyUntil-now) }

func (l *Link) lose() bool {
	if l.p.BurstEnter > 0 {
		if l.bad {
			l.bad = l.rng.Float64() >= l.p.BurstLeave
		} else {
			l.bad = l.rng.Float64() < l.p.BurstEnter
		}
		if l.bad && l.rng.Float64() < l.p.BurstLoss {
			return true
		}
	}
	return l.p.Loss > 0 && l.rng.Float64() < l.p.Loss
}

func (l *Link) send(b []byte, control bool) {
	now := l.sim.now
	l.Stats.Sent++
	l.Stats.SentBytes += int64(len(b))
	depart := now
	if l.p.RateBps > 0 {
		start := max(now, l.busyUntil)
		wait := start - now
		limit := l.p.QueueUs
		if limit == 0 {
			limit = 200_000
		}
		if wait > limit && !control {
			l.Stats.QueueDrops++
			return
		}
		ser := int64(len(b)+l.p.OverheadB) * 8_000_000 / int64(l.p.RateBps)
		depart = start + ser
		l.busyUntil = depart
		l.Stats.queueDelays = append(l.Stats.queueDelays, wait)
	} else {
		l.Stats.queueDelays = append(l.Stats.queueDelays, 0)
	}
	lost := l.lose()
	if lost && !control {
		l.Stats.RandomDrops++
		return
	}
	arrive := depart + l.p.DelayUs
	if l.p.JitterUs > 0 {
		arrive += l.rng.Int64N(2*l.p.JitterUs+1) - l.p.JitterUs
	}
	if l.p.ReorderRate > 0 && l.rng.Float64() < l.p.ReorderRate {
		arrive += l.p.ReorderUs
	}
	arrive = max(arrive, depart)
	if control {
		if lost {
			arrive += 3*l.p.DelayUs + 20_000
		}
		arrive = max(arrive, l.lastControl)
		l.lastControl = arrive
	} else if l.p.InOrder {
		arrive = max(arrive, l.lastArrival)
		l.lastArrival = arrive
	}
	dst := l.dst
	l.sim.At(arrive, func() {
		if dst.closed || dst.handler == nil {
			return
		}
		l.Stats.Delivered++
		if control {
			dst.handler.HandleControl(b, l.sim.now)
		} else {
			dst.handler.HandleDatagram(b, l.sim.now)
		}
	})
}

type Endpoint struct {
	sim     *Sim
	out     *Link
	handler Handler
	max     int
	closed  bool
}

func (e *Endpoint) SetHandler(h Handler) { e.handler = h }

func (e *Endpoint) Out() *Link { return e.out }

func (e *Endpoint) Close() { e.closed = true }

func (e *Endpoint) MaxDatagramSize() int { return e.max }

func (e *Endpoint) SendDatagram(p []byte) error {
	if e.closed {
		return ErrClosed
	}
	if len(p) > e.max {
		return ErrTooLarge
	}
	e.out.send(slices.Clone(p), false)
	return nil
}

func (e *Endpoint) SendControl(p []byte) error {
	if e.closed {
		return ErrClosed
	}
	e.out.send(slices.Clone(p), true)
	return nil
}

func (s *Sim) Pipe(aToB, bToA Params, maxDatagram int) (a, b *Endpoint) {
	a = &Endpoint{sim: s, max: maxDatagram}
	b = &Endpoint{sim: s, max: maxDatagram}
	a.out = &Link{sim: s, p: aToB, dst: b, rng: rand.New(rand.NewPCG(s.rng.Uint64(), s.rng.Uint64()))}
	b.out = &Link{sim: s, p: bToA, dst: a, rng: rand.New(rand.NewPCG(s.rng.Uint64(), s.rng.Uint64()))}
	return a, b
}
