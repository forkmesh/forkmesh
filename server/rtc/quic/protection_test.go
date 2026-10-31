package quic

import (
	"bytes"
	"encoding/hex"
	"strings"
	"testing"
)

func unhex(s string) []byte {
	s = strings.Join(strings.Fields(s), "")
	b, err := hex.DecodeString(s)
	if err != nil {
		panic(err)
	}
	return b
}

func pad(n int, b []byte) []byte {
	for len(b) < n {
		b = append(b, 0)
	}
	return b
}

func TestRFC9001InitialSecrets(t *testing.T) {
	dcid := unhex("8394c8f03e515708")
	client, server := initialSecrets(dcid)
	if got, want := hex.EncodeToString(client), "c00cf151ca5be075ed0ebfb5c80323c42d6b7db67881289af4008f1f6c357aea"; got != want {
		t.Errorf("client initial secret %s", got)
	}
	if got, want := hex.EncodeToString(server), "3c199828fd139efd216c155ad844cc81fb82fa8d7446fa7d78be803acdda951b"; got != want {
		t.Errorf("server initial secret %s", got)
	}
	check := func(name string, secret []byte, key, iv, hp string) {
		gotKey := hkdfExpandLabel(suiteAES128.hash, secret, "quic key", nil, 16)
		gotIV := hkdfExpandLabel(suiteAES128.hash, secret, "quic iv", nil, 12)
		gotHP := hkdfExpandLabel(suiteAES128.hash, secret, "quic hp", nil, 16)
		if hex.EncodeToString(gotKey) != key || hex.EncodeToString(gotIV) != iv || hex.EncodeToString(gotHP) != hp {
			t.Errorf("%s keys: key %x iv %x hp %x", name, gotKey, gotIV, gotHP)
		}
	}
	check("client", client, "1f369613dd76d5467730efcbe3b1a22d", "fa044b2f42a3fd3b46fb255c", "9f50449e04a0e810283a1e9933adedd2")
	check("server", server, "cf3a5331653c364c88f0f379b6067e37", "0ac1493ca1905853b0bba03e", "c206b8d9b9f0f37644430b490eeaa314")
}

var rfcClientInitialPayload = unhex(`
	060040f1010000ed0303ebf8fa56f129 39b9584a3896472ec40bb863cfd3e868
	04fe3a47f06a2b69484c000004130113 02010000c000000010000e00000b6578
	616d706c652e636f6dff01000100000a 00080006001d00170018001000070005
	04616c706e0005000501000000000033 00260024001d00209370b2c9caa47fba
	baf4559fedba753de171fa71f50f1ce1 5d43e994ec74d748002b000302030400
	0d0010000e0403050306030203080408 050806002d00020101001c0002400100
	3900320408ffffffffffffffff050480 00ffff07048000ffff08011001048000
	75300901100f088394c8f03e51570806 048000ffff`)

