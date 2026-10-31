package sfu

import (
	"math"

	"forkmesh.com/mirror-node/rtc/rtp"
)

const (
	historySlots       = 4096
	packetOverhead     = 28
	groupMicros        = 5000
	burstMicros        = 100_000
	trendWindow        = 20
	trendGain          = 4.0
	trendMaxDeltas     = 60
	thresholdStart     = 12.5
	thresholdUp        = 0.0087
	thresholdDown      = 0.039
	thresholdMaxOffset = 15.0
	overuseMicros      = 10000
	queueLimitMicros   = 100_000
	queueLossMicros    = 20_000
	minDelayBuckets    = 10
	minDelayBucket     = 1_000_000
	ackedWindowMicros  = 500_000
	ackedMinSpan       = 200_000
	ackedLimit         = 4096
	lossBuckets        = 4
	lossBucketMicros   = 250_000
	holdMicros         = 300_000
	decreaseFactor     = 0.85
	increasePerSecond  = 0.08
	increaseHeadroom   = 50_000
	minEstimate        = 50_000
	probeDuration      = 0.02
	probeMinPackets    = 5
	probeMaxPackets    = 40
	probePadding       = 240
	probeFirstInterval = 2_000_000
	probeMaxInterval   = 4_000_000
	probeQuiet         = 1_000_000
	probeTimeout       = 1_000_000
	probeSettle        = 300_000
	standingMicros     = 1_000_000
	standingFactor     = 0.9
	drainMicros        = 1_000_000
	drainFloor         = 0.5
)

type sentPacket struct {
	seq   int64
	at    int64
	size  int32
	probe int32
}

type trendline struct {
	started      bool
	groupFirst   int64
	groupSend    int64
	groupArrival int64
	groupStart   int64
	havePrev     bool
	prevSend     int64
	prevArrival  int64
	firstArrival int64
	accumulated  float64
	smoothed     float64
	xs           [trendWindow]float64
	ys           [trendWindow]float64
	count        int
	head         int
	deltas       int
	threshold    float64
	lastUpdate   int64
	prevTrend    float64
	overTime     float64
	overCount    int
	overusing    bool
	underusing   bool
}

func (t *trendline) reset() {
	*t = trendline{}
}

func (t *trendline) packet(send, arrival int64) {
	if !t.started {
		t.started = true
		t.groupFirst, t.groupSend, t.groupArrival, t.groupStart = send, send, arrival, arrival
		t.threshold = thresholdStart
		t.lastUpdate = -1
		t.overTime = -1
		return
	}
	if send < t.groupFirst {
		return
	}
	if send-t.groupFirst <= groupMicros || t.burst(send, arrival) {
		if send > t.groupSend {
			t.groupSend = send
		}
		if arrival > t.groupArrival {
			t.groupArrival = arrival
		}
		return
	}
	if t.havePrev {
		t.update(t.groupSend-t.prevSend, t.groupArrival-t.prevArrival, t.groupArrival)
	}
	t.havePrev = true
	t.prevSend, t.prevArrival = t.groupSend, t.groupArrival
	t.groupFirst, t.groupSend, t.groupArrival, t.groupStart = send, send, arrival, arrival
}

func (t *trendline) burst(send, arrival int64) bool {
	arrivalDelta := arrival - t.groupArrival
	sendDelta := send - t.groupSend
	if sendDelta == 0 {
		return true
	}
	return arrivalDelta-sendDelta < 0 && arrivalDelta <= groupMicros && arrival-t.groupStart < burstMicros
}

func (t *trendline) update(sendDelta, arrivalDelta, arrival int64) {
	if t.deltas == 0 {
		t.firstArrival = arrival
	}
	if t.deltas < 1000 {
		t.deltas++
	}
	delay := float64(arrivalDelta-sendDelta) / 1000
	t.accumulated += delay
	t.smoothed = 0.9*t.smoothed + 0.1*t.accumulated
	t.xs[t.head] = float64(arrival-t.firstArrival) / 1000
	t.ys[t.head] = t.smoothed
	t.head = (t.head + 1) % trendWindow
	if t.count < trendWindow {
		t.count++
	}
	trend := t.prevTrend
	if t.count == trendWindow {
		var sx, sy float64
		for i := 0; i < trendWindow; i++ {
			sx += t.xs[i]
			sy += t.ys[i]
		}
		mx, my := sx/trendWindow, sy/trendWindow
		var num, den float64
		for i := 0; i < trendWindow; i++ {
			num += (t.xs[i] - mx) * (t.ys[i] - my)
			den += (t.xs[i] - mx) * (t.xs[i] - mx)
		}
		if den != 0 {
			trend = num / den
		}
	}
	t.detect(trend, float64(sendDelta)/1000, arrival)
}

