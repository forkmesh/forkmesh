package stun

import (
	"bytes"
	"encoding/binary"
	"encoding/hex"
	"math/rand"
	"net/netip"
	"strings"
	"testing"
)

const vectorPassword = "VOkJxbRl1RmTxUk/WvJxBt"  // forkmesh-secret-scan:ignore-line

const sampleRequestHex = `
00 01 00 58
21 12 a4 42
b7 e7 a7 01
bc 34 d6 86
fa 87 df ae
80 22 00 10
53 54 55 4e
20 74 65 73
74 20 63 6c
69 65 6e 74
00 24 00 04
6e 00 01 ff
80 29 00 08
93 2f f9 b1
51 26 3b 36
00 06 00 09
65 76 74 6a
3a 68 36 76
59 20 20 20
00 08 00 14
9a ea a7 0c
bf d8 cb 56
78 1e f2 b5
b2 d3 f2 49
c1 b5 71 a2
80 28 00 04
e5 7a 3b cf
`

const sampleIPv4ResponseHex = `
01 01 00 3c
21 12 a4 42
b7 e7 a7 01
bc 34 d6 86
fa 87 df ae
80 22 00 0b
74 65 73 74
20 76 65 63
74 6f 72 20
00 20 00 08
00 01 a1 47
e1 12 a6 43
00 08 00 14
2b 91 f5 99
fd 9e 90 c3
8c 74 89 f9
2a f9 ba 53
f0 6b e7 d7
80 28 00 04
c0 7d 4c 96
`

const sampleIPv6ResponseHex = `
01 01 00 48
21 12 a4 42
b7 e7 a7 01
bc 34 d6 86
fa 87 df ae
80 22 00 0b
74 65 73 74
20 76 65 63
74 6f 72 20
00 20 00 14
00 02 a1 47
01 13 a9 fa
a5 d3 f1 79
bc 25 f4 b5
be d2 b9 d9
00 08 00 14
a3 82 95 4e
4b e6 7b f1
17 84 c9 7c
82 92 c2 75
bf e3 ed 41
80 28 00 04
c8 fb 0b 4c
`

const attrSoftware = 0x8022

var vectorTID = [12]byte{0xb7, 0xe7, 0xa7, 0x01, 0xbc, 0x34, 0xd6, 0x86, 0xfa, 0x87, 0xdf, 0xae}

func decodeHex(t testing.TB, s string) []byte {
	t.Helper()
	b, err := hex.DecodeString(strings.Join(strings.Fields(s), ""))
	if err != nil {
		t.Fatal(err)
	}
	return b
}

func vectors(t testing.TB) map[string][]byte {
	return map[string][]byte{
		"request":      decodeHex(t, sampleRequestHex),
		"ipv4response": decodeHex(t, sampleIPv4ResponseHex),
		"ipv6response": decodeHex(t, sampleIPv6ResponseHex),
	}
}

func TestParseSampleRequest(t *testing.T) {
	raw := decodeHex(t, sampleRequestHex)
	if !IsMessage(raw) {
		t.Fatal("IsMessage false")
	}
	m, err := Parse(raw)
	if err != nil {
		t.Fatal(err)
	}
	if m.Type != BindingRequest {
		t.Fatalf("type %#x", m.Type)
	}
	if m.TransactionID != vectorTID {
		t.Fatalf("tid %x", m.TransactionID)
	}
	if len(m.Attributes) != 6 {
		t.Fatalf("attributes %d", len(m.Attributes))
	}
	checks := []struct {
		typ  uint16
		want []byte
	}{
		{attrSoftware, []byte("STUN test client")},
		{AttrPriority, []byte{0x6e, 0x00, 0x01, 0xff}},
		{AttrICEControlled, []byte{0x93, 0x2f, 0xf9, 0xb1, 0x51, 0x26, 0x3b, 0x36}},
		{AttrUsername, []byte("evtj:h6vY")},
		{AttrFingerprint, []byte{0xe5, 0x7a, 0x3b, 0xcf}},
	}
	for _, c := range checks {
		v, ok := m.Get(c.typ)
		if !ok || !bytes.Equal(v, c.want) {
			t.Fatalf("attr %#x = %x %v", c.typ, v, ok)
		}
	}
	if v, ok := m.Get(AttrMessageIntegrity); !ok || len(v) != 20 {
		t.Fatalf("integrity %x", v)
	}
	if _, ok := m.Get(AttrUseCandidate); ok {
		t.Fatal("unexpected USE-CANDIDATE")
	}
}

