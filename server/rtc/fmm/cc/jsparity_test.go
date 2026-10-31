package cc_test

import (
	"bytes"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"math/rand/v2"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"forkmesh.com/mirror-node/rtc/fmm/cc"
	"forkmesh.com/mirror-node/rtc/fmm/link"
	"forkmesh.com/mirror-node/rtc/fmm/wire"
)

const jsParityFile = "testdata/js-parity.json"

type jsReport struct {
	Base     uint16  `json:"base"`
	RefTime  uint32  `json:"refTime"`
	Delay    uint16  `json:"delay"`
	Received string  `json:"received"`
	Arrival  []int64 `json:"arrival"`
	Hex      string  `json:"hex,omitempty"`
}

func reportOf(r *wire.Report, withHex bool) *jsReport {
	var sb strings.Builder
	for _, ok := range r.Received {
		if ok {
			sb.WriteByte('1')
		} else {
			sb.WriteByte('0')
		}
	}
	out := &jsReport{Base: r.Base, RefTime: r.RefTime, Delay: r.Delay, Received: sb.String(), Arrival: r.Arrival}
	if withHex {
		b, err := wire.Append(nil, r)
		if err != nil {
			panic(err)
		}
		out.Hex = hex.EncodeToString(b)
	}
	return out
}

type recorderTrace struct {
	Name   string `json:"name"`
	Repeat int    `json:"repeat"`
	Events []any  `json:"events"`
	Final  []any  `json:"final"`
}

func recorderScenario(name string, seed uint64, repeat int, start uint16, n int, loss, reorder, dup float64, jump int) recorderTrace {
	rng := rand.New(rand.NewPCG(seed, seed^0x51))
	rec := &cc.Recorder{Repeat: repeat}
	tr := recorderTrace{Name: name, Repeat: repeat}
	type arr struct {
		seq uint16
		at  int64
	}
	var arrivals []arr
	now := int64(1_000_000)
	seq := start
	for i := range n {
		if jump > 0 && i == n/2 {
			seq += uint16(jump)
		}
		now += int64(1000 + rng.IntN(3000))
		if rng.Float64() >= loss {
			arrivals = append(arrivals, arr{seq, now})
			if rng.Float64() < dup {
				arrivals = append(arrivals, arr{seq, now + int64(rng.IntN(2000))})
			}
		}
		seq++
	}
	for i := 0; i+1 < len(arrivals); i++ {
		if rng.Float64() < reorder {
			arrivals[i].seq, arrivals[i+1].seq = arrivals[i+1].seq, arrivals[i].seq
		}
	}
	next := arrivals[0].at
	emit := func(t int64) {
		due := rec.Due(t)
		r := rec.Report(t)
		if r == nil {
			tr.Events = append(tr.Events, []any{1, t, due, nil})
			return
		}
		tr.Events = append(tr.Events, []any{1, t, due, reportOf(r, true)})
	}
	for _, a := range arrivals {
		for next < a.at {
			emit(next)
			next += 7_000 + int64(rng.IntN(60_000))
		}
		rec.OnPacket(a.seq, a.at)
		tr.Events = append(tr.Events, []any{0, a.seq, a.at})
	}
	for k := 0; k < 6; k++ {
		emit(next)
		next += 50_000
	}
	tr.Final = []any{rec.Packets}
	return tr
}

type ctrlScenario struct {
	name     string
	seed     uint64
	cfg      cc.Config
	up       link.Params
	down     link.Params
	steps    []stepAt
	downSet  []stepAt
	seconds  int64
	video    float64
	fixedBps int
	requests []int64
}

type stepAt struct {
	at int64
	p  link.Params
}

type ctrlTrace struct {
	Name   string    `json:"name"`
	Config cc.Config `json:"config"`
	Events []any     `json:"events"`
	Final  []any     `json:"final"`
}

func snapshot(c *cc.Controller, now int64) []any {
	return []any{c.Target(), c.RTT(), c.Loss(), c.Burst(), c.Signal(), c.AckedRate(), c.Threshold(), c.QueueDelay(),
		c.Congested(now), c.ProbeEstimate(now), c.Reports, c.Overuses, c.LostTotal, c.RecvTotal, c.ProbesSent, c.ProbesWon,
		c.FastCuts, c.SilenceCuts, c.SilenceUndone, c.StandCuts}
}

