package fec

import (
	"bytes"
	"fmt"
	"math/rand/v2"
	"os"
	"testing"

	"forkmesh.com/mirror-node/rtc/fmm/wire"
)

func randomBlocks(r *rand.Rand, k, size int) [][]byte {
	out := make([][]byte, k)
	for i := range out {
		out[i] = make([]byte, size)
		for j := range out[i] {
			out[i][j] = byte(r.Uint32())
		}
	}
	return out
}

func TestTheFirstRepairIsPlainXOR(t *testing.T) {
	r := rand.New(rand.NewPCG(1, 2))
	blocks := randomBlocks(r, 7, 33)
	x := make([]byte, 33)
	for _, b := range blocks {
		for i := range b {
			x[i] ^= b[i]
		}
	}
	if !bytes.Equal(Encode(blocks, 3)[0], x) || !bytes.Equal(Encode(blocks, 1)[0], x) {
		t.Fatal("row 0 is not XOR parity")
	}
}

func TestReedSolomonRecoversAnyMErasures(t *testing.T) {
	r := rand.New(rand.NewPCG(3, 4))
	for _, geo := range [][2]int{{1, 1}, {2, 2}, {4, 2}, {8, 4}, {16, 4}, {32, 16}, {48, 16}} {
		k, m := geo[0], geo[1]
		blocks := randomBlocks(r, k, 64)
		reps := Encode(blocks, m)
		for trial := range 50 {
			lose := r.IntN(m + 1)
			idx := r.Perm(k + m)[:lose]
			data := append([][]byte(nil), blocks...)
			rep := append([][]byte(nil), reps...)
			for _, i := range idx {
				if i < k {
					data[i] = nil
				} else {
					rep[i-k] = nil
				}
			}
			if err := Reconstruct(data, rep); err != nil {
				t.Fatalf("k=%d m=%d trial %d: %v", k, m, trial, err)
			}
			for i := range blocks {
				if !bytes.Equal(data[i], blocks[i]) {
					t.Fatalf("k=%d m=%d block %d wrong", k, m, i)
				}
			}
		}
		data := append([][]byte(nil), blocks...)
		for i := 0; i <= m && i < k; i++ {
			data[i] = nil
		}
		if m < k && Reconstruct(data, reps) != ErrTooFew {
			t.Fatalf("k=%d m=%d recovered more than m erasures", k, m)
		}
	}
}

func mediaPacket(seq uint16, frame uint16, size int, r *rand.Rand) []byte {
	m := wire.Media{Stream: 5, TransportSeq: 99, Seq: seq, FrameID: frame, FragCount: 1, Timestamp: uint32(seq) * 20000, Payload: make([]byte, size)}
	for i := range m.Payload {
		m.Payload[i] = byte(r.Uint32())
	}
	return m.Append(nil)
}

type lossModel struct {
	r        *rand.Rand
	p        float64
	enter    float64
	leave    float64
	bad      bool
	gilbert  bool
	received int
	lost     int
}

func (l *lossModel) drop() bool {
	var d bool
	if l.gilbert {
		if l.bad {
			l.bad = l.r.Float64() >= l.leave
		} else {
			l.bad = l.r.Float64() < l.enter
		}
		d = l.bad
	} else {
		d = l.r.Float64() < l.p
	}
	if d {
		l.lost++
	} else {
		l.received++
	}
	return d
}

func runRecovery(t *testing.T, params Params, loss *lossModel, packets int) (raw, residual float64, overhead float64) {
	r := rand.New(rand.NewPCG(7, 8))
	enc := NewEncoder(5, 1200, 1<<40)
	enc.SetParams(params)
	dec := NewDecoder(5)
	got := map[uint16][]byte{}
	sent := map[uint16][]byte{}
	mediaLost, repairBytes, mediaBytes := 0, 0, 0
	for i := range packets {
		seq := uint16(i)
		pkt := mediaPacket(seq, uint16(i), 200+r.IntN(900), r)
		repairs := enc.Add(pkt, int64(i))
		sent[seq] = append([]byte(nil), pkt...)
		mediaBytes += len(pkt)
		if !loss.drop() {
			got[seq] = pkt
			for _, p := range dec.AddMedia(pkt, true, int64(i)) {
				got[wire.SeqOf(p)] = p
			}
		} else {
			mediaLost++
		}
		for _, rp := range repairs {
			repairBytes += len(rp)
			if loss.drop() {
				continue
			}
			rr, err := wire.ParseRepair(rp)
			if err != nil {
				t.Fatal(err)
			}
			for _, p := range dec.AddRepair(rr, int64(i)) {
				got[wire.SeqOf(p)] = p
			}
		}
	}
	missing := 0
	for seq, want := range sent {
		g, ok := got[seq]
		if !ok {
			missing++
			continue
		}
		a, _ := wire.ParseMedia(g)
		b, _ := wire.ParseMedia(want)
		a.TransportSeq, b.TransportSeq = 0, 0
		if !bytes.Equal(a.Append(nil), b.Append(nil)) {
			t.Fatalf("seq %d recovered wrong", seq)
		}
	}
	return float64(mediaLost) / float64(packets), float64(missing) / float64(packets), float64(repairBytes) / float64(mediaBytes)
}

