package rtp

import (
	"bytes"
	"math/rand"
	"reflect"
	"testing"
)

var chromeFeedbackOne = []byte{
	0xaf, 0xcd, 0x00, 0x05,
	0xfa, 0x17, 0xfa, 0x17,
	0x43, 0x03, 0x2f, 0xa0,
	0x00, 0x99, 0x00, 0x01,
	0x3d, 0xe8, 0x02, 0x17,
	0x20, 0x01, 0x94, 0x01,
}

var chromeFeedbackSeven = []byte{
	0xaf, 0xcd, 0x00, 0x07,
	0xfa, 0x17, 0xfa, 0x17,
	0x19, 0x3d, 0xd8, 0xbb,
	0x00, 0x04, 0x00, 0x07,
	0x10, 0x63, 0x6e, 0x01,
	0x20, 0x07, 0x4c, 0x24,
	0x24, 0x10, 0x0c, 0x0c,
	0x10, 0x00, 0x00, 0x03,
}

var chromeFeedbackMixed = []byte{
	0xaf, 0xcd, 0x00, 0x07,
	0x9b, 0x74, 0xf6, 0x1f,
	0x93, 0x71, 0xdc, 0xbc,
	0x85, 0x3c, 0x00, 0x09,
	0x63, 0xf9, 0x16, 0xb3,
	0xd5, 0x52, 0x00, 0x30,
	0x9b, 0xaa, 0x6a, 0xaa,
	0x7b, 0x01, 0x09, 0x01,
}

func received(ticks ...int32) []FeedbackStatus {
	out := make([]FeedbackStatus, len(ticks))
	for i, d := range ticks {
		out[i] = FeedbackStatus{Received: true, Delta: d}
	}
	return out
}

func TestTransportFeedbackMatchesChromeBytes(t *testing.T) {
	cases := []struct {
		name string
		want []byte
		fb   TransportFeedback
	}{
		{"one packet", chromeFeedbackOne, TransportFeedback{
			SenderSSRC: 4195875351, MediaSSRC: 1124282272, BaseSequence: 153,
			ReferenceTime: 4057090, FeedbackCount: 23, Statuses: received(148),
		}},
		{"seven packets", chromeFeedbackSeven, TransportFeedback{
			SenderSSRC: 4195875351, MediaSSRC: 423483579, BaseSequence: 4,
			ReferenceTime: 1074030, FeedbackCount: 1, Statuses: received(76, 36, 36, 16, 12, 12, 16),
		}},
	}
	for _, c := range cases {
		got, err := c.fb.Marshal()
		if err != nil {
			t.Fatalf("%s: %v", c.name, err)
		}
		if !bytes.Equal(got, c.want) {
			t.Fatalf("%s:\n got % x\nwant % x", c.name, got, c.want)
		}
		parsed, err := ParseTransportFeedback(c.want)
		if err != nil {
			t.Fatalf("%s: %v", c.name, err)
		}
		if !reflect.DeepEqual(*parsed, c.fb) {
			t.Fatalf("%s: parsed %+v want %+v", c.name, *parsed, c.fb)
		}
	}
}

func TestParseChromeFeedbackWithLossAndLargeDelta(t *testing.T) {
	f, err := ParseTransportFeedback(chromeFeedbackMixed)
	if err != nil {
		t.Fatal(err)
	}
	want := append(received(155, 170, 106, 170, 123), FeedbackStatus{}, FeedbackStatus{Received: true, Delta: 265}, FeedbackStatus{}, FeedbackStatus{})
	if f.SenderSSRC != 2608133663 || f.MediaSSRC != 2473712828 || f.BaseSequence != 34108 ||
		f.ReferenceTime != 6551830 || f.FeedbackCount != 179 || !reflect.DeepEqual(f.Statuses, want) {
		t.Fatalf("parsed %+v", f)
	}
}

