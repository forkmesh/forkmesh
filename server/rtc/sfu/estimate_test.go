package sfu

import (
	"math"
	"math/rand"
	"testing"
	"time"

	"forkmesh.com/mirror-node/rtc/rtp"
)

type linkStep struct {
	at       int64
	capacity float64
}

type linkSim struct {
	steps    []linkStep
	loss     float64
	jitter   int64
	base     int64
	offset   int64
	queue    int64
	rng      *rand.Rand
	free     int64
	lastRecv int64
}

func (l *linkSim) capacity(now int64) float64 {
	c := l.steps[0].capacity
	for _, s := range l.steps {
		if now >= s.at {
			c = s.capacity
		}
	}
	return c
}

func (l *linkSim) deliver(now int64, size int) (int64, bool) {
	if l.rng.Float64() < l.loss {
		return 0, false
	}
	start := max(now, l.free)
	if start-now > l.queue {
		return 0, false
	}
	l.free = start + int64(float64(size*8)*1e6/l.capacity(now))
	recv := l.free + l.base
	if l.jitter > 0 {
		recv += l.rng.Int63n(2*l.jitter) - l.jitter
	}
	recv = max(recv, l.lastRecv)
	l.lastRecv = recv
	return recv + l.offset, true
}

type pendingArrival struct {
	seq uint16
	at  int64
}

type simResult struct {
	estimates []float64
	queue     []int64
}

func simulate(t *testing.T, link *linkSim, seconds int, demand float64) simResult {
	d := newDownlink()
	d.reset(0)
	var recorder arrivals
	var inFlight []pendingArrival
	var result simResult
	const packet = 1200
	var credit float64
	for now := int64(1000); now <= int64(seconds)*1_000_000; now += 1000 {
		rate := demand
		if d.known() {
			rate = math.Min(demand, d.estimate)
		}
		credit += rate / 8 / 1000
		for credit >= packet {
			credit -= packet
			seq, ext := d.stamp()
			d.sent(ext, now, packet, 0)
			if at, ok := link.deliver(now, packet+packetOverhead); ok {
				inFlight = append(inFlight, pendingArrival{seq, at})
			}
		}
		if now%50_000 == 0 && d.known() {
			if _, packets, ok := d.wantProbe(now, 4*demand); ok {
				for i := 0; i < packets; i++ {
					seq, ext := d.stamp()
					d.sent(ext, now, probePadding+40, d.probe.id)
					if at, ok := link.deliver(now, probePadding+40+packetOverhead); ok {
						inFlight = append(inFlight, pendingArrival{seq, at})
					}
				}
			}
		}
		if now%100_000 == 0 {
			kept := inFlight[:0]
			for _, a := range inFlight {
				if a.at-link.offset <= now {
					recorder.record(a.seq, a.at)
				} else {
					kept = append(kept, a)
				}
			}
			inFlight = kept
			for _, raw := range recorder.feedback(1, 2) {
				f, err := rtp.ParseTransportFeedback(raw)
				if err != nil {
					t.Fatal(err)
				}
				d.feedback(f, now)
			}
			if d.known() && d.estimate >= 3*demand {
				d.estimate = 0
			}
			result.estimates = append(result.estimates, d.estimate)
			result.queue = append(result.queue, max(0, link.free-now))
		}
	}
	return result
}

func window(values []float64, from, to int) (lo, hi, mean float64) {
	lo, hi = math.Inf(1), math.Inf(-1)
	for _, v := range values[from:to] {
		lo, hi = math.Min(lo, v), math.Max(hi, v)
		mean += v
	}
	return lo, hi, mean / float64(to-from)
}

func TestTheEstimateSettlesUnderACappedLink(t *testing.T) {
	for _, capacity := range []float64{300_000, 1_000_000} {
		link := &linkSim{steps: []linkStep{{0, capacity}}, base: 20_000, offset: 1073 * 1_000_000, queue: 250_000, rng: rand.New(rand.NewSource(1))}
		r := simulate(t, link, 30, 3_000_000)
		lo, hi, mean := window(r.estimates, 100, 300)
		if lo < 0.5*capacity || hi > 1.3*capacity || mean < 0.7*capacity || mean > 1.1*capacity || lo == 0 {
			t.Fatalf("capacity %v: estimate %v..%v mean %v", capacity, lo, hi, mean)
		}
		var queued int64
		for _, q := range r.queue[100:] {
			queued += q
		}
		if avg := queued / int64(len(r.queue)-100); avg > 120_000 {
			t.Fatalf("capacity %v: mean queue %d us", capacity, avg)
		}
	}
}