type ccSender struct {
	ctrl *cc.Controller
	tr   *ctrlTrace
	sim  *link.Sim
}

func (s *ccSender) HandleDatagram(p []byte, now int64) {
	m, _, err := wire.Decode(p)
	if err != nil {
		return
	}
	r, ok := m.(*wire.Report)
	if !ok {
		return
	}
	s.ctrl.OnReport(r, now)
	s.tr.Events = append(s.tr.Events, []any{1, now, reportOf(r, false), snapshot(s.ctrl, now)})
}

func (s *ccSender) HandleControl(p []byte, now int64) {}

type ccReceiver struct{ rec *cc.Recorder }

func (r *ccReceiver) HandleDatagram(p []byte, now int64) {
	if len(p) >= 2 {
		r.rec.OnPacket(binary.BigEndian.Uint16(p), now)
	}
}

func (r *ccReceiver) HandleControl(p []byte, now int64) {}

func controllerScenario(sc ctrlScenario) ctrlTrace {
	sim := link.NewSim(sc.seed)
	a, b := sim.Pipe(sc.up, sc.down, 8000)
	ctrl := cc.NewController(sc.cfg)
	tr := ctrlTrace{Name: sc.name, Config: sc.cfg}
	snd := &ccSender{ctrl: ctrl, tr: &tr, sim: sim}
	rcv := &ccReceiver{rec: &cc.Recorder{Repeat: 1}}
	a.SetHandler(snd)
	b.SetHandler(rcv)
	for _, st := range sc.steps {
		p := st.p
		sim.At(st.at, func() { a.Out().SetParams(p) })
	}
	for _, st := range sc.downSet {
		p := st.p
		sim.At(st.at, func() { b.Out().SetParams(p) })
	}
	pacer := &cc.Pacer{}
	var seq uint16
	probeID := 0
	packet := func(size int) []byte {
		bb := make([]byte, size)
		return bb
	}
	sim.Every(20_000, 20_000, func() {
		pacer.Push(cc.ClassAudio, packet(100), sim.Now())
	})
	sim.Every(33_333, 33_333, func() {
		bps := sc.fixedBps
		if bps == 0 {
			bps = int(sc.video * float64(ctrl.Target()))
		}
		frame := bps / 30 / 8
		for frame > 0 {
			n := min(frame, 1200)
			pacer.Push(cc.ClassVideo, packet(max(n, 40)), sim.Now())
			frame -= n
		}
	})
	for _, at := range sc.requests {
		sim.At(at, func() {
			ctrl.RequestProbe(sim.Now())
			tr.Events = append(tr.Events, []any{3, sim.Now()})
		})
	}
	sim.Every(5_000, 5_000, func() {
		now := sim.Now()
		if rcv.rec.Due(now) {
			if r := rcv.rec.Report(now); r != nil {
				bb, err := wire.Append(nil, r)
				if err == nil {
					b.SendDatagram(bb)
				}
			}
		}
	})
	sim.Every(5_000, 5_000, func() {
		now := sim.Now()
		cuts := ctrl.SilenceCuts
		ctrl.CheckFeedback(now)
		ev := []any{2, now, 0}
		if ctrl.SilenceCuts != cuts {
			ev[2] = 1
		}
		if probeID > 0 && !pacer.Probing() {
			ctrl.EndProbe(probeID, now)
			ev = append(ev, probeID)
			probeID = 0
		} else {
			ev = append(ev, 0)
		}
		if probeID == 0 {
			id, rate, nbytes, ok := ctrl.StartProbe(now)
			if ok {
				pacer.SetProbe(rate, nbytes)
				probeID = id
				ev = append(ev, id, rate, nbytes)
			}
		}
		if ev[2] == 1 {
			ev = append(ev, snapshot(ctrl, now))
		}
		if len(ev) == 4 && ev[2] == 0 && ev[3] == 0 {
			tr.Events = append(tr.Events, now)
		} else {
			tr.Events = append(tr.Events, ev)
		}
		pacer.SetRate(int(cc.PacingFactor * float64(ctrl.Target())))
		pacer.Tick(now, func(p []byte, probing bool) {
			seq++
			binary.BigEndian.PutUint16(p, seq)
			pid := 0
			if probing {
				pid = probeID
			}
			ctrl.OnSent(seq, len(p), now, pid)
			if pid == 0 {
				tr.Events = append(tr.Events, []any{0, len(p), now})
			} else {
				tr.Events = append(tr.Events, []any{0, len(p), now, pid})
			}
			a.SendDatagram(p)
		}, func(size int) []byte { return packet(size) })
	})
	sim.RunUntil(sc.seconds * 1_000_000)
	tr.Final = snapshot(ctrl, sim.Now())
	return tr
}

