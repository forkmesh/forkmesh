package fec

import (
	"bytes"
	"encoding/hex"
	"encoding/json"
	"math/rand/v2"
	"os"
	"path/filepath"
	"testing"

	"forkmesh.com/mirror-node/rtc/fmm/frag"
	"forkmesh.com/mirror-node/rtc/fmm/wire"
)

type jsEncodeCase struct {
	K       int      `json:"k"`
	M       int      `json:"m"`
	Size    int      `json:"size"`
	Seed    int      `json:"seed"`
	Repairs []string `json:"repairs"`
}

type jsReconstructCase struct {
	Name   string    `json:"name"`
	Data   []*string `json:"data"`
	Repair []*string `json:"repair"`
	Error  string    `json:"error,omitempty"`
	Result []string  `json:"result,omitempty"`
}

type jsResidual struct {
	K     int     `json:"k"`
	M     int     `json:"m"`
	P     float64 `json:"p"`
	Burst float64 `json:"burst"`
	Value float64 `json:"value"`
}

type jsChoose struct {
	Audio bool    `json:"audio"`
	Loss  float64 `json:"loss"`
	Burst float64 `json:"burst"`
	MaxK  int     `json:"maxK"`
	K     int     `json:"k"`
	M     int     `json:"m"`
}

type jsChooseVideo struct {
	Loss  float64 `json:"loss"`
	Burst float64 `json:"burst"`
	MaxKI int     `json:"maxKI"`
	MaxKO int     `json:"maxKO"`
	IK    int     `json:"ik"`
	IM    int     `json:"im"`
	OK    int     `json:"ok"`
	OM    int     `json:"om"`
}

type jsEncState struct {
	K       int     `json:"k"`
	M       int     `json:"m"`
	Ratio   float64 `json:"ratio"`
	Groups  int     `json:"groups"`
	Repairs int     `json:"repairs"`
}

type jsEncStep struct {
	Op    string     `json:"op"`
	K     int        `json:"k,omitempty"`
	M     int        `json:"m,omitempty"`
	OK    int        `json:"ok,omitempty"`
	OM    int        `json:"om,omitempty"`
	On    bool       `json:"on,omitempty"`
	F     float64    `json:"f,omitempty"`
	Pkt   string     `json:"pkt,omitempty"`
	FECOn bool       `json:"fecOn,omitempty"`
	Now   int64      `json:"now"`
	Out   []string   `json:"out"`
	State jsEncState `json:"state"`
}

type jsEncoderTrace struct {
	Name        string      `json:"name"`
	Stream      uint8       `json:"stream"`
	MaxDatagram int         `json:"maxDatagram"`
	MaxDelay    int64       `json:"maxDelay"`
	Steps       []jsEncStep `json:"steps"`
}

type jsDecStep struct {
	Op        string   `json:"op"`
	Pkt       string   `json:"pkt"`
	Protected bool     `json:"protected,omitempty"`
	Now       int64    `json:"now"`
	ParseErr  string   `json:"parseError,omitempty"`
	Out       []string `json:"out"`
	Recovered int      `json:"recovered"`
	Failed    int      `json:"failed"`
	Groups    int      `json:"groups"`
}

type jsDecoderTrace struct {
	Name   string      `json:"name"`
	Stream uint8       `json:"stream"`
	Steps  []jsDecStep `json:"steps"`
}

type jsFixture struct {
	Encode      []jsEncodeCase      `json:"encode"`
	Reconstruct []jsReconstructCase `json:"reconstruct"`
	Residual    []jsResidual        `json:"residual"`
	Choose      []jsChoose          `json:"choose"`
	ChooseVideo []jsChooseVideo     `json:"chooseVideo"`
	Encoders    []jsEncoderTrace    `json:"encoders"`
	Decoders    []jsDecoderTrace    `json:"decoders"`
}

func fillBlock(i, x, seed int) byte {
	return byte((i*131 + x*17 + seed*977 + ((i * x) >> 3)) & 255)
}

