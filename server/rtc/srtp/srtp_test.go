package srtp

import (
	"bytes"
	"math/rand"
	"testing"
)

var profiles = []Profile{ProfileAES128CMHMACSHA1_80, ProfileAEADAES128GCM}

func pair(t *testing.T, p Profile) (*Context, *Context) {
	t.Helper()
	material := make([]byte, KeyingMaterialLength(p))
	for i := range material {
		material[i] = byte(i*7 + 3)
	}
	ck, cs, _, _, err := SplitKeyingMaterial(p, material)
	if err != nil {
		t.Fatal(err)
	}
	tx, err := NewContext(p, ck, cs)
	if err != nil {
		t.Fatal(err)
	}
	rx, err := NewContext(p, ck, cs)
	if err != nil {
		t.Fatal(err)
	}
	return tx, rx
}

func rtpPacket(seq uint16, ssrc uint32, payload []byte) []byte {
	p := []byte{0x80, 0x60, byte(seq >> 8), byte(seq), 0, 0, 0x10, 0x00, 0, 0, 0, 0}
	put32(p[8:], ssrc)
	return append(p, payload...)
}

func rtcpPacket(ssrc uint32) []byte {
	p := []byte{0x80, 0xc8, 0x00, 0x06, 0, 0, 0, 0}
	put32(p[4:], ssrc)
	body := make([]byte, 20)
	for i := range body {
		body[i] = byte(i + 1)
	}
	rr := []byte{0x81, 0xc9, 0x00, 0x01, 0, 0, 0, 0}
	put32(rr[4:], ssrc)
	return append(append(p, body...), rr...)
}

func TestKeyingMaterial(t *testing.T) {
	if n := KeyingMaterialLength(ProfileAES128CMHMACSHA1_80); n != 60 {
		t.Fatalf("cm length %d", n)
	}
	if n := KeyingMaterialLength(ProfileAEADAES128GCM); n != 56 {
		t.Fatalf("gcm length %d", n)
	}
	if n := KeyingMaterialLength(Profile(0x0002)); n != 0 {
		t.Fatalf("unknown length %d", n)
	}
	for _, p := range profiles {
		n := KeyingMaterialLength(p)
		m := make([]byte, n)
		for i := range m {
			m[i] = byte(i)
		}
		s := (n - 32) / 2
		ck, cs, sk, ss, err := SplitKeyingMaterial(p, m)
		if err != nil {
			t.Fatal(err)
		}
		if !bytes.Equal(ck, m[0:16]) || !bytes.Equal(sk, m[16:32]) || !bytes.Equal(cs, m[32:32+s]) || !bytes.Equal(ss, m[32+s:]) {
			t.Fatalf("profile %d: wrong split", p)
		}
		m[0] = 0xff
		if ck[0] == 0xff {
			t.Fatal("split aliases the input")
		}
		ck = append(ck, 1)
		if cs[0] != byte(32) {
			t.Fatal("append to one key overwrote another")
		}
		if _, _, _, _, err := SplitKeyingMaterial(p, m[:n-1]); err == nil {
			t.Fatal("short material accepted")
		}
		if _, _, _, _, err := SplitKeyingMaterial(p, append(m, 0)); err == nil {
			t.Fatal("long material accepted")
		}
	}
	if _, _, _, _, err := SplitKeyingMaterial(Profile(9), make([]byte, 60)); err == nil {
		t.Fatal("unknown profile accepted")
	}
}

func TestNewContextValidation(t *testing.T) {
	if _, err := NewContext(ProfileAES128CMHMACSHA1_80, make([]byte, 16), make([]byte, 12)); err == nil {
		t.Fatal("cm with 12-byte salt accepted")
	}
	if _, err := NewContext(ProfileAEADAES128GCM, make([]byte, 16), make([]byte, 14)); err == nil {
		t.Fatal("gcm with 14-byte salt accepted")
	}
	if _, err := NewContext(ProfileAEADAES128GCM, make([]byte, 32), make([]byte, 12)); err == nil {
		t.Fatal("32-byte key accepted")
	}
	if _, err := NewContext(Profile(3), make([]byte, 16), make([]byte, 14)); err == nil {
		t.Fatal("unknown profile accepted")
	}
}

