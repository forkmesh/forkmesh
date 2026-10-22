package jitter

import (
	"bytes"
	"encoding/json"
	"math/rand/v2"
	"os"
	"path/filepath"
	"testing"

	"forkmesh.com/mirror-node/rtc/fmm/frag"
)

const (
	opPush = iota
	opPull
	opTarget
)

const (
	vPush = iota
	vPop
	vLost
	vTiming
	vRecovery
	vKeyframe
	vAcks
	vStep
)

type audioTrace struct {
	Name   string      `json:"name"`
	Config AudioConfig `json:"config"`
	Ops    [][]int64   `json:"ops"`
	Stats  AudioStats  `json:"stats"`
}

type videoTrace struct {
	Name   string      `json:"name"`
	Config VideoConfig `json:"config"`
	Ops    [][]int64   `json:"ops"`
	Stats  VideoStats  `json:"stats"`
}

type parityFixture struct {
	Audio []audioTrace `json:"audio"`
	Video []videoTrace `json:"video"`
}

func b2i(b bool) int64 {
	if b {
		return 1
	}
	return 0
}

type audioRecorder struct {
	a      *Audio
	tr     audioTrace
	target int64
	extra  int64
}

func newAudioRecorder(name string, cfg AudioConfig) *audioRecorder {
	return &audioRecorder{a: NewAudio(cfg), tr: audioTrace{Name: name, Config: cfg}, target: -1, extra: -1}
}

func audioData(id int64) []byte { return []byte{byte(id), byte(id >> 8)} }

func dataTag(d []byte) int64 {
	if d == nil {
		return -1
	}
	return int64(d[0]) | int64(d[1])<<8
}

func (r *audioRecorder) push(id, ts, depth, arrival int64) {
	r.a.PushDepth(id, ts, audioData(id), depth, arrival)
	r.tr.Ops = append(r.tr.Ops, []int64{opPush, id, ts, depth, arrival})
}

func (r *audioRecorder) pull(now int64) AudioOut {
	o := r.a.PullOut(now)
	r.tr.Ops = append(r.tr.Ops, []int64{opPull, now, int64(o.Status), dataTag(o.Data), int64(o.Offset)})
	if r.a.TargetUs() != r.target || r.a.ExtraUs() != r.extra {
		r.target, r.extra = r.a.TargetUs(), r.a.ExtraUs()
		r.tr.Ops = append(r.tr.Ops, []int64{opTarget, r.target, r.extra})
	}
	return o
}

func (r *audioRecorder) done() audioTrace {
	r.tr.Stats = r.a.Stats
	return r.tr
}

func audioJitterTrace(name string, frames int, loss float64, delay func(i int, r *rand.Rand) int64) audioTrace {
	r := rand.New(rand.NewPCG(9, 9))
	rec := newAudioRecorder(name, AudioConfig{})
	type arrival struct {
		at int64
		id int
	}
	var arr []arrival
	for i := range frames {
		if r.Float64() < loss {
			continue
		}
		arr = append(arr, arrival{at: int64(i)*FrameUs + delay(i, r), id: i})
	}
	next := 0
	for t := int64(0); rec.a.next < int64(frames) && t < int64(frames)*FrameUs+2_000_000; t += FrameUs {
		for {
			best := -1
			for j := next; j < len(arr) && j < next+64; j++ {
				if arr[j].at <= t && (best < 0 || arr[j].at < arr[best].at) {
					best = j
				}
			}
			if best < 0 {
				break
			}
			rec.push(int64(arr[best].id), int64(arr[best].id)*FrameUs, 0, arr[best].at)
			arr[best].at = 1 << 62
			for next < len(arr) && arr[next].at == 1<<62 {
				next++
			}
		}
		rec.pull(t + 3_000)
	}
	return rec.done()
}

func audioLossTrace(name string, cfg AudioConfig, frames int, depth int64, lose func(r *rand.Rand) bool) audioTrace {
	r := rand.New(rand.NewPCG(5, 5))
	rec := newAudioRecorder(name, cfg)
	for t := int64(0); t < int64(frames)*FrameUs; t += FrameUs {
		i := int64(t / FrameUs)
		if !lose(r) {
			rec.push(i, t, depth, t+20_000+r.Int64N(5_000))
		}
		rec.pull(t + 3_000)
	}
	return rec.done()
}

