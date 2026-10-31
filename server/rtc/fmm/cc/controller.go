package cc

import (
	"math"

	"forkmesh.com/mirror-node/rtc/fmm/internal/seqnum"
	"forkmesh.com/mirror-node/rtc/fmm/wire"
)

const (
	historySlots = 8192

	stateHold     = 0
	stateIncrease = 1
	stateDecrease = 2

	SignalNormal = 0
	SignalOver   = 1
	SignalUnder  = 2

	groupSpanUs      = 5_000
	trendWindow      = 20
	trendSmoothing   = 0.9
	trendGain        = 4.0
	overuseTimeMs    = 10.0
	thresholdInit    = 12.5
	thresholdMin     = 6.0
	thresholdMax     = 600.0
	thresholdUp      = 0.0087
	thresholdDown    = 0.039
	adaptOffsetMs    = 15.0
	beta             = 0.85
	betaMin          = 0.5
	drainBeta        = 0.5
	ackedWindowUs    = 500_000
	lossWindowUs     = 1_000_000
	lossWindowPkts   = 200
	lossLow          = 0.02
	lossHigh         = 0.10
	lossHighRandom   = 0.35
	queueEvidenceUs  = 40_000
	overuseMemoryUs  = 2_000_000
	minRTTWindowUs   = 10_000_000
	probeMinPackets  = 5
	minProbePacket   = 250
	probeDurationUs  = 15_000
	probeEvalDelayUs = 300_000
	probeInterval    = 5_000_000
	probeQuiet       = 3_000_000
	probeFollowUp    = 500_000
	probeMaxFactor   = 2.0
	fastCutQueueUs   = 150_000
	recentWindowUs   = 200_000
	silenceReports   = 3
	silenceStepUs    = 100_000
	silenceFactor    = 0.5
	drainedUs        = 25_000
	recoveryProbeUs  = 300_000
	appLimitedRoom   = 50_000
	lowProbePacket   = 80
	lowProbeUs       = 40_000
	probeMemoryUs    = 5_000_000
	probeFloor       = 0.85
	standingQueueUs  = 40_000
	standWindowUs    = 200_000
	standRiseUs      = 4_000
	requestGapUs     = 2_000_000
	requestQuietUs   = 1_000_000
)

type Config struct {
	StartBps int
	MinBps   int
	MaxBps   int
}

func (c Config) withDefaults() Config {
	if c.MinBps <= 0 {
		c.MinBps = 50_000
	}
	if c.MaxBps <= 0 {
		c.MaxBps = 3_000_000
	}
	if c.StartBps <= 0 {
		c.StartBps = 300_000
	}
	c.StartBps = min(max(c.StartBps, c.MinBps), c.MaxBps)
	return c
}

type sentPacket struct {
	seq      int64
	sendTime int64
	arrival  int64
	size     int
	probe    int
	state    uint8
}

const (
	pktPending  = 0
	pktReceived = 1
	pktLost     = 2
)

type interArrival struct {
	cur, prev struct {
		firstSend, lastSend, lastArrival int64
		valid                            bool
	}
}

func (ia *interArrival) add(send, arrival int64) (sendDelta, arrDelta int64, ok bool) {
	if !ia.cur.valid {
		ia.cur.firstSend, ia.cur.lastSend, ia.cur.lastArrival, ia.cur.valid = send, send, arrival, true
		return 0, 0, false
	}
	if send < ia.cur.firstSend {
		return 0, 0, false
	}
	if send-ia.cur.firstSend <= groupSpanUs {
		ia.cur.lastSend = max(ia.cur.lastSend, send)
		ia.cur.lastArrival = max(ia.cur.lastArrival, arrival)
		return 0, 0, false
	}
	if ia.prev.valid {
		sendDelta = ia.cur.lastSend - ia.prev.lastSend
		arrDelta = ia.cur.lastArrival - ia.prev.lastArrival
		ok = true
	}
	ia.prev = ia.cur
	ia.cur.firstSend, ia.cur.lastSend, ia.cur.lastArrival = send, send, arrival
	return sendDelta, arrDelta, ok
}

type trendline struct {
	acc, smoothed float64
	first         int64
	haveFirst     bool
	xs, ys        [trendWindow]float64
	n, head       int
	count         int
	trend         float64
}

