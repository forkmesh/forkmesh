package hop

import (
	"testing"

	"forkmesh.com/mirror-node/rtc/fmm/cc"
	"forkmesh.com/mirror-node/rtc/fmm/fec"
	"forkmesh.com/mirror-node/rtc/fmm/frag"
	"forkmesh.com/mirror-node/rtc/fmm/link"
	"forkmesh.com/mirror-node/rtc/fmm/wire"
)

type sink struct {
	rcv  *Receiver
	snd  *Sender
	seen map[uint16]bool
}

func (s *sink) HandleDatagram(p []byte, now int64) {
	for _, b := range s.rcv.Handle(p, now) {
		s.seen[wire.SeqOf(b)] = true
	}
}

func (s *sink) HandleControl(p []byte, now int64) {}

type back struct{ snd *Sender }

func (b back) HandleDatagram(p []byte, now int64) {}

func (b back) HandleControl(p []byte, now int64) {
	if m, _, err := wire.Decode(p); err == nil {
		if r, ok := m.(*wire.Report); ok {
			b.snd.OnReport(r, now)
		}
	}
}

func residualThroughLink(t *testing.T, fixed *fec.Params, loss float64) (raw, after float64) {
	sim := link.NewSim(3)
	a, b := sim.Pipe(link.Params{DelayUs: 20_000, Loss: loss}, link.Params{DelayUs: 20_000}, 1200)
	snd := NewSender(a, cc.Config{})
	snd.FixedFEC = fixed
	snd.AddStream(1, true)
	s := &sink{rcv: NewReceiver(), snd: snd, seen: map[uint16]bool{}}
	b.SetHandler(s)
	a.SetHandler(back{snd})
	pk := frag.NewPacketizer(1, snd.MaxPayload())
	n := 0
	sim.Every(0, 20_000, func() {
		pkts, _ := pk.Packetize(make([]byte, 80), false, 0, true, uint32(sim.Now()))
		for _, p := range pkts {
			snd.Send(p, sim.Now())
			n++
		}
	})
	sim.Every(0, 5_000, func() {
		snd.Tick(sim.Now())
		if r := s.rcv.Report(sim.Now()); r != nil {
			b.SendControl(r)
		}
	})
	sim.RunUntil(60_000_000)
	got := 0
	for i := range n - 10 {
		if s.seen[uint16(i)] {
			got++
		}
	}
	return 1 - float64(s.rcv.Media)/float64(n), 1 - float64(got)/float64(n-10)
}

func TestFECThroughALossyHopMatchesTheCode(t *testing.T) {
	for _, p := range []fec.Params{{K: 2, M: 2}, {K: 3, M: 2}, {K: 1, M: 1}} {
		raw, after := residualThroughLink(t, &p, 0.10)
		want := fec.Residual(p.K, p.M, 0.10, 1)
		t.Logf("audio k=%d m=%d at 10%%: raw %.3f after %.4f (code alone %.4f)", p.K, p.M, raw, after, want)
		if after > 3*want+0.002 {
			t.Errorf("k=%d m=%d: residual %.4f, the code gives %.4f", p.K, p.M, after, want)
		}
	}
	raw, after := residualThroughLink(t, nil, 0.10)
	t.Logf("adaptive audio at 10%%: raw %.3f after %.4f", raw, after)
}

func TestRetransmissionRepairsVideoLossWithoutFEC(t *testing.T) {
	sim := link.NewSim(5)
	a, b := sim.Pipe(link.Params{DelayUs: 20_000, Loss: 0.10}, link.Params{DelayUs: 20_000}, 1200)
	snd := NewSender(a, cc.Config{StartBps: 1_000_000})
	snd.FixedFEC = &fec.Params{}
	snd.AddStream(1, false)
	rcv := NewReceiver()
	rcv.EnableNack(1)
	s := &sink{rcv: rcv, snd: snd, seen: map[uint16]bool{}}
	b.SetHandler(s)
	a.SetHandler(nackBack{snd: snd})
	pk := frag.NewPacketizer(1, snd.MaxPayload())
	n := 0
	sim.Every(0, 33_333, func() {
		key := n == 0
		pkts, _ := pk.Packetize(make([]byte, 3000), key, 0, false, uint32(sim.Now()))
		for _, p := range pkts {
			snd.Send(p, sim.Now())
			n++
		}
	})
	sim.Every(0, 5_000, func() {
		snd.Tick(sim.Now())
		rcv.SetRTT(snd.Controller().RTT())
		if r := rcv.Report(sim.Now()); r != nil {
			b.SendControl(r)
		}
		for _, m := range rcv.Nacks(sim.Now()) {
			b.SendControl(m)
		}
	})
	sim.RunUntil(20_000_000)
	got := 0
	for i := range n - 30 {
		if s.seen[uint16(i)] {
			got++
		}
	}
	residual := 1 - float64(got)/float64(n-30)
	t.Logf("10%% loss, NACK only: %d retransmissions for %d packets, residual %.4f", snd.Retransmits, n, residual)
	if residual > 0.002 {
		t.Fatalf("residual loss %.4f with retransmission", residual)
	}
}

type nackBack struct{ snd *Sender }

func (b nackBack) HandleDatagram(p []byte, now int64) {}

func (b nackBack) HandleControl(p []byte, now int64) {
	m, _, err := wire.Decode(p)
	if err != nil {
		return
	}
	switch v := m.(type) {
	case *wire.Report:
		b.snd.OnReport(v, now)
	case wire.Nack:
		b.snd.OnNack(v, now)
	}
}