func (t *trendline) detect(trend, sendDeltaMs float64, now int64) {
	if t.deltas < 2 {
		t.overusing, t.underusing = false, false
		return
	}
	modified := math.Min(float64(t.deltas), trendMaxDeltas) * trend * trendGain
	switch {
	case modified > t.threshold:
		if t.overTime < 0 {
			t.overTime = sendDeltaMs / 2
		} else {
			t.overTime += sendDeltaMs
		}
		t.overCount++
		if t.overTime*1000 > overuseMicros && t.overCount > 1 && trend >= t.prevTrend {
			t.overTime, t.overCount = 0, 0
			t.overusing, t.underusing = true, false
		}
	case modified < -t.threshold:
		t.overTime, t.overCount = -1, 0
		t.overusing, t.underusing = false, true
	default:
		t.overTime, t.overCount = -1, 0
		t.overusing, t.underusing = false, false
	}
	t.prevTrend = trend
	if t.lastUpdate < 0 {
		t.lastUpdate = now
	}
	abs := math.Abs(modified)
	if abs > t.threshold+thresholdMaxOffset {
		t.lastUpdate = now
		return
	}
	k := thresholdUp
	if abs < t.threshold {
		k = thresholdDown
	}
	dt := math.Min(float64(now-t.lastUpdate)/1000, 100)
	t.threshold += k * (abs - t.threshold) * dt
	t.threshold = math.Max(6, math.Min(600, t.threshold))
	t.lastUpdate = now
}

type ackedBytes struct {
	at    int64
	bytes int64
}

type probeCluster struct {
	id       int32
	target   float64
	sentAt   int64
	packets  int
	reported int
	received int
	first    int64
	last     int64
	bytes    int64
	done     bool
}

type downlink struct {
	next          int64
	history       [historySlots]sentPacket
	trend         trendline
	minDelay      [minDelayBuckets]int64
	minBucket     int64
	queue         float64
	haveQueue     bool
	acked         []ackedBytes
	lossStart     int64
	lost          [lossBuckets]int
	got           [lossBuckets]int
	reference     int64
	haveReference bool
	estimate      float64
	lastDecrease  int64
	lastOveruse   int64
	lastIncrease  int64
	probe         probeCluster
	probes        int32
	nextProbe     int64
	probeInterval int64
	feedbacks     int
}

func newDownlink() *downlink {
	d := &downlink{next: seqOffset, probeInterval: probeFirstInterval}
	d.resetDelay()
	return d
}

func (d *downlink) resetDelay() {
	for i := range d.minDelay {
		d.minDelay[i] = math.MaxInt64
	}
	d.minBucket = 0
	d.haveQueue = false
	d.queue = 0
	d.trend.reset()
}

func (d *downlink) reset(now int64) {
	d.resetDelay()
	d.estimate = 0
	d.acked = d.acked[:0]
	d.lost, d.got = [lossBuckets]int{}, [lossBuckets]int{}
	d.probe = probeCluster{}
	d.probeInterval = probeFirstInterval
	d.nextProbe = now + probeFirstInterval
}

func (d *downlink) known() bool {
	return d.estimate > 0
}

func (d *downlink) stamp() (uint16, int64) {
	seq := d.next
	d.next++
	return uint16(seq), seq
}

func (d *downlink) sent(seq int64, at int64, size int, probe int32) {
	d.history[seq&(historySlots-1)] = sentPacket{seq: seq, at: at, size: int32(size + packetOverhead), probe: probe}
}

func (d *downlink) lookup(seq int64) (sentPacket, bool) {
	p := d.history[seq&(historySlots-1)]
	return p, p.seq == seq && p.size > 0
}