func audioFuzzTrace(name string, seed uint64, cfg AudioConfig, ops int) audioTrace {
	r := rand.New(rand.NewPCG(seed, seed+1))
	rec := newAudioRecorder(name, cfg)
	now := int64(0)
	id := int64(100)
	for range ops {
		switch k := r.IntN(20); {
		case k < 9:
			x := id
			switch r.IntN(10) {
			case 0:
				x = id - r.Int64N(8)
			case 1:
				x = id + r.Int64N(6)
			case 2:
				if r.IntN(15) == 0 {
					x = id + 200 + r.Int64N(100)
				}
			}
			depth := []int64{0, 0, 40_000, 100_000, 1_000_000}[r.IntN(5)]
			rec.push(x, x*FrameUs+7, depth, now+r.Int64N(30_000))
			if x >= id {
				id = x + 1
			}
		case k < 10:
			for range 1 + r.IntN(40) {
				rec.push(id, id*FrameUs+7, 0, now)
				id++
			}
		default:
			now += []int64{FrameUs, FrameUs, FrameUs, 5_000, 45_000, 400_000}[r.IntN(6)]
			rec.pull(now)
		}
	}
	return rec.done()
}

func audioUnitTrace() audioTrace {
	rec := newAudioRecorder("conceal hands over the next packet", AudioConfig{StartDelayUs: 20_000, MinDelayUs: 20_000})
	rec.push(0, 0, 0, 0)
	rec.push(2, 40_000, 0, 40_000)
	rec.pull(10_000)
	rec.pull(30_000)
	rec.pull(50_000)
	rec.pull(70_000)
	rec.push(1, 20_000, 0, 80_000)
	rec.pull(90_000)
	return rec.done()
}

type videoRecorder struct {
	v  *Video
	tr videoTrace
}

func newVideoRecorder(name string, cfg VideoConfig) *videoRecorder {
	return &videoRecorder{v: NewVideo(cfg), tr: videoTrace{Name: name, Config: cfg}}
}

func (r *videoRecorder) push(f *frag.Frame, now int64) {
	r.v.Push(f, now)
	r.tr.Ops = append(r.tr.Ops, []int64{vPush, f.ID, b2i(f.Keyframe), b2i(f.Recovery), b2i(f.LongTerm), b2i(f.Face),
		int64(f.Layer), f.Ref, f.Timestamp, f.FirstSeq, f.LastSeq, f.Complete, now})
}

func (r *videoRecorder) pop(now int64) *frag.Frame {
	f := r.v.Pop(now)
	id := int64(-1)
	if f != nil {
		id = f.ID
	}
	r.tr.Ops = append(r.tr.Ops, []int64{vPop, now, id, r.v.WaitUs(), r.v.LastID()})
	return f
}

func (r *videoRecorder) lost(l frag.Lost, now int64) {
	r.v.Lost(l, now)
	r.tr.Ops = append(r.tr.Ops, []int64{vLost, l.ID, b2i(l.Keyframe), int64(l.Layer), now})
}

func (r *videoRecorder) timing(rtt int64) {
	r.v.SetTiming(rtt)
	r.tr.Ops = append(r.tr.Ops, []int64{vTiming, rtt})
}

func (r *videoRecorder) recovery(now int64) (bool, int64) {
	ok, stuck := r.v.RecoveryNeeded(now)
	r.tr.Ops = append(r.tr.Ops, []int64{vRecovery, now, b2i(ok), stuck})
	return ok, stuck
}

func (r *videoRecorder) keyframe(now int64) bool {
	ok := r.v.KeyframeNeeded(now)
	r.tr.Ops = append(r.tr.Ops, []int64{vKeyframe, now, b2i(ok)})
	return ok
}