func (t *trendline) update(sendDelta, arrDelta, arrival int64) float64 {
	if !t.haveFirst {
		t.first, t.haveFirst = arrival, true
	}
	d := float64(arrDelta-sendDelta) / 1000
	t.count++
	t.acc += d
	t.smoothed = trendSmoothing*t.smoothed + (1-trendSmoothing)*t.acc
	t.xs[t.head] = float64(arrival-t.first) / 1000
	t.ys[t.head] = t.smoothed
	t.head = (t.head + 1) % trendWindow
	if t.n < trendWindow {
		t.n++
	}
	if t.n == trendWindow {
		var sx, sy float64
		for i := range trendWindow {
			sx += t.xs[i]
			sy += t.ys[i]
		}
		mx, my := sx/trendWindow, sy/trendWindow
		var num, den float64
		for i := range trendWindow {
			num += (t.xs[i] - mx) * (t.ys[i] - my)
			den += (t.xs[i] - mx) * (t.xs[i] - mx)
		}
		if den != 0 {
			t.trend = num / den
		}
	}
	return float64(min(t.count, 60)) * t.trend * trendGain
}

type detector struct {
	threshold  float64
	lastUpdate int64
	overTime   float64
	overCount  int
	prevTrend  float64
	signal     int
}

func (d *detector) detect(trend, sendDeltaMs float64, count int, now int64) int {
	if count < 2 {
		return SignalNormal
	}
	switch {
	case trend > d.threshold:
		if d.overTime < 0 {
			d.overTime = sendDeltaMs / 2
		} else {
			d.overTime += sendDeltaMs
		}
		d.overCount++
		if d.overTime > overuseTimeMs && d.overCount > 1 && trend >= d.prevTrend {
			d.overTime, d.overCount = 0, 0
			d.signal = SignalOver
		}
	case trend < -d.threshold:
		d.overTime, d.overCount = -1, 0
		d.signal = SignalUnder
	default:
		d.overTime, d.overCount = -1, 0
		d.signal = SignalNormal
	}
	d.prevTrend = trend
	a := math.Abs(trend)
	if d.lastUpdate == 0 {
		d.lastUpdate = now
	}
	if a <= d.threshold+adaptOffsetMs {
		k := thresholdDown
		if a > d.threshold {
			k = thresholdUp
		}
		dt := min(float64(now-d.lastUpdate)/1000, 100)
		d.threshold = min(max(d.threshold+k*(a-d.threshold)*dt, thresholdMin), thresholdMax)
	}
	d.lastUpdate = now
	return d.signal
}

type aimd struct {
	rate         float64
	state        int
	lastChange   int64
	lastDecrease int64
	capEst       float64
	capVar       float64
	haveCap      bool
}

func (a *aimd) capUpper() float64 {
	dev := math.Sqrt(a.capVar*a.capEst/1000) * 1000
	return a.capEst + 3*dev
}

func (a *aimd) update(signal int, acked, floor float64, rtt, queueing int64, now int64, minBps, maxBps float64) {
	switch signal {
	case SignalOver:
		a.state = stateDecrease
	case SignalUnder:
		a.state = stateHold
	default:
		if a.state == stateHold {
			a.state = stateIncrease
		}
	}
	dt := float64(min(now-a.lastChange, 1_000_000)) / 1e6
	if a.lastChange == 0 {
		dt = 0
	}
	a.lastChange = now
	switch a.state {
	case stateIncrease:
		prev := a.rate
		if a.haveCap && acked > a.capUpper() {
			a.haveCap = false
		}
		if a.haveCap {
			response := float64(rtt+100_000) / 1e6
			a.rate += max(4000, 1200*8/response) * dt
		} else {
			a.rate += max(a.rate*(math.Pow(1.08, dt)-1), 1000*dt)
		}
		if lim := max(1.5*acked+10_000, acked+appLimitedRoom); acked > 0 && a.rate > lim {
			a.rate = max(prev, lim)
		}
	case stateDecrease:
		wait := min(max(rtt, 10_000), 200_000)
		if now-a.lastDecrease >= wait {
			base := max(acked, floor)
			if base <= 0 {
				base = a.rate
			}
			b := min(max(beta-drainBeta*float64(queueing)/1e6, betaMin), beta)
			nr := b * base
			if nr > a.rate && a.haveCap {
				nr = b * a.capEst
			}
			if nr < a.rate {
				a.rate = nr
			}
			if acked > 0 {
				x := acked / 1000
				if !a.haveCap {
					a.capEst, a.capVar, a.haveCap = x*1000, 0.4, true
				} else {
					e := a.capEst / 1000
					e = 0.95*e + 0.05*x
					v := 0.95*a.capVar + 0.05*(e-x)*(e-x)/max(e, 1)
					a.capEst, a.capVar = e*1000, min(max(v, 0.4), 2.5)
				}
			}
			a.lastDecrease = now
		}
		a.state = stateHold
	}
	a.rate = min(max(a.rate, minBps), maxBps)
}

