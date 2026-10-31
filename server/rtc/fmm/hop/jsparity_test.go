package hop

import (
	"bytes"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"os"
	"path/filepath"
	"testing"

	"forkmesh.com/mirror-node/rtc/fmm/cc"
	"forkmesh.com/mirror-node/rtc/fmm/fec"
	"forkmesh.com/mirror-node/rtc/fmm/frag"
	"forkmesh.com/mirror-node/rtc/fmm/link"
	"forkmesh.com/mirror-node/rtc/fmm/wire"
)

type jsOp struct {
	O  string    `json:"o"`
	T  int64     `json:"t"`
	S  int       `json:"s,omitempty"`
	N  int       `json:"n,omitempty"`
	K  bool      `json:"k,omitempty"`
	L  int       `json:"l,omitempty"`
	A  int       `json:"a,omitempty"`
	I  int       `json:"i,omitempty"`
	R  int64     `json:"r,omitempty"`
	C  int       `json:"c"`
	H  string    `json:"h,omitempty"`
	St []float64 `json:"st,omitempty"`
}

type jsStream struct {
	ID    int  `json:"id"`
	Audio bool `json:"audio"`
	Depth int  `json:"depth"`
	Nack  bool `json:"nack"`
}

type jsScenario struct {
	Name     string      `json:"name"`
	Config   cc.Config   `json:"config"`
	MaxDgram int         `json:"maxDatagram"`
	Fixed    *fec.Params `json:"fixed"`
	Repeat   int         `json:"repeat"`
	Streams  []jsStream  `json:"streams"`
	Ops      []jsOp      `json:"ops"`
	Totals   []int       `json:"totals"`
}

type jsPack struct {
	Msgs     []string `json:"msgs"`
	Max      int      `json:"max"`
	Datagram []string `json:"datagrams"`
	Control  []string `json:"control"`
}

type jsHopFixture struct {
	Scenarios []jsScenario `json:"scenarios"`
	Pack      []jsPack     `json:"pack"`
}

func digest(bs [][]byte) string {
	h := sha256.New()
	for _, b := range bs {
		var n [2]byte
		binary.BigEndian.PutUint16(n[:], uint16(len(b)))
		h.Write(n[:])
		h.Write(b)
	}
	return hex.EncodeToString(h.Sum(nil)[:8])
}

type indexer struct {
	all     [][]byte
	content map[string][]int
}

func newIndexer() *indexer { return &indexer{content: map[string][]int{}} }

func (x *indexer) add(p []byte) {
	x.content[string(p)] = append(x.content[string(p)], len(x.all))
	x.all = append(x.all, bytes.Clone(p))
}

func (x *indexer) pop(p []byte) int {
	l := x.content[string(p)]
	if len(l) == 0 {
		return -1
	}
	x.content[string(p)] = l[1:]
	return l[0]
}

type recTransport struct {
	ep      *link.Endpoint
	sent    *indexer
	pending [][]byte
}

func (r *recTransport) SendDatagram(p []byte) error {
	r.sent.add(p)
	r.pending = append(r.pending, bytes.Clone(p))
	return r.ep.SendDatagram(p)
}

func (r *recTransport) MaxDatagramSize() int { return r.ep.MaxDatagramSize() }

type jsRecvSide struct {
	sc   *jsScenario
	rcv  *Receiver
	sent *indexer
}

func (s *jsRecvSide) HandleDatagram(p []byte, now int64) {
	i := s.sent.pop(p)
	out := s.rcv.Handle(p, now)
	op := jsOp{O: "d", T: now, I: i, C: len(out)}
	if len(out) != 1 || !bytes.Equal(out[0], p) {
		op.H = digest(out)
	}
	s.sc.Ops = append(s.sc.Ops, op)
}

func (s *jsRecvSide) HandleControl(p []byte, now int64) {}

type jsSendSide struct {
	sc  *jsScenario
	snd *Sender
	fb  *indexer
}

func (s *jsSendSide) HandleDatagram(p []byte, now int64) {}

func (s *jsSendSide) HandleControl(p []byte, now int64) {
	i := s.fb.pop(p)
	s.sc.Ops = append(s.sc.Ops, jsOp{O: "b", T: now, I: i, C: 0})
	m, _, err := wire.Decode(p)
	if err != nil {
		return
	}
	switch v := m.(type) {
	case *wire.Report:
		s.snd.OnReport(v, now)
	case wire.Nack:
		s.snd.OnNack(v, now)
	}
}

