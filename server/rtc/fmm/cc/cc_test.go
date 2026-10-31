package cc_test

import (
	"fmt"
	"slices"
	"strings"
	"testing"

	"forkmesh.com/mirror-node/rtc/fmm/cc"
	"forkmesh.com/mirror-node/rtc/fmm/hop"
	"forkmesh.com/mirror-node/rtc/fmm/link"
	"forkmesh.com/mirror-node/rtc/fmm/wire"
)

type senderSide struct{ snd *hop.Sender }

func (s senderSide) HandleDatagram(p []byte, now int64) {}

func (s senderSide) HandleControl(p []byte, now int64) {
	m, _, err := wire.Decode(p)
	if err != nil {
		return
	}
	if r, ok := m.(*wire.Report); ok {
		s.snd.OnReport(r, now)
	}
}

type receiverSide struct {
	rcv   *hop.Receiver
	bytes int
}

func (r *receiverSide) HandleDatagram(p []byte, now int64) {
	for _, m := range r.rcv.Handle(p, now) {
		r.bytes += len(m)
	}
}

func (r *receiverSide) HandleControl(p []byte, now int64) {}

type sample struct {
	at       int64
	capacity int
	target   int
	goodput  int
	queue    int64
	rtt      int64
	loss     float64
}

type step struct {
	at     int64
	params link.Params
}

type ccRun struct {
	samples []sample
	link    *link.Link
	snd     *hop.Sender
}

func base(rate int) link.Params {
	return link.Params{DelayUs: 25_000, RateBps: rate, QueueUs: 400_000, OverheadB: 40}
}

func runCC(seconds int, steps []step) *ccRun {
	sim := link.NewSim(42)
	a, b := sim.Pipe(steps[0].params, link.Params{DelayUs: 25_000}, 1200)
	snd := hop.NewSender(a, cc.Config{StartBps: 300_000, MinBps: 50_000, MaxBps: 5_000_000})
	snd.AddStream(1, false)
	rcv := &receiverSide{rcv: hop.NewReceiver()}
	a.SetHandler(senderSide{snd})
	b.SetHandler(rcv)
	for _, s := range steps[1:] {
		p := s.params
		sim.At(s.at, func() { a.Out().SetParams(p) })
	}
	sim.Every(0, 5_000, func() {
		now := sim.Now()
		snd.Tick(now)
		if rep := rcv.rcv.Report(now); rep != nil {
			b.SendControl(rep)
		}
	})
	var seq, frame uint16
	debt := 0.0
	sim.Every(1_000, 33_333, func() {
		now := sim.Now()
		debt += float64(snd.Target()) * 0.95 / (1 + snd.FECRatio(1)) / 30 / 8
		size := int(debt)
		debt -= float64(size)
		maxp := snd.MaxPayload()
		n := max(1, (size+maxp-1)/maxp)
		for i := range n {
			m := wire.Media{Stream: 1, Seq: seq, FrameID: frame, FragIndex: uint8(i), FragCount: uint8(n), Timestamp: uint32(now), Payload: make([]byte, size/n)}
			seq++
			snd.Send(m.Append(nil), now)
		}
		frame++
	})
	run := &ccRun{link: a.Out(), snd: snd}
	lastBytes := 0
	sim.Every(500_000, 500_000, func() {
		now := sim.Now()
		c := snd.Controller()
		run.samples = append(run.samples, sample{
			at: now, capacity: a.Out().Params().RateBps, target: c.Target(),
			goodput: (rcv.bytes - lastBytes) * 16, queue: a.Out().QueueDelay(now), rtt: c.RTT(), loss: c.Loss(),
		})
		lastBytes = rcv.bytes
	})
	sim.RunUntil(int64(seconds) * 1_000_000)
	return run
}

func (r *ccRun) between(from, to int64) []sample {
	var out []sample
	for _, s := range r.samples {
		if s.at >= from && s.at < to {
			out = append(out, s)
		}
	}
	return out
}

func meanTarget(s []sample) float64 {
	t := 0.0
	for _, x := range s {
		t += float64(x.target)
	}
	return t / float64(len(s))
}