func TestParseSampleResponses(t *testing.T) {
	cases := []struct {
		hex  string
		want netip.AddrPort
	}{
		{sampleIPv4ResponseHex, netip.MustParseAddrPort("192.0.2.1:32853")},
		{sampleIPv6ResponseHex, netip.MustParseAddrPort("[2001:db8:1234:5678:11:2233:4455:6677]:32853")},
	}
	for _, c := range cases {
		raw := decodeHex(t, c.hex)
		m, err := Parse(raw)
		if err != nil {
			t.Fatal(err)
		}
		if m.Type != BindingSuccess || m.TransactionID != vectorTID {
			t.Fatalf("header %#x %x", m.Type, m.TransactionID)
		}
		sw, ok := m.Get(attrSoftware)
		if !ok || string(sw) != "test vector" {
			t.Fatalf("software %q", sw)
		}
		v, ok := m.Get(AttrXORMappedAddress)
		if !ok {
			t.Fatal("no XOR-MAPPED-ADDRESS")
		}
		got, err := DecodeXORAddress(v, m.TransactionID)
		if err != nil {
			t.Fatal(err)
		}
		if got != c.want {
			t.Fatalf("address %v want %v", got, c.want)
		}
	}
}

func TestVectorIntegrityAndFingerprint(t *testing.T) {
	for name, raw := range vectors(t) {
		if !CheckIntegrity(raw, []byte(vectorPassword)) {
			t.Fatalf("%s: integrity false with right key", name)
		}
		if CheckIntegrity(raw, []byte(vectorPassword+"x")) {
			t.Fatalf("%s: integrity true with wrong key", name)
		}
		if CheckIntegrity(raw, nil) || CheckIntegrity(raw, []byte{}) {
			t.Fatalf("%s: integrity true with empty key", name)
		}
		if !CheckFingerprint(raw) {
			t.Fatalf("%s: fingerprint false", name)
		}
		for i := 0; i < len(raw)-8; i++ {
			for _, bit := range []byte{0x01, 0x80} {
				flipped := append([]byte(nil), raw...)
				flipped[i] ^= bit
				if CheckIntegrity(flipped, []byte(vectorPassword)) {
					t.Fatalf("%s: integrity true after flipping byte %d", name, i)
				}
			}
		}
		for i := 0; i < len(raw); i++ {
			flipped := append([]byte(nil), raw...)
			flipped[i] ^= 0x04
			if CheckFingerprint(flipped) {
				t.Fatalf("%s: fingerprint true after flipping byte %d", name, i)
			}
		}
		fpOnly := append([]byte(nil), raw...)
		fpOnly[len(fpOnly)-1] ^= 0xff
		if !CheckIntegrity(fpOnly, []byte(vectorPassword)) {
			t.Fatalf("%s: integrity must not cover FINGERPRINT", name)
		}
	}
}

func TestIntegrityWithoutFingerprint(t *testing.T) {
	m := &Message{Type: BindingRequest, TransactionID: vectorTID}
	m.Add(AttrUsername, []byte("a:b"))
	raw := m.Encode([]byte("pw"), false)
	if !CheckIntegrity(raw, []byte("pw")) {
		t.Fatal("integrity false")
	}
	if CheckFingerprint(raw) {
		t.Fatal("fingerprint true without FINGERPRINT")
	}
	if CheckIntegrity(m.Encode(nil, true), []byte("pw")) {
		t.Fatal("integrity true without MESSAGE-INTEGRITY")
	}
}