func (d *downlink) lossBucket(now int64) int {
	if d.lossStart == 0 {
		d.lossStart = now
	}
	for now-d.lossStart >= lossBucketMicros {
		d.lossStart += lossBucketMicros
		i := int(d.lossStart/lossBucketMicros) % lossBuckets
		d.lost[i], d.got[i] = 0, 0
		if now-d.lossStart > lossBuckets*lossBucketMicros {
			d.lossStart = now
			d.lost, d.got = [lossBuckets]int{}, [lossBuckets]int{}
		}
	}
	return int(d.lossStart/lossBucketMicros) % lossBuckets
}

func (d *downlink) lossRate() float64 {
	lost, total := 0, 0
	for i := range d.lost {
		lost += d.lost[i]
		total += d.lost[i] + d.got[i]
	}
	if total < 20 {
		return 0
	}
	return float64(lost) / float64(total)
}

func (d *downlink) ackedRate() float64 {
	if len(d.acked) == 0 {
		return 0
	}
	latest := d.acked[len(d.acked)-1].at
	var sum int64
	oldest := latest
	for _, a := range d.acked {
		sum += a.bytes
		if a.at < oldest {
			oldest = a.at
		}
	}
	span := latest - oldest
	if span < ackedMinSpan {
		span = ackedMinSpan
	}
	if span > ackedWindowMicros {
		span = ackedWindowMicros
	}
	return float64(sum) * 8e6 / float64(span)
}

func (d *downlink) delaySample(owd int64, now int64) {
	bucket := now / minDelayBucket
	if bucket != d.minBucket {
		for b := d.minBucket + 1; b <= bucket && b-d.minBucket <= minDelayBuckets; b++ {
			d.minDelay[b%minDelayBuckets] = math.MaxInt64
		}
		d.minBucket = bucket
	}
	slot := bucket % minDelayBuckets
	if owd < d.minDelay[slot] {
		d.minDelay[slot] = owd
	}
	floor := int64(math.MaxInt64)
	for _, m := range d.minDelay {
		if m < floor {
			floor = m
		}
	}
	q := float64(owd - floor)
	if !d.haveQueue {
		d.queue, d.haveQueue = q, true
	} else {
		d.queue += 0.05 * (q - d.queue)
	}
}

func (d *downlink) feedback(f *rtp.TransportFeedback, now int64) {
	base := unwrapSeq(d.next-1, true, f.BaseSequence)
	reference := int64(f.ReferenceTime)
	if d.haveReference {
		prev := d.reference / rtp.TCCReferenceMicros
		diff := (reference - prev) & 0xffffff
		if diff >= 1<<23 {
			diff -= 1 << 24
		}
		reference = prev + diff
	}
	clock := reference * rtp.TCCReferenceMicros
	d.reference, d.haveReference = clock, true
	d.feedbacks++
	bucket := d.lossBucket(now)
	for i, s := range f.Statuses {
		seq := base + int64(i)
		if s.Received {
			clock += int64(s.Delta) * rtp.TCCTickMicros
		}
		sent, ok := d.lookup(seq)
		if !ok {
			continue
		}
		if sent.probe != 0 && sent.probe == d.probe.id && !d.probe.done {
			d.probe.reported++
			if s.Received {
				if d.probe.received == 0 || clock < d.probe.first {
					d.probe.first = clock
				} else {
					d.probe.bytes += int64(sent.size)
				}
				if clock > d.probe.last {
					d.probe.last = clock
				}
				d.probe.received++
			}
		}
		if !s.Received {
			if sent.probe == 0 {
				d.lost[bucket]++
			}
			continue
		}
		if sent.probe != 0 {
			continue
		}
		d.got[bucket]++
		d.acked = append(d.acked, ackedBytes{at: clock, bytes: int64(sent.size)})
		d.delaySample(clock-sent.at, now)
		d.trend.packet(sent.at, clock)
	}
	if len(d.acked) > 0 {
		cutoff := d.acked[len(d.acked)-1].at - ackedWindowMicros
		keep := 0
		for keep < len(d.acked) && d.acked[keep].at < cutoff {
			keep++
		}
		if keep > 0 {
			d.acked = append(d.acked[:0], d.acked[keep:]...)
		}
		if len(d.acked) > ackedLimit {
			d.acked = append(d.acked[:0], d.acked[len(d.acked)-ackedLimit:]...)
		}
	}
	d.finishProbe(now, false)
	d.adapt(now)
}

