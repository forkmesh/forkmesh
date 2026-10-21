package rtp

import (
	"bytes"
	"encoding/binary"
	"math/rand"
	"slices"
	"testing"
)

func handBuiltPacket() []byte {
	b := []byte{
		0xB2, 0xE0, 0x12, 0x34,
		0x00, 0x01, 0x02, 0x03,
		0xDE, 0xAD, 0xBE, 0xEF,
		0x11, 0x11, 0x11, 0x11,
		0x22, 0x22, 0x22, 0x22,
		0xBE, 0xDE, 0x00, 0x02,
		0x10, 0xAA, 0x00, 0x00,
		0x21, 0xBB, 0xCC, 0x00,
		'p', 'a', 'y', 'l', 'o', 'a', 'd',
		0x00, 0x00, 0x03,
	}
	return b
}

func TestParseHandBuilt(t *testing.T) {
	b := handBuiltPacket()
	h, err := ParseHeader(b)
	if err != nil {
		t.Fatal(err)
	}
	if !h.Marker || h.PayloadType != 96 || h.SequenceNumber != 0x1234 || h.Timestamp != 0x00010203 || h.SSRC != 0xDEADBEEF {
		t.Fatalf("fixed header %+v", h)
	}
	if !slices.Equal(h.CSRC, []uint32{0x11111111, 0x22222222}) {
		t.Fatalf("csrc %x", h.CSRC)
	}
	if h.ExtensionProfile != 0xBEDE {
		t.Fatalf("profile %x", h.ExtensionProfile)
	}
	if !bytes.Equal(h.Extension, []byte{0x10, 0xAA, 0x00, 0x00, 0x21, 0xBB, 0xCC, 0x00}) {
		t.Fatalf("extension %x", h.Extension)
	}
	if h.PayloadOffset != 32 || h.PaddingLength != 3 {
		t.Fatalf("offset %d padding %d", h.PayloadOffset, h.PaddingLength)
	}
	if got := string(b[h.PayloadOffset : len(b)-h.PaddingLength]); got != "payload" {
		t.Fatalf("payload %q", got)
	}
}

func TestParseMinimal(t *testing.T) {
	b := []byte{0x80, 0x6F, 0xFF, 0xFF, 0, 0, 0, 1, 0, 0, 0, 2, 9}
	h, err := ParseHeader(b)
	if err != nil {
		t.Fatal(err)
	}
	if h.Marker || h.PayloadType != 111 || h.SequenceNumber != 65535 || h.SSRC != 2 || h.CSRC != nil || h.Extension != nil || h.PayloadOffset != 12 || h.PaddingLength != 0 {
		t.Fatalf("%+v", h)
	}
}

func TestSetSSRCAndPayloadType(t *testing.T) {
	b := handBuiltPacket()
	SetSSRC(b, 0x01020304)
	SetPayloadType(b, 100)
	h, err := ParseHeader(b)
	if err != nil {
		t.Fatal(err)
	}
	if h.SSRC != 0x01020304 || h.PayloadType != 100 || !h.Marker {
		t.Fatalf("%+v", h)
	}
	SetPayloadType(b, 0xFF)
	if b[1] != 0xFF {
		t.Fatalf("pt byte %x", b[1])
	}
	b[1] = 0x00
	SetPayloadType(b, 96)
	if b[1] != 96 {
		t.Fatalf("pt byte %x", b[1])
	}
	SetSSRC(nil, 1)
	SetSSRC(make([]byte, 11), 1)
	SetPayloadType(nil, 1)
	SetPayloadType([]byte{0x80}, 1)
}

func TestIsRTCP(t *testing.T) {
	for _, c := range []struct {
		b    []byte
		want bool
	}{
		{NewPLI(1, 2), true},
		{NewReceiverReport(1), true},
		{[]byte{0x80, 192}, true},
		{[]byte{0x80, 223}, true},
		{[]byte{0x80, 191}, false},
		{[]byte{0x80, 224}, false},
		{[]byte{0x80, 96}, false},
		{[]byte{0x80, 0xE0}, false},
		{[]byte{0x80}, false},
		{nil, false},
	} {
		if got := IsRTCP(c.b); got != c.want {
			t.Errorf("IsRTCP(%x) = %v", c.b, got)
		}
	}
}