func TestEncodeRoundTrip(t *testing.T) {
	key := []byte(vectorPassword)
	addrs := []netip.AddrPort{
		netip.MustParseAddrPort("192.0.2.1:32853"),
		netip.MustParseAddrPort("[2001:db8:1234:5678:11:2233:4455:6677]:32853"),
		netip.MustParseAddrPort("[::ffff:10.1.2.3]:9"),
		netip.MustParseAddrPort("203.0.113.200:65535"),
	}
	for _, addr := range addrs {
		m := &Message{Type: BindingSuccess, TransactionID: vectorTID}
		m.AddXORMappedAddress(addr)
		raw := m.Encode(key, true)
		if !IsMessage(raw) {
			t.Fatalf("%v: IsMessage false", addr)
		}
		p, err := Parse(raw)
		if err != nil {
			t.Fatalf("%v: %v", addr, err)
		}
		if !CheckIntegrity(raw, key) || !CheckFingerprint(raw) {
			t.Fatalf("%v: checks failed", addr)
		}
		if p.Type != BindingSuccess || p.TransactionID != vectorTID {
			t.Fatalf("%v: header", addr)
		}
		if len(p.Attributes) != 3 || p.Attributes[1].Type != AttrMessageIntegrity || p.Attributes[2].Type != AttrFingerprint {
			t.Fatalf("%v: attributes %v", addr, p.Attributes)
		}
		v, _ := p.Get(AttrXORMappedAddress)
		got, err := DecodeXORAddress(v, p.TransactionID)
		if err != nil {
			t.Fatal(err)
		}
		want := netip.AddrPortFrom(addr.Addr().Unmap(), addr.Port())
		if got != want {
			t.Fatalf("got %v want %v", got, want)
		}
	}
}

func TestEncodeVectorAddressBytes(t *testing.T) {
	m := &Message{Type: BindingSuccess, TransactionID: vectorTID}
	m.AddXORMappedAddress(netip.MustParseAddrPort("192.0.2.1:32853"))
	m.AddXORMappedAddress(netip.MustParseAddrPort("[2001:db8:1234:5678:11:2233:4455:6677]:32853"))
	want4 := decodeHex(t, "00 01 a1 47 e1 12 a6 43")
	want6 := decodeHex(t, "00 02 a1 47 01 13 a9 fa a5 d3 f1 79 bc 25 f4 b5 be d2 b9 d9")
	if !bytes.Equal(m.Attributes[0].Value, want4) || !bytes.Equal(m.Attributes[1].Value, want6) {
		t.Fatalf("got %x %x", m.Attributes[0].Value, m.Attributes[1].Value)
	}
}

func TestEncodeReencodesVector(t *testing.T) {
	for name, raw := range vectors(t) {
		m, err := Parse(raw)
		if err != nil {
			t.Fatal(err)
		}
		out := m.Encode([]byte(vectorPassword), true)
		if len(out) != len(raw) {
			t.Fatalf("%s: length %d want %d", name, len(out), len(raw))
		}
		if !CheckIntegrity(out, []byte(vectorPassword)) || !CheckFingerprint(out) {
			t.Fatalf("%s: re-encoded checks failed", name)
		}
		plain := m.Encode(nil, false)
		p, err := Parse(plain)
		if err != nil {
			t.Fatal(err)
		}
		if _, ok := p.Get(AttrMessageIntegrity); ok {
			t.Fatalf("%s: stale MESSAGE-INTEGRITY kept", name)
		}
		if _, ok := p.Get(AttrFingerprint); ok {
			t.Fatalf("%s: stale FINGERPRINT kept", name)
		}
	}
}

