package jitter

import (
	"math/rand/v2"
	"testing"

	"forkmesh.com/mirror-node/rtc/fmm/frag"
)

type delayProfile struct {
	name     string
	delay    func(i int, r *rand.Rand) int64
	loss     float64
	maxConc  float64
	maxDelay int64
}

func runAudio(p delayProfile, frames int) (AudioStats, int64, int64) {
	r := rand.New(rand.NewPCG(9, 9))
	a := NewAudio(AudioConfig{})
	type arrival struct {
		at int64
		id int
	}
	var arr []arrival
	for i := range frames {
		if r.Float64() < p.loss {
			continue
		}
		arr = append(arr, arrival{at: int64(i)*FrameUs + p.delay(i, r), id: i})
	}
	next := 0
	var sumLag, maxTarget int64
	pulls := 0
	for t := int64(0); a.next < int64(frames) && t < int64(frames)*FrameUs+2_000_000; t += FrameUs {
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
			a.Push(int64(arr[best].id), int64(arr[best].id)*FrameUs, []byte{1}, arr[best].at)
			arr[best].at = 1 << 62
			for next < len(arr) && arr[next].at == 1<<62 {
				next++
			}
		}
		_, st := a.Pull(t + 3_000)
		if st != StatusWaiting {
			sumLag += a.TargetUs()
			pulls++
		}
		maxTarget = max(maxTarget, a.TargetUs())
	}
	return a.Stats, sumLag / int64(max(pulls, 1)), maxTarget
}

func TestAudioBufferTracksJitterProfiles(t *testing.T) {
	profiles := []delayProfile{
		{"constant 40 ms", func(int, *rand.Rand) int64 { return 40_000 }, 0, 0.002, 30_000},
		{"100±30 ms uniform", func(_ int, r *rand.Rand) int64 { return 70_000 + r.Int64N(60_001) }, 0, 0.03, 80_000},
		{"wifi 10 ms + 5% 150 ms spikes", func(_ int, r *rand.Rand) int64 {
			if r.Float64() < 0.05 {
				return 160_000
			}
			return 10_000 + r.Int64N(5_000)
		}, 0, 0.06, 200_000},
		{"5% loss, 20±5 ms", func(_ int, r *rand.Rand) int64 { return 15_000 + r.Int64N(10_001) }, 0.05, 0.065, 40_000},
		{"step 30 ms -> 130 ms -> 30 ms", func(i int, r *rand.Rand) int64 {
			if i > 1000 && i < 2000 {
				return 130_000 + r.Int64N(5_000)
			}
			return 30_000 + r.Int64N(5_000)
		}, 0, 0.03, 60_000},
	}
	for _, p := range profiles {
		st, meanTarget, maxTarget := runAudio(p, 3000)
		total := st.Played + st.Concealed + st.Inserted
		conc := float64(st.Concealed+st.Inserted) / float64(total)
		t.Logf("%-30s played %4d conceal %3d insert %3d drop %3d late %3d  concealment %.2f%%  mean target %3d ms  max %3d ms",
			p.name, st.Played, st.Concealed, st.Inserted, st.Dropped, st.Late, 100*conc, meanTarget/1000, maxTarget/1000)
		if conc > p.maxConc {
			t.Errorf("%s: concealment %.3f", p.name, conc)
		}
		if meanTarget > p.maxDelay {
			t.Errorf("%s: mean target %d", p.name, meanTarget)
		}
	}
}

func TestAudioConcealHandsOverTheNextPacketForFECDecode(t *testing.T) {
	a := NewAudio(AudioConfig{StartDelayUs: 20_000, MinDelayUs: 20_000})
	a.Push(0, 0, []byte{0}, 0)
	a.Push(2, 40_000, []byte{2}, 40_000)
	if _, st := a.Pull(10_000); st != StatusWaiting {
		t.Fatal(st)
	}
	if d, st := a.Pull(30_000); st != StatusPlay || d[0] != 0 {
		t.Fatal(st)
	}
	if d, st := a.Pull(50_000); st != StatusConceal || d == nil || d[0] != 2 {
		t.Fatal(st, d)
	}
	if d, st := a.Pull(70_000); st != StatusPlay || d[0] != 2 {
		t.Fatal(st)
	}
	a.Push(1, 20_000, []byte{1}, 80_000)
	if a.Stats.Late != 1 {
		t.Fatal("late frame not counted")
	}
}

func vframe(id int64, key bool, layer int, ref int64, firstSeq, n int64, at int64) *frag.Frame {
	return &frag.Frame{ID: id, Keyframe: key, Layer: layer, Ref: ref, Timestamp: id * 33_333, FirstSeq: firstSeq, LastSeq: firstSeq + n - 1, Complete: at}
}