func TestRecoveryRates(t *testing.T) {
	type row struct {
		name   string
		params Params
		loss   func() *lossModel
		max    float64
	}
	random := func(p float64) func() *lossModel {
		return func() *lossModel { return &lossModel{r: rand.New(rand.NewPCG(11, 12)), p: p} }
	}
	burst := func() *lossModel {
		return &lossModel{r: rand.New(rand.NewPCG(13, 14)), gilbert: true, enter: 0.02, leave: 0.25}
	}
	rows := []row{
		{"xor k=8, 2% random", Params{K: 8, M: 1}, random(0.02), 0.004},
		{"xor k=4, 5% random", Params{K: 4, M: 1}, random(0.05), 0.012},
		{"rs 16+4, 5% random", Params{K: 16, M: 4}, random(0.05), 0.002},
		{"rs 16+6, 10% random", Params{K: 16, M: 6}, random(0.10), 0.006},
		{"rs 24+12, 20% random", Params{K: 24, M: 12}, random(0.20), 0.03},
		{"rs 16+6, burst 7%/4", Params{K: 16, M: 6}, burst, -0.6},
		{"policy burst 7%/4", Choose(false, 0.074, 4, 48), burst, -0.4},
		{"policy video 10%", Choose(false, 0.10, 1, 48), random(0.10), 0.01},
		{"policy audio 20%", Choose(true, 0.20, 1, 3), random(0.20), 0.02},
	}
	packets := 2000
	if os.Getenv("FMM_SIM") != "" {
		packets = 20000
	}
	for _, rw := range rows {
		raw, res, over := runRecovery(t, rw.params, rw.loss(), packets)
		t.Logf("%-22s k=%-2d m=%-2d raw loss %5.2f%%  after FEC %5.2f%%  overhead %5.1f%%", rw.name, rw.params.K, rw.params.M, 100*raw, 100*res, 100*over)
		limit := rw.max
		if limit < 0 {
			limit = -limit * raw
		}
		if res > limit {
			t.Errorf("%s: residual loss %.4f above %.4f", rw.name, res, limit)
		}
	}
}

func TestPolicyIsOffOnCleanLinksAndGrowsWithLoss(t *testing.T) {
	if Choose(false, 0.001, 1, 48).On() || Choose(true, 0.005, 1, 4).On() {
		t.Fatal("FEC on a clean link")
	}
	prev := 0.0
	for _, p := range []float64{0.01, 0.03, 0.05, 0.10, 0.20} {
		c := Choose(false, p, 1, 48)
		if !c.On() || c.Ratio() < prev {
			t.Fatalf("loss %.2f: %+v", p, c)
		}
		prev = c.Ratio()
		if c.Ratio() > videoOverhead+1e-9 {
			t.Fatalf("loss %.2f: overhead %.2f over cap", p, c.Ratio())
		}
	}
	if b := Choose(false, 0.05, 4, 48); b.Ratio() <= Choose(false, 0.05, 1, 48).Ratio() {
		t.Fatal("bursts did not add repairs")
	}
	if c := Choose(false, 0.05, 1, 6); c.K > 6 {
		t.Fatalf("group larger than the packets available: %+v", c)
	}
}