func (r *videoRecorder) acks() []int64 {
	a := r.v.TakeAcks()
	r.tr.Ops = append(r.tr.Ops, append([]int64{vAcks}, a...))
	return a
}

func (r *videoRecorder) step(now int64) (bool, bool) {
	var ids []int64
	for {
		f := r.v.Pop(now)
		if f == nil {
			break
		}
		ids = append(ids, f.ID)
	}
	ok, stuck := r.v.RecoveryNeeded(now)
	key := r.v.KeyframeNeeded(now)
	op := []int64{vStep, now, b2i(ok), stuck, b2i(key), r.v.WaitUs(), r.v.LastID()}
	r.tr.Ops = append(r.tr.Ops, append(op, ids...))
	return ok, key
}

func (r *videoRecorder) done() videoTrace {
	r.tr.Stats = r.v.Stats
	return r.tr
}

type videoSim struct {
	name      string
	seed      uint64
	frames    int
	loss      float64
	jitter    int64
	layers    int
	face      bool
	cfg       VideoConfig
	ignoreRec bool
	rtt       int64
}

type sentFrame struct {
	f      *frag.Frame
	at     int64
	lost   bool
	lostAt int64
}

func videoSimTrace(s videoSim) videoTrace {
	r := rand.New(rand.NewPCG(s.seed, s.seed*3+1))
	rec := newVideoRecorder(s.name, s.cfg)
	rec.timing(s.rtt)
	var queue []sentFrame
	seq := int64(0)
	lastL0, lastByLayer := int64(-1), [3]int64{-1, -1, -1}
	acked := int64(-1)
	var ackAt []struct{ id, at int64 }
	keyAt, recAt := int64(0), int64(-1)
	recFrom := int64(-1)
	id := int64(0)
	const frameUs = 33_333
	end := int64(s.frames) * frameUs
	const stepUs = 10_000
	for now := int64(0); now < end+1_500_000; now += stepUs {
		if now < end && now%frameUs < stepUs {
			ts := now
			f := &frag.Frame{ID: id, Timestamp: ts}
			layer := 0
			if s.layers == 3 {
				layer = []int{0, 2, 1, 2}[id%4]
			} else if s.layers == 2 {
				layer = int(id % 2)
			}
			switch {
			case s.face && id%45 != 0:
				f.Face, f.Ref, layer = true, -1, 0
			case keyAt >= 0 && now >= keyAt:
				f.Keyframe, f.Ref, layer = true, -1, 0
				f.Face = s.face
				keyAt = -1
				lastByLayer = [3]int64{-1, -1, -1}
				f.LongTerm = !s.face
			case recAt >= 0 && now >= recAt && recFrom >= 0:
				f.Recovery, f.Ref, layer = true, recFrom, 0
				f.LongTerm = true
				recAt = -1
			default:
				if layer == 0 {
					f.Ref = lastL0
					f.LongTerm = true
				} else {
					f.Ref = lastByLayer[layer-1]
					if f.Ref < 0 {
						f.Ref = lastL0
					}
				}
			}
			if f.Keyframe && s.face {
				layer = 0
			}
			f.Layer = layer
			n := int64(1)
			if !f.Face || f.Keyframe {
				n = 1 + r.Int64N(6)
				if f.Keyframe {
					n += 6
				}
			}
			f.FirstSeq, f.LastSeq = seq, seq+n-1
			seq += n
			lost := false
			for range n {
				if r.Float64() < s.loss {
					lost = true
				}
			}
			at := now + 20_000 + r.Int64N(s.jitter+1) + n*400
			f.Complete = at
			queue = append(queue, sentFrame{f: f, at: at, lost: lost, lostAt: now + 1_000_000})
			if layer == 0 && !f.Face {
				lastL0 = id
			}
			if !f.Face {
				lastByLayer[layer] = id
			}
			id++
		}
		keep := queue[:0]
		for _, q := range queue {
			switch {
			case q.lost && now >= q.lostAt:
				rec.lost(frag.Lost{ID: q.f.ID, Keyframe: q.f.Keyframe, Layer: q.f.Layer}, now)
			case !q.lost && now >= q.at:
				rec.push(q.f, now)
			default:
				keep = append(keep, q)
			}
		}
		queue = keep
		ok, key := rec.step(now)
		if ok && !s.ignoreRec && acked >= 0 && recAt < 0 {
			recAt, recFrom = now+s.rtt, acked
		}
		if key && keyAt < 0 {
			keyAt = now + s.rtt
		}
		if now%50_000 == 0 {
			for _, a := range rec.acks() {
				ackAt = append(ackAt, struct{ id, at int64 }{a, now + s.rtt})
			}
		}
		for len(ackAt) > 0 && ackAt[0].at <= now {
			acked = max(acked, ackAt[0].id)
			ackAt = ackAt[1:]
		}
	}
	return rec.done()
}