func TestVideoReleasesInOrderAndSkipsWhatItMust(t *testing.T) {
	v := NewVideo(VideoConfig{})
	v.Push(vframe(0, true, 0, -1, 0, 3, 0), 0)
	v.Push(vframe(2, false, 1, 0, 3, 1, 1000), 1000)
	if f := v.Pop(1000); f == nil || f.ID != 0 {
		t.Fatal("keyframe not released")
	}
	if f := v.Pop(1000); f == nil || f.ID != 2 {
		t.Fatal("a frame after a dropped layer frame waited although its sequence numbers are contiguous")
	}
	v.Push(vframe(4, false, 0, 0, 6, 1, 2000), 2000)
	if f := v.Pop(2000); f != nil {
		t.Fatal("released across a sequence gap without waiting")
	}
	if f := v.Pop(2000 + v.WaitUs()); f == nil || f.ID != 4 {
		t.Fatal("did not skip the gap after the wait")
	}
	if v.KeyframeNeeded(3000) {
		t.Fatal("asked for a keyframe although the TL0 chain is intact")
	}
	v.Push(vframe(6, false, 2, 5, 9, 1, 4000), 4000)
	v.Push(vframe(8, false, 0, 6, 10, 1, 4000), 4000)
	if f := v.Pop(4000 + v.WaitUs()); f != nil {
		t.Fatalf("released frame %d with a missing reference", f.ID)
	}
	if v.Stats.Undecodable != 2 {
		t.Fatalf("undecodable %d", v.Stats.Undecodable)
	}
	if v.KeyframeNeeded(5000) {
		t.Fatal("a broken TL0 chain asked for a keyframe before trying long-term recovery")
	}
	if ok, stuck := v.RecoveryNeeded(5000); !ok || stuck != 5 {
		t.Fatalf("broken TL0 chain did not ask for recovery: %v %d", ok, stuck)
	}
	if ok, _ := v.RecoveryNeeded(6000); ok {
		t.Fatal("recovery requests are not rate limited")
	}
	v.Push(vframe(12, false, 0, 8, 12, 1, 7000), 7000)
	if v.Pop(7000) != nil {
		t.Fatal("decoded on a broken chain")
	}
	if ok, _ := v.RecoveryNeeded(1_000_000); ok || !v.KeyframeNeeded(1_000_000) || v.Stats.Escalated != 1 {
		t.Fatal("an unanswered recovery did not escalate to a keyframe")
	}
	if v.KeyframeNeeded(1_000_001) {
		t.Fatal("keyframe requests are not rate limited")
	}
	v.Push(vframe(13, true, 0, -1, 13, 4, 8000), 8000)
	if f := v.Pop(8000); f == nil || f.ID != 13 || v.KeyframeNeeded(9_000_000) {
		t.Fatal("keyframe did not recover the stream")
	}
	v.Push(vframe(11, false, 1, 8, 11, 1, 9000), 9000)
	if v.Stats.Late != 1 {
		t.Fatal("late frame not dropped")
	}
}

func TestVideoWaitAdaptsToJitter(t *testing.T) {
	r := rand.New(rand.NewPCG(4, 4))
	calm, rough := NewVideo(VideoConfig{}), NewVideo(VideoConfig{})
	seq := int64(0)
	for i := range int64(300) {
		ts := i * 33_333
		calm.Push(&frag.Frame{ID: i, Keyframe: i == 0, Ref: i - 1, Timestamp: ts, FirstSeq: seq, LastSeq: seq, Complete: ts + 20_000}, ts)
		rough.Push(&frag.Frame{ID: i, Keyframe: i == 0, Ref: i - 1, Timestamp: ts, FirstSeq: seq, LastSeq: seq, Complete: ts + 70_000 + r.Int64N(60_000)}, ts)
		seq++
		calm.Pop(ts + 100_000)
		rough.Pop(ts + 200_000)
	}
	if calm.WaitUs() > 40_000 || rough.WaitUs() < 60_000 {
		t.Fatalf("calm %d rough %d", calm.WaitUs(), rough.WaitUs())
	}
}

func TestAMissingTL0RepairedLateIsStillDecoded(t *testing.T) {
	v := NewVideo(VideoConfig{})
	v.Push(vframe(0, true, 0, -1, 0, 1, 0), 0)
	v.Pop(0)
	v.Push(vframe(2, false, 2, 1, 3, 1, 10_000), 10_000)
	if v.Pop(10_000+v.WaitUs()) != nil {
		t.Fatal("released a frame whose reference is missing")
	}
	v.Push(vframe(1, false, 0, 0, 1, 2, 200_000), 200_000)
	if f := v.Pop(200_000); f == nil || f.ID != 1 {
		t.Fatal("a TL0 frame repaired after its dependants timed out was dropped")
	}
	v.Push(vframe(4, false, 0, 1, 5, 1, 210_000), 210_000)
	v.Push(vframe(3, false, 1, 1, 4, 1, 210_000), 210_000)
	if f := v.Pop(210_000); f == nil || f.ID != 3 {
		t.Fatal("chain not continued after the late TL0")
	}
	if f := v.Pop(210_000); f == nil || f.ID != 4 || v.KeyframeNeeded(300_000) {
		t.Fatal("asked for a keyframe though the chain recovered")
	}
}