func TestGroupsSpanGapsAndReorderingAndCloseOnTime(t *testing.T) {
	r := rand.New(rand.NewPCG(1, 1))
	enc := NewEncoder(1, 1200, 50_000)
	enc.SetParams(Params{K: 4, M: 2})
	dec := NewDecoder(1)
	var pkts [][]byte
	var repairs [][]byte
	for i, seq := range []uint16{10, 12, 11, 15} {
		p := mediaPacket(seq, uint16(i), 100+50*i, r)
		repairs = append(repairs, enc.Add(p, int64(i))...)
		pkts = append(pkts, p)
	}
	if len(repairs) != 2 {
		t.Fatalf("%d repairs for a full group with gaps and reordering", len(repairs))
	}
	rp, err := wire.ParseRepair(repairs[0])
	if err != nil || rp.BaseSeq != 10 || string(rp.Offsets) != string([]uint8{0, 1, 2, 5}) {
		t.Fatalf("%v %+v", err, rp.Offsets)
	}
	dec.AddMedia(pkts[0], true, 0)
	dec.AddMedia(pkts[3], true, 0)
	var got [][]byte
	for _, b := range repairs {
		rr, _ := wire.ParseRepair(b)
		got = append(got, dec.AddRepair(rr, 0)...)
	}
	if len(got) != 2 || !bytes.Equal(got[0][wire.RecoveryOffset:], pkts[2][wire.RecoveryOffset:]) || !bytes.Equal(got[1][wire.RecoveryOffset:], pkts[1][wire.RecoveryOffset:]) {
		t.Fatalf("recovered %d packets, not seqs 11 and 12", len(got))
	}
	enc.Add(mediaPacket(30, 9, 100, r), 100_000)
	if enc.Flush(120_000) != nil {
		t.Fatal("flushed before the delay")
	}
	if len(enc.Flush(150_000)) != 1 {
		t.Fatal("no flush after the delay")
	}
	big := mediaPacket(40, 10, 1200-wire.MediaHeaderLen, r)
	enc.Add(big, 160_000)
	if big[10]&wire.FlagFEC != 0 {
		t.Fatal("a packet too large to protect is flagged")
	}
}

func TestUnequalProtectionKeepsTwoGroupsOpen(t *testing.T) {
	r := rand.New(rand.NewPCG(2, 2))
	enc := NewEncoder(1, 1200, 1<<40)
	enc.SetUnequal(true)
	enc.SetClassParams(Params{K: 4, M: 3}, Params{K: 4, M: 1})
	var groups []wire.Repair
	for i := range 8 {
		m := wire.Media{Stream: 1, Seq: uint16(i), FrameID: uint16(i), FragCount: 1, Layer: uint8(i % 2), RefDelta: uint8(i % 2), Payload: make([]byte, 50+r.IntN(50))}
		if i == 0 {
			m.Keyframe, m.RefDelta = true, 0
		}
		for _, b := range enc.Add(m.Append(nil), int64(i)) {
			rp, _ := wire.ParseRepair(b)
			groups = append(groups, rp)
		}
	}
	var important, other int
	for _, g := range groups {
		if g.Index != 0 {
			continue
		}
		if g.Offsets[1] != 2 {
			t.Fatalf("group mixes layers: %v", g.Offsets)
		}
		if g.BaseSeq%2 == 0 {
			important = int(g.M)
		} else {
			other = int(g.M)
		}
	}
	if important <= other {
		t.Fatalf("TL0 group has %d repairs, upper layers %d", important, other)
	}
}

func BenchmarkEncode(b *testing.B) {
	r := rand.New(rand.NewPCG(1, 2))
	for _, geo := range [][2]int{{8, 1}, {16, 4}, {32, 8}, {48, 16}} {
		blocks := randomBlocks(r, geo[0], 1200)
		b.Run(fmt.Sprintf("k%d_m%d", geo[0], geo[1]), func(b *testing.B) {
			b.SetBytes(int64(geo[0] * 1200))
			for b.Loop() {
				Encode(blocks, geo[1])
			}
		})
	}
}

func BenchmarkReconstruct(b *testing.B) {
	r := rand.New(rand.NewPCG(1, 2))
	k, m := 16, 4
	blocks := randomBlocks(r, k, 1200)
	reps := Encode(blocks, m)
	b.SetBytes(int64(k * 1200))
	for b.Loop() {
		data := append([][]byte(nil), blocks...)
		data[1], data[5], data[9], data[13] = nil, nil, nil, nil
		Reconstruct(data, reps)
	}
}

func FuzzDecoder(f *testing.F) {
	r := rand.New(rand.NewPCG(5, 6))
	enc := NewEncoder(5, 1200, 1<<40)
	enc.SetParams(Params{K: 3, M: 2})
	var seeds [][]byte
	for i := range 3 {
		p := mediaPacket(uint16(i), uint16(i), 50, r)
		seeds = append(seeds, p)
		seeds = append(seeds, enc.Add(p, 0)...)
	}
	for _, s := range seeds {
		f.Add(s, s)
	}
	f.Fuzz(func(t *testing.T, a, b []byte) {
		d := NewDecoder(5)
		for i, p := range [][]byte{a, b, a} {
			k, err := wire.Kind(p)
			if err != nil {
				continue
			}
			var out [][]byte
			switch k {
			case wire.KindMedia:
				if m, err := wire.ParseMedia(p); err == nil {
					out = d.AddMedia(p, m.FEC, int64(i))
				}
			case wire.KindRepair:
				if rp, err := wire.ParseRepair(p); err == nil {
					out = d.AddRepair(rp, int64(i))
				}
			}
			for _, o := range out {
				if _, err := wire.ParseMedia(o); err != nil {
					t.Fatal("recovered an invalid packet")
				}
			}
		}
	})
}
