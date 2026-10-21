package frag

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"math/rand/v2"
	"os"
	"path/filepath"
	"testing"

	"forkmesh.com/mirror-node/rtc/fmm/wire"
)

type jsOpts struct {
	Keyframe    bool  `json:"keyframe"`
	Layer       int   `json:"layer"`
	Independent bool  `json:"independent"`
	Ref         int64 `json:"ref"`
	LongTerm    bool  `json:"longTerm"`
	Face        bool  `json:"face"`
	Aux         uint8 `json:"aux"`
}

type jsPacket struct {
	Header string `json:"header"`
	Len    int    `json:"len"`
	Sum    string `json:"sum"`
}

type jsPacketizeStep struct {
	Size    int        `json:"size"`
	Seed    int        `json:"seed"`
	Simple  bool       `json:"simple"`
	Opts    jsOpts     `json:"opts"`
	TS      uint32     `json:"ts"`
	Error   string     `json:"error,omitempty"`
	Packets []jsPacket `json:"packets"`
	LastID  int64      `json:"lastId"`
	NextID  int64      `json:"nextId"`
}

type jsPacketizeCase struct {
	Name       string            `json:"name"`
	Stream     uint8             `json:"stream"`
	MaxPayload int               `json:"maxPayload"`
	MaxFrame   int               `json:"maxFrame"`
	Steps      []jsPacketizeStep `json:"steps"`
}

type jsLongRun struct {
	Stream     uint8  `json:"stream"`
	MaxPayload int    `json:"maxPayload"`
	Frames     int    `json:"frames"`
	Pattern    []int  `json:"pattern"`
	Sum        string `json:"sum"`
	LastID     int64  `json:"lastId"`
}

type jsFrame struct {
	ID           int64  `json:"id"`
	Keyframe     bool   `json:"keyframe"`
	Recovery     bool   `json:"recovery"`
	LongTerm     bool   `json:"longTerm"`
	Face         bool   `json:"face"`
	Layer        int    `json:"layer"`
	Ref          int64  `json:"ref"`
	Aux          int    `json:"aux"`
	Timestamp    int64  `json:"timestamp"`
	FirstSeq     int64  `json:"firstSeq"`
	LastSeq      int64  `json:"lastSeq"`
	Data         string `json:"data"`
	FirstArrival int64  `json:"firstArrival"`
	Complete     int64  `json:"complete"`
	Recovered    int    `json:"recovered"`
}

type jsLost struct {
	ID       int64 `json:"id"`
	Keyframe bool  `json:"keyframe"`
	Layer    int   `json:"layer"`
}

type jsCounters struct {
	Completed  int `json:"completed"`
	Duplicates int `json:"duplicates"`
	Corrupt    int `json:"corrupt"`
	Expired    int `json:"expired"`
	Bytes      int `json:"bytes"`
	Partials   int `json:"partials"`
}

type jsReassemblyStep struct {
	Op        string     `json:"op"`
	Packet    string     `json:"packet,omitempty"`
	Recovered bool       `json:"recovered,omitempty"`
	Now       int64      `json:"now"`
	ParseErr  string     `json:"parseError,omitempty"`
	Frame     *jsFrame   `json:"frame,omitempty"`
	Lost      []jsLost   `json:"lost,omitempty"`
	Counters  jsCounters `json:"counters"`
}

type jsReassembly struct {
	Name    string             `json:"name"`
	Timeout int64              `json:"timeout"`
	Steps   []jsReassemblyStep `json:"steps"`
}

type jsFixture struct {
	Packetize  []jsPacketizeCase `json:"packetize"`
	LongRuns   []jsLongRun       `json:"longRuns"`
	Reassembly []jsReassembly    `json:"reassembly"`
}

func fillData(n, seed int) []byte {
	b := make([]byte, n)
	for i := range b {
		b[i] = byte((i*131 + seed*977 + (i>>3)*29) & 255)
	}
	return b
}

func errName(err error) string {
	switch err {
	case nil:
		return ""
	case ErrTooLarge:
		return "ErrTooLarge"
	case ErrRef:
		return "ErrRef"
	case ErrLayer:
		return "ErrLayer"
	}
	for name, e := range map[string]error{
		"ErrShort": wire.ErrShort, "ErrLong": wire.ErrLong, "ErrVersion": wire.ErrVersion, "ErrKind": wire.ErrKind,
		"ErrFragment": wire.ErrFragment, "ErrFlags": wire.ErrFlags,
	} {
		if err == e {
			return name
		}
	}
	return err.Error()
}

