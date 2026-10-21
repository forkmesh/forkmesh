package quic

import (
	"time"
)

const (
	packetThreshold     = 3
	timerGranularity    = time.Millisecond
	defaultInitialRTT   = 333 * time.Millisecond
	persistentThreshold = 3
	maxPTOBackoff       = 16
)

type rttState struct {
	hasSample bool
	latest    time.Duration
	smoothed  time.Duration
	variance  time.Duration
	min       time.Duration
	firstAt   time.Time
}

func newRTT(initial time.Duration) rttState {
	return rttState{smoothed: initial, variance: initial / 2}
}

func (r *rttState) update(sample, ackDelay time.Duration, now time.Time) {
	if sample <= 0 {
		sample = time.Microsecond
	}
	r.latest = sample
	if !r.hasSample {
		r.hasSample = true
		r.min = sample
		r.smoothed = sample
		r.variance = sample / 2
		r.firstAt = now
		return
	}
	if sample < r.min {
		r.min = sample
	}
	adjusted := sample
	if sample >= r.min+ackDelay {
		adjusted = sample - ackDelay
	}
	diff := r.smoothed - adjusted
	if diff < 0 {
		diff = -diff
	}
	r.variance = (3*r.variance + diff) / 4
	r.smoothed = (7*r.smoothed + adjusted) / 8
}

func (r *rttState) pto(maxAckDelay time.Duration) time.Duration {
	v := 4 * r.variance
	if v < timerGranularity {
		v = timerGranularity
	}
	return r.smoothed + v + maxAckDelay
}

func (r *rttState) lossDelay() time.Duration {
	d := r.latest
	if r.smoothed > d {
		d = r.smoothed
	}
	d = d * 9 / 8
	if d < timerGranularity {
		d = timerGranularity
	}
	return d
}

type sentFrameKind uint8

const (
	sfStream sentFrameKind = iota + 1
	sfCrypto
	sfMaxData
	sfMaxStreamData
	sfMaxStreamsBidi
	sfMaxStreamsUni
	sfResetStream
	sfStopSending
	sfNewCID
	sfRetireCID
	sfHandshakeDone
	sfDataBlocked
	sfStreamDataBlocked
	sfStreamsBlockedBidi
	sfStreamsBlockedUni
	sfAck
	sfDatagram
	sfPing
)

type sentFrame struct {
	kind   sentFrameKind
	fin    bool
	id     uint64
	offset uint64
	length uint64
}

type sentPacket struct {
	pn           int64
	sentAt       time.Time
	size         int
	ackEliciting bool
	inFlight     bool
	frames       []sentFrame
}

type congestion struct {
	maxDatagram   int
	cwnd          int
	ssthresh      int
	inFlight      int
	recoveryStart time.Time
	ackedBytes    int
	cwndLimited   bool
}

func newCongestion(maxDatagram int) congestion {
	c := congestion{maxDatagram: maxDatagram, ssthresh: 1 << 62}
	c.cwnd = c.initialWindow()
	return c
}

func (c *congestion) initialWindow() int {
	w := 2 * c.maxDatagram
	if w < 14720 {
		w = 14720
	}
	if lim := 10 * c.maxDatagram; w > lim {
		w = lim
	}
	return w
}

func (c *congestion) minWindow() int { return 2 * c.maxDatagram }

func (c *congestion) available() int {
	if c.inFlight >= c.cwnd {
		return 0
	}
	return c.cwnd - c.inFlight
}

func (c *congestion) onSent(size int) {
	c.inFlight += size
}

func (c *congestion) remove(size int) {
	c.inFlight -= size
	if c.inFlight < 0 {
		c.inFlight = 0
	}
}

func (c *congestion) onAcked(p *sentPacket) {
	c.remove(p.size)
	if !p.sentAt.After(c.recoveryStart) {
		return
	}
	if !c.cwndLimited {
		return
	}
	if c.cwnd < c.ssthresh {
		c.cwnd += p.size
		return
	}
	c.ackedBytes += p.size
	if c.ackedBytes >= c.cwnd {
		c.ackedBytes -= c.cwnd
		c.cwnd += c.maxDatagram
	}
}

func (c *congestion) onCongestionEvent(sentAt, now time.Time) {
	if !sentAt.After(c.recoveryStart) {
		return
	}
	c.recoveryStart = now
	c.ssthresh = c.cwnd / 2
	if c.ssthresh < c.minWindow() {
		c.ssthresh = c.minWindow()
	}
	c.cwnd = c.ssthresh
	c.ackedBytes = 0
}

func (c *congestion) onPersistentCongestion() {
	c.cwnd = c.minWindow()
	c.recoveryStart = time.Time{}
	c.ackedBytes = 0
}

func (c *congestion) reset() {
	inFlight := c.inFlight
	*c = newCongestion(c.maxDatagram)
	c.inFlight = inFlight
}

type pacer struct {
	tokens   float64
	last     time.Time
	maxBurst float64
}

func (p *pacer) rate(cwnd int, srtt time.Duration) float64 {
	if srtt <= 0 {
		srtt = timerGranularity
	}
	return 1.25 * float64(cwnd) / srtt.Seconds()
}

func (p *pacer) refill(now time.Time, cwnd int, srtt time.Duration) {
	if p.maxBurst == 0 {
		p.maxBurst = float64(cwnd)
	}
	if p.last.IsZero() {
		p.last = now
		p.tokens = p.maxBurst
		return
	}
	elapsed := now.Sub(p.last).Seconds()
	if elapsed > 0 {
		p.tokens += elapsed * p.rate(cwnd, srtt)
		p.last = now
	}
	if p.tokens > p.maxBurst {
		p.tokens = p.maxBurst
	}
}

func (p *pacer) canSend(size int) bool { return p.tokens >= float64(size) }

func (p *pacer) onSent(size int) { p.tokens -= float64(size) }

func (p *pacer) nextSend(size int, cwnd int, srtt time.Duration, now time.Time) time.Time {
	missing := float64(size) - p.tokens
	if missing <= 0 {
		return now
	}
	return now.Add(time.Duration(missing / p.rate(cwnd, srtt) * float64(time.Second)))
}
