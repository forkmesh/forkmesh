package facestream

import (
	"bytes"
	"errors"
	"math"
	"testing"
)

func encodeAll(t testing.TB, cfg Config, inputs []Input) ([][]byte, []Frame) {
	t.Helper()
	enc, err := NewEncoder(cfg)
	if err != nil {
		t.Fatal(err)
	}
	packets := make([][]byte, 0, len(inputs))
	frames := make([]Frame, 0, len(inputs))
	for i := range inputs {
		p, err := enc.Encode(&inputs[i])
		if err != nil {
			t.Fatal(err)
		}
		packets = append(packets, p)
		frames = append(frames, enc.Last())
	}
	return packets, frames
}

type streamStats struct {
	meanBytes, keyBytes, deltaBytes float64
	rms, worst                      float64
	truthRMS                        float64
}

func measure(t testing.TB, cfg Config, inputs []Input, truths [][LandmarkCount][2]float64) streamStats {
	packets, frames := encodeAll(t, cfg, inputs)
	dec := NewDecoder()
	var st streamStats
	var keys, deltas int
	var sum, truthSum float64
	var n int
	for i, p := range packets {
		got, err := dec.Decode(p)
		if err != nil {
			t.Fatalf("frame %d: %v", i, err)
		}
		if got.Points != frames[i].Points || got.Pose != frames[i].Pose || got.Expression != frames[i].Expression || got.Time != frames[i].Time {
			t.Fatalf("frame %d: decoder and encoder disagree", i)
		}
		step := got.Pose.Scale / float64(got.Precision)
		for k := range got.Points {
			for axis := 0; axis < 2; axis++ {
				e := math.Abs(got.Points[k][axis] - inputs[i].Points[k][axis])
				if e > step/2+1e-9 {
					t.Fatalf("frame %d point %d: error %.4f over half a step %.4f", i, k, e, step/2)
				}
				sum += e * e
				st.worst = math.Max(st.worst, e)
				if truths != nil {
					d := got.Points[k][axis] - truths[i][k][axis]
					truthSum += d * d
				}
				n++
			}
		}
		if got.Key {
			keys++
			st.keyBytes += float64(len(p))
		} else {
			deltas++
			st.deltaBytes += float64(len(p))
		}
		st.meanBytes += float64(len(p))
	}
	st.meanBytes /= float64(len(packets))
	if keys > 0 {
		st.keyBytes /= float64(keys)
	}
	if deltas > 0 {
		st.deltaBytes /= float64(deltas)
	}
	st.rms = math.Sqrt(2 * sum / float64(n))
	st.truthRMS = math.Sqrt(2 * truthSum / float64(n))
	return st
}

func TestRoundTripStaysWithinHalfAStepAndUnderBudget(t *testing.T) {
	inputs, truths := synthSequence(synthOptions{frames: 600, fps: 30, noise: 0.5, seed: 7, jitter: true})
	for _, precision := range []int{48, 64, 96, 128} {
		st := measure(t, Config{Width: 1280, Height: 720, Precision: precision}, inputs, truths)
		t.Logf("precision %3d: %.1f bytes/frame (key %.1f, delta %.1f), %.2f kbit/s at 30 fps, codec RMS %.3f px, worst %.3f px, RMS against the noiseless truth %.3f px",
			precision, st.meanBytes, st.keyBytes, st.deltaBytes, st.meanBytes*8*30/1000, st.rms, st.worst, st.truthRMS)
		if precision == DefaultPrecision && st.meanBytes > 60 {
			t.Fatalf("default precision uses %.1f bytes per frame", st.meanBytes)
		}
	}
}

