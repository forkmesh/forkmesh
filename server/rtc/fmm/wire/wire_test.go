package wire

import (
	"bytes"
	"encoding/hex"
	"reflect"
	"testing"
)

func TestMediaHeaderLayoutIsExact(t *testing.T) {
	m := Media{
		Stream: 3, TransportSeq: 0x0102, Seq: 0x0304, FrameID: 0x0506,
		FragIndex: 1, FragCount: 4, FEC: true, Layer: 2, RefDelta: 1,
		Timestamp: 0x0A0B0C0D, Payload: []byte{0xEE},
	}
	got := hex.EncodeToString(m.Append(nil))
	want := "1003" + "0102" + "0304" + "0506" + "01" + "04" + "42" + "01" + "0a0b0c0d" + "ee"
	if got != want {
		t.Fatalf("got %s want %s", got, want)
	}
	back, err := ParseMedia(m.Append(nil))
	if err != nil || !reflect.DeepEqual(back, m) {
		t.Fatalf("%v %+v", err, back)
	}
}

func TestRepairAndPaddingLayoutsAreExact(t *testing.T) {
	r := Repair{Stream: 9, TransportSeq: 7, BaseSeq: 0xFFFE, Scheme: SchemeRS, K: 3, M: 2, Index: 1, Offsets: []uint8{0, 2, 9}, Symbol: make([]byte, 12)}
	b := r.Append(nil)
	if hex.EncodeToString(b[:RepairHeaderLen+3]) != "11090007fffe01030201000209" {
		t.Fatal(hex.EncodeToString(b[:RepairHeaderLen+3]))
	}
	if m := r.Members(nil); m[0] != 0xFFFE || m[1] != 0 || m[2] != 7 {
		t.Fatal(m)
	}
	back, err := ParseRepair(b)
	if err != nil || !reflect.DeepEqual(back, r) {
		t.Fatalf("%v %+v", err, back)
	}
	p := AppendPadding(nil, 0xBEEF, 20)
	if len(p) != 20 || hex.EncodeToString(p[:4]) != "1200beef" {
		t.Fatal(hex.EncodeToString(p))
	}
	if s, err := ParsePadding(p); err != nil || s != 0xBEEF {
		t.Fatal(s, err)
	}
}

func TestParsersRefuseBadHeaders(t *testing.T) {
	good := (&Media{FragCount: 1, Payload: []byte{1}}).Append(nil)
	cases := map[string]func([]byte){
		"version":      func(b []byte) { b[0] = 0x20 },
		"kind":         func(b []byte) { b[0] = 0x13 },
		"zero count":   func(b []byte) { b[9] = 0 },
		"index>=count": func(b []byte) { b[8] = 1 },
		"recovery key": func(b []byte) { b[10] = FlagKeyframe | FlagRecovery },
		"recovery ref": func(b []byte) { b[10] = FlagRecovery },
		"recovery tl1": func(b []byte) { b[10] = FlagRecovery | 1; b[11] = 2 },
		"key layer":    func(b []byte) { b[10] = FlagKeyframe | 1 },
		"key ref":      func(b []byte) { b[10] = FlagKeyframe; b[11] = 1 },
	}
	for name, mutate := range cases {
		b := append([]byte(nil), good...)
		mutate(b)
		if _, err := ParseMedia(b); err == nil {
			t.Errorf("%s accepted", name)
		}
	}
	if _, err := ParseMedia(good[:10]); err == nil {
		t.Error("short accepted")
	}
	if _, err := ParseMedia(make([]byte, MaxDatagramLen+1)); err == nil {
		t.Error("long accepted")
	}
	four := []uint8{0, 1, 2, 3}
	bad := []Repair{
		{K: 0, M: 1, Symbol: make([]byte, 12)},
		{K: 49, M: 1, Offsets: make([]uint8, 49), Symbol: make([]byte, 12)},
		{K: 4, M: 2, Scheme: SchemeXOR, Offsets: four, Symbol: make([]byte, 12)},
		{K: 4, M: 2, Index: 2, Scheme: SchemeRS, Offsets: four, Symbol: make([]byte, 12)},
		{K: 4, M: 1, Scheme: 2, Offsets: four, Symbol: make([]byte, 12)},
		{K: 4, M: 1, Offsets: four, Symbol: make([]byte, 11)},
		{K: 4, M: 1, Offsets: []uint8{1, 2, 3, 4}, Symbol: make([]byte, 12)},
		{K: 4, M: 1, Offsets: []uint8{0, 2, 2, 3}, Symbol: make([]byte, 12)},
	}
	for i, r := range bad {
		if _, err := ParseRepair(r.Append(nil)); err == nil {
			t.Errorf("repair %d accepted", i)
		}
	}
}