func vframe2(id int64, key bool, layer int, ref int64, firstSeq, n int64, at int64) *frag.Frame {
	return &frag.Frame{ID: id, Keyframe: key, Layer: layer, Ref: ref, Timestamp: id * 33_333, FirstSeq: firstSeq, LastSeq: firstSeq + n - 1, Complete: at}
}

func videoUnitTraces() []videoTrace {
	var out []videoTrace

	a := newVideoRecorder("releases in order and skips", VideoConfig{})
	a.push(vframe2(0, true, 0, -1, 0, 3, 0), 0)
	a.push(vframe2(2, false, 1, 0, 3, 1, 1000), 1000)
	a.pop(1000)
	a.pop(1000)
	a.push(vframe2(4, false, 0, 0, 6, 1, 2000), 2000)
	a.pop(2000)
	a.pop(2000 + a.v.WaitUs())
	a.keyframe(3000)
	a.push(vframe2(6, false, 0, 5, 10, 1, 3000), 3000)
	a.pop(3000)
	a.pop(3000 + 1_100_000)
	a.recovery(3000 + 1_100_000)
	a.keyframe(3000 + 1_100_000)
	a.recovery(3000 + 2_000_000)
	a.keyframe(3000 + 2_000_000)
	a.push(vframe2(5, false, 0, 4, 9, 1, 3000), 3000)
	out = append(out, a.done())

	b := newVideoRecorder("missing TL0 repaired late", VideoConfig{})
	b.push(vframe2(0, true, 0, -1, 0, 1, 0), 0)
	b.pop(0)
	b.push(vframe2(2, false, 2, 1, 3, 1, 10_000), 10_000)
	b.pop(10_000 + b.v.WaitUs())
	b.push(vframe2(1, false, 0, 0, 1, 2, 200_000), 200_000)
	b.pop(200_000)
	b.push(vframe2(4, false, 0, 1, 5, 1, 210_000), 210_000)
	b.push(vframe2(3, false, 1, 1, 4, 1, 210_000), 210_000)
	b.pop(210_000)
	b.pop(210_000)
	b.keyframe(300_000)
	out = append(out, b.done())

	c := newVideoRecorder("recovery frame skips the broken chain", VideoConfig{})
	key := vframe2(0, true, 0, -1, 0, 2, 0)
	key.LongTerm = true
	c.push(key, 0)
	c.pop(0)
	c.push(vframe2(1, false, 0, 0, 2, 1, 1000), 1000)
	c.pop(1000)
	c.acks()
	c.push(vframe2(4, false, 0, 3, 7, 1, 50_000), 50_000)
	c.push(vframe2(5, false, 1, 4, 8, 1, 60_000), 60_000)
	c.pop(60_000)
	rec := vframe2(6, false, 0, 0, 12, 3, 70_000)
	rec.Recovery = true
	c.push(rec, 70_000)
	c.pop(70_000)
	c.push(vframe2(7, false, 2, 6, 15, 1, 80_000), 80_000)
	c.pop(80_000)
	c.recovery(1_000_000)
	c.keyframe(1_000_000)
	c.acks()
	out = append(out, c.done())

	d := newVideoRecorder("face frames without a keyframe", VideoConfig{})
	face := func(id, seq int64) *frag.Frame {
		f := vframe2(id, false, 0, -1, seq, 1, id*33_333)
		f.Face = true
		return f
	}
	d.push(face(10, 0), 0)
	d.pop(0)
	d.push(face(12, 2), 70_000)
	d.pop(70_000)
	ref := vframe2(13, true, 0, -1, 3, 20, 100_000)
	ref.Face = true
	d.push(ref, 100_000)
	d.push(face(14, 23), 100_000)
	d.pop(100_000)
	d.pop(100_000)
	d.keyframe(200_000)
	out = append(out, d.done())
	return out
}