func TestRoundTripAcrossWrap(t *testing.T) {
	for _, p := range profiles {
		tx, rx := pair(t, p)
		var sealed [][]byte
		var seqs []uint16
		for s := 65530; s < 65536+6; s++ {
			seq := uint16(s)
			payload := bytes.Repeat([]byte{byte(s)}, 50)
			plain := rtpPacket(seq, 0x11223344, payload)
			enc, err := tx.EncryptRTP(nil, plain)
			if err != nil {
				t.Fatal(err)
			}
			if bytes.Equal(enc[12:62], payload) {
				t.Fatalf("profile %d: payload not encrypted", p)
			}
			dec, err := rx.DecryptRTP(nil, enc)
			if err != nil {
				t.Fatalf("profile %d seq %d: %v", p, seq, err)
			}
			if !bytes.Equal(dec, plain) {
				t.Fatalf("profile %d seq %d: mismatch", p, seq)
			}
			sealed = append(sealed, enc)
			seqs = append(seqs, seq)
		}
		if got := tx.txRTP[0x11223344].index; got != 1<<16|5 {
			t.Fatalf("profile %d: sender index %x", p, got)
		}
		if got := rx.rxRTP[0x11223344].window.top; got != 1<<16|5 {
			t.Fatalf("profile %d: receiver index %x", p, got)
		}
		_, fresh := pair(t, p)
		if _, err := fresh.DecryptRTP(nil, sealed[6]); err != errAuth {
			t.Fatalf("profile %d: post-wrap packet opened with ROC 0: %v", p, err)
		}
		if seqs[6] != 0 {
			t.Fatal("test setup")
		}
	}
}

func TestReorderAcrossWrap(t *testing.T) {
	for _, p := range profiles {
		tx, rx := pair(t, p)
		var sealed [][]byte
		for s := 65530; s < 65536+6; s++ {
			enc, err := tx.EncryptRTP(nil, rtpPacket(uint16(s), 7, []byte("payload")))
			if err != nil {
				t.Fatal(err)
			}
			sealed = append(sealed, enc)
		}
		order := []int{0, 1, 7, 6, 2, 8, 3, 4, 5, 9, 11, 10}
		for _, i := range order {
			if _, err := rx.DecryptRTP(nil, sealed[i]); err != nil {
				t.Fatalf("profile %d packet %d: %v", p, i, err)
			}
		}
		for _, i := range order {
			if _, err := rx.DecryptRTP(nil, sealed[i]); err != errReplay {
				t.Fatalf("profile %d packet %d replay: %v", p, i, err)
			}
		}
	}
}

func TestSenderReorderAcrossWrap(t *testing.T) {
	for _, p := range profiles {
		tx, rx := pair(t, p)
		a, _ := tx.EncryptRTP(nil, rtpPacket(65535, 9, []byte("a")))
		b, _ := tx.EncryptRTP(nil, rtpPacket(1, 9, []byte("b")))
		c, err := tx.EncryptRTP(nil, rtpPacket(0, 9, []byte("c")))
		if err != nil {
			t.Fatal(err)
		}
		for _, pk := range [][]byte{a, b, c} {
			if _, err := rx.DecryptRTP(nil, pk); err != nil {
				t.Fatalf("profile %d: %v", p, err)
			}
		}
		if tx.txRTP[9].index != 1<<16|1 {
			t.Fatalf("profile %d: sender index %x", p, tx.txRTP[9].index)
		}
	}
}