type pacerTrace struct {
	Name   string `json:"name"`
	Events []any  `json:"events"`
}

func pacerScenario(name string, seed uint64, ticks int) pacerTrace {
	rng := rand.New(rand.NewPCG(seed, seed^0x77))
	p := &cc.Pacer{}
	tr := pacerTrace{Name: name}
	id := uint32(0)
	mk := func(size int) []byte {
		id++
		bb := make([]byte, max(size, 4))
		binary.BigEndian.PutUint32(bb, id)
		return bb
	}
	now := int64(10_000)
	for t := range ticks {
		now += int64(4_000 + rng.IntN(2_000))
		switch r := rng.IntN(100); {
		case r < 5:
			bps := 100_000 + rng.IntN(3_000_000)
			p.SetRate(bps)
			tr.Events = append(tr.Events, []any{1, bps})
		case r < 7:
			bps, nb := 200_000+rng.IntN(4_000_000), 400+rng.IntN(8000)
			p.SetProbe(bps, nb)
			tr.Events = append(tr.Events, []any{2, bps, nb})
		case r < 9:
			age := int64(50_000 + rng.IntN(300_000))
			p.DropOlder(age, now)
			tr.Events = append(tr.Events, []any{3, age, now})
		case r < 10:
			bps := rng.IntN(2_000_000)
			p.SetDrainCap(bps)
			tr.Events = append(tr.Events, []any{4, bps})
		}
		pushes := rng.IntN(4)
		if t%97 == 50 {
			pushes = 40
		}
		for range pushes {
			cls := rng.IntN(4) - 0
			size := 40 + rng.IntN(1160)
			if cls == 0 {
				size = 60 + rng.IntN(140)
			}
			if t%331 == 200 {
				cls, size = cc.ClassVideo, 1200
				for range 1900 {
					p.Push(cls, mk(size), now)
				}
				tr.Events = append(tr.Events, []any{6, cls, size, now, 1900})
				continue
			}
			p.Push(cls, mk(size), now)
			tr.Events = append(tr.Events, []any{0, cls, size, now})
		}
		var sent []any
		usePad := t%3 != 0
		var pad func(int) []byte
		if usePad {
			pad = func(size int) []byte { return mk(size) }
		}
		p.Tick(now, func(b []byte, probe bool) {
			pr := 0
			if probe {
				pr = 1
			}
			sent = append(sent, binary.BigEndian.Uint32(b), len(b), pr)
		}, pad)
		padFlag := 0
		if usePad {
			padFlag = 1
		}
		tr.Events = append(tr.Events, []any{5, now, padFlag, sent, p.QueuedBytes(), p.Dropped, p.OldestUs(now), p.Probing()})
	}
	return tr
}

type jsFixture struct {
	Recorder   []recorderTrace `json:"recorder"`
	Controller []ctrlTrace     `json:"controller"`
	Pacer      []pacerTrace    `json:"pacer"`
}