func makeBlocks(k, size, seed int) [][]byte {
	out := make([][]byte, k)
	for i := range out {
		out[i] = make([]byte, size)
		for x := range out[i] {
			out[i][x] = fillBlock(i, x, seed)
		}
	}
	return out
}

func hexes(bs [][]byte) []string {
	out := []string{}
	for _, b := range bs {
		out = append(out, hex.EncodeToString(b))
	}
	return out
}

func optHexes(bs [][]byte) []*string {
	out := make([]*string, len(bs))
	for i, b := range bs {
		if b != nil {
			s := hex.EncodeToString(b)
			out[i] = &s
		}
	}
	return out
}

func errName(err error) string {
	switch err {
	case nil:
		return ""
	case ErrTooFew:
		return "ErrTooFew"
	case ErrGeometry:
		return "ErrGeometry"
	case wire.ErrShort:
		return "ErrShort"
	case wire.ErrLong:
		return "ErrLong"
	case wire.ErrRepair:
		return "ErrRepair"
	case wire.ErrKind:
		return "ErrKind"
	case wire.ErrVersion:
		return "ErrVersion"
	}
	return err.Error()
}

var geometries = [][2]int{
	{1, 1}, {2, 1}, {3, 1}, {4, 1}, {5, 1}, {6, 1}, {7, 1}, {8, 1},
	{2, 2}, {3, 2}, {4, 2}, {8, 4}, {12, 3}, {16, 4}, {16, 6}, {24, 12}, {32, 8}, {32, 16}, {48, 16}, {200, 56},
}

func encodeCases() []jsEncodeCase {
	var out []jsEncodeCase
	for n, g := range geometries {
		size := 16 + n*2
		blocks := makeBlocks(g[0], size, n)
		out = append(out, jsEncodeCase{K: g[0], M: g[1], Size: size, Seed: n, Repairs: hexes(Encode(blocks, g[1]))})
	}
	return out
}

func reconstructCases() []jsReconstructCase {
	rng := rand.New(rand.NewPCG(3, 4))
	var out []jsReconstructCase
	run := func(name string, data, repair [][]byte) {
		c := jsReconstructCase{Name: name, Data: optHexes(data), Repair: optHexes(repair)}
		work := append([][]byte(nil), data...)
		if err := Reconstruct(work, repair); err != nil {
			c.Error = errName(err)
		} else {
			c.Result = hexes(work)
		}
		out = append(out, c)
	}
	for n, g := range geometries {
		k, m := g[0], g[1]
		if k > 48 {
			continue
		}
		size := 16 + n*2
		blocks := makeBlocks(k, size, n)
		repairs := Encode(blocks, m)
		trials := 4
		if k >= 24 {
			trials = 2
		}
		for trial := range trials {
			data := append([][]byte(nil), blocks...)
			rep := append([][]byte(nil), repairs...)
			lose := rng.IntN(m + 2)
			for _, i := range rng.Perm(k)[:min(lose, k)] {
				data[i] = nil
			}
			for _, j := range rng.Perm(m)[:rng.IntN(m+1)] {
				if trial%2 == 0 {
					rep[j] = nil
				}
			}
			run("geometry", data, rep)
		}
		if k+m <= 64 {
			data := append([][]byte(nil), blocks...)
			for i := range min(m, k) {
				data[k-1-i] = nil
			}
			run("first-k-repairs-last-data", data, repairs)
		}
	}
	b := makeBlocks(4, 30, 77)
	r := Encode(b, 2)
	run("too-few", [][]byte{nil, nil, nil, b[3]}, r)
	run("no-missing", b, r)
	run("mismatched-data", [][]byte{nil, b[1][:29], b[2], b[3]}, r)
	run("mismatched-repair", [][]byte{nil, b[1], b[2], b[3]}, [][]byte{r[0][:20], r[1]})
	run("repair-only-size", [][]byte{nil, nil}, [][]byte{r[0], r[1]})
	run("empty", nil, r)
	run("no-repair", b, nil)
	big := makeBlocks(250, 2, 5)
	run("too-large", append([][]byte{nil}, big[1:]...), Encode(big, 7))
	run("nil-repairs-skipped", [][]byte{nil, b[1], nil, b[3]}, [][]byte{nil, r[1], nil})
	return out
}

