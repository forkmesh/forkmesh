package sfu

import (
	"time"

	"forkmesh.com/mirror-node/rtc/rtp"
)

const (
	seqOffset       = 1 << 16
	cacheSlots      = 1024
	cacheBytes      = 256 << 10
	cacheAge        = time.Second
	missingLimit    = 256
	missingAge      = time.Second
	nackTries       = 6
	nackSpacing     = 20 * time.Millisecond
	nackMinRetry    = 40 * time.Millisecond
	nackMaxRetry    = 300 * time.Millisecond
	nackPerPacket   = 128
	nackFloor       = 20
	nackShare       = 4
	defaultRTT      = 100 * time.Millisecond
	videoClock      = 90000
	audioClock      = 48000
	reportInterval  = 500 * time.Millisecond
	arrivalSlots    = 4096
	feedbackMaxRuns = 300
)

func unwrapSeq(last int64, started bool, seq uint16) int64 {
	if !started {
		return int64(seq) + seqOffset
	}
	return last + int64(int16(seq-uint16(last)))
}

type cached struct {
	ext    int64
	at     time.Time
	packet []byte
}

type packetCache struct {
	slots   [cacheSlots]cached
	highest int64
	low     int64
	bytes   int
}

func (c *packetCache) put(ext int64, at time.Time, packet []byte) {
	if c.highest == 0 {
		c.highest, c.low = ext, ext
	}
	if ext > c.highest {
		c.highest = ext
	}
	if ext <= c.highest-cacheSlots {
		return
	}
	slot := &c.slots[ext&(cacheSlots-1)]
	if slot.packet != nil {
		c.bytes -= len(slot.packet)
	}
	*slot = cached{ext: ext, at: at, packet: packet}
	c.bytes += len(packet)
	if ext < c.low {
		c.low = ext
	}
	if floor := c.highest - cacheSlots + 1; c.low < floor {
		c.low = floor
	}
	for c.bytes > cacheBytes && c.low <= c.highest {
		old := &c.slots[c.low&(cacheSlots-1)]
		if old.packet != nil && old.ext == c.low {
			c.bytes -= len(old.packet)
			*old = cached{}
		}
		c.low++
	}
}

func (c *packetCache) ext(seq uint16) int64 {
	return unwrapSeq(c.highest, c.highest != 0, seq)
}

func (c *packetCache) has(ext int64) bool {
	if c.highest == 0 {
		return false
	}
	slot := &c.slots[ext&(cacheSlots-1)]
	return slot.packet != nil && slot.ext == ext
}

func (c *packetCache) get(seq uint16, now time.Time) (int64, []byte) {
	if c.highest == 0 {
		return 0, nil
	}
	ext := c.ext(seq)
	slot := &c.slots[ext&(cacheSlots-1)]
	if slot.packet == nil || slot.ext != ext || now.Sub(slot.at) > cacheAge {
		return 0, nil
	}
	return ext, slot.packet
}

type missingPacket struct {
	seen  time.Time
	sent  time.Time
	tries int
}

type receiveStats struct {
	clock         int64
	started       bool
	base          int64
	highest       int64
	received      int64
	expectedPrior int64
	receivedPrior int64
	jitter        float64
	lastTransit   int64
	haveTransit   bool
	srNTP         uint64
	srAt          time.Time
	missing       map[int64]*missingPacket
	lastNack      time.Time
	rtt           time.Duration
	window        time.Time
	windowGot     int
	priorGot      int
	windowNacked  int
}

func newReceiveStats(kind string) *receiveStats {
	s := &receiveStats{clock: audioClock, rtt: defaultRTT}
	if kind == "video" {
		s.clock = videoClock
		s.missing = map[int64]*missingPacket{}
	}
	return s
}

func (s *receiveStats) ext(seq uint16) int64 {
	return unwrapSeq(s.highest, s.started, seq)
}

func (s *receiveStats) arrived(ext int64, timestamp uint32, now time.Time, epoch time.Time, recovered bool) bool {
	gapTooBig := false
	if !s.started {
		s.started = true
		s.base, s.highest = ext, ext
	} else if ext > s.highest {
		if s.missing != nil {
			gap := ext - s.highest - 1
			if gap > missingLimit {
				clear(s.missing)
				gapTooBig = true
			} else {
				if int64(len(s.missing))+gap > missingLimit {
					clear(s.missing)
				}
				for k := s.highest + 1; k < ext; k++ {
					s.missing[k] = &missingPacket{seen: now}
				}
			}
		}
		s.highest = ext
	} else if ext < s.base {
		s.base = ext
	}
	if s.missing != nil {
		if m, ok := s.missing[ext]; ok {
			if recovered && m.tries == 1 {
				sample := now.Sub(m.sent)
				s.rtt = (7*s.rtt + sample) / 8
			}
			delete(s.missing, ext)
		}
	}
	if recovered {
		return gapTooBig
	}
	s.roll(now)
	s.windowGot++
	s.received++
	arrival := now.Sub(epoch).Microseconds() * s.clock / 1_000_000
	transit := arrival - int64(timestamp)
	if s.haveTransit {
		d := transit - s.lastTransit
		if d < 0 {
			d = -d
		}
		if d < s.clock*10 {
			s.jitter += (float64(d) - s.jitter) / 16
		}
	}
	s.lastTransit, s.haveTransit = transit, true
	return gapTooBig
}