func buildJSParity() []byte {
	var f jsFixture
	f.Recorder = append(f.Recorder,
		recorderScenario("clean-wrap", 1, 0, 65400, 400, 0, 0, 0, 0),
		recorderScenario("loss-reorder-dup", 2, 1, 65500, 700, 0.1, 0.05, 0.03, 0),
		recorderScenario("heavy-loss", 3, 1, 100, 500, 0.4, 0.02, 0, 0),
		recorderScenario("jump", 4, 0, 30000, 300, 0.05, 0, 0, 5000),
		recorderScenario("window-clip", 5, 1, 7, 300, 0, 0, 0, 2100),
	)
	cfg := cc.Config{StartBps: 300_000, MinBps: 50_000, MaxBps: 3_000_000}
	capped := func(rate int) link.Params {
		return link.Params{DelayUs: 25_000, RateBps: rate, QueueUs: 400_000, OverheadB: 40}
	}
	back := link.Params{DelayUs: 25_000}
	scenarios := []ctrlScenario{
		{name: "cap-step", seed: 11, cfg: cfg, up: capped(1_000_000), down: back, seconds: 8, video: 0.9,
			steps: []stepAt{{3_000_000, capped(300_000)}, {5_500_000, capped(1_000_000)}}},
		{name: "loss-5", seed: 12, cfg: cfg, up: link.Params{DelayUs: 25_000, RateBps: 1_500_000, QueueUs: 400_000, OverheadB: 40, Loss: 0.05}, down: back, seconds: 5, video: 0.9},
		{name: "loss-30", seed: 13, cfg: cfg, up: link.Params{DelayUs: 25_000, RateBps: 1_000_000, QueueUs: 400_000, OverheadB: 40, Loss: 0.30}, down: back, seconds: 5, video: 0.9},
		{name: "standing-queue", seed: 14, cfg: cc.Config{StartBps: 900_000, MinBps: 50_000, MaxBps: 3_000_000}, up: capped(500_000), down: back, seconds: 6, video: 1.0},
		{name: "feedback-silence", seed: 15, cfg: cfg, up: capped(800_000), down: back, seconds: 6, video: 0.9,
			downSet: []stepAt{{3_000_000, link.Params{DelayUs: 25_000, Loss: 1}}, {3_600_000, back}}},
		{name: "app-limited", seed: 16, cfg: cfg, up: capped(1_000_000), down: back, seconds: 7, fixedBps: 30_000,
			requests: []int64{1_500_000, 2_500_000, 3_000_000, 4_700_000, 6_900_000}},
		{name: "drop-to-100k", seed: 17, cfg: cfg, up: capped(1_200_000), down: back, seconds: 8, video: 0.9,
			steps:   []stepAt{{3_000_000, capped(100_000)}, {5_500_000, capped(1_200_000)}},
			downSet: []stepAt{{3_000_000, link.Params{DelayUs: 25_000, RateBps: 100_000, QueueUs: 400_000, OverheadB: 40}}, {5_500_000, back}}},
		{name: "burst-jitter", seed: 18, cfg: cfg, up: link.Params{DelayUs: 30_000, JitterUs: 8_000, ReorderRate: 0.03, ReorderUs: 10_000, RateBps: 1_000_000, QueueUs: 300_000, OverheadB: 40,
			BurstEnter: 0.02, BurstLeave: 0.25, BurstLoss: 0.9}, down: link.Params{DelayUs: 30_000, JitterUs: 5_000}, seconds: 6, video: 0.9},
	}
	for _, sc := range scenarios {
		f.Controller = append(f.Controller, controllerScenario(sc))
	}
	f.Pacer = append(f.Pacer, pacerScenario("mixed", 21, 700), pacerScenario("mixed-2", 22, 500))
	out, err := json.Marshal(f)
	if err != nil {
		panic(err)
	}
	return append(out, '\n')
}

func TestJSParity(t *testing.T) {
	got := buildJSParity()
	if again := buildJSParity(); !bytes.Equal(got, again) {
		t.Fatal("the JS parity fixture is not deterministic")
	}
	if os.Getenv("FMM_JS_PARITY_WRITE") == "1" {
		if err := os.MkdirAll(filepath.Dir(jsParityFile), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(jsParityFile, got, 0o644); err != nil {
			t.Fatal(err)
		}
		t.Logf("wrote %s (%d bytes)", jsParityFile, len(got))
		return
	}
	stored, err := os.ReadFile(jsParityFile)
	if err != nil {
		t.Fatalf("%v (run with FMM_JS_PARITY_WRITE=1)", err)
	}
	if !bytes.Equal(stored, got) {
		t.Fatalf("%s is stale; regenerate with FMM_JS_PARITY_WRITE=1", jsParityFile)
	}
}