func policyCases() ([]jsResidual, []jsChoose, []jsChooseVideo) {
	var res []jsResidual
	for _, k := range []int{1, 2, 3, 8, 16, 48} {
		for _, m := range []int{1, 2, 4, 16} {
			for _, p := range []float64{0, 0.001, 0.01, 0.05, 0.1, 0.2, 0.5, 0.99, 1, 2} {
				for _, burst := range []float64{0, 1, 1.7, 3.3, 8} {
					res = append(res, jsResidual{K: k, M: m, P: p, Burst: burst, Value: Residual(k, m, p, burst)})
				}
			}
		}
	}
	var ch []jsChoose
	losses := []float64{0, 0.004, 0.005, 0.009, 0.01, 0.02, 0.03, 0.05, 0.08, 0.1, 0.15, 0.2, 0.3, 0.5, 0.7}
	bursts := []float64{0.5, 1, 1.4, 1.5, 2, 3.3, 5}
	for _, audio := range []bool{true, false} {
		for _, loss := range losses {
			for _, burst := range bursts {
				for _, maxK := range []int{0, 1, 2, 3, 8, 16, 48} {
					p := Choose(audio, loss, burst, maxK)
					ch = append(ch, jsChoose{Audio: audio, Loss: loss, Burst: burst, MaxK: maxK, K: p.K, M: p.M})
				}
			}
		}
	}
	var cv []jsChooseVideo
	for _, loss := range losses {
		for _, burst := range bursts {
			for _, ks := range [][2]int{{2, 2}, {8, 16}, {48, 48}, {16, 4}, {0, 100}} {
				i, o := ChooseVideo(loss, burst, ks[0], ks[1])
				cv = append(cv, jsChooseVideo{Loss: loss, Burst: burst, MaxKI: ks[0], MaxKO: ks[1], IK: i.K, IM: i.M, OK: o.K, OM: o.M})
			}
		}
	}
	return res, ch, cv
}

type encOp struct {
	op   string
	p, o Params
	on   bool
	f    float64
	pkt  []byte
	now  int64
}

func encState(e *Encoder) jsEncState {
	return jsEncState{K: e.Params().K, M: e.Params().M, Ratio: e.Ratio(0.6), Groups: e.Groups, Repairs: e.Repairs}
}

func runEncoder(name string, stream uint8, maxDatagram int, maxDelay int64, ops []encOp) (jsEncoderTrace, [][]byte, [][]byte) {
	e := NewEncoder(stream, maxDatagram, maxDelay)
	tr := jsEncoderTrace{Name: name, Stream: stream, MaxDatagram: maxDatagram, MaxDelay: maxDelay}
	var media, repairs [][]byte
	for _, op := range ops {
		st := jsEncStep{Op: op.op, Now: op.now, Out: []string{}}
		var out [][]byte
		switch op.op {
		case "setParams":
			st.K, st.M = op.p.K, op.p.M
			e.SetParams(op.p)
		case "setClass":
			st.K, st.M, st.OK, st.OM = op.p.K, op.p.M, op.o.K, op.o.M
			e.SetClassParams(op.p, op.o)
		case "unequal":
			st.On = op.on
			e.SetUnequal(op.on)
		case "scale":
			st.F = op.f
			e.Scale(op.f)
		case "add":
			pkt := append([]byte(nil), op.pkt...)
			st.Pkt = hex.EncodeToString(pkt)
			out = e.Add(pkt, op.now)
			st.FECOn = len(pkt) > 10 && wire.FlagsOf(pkt)&wire.FlagFEC != 0
			media = append(media, pkt)
		case "flush":
			out = e.Flush(op.now)
		}
		st.Out = hexes(out)
		repairs = append(repairs, out...)
		st.State = encState(e)
		tr.Steps = append(tr.Steps, st)
	}
	return tr, media, repairs
}

