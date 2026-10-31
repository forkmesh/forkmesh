package srtp

import (
	"bytes"
	"encoding/hex"
	"strings"
	"testing"
)

func hx(t *testing.T, s string) []byte {
	t.Helper()
	b, err := hex.DecodeString(strings.Join(strings.Fields(s), ""))
	if err != nil {
		t.Fatalf("bad hex %q: %v", s, err)
	}
	return b
}

func TestRFC3711AESCMKeystream(t *testing.T) {
	key := hx(t, "2B7E151628AED2A6ABF7158809CF4F3C")
	salt := hx(t, "F0F1F2F3F4F5F6F7F8F9FAFBFCFD")
	c, err := newContextFromSessionKeys(ProfileAES128CMHMACSHA1_80, sessionKeys{
		rtpKey: key, rtpSalt: salt, rtpAuth: make([]byte, 20),
		rtcpKey: key, rtcpSalt: salt, rtcpAuth: make([]byte, 20),
	})
	if err != nil {
		t.Fatal(err)
	}
	const blocks = 65282
	ks := make([]byte, blocks*16)
	cmXOR(c.rtpBlock, c.rtpSalt, 0, 0, ks, ks)
	want := map[int]string{
		0x0000: "E03EAD0935C95E80E166B16DD92B4EB4",
		0x0001: "D23513162B02D0F72A43A2FE4A5F97AB",
		0x0002: "41E95B3BB0A2E8DD477901E4FCA894C0",
		0xFEFF: "EC8CDF7398607CB0F2D21675EA9EA1E4",
		0xFF00: "362B7C3C6773516318A077D7FC5073AE",
		0xFF01: "6A2CC3787889374FBEB4C81B17BA6C44",
	}
	for blk, w := range want {
		got := ks[blk*16 : blk*16+16]
		if !bytes.Equal(got, hx(t, w)) {
			t.Errorf("block %04X: got %X want %s", blk, got, w)
		}
	}
}

func TestRFC3711KeyDerivation(t *testing.T) {
	mk := hx(t, "E1F97A0D3E018BE0D64FA32C06DE4139")
	ms := hx(t, "0EC675AD498AFEEBB6960B3AABE6")
	enc, err := prf(mk, ms, labelRTPEncryption, 16)
	if err != nil {
		t.Fatal(err)
	}
	if want := hx(t, "C61E7A93744F39EE10734AFE3FF7A087"); !bytes.Equal(enc, want) {
		t.Errorf("cipher key %X want %X", enc, want)
	}
	salt, _ := prf(mk, ms, labelRTPSalt, 14)
	if want := hx(t, "30CBBC08863D8C85D49DB34A9AE1"); !bytes.Equal(salt, want) {
		t.Errorf("cipher salt %X want %X", salt, want)
	}
	auth, _ := prf(mk, ms, labelRTPAuth, 94)
	wantAuth := hx(t, `CEBE321F6FF7716B6FD4AB49AF256A15
		6D38BAA48F0A0ACF3C34E2359E6CDBCE
		E049646C43D9327AD175578EF7227098
		6371C10C9A369AC2F94A8C5FBCDDDC25
		6D6E919A48B610EF17C2041E47403576
		6B68642C59BBFC2F34DB60DBDFB2`)
	if !bytes.Equal(auth, wantAuth) {
		t.Errorf("auth key %X want %X", auth, wantAuth)
	}
	k, err := deriveSessionKeys(ProfileAES128CMHMACSHA1_80, mk, ms)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(k.rtpKey, enc) || !bytes.Equal(k.rtpSalt, salt) || !bytes.Equal(k.rtpAuth, wantAuth[:20]) {
		t.Errorf("session keys do not match the RFC 3711 B.3 derivation")
	}
}

func rfc7714Context(t *testing.T) *Context {
	t.Helper()
	key := hx(t, "000102030405060708090a0b0c0d0e0f")
	salt := hx(t, "517569642070726f2071756f")
	c, err := newContextFromSessionKeys(ProfileAEADAES128GCM, sessionKeys{
		rtpKey: key, rtpSalt: salt, rtcpKey: key, rtcpSalt: salt,
	})
	if err != nil {
		t.Fatal(err)
	}
	return c
}

func TestRFC7714RTPEncryptDecrypt(t *testing.T) {
	plain := hx(t, `8040f17b 8041f8d3 5501a0b2 47616c6c
		69612065 7374206f 6d6e6973 20646976
		69736120 696e2070 61727465 73207472
		6573`)
	sealed := hx(t, `8040f17b 8041f8d3 5501a0b2 f24de3a3
		fb34de6c acba861c 9d7e4bca be633bd5
		0d294e6f 42a5f47a 51c7d19b 36de3adf
		8833899d 7f27beb1 6a9152cf 765ee439
		0cce`)
	got, err := rfc7714Context(t).EncryptRTP(nil, plain)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(got, sealed) {
		t.Fatalf("16.1.1 encrypt\n got %x\nwant %x", got, sealed)
	}
	opened, err := rfc7714Context(t).DecryptRTP(nil, sealed)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(opened, plain) {
		t.Fatalf("16.1.2 decrypt\n got %x\nwant %x", opened, plain)
	}
}