func videoFuzzTrace(name string, seed uint64, cfg VideoConfig, ops int) videoTrace {
	r := rand.New(rand.NewPCG(seed, seed^0xabc))
	rec := newVideoRecorder(name, cfg)
	now := int64(0)
	id := int64(0)
	seq := int64(0)
	for range ops {
		switch k := r.IntN(24); {
		case k < 10:
			x := id
			if r.IntN(6) == 0 {
				x = id - r.Int64N(5)
			}
			n := 1 + r.Int64N(4)
			f := &frag.Frame{ID: x, Layer: r.IntN(3), Ref: x - 1 - r.Int64N(3), Timestamp: x * 33_333, FirstSeq: seq + r.Int64N(3), Complete: now - r.Int64N(50_000)}
			switch r.IntN(12) {
			case 0:
				f.Keyframe, f.Ref, f.Layer = true, -1, 0
			case 1:
				f.Recovery, f.Layer = true, 0
				f.Ref = x - 2 - r.Int64N(20)
			case 2:
				f.Face, f.Ref, f.Layer = true, -1, 0
			}
			f.LongTerm = r.IntN(2) == 0
			f.LastSeq = f.FirstSeq + n - 1
			seq = f.LastSeq + 1
			rec.push(f, now)
			if x >= id {
				id = x + 1 + r.Int64N(2)
			}
		case k < 12:
			rec.lost(frag.Lost{ID: id - r.Int64N(6), Keyframe: r.IntN(5) == 0, Layer: r.IntN(3)}, now)
		case k < 13:
			rec.timing(r.Int64N(400_000))
		case k < 15:
			rec.recovery(now)
		case k < 17:
			rec.keyframe(now)
		case k < 18:
			rec.acks()
		default:
			now += []int64{5_000, 5_000, 20_000, 100_000, 400_000, 1_200_000}[r.IntN(6)]
			rec.pop(now)
		}
	}
	return rec.done()
}