type probe struct {
	id                  int
	rate                int
	bytes               int
	sentBytes, sentPkts int
	firstSend, lastSend int64
	lastSize            int
	recvBytes, recvPkts int
	firstArr, lastArr   int64
	firstArrSize        int
	ended               bool
	endAt               int64
}

type arrivalSample struct {
	at   int64
	size int
}

type Controller struct {
	cfg  Config
	hist []sentPacket

	haveSent  bool
	lastSeq   int64
	finalized int64
	haveRef   bool
	refTicks  int64

	ia     interArrival
	tl     trendline
	det    detector
	rate   aimd
	signal int

	acked       []arrivalSample
	ackedSum    int
	ackedNewest int64
	ackedRate   float64

	lossRecv, lossLost int
	lossRuns           int
	lossPrevLost       bool
	lossStart          int64
	loss, burst        float64
	lossBound          float64
	lossGrow           bool
	lastLossCut        int64
	lastLossUpdate     int64
	lastOveruse        int64

	qWinStart     int64
	qWinMin       int64
	qMins         [3]int64
	qMin          int64
	lastStand     int64
	StandCuts     int
	owdBuckets    [3]int64
	owdAt         int64
	owdHave       [3]bool
	queueDelay    int64
	lastFastCut   int64
	lastSent      int64
	lastSilence   int64
	preSilence    float64
	probeEst      float64
	probeAt       int64
	silent        bool
	silentAt      int64
	cutSeq        int64
	recovering    bool
	drainedAt     int64
	FastCuts      int
	SilenceCuts   int
	SilenceUndone int

	srtt       int64
	minRTT     int64
	minRTTAt   int64
	rttBuckets [3]int64
	lastReport int64

	probes      []*probe
	nextProbeID int
	probeQueue  []int
	nextProbeAt int64
	lastProbeAt int64

	target int

	Reports    int
	Overuses   int
	LostTotal  int
	RecvTotal  int
	ProbesSent int
	ProbesWon  int
}

func NewController(cfg Config) *Controller {
	cfg = cfg.withDefaults()
	c := &Controller{
		cfg:       cfg,
		hist:      make([]sentPacket, historySlots),
		lossBound: float64(cfg.MaxBps),
		target:    cfg.StartBps,
	}
	c.det.threshold = thresholdInit
	c.det.overTime = -1
	c.rate.rate = float64(cfg.StartBps)
	c.probeQueue = []int{3 * cfg.StartBps, 6 * cfg.StartBps}
	return c
}

func (c *Controller) Target() int        { return c.target }
func (c *Controller) RTT() int64         { return c.srtt }
func (c *Controller) Loss() float64      { return c.loss }
func (c *Controller) Burst() float64     { return c.burst }
func (c *Controller) Signal() int        { return c.signal }
func (c *Controller) AckedRate() int     { return int(c.ackedRate) }
func (c *Controller) Threshold() float64 { return c.det.threshold }

func (c *Controller) OnSent(tseq uint16, size int, now int64, probeID int) {
	s := int64(tseq)
	if c.haveSent {
		s = seqnum.Near16(tseq, c.lastSeq)
	} else {
		c.finalized = s
		c.lastSeq = s
		c.haveSent = true
	}
	c.lastSeq = max(c.lastSeq, s)
	c.lastSent = now
	c.hist[s%historySlots] = sentPacket{seq: s, sendTime: now, size: size, probe: probeID}
	if probeID > 0 {
		if p := c.probeByID(probeID); p != nil {
			if p.sentPkts == 0 {
				p.firstSend = now
			}
			p.sentPkts++
			p.sentBytes += size
			p.lastSend = now
			p.lastSize = size
		}
	}
}