func sampleMessages() []Message {
	return []Message{
		Join{Ticket: "eyJyb29tIjoiYSJ9.c2ln"},
		Joined{Peer: "p1"},
		Publish{Stream: 1, Kind: TrackAudio, Layers: 1, Name: "mic"},
		Publish{Stream: 2, Kind: TrackVideo, Layers: 3, Name: ""},
		Publish{Stream: 3, Kind: TrackVideo, Layers: 1, Name: "cam", Track: 2, Rank: 0},
		Publish{Stream: 4, Kind: TrackVideo, Layers: 3, Name: "cam", Track: 4, Rank: 3},
		Unpublish{Stream: 2},
		TrackAdded{Stream: 7, Kind: TrackVideo, Layers: 3, Peer: "p2", Name: "cam"},
		TrackRemoved{Stream: 7},
		Subscribe{Stream: 7, MaxLayer: 2},
		Unsubscribe{Stream: 7},
		KeyframeRequest{Stream: 7},
		&Report{Base: 65530, RefTime: 123456, Delay: 40,
			Received: []bool{true, false, true, true, true, false, false, true, true},
			Arrival:  []int64{0, 0, 10, 300, -5, 0, 0, 254, 254}},
		BitrateHint{Stream: AllStreams, Bitrate: 1_500_000},
		Leave{Reason: "bye"},
		Error{Code: 403, Text: "ticket expired"},
		Nack{Stream: 4, Seqs: []uint16{65535, 0, 7}},
		Ack{Stream: 4, Frames: []uint16{900, 896, 65535}},
		Recovery{Stream: 4, Frame: 1234},
	}
}

func TestEveryControlMessageRoundTrips(t *testing.T) {
	var stream []byte
	for _, m := range sampleMessages() {
		b, err := Append(nil, m)
		if err != nil {
			t.Fatalf("%T: %v", m, err)
		}
		back, n, err := Decode(b)
		if err != nil || n != len(b) || !reflect.DeepEqual(back, m) {
			t.Fatalf("%T: %v %d %+v", m, err, n, back)
		}
		stream = append(stream, b...)
	}
	var r Reader
	var got []Message
	for i := 0; i < len(stream); i += 5 {
		if err := r.Write(stream[i:min(i+5, len(stream))]); err != nil {
			t.Fatal(err)
		}
		for {
			m, err := r.Next()
			if err != nil {
				t.Fatal(err)
			}
			if m == nil {
				break
			}
			got = append(got, m)
		}
	}
	if !reflect.DeepEqual(got, sampleMessages()) {
		t.Fatalf("stream decode mismatch: %d messages", len(got))
	}
}

func TestReportIsAsCompactAsTransportWideFeedback(t *testing.T) {
	n := 100
	r := &Report{Received: make([]bool, n), Arrival: make([]int64, n)}
	for i := range n {
		r.Received[i] = i%10 != 3
	}
	at := int64(0)
	first := true
	for i := range n {
		if r.Received[i] {
			if !first {
				at += 8
			}
			first = false
			r.Arrival[i] = at
		}
	}
	b, err := Append(nil, r)
	if err != nil {
		t.Fatal(err)
	}
	if len(b) > ControlHeaderLen+10+13+90 {
		t.Fatalf("report of %d packets is %d bytes", n, len(b))
	}
}