var rfcClientInitialProtected = unhex(`
	c000000001088394c8f03e5157080000 449e7b9aec34d1b1c98dd7689fb8ec11
	d242b123dc9bd8bab936b47d92ec356c 0bab7df5976d27cd449f63300099f399
	1c260ec4c60d17b31f8429157bb35a12 82a643a8d2262cad67500cadb8e7378c
	8eb7539ec4d4905fed1bee1fc8aafba1 7c750e2c7ace01e6005f80fcb7df6212
	30c83711b39343fa028cea7f7fb5ff89 eac2308249a02252155e2347b63d58c5
	457afd84d05dfffdb20392844ae81215 4682e9cf012f9021a6f0be17ddd0c208
	4dce25ff9b06cde535d0f920a2db1bf3 62c23e596d11a4f5a6cf3948838a3aec
	4e15daf8500a6ef69ec4e3feb6b1d98e 610ac8b7ec3faf6ad760b7bad1db4ba3
	485e8a94dc250ae3fdb41ed15fb6a8e5 eba0fc3dd60bc8e30c5c4287e53805db
	059ae0648db2f64264ed5e39be2e20d8 2df566da8dd5998ccabdae053060ae6c
	7b4378e846d29f37ed7b4ea9ec5d82e7 961b7f25a9323851f681d582363aa5f8
	9937f5a67258bf63ad6f1a0b1d96dbd4 faddfcefc5266ba6611722395c906556
	be52afe3f565636ad1b17d508b73d874 3eeb524be22b3dcbc2c7468d54119c74
	68449a13d8e3b95811a198f3491de3e7 fe942b330407abf82a4ed7c1b311663a
	c69890f4157015853d91e923037c227a 33cdd5ec281ca3f79c44546b9d90ca00
	f064c99e3dd97911d39fe9c5d0b23a22 9a234cb36186c4819e8b9c5927726632
	291d6a418211cc2962e20fe47feb3edf 330f2c603a9d48c0fcb5699dbfe58964
	25c5bac4aee82e57a85aaf4e2513e4f0 5796b07ba2ee47d80506f8d2c25e50fd
	14de71e6c418559302f939b0e1abd576 f279c4b2e0feb85c1f28ff18f58891ff
	ef132eef2fa09346aee33c28eb130ff2 8f5b766953334113211996d20011a198
	e3fc433f9f2541010ae17c1bf202580f 6047472fb36857fe843b19f5984009dd
	c324044e847a4f4a0ab34f719595de37 252d6235365e9b84392b061085349d73
	203a4a13e96f5432ec0fd4a1ee65accd d5e3904df54c1da510b0ff20dcc0c77f
	cb2c0e0eb605cb0504db87632cf3d8b4 dae6e705769d1de354270123cb11450e
	fc60ac47683d7b8d0f811365565fd98c 4c8eb936bcab8d069fc33bd801b03ade
	a2e1fbc5aa463d08ca19896d2bf59a07 1b851e6c239052172f296bfb5e724047
	90a2181014f3b94a4e97d117b4381303 68cc39dbb2d198065ae3986547926cd2
	162f40a29f0c3c8745c0f50fba3852e5 66d44575c29d39a03f0cda721984b6f4
	40591f355e12d439ff150aab7613499d bd49adabc8676eef023b15b65bfc5ca0
	6948109f23f350db82123535eb8a7433 bdabcb909271a6ecbcb58b936a88cd4e
	8f2e6ff5800175f113253d8fa9ca8885 c2f552e657dc603f252e1a8e308f76f0
	be79e2fb8f5d5fbbe2e30ecadd220723 c8c0aea8078cdfcb3868263ff8f09400
	54da48781893a7e49ad5aff4af300cd8 04a6b6279ab3ff3afb64491c85194aab
	760d58a606654f9f4400e8b38591356f bf6425aca26dc85244259ff2b19c41b9
	f96f3ca9ec1dde434da7d2d392b905dd f3d1f9af93d1af5950bd493f5aa731b4
	056df31bd267b6b90a079831aaf579be 0a39013137aac6d404f518cfd4684064
	7e78bfe706ca4cf5e9c5453e9f7cfd2b 8b4c8d169a44e55c88d4a9a7f9474241
	e221af44860018ab0856972e194cd934`)

func protectForTest(k *directionKeys, header, payload []byte, pn int64) []byte {
	pnLen := int(header[0]&0x03) + 1
	pnOffset := len(header) - pnLen
	pkt := make([]byte, 0, len(header)+len(payload)+aeadTagLen)
	pkt = append(pkt, header...)
	pkt = append(pkt, payload...)
	pkt = k.key.seal(pkt, len(header), pn)
	k.hp.protect(pkt, pnOffset, pnLen)
	return pkt
}