func (c *Controller) probeByID(id int) *probe {
	for _, p := range c.probes {
		if p.id == id {
			return p
		}
	}
	return nil
}

func (c *Controller) OnReport(r *wire.Report, now int64) {
	if !c.haveSent {
		return
	}
	c.Reports++
	base := seqnum.Near16(r.Base, c.lastSeq)
	ref := int64(r.RefTime)
	if c.haveRef {
		ref = seqnum.Near32(r.RefTime, c.refTicks)
	}
	c.haveRef, c.refTicks = true, ref
	newest := int64(-1)
	newestOwd, haveOwd, newestSend := int64(0), false, int64(0)
	for i, ok := range r.Received {
		s := base + int64(i)
		if s > c.lastSeq {
			break
		}
		h := &c.hist[s%historySlots]
		if h.seq != s || !ok {
			continue
		}
		newest = s
		if h.state != pktPending {
			continue
		}
		h.state = pktReceived
		h.arrival = (ref + r.Arrival[i]) * TickUs
		c.onArrival(h, now)
		newestOwd, haveOwd, newestSend = h.arrival-h.sendTime, true, h.sendTime
		c.trackOwd(newestOwd, now)
	}
	if base-c.finalized > historySlots {
		c.finalized = base - historySlots
	}
	for ; c.finalized < base && c.finalized <= c.lastSeq; c.finalized++ {
		h := &c.hist[c.finalized%historySlots]
		if h.seq != c.finalized {
			continue
		}
		if h.state == pktPending {
			h.state = pktLost
		}
		if h.seq > c.cutSeq {
			c.countOutcome(h.state == pktLost)
		}
	}
	if newest >= 0 {
		h := &c.hist[newest%historySlots]
		rtt := now - h.sendTime - int64(r.Delay)*TickUs
		if rtt > 0 {
			if c.srtt == 0 {
				c.srtt = rtt
			} else {
				c.srtt = (7*c.srtt + rtt) / 8
			}
			c.trackMinRTT(rtt, now)
		}
	}
	if haveOwd {
		c.queueDelay = max(newestOwd-c.minOwd(), 0)
		if c.silent && newestSend >= c.silentAt-ReportInterval {
			c.silent = false
			if c.queueDelay < queueEvidenceUs {
				c.rate.rate = max(c.rate.rate, c.preSilence)
				c.SilenceUndone++
			}
		}
		c.trackQueue(now)
		c.fastCut(now)
		c.standingCut(now)
		if c.queueDelay > drainedUs {
			c.drainedAt = 0
		} else if c.drainedAt == 0 {
			c.drainedAt = now
		}
	}
	c.updateLoss(now)
	queueing := int64(0)
	if c.srtt > 0 && c.minRTT > 0 {
		queueing = max(c.srtt-c.minRTT, 0)
	}
	floor := 0.0
	if c.queueDelay < fastCutQueueUs {
		floor = probeFloor * float64(c.ProbeEstimate(now))
	}
	c.rate.update(c.signal, c.ackedRate, floor, c.srtt, queueing, now, float64(c.cfg.MinBps), float64(c.cfg.MaxBps))
	if c.signal == SignalOver {
		c.lastOveruse = now
		c.Overuses++
	}
	c.evaluateProbes(now)
	c.updateTarget(now)
	c.lastReport = now
}