func (sc *jsScenario) state(snd *Sender) []float64 {
	st := []float64{float64(snd.Target()), snd.OverheadBps(), float64(snd.Retransmits), float64(snd.RtxSkipped), float64(snd.Padding),
		float64(snd.Sent), float64(snd.Pacer().QueuedBytes()), float64(snd.Controller().RTT()), snd.Controller().Loss()}
	for _, s := range sc.Streams {
		m, r := snd.StreamRate(uint8(s.ID))
		st = append(st, snd.FECRatio(uint8(s.ID)), m, r, snd.StreamOverheadBps(uint8(s.ID)))
	}
	return st
}

func runHopScenario(name string, seed uint64, up, down link.Params, cfg cc.Config, fixed *fec.Params, streams []jsStream, seconds int64) jsScenario {
	sc := jsScenario{Name: name, Config: cfg, MaxDgram: 1200, Fixed: fixed, Repeat: 1, Streams: streams}
	sim := link.NewSim(seed)
	a, b := sim.Pipe(up, down, sc.MaxDgram)
	sent := newIndexer()
	tr := &recTransport{ep: a, sent: sent}
	snd := NewSender(tr, cfg)
	snd.FixedFEC = fixed
	rcv := NewReceiver()
	rcv.SetRepeat(sc.Repeat)
	pks := map[int]*frag.Packetizer{}
	for _, s := range streams {
		snd.AddStream(uint8(s.ID), s.Audio)
		pks[s.ID] = frag.NewPacketizer(uint8(s.ID), snd.MaxPayload())
		if s.Nack {
			rcv.EnableNack(uint8(s.ID))
		}
	}
	fb := newIndexer()
	b.SetHandler(&jsRecvSide{sc: &sc, rcv: rcv, sent: sent})
	a.SetHandler(&jsSendSide{sc: &sc, snd: snd, fb: fb})
	frame := 0
	for _, s := range streams {
		s := s
		if s.Audio {
			sim.Every(0, 20_000, func() {
				now := sim.Now()
				n := 60 + (frame*37)%90
				frame++
				pkts, err := pks[s.ID].PacketizeFrame(make([]byte, n), frag.Opts{Independent: true, Ref: -1, Aux: uint8(s.Depth)}, uint32(now))
				if err != nil {
					panic(err)
				}
				for _, p := range pkts {
					snd.Send(p, now)
				}
				sc.Ops = append(sc.Ops, jsOp{O: "f", T: now, S: s.ID, N: n, A: s.Depth, K: false, L: 0, C: len(pkts)})
			})
			continue
		}
		count := 0
		sim.Every(1_000, 33_333, func() {
			now := sim.Now()
			key := count%90 == 0
			layer := []int{0, 2, 1, 2}[count%4]
			if key {
				layer = 0
			}
			n := 800 + (count*7919)%4000
			if key {
				n *= 4
			}
			count++
			pkts, err := pks[s.ID].Packetize(make([]byte, n), key, layer, false, uint32(now))
			if err != nil {
				panic(err)
			}
			for _, p := range pkts {
				snd.Send(p, now)
			}
			sc.Ops = append(sc.Ops, jsOp{O: "v", T: now, S: s.ID, N: n, K: key, L: layer, C: len(pkts)})
		})
	}
	ticks := 0
	sim.Every(0, 5_000, func() {
		now := sim.Now()
		tr.pending = nil
		snd.Tick(now)
		op := jsOp{O: "t", T: now, C: len(tr.pending), H: digest(tr.pending)}
		if ticks%8 == 0 {
			op.St = sc.state(snd)
		}
		ticks++
		sc.Ops = append(sc.Ops, op)
		rtt := snd.Controller().RTT()
		rcv.SetRTT(rtt)
		var msgs [][]byte
		if r := rcv.Report(now); r != nil {
			msgs = append(msgs, r)
		}
		msgs = append(msgs, rcv.Nacks(now)...)
		if len(msgs) > 0 {
			sc.Ops = append(sc.Ops, jsOp{O: "r", T: now, R: rtt, C: len(msgs), H: digest(msgs)})
		} else {
			sc.Ops = append(sc.Ops, jsOp{O: "r", T: now, R: rtt})
		}
		for _, m := range msgs {
			fb.add(m)
			b.SendControl(m)
		}
	})
	sim.RunUntil(seconds * 1_000_000)
	sc.Totals = []int{snd.Sent, snd.Retransmits, snd.RtxSkipped, snd.Padding, rcv.Media, rcv.Recovered, rcv.Repairs, rcv.Padding, rcv.Invalid, rcv.Nacked}
	return sc
}