func TestReplayWindow(t *testing.T) {
	for _, p := range profiles {
		tx, rx := pair(t, p)
		var sealed [][]byte
		for i := 0; i < 300; i++ {
			enc, err := tx.EncryptRTP(nil, rtpPacket(uint16(1000+i), 5, []byte{byte(i)}))
			if err != nil {
				t.Fatal(err)
			}
			sealed = append(sealed, enc)
		}
		for i := 0; i < 300; i++ {
			if i == 150 || i == 250 {
				continue
			}
			if _, err := rx.DecryptRTP(nil, sealed[i]); err != nil {
				t.Fatalf("profile %d packet %d: %v", p, i, err)
			}
		}
		if _, err := rx.DecryptRTP(nil, sealed[299]); err != errReplay {
			t.Fatalf("profile %d: duplicate accepted: %v", p, err)
		}
		if _, err := rx.DecryptRTP(nil, sealed[250]); err != nil {
			t.Fatalf("profile %d: late packet in window rejected: %v", p, err)
		}
		if _, err := rx.DecryptRTP(nil, sealed[250]); err != errReplay {
			t.Fatalf("profile %d: late duplicate accepted: %v", p, err)
		}
		if _, err := rx.DecryptRTP(nil, sealed[150]); err != errReplay {
			t.Fatalf("profile %d: packet older than the window accepted: %v", p, err)
		}
		if _, err := rx.DecryptRTP(nil, sealed[172]); err != errReplay {
			t.Fatalf("profile %d: packet at window edge accepted: %v", p, err)
		}
		if _, err := rx.DecryptRTP(nil, sealed[171]); err != errReplay {
			t.Fatalf("profile %d: packet just past window accepted: %v", p, err)
		}
	}
}

func TestReplayWindowEdges(t *testing.T) {
	var w replayWindow
	w.mark(1000)
	for d := uint64(1); d < 128; d++ {
		if !w.fresh(1000 - d) {
			t.Fatalf("unseen %d reported seen", d)
		}
	}
	if w.fresh(1000 - 128) {
		t.Fatal("index 128 behind accepted")
	}
	w.mark(1000 - 127)
	if w.fresh(1000 - 127) {
		t.Fatal("marked edge reported fresh")
	}
	w.mark(1001)
	if w.fresh(1000-127) || !w.fresh(1000-126) {
		t.Fatal("edge state wrong after shift by one")
	}
	w.mark(1000 + 64)
	if w.fresh(1000) || w.fresh(1001) || !w.fresh(1002) {
		t.Fatal("shift by 63 lost bits")
	}
	w.mark(1000 + 64 + 64)
	if w.fresh(1000+64) || w.fresh(1001) || !w.fresh(1002) {
		t.Fatal("shift by 64 lost bits")
	}
	w.mark(5000)
	if w.fresh(5000) || !w.fresh(4999) || w.fresh(5000-128) {
		t.Fatal("big shift wrong")
	}
}

func TestForgedPacketsDoNotMoveState(t *testing.T) {
	for _, p := range profiles {
		tx, rx := pair(t, p)
		first, _ := tx.EncryptRTP(nil, rtpPacket(100, 42, []byte("first")))
		if _, err := rx.DecryptRTP(nil, first); err != nil {
			t.Fatal(err)
		}
		forged, _ := tx.EncryptRTP(nil, rtpPacket(30000, 42, []byte("forged")))
		forged[len(forged)-1] ^= 1
		if _, err := rx.DecryptRTP(nil, forged); err != errAuth {
			t.Fatalf("profile %d: forged accepted: %v", p, err)
		}
		if rx.rxRTP[42].window.top != 100 {
			t.Fatalf("profile %d: forged packet moved state to %d", p, rx.rxRTP[42].window.top)
		}
		forged[len(forged)-1] ^= 1
		if _, err := rx.DecryptRTP(nil, forged); err != nil {
			t.Fatalf("profile %d: genuine packet rejected after forged copy: %v", p, err)
		}
		stranger, _ := tx.EncryptRTP(nil, rtpPacket(5, 77, []byte("x")))
		stranger[13] ^= 0x40
		if _, err := rx.DecryptRTP(nil, stranger); err != errAuth {
			t.Fatalf("profile %d: forged new ssrc: %v", p, err)
		}
		if _, ok := rx.rxRTP[77]; ok {
			t.Fatalf("profile %d: forged packet created ssrc state", p)
		}
		rtcp, _ := tx.EncryptRTCP(nil, rtcpPacket(88))
		rtcp[10] ^= 1
		if _, err := rx.DecryptRTCP(nil, rtcp); err != errAuth {
			t.Fatalf("profile %d: forged rtcp: %v", p, err)
		}
		if _, ok := rx.rxRTCP[88]; ok {
			t.Fatalf("profile %d: forged rtcp created state", p)
		}
	}
}