func (c *Controller) onArrival(h *sentPacket, now int64) {
	c.acked = append(c.acked, arrivalSample{at: h.arrival, size: h.size})
	c.ackedSum += h.size
	c.ackedNewest = max(c.ackedNewest, h.arrival)
	newest := c.ackedNewest
	drop := 0
	for drop < len(c.acked) && c.acked[drop].at < newest-ackedWindowUs {
		c.ackedSum -= c.acked[drop].size
		drop++
	}
	if drop > 0 {
		c.acked = append(c.acked[:0], c.acked[drop:]...)
	}
	if len(c.acked) >= 2 {
		span := newest - c.acked[0].at
		if span >= 100_000 {
			c.ackedRate = float64(c.ackedSum) * 8e6 / float64(max(span, ackedWindowUs/2))
		}
	}
	if h.probe > 0 {
		if p := c.probeByID(h.probe); p != nil {
			if p.recvPkts == 0 || h.arrival < p.firstArr {
				p.firstArr, p.firstArrSize = h.arrival, h.size
			}
			if h.arrival > p.lastArr || p.recvPkts == 0 {
				p.lastArr = h.arrival
			}
			p.recvPkts++
			p.recvBytes += h.size
		}
	}
	sd, ad, ok := c.ia.add(h.sendTime, h.arrival)
	if !ok {
		return
	}
	trend := c.tl.update(sd, ad, h.arrival)
	c.signal = c.det.detect(trend, float64(sd)/1000, c.tl.count, now)
}

func (c *Controller) trackOwd(d, now int64) {
	if c.owdAt == 0 || now-c.owdAt >= minRTTWindowUs {
		c.owdBuckets[1], c.owdBuckets[0] = c.owdBuckets[0], c.owdBuckets[2]
		c.owdHave[1], c.owdHave[0] = c.owdHave[0], c.owdHave[2]
		c.owdBuckets[2], c.owdHave[2] = d, true
		c.owdAt = now
	}
	c.owdBuckets[2] = min(c.owdBuckets[2], d)
}

func (c *Controller) minOwd() int64 {
	m := c.owdBuckets[2]
	for i := range 2 {
		if c.owdHave[i] {
			m = min(m, c.owdBuckets[i])
		}
	}
	return m
}

func (c *Controller) QueueDelay() int64 { return c.queueDelay }

func (c *Controller) trackQueue(now int64) {
	if c.qWinStart == 0 {
		c.qWinStart, c.qWinMin = now, c.queueDelay
		return
	}
	c.qWinMin = min(c.qWinMin, c.queueDelay)
	if now-c.qWinStart < standWindowUs {
		return
	}
	c.qMins[0], c.qMins[1], c.qMins[2] = c.qMins[1], c.qMins[2], c.qWinMin
	c.qWinStart, c.qWinMin = now, c.queueDelay
	c.qMin = 0
	if c.qMins[2] >= standingQueueUs && c.qMins[2] >= c.qMins[1]+standRiseUs && c.qMins[1] >= c.qMins[0]+standRiseUs {
		c.qMin = c.qMins[2]
	}
}

func (c *Controller) standingCut(now int64) {
	if c.qMin < standingQueueUs || now-c.lastFastCut < max(c.srtt, standWindowUs) || now-c.lastStand < max(c.srtt, standWindowUs) {
		return
	}
	base := c.recentRate()
	if base <= 0 {
		base = c.ackedRate
	}
	b := min(max(beta-drainBeta*float64(c.qMin)/1e6, betaMin), beta)
	if nr := b * base; nr < c.rate.rate {
		c.rate.rate = max(nr, float64(c.cfg.MinBps))
		c.rate.state = stateHold
		c.rate.lastDecrease = now
	}
	c.lastStand, c.lastOveruse = now, now
	c.StandCuts++
}

func (c *Controller) recentRate() float64 {
	if len(c.acked) < 2 {
		return 0
	}
	newest := c.ackedNewest
	sum, oldest := 0, newest
	for i := len(c.acked) - 1; i >= 0 && c.acked[i].at >= newest-recentWindowUs; i-- {
		sum += c.acked[i].size
		oldest = c.acked[i].at
	}
	span := newest - oldest
	if span < recentWindowUs/2 {
		return 0
	}
	return float64(sum) * 8e6 / float64(span)
}

func (c *Controller) fastCut(now int64) {
	if c.queueDelay < fastCutQueueUs || now-c.lastFastCut < max(c.srtt, 100_000) {
		return
	}
	rr := c.recentRate()
	if rr <= 0 {
		return
	}
	b := min(max(beta-drainBeta*float64(c.queueDelay)/1e6, betaMin), beta)
	nr := b * rr
	if nr >= c.rate.rate {
		return
	}
	c.rate.rate = max(nr, float64(c.cfg.MinBps))
	c.rate.state = stateHold
	c.rate.lastDecrease = now
	c.lastFastCut, c.lastOveruse = now, now
	c.FastCuts++
	c.forgetLoss(now)
}