func packetSummary(p []byte) jsPacket {
	s := sha256.Sum256(p)
	return jsPacket{Header: hex.EncodeToString(p[:wire.MediaHeaderLen]), Len: len(p), Sum: hex.EncodeToString(s[:])}
}

type pstep struct {
	size, seed int
	simple     bool
	o          Opts
	ts         uint32
}

func runPacketize(name string, stream uint8, maxPayload int, steps []pstep) jsPacketizeCase {
	p := NewPacketizer(stream, maxPayload)
	c := jsPacketizeCase{Name: name, Stream: stream, MaxPayload: maxPayload, MaxFrame: p.MaxFrame()}
	for _, s := range steps {
		data := fillData(s.size, s.seed)
		var pkts [][]byte
		var err error
		if s.simple {
			pkts, err = p.Packetize(data, s.o.Keyframe, s.o.Layer, s.o.Independent, s.ts)
		} else {
			pkts, err = p.PacketizeFrame(data, s.o, s.ts)
		}
		st := jsPacketizeStep{Size: s.size, Seed: s.seed, Simple: s.simple, TS: s.ts, Error: errName(err),
			Opts:    jsOpts{Keyframe: s.o.Keyframe, Layer: s.o.Layer, Independent: s.o.Independent, Ref: s.o.Ref, LongTerm: s.o.LongTerm, Face: s.o.Face, Aux: s.o.Aux},
			Packets: []jsPacket{}}
		for _, b := range pkts {
			st.Packets = append(st.Packets, packetSummary(b))
		}
		st.LastID, st.NextID = p.LastID(), p.NextID()
		c.Steps = append(c.Steps, st)
	}
	return c
}

func vid(size, seed int, layer int, key bool, ts uint32) pstep {
	return pstep{size: size, seed: seed, o: Opts{Keyframe: key, Layer: layer, Ref: -1}, ts: ts}
}

func packetizeCases() []jsPacketizeCase {
	var out []jsPacketizeCase
	var l1t3 []pstep
	pattern := []int{0, 2, 1, 2}
	for i := range 24 {
		l1t3 = append(l1t3, vid(300+i*97, i, pattern[i%4], i == 0 || i == 16, uint32(i)*33333))
	}
	out = append(out, runPacketize("l1t3", 3, 1162, l1t3))
	var l1t2 []pstep
	for i := range 12 {
		l1t2 = append(l1t2, pstep{size: 50 + i*400, seed: 100 + i, simple: true, o: Opts{Keyframe: i == 0, Layer: i % 2}, ts: 0xFFFFFF00 + uint32(i)*40})
	}
	out = append(out, runPacketize("l1t2-simple-wrapping-ts", 7, 500, l1t2))
	out = append(out, runPacketize("errors-before-keyframe", 1, 100, []pstep{
		vid(10, 1, 0, false, 0),
		vid(10, 1, 1, false, 0),
		{size: 10, seed: 1, o: Opts{Ref: 0}, ts: 0},
		vid(10, 1, 8, false, 0),
		vid(10, 1, -1, false, 0),
		vid(10, 1, 1, true, 0),
		{size: 10, seed: 1, o: Opts{Keyframe: true, Ref: 2}, ts: 0},
		vid(25501, 1, 0, true, 0),
		vid(25500, 2, 0, true, 0),
		vid(0, 3, 0, false, 10),
		{size: 5, seed: 4, o: Opts{Independent: true, Ref: 0}, ts: 0},
		{size: 5, seed: 4, o: Opts{Layer: 1, Ref: 0}, ts: 0},
	}))
	var rec []pstep
	rec = append(rec, pstep{size: 2000, seed: 9, o: Opts{Keyframe: true, Ref: -1, LongTerm: true}, ts: 1})
	for i := 1; i < 10; i++ {
		rec = append(rec, pstep{size: 400 + i, seed: 9 + i, o: Opts{Layer: i % 3, Ref: -1, LongTerm: i%3 == 0}, ts: uint32(i * 1000)})
	}
	rec = append(rec,
		pstep{size: 900, seed: 30, o: Opts{Ref: 3, LongTerm: true}, ts: 20000},
		pstep{size: 300, seed: 31, o: Opts{Layer: 2, Ref: -1}, ts: 21000},
		pstep{size: 300, seed: 32, o: Opts{Layer: 1, Ref: -1}, ts: 22000},
		pstep{size: 300, seed: 33, o: Opts{Ref: 12}, ts: 23000},
		pstep{size: 300, seed: 34, o: Opts{Ref: 20}, ts: 23000},
		pstep{size: 300, seed: 35, o: Opts{Ref: 0, Face: true}, ts: 24000},
		pstep{size: 64, seed: 36, o: Opts{Independent: true, Face: true, Ref: -1}, ts: 25000},
		pstep{size: 3000, seed: 37, o: Opts{Keyframe: true, Face: true, Ref: -1}, ts: 26000},
	)
	for i := range 260 {
		rec = append(rec, pstep{size: 20, seed: i, o: Opts{Layer: 1, Ref: -1}, ts: uint32(27000 + i)})
	}
	rec = append(rec,
		pstep{size: 20, seed: 1, o: Opts{Layer: 2, Ref: -1}, ts: 30000},
		pstep{size: 20, seed: 1, o: Opts{Layer: 1, Ref: -1}, ts: 30001},
		pstep{size: 20, seed: 1, o: Opts{Ref: -1}, ts: 30002},
		pstep{size: 20, seed: 2, o: Opts{Ref: 0}, ts: 30003},
	)
	out = append(out, runPacketize("recovery-longterm-face", 9, 1162, rec))
	var audio []pstep
	for i := range 30 {
		audio = append(audio, pstep{size: 60 + i%7, seed: i, o: Opts{Independent: true, Aux: uint8(i * 9), Ref: -1}, ts: uint32(i * 20000)})
	}
	audio = append(audio, pstep{size: 60, seed: 99, o: Opts{Independent: true, Keyframe: true, Aux: 50, Ref: -1}, ts: 1})
	audio = append(audio, pstep{size: 60, seed: 98, simple: true, o: Opts{Independent: true}, ts: 2})
	out = append(out, runPacketize("audio-independent-aux", 2, 1162, audio))
	out = append(out, runPacketize("tiny-payload", 4, 0, []pstep{vid(5, 1, 0, true, 0), vid(255, 2, 0, false, 1), vid(256, 3, 0, false, 2)}))
	return out
}