func (s *receiveStats) roll(now time.Time) {
	if now.Sub(s.window) >= time.Second {
		if now.Sub(s.window) >= 2*time.Second {
			s.priorGot = 0
		} else {
			s.priorGot = s.windowGot
		}
		s.window, s.windowGot, s.windowNacked = now, 0, 0
	}
}

func (s *receiveStats) nacks(now time.Time) []uint16 {
	if s.missing == nil || len(s.missing) == 0 || now.Sub(s.lastNack) < nackSpacing {
		return nil
	}
	s.roll(now)
	allowed := max(nackFloor, max(s.priorGot, s.windowGot)/nackShare) - s.windowNacked
	if allowed <= 0 {
		return nil
	}
	retry := s.rtt * 5 / 4
	if retry < nackMinRetry {
		retry = nackMinRetry
	}
	if retry > nackMaxRetry {
		retry = nackMaxRetry
	}
	var seqs []uint16
	for ext, m := range s.missing {
		if now.Sub(m.seen) > missingAge || m.tries >= nackTries {
			delete(s.missing, ext)
			continue
		}
		if m.tries > 0 && now.Sub(m.sent) < retry {
			continue
		}
		if len(seqs) >= min(nackPerPacket, allowed) {
			continue
		}
		m.tries++
		m.sent = now
		seqs = append(seqs, uint16(ext))
	}
	if len(seqs) > 0 {
		s.lastNack = now
		s.windowNacked += len(seqs)
	}
	return seqs
}

func (s *receiveStats) report(ssrc uint32, now time.Time) (rtp.ReportBlock, bool) {
	if !s.started {
		return rtp.ReportBlock{}, false
	}
	expected := s.highest - s.base + 1
	lost := expected - s.received
	expectedInterval := expected - s.expectedPrior
	receivedInterval := s.received - s.receivedPrior
	s.expectedPrior, s.receivedPrior = expected, s.received
	lostInterval := expectedInterval - receivedInterval
	var fraction uint8
	if expectedInterval > 0 && lostInterval > 0 {
		fraction = uint8(lostInterval << 8 / expectedInterval)
	}
	if lost > 0x7fffff {
		lost = 0x7fffff
	}
	block := rtp.ReportBlock{
		SSRC:         ssrc,
		FractionLost: fraction,
		Lost:         int32(lost),
		HighestSeq:   uint32(s.highest - seqOffset),
		Jitter:       uint32(s.jitter),
	}
	if s.srNTP != 0 {
		block.LSR = uint32(s.srNTP >> 16)
		block.DLSR = uint32(now.Sub(s.srAt) * 65536 / time.Second)
	}
	return block, true
}

type arrivals struct {
	started bool
	next    int64
	highest int64
	times   [arrivalSlots]int64
	seqs    [arrivalSlots]int64
	count   uint8
	pending bool
}

func (a *arrivals) record(seq uint16, at int64) {
	ext := unwrapSeq(a.highest, a.started, seq)
	if !a.started {
		a.started = true
		a.next, a.highest = ext, ext
	}
	if ext < a.next || ext <= a.highest-arrivalSlots {
		return
	}
	if ext > a.highest {
		a.highest = ext
	}
	if a.highest-a.next >= arrivalSlots {
		a.next = a.highest - arrivalSlots + 1
	}
	slot := ext & (arrivalSlots - 1)
	a.seqs[slot] = ext
	a.times[slot] = at
	a.pending = true
}

func (a *arrivals) arrival(ext int64) (int64, bool) {
	slot := ext & (arrivalSlots - 1)
	if a.seqs[slot] != ext {
		return 0, false
	}
	return a.times[slot], true
}

func (a *arrivals) feedback(sender, media uint32) [][]byte {
	if !a.pending {
		return nil
	}
	a.pending = false
	var out [][]byte
	for a.next <= a.highest {
		start := a.next
		for start <= a.highest {
			if _, ok := a.arrival(start); ok {
				break
			}
			start++
		}
		if start > a.highest {
			a.next = start
			break
		}
		first, _ := a.arrival(start)
		reference := first / rtp.TCCReferenceMicros
		clock := reference * rtp.TCCReferenceMicros
		f := rtp.TransportFeedback{
			SenderSSRC:    sender,
			MediaSSRC:     media,
			BaseSequence:  uint16(a.next),
			ReferenceTime: uint32(reference) & 0xffffff,
			FeedbackCount: a.count,
		}
		ext := a.next
		lastReceived := ext
		for ; ext <= a.highest && len(f.Statuses) < feedbackMaxRuns; ext++ {
			at, ok := a.arrival(ext)
			if !ok {
				f.Statuses = append(f.Statuses, rtp.FeedbackStatus{})
				continue
			}
			delta := (at - clock) / rtp.TCCTickMicros
			if delta < -32768 || delta > 32767 {
				break
			}
			f.Statuses = append(f.Statuses, rtp.FeedbackStatus{Received: true, Delta: int32(delta)})
			clock += delta * rtp.TCCTickMicros
			lastReceived = ext
		}
		f.Statuses = f.Statuses[:lastReceived-a.next+1]
		a.next = lastReceived + 1
		packet, err := f.Marshal()
		if err != nil {
			break
		}
		a.count++
		out = append(out, packet)
	}
	return out
}