func (c *Controller) forgetLoss(now int64) {
	c.cutSeq = c.lastSeq
	c.lossRecv, c.lossLost, c.lossRuns, c.lossStart = 0, 0, 0, now
	c.recovering = true
	c.drainedAt = 0
}

func (c *Controller) CheckFeedback(now int64) {
	if c.lastReport == 0 || c.lastSent <= c.lastReport {
		return
	}
	limit := max(silenceReports*ReportInterval, 2*c.minRTT)
	if now-c.lastReport < limit || now-c.lastSilence < silenceStepUs {
		return
	}
	if !c.silent {
		c.silent, c.silentAt = true, now
		c.preSilence = c.rate.rate
		c.forgetLoss(now)
	}
	c.lastSilence, c.lastOveruse = now, now
	c.rate.rate = max(c.rate.rate*silenceFactor, float64(c.cfg.MinBps))
	c.rate.state = stateHold
	c.SilenceCuts++
	c.updateTarget(now)
}

func (c *Controller) trackMinRTT(rtt, now int64) {
	if c.minRTTAt == 0 || now-c.minRTTAt >= minRTTWindowUs {
		c.rttBuckets[1], c.rttBuckets[0] = c.rttBuckets[0], c.rttBuckets[2]
		c.rttBuckets[2] = rtt
		c.minRTTAt = now
	}
	c.rttBuckets[2] = min(c.rttBuckets[2], rtt)
	c.minRTT = c.rttBuckets[2]
	for _, b := range c.rttBuckets[:2] {
		if b > 0 {
			c.minRTT = min(c.minRTT, b)
		}
	}
}

func (c *Controller) countOutcome(lost bool) {
	if lost {
		c.lossLost++
		c.LostTotal++
		if !c.lossPrevLost {
			c.lossRuns++
		}
	} else {
		c.lossRecv++
		c.RecvTotal++
	}
	c.lossPrevLost = lost
}

func (c *Controller) Congested(now int64) bool { return c.queueEvidence(now) }

func (c *Controller) queueEvidence(now int64) bool {
	if c.lastOveruse > 0 && now-c.lastOveruse < overuseMemoryUs {
		return true
	}
	return c.srtt > 0 && c.minRTT > 0 && c.srtt-c.minRTT > queueEvidenceUs
}

func (c *Controller) updateLoss(now int64) {
	if c.lossStart == 0 {
		c.lossStart = now
	}
	total := c.lossRecv + c.lossLost
	if total < lossWindowPkts && (now-c.lossStart < lossWindowUs || total < 20) {
		if c.lossGrow && c.lastLossUpdate > 0 {
			dt := float64(now-c.lastReport) / 1e6
			c.lossBound = min(c.lossBound*math.Pow(1.08, dt), float64(c.cfg.MaxBps))
		}
		return
	}
	frac := float64(c.lossLost) / float64(total)
	c.loss = frac
	c.burst = 1
	if c.lossRuns > 0 {
		c.burst = float64(c.lossLost) / float64(c.lossRuns)
	}
	c.lossRecv, c.lossLost, c.lossRuns, c.lossStart = 0, 0, 0, now
	c.lastLossUpdate = now
	high := lossHigh
	if !c.queueEvidence(now) {
		high = lossHighRandom
	}
	c.lossGrow = frac < lossLow || frac <= high && high == lossHighRandom
	switch {
	case frac > high:
		if now-c.lastLossCut >= 300_000+c.srtt {
			c.lossBound = min(c.lossBound, float64(c.target)) * (1 - 0.5*frac)
			c.lastLossCut = now
		}
	case frac < lossLow:
		c.lossBound = max(c.lossBound, 1.08*float64(c.target))
	}
	c.lossBound = min(max(c.lossBound, float64(c.cfg.MinBps)), float64(c.cfg.MaxBps))
}

func (c *Controller) updateTarget(now int64) {
	t := min(c.rate.rate, c.lossBound)
	c.target = int(min(max(t, float64(c.cfg.MinBps)), float64(c.cfg.MaxBps)))
}