func longRuns() []jsLongRun {
	var out []jsLongRun
	for _, lr := range []jsLongRun{
		{Stream: 5, MaxPayload: 40, Frames: 70000, Pattern: []int{0, 2, 1, 2}},
		{Stream: 6, MaxPayload: 1162, Frames: 66000, Pattern: []int{0}},
	} {
		p := NewPacketizer(lr.Stream, lr.MaxPayload)
		h := sha256.New()
		for i := range lr.Frames {
			layer := lr.Pattern[i%len(lr.Pattern)]
			pkts, err := p.Packetize(fillData(1+i%97, i), i%5000 == 0, layer, false, uint32(i)*33333)
			if err != nil {
				panic(err)
			}
			for _, b := range pkts {
				h.Write(b)
			}
		}
		lr.Sum = hex.EncodeToString(h.Sum(nil))
		lr.LastID = p.LastID()
		out = append(out, lr)
	}
	return out
}

type rinput struct {
	pkt       []byte
	recovered bool
	now       int64
	expire    bool
}

func counters(r *Reassembler) jsCounters {
	return jsCounters{Completed: r.Completed, Duplicates: r.Duplicates, Corrupt: r.Corrupt, Expired: r.Expired, Bytes: r.bytes, Partials: len(r.order)}
}

func runReassembly(name string, timeout int64, inputs []rinput) jsReassembly {
	r := NewReassembler()
	if timeout > 0 {
		r.Timeout = timeout
	}
	out := jsReassembly{Name: name, Timeout: r.Timeout}
	for _, in := range inputs {
		st := jsReassemblyStep{Now: in.now}
		if in.expire {
			st.Op = "expire"
			for _, l := range r.Expire(in.now) {
				st.Lost = append(st.Lost, jsLost{ID: l.ID, Keyframe: l.Keyframe, Layer: l.Layer})
			}
		} else {
			st.Op = "add"
			st.Packet = hex.EncodeToString(in.pkt)
			st.Recovered = in.recovered
			m, err := wire.ParseMedia(in.pkt)
			if err != nil {
				st.ParseErr = errName(err)
			} else if f := r.Add(&m, in.recovered, in.now); f != nil {
				st.Frame = &jsFrame{ID: f.ID, Keyframe: f.Keyframe, Recovery: f.Recovery, LongTerm: f.LongTerm, Face: f.Face,
					Layer: f.Layer, Ref: f.Ref, Aux: f.Aux, Timestamp: f.Timestamp, FirstSeq: f.FirstSeq, LastSeq: f.LastSeq,
					Data: hex.EncodeToString(f.Data), FirstArrival: f.FirstArrival, Complete: f.Complete, Recovered: f.Recovered}
			}
		}
		st.Counters = counters(r)
		out.Steps = append(out.Steps, st)
	}
	return out
}