func TestEncodePadding(t *testing.T) {
	m := &Message{Type: BindingRequest}
	m.Add(AttrUsername, []byte("abcde"))
	m.Add(AttrUseCandidate, nil)
	raw := m.Encode(nil, false)
	want := decodeHex(t, "00 01 00 10 21 12 a4 42 000000000000000000000000 00 06 00 05 61 62 63 64 65 00 00 00 00 25 00 00")
	if !bytes.Equal(raw, want) {
		t.Fatalf("got %x", raw)
	}
	p, err := Parse(raw)
	if err != nil {
		t.Fatal(err)
	}
	if v, ok := p.Get(AttrUseCandidate); !ok || len(v) != 0 {
		t.Fatal("USE-CANDIDATE lost")
	}
}

func TestEncodeDropsOversized(t *testing.T) {
	m := &Message{Type: BindingRequest}
	m.Add(AttrUsername, make([]byte, 0x10000))
	m.Add(0x8022, make([]byte, 0xFFF0))
	m.Add(AttrPriority, []byte{1, 2, 3, 4})
	raw := m.Encode([]byte("k"), true)
	if !IsMessage(raw) || !CheckIntegrity(raw, []byte("k")) || !CheckFingerprint(raw) {
		t.Fatal("oversized encode broke message")
	}
	p, err := Parse(raw)
	if err != nil {
		t.Fatal(err)
	}
	if _, ok := p.Get(AttrUsername); ok {
		t.Fatal("oversized value kept")
	}
	if _, ok := p.Get(AttrPriority); !ok {
		t.Fatal("small attribute dropped")
	}
}

func TestAddCopiesValue(t *testing.T) {
	v := []byte("user")
	m := &Message{}
	m.Add(AttrUsername, v)
	v[0] = 'X'
	got, _ := m.Get(AttrUsername)
	if string(got) != "user" {
		t.Fatalf("got %q", got)
	}
}

func TestParseCopiesInput(t *testing.T) {
	raw := decodeHex(t, sampleRequestHex)
	m, err := Parse(raw)
	if err != nil {
		t.Fatal(err)
	}
	for i := range raw {
		raw[i] = 0
	}
	v, _ := m.Get(AttrUsername)
	if string(v) != "evtj:h6vY" {
		t.Fatalf("aliased input: %q", v)
	}
	_ = append(v, 'Z')
	sw, _ := m.Get(attrSoftware)
	if string(sw) != "STUN test client" {
		t.Fatalf("append clobbered neighbour: %q", sw)
	}
}

func TestAddErrorCode(t *testing.T) {
	m := &Message{Type: BindingError}
	m.AddErrorCode(487, "Role Conflict")
	v, _ := m.Get(AttrErrorCode)
	want := append([]byte{0, 0, 4, 87}, "Role Conflict"...)
	if !bytes.Equal(v, want) {
		t.Fatalf("got %x", v)
	}
	m = &Message{}
	m.AddErrorCode(42, "")
	v, _ = m.Get(AttrErrorCode)
	if !bytes.Equal(v, []byte{0, 0, 5, 0}) {
		t.Fatalf("out of range code %x", v)
	}
	m = &Message{}
	m.AddErrorCode(400, strings.Repeat("é", 500))
	v, _ = m.Get(AttrErrorCode)
	if len(v)-4 > 763 || (len(v)-4)%2 != 0 {
		t.Fatalf("reason length %d", len(v)-4)
	}
	raw := m.Encode(nil, true)
	if _, err := Parse(raw); err != nil || !CheckFingerprint(raw) {
		t.Fatal(err)
	}
}

func TestAddXORMappedAddressInvalid(t *testing.T) {
	m := &Message{}
	m.AddXORMappedAddress(netip.AddrPort{})
	if len(m.Attributes) != 0 {
		t.Fatal("invalid address added")
	}
}

func TestDecodeXORAddressErrors(t *testing.T) {
	bad := [][]byte{
		nil,
		{0, 1, 0},
		{0, 1, 0, 0, 1, 2, 3},
		{0, 1, 0, 0, 1, 2, 3, 4, 5},
		make([]byte, 20),
		append([]byte{0, 2, 0, 0}, make([]byte, 15)...),
		append([]byte{0, 2, 0, 0}, make([]byte, 17)...),
		{0, 3, 0, 0, 1, 2, 3, 4},
	}
	for i, v := range bad {
		if _, err := DecodeXORAddress(v, vectorTID); err == nil {
			t.Fatalf("case %d accepted", i)
		}
	}
}