func TestTransportFeedbackChunkChoice(t *testing.T) {
	lost := FeedbackStatus{}
	cases := []struct {
		name     string
		statuses []FeedbackStatus
		chunks   []uint16
	}{
		{"run of small", received(1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1), []uint16{0x2000 | 20}},
		{"two bit with large", append(received(1, 300), lost, lost, lost), []uint16{0xc000 | 1<<12 | 2<<10 | 1<<4}},
		{"one bit", append(append(received(1, 2, 3), lost), received(1, 2, 3, 4, 5, 6, 7, 8, 9, 10)...), []uint16{0x8000 | 0x3bff}},
		{"long loss run", append(append(received(1), make([]FeedbackStatus, 40)...), received(4)...), []uint16{0x8000 | 1<<13, 0x0000 | 27, 0x2000 | 1}},
	}
	for _, c := range cases {
		c.statuses[len(c.statuses)-1].Received = true
		p, err := (&TransportFeedback{BaseSequence: 65530, Statuses: c.statuses}).Marshal()
		if err != nil {
			t.Fatalf("%s: %v", c.name, err)
		}
		for i, want := range c.chunks {
			got := uint16(p[20+2*i])<<8 | uint16(p[21+2*i])
			if got != want {
				t.Fatalf("%s: chunk %d %04x want %04x (% x)", c.name, i, got, want, p)
			}
		}
		back, err := ParseTransportFeedback(p)
		if err != nil || !reflect.DeepEqual(back.Statuses, c.statuses) {
			t.Fatalf("%s: round trip %v %+v", c.name, err, back)
		}
	}
}

func TestTransportFeedbackRoundTripsRandomly(t *testing.T) {
	rng := rand.New(rand.NewSource(7))
	for round := 0; round < 500; round++ {
		n := 1 + rng.Intn(300)
		statuses := make([]FeedbackStatus, n)
		for i := range statuses {
			switch r := rng.Intn(10); {
			case r < 2:
			case r < 8:
				statuses[i] = FeedbackStatus{Received: true, Delta: int32(rng.Intn(256))}
			default:
				statuses[i] = FeedbackStatus{Received: true, Delta: int32(rng.Intn(65536) - 32768)}
			}
		}
		statuses[n-1] = FeedbackStatus{Received: true, Delta: 3}
		f := TransportFeedback{SenderSSRC: rng.Uint32(), MediaSSRC: rng.Uint32(), BaseSequence: uint16(rng.Uint32()),
			ReferenceTime: rng.Uint32() & 0xffffff, FeedbackCount: uint8(round), Statuses: statuses}
		p, err := f.Marshal()
		if err != nil {
			t.Fatal(err)
		}
		if len(p)%4 != 0 {
			t.Fatalf("length %d", len(p))
		}
		back, err := ParseTransportFeedback(p)
		if err != nil {
			t.Fatal(err)
		}
		if !reflect.DeepEqual(*back, f) {
			t.Fatalf("round %d differs", round)
		}
		parts, err := SplitCompound(p)
		if err != nil || len(parts) != 1 {
			t.Fatalf("not a valid compound: %v", err)
		}
	}
}

func TestTransportFeedbackRefusesBadInput(t *testing.T) {
	if _, err := (&TransportFeedback{}).Marshal(); err == nil {
		t.Fatal("empty feedback marshalled")
	}
	if _, err := (&TransportFeedback{Statuses: received(40000)}).Marshal(); err == nil {
		t.Fatal("out of range delta marshalled")
	}
	reserved := append([]byte(nil), chromeFeedbackOne...)
	reserved[20], reserved[21] = 0x60, 0x01
	if _, err := ParseTransportFeedback(reserved); err == nil {
		t.Fatal("reserved symbol accepted")
	}
	empty := append([]byte(nil), chromeFeedbackOne...)
	empty[15] = 0
	if _, err := ParseTransportFeedback(empty); err == nil {
		t.Fatal("zero statuses accepted")
	}
	for n := 0; n < len(chromeFeedbackSeven); n++ {
		cut := append([]byte(nil), chromeFeedbackSeven[:n]...)
		if n >= 4 {
			cut[2], cut[3] = 0, byte(n/4-1)
		}
		ParseTransportFeedback(cut)
	}
	huge := append([]byte(nil), chromeFeedbackOne...)
	huge[14], huge[15] = 0xff, 0xff
	huge[20], huge[21] = 0x1f, 0xff
	if _, err := ParseTransportFeedback(huge); err == nil {
		t.Fatal("feedback claiming 65535 packets accepted")
	}
	if _, err := ParseTransportFeedback(NewPLI(1, 2)); err == nil {
		t.Fatal("pli parsed as feedback")
	}
}