func buildParityFixture() parityFixture {
	var fx parityFixture
	fx.Audio = append(fx.Audio,
		audioJitterTrace("constant 40 ms", 400, 0, func(int, *rand.Rand) int64 { return 40_000 }),
		audioJitterTrace("100±30 ms uniform, reordering", 550, 0, func(_ int, r *rand.Rand) int64 { return 70_000 + r.Int64N(60_001) }),
		audioJitterTrace("wifi 10 ms + 5% 150 ms spikes", 550, 0, func(_ int, r *rand.Rand) int64 {
			if r.Float64() < 0.05 {
				return 160_000
			}
			return 10_000 + r.Int64N(5_000)
		}),
		audioJitterTrace("5% loss, 20±5 ms", 500, 0.05, func(_ int, r *rand.Rand) int64 { return 15_000 + r.Int64N(10_001) }),
		audioJitterTrace("step 30 ms -> 130 ms -> 30 ms", 650, 0, func(i int, r *rand.Rand) int64 {
			if i > 200 && i < 450 {
				return 130_000 + r.Int64N(5_000)
			}
			return 30_000 + r.Int64N(5_000)
		}),
		audioUnitTrace(),
	)
	random := func(p float64) func(*rand.Rand) bool { return func(r *rand.Rand) bool { return r.Float64() < p } }
	burst := func() func(*rand.Rand) bool {
		bad := false
		return func(r *rand.Rand) bool {
			if bad {
				bad = r.Float64() >= 0.3
			} else {
				bad = r.Float64() < 0.04
			}
			return bad
		}
	}
	for _, c := range []struct {
		name string
		lose func() func(*rand.Rand) bool
	}{
		{"random 10%", func() func(*rand.Rand) bool { return random(0.10) }},
		{"random 20%", func() func(*rand.Rand) bool { return random(0.20) }},
		{"random 30%", func() func(*rand.Rand) bool { return random(0.30) }},
		{"bursts of 3.3 at 12%", burst},
	} {
		fx.Audio = append(fx.Audio,
			audioLossTrace(c.name+", no DRED", AudioConfig{}, 450, 0, c.lose()),
			audioLossTrace(c.name+", DRED 1 s", AudioConfig{}, 450, 1_000_000, c.lose()),
		)
	}
	fx.Audio = append(fx.Audio,
		audioLossTrace("random 20%, DRED 100 ms, no look-ahead", AudioConfig{NoLookAhead: true}, 500, 100_000, random(0.2)),
		audioFuzzTrace("fuzz default", 21, AudioConfig{}, 900),
		audioFuzzTrace("fuzz no look-ahead", 22, AudioConfig{NoLookAhead: true, StartDelayUs: 30_000}, 800),
	)
	fx.Video = append(fx.Video, videoUnitTraces()...)
	fx.Video = append(fx.Video,
		videoSimTrace(videoSim{name: "L1T3 10% loss with recovery", seed: 3, frames: 240, loss: 0.10 / 3, jitter: 30_000, layers: 3, rtt: 40_000}),
		videoSimTrace(videoSim{name: "L1T2 bursts, jitter 80 ms", seed: 4, frames: 200, loss: 0.05, jitter: 80_000, layers: 2, rtt: 60_000}),
		videoSimTrace(videoSim{name: "recovery ignored escalates to a keyframe", seed: 5, frames: 200, loss: 0.04, jitter: 10_000, layers: 1, rtt: 40_000, ignoreRec: true}),
		videoSimTrace(videoSim{name: "no recovery config", seed: 6, frames: 160, loss: 0.04, jitter: 10_000, layers: 3, rtt: 40_000, cfg: VideoConfig{NoRecovery: true}}),
		videoSimTrace(videoSim{name: "face frames and reference pictures", seed: 7, frames: 160, loss: 0.08, jitter: 20_000, layers: 1, rtt: 40_000, face: true}),
		videoFuzzTrace("fuzz default", 31, VideoConfig{}, 1200),
		videoFuzzTrace("fuzz no recovery", 32, VideoConfig{NoRecovery: true, MinWaitUs: 20_000}, 800),
	)
	return fx
}

func encodeParityFixture(fx parityFixture) []byte {
	var buf bytes.Buffer
	buf.WriteString("{\"audio\":[\n")
	for i, tr := range fx.Audio {
		b, _ := json.Marshal(tr)
		buf.Write(b)
		if i < len(fx.Audio)-1 {
			buf.WriteString(",")
		}
		buf.WriteString("\n")
	}
	buf.WriteString("],\"video\":[\n")
	for i, tr := range fx.Video {
		b, _ := json.Marshal(tr)
		buf.Write(b)
		if i < len(fx.Video)-1 {
			buf.WriteString(",")
		}
		buf.WriteString("\n")
	}
	buf.WriteString("]}\n")
	return buf.Bytes()
}

func TestJSParity(t *testing.T) {
	got := encodeParityFixture(buildParityFixture())
	if again := encodeParityFixture(buildParityFixture()); !bytes.Equal(got, again) {
		t.Fatal("the fixture is not deterministic")
	}
	path := filepath.Join("testdata", "js-parity.json")
	if os.Getenv("FMM_JS_PARITY_WRITE") == "1" {
		if err := os.MkdirAll("testdata", 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(path, got, 0o644); err != nil {
			t.Fatal(err)
		}
		t.Logf("wrote %s (%d bytes)", path, len(got))
		return
	}
	stored, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("%v; run with FMM_JS_PARITY_WRITE=1", err)
	}
	if !bytes.Equal(stored, got) {
		t.Fatal("testdata/js-parity.json is stale; run with FMM_JS_PARITY_WRITE=1")
	}
}