func TestRFC9001PacketProtection(t *testing.T) {
	dcid := unhex("8394c8f03e515708")
	clientSeal, serverSeal := initialKeys(dcid, true)
	cases := []struct {
		name    string
		keys    *directionKeys
		pn      int64
		header  []byte
		payload []byte
		want    []byte
	}{
		{"client initial", clientSeal, 2, unhex("c300000001088394c8f03e5157080000449e00000002"), pad(1162, rfcClientInitialPayload), rfcClientInitialProtected},
		{"server initial", serverSeal, 1, unhex("c1000000010008f067a5502a4262b50040750001"), unhex(`
			02000000000600405a020000560303ee fce7f7b37ba1d1632e96677825ddf739
			88cfc79825df566dc5430b9a045a1200 130100002e00330024001d00209d3c94
			0d89690b84d08a60993c144eca684d10 81287c834d5311bcf32bb9da1a002b00
			020304`), unhex(`
			cf000000010008f067a5502a4262b500 4075c0d95a482cd0991cd25b0aac406a
			5816b6394100f37a1c69797554780bb3 8cc5a99f5ede4cf73c3ec2493a1839b3
			dbcba3f6ea46c5b7684df3548e7ddeb9 c3bf9c73cc3f3bded74b562bfb19fb84
			022f8ef4cdd93795d77d06edbb7aaf2f 58891850abbdca3d20398c276456cbc4
			2158407dd074ee`)},
	}
	for _, tc := range cases {
		got := protectForTest(tc.keys, tc.header, tc.payload, tc.pn)
		if !bytes.Equal(got, tc.want) {
			t.Errorf("%s: protected packet differs\ngot  %x\nwant %x", tc.name, got, tc.want)
			continue
		}
		pkt := append([]byte(nil), tc.want...)
		h, err := parseLongHeader(pkt)
		if err != nil {
			t.Fatalf("%s: %v", tc.name, err)
		}
		pnLen, ok := tc.keys.hp.unprotect(pkt, h.pnOffset)
		if !ok {
			t.Fatalf("%s: header protection", tc.name)
		}
		pn := decodePacketNumber(tc.pn-1, readTruncatedPN(pkt[h.pnOffset:], pnLen), pnLen)
		if pn != tc.pn {
			t.Errorf("%s: packet number %d", tc.name, pn)
		}
		payload, err := tc.keys.key.open(pkt[:h.end], h.pnOffset+pnLen, pn)
		if err != nil {
			t.Fatalf("%s: open: %v", tc.name, err)
		}
		if !bytes.Equal(pkt[:h.pnOffset+pnLen], tc.header) || !bytes.Equal(payload, tc.payload) {
			t.Errorf("%s: unprotected packet differs", tc.name)
		}
	}
}

func TestRFC9001ServerInitialDecryptsWithClientView(t *testing.T) {
	dcid := unhex("8394c8f03e515708")
	_, clientOpen := initialKeys(dcid, true)
	pkt := unhex(`
		cf000000010008f067a5502a4262b500 4075c0d95a482cd0991cd25b0aac406a
		5816b6394100f37a1c69797554780bb3 8cc5a99f5ede4cf73c3ec2493a1839b3
		dbcba3f6ea46c5b7684df3548e7ddeb9 c3bf9c73cc3f3bded74b562bfb19fb84
		022f8ef4cdd93795d77d06edbb7aaf2f 58891850abbdca3d20398c276456cbc4
		2158407dd074ee`)
	c := &Conn{}
	pn, payload, ok := c.openLong(pkt, 18, &pnSpace{open: clientOpen, largestRecv: -1})
	if !ok || pn != 1 || payload[0] != frameAck {
		t.Fatalf("server initial: ok=%v pn=%d", ok, pn)
	}
}