func TestParseMalformed(t *testing.T) {
	base := handBuiltPacket()
	cases := map[string][]byte{
		"empty":           nil,
		"short":           base[:11],
		"version 1":       append([]byte{0x40}, base[1:]...),
		"version 0":       append([]byte{0x00}, base[1:]...),
		"csrc overrun":    {0x8F, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4},
		"ext no words":    {0x90, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xBE, 0xDE},
		"ext overrun":     {0x90, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xBE, 0xDE, 0x00, 0x02, 1, 2, 3, 4},
		"pad zero":        {0xA0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0},
		"pad too long":    {0xA0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 3},
		"pad no payload":  {0xA0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
		"pad past ext":    {0xB0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xBE, 0xDE, 0, 0, 2},
		"pad huge":        append(slices.Clone(base[:len(base)-1]), 0xFF),
		"truncated ext":   base[:28],
		"truncated csrcs": base[:18],
	}
	for name, b := range cases {
		if _, err := ParseHeader(b); err == nil {
			t.Errorf("%s: no error", name)
		}
	}
	h, err := ParseHeader([]byte{0xA0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2})
	if err != nil || h.PaddingLength != 2 || h.PayloadOffset != 12 {
		t.Fatalf("all-padding packet %+v %v", h, err)
	}
}

func TestParseTruncationsNeverPanic(t *testing.T) {
	base := handBuiltPacket()
	for i := 0; i <= len(base); i++ {
		ParseHeader(base[:i])
	}
	r := rand.New(rand.NewSource(1))
	for i := 0; i < 20000; i++ {
		b := make([]byte, r.Intn(64))
		r.Read(b)
		if len(b) > 0 && r.Intn(2) == 0 {
			b[0] = b[0]&0x3F | 0x80
		}
		h, err := ParseHeader(b)
		if err == nil {
			if h.PayloadOffset+h.PaddingLength > len(b) {
				t.Fatalf("offsets out of range %x %+v", b, h)
			}
		}
		SplitCompound(b)
		ParseNACK(b)
		PacketType(b)
		Count(b)
		SenderSSRC(b)
		MediaSSRC(b)
		c := slices.Clone(b)
		SetMediaSSRC(c, 7)
		SetSSRC(c, 7)
		SetPayloadType(c, 7)
		IsRTCP(b)
	}
}

func TestPLI(t *testing.T) {
	p := NewPLI(0xAABBCCDD, 0x11223344)
	want := []byte{0x81, 206, 0x00, 0x02, 0xAA, 0xBB, 0xCC, 0xDD, 0x11, 0x22, 0x33, 0x44}
	if !bytes.Equal(p, want) {
		t.Fatalf("%x", p)
	}
	if PacketType(p) != TypePSFB || Count(p) != FmtPLI || SenderSSRC(p) != 0xAABBCCDD || MediaSSRC(p) != 0x11223344 {
		t.Fatal("accessors")
	}
	SetMediaSSRC(p, 5)
	if MediaSSRC(p) != 5 {
		t.Fatal("set media")
	}
}

func TestFIR(t *testing.T) {
	p := NewFIR(1, 0x01020304, 9)
	want := []byte{0x84, 206, 0x00, 0x04, 0, 0, 0, 1, 0, 0, 0, 0, 0x01, 0x02, 0x03, 0x04, 9, 0, 0, 0}
	if !bytes.Equal(p, want) {
		t.Fatalf("%x", p)
	}
	if PacketType(p) != TypePSFB || Count(p) != FmtFIR || SenderSSRC(p) != 1 || MediaSSRC(p) != 0x01020304 {
		t.Fatal("accessors")
	}
	SetMediaSSRC(p, 0x0A0B0C0D)
	if MediaSSRC(p) != 0x0A0B0C0D || binary.BigEndian.Uint32(p[8:12]) != 0 {
		t.Fatalf("set media %x", p)
	}
}

func TestReceiverReport(t *testing.T) {
	p := NewReceiverReport(0x01020304)
	if !bytes.Equal(p, []byte{0x80, 201, 0, 1, 1, 2, 3, 4}) {
		t.Fatalf("%x", p)
	}
	if PacketType(p) != TypeRR || Count(p) != 0 || SenderSSRC(p) != 0x01020304 {
		t.Fatal("accessors")
	}
	parts, err := SplitCompound(p)
	if err != nil || len(parts) != 1 {
		t.Fatal(parts, err)
	}
}

func decodeREMB(t *testing.T, p []byte) (uint64, []uint32) {
	t.Helper()
	if len(p) < 20 || PacketType(p) != TypePSFB || Count(p) != FmtREMB || string(p[12:16]) != "REMB" {
		t.Fatalf("not remb %x", p)
	}
	if binary.BigEndian.Uint32(p[8:12]) != 0 {
		t.Fatal("media ssrc not zero")
	}
	n := int(p[16])
	if (int(binary.BigEndian.Uint16(p[2:4]))+1)*4 != len(p) || len(p) != 20+4*n {
		t.Fatalf("length %x", p)
	}
	exp := p[17] >> 2
	mant := uint64(p[17]&3)<<16 | uint64(p[18])<<8 | uint64(p[19])
	var ssrcs []uint32
	for i := 0; i < n; i++ {
		ssrcs = append(ssrcs, binary.BigEndian.Uint32(p[20+4*i:]))
	}
	return mant << exp, ssrcs
}

func TestREMB(t *testing.T) {
	for _, bps := range []uint64{0, 1, 262143, 262144, 1_500_000, 2_500_000, 10_000_000_000, 1 << 63} {
		p := NewREMB(0x0A0B0C0D, bps, []uint32{1, 2})
		if SenderSSRC(p) != 0x0A0B0C0D || MediaSSRC(p) != 0 {
			t.Fatal("ssrc fields")
		}
		got, ssrcs := decodeREMB(t, p)
		if !slices.Equal(ssrcs, []uint32{1, 2}) {
			t.Fatalf("ssrcs %v", ssrcs)
		}
		if got > bps {
			t.Fatalf("%d decoded as %d, above input", bps, got)
		}
		if bps-got > bps>>17 {
			t.Fatalf("%d decoded as %d, off by more than mantissa precision", bps, got)
		}
	}
	p := NewREMB(1, 1_500_000, nil)
	got, _ := decodeREMB(t, p)
	if got < 1_499_990 || got > 1_500_000 {
		t.Fatalf("1.5 Mbps decoded as %d", got)
	}
	many := make([]uint32, 300)
	p = NewREMB(1, 1000, many)
	if _, s := decodeREMB(t, p); len(s) != 255 {
		t.Fatalf("ssrc cap %d", len(s))
	}
}

func TestNACKWrap(t *testing.T) {
	lost := []uint16{65534, 65535, 0, 1, 17}
	p := NewNACK(3, 4, lost)
	if PacketType(p) != TypeRTPFB || Count(p) != FmtNACK || SenderSSRC(p) != 3 || MediaSSRC(p) != 4 {
		t.Fatal("header")
	}
	if len(p) != 20 {
		t.Fatalf("want two pairs, got %x", p)
	}
	pairs := [2][2]uint16{
		{binary.BigEndian.Uint16(p[12:14]), binary.BigEndian.Uint16(p[14:16])},
		{binary.BigEndian.Uint16(p[16:18]), binary.BigEndian.Uint16(p[18:20])},
	}
	if pairs != [2][2]uint16{{65534, 0b111}, {17, 0}} {
		t.Fatalf("pairs %v", pairs)
	}
	media, got, err := ParseNACK(p)
	if err != nil || media != 4 {
		t.Fatal(media, err)
	}
	if !slices.Equal(got, lost) {
		t.Fatalf("got %v", got)
	}
	shuffled := []uint16{17, 1, 65535, 0, 65534, 1}
	if !bytes.Equal(NewNACK(3, 4, shuffled), p) {
		t.Fatalf("unsorted input packs differently")
	}
}

func TestNACKPacking(t *testing.T) {
	for _, lost := range [][]uint16{
		{5},
		{0, 16},
		{0, 17},
		{100, 101, 102, 200, 216, 217, 233},
		{65535, 15},
		{65535, 16},
		{30000, 1, 2},
	} {
		p := NewNACK(1, 2, lost)
		_, got, err := ParseNACK(p)
		if err != nil {
			t.Fatal(err)
		}
		if !slices.Equal(got, circularOrder(lost)) {
			t.Fatalf("%v -> %v", lost, got)
		}
	}
	if n := len(NewNACK(1, 2, []uint16{0, 16})); n != 16 {
		t.Fatalf("0,16 should fit one pair, %d bytes", n)
	}
	if n := len(NewNACK(1, 2, []uint16{0, 17})); n != 20 {
		t.Fatalf("0,17 should need two pairs, %d bytes", n)
	}
	if NewNACK(1, 2, nil) != nil {
		t.Fatal("empty list should give nil")
	}
	r := rand.New(rand.NewSource(2))
	for i := 0; i < 500; i++ {
		start := uint16(r.Intn(65536))
		var lost []uint16
		for j := 0; j < 200; j++ {
			if r.Intn(3) == 0 {
				lost = append(lost, start+uint16(j))
			}
		}
		if len(lost) == 0 {
			continue
		}
		_, got, err := ParseNACK(NewNACK(1, 2, lost))
		if err != nil || !slices.Equal(got, lost) {
			t.Fatalf("start %d: %v vs %v (%v)", start, lost, got, err)
		}
	}
}

func TestParseNACKMalformed(t *testing.T) {
	good := NewNACK(1, 2, []uint16{10, 11})
	cases := map[string][]byte{
		"short":       good[:11],
		"no fci":      {0x81, 205, 0, 2, 0, 0, 0, 1, 0, 0, 0, 2},
		"wrong type":  NewPLI(1, 2),
		"wrong fmt":   {0x83, 205, 0, 3, 0, 0, 0, 1, 0, 0, 0, 2, 0, 1, 0, 0},
		"version":     append([]byte{0x41}, good[1:]...),
		"len overrun": {0x81, 205, 0, 9, 0, 0, 0, 1, 0, 0, 0, 2, 0, 1, 0, 0},
	}
	for name, b := range cases {
		if _, _, err := ParseNACK(b); err == nil {
			t.Errorf("%s: no error", name)
		}
	}
}

func TestSplitCompound(t *testing.T) {
	sr := make([]byte, 28)
	sr[0] = 0x80
	sr[1] = TypeSR
	binary.BigEndian.PutUint16(sr[2:4], 6)
	binary.BigEndian.PutUint32(sr[4:8], 0x11111111)
	sdes := []byte{0x81, TypeSDES, 0, 3, 0x11, 0x11, 0x11, 0x11, 1, 4, 'a', 'b', 'c', 'd', 0, 0}
	pli := NewPLI(0x11111111, 0x22222222)
	compound := slices.Concat(sr, sdes, pli)
	parts, err := SplitCompound(compound)
	if err != nil {
		t.Fatal(err)
	}
	if len(parts) != 3 || !bytes.Equal(parts[0], sr) || !bytes.Equal(parts[1], sdes) || !bytes.Equal(parts[2], pli) {
		t.Fatalf("%x", parts)
	}
	if PacketType(parts[0]) != TypeSR || PacketType(parts[1]) != TypeSDES || Count(parts[1]) != 1 || PacketType(parts[2]) != TypePSFB {
		t.Fatal("types")
	}
	if MediaSSRC(parts[2]) != 0x22222222 || SenderSSRC(parts[0]) != 0x11111111 {
		t.Fatal("ssrcs")
	}
	parts[0] = append(parts[0], 0xFF)
	if compound[28] != sdes[0] {
		t.Fatal("append through a part overwrote the next packet")
	}
	for name, b := range map[string][]byte{
		"empty":       nil,
		"trailing":    append(slices.Clone(compound), 0x80),
		"trailing 4":  append(slices.Clone(compound), 0x80, 0xC9, 0x00, 0x01),
		"truncated":   compound[:len(compound)-1],
		"bad version": slices.Concat(sr, []byte{0x40, TypeRR, 0, 1, 0, 0, 0, 0}),
		"len overrun": {0x80, TypeRR, 0x00, 0x05, 0, 0, 0, 0},
	} {
		if _, err := SplitCompound(b); err == nil {
			t.Errorf("%s: no error", name)
		}
	}
}

func TestAccessorsShortInput(t *testing.T) {
	if PacketType(nil) != 0 || Count(nil) != 0 || SenderSSRC([]byte{0x80, 201, 0, 1}) != 0 || MediaSSRC(NewReceiverReport(1)) != 0 {
		t.Fatal("short accessors")
	}
	SetMediaSSRC(nil, 1)
	SetMediaSSRC(make([]byte, 11), 1)
}