func (d *downlink) overuse() bool {
	return d.trend.overusing || (d.haveQueue && d.queue > queueLimitMicros)
}

func (d *downlink) adapt(now int64) {
	acked := d.ackedRate()
	loss := d.lossRate()
	over := d.overuse() && (d.probe.id == 0 || now-d.probe.sentAt > probeSettle)
	if over {
		d.lastOveruse = now
	}
	held := now-d.lastDecrease < holdMicros
	switch {
	case over && !held && acked > 0:
		factor := decreaseFactor
		if d.haveQueue {
			factor = math.Min(factor, math.Max(drainFloor, 1-d.queue/drainMicros))
		}
		target := factor * acked
		switch {
		case !d.known() || target < d.estimate:
			d.estimate = target
			d.lastDecrease = now
		case now-d.lastDecrease >= standingMicros:
			d.estimate *= standingFactor
			d.lastDecrease = now
		}
		d.lastIncrease = now
	case loss > 0.1 && !held && acked > 0 && ((d.haveQueue && d.queue > queueLossMicros) || now-d.lastOveruse < 1_000_000):
		base := acked
		if d.known() && d.estimate < base {
			base = d.estimate
		}
		d.estimate = base * (1 - 0.5*loss)
		d.lastDecrease = now
		d.lastIncrease = now
	case d.known() && !over && !held && !d.trend.underusing && loss < 0.1:
		dt := float64(now-d.lastIncrease) / 1e6
		if dt > 1 {
			dt = 1
		}
		if dt > 0 {
			limit := 1.5*acked + increaseHeadroom
			if d.estimate < limit {
				d.estimate = math.Min(d.estimate*(1+increasePerSecond*dt), limit)
			}
		}
		d.lastIncrease = now
	default:
		d.lastIncrease = now
	}
	if d.known() && d.estimate < minEstimate {
		d.estimate = minEstimate
	}
}

func (d *downlink) finishProbe(now int64, force bool) {
	p := &d.probe
	if p.id == 0 || p.done {
		return
	}
	if p.reported < p.packets && now-p.sentAt < probeTimeout && !force {
		return
	}
	p.done = true
	if p.received < probeMinPackets-1 || p.received*5 < p.packets*3 {
		d.probeInterval = min(d.probeInterval*2, probeMaxInterval)
		d.nextProbe = now + d.probeInterval
		return
	}
	rate := math.Inf(1)
	if p.last > p.first {
		rate = float64(p.bytes) * 8e6 / float64(p.last-p.first)
	}
	result := math.Min(0.9*rate, p.target)
	if d.known() && result > d.estimate {
		d.estimate = result
	}
	if rate >= 0.9*p.target {
		d.probeInterval = probeFirstInterval
		d.nextProbe = now + probeQuiet
	} else {
		d.probeInterval = min(d.probeInterval*2, probeMaxInterval)
		d.nextProbe = now + d.probeInterval
	}
}

func (d *downlink) wantProbe(now int64, limit float64) (float64, int, bool) {
	if !d.known() || d.estimate >= limit || now < d.nextProbe {
		return 0, 0, false
	}
	if d.probe.id != 0 && !d.probe.done {
		d.finishProbe(now, false)
		return 0, 0, false
	}
	if now-d.lastOveruse < probeQuiet || now-d.lastDecrease < probeQuiet || d.lossRate() > 0.05 {
		d.nextProbe = now + probeQuiet/2
		return 0, 0, false
	}
	target := math.Min(2*d.estimate, 1.25*limit)
	packets := int(target * probeDuration / 8 / (probePadding + 40))
	packets = max(probeMinPackets, min(probeMaxPackets, packets))
	d.probes++
	d.probe = probeCluster{id: d.probes, target: target, sentAt: now, packets: packets}
	return target, packets, true
}