func TestRFC9001Retry(t *testing.T) {
	odcid := unhex("8394c8f03e515708")
	retry := unhex("ff000000010008f067a5502a4262b5746f6b656e04a265ba2eff4d829058fb3f0f2496ba")
	if !validRetryTag(odcid, retry) {
		t.Fatal("RFC 9001 A.4 retry tag rejected")
	}
	tampered := append([]byte(nil), retry...)
	tampered[20] ^= 1
	if validRetryTag(odcid, tampered) {
		t.Fatal("tampered retry accepted")
	}
	h, err := parseLongHeader(retry)
	if err != nil || h.typ != packetRetry || !bytes.Equal(h.scid, unhex("f067a5502a4262b5")) {
		t.Fatalf("retry header %+v %v", h, err)
	}
	if got := retry[h.pnOffset : len(retry)-aeadTagLen]; string(got) != "token" {
		t.Fatalf("retry token %q", got)
	}
	built := appendRetry(nil, nil, unhex("f067a5502a4262b5"), odcid, []byte("token"))
	if !validRetryTag(odcid, built) || !bytes.Equal(built[1:len(built)-aeadTagLen], retry[1:len(retry)-aeadTagLen]) || built[0]&0xf0 != 0xf0 {
		t.Fatalf("built retry %x", built)
	}
}

func TestKeyUpdateDerivation(t *testing.T) {
	secret := unhex("9ac312a7f877468ebe69422748ad00a15443f18203a07d6060f688f30f21632b")
	k := newDirectionKeys(suiteAES128, secret)
	next := k.nextGeneration()
	want := hkdfExpandLabel(suiteAES128.hash, secret, "quic ku", nil, 32)
	if !bytes.Equal(next.secret, want) {
		t.Fatal("key update secret")
	}
	if next.hp.block != k.hp.block {
		t.Fatal("header protection key must not change on key update")
	}
	hdr := []byte{0x40, 0xaa, 0xbb, 0, 1}
	pkt := protectForTest(next, hdr, []byte{1, 2, 3, 4}, 1)
	if _, err := k.key.open(append([]byte(nil), pkt...), len(hdr), 1); err == nil {
		t.Fatal("old key opened a packet sealed with the updated key")
	}
}

func TestPacketNumberCoding(t *testing.T) {
	if got := decodePacketNumber(0xa82f30ea, 0x9b32, 2); got != 0xa82f9b32 {
		t.Fatalf("RFC 9000 A.3 example decoded to %x", got)
	}
	for _, tc := range []struct {
		pn, acked int64
		want      int
	}{{0, -1, 1}, {126, -1, 1}, {200, -1, 2}, {0xac5c02, 0xabe8b3, 2}, {0xace8fe, 0xabe8b3, 3}, {1 << 30, 0, 4}} {
		if got := packetNumberLen(tc.pn, tc.acked); got != tc.want {
			t.Errorf("packetNumberLen(%x, %x) = %d, want %d", tc.pn, tc.acked, got, tc.want)
		}
	}
	for largest := int64(-1); largest < 70000; largest += 997 {
		for delta := int64(1); delta < 100; delta += 13 {
			pn := largest + delta
			n := packetNumberLen(pn, largest)
			trunc := uint64(pn) & (1<<(8*uint(n)) - 1)
			if got := decodePacketNumber(largest, trunc, n); got != pn {
				t.Fatalf("round trip %d via %d bytes from %d: %d", pn, n, largest, got)
			}
		}
	}
}

func TestVarint(t *testing.T) {
	for _, tc := range []struct {
		v   uint64
		enc string
	}{{37, "25"}, {15293, "7bbd"}, {494878333, "9d7f3e7d"}, {151288809941952652, "c2197c5eff14e88c"}} {
		b := AppendVarint(nil, tc.v)
		if hex.EncodeToString(b) != tc.enc {
			t.Errorf("encode %d: %x", tc.v, b)
		}
		v, n := ReadVarint(b)
		if v != tc.v || n != len(b) {
			t.Errorf("decode %x: %d %d", b, v, n)
		}
	}
	if v, n := ReadVarint(unhex("4025")); v != 37 || n != 2 {
		t.Error("two-byte 37")
	}
	if _, n := readMinimalVarint(unhex("4025")); n != -1 {
		t.Error("non-minimal varint accepted")
	}
	if _, n := ReadVarint(unhex("c0000000")); n != 0 {
		t.Error("truncated varint accepted")
	}
}