func mediaPkt(m wire.Media) []byte { return m.Append(nil) }

func reassemblyCases() []jsReassembly {
	var out []jsReassembly
	rng := rand.New(rand.NewPCG(11, 12))
	p := NewPacketizer(4, 60)
	var pkts [][]byte
	pattern := []int{0, 2, 1, 2}
	for i := range 40 {
		var o Opts
		switch {
		case i == 0:
			o = Opts{Keyframe: true, Ref: -1, LongTerm: true}
		case i == 21:
			o = Opts{Ref: 16, LongTerm: true}
		case i == 30:
			o = Opts{Independent: true, Face: true, Ref: -1}
		default:
			o = Opts{Layer: pattern[i%4], Ref: -1, LongTerm: pattern[i%4] == 0}
		}
		b, err := p.PacketizeFrame(fillData(1+rng.IntN(400), i), o, uint32(i)*33333+0xFFFF0000)
		if err != nil {
			panic(err)
		}
		pkts = append(pkts, b...)
	}
	var shuffled []rinput
	now := int64(0)
	for i, b := range pkts {
		if rng.IntN(12) == 0 {
			continue
		}
		shuffled = append(shuffled, rinput{pkt: b, recovered: i%9 == 4})
		if rng.IntN(6) == 0 {
			shuffled = append(shuffled, rinput{pkt: b})
		}
	}
	rng.Shuffle(len(shuffled), func(i, j int) {
		if j > i-6 && j < i+6 {
			shuffled[i], shuffled[j] = shuffled[j], shuffled[i]
		}
	})
	var inputs []rinput
	for i, in := range shuffled {
		now += int64(rng.IntN(3000))
		in.now = now
		inputs = append(inputs, in)
		if i%25 == 24 {
			inputs = append(inputs, rinput{expire: true, now: now})
		}
	}
	inputs = append(inputs, rinput{expire: true, now: now + 500_000}, rinput{expire: true, now: now + 3_000_000})
	for _, b := range pkts[:10] {
		inputs = append(inputs, rinput{pkt: b, now: now + 3_000_001})
	}
	out = append(out, runReassembly("shuffle-drop-duplicate-expire", 0, inputs))

	base := wire.Media{Stream: 1, Seq: 100, FrameID: 7, FragIndex: 0, FragCount: 3, Layer: 1, RefDelta: 1, Timestamp: 5, Payload: []byte{1, 2, 3}}
	with := func(f func(*wire.Media)) []byte {
		m := base
		f(&m)
		return mediaPkt(m)
	}
	key := wire.Media{Stream: 1, Seq: 90, FrameID: 6, FragCount: 1, Keyframe: true, Timestamp: 4, Payload: []byte{9}}
	bad := []rinput{
		{pkt: mediaPkt(key), now: 1},
		{pkt: with(func(m *wire.Media) {}), now: 2},
		{pkt: with(func(m *wire.Media) { m.FragIndex, m.Seq, m.FragCount = 1, 101, 4 }), now: 3},
		{pkt: with(func(m *wire.Media) {}), now: 4},
		{pkt: with(func(m *wire.Media) { m.FragIndex, m.Seq, m.LongTerm = 1, 101, true }), now: 5},
		{pkt: with(func(m *wire.Media) {}), now: 6},
		{pkt: with(func(m *wire.Media) { m.FragIndex, m.Seq, m.Layer = 1, 101, 2 }), now: 7},
		{pkt: with(func(m *wire.Media) {}), now: 8},
		{pkt: with(func(m *wire.Media) { m.FragIndex, m.Seq, m.RefDelta = 1, 101, 2 }), now: 9},
		{pkt: with(func(m *wire.Media) {}), now: 10},
		{pkt: with(func(m *wire.Media) { m.FragIndex, m.Seq = 1, 102 }), now: 11},
		{pkt: with(func(m *wire.Media) {}), now: 12},
		{pkt: with(func(m *wire.Media) { m.FragIndex, m.Seq, m.Face = 1, 101, true }), now: 13},
		{pkt: with(func(m *wire.Media) {}), now: 14},
		{pkt: with(func(m *wire.Media) { m.FragIndex, m.Seq, m.Recovery, m.Layer = 1, 101, true, 0 }), now: 15},
		{pkt: with(func(m *wire.Media) {}), now: 16},
		{pkt: with(func(m *wire.Media) { m.FragIndex, m.Seq, m.Payload = 1, 101, nil }), now: 17},
		{pkt: with(func(m *wire.Media) { m.FragIndex, m.Seq, m.Payload = 1, 101, nil }), now: 18},
		{pkt: with(func(m *wire.Media) { m.FragIndex, m.Seq, m.Payload = 1, 101, nil }), now: 19},
		{pkt: with(func(m *wire.Media) { m.FragIndex, m.Seq, m.Payload = 1, 101, []byte{4} }), now: 20},
		{pkt: with(func(m *wire.Media) { m.FragIndex, m.Seq = 2, 102 }), now: 21},
		{pkt: mediaPkt(wire.Media{Stream: 1, Seq: 200, FrameID: 9, FragCount: 1, Layer: 0, RefDelta: 3, Timestamp: 6}), now: 22},
		{pkt: mediaPkt(wire.Media{Stream: 1, Seq: 201, FrameID: 9, FragCount: 1, Layer: 0, RefDelta: 3, Timestamp: 6, Payload: []byte{1}}), now: 23},
		{pkt: []byte{0x10, 1, 0}, now: 24},
		{pkt: with(func(m *wire.Media) { m.FragIndex = 3 }), now: 25},
		{pkt: mediaPkt(wire.Media{Stream: 1, Seq: 300, FrameID: 10, FragCount: 2, Keyframe: true, Timestamp: 6, Payload: []byte{1}}), recovered: true, now: 26},
		{pkt: mediaPkt(wire.Media{Stream: 1, Seq: 301, FrameID: 10, FragIndex: 1, FragCount: 2, Keyframe: true, Timestamp: 6, Payload: []byte{2, 3}}), recovered: true, now: 27},
		{expire: true, now: 1_000_005},
		{expire: true, now: 1_000_006},
		{expire: true, now: 9_000_000},
	}
	out = append(out, runReassembly("disagreeing-empty-corrupt", 0, bad))

	var evict []rinput
	for i := range 110 {
		id := uint16(65500 + i*2)
		seq := uint16(65000 + i*3)
		evict = append(evict, rinput{pkt: mediaPkt(wire.Media{Stream: 2, Seq: seq, FrameID: id, FragIndex: 0, FragCount: 2, Layer: 0, RefDelta: 2, Timestamp: uint32(0xFFFFFFF0) + uint32(i*7), Payload: fillData(5+i, i)}), now: int64(i * 1000)})
		if i%3 == 0 {
			evict = append(evict, rinput{pkt: mediaPkt(wire.Media{Stream: 2, Seq: seq + 1, FrameID: id, FragIndex: 1, FragCount: 2, Layer: 0, RefDelta: 2, Timestamp: uint32(0xFFFFFFF0) + uint32(i*7), Payload: fillData(4, i)}), now: int64(i*1000 + 500)})
		}
		if i%50 == 49 {
			evict = append(evict, rinput{expire: true, now: int64(i*1000 + 1)})
		}
	}
	for i := range 110 {
		id := uint16(65500 + i*2)
		seq := uint16(65000 + i*3 + 1)
		evict = append(evict, rinput{pkt: mediaPkt(wire.Media{Stream: 2, Seq: seq, FrameID: id, FragIndex: 1, FragCount: 2, Layer: 0, RefDelta: 2, Timestamp: uint32(0xFFFFFFF0) + uint32(i*7), Payload: fillData(3, i)}), now: int64(110000 + i*500)})
	}
	evict = append(evict, rinput{expire: true, now: 170_000}, rinput{expire: true, now: 300_000})
	out = append(out, runReassembly("eviction-wrapping-ids-short-timeout", 120_000, evict))
	return out
}

func buildJSFixture() []byte {
	f := jsFixture{Packetize: packetizeCases(), LongRuns: longRuns(), Reassembly: reassemblyCases()}
	b, err := json.MarshalIndent(f, "", " ")
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