func videoPackets(n, maxPayload int, seed uint64) [][]byte {
	rng := rand.New(rand.NewPCG(seed, seed+1))
	p := frag.NewPacketizer(3, maxPayload)
	pattern := []int{0, 2, 1, 2}
	var out [][]byte
	for i := range n {
		data := make([]byte, 20+rng.IntN(maxPayload*3))
		for j := range data {
			data[j] = byte(rng.Uint32())
		}
		pkts, err := p.Packetize(data, i%30 == 0, pattern[i%4]*boolInt(i%30 != 0), false, uint32(i*33333))
		if err != nil {
			panic(err)
		}
		out = append(out, pkts...)
	}
	return out
}

func boolInt(b bool) int {
	if b {
		return 1
	}
	return 0
}

func audioPackets(n int) [][]byte {
	p := frag.NewPacketizer(1, 1162)
	var out [][]byte
	for i := range n {
		data := make([]byte, 40+i%30)
		for j := range data {
			data[j] = byte(i*7 + j)
		}
		pkts, _ := p.PacketizeFrame(data, frag.Opts{Independent: true, Ref: -1, Aux: 10}, uint32(i*20000))
		out = append(out, pkts...)
	}
	return out
}

func encoderCases() ([]jsEncoderTrace, [][][]byte) {
	var traces []jsEncoderTrace
	var streams [][][]byte
	add := func(tr jsEncoderTrace, media, repairs [][]byte) {
		traces = append(traces, tr)
		streams = append(streams, [][]byte{}, nil)
		streams[len(streams)-2] = media
		streams[len(streams)-1] = repairs
	}
	{
		pkts := audioPackets(60)
		ops := []encOp{{op: "add", pkt: pkts[0], now: 0}, {op: "setParams", p: Params{K: 2, M: 1}}}
		now := int64(0)
		for i, b := range pkts[1:] {
			now += 20000
			ops = append(ops, encOp{op: "add", pkt: b, now: now})
			switch i {
			case 15:
				ops = append(ops, encOp{op: "setParams", p: Params{K: 3, M: 6}})
			case 30:
				ops = append(ops, encOp{op: "scale", f: 0.4})
			case 40:
				ops = append(ops, encOp{op: "setParams", p: Params{K: 1, M: 1}})
			case 50:
				ops = append(ops, encOp{op: "setParams", p: Params{K: 99, M: 99}})
			case 52:
				ops = append(ops, encOp{op: "flush", now: now + 10000}, encOp{op: "flush", now: now + 60000})
			case 55:
				ops = append(ops, encOp{op: "scale", f: 0.01})
			}
		}
		tr, m, r := runEncoder("audio", 1, 1156, 60000, ops)
		add(tr, m, r)
	}
	{
		pkts := videoPackets(50, 300, 7)
		rng := rand.New(rand.NewPCG(8, 9))
		ops := []encOp{{op: "setClass", p: Params{K: 8, M: 3}, o: Params{K: 12, M: 2}}, {op: "unequal", on: true}}
		now := int64(0)
		for i := 0; i < len(pkts); i++ {
			now += int64(rng.IntN(4000))
			b := pkts[i]
			if i+1 < len(pkts) && rng.IntN(8) == 0 {
				ops = append(ops, encOp{op: "add", pkt: pkts[i+1], now: now})
				ops = append(ops, encOp{op: "add", pkt: b, now: now})
				i++
				continue
			}
			if rng.IntN(15) == 0 {
				continue
			}
			ops = append(ops, encOp{op: "add", pkt: b, now: now})
			if rng.IntN(20) == 0 {
				ops = append(ops, encOp{op: "add", pkt: b, now: now})
			}
			if rng.IntN(10) == 0 {
				ops = append(ops, encOp{op: "flush", now: now})
			}
			if i == 60 {
				jump := append([]byte(nil), b...)
				wire.SetSeq(jump, wire.SeqOf(b)+300)
				ops = append(ops, encOp{op: "add", pkt: jump, now: now})
			}
			if i == 90 {
				ops = append(ops, encOp{op: "scale", f: 0.5})
			}
			if i == 120 {
				ops = append(ops, encOp{op: "setClass", p: Params{K: 4, M: 4}, o: Params{}})
			}
			if i == 150 {
				ops = append(ops, encOp{op: "unequal", on: false})
			}
		}
		ops = append(ops, encOp{op: "flush", now: now + 49999}, encOp{op: "flush", now: now + 200000})
		tr, m, r := runEncoder("video-two-groups", 3, 1156, 50000, ops)
		add(tr, m, r)
	}
	{
		pkts := videoPackets(20, 250, 21)
		big := frag.NewPacketizer(3, 600)
		huge, _ := big.Packetize(make([]byte, 590), true, 0, false, 0)
		ops := []encOp{{op: "setParams", p: Params{K: 16, M: 4}}, {op: "add", pkt: huge[0], now: 1}, {op: "add", pkt: []byte{1, 2, 3}, now: 1}}
		for i, b := range pkts {
			ops = append(ops, encOp{op: "add", pkt: b, now: int64(i * 3000)})
		}
		ops = append(ops, encOp{op: "flush", now: 10_000_000})
		tr, m, r := runEncoder("small-datagram", 3, 320, 50000, ops)
		add(tr, m, r)
	}
	return traces, streams
}