func TestTamperingRejected(t *testing.T) {
	for _, p := range profiles {
		tx, _ := pair(t, p)
		plain := rtpPacket(4242, 0xabcdef01, bytes.Repeat([]byte{0x5a}, 40))
		sealed, err := tx.EncryptRTP(nil, plain)
		if err != nil {
			t.Fatal(err)
		}
		for i := range sealed {
			for _, bit := range []byte{0x01, 0x80} {
				_, rx := pair(t, p)
				bad := append([]byte(nil), sealed...)
				bad[i] ^= bit
				if out, err := rx.DecryptRTP(nil, bad); err == nil {
					t.Fatalf("profile %d: rtp byte %d bit %x tampered but accepted: %x", p, i, bit, out)
				}
			}
		}
		rtcpPlain := rtcpPacket(0x01020304)
		rtcpSealed, err := tx.EncryptRTCP(nil, rtcpPlain)
		if err != nil {
			t.Fatal(err)
		}
		for i := range rtcpSealed {
			for _, bit := range []byte{0x01, 0x80} {
				_, rx := pair(t, p)
				bad := append([]byte(nil), rtcpSealed...)
				bad[i] ^= bit
				if out, err := rx.DecryptRTCP(nil, bad); err == nil {
					t.Fatalf("profile %d: rtcp byte %d bit %x tampered but accepted: %x", p, i, bit, out)
				}
			}
		}
		_, rx := pair(t, p)
		if out, err := rx.DecryptRTCP(nil, rtcpSealed); err != nil || !bytes.Equal(out, rtcpPlain) {
			t.Fatalf("profile %d: rtcp round trip: %v", p, err)
		}
		if _, err := rx.DecryptRTCP(nil, rtcpSealed); err != errReplay {
			t.Fatalf("profile %d: rtcp replay: %v", p, err)
		}
	}
}

func TestWrongKeyRejected(t *testing.T) {
	for _, p := range profiles {
		tx, _ := pair(t, p)
		key := make([]byte, 16)
		salt := make([]byte, (KeyingMaterialLength(p)-32)/2)
		other, err := NewContext(p, key, salt)
		if err != nil {
			t.Fatal(err)
		}
		pk, _ := tx.EncryptRTP(nil, rtpPacket(1, 1, []byte("hello")))
		if _, err := other.DecryptRTP(nil, pk); err != errAuth {
			t.Fatalf("profile %d: wrong key: %v", p, err)
		}
	}
}

func TestRTCPRoundTripAndIndex(t *testing.T) {
	for _, p := range profiles {
		tx, rx := pair(t, p)
		for i := 0; i < 5; i++ {
			for _, ssrc := range []uint32{1, 2} {
				plain := rtcpPacket(ssrc)
				enc, err := tx.EncryptRTCP(nil, plain)
				if err != nil {
					t.Fatal(err)
				}
				if bytes.Equal(enc[8:28], plain[8:28]) {
					t.Fatal("rtcp body not encrypted")
				}
				tl := cmTagLength
				if p == ProfileAEADAES128GCM {
					tl = gcmTagLength
				}
				if len(enc) != len(plain)+tl+4 {
					t.Fatalf("profile %d: length %d", p, len(enc))
				}
				var word uint32
				if p == ProfileAEADAES128GCM {
					word = be32(enc[len(enc)-4:])
				} else {
					word = be32(enc[len(enc)-14:])
				}
				if word != eFlag|uint32(i) {
					t.Fatalf("profile %d: index word %08x at packet %d", p, word, i)
				}
				dec, err := rx.DecryptRTCP(nil, enc)
				if err != nil || !bytes.Equal(dec, plain) {
					t.Fatalf("profile %d: rtcp decrypt %v", p, err)
				}
			}
		}
	}
}

func TestRTCPIndexExhausted(t *testing.T) {
	for _, p := range profiles {
		tx, _ := pair(t, p)
		tx.txRTCP[3] = maxSRTCPIndex
		if _, err := tx.EncryptRTCP(nil, rtcpPacket(3)); err != nil {
			t.Fatal(err)
		}
		if _, err := tx.EncryptRTCP(nil, rtcpPacket(3)); err != errExhausted {
			t.Fatalf("profile %d: index wrap allowed: %v", p, err)
		}
	}
}

