package link

import (
	"encoding/binary"
	"math"
	"testing"
)

type sink struct {
	datagrams []int64
	order     []uint32
	controls  []uint32
}

func (s *sink) HandleDatagram(p []byte, now int64) {
	s.datagrams = append(s.datagrams, now)
	s.order = append(s.order, binary.BigEndian.Uint32(p))
}

func (s *sink) HandleControl(p []byte, now int64) {
	s.controls = append(s.controls, binary.BigEndian.Uint32(p))
}

func run(t *testing.T, p Params, n int, gap int64, size int) (*sink, *Link) {
	sim := NewSim(1)
	a, b := sim.Pipe(p, Params{}, 1500)
	s := &sink{}
	b.SetHandler(s)
	for i := range n {
		buf := make([]byte, size)
		binary.BigEndian.PutUint32(buf, uint32(i))
		sim.At(int64(i)*gap, func() {
			if err := a.SendDatagram(buf); err != nil {
				t.Fatal(err)
			}
			if p.RateBps == 0 {
				a.SendControl(buf)
			}
		})
	}
	sim.RunUntil(int64(n)*gap + 10_000_000)
	return s, a.Out()
}

func TestRandomAndBurstLossHitTheirRates(t *testing.T) {
	s, _ := run(t, Params{Loss: 0.1}, 20000, 1000, 100)
	if got := 1 - float64(len(s.order))/20000; math.Abs(got-0.1) > 0.01 {
		t.Fatalf("loss %.3f", got)
	}
	s, _ = run(t, Params{BurstEnter: 0.02, BurstLeave: 0.25, BurstLoss: 0.9}, 20000, 1000, 100)
	got := 1 - float64(len(s.order))/20000
	if math.Abs(got-0.066) > 0.015 {
		t.Fatalf("burst loss %.3f", got)
	}
	if len(s.controls) != 20000 {
		t.Fatalf("control lost: %d", len(s.controls))
	}
	for i, v := range s.controls {
		if v != uint32(i) {
			t.Fatal("control out of order")
		}
	}
}

func TestJitterReordersUnlessInOrder(t *testing.T) {
	s, _ := run(t, Params{DelayUs: 100_000, JitterUs: 30_000}, 2000, 2000, 100)
	reordered := 0
	for i := 1; i < len(s.order); i++ {
		if s.order[i] < s.order[i-1] {
			reordered++
		}
	}
	if reordered == 0 {
		t.Fatal("no reordering with jitter")
	}
	s, _ = run(t, Params{DelayUs: 100_000, JitterUs: 30_000, InOrder: true}, 2000, 2000, 100)
	for i := 1; i < len(s.order); i++ {
		if s.order[i] < s.order[i-1] {
			t.Fatal("in-order link reordered")
		}
	}
}

func TestARateCapQueuesThenDropsTheTail(t *testing.T) {
	s, l := run(t, Params{RateBps: 1_000_000, QueueUs: 100_000}, 2000, 1000, 250)
	got := float64(len(s.order)) * 250 * 8 / 2.0
	if math.Abs(got-1_000_000) > 80_000 {
		t.Fatalf("delivered %.0f bps", got)
	}
	if l.Stats.QueueDrops == 0 {
		t.Fatal("no tail drops at twice the rate")
	}
	if q := l.Stats.QueueDelayPercentile(0.99); q > 101_000 {
		t.Fatalf("queue delay %d over the limit", q)
	}
}
