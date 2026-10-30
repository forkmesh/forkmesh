package frag

import (
	"bytes"
	"math/rand/v2"
	"testing"

	"forkmesh.com/mirror-node/rtc/fmm/wire"
)

func TestFramesSurviveShuffleAndDuplicates(t *testing.T) {
	r := rand.New(rand.NewPCG(1, 2))
	p := NewPacketizer(4, 1000)
	ra := NewReassembler()
	var all [][]byte
	var frames [][]byte
	for i := range 50 {
		data := make([]byte, r.IntN(9000))
		for j := range data {
			data[j] = byte(r.Uint32())
		}
		frames = append(frames, data)
		pkts, err := p.Packetize(data, i == 0, 0, false, uint32(i*33333))
		if err != nil {
			t.Fatal(err)
		}
		all = append(all, pkts...)
	}
	all = append(all, all[:20]...)
	r.Shuffle(len(all), func(i, j int) { all[i], all[j] = all[j], all[i] })
	got := map[int64][]byte{}
	for _, b := range all {
		m, err := wire.ParseMedia(b)
		if err != nil {
			t.Fatal(err)
		}
		if f := ra.Add(&m, false, 0); f != nil {
			if _, dup := got[f.ID]; dup {
				t.Fatal("frame delivered twice")
			}
			got[f.ID] = f.Data
			if f.LastSeq-f.FirstSeq+1 != int64(max(1, (len(f.Data)+999)/1000)) {
				t.Fatalf("frame %d seq span wrong", f.ID)
			}
		}
	}
	for i, want := range frames {
		if !bytes.Equal(got[int64(i)], want) {
			t.Fatalf("frame %d differs", i)
		}
	}
}

func TestTemporalLayerReferences(t *testing.T) {
	p := NewPacketizer(1, 1000)
	pattern := []int{0, 2, 1, 2, 0, 2, 1, 2}
	wantRef := []uint8{0, 1, 2, 1, 4, 1, 2, 1}
	for i, l := range pattern {
		pkts, err := p.Packetize([]byte{1}, i == 0, l, false, 0)
		if err != nil {
			t.Fatal(err)
		}
		m, _ := wire.ParseMedia(pkts[0])
		if m.RefDelta != wantRef[i] || m.FrameID != uint16(i) {
			t.Fatalf("frame %d layer %d: ref delta %d want %d", i, l, m.RefDelta, wantRef[i])
		}
	}
	audio := NewPacketizer(2, 1000)
	pkts, _ := audio.Packetize([]byte{1}, false, 0, true, 0)
	if m, _ := wire.ParseMedia(pkts[0]); m.RefDelta != 0 || m.Keyframe {
		t.Fatal("independent frame carries a reference")
	}
	if _, err := NewPacketizer(3, 1000).Packetize([]byte{1}, false, 0, false, 0); err != ErrRef {
		t.Fatal("delta frame before any keyframe accepted")
	}
	if _, err := p.Packetize(make([]byte, 256*1000), true, 0, false, 0); err != ErrTooLarge {
		t.Fatal("oversized frame accepted")
	}
}

func TestIncompleteFramesExpireAndStayDone(t *testing.T) {
	p := NewPacketizer(1, 100)
	pkts, _ := p.Packetize(make([]byte, 300), true, 0, false, 0)
	ra := NewReassembler()
	m, _ := wire.ParseMedia(pkts[0])
	ra.Add(&m, false, 0)
	if l := ra.Expire(500_000); len(l) != 0 {
		t.Fatal("expired early")
	}
	l := ra.Expire(DefaultLimit)
	if len(l) != 1 || !l[0].Keyframe {
		t.Fatalf("%+v", l)
	}
	for _, b := range pkts[1:] {
		m, _ := wire.ParseMedia(b)
		if ra.Add(&m, false, DefaultLimit) != nil {
			t.Fatal("expired frame completed")
		}
	}
}

func FuzzReassembler(f *testing.F) {
	p := NewPacketizer(1, 40)
	pkts, _ := p.Packetize(make([]byte, 100), true, 0, false, 7)
	for _, b := range pkts {
		f.Add(b, pkts[0])
	}
	f.Fuzz(func(t *testing.T, a, b []byte) {
		ra := NewReassembler()
		for i, x := range [][]byte{a, b, a, b} {
			m, err := wire.ParseMedia(x)
			if err != nil {
				continue
			}
			if fr := ra.Add(&m, false, int64(i)); fr != nil && fr.LastSeq < fr.FirstSeq {
				t.Fatal("bad frame span")
			}
		}
		ra.Expire(1 << 40)
		if ra.bytes != 0 || len(ra.partial) != 0 || len(ra.order) != 0 {
			t.Fatal("state left after expiry")
		}
	})
}

func TestExplicitReferencesAndFrameFlags(t *testing.T) {
	p := NewPacketizer(1, 1000)
	for i := range 10 {
		if _, err := p.PacketizeFrame([]byte{1}, Opts{Keyframe: i == 0, Ref: -1, LongTerm: true}, 0); err != nil {
			t.Fatal(err)
		}
	}
	pkts, err := p.PacketizeFrame([]byte{2}, Opts{Ref: 3}, 0)
	if err != nil {
		t.Fatal(err)
	}
	m, _ := wire.ParseMedia(pkts[0])
	if !m.Recovery || m.RefDelta != 7 || m.FrameID != 10 || p.LastID() != 10 {
		t.Fatalf("%+v", m)
	}
	ra := NewReassembler()
	f := ra.Add(&m, false, 0)
	if f == nil || !f.Recovery || f.Ref != 3 {
		t.Fatalf("%+v", f)
	}
	pkts, _ = p.PacketizeFrame([]byte{3}, Opts{Layer: 1, Ref: -1}, 0)
	if m, _ := wire.ParseMedia(pkts[0]); m.RefDelta != 1 {
		t.Fatal("the layer structure did not continue from the recovery frame")
	}
	for _, bad := range []Opts{{Ref: 0, Layer: 1}, {Ref: 50}, {Ref: 0, Keyframe: true}, {Ref: -400}} {
		if bad.Ref == -400 {
			bad.Ref = 0
			for range 300 {
				p.PacketizeFrame([]byte{1}, Opts{Ref: -1}, 0)
			}
		}
		if _, err := p.PacketizeFrame([]byte{1}, bad, 0); err == nil {
			t.Errorf("%+v accepted", bad)
		}
	}
	face := NewPacketizer(2, 1000)
	pkts, err = face.PacketizeFrame([]byte{4}, Opts{Face: true, Independent: true, Ref: -1}, 0)
	if err != nil {
		t.Fatal("a face frame needed a keyframe first")
	}
	m, _ = wire.ParseMedia(pkts[0])
	if f := NewReassembler().Add(&m, false, 0); f == nil || !f.Face || f.Ref != -1 || f.Keyframe {
		t.Fatalf("%+v", f)
	}
	audio := NewPacketizer(3, 1000)
	pkts, _ = audio.PacketizeFrame([]byte{5}, Opts{Independent: true, Ref: -1, Aux: 100}, 0)
	m, _ = wire.ParseMedia(pkts[0])
	if f := NewReassembler().Add(&m, false, 0); f == nil || f.Aux != 100 {
		t.Fatalf("redundancy depth lost: %+v", f)
	}
}