func p95Queue(s []sample) int64 {
	q := make([]int64, len(s))
	for i, x := range s {
		q[i] = x.queue
	}
	slices.Sort(q)
	return q[min(len(q)-1, len(q)*95/100)]
}

func (r *ccRun) table(every int) string {
	var b strings.Builder
	b.WriteString("   t(s)  cap(kbps)  target(kbps)  goodput(kbps)  queue(ms)  rtt(ms)  loss%\n")
	for i, s := range r.samples {
		if (i+1)%every != 0 {
			continue
		}
		capS := "none"
		if s.capacity > 0 {
			capS = fmt.Sprint(s.capacity / 1000)
		}
		fmt.Fprintf(&b, "  %5.1f  %9s  %12d  %13d  %9.0f  %7.0f  %5.1f\n", float64(s.at)/1e6, capS, s.target/1000, s.goodput/1000, float64(s.queue)/1000, float64(s.rtt)/1000, 100*s.loss)
	}
	return b.String()
}

func TestConvergesUnderEachCap(t *testing.T) {
	for _, capacity := range []int{300_000, 1_000_000, 3_000_000} {
		r := runCC(40, []step{{0, base(capacity)}})
		steady := r.between(20_000_000, 40_000_000)
		mean := meanTarget(steady)
		q := p95Queue(steady)
		peak := 0
		for _, x := range steady {
			peak = max(peak, x.target)
		}
		t.Logf("cap %d kbps: peak target %d kbps (%.0f%% of cap)", capacity/1000, peak/1000, 100*float64(peak)/float64(capacity))
		t.Logf("cap %d kbps: mean target %0.f kbps (%.0f%% of cap), p95 queue %d ms, link queue p95 %d ms, drops %d\n%s",
			capacity/1000, mean/1000, 100*mean/float64(capacity), q/1000, r.link.Stats.QueueDelayPercentile(0.95)/1000, r.link.Stats.QueueDrops, r.table(4))
		if mean < 0.6*float64(capacity) || mean > 1.1*float64(capacity) {
			t.Errorf("cap %d: mean target %.0f not near the cap", capacity, mean)
		}
		if q > 150_000 {
			t.Errorf("cap %d: p95 queue %d ms", capacity, q/1000)
		}
	}
}

func TestRecoversAfterACapDropAndRise(t *testing.T) {
	r := runCC(60, []step{{0, base(2_500_000)}, {20_000_000, base(500_000)}, {40_000_000, base(2_500_000)}})
	t.Logf("2.5 Mbps, 500 kbps from 20 s, 2.5 Mbps from 40 s\n%s", r.table(2))
	before := meanTarget(r.between(12_000_000, 20_000_000))
	after := meanTarget(r.between(23_000_000, 40_000_000))
	back := meanTarget(r.between(50_000_000, 60_000_000))
	if before < 1_500_000 {
		t.Errorf("before the drop: %.0f", before)
	}
	if after > 600_000 || after < 250_000 {
		t.Errorf("during the drop: %.0f", after)
	}
	if back < 1_500_000 {
		t.Errorf("after the rise: %.0f", back)
	}
	if q := p95Queue(r.between(23_000_000, 40_000_000)); q > 200_000 {
		t.Errorf("queue during the drop %d ms", q/1000)
	}
	for _, s := range r.between(20_000_000, 60_000_000) {
		if s.capacity == 2_500_000 && s.target >= 1_500_000 {
			t.Logf("back above 1.5 Mbps %.1f s after the rise", float64(s.at-40_000_000)/1e6)
			break
		}
	}
}

func TestDoesNotCollapseUnderRandomLoss(t *testing.T) {
	for _, loss := range []float64{0.05, 0.10} {
		p := base(2_000_000)
		p.Loss = loss
		r := runCC(40, []step{{0, p}})
		mean := meanTarget(r.between(20_000_000, 40_000_000))
		t.Logf("2 Mbps with %.0f%% random loss: mean target %.0f kbps\n%s", 100*loss, mean/1000, r.table(8))
		if mean < 1_200_000 {
			t.Errorf("collapsed under %.0f%% loss: %.0f", 100*loss, mean)
		}
	}
}