func TestControlBoundsAreEnforced(t *testing.T) {
	long := string(bytes.Repeat([]byte{'a'}, MaxName+1))
	bad := []Message{
		Join{}, Joined{Peer: ""}, Joined{Peer: long},
		Publish{Kind: TrackAudio, Layers: 2}, Publish{Kind: TrackVideo, Layers: 9}, Publish{Kind: 4, Layers: 1}, Publish{Kind: TrackAuth, Layers: 2},
		Subscribe{MaxLayer: 8}, Joined{Peer: "\xff"},
		&Report{}, &Report{Received: []bool{false}, Arrival: []int64{0}},
		&Report{Received: []bool{true}, Arrival: []int64{5}},
		&Report{Received: []bool{true, true}, Arrival: []int64{0, 40000}},
		&Report{Received: make([]bool, MaxReportPackets+1), Arrival: make([]int64, MaxReportPackets+1)},
		Nack{Stream: 1}, Nack{Stream: 1, Seqs: make([]uint16, MaxNack+1)},
	}
	for i, m := range bad {
		if _, err := Append(nil, m); err == nil {
			t.Errorf("%d %T accepted", i, m)
		}
	}
	if _, _, err := Decode([]byte{MsgJoin, 0xFF, 0xFF}); err != ErrBound {
		t.Error(err)
	}
	if _, _, err := Decode([]byte{0x77, 0, 0}); err != ErrType {
		t.Error(err)
	}
	b, _ := Append(nil, Unpublish{Stream: 1})
	b = append(b[:2], 2, 1, 0)
	if _, _, err := Decode(b); err != ErrBody {
		t.Error("trailing body bytes accepted", err)
	}
	var r Reader
	if err := r.Write(make([]byte, MaxReaderBuffer+1)); err == nil {
		t.Error("oversized reader buffer accepted")
	}
}

func TestNewFlagsAndFeedbackLayout(t *testing.T) {
	m := Media{Stream: 1, FrameID: 9, FragCount: 1, Recovery: true, LongTerm: true, Face: true, RefDelta: 7, Payload: []byte{1}}
	b := m.Append(nil)
	if b[10] != FlagRecovery|FlagLongTerm|FlagFace || b[11] != 7 {
		t.Fatal(hex.EncodeToString(b))
	}
	back, err := ParseMedia(b)
	if err != nil || !reflect.DeepEqual(back, m) {
		t.Fatalf("%v %+v", err, back)
	}
	ack, _ := Append(nil, Ack{Stream: 2, Frames: []uint16{5}})
	nack, _ := Append(nil, Nack{Stream: 2, Seqs: []uint16{1, 2}})
	fb := AppendFeedback(nil, append(ack, nack...))
	if hex.EncodeToString(fb[:4+3+4]) != "13000000"+"0f0004"+"02010005" {
		t.Fatal(hex.EncodeToString(fb))
	}
	msgs, err := ParseFeedback(fb)
	if err != nil || len(msgs) != 2 || msgs[0].Type() != MsgAck || msgs[1].Type() != MsgNack {
		t.Fatalf("%v %+v", err, msgs)
	}
	join, _ := Append(nil, Join{Ticket: "x"})
	for name, bad := range map[string][]byte{
		"empty":       AppendFeedback(nil, nil),
		"join inside": AppendFeedback(nil, join),
		"truncated":   AppendFeedback(nil, ack[:len(ack)-1]),
		"reserved":    append([]byte{0x13, 1, 0, 0}, ack...),
	} {
		if _, err := ParseFeedback(bad); err == nil {
			t.Errorf("%s accepted", name)
		}
	}
	bad := []Message{
		Publish{Stream: 3, Kind: TrackAudio, Layers: 1, Track: 3, Rank: 1},
		Publish{Stream: 3, Kind: TrackVideo, Layers: 1, Track: 2, Rank: MaxEncodings},
		Publish{Stream: 3, Kind: TrackVideo, Layers: 1, Rank: 1},
		Ack{Stream: 1},
		Ack{Stream: 1, Frames: make([]uint16, MaxAck+1)},
	}
	for _, m := range bad {
		if _, err := Append(nil, m); err == nil {
			t.Errorf("%+v encoded", m)
		}
	}
	trail, _ := Append(nil, Publish{Stream: 3, Kind: TrackVideo, Layers: 1, Track: 3, Rank: 1})
	trail[len(trail)-2] = 0
	if _, _, err := Decode(trail); err == nil {
		t.Error("an encoding of track 0 decoded")
	}
}