func TestLossConcealsUntilTheNextKey(t *testing.T) {
	inputs, _ := synthSequence(synthOptions{frames: 90, fps: 30, noise: 0.5, seed: 3})
	packets, frames := encodeAll(t, Config{Width: 1280, Height: 720}, inputs)
	dec := NewDecoder()
	for i, p := range packets {
		if i == 10 || i == 11 {
			continue
		}
		got, err := dec.Decode(p)
		if err != nil {
			t.Fatalf("frame %d: %v", i, err)
		}
		switch {
		case i < 10 || i >= 30:
			if got.Stale || got.Points != frames[i].Points {
				t.Fatalf("frame %d should decode exactly", i)
			}
		default:
			if !got.Stale {
				t.Fatalf("frame %d should be concealed", i)
			}
			if got.Pose != frames[i].Pose || got.Expression != frames[i].Expression {
				t.Fatalf("frame %d: pose and expression must survive a loss", i)
			}
			if got.Time != frames[i].Time {
				t.Fatalf("frame %d: time %d, want %d", i, got.Time, frames[i].Time)
			}
			for k := range got.Points {
				d := math.Hypot(got.Points[k][0]-inputs[i].Points[k][0], got.Points[k][1]-inputs[i].Points[k][1])
				if d > 0.6*got.Pose.Scale {
					t.Fatalf("frame %d point %d concealed %.1f px away", i, k, d)
				}
			}
		}
	}
}

func TestDeltaBeforeKeyAndRepeatsAreRefused(t *testing.T) {
	inputs, _ := synthSequence(synthOptions{frames: 5, fps: 30, seed: 5})
	packets, _ := encodeAll(t, Config{Width: 640, Height: 480}, inputs)
	dec := NewDecoder()
	if _, err := dec.Decode(packets[1]); !errors.Is(err, ErrNeedKey) {
		t.Fatalf("delta before key: %v", err)
	}
	if _, err := dec.Decode(packets[0]); err != nil {
		t.Fatal(err)
	}
	if _, err := dec.Decode(packets[1]); err != nil {
		t.Fatal(err)
	}
	if _, err := dec.Decode(packets[1]); !errors.Is(err, ErrOld) {
		t.Fatalf("repeat: %v", err)
	}
	if _, err := dec.Decode(packets[0][:5]); !errors.Is(err, ErrShort) {
		t.Fatalf("short key: %v", err)
	}
	bad := append([]byte(nil), packets[2]...)
	bad[0] = bad[0]&^0x30 | 2<<4
	if _, err := dec.Decode(bad); !errors.Is(err, ErrVersion) {
		t.Fatalf("version: %v", err)
	}
}

func TestRequestKeyAndTimeJumpsSendKeys(t *testing.T) {
	inputs, _ := synthSequence(synthOptions{frames: 6, fps: 30, seed: 9})
	enc, _ := NewEncoder(Config{Width: 640, Height: 480})
	kinds := []bool{}
	for i := range inputs {
		if i == 2 {
			enc.RequestKey()
		}
		if i == 4 {
			inputs[i].Time += 10000
			inputs[5].Time += 10000
		}
		p, err := enc.Encode(&inputs[i])
		if err != nil {
			t.Fatal(err)
		}
		kinds = append(kinds, PacketKind(p) == KindKey)
	}
	want := []bool{true, false, true, false, true, false}
	for i := range want {
		if kinds[i] != want[i] {
			t.Fatalf("key pattern %v, want %v", kinds, want)
		}
	}
}

func TestBadInputAndConfigAreRefused(t *testing.T) {
	if _, err := NewEncoder(Config{Width: 0, Height: 10}); err == nil {
		t.Fatal("zero width accepted")
	}
	if _, err := NewEncoder(Config{Width: 10, Height: 10, Precision: 8}); err == nil {
		t.Fatal("precision 8 accepted")
	}
	enc, _ := NewEncoder(Config{Width: 10, Height: 10})
	in := Input{}
	in.Points[3][1] = math.NaN()
	if _, err := enc.Encode(&in); !errors.Is(err, ErrInput) {
		t.Fatalf("NaN: %v", err)
	}
}

func TestExtremePosesStayBounded(t *testing.T) {
	enc, _ := NewEncoder(Config{Width: 4096, Height: 4096, Precision: 255})
	dec := NewDecoder()
	poses := []Pose{
		{Rotation: Quat{0, 1, 0, 0}, X: 1e6, Y: -1e6, Scale: 1e6},
		{Rotation: Quat{0, 0, 0, 0}, X: 0, Y: 0, Scale: 0},
		{Rotation: Quat{-1, 0.2, 0.3, 0.1}, X: 20000, Y: 3, Scale: 4000},
	}
	for i, pose := range poses {
		in := Input{Time: uint32(i * 33), Pose: pose}
		for k := range in.Points {
			in.Points[k] = [2]float64{1e7 * float64(k%3-1), -1e7}
		}
		p, err := enc.Encode(&in)
		if err != nil {
			t.Fatal(err)
		}
		got, err := dec.Decode(p)
		if err != nil {
			t.Fatalf("pose %d: %v", i, err)
		}
		for _, q := range got.Points {
			if !finite(q[0]) || !finite(q[1]) {
				t.Fatalf("pose %d: non-finite point", i)
			}
		}
	}
}