func TestRTPIndexExhausted(t *testing.T) {
	for _, p := range profiles {
		tx, _ := pair(t, p)
		tx.txRTP[3] = &txRTPState{index: uint64(maxROC)<<16 | 65535}
		if _, err := tx.EncryptRTP(nil, rtpPacket(0, 3, nil)); err != errExhausted {
			t.Fatalf("profile %d: roc wrap allowed: %v", p, err)
		}
	}
}

func TestCMRTCPUnencryptedFlag(t *testing.T) {
	tx, rx := pair(t, ProfileAES128CMHMACSHA1_80)
	plain := rtcpPacket(55)
	pk := append([]byte(nil), plain...)
	pk = append(pk, 0, 0, 0, 9)
	pk = append(pk, tx.rtcpAuthTag(make([]byte, 20), pk)...)
	out, err := rx.DecryptRTCP(nil, pk)
	if err != nil || !bytes.Equal(out, plain) {
		t.Fatalf("E=0 packet: %v", err)
	}
	pk[len(pk)-11] = 10
	if _, err := rx.DecryptRTCP(nil, pk); err != errAuth {
		t.Fatalf("E=0 index tamper: %v", err)
	}
}

func TestHeaderExtensionAndCSRC(t *testing.T) {
	for _, p := range profiles {
		tx, rx := pair(t, p)
		hdr := []byte{0x92, 0x6f, 0x12, 0x34, 0, 0, 0, 1, 0xde, 0xad, 0xbe, 0xef,
			0x01, 0x01, 0x01, 0x01, 0x02, 0x02, 0x02, 0x02,
			0xbe, 0xde, 0x00, 0x02, 0x10, 0xaa, 0x21, 0xbb, 0xcc, 0x00, 0x00, 0x00}
		payload := bytes.Repeat([]byte{0x33}, 25)
		plain := append(append([]byte(nil), hdr...), payload...)
		enc, err := tx.EncryptRTP(nil, plain)
		if err != nil {
			t.Fatal(err)
		}
		if !bytes.Equal(enc[:len(hdr)], hdr) {
			t.Fatalf("profile %d: header changed", p)
		}
		if bytes.Equal(enc[len(hdr):len(hdr)+len(payload)], payload) {
			t.Fatalf("profile %d: payload not encrypted", p)
		}
		dec, err := rx.DecryptRTP(nil, enc)
		if err != nil || !bytes.Equal(dec, plain) {
			t.Fatalf("profile %d: round trip %v", p, err)
		}
		if n, err := rtpHeaderLength(plain); err != nil || n != len(hdr) {
			t.Fatalf("header length %d %v", n, err)
		}
		short := append([]byte(nil), hdr...)
		short[23] = 0x20
		if _, err := tx.EncryptRTP(nil, short); err != errShortPacket {
			t.Fatalf("profile %d: overlong extension: %v", p, err)
		}
		cc := []byte{0x8f, 0x60, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1}
		if _, err := tx.EncryptRTP(nil, cc); err != errShortPacket {
			t.Fatalf("profile %d: missing csrcs: %v", p, err)
		}
		bare := append([]byte(nil), hdr...)
		bare[3] = 0x35
		empty, err := tx.EncryptRTP(nil, bare)
		if err != nil {
			t.Fatal(err)
		}
		if dec, err := rx.DecryptRTP(nil, empty); err != nil || !bytes.Equal(dec, bare) {
			t.Fatalf("profile %d: empty payload %v", p, err)
		}
		if _, err := tx.EncryptRTP(nil, []byte{0x40, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1}); err != errVersion {
			t.Fatalf("profile %d: version 1 accepted: %v", p, err)
		}
	}
}