func TestRecoveryFrameSkipsTheBrokenChainAtOnce(t *testing.T) {
	v := NewVideo(VideoConfig{})
	key := vframe(0, true, 0, -1, 0, 2, 0)
	key.LongTerm = true
	v.Push(key, 0)
	v.Pop(0)
	v.Push(vframe(1, false, 0, 0, 2, 1, 1000), 1000)
	v.Pop(1000)
	if a := v.TakeAcks(); len(a) != 1 || a[0] != 0 {
		t.Fatalf("long-term keyframe not acknowledged: %v", a)
	}
	v.Push(vframe(4, false, 0, 3, 7, 1, 50_000), 50_000)
	v.Push(vframe(5, false, 1, 4, 8, 1, 60_000), 60_000)
	if v.Pop(60_000) != nil {
		t.Fatal("released frame 4 without its reference")
	}
	rec := vframe(6, false, 0, 0, 12, 3, 70_000)
	rec.Recovery = true
	v.Push(rec, 70_000)
	if f := v.Pop(70_000); f == nil || f.ID != 6 {
		t.Fatalf("recovery frame predicted from a decoded long-term frame was not released at once: %+v", f)
	}
	v.Push(vframe(7, false, 2, 6, 15, 1, 80_000), 80_000)
	if f := v.Pop(80_000); f == nil || f.ID != 7 {
		t.Fatal("the chain did not continue after the recovery frame")
	}
	if v.Stats.Recovered != 1 || v.Stats.Undecodable != 2 {
		t.Fatalf("%+v", v.Stats)
	}
	if ok, _ := v.RecoveryNeeded(1_000_000); ok || v.KeyframeNeeded(1_000_000) {
		t.Fatal("asked for help after the recovery frame repaired the chain")
	}
}

func TestIndependentFaceFramesFlowWithoutAKeyframe(t *testing.T) {
	v := NewVideo(VideoConfig{})
	face := func(id, seq int64) *frag.Frame {
		f := vframe(id, false, 0, -1, seq, 1, id*33_333)
		f.Face = true
		return f
	}
	v.Push(face(10, 0), 0)
	if f := v.Pop(0); f == nil || f.ID != 10 {
		t.Fatal("a face frame waited for a keyframe")
	}
	v.Push(face(12, 2), 70_000)
	if f := v.Pop(70_000); f == nil || f.ID != 12 {
		t.Fatal("a face frame waited for a lost one before it")
	}
	ref := vframe(13, true, 0, -1, 3, 20, 100_000)
	ref.Face = true
	v.Push(ref, 100_000)
	v.Push(face(14, 23), 100_000)
	if f := v.Pop(100_000); f == nil || !f.Keyframe || !f.Face {
		t.Fatal("reference picture not released")
	}
	if f := v.Pop(100_000); f == nil || f.ID != 14 || v.KeyframeNeeded(200_000) {
		t.Fatal("face frame after the reference picture")
	}
}

func runAudioLoss(lose func(i int, r *rand.Rand) bool, depthUs int64, frames int) AudioStats {
	r := rand.New(rand.NewPCG(5, 5))
	a := NewAudio(AudioConfig{})
	for t := int64(0); t < int64(frames)*FrameUs; t += FrameUs {
		i := int(t / FrameUs)
		if !lose(i, r) {
			a.PushDepth(int64(i), t, []byte{byte(i)}, depthUs, t+20_000+r.Int64N(5_000))
		}
		a.PullOut(t + 3_000)
	}
	return a.Stats
}

func TestDREDRecoversBurstsThatFECCannot(t *testing.T) {
	bad := false
	burst := func(_ int, r *rand.Rand) bool {
		if bad {
			bad = r.Float64() >= 0.3
		} else {
			bad = r.Float64() < 0.04
		}
		return bad
	}
	random := func(p float64) func(int, *rand.Rand) bool {
		return func(_ int, r *rand.Rand) bool { return r.Float64() < p }
	}
	cases := []struct {
		name string
		lose func(int, *rand.Rand) bool
		max  float64
	}{
		{"random 10%", random(0.10), 0.005},
		{"random 20%", random(0.20), 0.01},
		{"random 30%", random(0.30), 0.03},
		{"bursts of 3.3 at 12%", burst, 0.03},
	}
	for _, c := range cases {
		without := runAudioLoss(c.lose, 0, 5000)
		with := runAudioLoss(c.lose, 1_000_000, 5000)
		total := func(s AudioStats) float64 { return float64(s.Played + s.Concealed + s.DRED + s.Inserted) }
		plain := func(s AudioStats) float64 { return float64(s.Concealed-s.FEC+s.Inserted) / total(s) }
		t.Logf("%-22s no DRED: plain %5.2f%% FEC %5.2f%% | DRED 1 s: plain %5.2f%% FEC %5.2f%% DRED %5.2f%%",
			c.name, 100*plain(without), 100*float64(without.FEC)/total(without),
			100*plain(with), 100*float64(with.FEC)/total(with), 100*float64(with.DRED)/total(with))
		if plain(with) > c.max {
			t.Errorf("%s: %.3f of frames concealed with DRED", c.name, plain(with))
		}
	}
}