func (c *Controller) ProbeEstimate(now int64) int {
	if c.probeAt == 0 || now-c.probeAt > probeMemoryUs || c.lastFastCut > c.probeAt || c.lastSilence > c.probeAt {
		return 0
	}
	return int(min(c.probeEst, float64(c.cfg.MaxBps)))
}

func (c *Controller) RequestProbe(now int64) {
	if c.target >= c.cfg.MaxBps || now-c.lastProbeAt < requestGapUs || now-c.lastOveruse < requestQuietUs || c.queueEvidence(now) {
		return
	}
	if c.nextProbeAt == 0 || c.nextProbeAt > now {
		c.nextProbeAt = now
	}
}

func ProbePacket(rate int) int {
	return min(max(rate*lowProbeUs/8_000_000/probeMinPackets, lowProbePacket), minProbePacket)
}

func (c *Controller) StartProbe(now int64) (id, rate, bytes int, ok bool) {
	for _, p := range c.probes {
		if !p.ended {
			return 0, 0, 0, false
		}
	}
	var r int
	switch {
	case c.recovering && c.drainedAt > 0 && now-c.drainedAt >= recoveryProbeUs:
		r = int(probeMaxFactor * float64(c.target))
		c.recovering = false
	case len(c.probeQueue) > 0:
		r = c.probeQueue[0]
		c.probeQueue = c.probeQueue[1:]
	case c.nextProbeAt > 0 && now >= c.nextProbeAt:
		r = int(probeMaxFactor * float64(c.target))
		c.nextProbeAt = 0
	case c.lastProbeAt > 0 && now-c.lastProbeAt >= probeInterval && now-c.lastOveruse >= probeQuiet && (c.loss < lossLow || c.lossGrow && !c.queueEvidence(now)):
		r = int(probeMaxFactor * float64(c.target))
	default:
		return 0, 0, 0, false
	}
	if c.target >= c.cfg.MaxBps {
		c.lastProbeAt = now
		return 0, 0, 0, false
	}
	r = min(r, int(probeMaxFactor*float64(c.cfg.MaxBps)))
	c.nextProbeID++
	p := &probe{id: c.nextProbeID, rate: r, bytes: max(r*probeDurationUs/8_000_000, probeMinPackets*ProbePacket(r))}
	c.probes = append(c.probes, p)
	c.lastProbeAt = now
	c.ProbesSent++
	return p.id, p.rate, p.bytes, true
}

func (c *Controller) EndProbe(id int, now int64) {
	if p := c.probeByID(id); p != nil && !p.ended {
		p.ended = true
		p.endAt = now
	}
}

func (c *Controller) evaluateProbes(now int64) {
	keep := c.probes[:0]
	for _, p := range c.probes {
		if !p.ended {
			keep = append(keep, p)
			continue
		}
		wait := max(probeEvalDelayUs, 2*c.srtt)
		if p.recvPkts < p.sentPkts && now-p.endAt < wait {
			keep = append(keep, p)
			continue
		}
		c.finishProbe(p, now)
	}
	c.probes = keep
}

func (c *Controller) finishProbe(p *probe, now int64) {
	if p.sentPkts < probeMinPackets || float64(p.recvPkts) < 0.8*float64(p.sentPkts) || p.lastArr <= p.firstArr {
		return
	}
	recv := float64(p.recvBytes-p.firstArrSize) * 8e6 / float64(p.lastArr-p.firstArr)
	send := float64(p.rate)
	if p.lastSend > p.firstSend {
		send = float64(p.sentBytes-p.lastSize) * 8e6 / float64(p.lastSend-p.firstSend)
	}
	est := min(recv, send)
	if recv < 0.9*send {
		est = 0.95 * recv
	}
	if est > float64(c.target) {
		c.probeEst, c.probeAt = est, now
		c.ProbesWon++
		est = min(est, float64(c.cfg.MaxBps))
		if est > c.rate.rate {
			c.rate.rate = est
			c.rate.haveCap = false
		}
		c.lossBound = max(c.lossBound, est)
		c.updateTarget(now)
		if est >= 0.7*float64(p.rate) && c.target < c.cfg.MaxBps {
			c.nextProbeAt = now + probeFollowUp
		}
	}
}