func TestParseRejects(t *testing.T) {
	good := decodeHex(t, sampleIPv4ResponseHex)
	mutate := func(f func(b []byte) []byte) []byte {
		return f(append([]byte(nil), good...))
	}
	cases := map[string][]byte{
		"empty":         nil,
		"short header":  good[:19],
		"header only":   good[:20],
		"bad cookie":    mutate(func(b []byte) []byte { b[4] ^= 1; return b }),
		"top bit":       mutate(func(b []byte) []byte { b[0] |= 0x80; return b }),
		"second bit":    mutate(func(b []byte) []byte { b[0] |= 0x40; return b }),
		"trailing data": append(append([]byte(nil), good...), 0, 0, 0, 0),
		"length short":  mutate(func(b []byte) []byte { b[3] -= 4; return b }),
		"length long":   mutate(func(b []byte) []byte { b[3] += 4; return b }),
		"length odd":    mutate(func(b []byte) []byte { b[3]++; return b }),
		"attr overrun":  mutate(func(b []byte) []byte { b[len(b)-5] = 0x05; return b }),
		"attr huge":     mutate(func(b []byte) []byte { b[22] = 0xff; b[23] = 0xff; return b }),
		"attr after fp": append(mutate(func(b []byte) []byte { b[3] += 4; return b }), 0x00, 0x25, 0x00, 0x00),
		"half attr":     append(mutate(func(b []byte) []byte { b[3] += 4; return b }), 0x00, 0x25, 0x00, 0x08),
	}
	for name, b := range cases {
		if m, err := Parse(b); err == nil {
			t.Fatalf("%s: accepted %+v", name, m)
		}
		if CheckIntegrity(b, []byte(vectorPassword)) {
			t.Fatalf("%s: integrity accepted", name)
		}
		if CheckFingerprint(b) {
			t.Fatalf("%s: fingerprint accepted", name)
		}
	}
	for _, name := range []string{"empty", "short header", "bad cookie", "top bit", "second bit", "trailing data", "length short", "length long", "length odd"} {
		if IsMessage(cases[name]) {
			t.Fatalf("%s: IsMessage true", name)
		}
	}
}

func TestParseHeaderOnly(t *testing.T) {
	m := &Message{Type: BindingRequest, TransactionID: vectorTID}
	raw := m.Encode(nil, false)
	if len(raw) != 20 {
		t.Fatalf("len %d", len(raw))
	}
	p, err := Parse(raw)
	if err != nil || len(p.Attributes) != 0 {
		t.Fatal(err)
	}
	if CheckIntegrity(raw, []byte("k")) || CheckFingerprint(raw) {
		t.Fatal("empty message passed checks")
	}
}

func TestParseTruncatedPrefixes(t *testing.T) {
	for name, raw := range vectors(t) {
		for n := 0; n < len(raw); n++ {
			if _, err := Parse(raw[:n]); err == nil {
				t.Fatalf("%s: prefix %d accepted", name, n)
			}
			if CheckIntegrity(raw[:n], []byte(vectorPassword)) || CheckFingerprint(raw[:n]) {
				t.Fatalf("%s: prefix %d passed checks", name, n)
			}
		}
	}
}

func TestParseIgnoresAttributesAfterIntegrity(t *testing.T) {
	m := &Message{Type: BindingRequest, TransactionID: vectorTID}
	m.Add(AttrUsername, []byte("a:b"))
	raw := m.Encode([]byte("pw"), false)
	extra := []byte{0x00, 0x25, 0x00, 0x00}
	tampered := append(append([]byte(nil), raw...), extra...)
	binary.BigEndian.PutUint16(tampered[2:4], uint16(len(tampered)-20))
	p, err := Parse(tampered)
	if err != nil {
		t.Fatal(err)
	}
	if _, ok := p.Get(AttrUseCandidate); ok {
		t.Fatal("attribute after MESSAGE-INTEGRITY exposed")
	}
	if CheckIntegrity(tampered, []byte("pw")) {
		t.Fatal("integrity accepted attribute after MESSAGE-INTEGRITY")
	}
}