func TestRFC7714RTCPEncryptDecrypt(t *testing.T) {
	plain := hx(t, `81c8000d 4d617273 4e545031 4e545032
		52545020 0000042a 0000e930 4c756e61
		deadbeef deadbeef deadbeef deadbeef
		deadbeef`)
	sealed := hx(t, `81c8000d 4d617273 63e94885 dcdab67c
		a727d766 2f6b7e99 7ff5c0f7 6c06f32d
		c676a5f1 730d6fda 4ce09b46 86303ded
		0bb9275b c84aa458 96cf4d2f c5abf872
		45d9eade 800005d4`)
	c := rfc7714Context(t)
	c.txRTCP[0x4d617273] = 0x5d4
	got, err := c.EncryptRTCP(nil, plain)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(got, sealed) {
		t.Fatalf("17.1 encrypt\n got %x\nwant %x", got, sealed)
	}
	if c.txRTCP[0x4d617273] != 0x5d5 {
		t.Fatalf("SRTCP index not advanced: %x", c.txRTCP[0x4d617273])
	}
	opened, err := rfc7714Context(t).DecryptRTCP(nil, sealed)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(opened, plain) {
		t.Fatalf("17.1 decrypt\n got %x\nwant %x", opened, plain)
	}
}

func TestRFC7714RTCPTaggingOnly(t *testing.T) {
	plain := hx(t, `81c8000d 4d617273 4e545031 4e545032
		52545020 0000042a 0000e930 4c756e61
		deadbeef deadbeef deadbeef deadbeef
		deadbeef`)
	tagged := hx(t, `81c8000d 4d617273 4e545031 4e545032
		52545020 0000042a 0000e930 4c756e61
		deadbeef deadbeef deadbeef deadbeef
		deadbeef 841dd968 3dd78ec9 2ae58790
		125f62b3 000005d4`)
	opened, err := rfc7714Context(t).DecryptRTCP(nil, tagged)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(opened, plain) {
		t.Fatalf("17.3 verify\n got %x\nwant %x", opened, plain)
	}
	bad := append([]byte(nil), tagged...)
	bad[20] ^= 1
	if _, err := rfc7714Context(t).DecryptRTCP(nil, bad); err != errAuth {
		t.Fatalf("tampered tag-only packet: %v", err)
	}
}

func TestLibsrtpAESCMReference(t *testing.T) {
	key := hx(t, "e1f97a0d3e018be0d64fa32c06de4139")
	salt := hx(t, "0ec675ad498afeebb6960b3aabe6")
	rtpPlain := hx(t, "800f1234decafbadcafebabeabababababababababababababababab")
	rtpSealed := hx(t, `800f1234decafbadcafebabe4e55dc4ce79978d88ca4d215949d2402b78d6acc99ea179b8dbb`)
	rtcpPlain := hx(t, "81c8000bcafebabeabababababababababababababababab")
	rtcpSealed := hx(t, `81c8000bcafebabe7128035be487b9bdbef89041f977a5a880000001993e08cd54d6c1230798`)
	checkReference(t, ProfileAES128CMHMACSHA1_80, key, salt, rtpPlain, rtpSealed, rtcpPlain, rtcpSealed)
}

func TestLibsrtpGCMReference(t *testing.T) {
	key := hx(t, "000102030405060708090a0b0c0d0e0f")
	salt := hx(t, "a0a1a2a3a4a5a6a7a8a9aaab")
	rtpPlain := hx(t, "800f1234decafbadcafebabeabababababababababababababababab")
	rtpSealed := hx(t, `800f1234decafbadcafebabec5002ede04cfdd2eb91159e0880aa06ed2976826f796b201df3131a127e8a392`)
	rtcpPlain := hx(t, "81c8000bcafebabeabababababababababababababababab")
	rtcpSealed := hx(t, `81c8000bcafebabec98b8b5df0392a55852b6c21ac8e7025c52c6fbea2b3b446ea31123ba88ce61e80000001`)
	checkReference(t, ProfileAEADAES128GCM, key, salt, rtpPlain, rtpSealed, rtcpPlain, rtcpSealed)
}

func checkReference(t *testing.T, p Profile, key, salt, rtpPlain, rtpSealed, rtcpPlain, rtcpSealed []byte) {
	t.Helper()
	tx, err := NewContext(p, key, salt)
	if err != nil {
		t.Fatal(err)
	}
	got, err := tx.EncryptRTP(nil, rtpPlain)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(got, rtpSealed) {
		t.Errorf("rtp\n got %x\nwant %x", got, rtpSealed)
	}
	tx.txRTCP[0xcafebabe] = 1
	got, err = tx.EncryptRTCP(nil, rtcpPlain)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(got, rtcpSealed) {
		t.Errorf("rtcp\n got %x\nwant %x", got, rtcpSealed)
	}
	rx, err := NewContext(p, key, salt)
	if err != nil {
		t.Fatal(err)
	}
	if got, err = rx.DecryptRTP(nil, rtpSealed); err != nil || !bytes.Equal(got, rtpPlain) {
		t.Errorf("rtp decrypt %x %v", got, err)
	}
	if got, err = rx.DecryptRTCP(nil, rtcpSealed); err != nil || !bytes.Equal(got, rtcpPlain) {
		t.Errorf("rtcp decrypt %x %v", got, err)
	}
}