func TestTruncatedPackets(t *testing.T) {
	for _, p := range profiles {
		tx, _ := pair(t, p)
		hdr := []byte{0x91, 0x6f, 0x12, 0x34, 0, 0, 0, 1, 0xde, 0xad, 0xbe, 0xef,
			0x01, 0x01, 0x01, 0x01, 0xbe, 0xde, 0x00, 0x01, 0x10, 0xaa, 0x00, 0x00}
		rtp, err := tx.EncryptRTP(nil, append(hdr, bytes.Repeat([]byte{1}, 30)...))
		if err != nil {
			t.Fatal(err)
		}
		rtcp, err := tx.EncryptRTCP(nil, rtcpPacket(9))
		if err != nil {
			t.Fatal(err)
		}
		for n := 0; n < len(rtp); n++ {
			_, rx := pair(t, p)
			if _, err := rx.DecryptRTP(nil, rtp[:n]); err == nil {
				t.Fatalf("profile %d: rtp truncated to %d accepted", p, n)
			}
			if n < len(hdr) {
				if _, err := tx.EncryptRTP(nil, rtp[:n]); err == nil {
					t.Fatalf("profile %d: encrypt of %d-byte header accepted", p, n)
				}
			}
		}
		for n := 0; n < len(rtcp); n++ {
			_, rx := pair(t, p)
			if _, err := rx.DecryptRTCP(nil, rtcp[:n]); err == nil {
				t.Fatalf("profile %d: rtcp truncated to %d accepted", p, n)
			}
			if n < 8 {
				if _, err := tx.EncryptRTCP(nil, rtcp[:n]); err == nil {
					t.Fatalf("profile %d: rtcp encrypt of %d bytes accepted", p, n)
				}
			}
		}
	}
}

func TestRandomInputNoPanic(t *testing.T) {
	r := rand.New(rand.NewSource(1))
	for _, p := range profiles {
		tx, rx := pair(t, p)
		for i := 0; i < 20000; i++ {
			b := make([]byte, r.Intn(80))
			r.Read(b)
			if len(b) > 0 && r.Intn(2) == 0 {
				b[0] = 0x80 | b[0]&0x3f
			}
			if _, err := rx.DecryptRTP(nil, b); err == nil {
				t.Fatalf("random rtp accepted: %x", b)
			}
			if _, err := rx.DecryptRTCP(nil, b); err == nil {
				t.Fatalf("random rtcp accepted: %x", b)
			}
			tx.EncryptRTP(nil, b)
			tx.EncryptRTCP(nil, b)
		}
	}
}

func TestDstReuse(t *testing.T) {
	for _, p := range profiles {
		tx, rx := pair(t, p)
		plain := rtpPacket(1, 1, []byte("reuse the buffer"))
		buf := make([]byte, 0, 1500)
		enc, err := tx.EncryptRTP(buf, plain)
		if err != nil {
			t.Fatal(err)
		}
		if &enc[0] != &buf[:1][0] {
			t.Fatal("large dst not used")
		}
		out := make([]byte, 0, 1500)
		dec, err := rx.DecryptRTP(out, enc)
		if err != nil || !bytes.Equal(dec, plain) || &dec[0] != &out[:1][0] {
			t.Fatalf("decrypt into dst: %v", err)
		}
		small := make([]byte, 4)
		enc2, err := tx.EncryptRTP(small, rtpPacket(2, 1, []byte("x")))
		if err != nil || &enc2[0] == &small[0] {
			t.Fatalf("small dst reused or error %v", err)
		}
	}
}

func TestAnIndexIsNeverSealedTwice(t *testing.T) {
	for _, profile := range []Profile{ProfileAES128CMHMACSHA1_80, ProfileAEADAES128GCM} {
		key := make([]byte, 16)
		salt := make([]byte, 14)
		if profile == ProfileAEADAES128GCM {
			salt = salt[:12]
		}
		tx, err := NewContext(profile, key, salt)
		if err != nil {
			t.Fatal(err)
		}
		packet := func(seq uint16, fill byte) []byte {
			p := []byte{0x80, 96, byte(seq >> 8), byte(seq), 0, 0, 0, 1, 0, 0, 0, 9, fill, fill, fill}
			return p
		}
		for seq := uint16(100); seq < 105; seq++ {
			if _, err := tx.EncryptRTP(nil, packet(seq, 1)); err != nil {
				t.Fatal(err)
			}
		}
		if _, err := tx.EncryptRTP(nil, packet(102, 2)); err == nil {
			t.Fatalf("%v sealed index 102 twice", profile)
		}
		if _, err := tx.EncryptRTP(nil, packet(98, 2)); err != nil {
			t.Fatalf("%v refused an unsent older index: %v", profile, err)
		}
		if _, err := tx.EncryptRTP(nil, packet(105, 2)); err != nil {
			t.Fatal(err)
		}
	}
}
