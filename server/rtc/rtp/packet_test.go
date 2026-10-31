package rtp

import (
	"bytes"
	"testing"
)

func TestExtensionElementsOneAndTwoByte(t *testing.T) {
	h, err := ParseHeader(handBuiltPacket())
	if err != nil {
		t.Fatal(err)
	}
	if v, ok := ExtensionElement(h, 1); !ok || !bytes.Equal(v, []byte{0xAA}) {
		t.Fatalf("id 1 %x %v", v, ok)
	}
	if v, ok := ExtensionElement(h, 2); !ok || !bytes.Equal(v, []byte{0xBB, 0xCC}) {
		t.Fatalf("id 2 %x %v", v, ok)
	}
	if seq, ok := TransportSequence(h, 2); !ok || seq != 0xBBCC {
		t.Fatalf("transport seq %x %v", seq, ok)
	}
	if _, ok := ExtensionElement(h, 3); ok {
		t.Fatal("absent id found")
	}
	if _, ok := TransportSequence(h, 1); ok {
		t.Fatal("one-byte element read as a sequence")
	}
	two := Header{ExtensionProfile: 0x1000, Extension: []byte{0x00, 20, 2, 0x12, 0x34, 7, 0, 0}}
	if seq, ok := TransportSequence(two, 20); !ok || seq != 0x1234 {
		t.Fatalf("two-byte %x %v", seq, ok)
	}
	for _, broken := range [][]byte{{0x15, 1}, {20, 9, 1}, {0xF0, 0, 0, 0}} {
		ExtensionElement(Header{ExtensionProfile: 0xBEDE, Extension: broken}, 1)
		ExtensionElement(Header{ExtensionProfile: 0x1000, Extension: broken}, 20)
	}
	if _, ok := ExtensionElement(Header{ExtensionProfile: 0x1234, Extension: []byte{0x10, 1}}, 1); ok {
		t.Fatal("unknown profile read")
	}
}

func TestPacketMarshalStampsTransportSequence(t *testing.T) {
	for _, id := range []uint8{3, 14, 20} {
		p := Packet{Marker: true, PayloadType: 96, SequenceNumber: 0xfffe, Timestamp: 90000, SSRC: 0xdeadbeef,
			CSRC: []uint32{7}, TransportID: id, TransportSeq: 4321, Payload: []byte("frame"), Padding: 3}
		b := p.Marshal(nil)
		if len(b) != p.Size() {
			t.Fatalf("size %d want %d", len(b), p.Size())
		}
		h, err := ParseHeader(b)
		if err != nil {
			t.Fatal(err)
		}
		if !h.Marker || h.PayloadType != 96 || h.SequenceNumber != 0xfffe || h.Timestamp != 90000 || h.SSRC != 0xdeadbeef ||
			len(h.CSRC) != 1 || h.CSRC[0] != 7 || h.PaddingLength != 3 {
			t.Fatalf("header %+v", h)
		}
		if seq, ok := TransportSequence(h, id); !ok || seq != 4321 {
			t.Fatalf("id %d seq %d %v", id, seq, ok)
		}
		if !bytes.Equal(Payload(b, h), []byte("frame")) {
			t.Fatalf("payload %q", Payload(b, h))
		}
	}
	plain := (&Packet{PayloadType: 111, SequenceNumber: 1, SSRC: 2, Payload: []byte{1, 2}}).Marshal(nil)
	if !bytes.Equal(plain, []byte{0x80, 111, 0, 1, 0, 0, 0, 0, 0, 0, 0, 2, 1, 2}) {
		t.Fatalf("plain % x", plain)
	}
}

func TestRTXFollowsRFC4588(t *testing.T) {
	original := Packet{Marker: true, PayloadType: 96, SequenceNumber: 0xabcd, Timestamp: 1234567, SSRC: 1111,
		Payload: []byte{0x90, 0x80, 0x01, 'v', 'p', '8'}, Padding: 4}
	raw := original.Marshal(nil)
	h, _ := ParseHeader(raw)
	rtx := Packet{Marker: h.Marker, PayloadType: 97, SequenceNumber: 7, Timestamp: h.Timestamp, SSRC: 2222,
		TransportID: 3, TransportSeq: 99, Payload: RTXPayload(nil, h.SequenceNumber, Payload(raw, h))}
	b := rtx.Marshal(nil)
	want := []byte{
		0x90, 0x80 | 97, 0x00, 0x07,
		0x00, 0x12, 0xd6, 0x87,
		0x00, 0x00, 0x08, 0xae,
		0xbe, 0xde, 0x00, 0x01,
		0x31, 0x00, 0x63, 0x00,
		0xab, 0xcd,
		0x90, 0x80, 0x01, 'v', 'p', '8',
	}
	if !bytes.Equal(b, want) {
		t.Fatalf("rtx\n got % x\nwant % x", b, want)
	}
	rh, _ := ParseHeader(b)
	osn, payload, err := ParseRTX(Payload(b, rh))
	if err != nil || osn != 0xabcd || !bytes.Equal(payload, original.Payload) {
		t.Fatalf("parse rtx %x %q %v", osn, payload, err)
	}
	if _, _, err := ParseRTX([]byte{1}); err == nil {
		t.Fatal("short rtx accepted")
	}
}

func TestReceiverReportBlocks(t *testing.T) {
	blocks := []ReportBlock{
		{SSRC: 0x01020304, FractionLost: 25, Lost: 1000, HighestSeq: 0x0001ffff, Jitter: 90, LSR: 0xaabbccdd, DLSR: 65536},
		{SSRC: 5, FractionLost: 0, Lost: -3, HighestSeq: 2, Jitter: 0},
	}
	p := NewReceiverReport(0xcafebabe, blocks...)
	if len(p) != 8+48 || Count(p) != 2 || PacketType(p) != TypeRR || SenderSSRC(p) != 0xcafebabe {
		t.Fatalf("rr % x", p)
	}
	if !bytes.Equal(p[8:16], []byte{1, 2, 3, 4, 25, 0x00, 0x03, 0xe8}) {
		t.Fatalf("block start % x", p[8:16])
	}
	if !bytes.Equal(p[36:40], []byte{0, 0xff, 0xff, 0xfd}) {
		t.Fatalf("negative lost % x", p[36:40])
	}
	back, err := ParseReportBlocks(p)
	if err != nil || len(back) != 2 || back[0] != blocks[0] || back[1] != blocks[1] {
		t.Fatalf("parsed %+v %v", back, err)
	}
	huge := NewReceiverReport(1, ReportBlock{Lost: 1 << 30})
	if back, _ := ParseReportBlocks(huge); back[0].Lost != 0x7fffff {
		t.Fatalf("lost not clamped %d", back[0].Lost)
	}
	if _, err := ParseReportBlocks(NewPLI(1, 2)); err == nil {
		t.Fatal("pli parsed as report")
	}
	sr := make([]byte, 28)
	sr[0], sr[1], sr[3] = 0x80, TypeSR, 6
	sr[8], sr[15] = 0x11, 0x22
	if ntp, ok := SenderReportNTP(sr); !ok || ntp != 0x1100000000000022 {
		t.Fatalf("ntp %x %v", ntp, ok)
	}
}