func buildHopFixture() jsHopFixture {
	var fx jsHopFixture
	audio := []jsStream{{ID: 1, Audio: true}}
	fx.Scenarios = append(fx.Scenarios, runHopScenario("audio-loss-20", 11,
		link.Params{DelayUs: 20_000, Loss: 0.20, OverheadB: 40}, link.Params{DelayUs: 20_000, OverheadB: 40},
		cc.Config{}, nil, audio, 5))
	av := []jsStream{{ID: 1, Audio: true, Depth: 10}, {ID: 2, Nack: true}}
	fx.Scenarios = append(fx.Scenarios, runHopScenario("dred-video-loss-10", 12,
		link.Params{DelayUs: 25_000, Loss: 0.10, OverheadB: 40}, link.Params{DelayUs: 25_000, Loss: 0.05, OverheadB: 40},
		cc.Config{StartBps: 1_000_000}, nil, av, 5))
	fx.Scenarios = append(fx.Scenarios, runHopScenario("cap-300k", 13,
		link.Params{DelayUs: 20_000, RateBps: 300_000, QueueUs: 400_000, OverheadB: 40}, link.Params{DelayUs: 20_000, OverheadB: 40},
		cc.Config{StartBps: 2_000_000}, nil, []jsStream{{ID: 1, Audio: true, Depth: 100}, {ID: 3, Nack: true}}, 7))
	fx.Scenarios = append(fx.Scenarios, runHopScenario("burst-jitter-fixed", 14,
		link.Params{DelayUs: 30_000, JitterUs: 8_000, BurstEnter: 0.02, BurstLeave: 0.25, BurstLoss: 0.9, ReorderRate: 0.02, ReorderUs: 15_000, OverheadB: 40},
		link.Params{DelayUs: 30_000, JitterUs: 4_000, InOrder: true, OverheadB: 40},
		cc.Config{StartBps: 800_000, MaxBps: 1_500_000}, &fec.Params{K: 4, M: 2}, []jsStream{{ID: 1, Audio: true}, {ID: 2, Nack: true}}, 4))
	msg := func(m wire.Message) []byte {
		b, err := wire.Append(nil, m)
		if err != nil {
			panic(err)
		}
		return b
	}
	seqs := func(n int) []uint16 {
		out := make([]uint16, n)
		for i := range out {
			out[i] = uint16(i * 3)
		}
		return out
	}
	sets := [][][]byte{
		{msg(wire.Nack{Stream: 2, Seqs: seqs(5)}), msg(wire.Ack{Stream: 3, Frames: []uint16{7, 9}})},
		{msg(wire.Nack{Stream: 2, Seqs: seqs(128)}), msg(wire.Nack{Stream: 4, Seqs: seqs(100)}), msg(wire.Recovery{Stream: 2, Frame: 77}), msg(wire.Nack{Stream: 5, Seqs: seqs(128)})},
		{msg(wire.Nack{Stream: 1, Seqs: seqs(128)})},
	}
	for _, set := range sets {
		for _, max := range []int{1200, 300, 120, 40} {
			p := jsPack{Max: max}
			for _, m := range set {
				p.Msgs = append(p.Msgs, hex.EncodeToString(m))
			}
			d, c := PackFeedback(set, max)
			for _, x := range d {
				p.Datagram = append(p.Datagram, hex.EncodeToString(x))
			}
			for _, x := range c {
				p.Control = append(p.Control, hex.EncodeToString(x))
			}
			fx.Pack = append(fx.Pack, p)
		}
	}
	return fx
}

func TestJSParity(t *testing.T) {
	b, err := json.Marshal(buildHopFixture())
	if err != nil {
		t.Fatal(err)
	}
	b = append(b, '\n')
	path := filepath.Join("testdata", "js-parity.json")
	if os.Getenv("FMM_JS_PARITY_WRITE") == "1" {
		if err := os.MkdirAll("testdata", 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(path, b, 0o644); err != nil {
			t.Fatal(err)
		}
		t.Logf("wrote %s (%d bytes)", path, len(b))
		return
	}
	stored, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("%v (run with FMM_JS_PARITY_WRITE=1)", err)
	}
	if !bytes.Equal(stored, b) {
		t.Fatalf("%s is stale: run FMM_JS_PARITY_WRITE=1 go test -run JSParity ./rtc/fmm/hop", path)
	}
}