func FuzzParseFeedback(f *testing.F) {
	ack, _ := Append(nil, Ack{Stream: 2, Frames: []uint16{5, 9}})
	rep, _ := Append(nil, sampleMessages()[12])
	f.Add(AppendFeedback(nil, append(ack, rep...)))
	f.Fuzz(func(t *testing.T, b []byte) {
		msgs, err := ParseFeedback(b)
		if err != nil {
			return
		}
		var again []byte
		for _, m := range msgs {
			if !FeedbackType(m.Type()) {
				t.Fatalf("%T inside feedback", m)
			}
			again, err = Append(again, m)
			if err != nil {
				t.Fatal(err)
			}
		}
		if !bytes.Equal(AppendFeedback(nil, again), b) {
			t.Fatal("feedback does not round-trip")
		}
	})
}

func FuzzParseDatagram(f *testing.F) {
	f.Add((&Media{Stream: 1, FragCount: 2, FragIndex: 1, Layer: 1, RefDelta: 1, Payload: []byte("x")}).Append(nil))
	f.Add((&Repair{K: 4, M: 2, Scheme: SchemeRS, Offsets: []uint8{0, 1, 5, 7}, Symbol: make([]byte, 20)}).Append(nil))
	f.Add(AppendPadding(nil, 1, 30))
	f.Fuzz(func(t *testing.T, b []byte) {
		k, err := Kind(b)
		if err != nil {
			return
		}
		switch k {
		case KindMedia:
			m, err := ParseMedia(b)
			if err == nil && !bytes.Equal(m.Append(nil), b) {
				t.Fatal("media does not round-trip")
			}
		case KindRepair:
			r, err := ParseRepair(b)
			if err == nil && !bytes.Equal(r.Append(nil), b) {
				t.Fatal("repair does not round-trip")
			}
		case KindPadding:
			ParsePadding(b)
		}
	})
}

func FuzzDecodeControl(f *testing.F) {
	for _, m := range sampleMessages() {
		b, _ := Append(nil, m)
		f.Add(b)
	}
	f.Fuzz(func(t *testing.T, b []byte) {
		m, n, err := Decode(b)
		if err != nil {
			return
		}
		again, err := Append(nil, m)
		if err != nil {
			t.Fatalf("decoded %T does not encode: %v", m, err)
		}
		if !bytes.Equal(again, b[:n]) {
			t.Fatalf("%T is not canonical", m)
		}
	})
}

func FuzzControlReader(f *testing.F) {
	var all []byte
	for _, m := range sampleMessages() {
		b, _ := Append(nil, m)
		all = append(all, b...)
	}
	f.Add(all, uint8(7))
	f.Fuzz(func(t *testing.T, b []byte, chunk uint8) {
		step := int(chunk) + 1
		var r Reader
		for i := 0; i < len(b); i += step {
			if r.Write(b[i:min(i+step, len(b))]) != nil {
				return
			}
			for {
				m, err := r.Next()
				if err != nil {
					return
				}
				if m == nil {
					break
				}
			}
		}
	})
}