func TestIntegrityWrongSize(t *testing.T) {
	m := &Message{Type: BindingRequest}
	m.Add(AttrUsername, []byte("a:b"))
	m.Attributes = append(m.Attributes, Attribute{Type: 0x9999, Value: nil})
	raw := m.Encode(nil, false)
	binary.BigEndian.PutUint16(raw[len(raw)-4:], AttrMessageIntegrity)
	if CheckIntegrity(raw, []byte("pw")) {
		t.Fatal("zero length MESSAGE-INTEGRITY accepted")
	}
}

func TestFingerprintWrongSize(t *testing.T) {
	m := &Message{Type: BindingRequest}
	m.Attributes = append(m.Attributes, Attribute{Type: 0x9999, Value: []byte{1, 2}})
	raw := m.Encode(nil, false)
	binary.BigEndian.PutUint16(raw[20:22], AttrFingerprint)
	if CheckFingerprint(raw) {
		t.Fatal("short FINGERPRINT accepted")
	}
}

func TestIsMessage(t *testing.T) {
	for name, raw := range vectors(t) {
		if !IsMessage(raw) {
			t.Fatalf("%s: false", name)
		}
	}
	rtp := make([]byte, 40)
	rtp[0] = 0x80
	if IsMessage(rtp) {
		t.Fatal("RTP accepted")
	}
	dtls := make([]byte, 40)
	dtls[0] = 22
	if IsMessage(dtls) {
		t.Fatal("DTLS accepted")
	}
}

func TestGarbageNoPanic(t *testing.T) {
	rng := rand.New(rand.NewSource(5769))
	seeds := vectors(t)
	exercise := func(b []byte) {
		IsMessage(b)
		if m, err := Parse(b); err == nil {
			for _, a := range m.Attributes {
				if a.Type == AttrXORMappedAddress {
					DecodeXORAddress(a.Value, m.TransactionID)
				}
			}
			m.Encode([]byte("k"), true)
		}
		CheckIntegrity(b, []byte(vectorPassword))
		CheckFingerprint(b)
		DecodeXORAddress(b, vectorTID)
	}
	for i := 0; i < 20000; i++ {
		n := rng.Intn(120)
		b := make([]byte, n)
		rng.Read(b)
		if n >= 8 && rng.Intn(2) == 0 {
			b[0] &= 0x3f
			binary.BigEndian.PutUint32(b[4:8], 0x2112A442)
		}
		if n >= 20 && rng.Intn(2) == 0 {
			binary.BigEndian.PutUint16(b[2:4], uint16(n-20))
		}
		exercise(b)
	}
	for _, raw := range seeds {
		for i := 0; i < 5000; i++ {
			b := append([]byte(nil), raw...)
			for k := rng.Intn(4) + 1; k > 0; k-- {
				b[20+rng.Intn(len(b)-20)] = byte(rng.Intn(256))
			}
			exercise(b)
		}
	}
}

func FuzzParse(f *testing.F) {
	for _, raw := range vectors(f) {
		f.Add(raw)
	}
	f.Fuzz(func(t *testing.T, b []byte) {
		IsMessage(b)
		CheckIntegrity(b, []byte(vectorPassword))
		CheckFingerprint(b)
		DecodeXORAddress(b, vectorTID)
		m, err := Parse(b)
		if err != nil {
			return
		}
		for _, a := range m.Attributes {
			DecodeXORAddress(a.Value, m.TransactionID)
		}
		out := m.Encode([]byte("k"), true)
		if _, err := Parse(out); err != nil {
			t.Fatalf("re-encode unparseable: %v", err)
		}
		if !CheckIntegrity(out, []byte("k")) || !CheckFingerprint(out) {
			t.Fatal("re-encode checks failed")
		}
	})
}