type decIn struct {
	repair    bool
	pkt       []byte
	protected bool
	now       int64
}

func runDecoder(name string, stream uint8, ins []decIn) jsDecoderTrace {
	d := NewDecoder(stream)
	tr := jsDecoderTrace{Name: name, Stream: stream}
	for _, in := range ins {
		st := jsDecStep{Pkt: hex.EncodeToString(in.pkt), Now: in.now, Out: []string{}}
		var out [][]byte
		if in.repair {
			st.Op = "repair"
			r, err := wire.ParseRepair(in.pkt)
			if err != nil {
				st.ParseErr = errName(err)
			} else {
				out = d.AddRepair(r, in.now)
			}
		} else {
			st.Op = "media"
			st.Protected = in.protected
			out = d.AddMedia(append([]byte(nil), in.pkt...), in.protected, in.now)
		}
		st.Out = hexes(out)
		st.Recovered, st.Failed, st.Groups = d.Recovered, d.Failed, len(d.groups)
		tr.Steps = append(tr.Steps, st)
	}
	return tr
}

func decoderCases(streams [][][]byte) []jsDecoderTrace {
	var out []jsDecoderTrace
	rng := rand.New(rand.NewPCG(31, 32))
	for n := 0; n+1 < len(streams); n += 2 {
		media, repairs := streams[n], streams[n+1]
		var ins []decIn
		for _, b := range media {
			if len(b) < wire.MediaHeaderLen {
				continue
			}
			ins = append(ins, decIn{pkt: b, protected: wire.FlagsOf(b)&wire.FlagFEC != 0})
		}
		for _, b := range repairs {
			ins = append(ins, decIn{repair: true, pkt: b})
		}
		rng.Shuffle(len(ins), func(i, j int) {
			if j > i-12 && j < i+12 {
				ins[i], ins[j] = ins[j], ins[i]
			}
		})
		var kept []decIn
		now := int64(0)
		for _, in := range ins {
			now += int64(rng.IntN(5000))
			in.now = now
			if rng.IntN(6) == 0 {
				continue
			}
			kept = append(kept, in)
			if rng.IntN(25) == 0 {
				kept = append(kept, in)
			}
		}
		stream := uint8(3)
		if n == 0 {
			stream = 1
		}
		out = append(out, runDecoder("shuffled-lossy-"+traceNames[n/2], stream, kept))
	}
	var special []decIn
	pkts := videoPackets(12, 200, 41)
	enc := NewEncoder(3, 1156, 50000)
	enc.SetParams(Params{K: 4, M: 2})
	var groups [][][]byte
	var cur [][]byte
	for _, b := range pkts {
		cur = append(cur, b)
		if rs := enc.Add(b, 0); len(rs) > 0 {
			groups = append(groups, append(append([][]byte(nil), cur...), rs...))
			cur = nil
		}
	}
	t := int64(0)
	for gi, g := range groups {
		k := len(g) - 2
		for i := 0; i < len(g); i++ {
			t += 100
			isRepair := i >= k
			switch gi % 5 {
			case 0:
				if i == 0 || i == 1 {
					continue
				}
			case 1:
				if i == 0 {
					special = append(special, decIn{pkt: g[i], protected: false, now: t})
					continue
				}
			case 2:
				if isRepair && i == k {
					bad := append([]byte(nil), g[i]...)
					for x := wire.RepairHeaderLen + k; x < len(bad); x++ {
						bad[x] ^= byte(x * 13)
					}
					special = append(special, decIn{repair: true, pkt: bad, now: t})
					continue
				}
				if i == 1 || isRepair {
					continue
				}
			case 3:
				if i == 2 {
					longer := append(append([]byte(nil), g[i]...), make([]byte, 40)...)
					special = append(special, decIn{pkt: longer, protected: true, now: t})
					continue
				}
				if i == 0 {
					continue
				}
			}
			special = append(special, decIn{repair: isRepair, pkt: g[i], protected: !isRepair, now: t})
		}
	}
	special = append(special, decIn{repair: true, pkt: []byte{0x11, 3, 0, 0, 0, 0, 0, 1, 1, 0, 0}, now: t})
	special = append(special, decIn{repair: true, pkt: []byte{0x11, 3, 0, 0, 0, 0, 0, 2, 1, 0, 0, 0}, now: t})
	for i := range 70 {
		sym := make([]byte, 30)
		for x := range sym {
			sym[x] = byte(i + x)
		}
		r := wire.Repair{Stream: 3, BaseSeq: uint16(60000 + i*8), Scheme: wire.SchemeXOR, K: 2, M: 1, Offsets: []uint8{0, 1}, Symbol: sym}
		special = append(special, decIn{repair: true, pkt: r.Append(nil), now: t + int64(i)*10000})
	}
	special = append(special, decIn{repair: true, pkt: (&wire.Repair{Stream: 3, BaseSeq: 1, Scheme: wire.SchemeRS, K: 2, M: 2, Index: 1, Offsets: []uint8{0, 3}, Symbol: make([]byte, 30)}).Append(nil), now: t + 3_000_000})
	out = append(out, runDecoder("special", 3, special))
	return out
}

var traceNames = []string{"audio", "video-two-groups", "small-datagram"}

func buildJSFixture() []byte {
	f := jsFixture{Encode: encodeCases(), Reconstruct: reconstructCases()}
	f.Residual, f.Choose, f.ChooseVideo = policyCases()
	var streams [][][]byte
	f.Encoders, streams = encoderCases()
	f.Decoders = decoderCases(streams)
	b, err := json.Marshal(f)
	if err != nil {
		panic(err)
	}
	return append(b, '\n')
}

func TestJSParity(t *testing.T) {
	got := buildJSFixture()
	if !bytes.Equal(got, buildJSFixture()) {
		t.Fatal("fixture is not deterministic")
	}
	path := filepath.Join("testdata", "js-parity.json")
	if os.Getenv("FMM_JS_PARITY_WRITE") == "1" {
		if err := os.MkdirAll("testdata", 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(path, got, 0o644); err != nil {
			t.Fatal(err)
		}
		return
	}
	want, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("%v (write it with FMM_JS_PARITY_WRITE=1)", err)
	}
	if !bytes.Equal(got, want) {
		t.Fatal("testdata/js-parity.json is stale; regenerate with FMM_JS_PARITY_WRITE=1")
	}
}