func TestRandomLossAndJitterAloneDoNotCutTheEstimate(t *testing.T) {
	cases := []*linkSim{
		{steps: []linkStep{{0, 100_000_000}}, loss: 0.1, base: 20_000, queue: 250_000},
		{steps: []linkStep{{0, 100_000_000}}, base: 100_000, jitter: 30_000, queue: 250_000},
	}
	for i, link := range cases {
		link.rng = rand.New(rand.NewSource(int64(i) + 2))
		r := simulate(t, link, 20, 3_000_000)
		cut := 0
		for _, e := range r.estimates[20:] {
			if e != 0 && e < 2_000_000 {
				cut++
			}
		}
		if cut > len(r.estimates)/10 {
			t.Fatalf("case %d: estimate below 2 Mbps in %d of %d samples", i, cut, len(r.estimates)-20)
		}
	}
}

func TestTheEstimateRecoversAfterTheLinkDoes(t *testing.T) {
	link := &linkSim{steps: []linkStep{{0, 2_000_000}, {10_000_000, 300_000}, {20_000_000, 2_000_000}}, base: 20_000, queue: 400_000, rng: rand.New(rand.NewSource(5))}
	r := simulate(t, link, 32, 3_000_000)
	if _, hi, _ := window(r.estimates, 150, 200); hi > 450_000 || hi == 0 {
		t.Fatalf("estimate during the drop reached %v", hi)
	}
	if _, _, mean := window(r.estimates, 280, 320); mean < 1_400_000 {
		t.Fatalf("estimate 8 s after recovery %v", mean)
	}
}

func TestPacketCacheBoundsAgeAndBytes(t *testing.T) {
	var c packetCache
	now := time.Now()
	for i := int64(0); i < 2000; i++ {
		c.put(seqOffset+i, now, make([]byte, 1000))
	}
	if c.bytes > cacheBytes {
		t.Fatalf("cache holds %d bytes", c.bytes)
	}
	if _, p := c.get(uint16(1999), now); p == nil {
		t.Fatal("newest packet missing")
	}
	if _, p := c.get(uint16(1000), now); p != nil {
		t.Fatal("packet beyond the byte bound kept")
	}
	if _, p := c.get(uint16(1999), now.Add(cacheAge+time.Millisecond)); p != nil {
		t.Fatal("expired packet returned")
	}
	c.put(seqOffset+1990, now, make([]byte, 10))
	if ext, p := c.get(uint16(1990), now); p == nil || len(p) != 10 || ext != seqOffset+1990 {
		t.Fatal("replaced packet wrong")
	}
	var wrap packetCache
	for i := int64(65530); i < 65545; i++ {
		wrap.put(seqOffset+i, now, []byte{byte(i)})
	}
	if ext, p := wrap.get(3, now); p == nil || ext != seqOffset+65539 {
		t.Fatalf("wrapped lookup %d %v", ext, p)
	}
	total := 0
	for i := range wrap.slots {
		total += len(wrap.slots[i].packet)
	}
	if total != wrap.bytes {
		t.Fatalf("byte count %d, slots hold %d", wrap.bytes, total)
	}
}

func TestArrivalFeedbackSplitsLongGapsAndReportsLateLoss(t *testing.T) {
	var a arrivals
	a.record(100, 1_000_000)
	a.record(101, 1_001_000)
	a.record(104, 1_002_000)
	a.record(105, 1_002_000+9_000_000)
	packets := a.feedback(7, 8)
	if len(packets) != 2 {
		t.Fatalf("%d feedback packets", len(packets))
	}
	first, err := rtp.ParseTransportFeedback(packets[0])
	if err != nil {
		t.Fatal(err)
	}
	if first.BaseSequence != 100 || len(first.Statuses) != 5 || first.Statuses[2].Received || !first.Statuses[4].Received || first.FeedbackCount != 0 {
		t.Fatalf("first %+v", first)
	}
	clock := int64(first.ReferenceTime) * rtp.TCCReferenceMicros
	var times []int64
	for _, s := range first.Statuses {
		if s.Received {
			clock += int64(s.Delta) * rtp.TCCTickMicros
			times = append(times, clock)
		}
	}
	if times[0] != 1_000_000 || times[1] != 1_001_000 || times[2] != 1_002_000 {
		t.Fatalf("arrival times %v", times)
	}
	second, _ := rtp.ParseTransportFeedback(packets[1])
	if second.BaseSequence != 105 || len(second.Statuses) != 1 || second.FeedbackCount != 1 {
		t.Fatalf("second %+v", second)
	}
	a.record(102, 2_000_000)
	if len(a.feedback(7, 8)) != 0 {
		t.Fatal("a packet already reported lost was reported again")
	}
	a.record(65535, 3_000_000)
	a.record(0, 3_000_100)
	if len(a.feedback(7, 8)) != 0 {
		t.Fatal("a sequence far behind was taken as new")
	}
}