func referenceSample() *Reference {
	r := &Reference{ID: 3, Format: FormatJPEG, Time: 123456, CropX: 500, CropY: 120, CropW: 320, CropH: 400, Width: 256, Height: 320,
		MaskW: 64, MaskH: 80, Image: bytes.Repeat([]byte{0xFF, 0xD8, 0x11}, 100)}
	inputs, _ := synthSequence(synthOptions{frames: 1, fps: 30, seed: 2})
	for i, p := range inputs[0].Points {
		r.Points[i] = MapToReference(r, p)
	}
	r.Mask = make([]byte, 64*80)
	for y := 0; y < 80; y++ {
		for x := 0; x < 64; x++ {
			dx := float64(x-32) / 20
			dy := float64(y-34) / 26
			if dx*dx+dy*dy < 1 || (y > 60 && x > 8 && x < 56) {
				r.Mask[y*64+x] = 1
			}
		}
	}
	return r
}

func TestReferenceRoundTrip(t *testing.T) {
	r := referenceSample()
	packet, err := EncodeReference(r)
	if err != nil {
		t.Fatal(err)
	}
	maskBytes := len(packet) - referenceFixed - len(r.Image)
	t.Logf("reference metadata %d bytes, 64x80 mask %d bytes", referenceFixed, maskBytes)
	if maskBytes > 200 {
		t.Fatalf("mask takes %d bytes", maskBytes)
	}
	got, err := DecodeReference(packet)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(got.Mask, r.Mask) || !bytes.Equal(got.Image, r.Image) || got.CropW != 320 || got.Time != 123456 || got.ID != 3 {
		t.Fatal("reference fields changed")
	}
	for i := range r.Points {
		for axis := 0; axis < 2; axis++ {
			if math.Abs(got.Points[i][axis]-r.Points[i][axis]) > 1.0/32+1e-9 {
				t.Fatalf("point %d moved", i)
			}
		}
	}
	if _, err := NewDecoder().Decode(packet); !errors.Is(err, ErrNotAFrame) {
		t.Fatalf("frame decoder took a reference: %v", err)
	}
	r.Image = nil
	if _, err := EncodeReference(r); err == nil {
		t.Fatal("empty image accepted")
	}
}

func FuzzDecode(f *testing.F) {
	inputs, _ := synthSequence(synthOptions{frames: 4, fps: 30, seed: 11})
	packets, _ := encodeAll(f, Config{Width: 1280, Height: 720}, inputs)
	for _, p := range packets {
		f.Add(p)
	}
	f.Add([]byte{0x50, 0})
	f.Fuzz(func(t *testing.T, data []byte) {
		dec := NewDecoder()
		if _, err := dec.Decode(packets[0]); err != nil {
			t.Fatal(err)
		}
		got, err := dec.Decode(data)
		if err != nil {
			return
		}
		for _, q := range got.Points {
			if !finite(q[0]) || !finite(q[1]) {
				t.Fatal("non-finite point")
			}
		}
		fresh := NewDecoder()
		_, _ = fresh.Decode(data)
	})
}

func FuzzDecodeReference(f *testing.F) {
	packet, _ := EncodeReference(referenceSample())
	f.Add(packet)
	f.Add(packet[:referenceFixed+1])
	f.Fuzz(func(t *testing.T, data []byte) {
		r, err := DecodeReference(data)
		if err != nil {
			return
		}
		if r.MaskW > 0 && len(r.Mask) != r.MaskW*r.MaskH {
			t.Fatal("mask size")
		}
		if len(r.Image) == 0 || r.Width > MaxReferenceSide || r.Height > MaxReferenceSide {
			t.Fatal("bounds")
		}
	})
}
