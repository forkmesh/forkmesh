package e2ee

import (
	"bytes"
	"encoding/json"
	"os"
	"path/filepath"
	"testing"
	"time"
)

func seedMessages(f *testing.F, pick func(v *vectorFile) [][]byte) {
	for _, name := range []string{"go-vectors.json", "js-vectors.json"} {
		b, err := os.ReadFile(filepath.Join("testdata", name))
		if err != nil {
			continue
		}
		var v vectorFile
		if json.Unmarshal(b, &v) != nil {
			continue
		}
		for _, m := range pick(&v) {
			f.Add(m)
		}
	}
	f.Add([]byte(`{}`))
	f.Add([]byte(`{"v":1,"epoch":"1"}`))
	f.Add([]byte(`null`))
}

func presenceSeeds(v *vectorFile) [][]byte {
	var out [][]byte
	for _, p := range v.Presences {
		out = append(out, p.Message)
	}
	for _, r := range v.Rejects {
		if r.What == "presence" {
			out = append(out, r.Message)
		}
	}
	return out
}

func keySeeds(v *vectorFile) [][]byte {
	var out [][]byte
	for _, k := range v.Keys {
		out = append(out, k.Message)
	}
	for _, r := range v.Rejects {
		if r.What == "keys" {
			out = append(out, r.Message)
		}
	}
	return out
}

func FuzzParsePresence(f *testing.F) {
	seedMessages(f, presenceSeeds)
	f.Fuzz(func(t *testing.T, b []byte) {
		p, err := ParsePresence(b)
		if err != nil {
			return
		}
		again, err := ParsePresence(p.Marshal())
		if err != nil {
			t.Fatalf("re-parse: %v", err)
		}
		if !bytes.Equal(again.Marshal(), p.Marshal()) || !bytes.Equal(again.SignedMessage(), p.SignedMessage()) {
			t.Fatal("presence does not round-trip")
		}
		p.Verify()
	})
}

func FuzzParseKeyMessage(f *testing.F) {
	seedMessages(f, keySeeds)
	f.Fuzz(func(t *testing.T, b []byte) {
		m, err := ParseKeyMessage(b)
		if err != nil {
			return
		}
		again, err := ParseKeyMessage(m.Marshal())
		if err != nil {
			t.Fatalf("re-parse: %v", err)
		}
		if !bytes.Equal(again.Marshal(), m.Marshal()) || !bytes.Equal(again.SignedMessage(), m.SignedMessage()) {
			t.Fatal("key message does not round-trip")
		}
		for i := 1; i < len(m.Keys); i++ {
			if m.Keys[i-1].Peer >= m.Keys[i].Peer {
				t.Fatal("unsorted keys accepted")
			}
		}
		m.Verify()
	})
}

func fuzzVectors(f *testing.F) *vectorFile {
	b, err := os.ReadFile(filepath.Join("testdata", "go-vectors.json"))
	if err != nil {
		f.Fatal(err)
	}
	var v vectorFile
	if err := json.Unmarshal(b, &v); err != nil {
		f.Fatal(err)
	}
	return &v
}

func FuzzSessionInput(f *testing.F) {
	v := fuzzVectors(f)
	for _, m := range presenceSeeds(v) {
		f.Add(true, []byte(m))
	}
	for _, m := range keySeeds(v) {
		f.Add(false, []byte(m))
	}
	f.Fuzz(func(t *testing.T, presence bool, b []byte) {
		s := sessionFor(t, v, "bob")
		for _, p := range v.Presences {
			if p.Name == "alice" {
				s.AcceptPresence(p.Message)
			}
		}
		if presence {
			s.AcceptPresence(b)
		} else {
			s.OpenKeys(b)
		}
		if len(s.Peers()) > MaxParticipants-1 {
			t.Fatal("roster grew past the limit")
		}
	})
}

func FuzzFrameOpen(f *testing.F) {
	key := frameKey("p", 1, 9)
	var s FrameSender
	s.SetKey(key)
	for _, clear := range []int{0, 1, 3, 10} {
		sealed, _ := s.Seal([]byte("a frame of media"), clear)
		f.Add(sealed, clear)
	}
	f.Add([]byte{}, 0)
	f.Add(make([]byte, FrameTrailer+FrameTag), 1)
	f.Fuzz(func(t *testing.T, b []byte, clear int) {
		var r FrameReceiver
		r.SetKey(key)
		ParseFrameTrailer(b)
		plain, err := r.Open("p", b, clear)
		if err != nil {
			return
		}
		if _, err := r.Open("p", b, clear); err == nil {
			t.Fatal("the same frame opened twice")
		}
		if len(plain) != len(b)-FrameTag-FrameTrailer {
			t.Fatal("plaintext length")
		}
	})
}

func roundSeeds(v *vectorFile, kind string) [][]byte {
	var out [][]byte
	for _, c := range v.Round.Commits {
		if kind == "commit" {
			out = append(out, c.Commit)
		} else {
			out = append(out, c.Reveal)
		}
	}
	for _, r := range v.Rejects {
		if r.What == kind {
			out = append(out, r.Message)
		}
	}
	return out
}

func FuzzParseCommit(f *testing.F) {
	seedMessages(f, func(v *vectorFile) [][]byte { return roundSeeds(v, "commit") })
	f.Fuzz(func(t *testing.T, b []byte) {
		c, err := ParseCommit(b)
		if err != nil {
			return
		}
		again, err := ParseCommit(c.Marshal())
		if err != nil || !bytes.Equal(again.Marshal(), c.Marshal()) || !bytes.Equal(again.SignedMessage(), c.SignedMessage()) {
			t.Fatal("commit does not round-trip")
		}
		c.Verify()
		c.Opens(c.C)
	})
}

func FuzzParseReveal(f *testing.F) {
	seedMessages(f, func(v *vectorFile) [][]byte { return roundSeeds(v, "reveal") })
	f.Fuzz(func(t *testing.T, b []byte) {
		r, err := ParseReveal(b)
		if err != nil {
			return
		}
		again, err := ParseReveal(r.Marshal())
		if err != nil || !bytes.Equal(again.Marshal(), r.Marshal()) {
			t.Fatal("reveal does not round-trip")
		}
	})
}

func FuzzVerificationInput(f *testing.F) {
	v := fuzzVectors(f)
	for _, m := range roundSeeds(v, "commit") {
		f.Add(true, []byte(m))
	}
	for _, m := range roundSeeds(v, "reveal") {
		f.Add(false, []byte(m))
	}
	f.Fuzz(func(t *testing.T, commit bool, b []byte) {
		s := sessionFor(t, v, "bob", bytes.Repeat([]byte{1}, 2*NonceSize))
		for _, p := range v.Presences {
			if p.Name == "alice" || p.Name == "carol" {
				s.AcceptPresence(p.Message)
			}
		}
		s.Commit()
		s.AcceptCommit(v.Round.Commits[0].Commit)
		if commit {
			s.AcceptCommit(b)
		} else {
			s.AcceptReveal(b)
		}
		s.Reveal()
		st := s.Verification()
		if st.Code != nil && len(st.Waiting) != 0 {
			t.Fatal("code while still waiting")
		}
		s.Commit()
		s.Verification()
	})
}

func FuzzParseAuthMessage(f *testing.F) {
	for _, name := range []string{"go-vectors.json", "js-vectors.json"} {
		b, err := os.ReadFile(filepath.Join("testdata", name))
		if err != nil {
			continue
		}
		var v vectorFile
		if json.Unmarshal(b, &v) != nil {
			continue
		}
		for _, a := range v.Media.Auth {
			f.Add(mustHex(f, a))
		}
		for _, r := range v.Media.Rejects {
			f.Add(mustHex(f, r.Message))
		}
	}
	f.Add(make([]byte, AuthHeaderSize+AuthDigestSize+SignatureSize))
	f.Fuzz(func(t *testing.T, b []byte) {
		m, err := ParseAuthMessage(b)
		if err != nil {
			return
		}
		if !bytes.Equal(m.Marshal(), b) {
			t.Fatal("auth message does not round-trip")
		}
		if m.Count() < 1 || m.Count() > MaxAuthFrames {
			t.Fatal("count out of range")
		}
		for i := 0; i < m.Count(); i++ {
			if _, ok := m.digestAt(m.First + uint64(i)); !ok {
				t.Fatal("digest out of range")
			}
		}
		m.Verify("call", "p", make([]byte, 32))
	})
}

func FuzzAcceptAuth(f *testing.F) {
	key := frameKey("p", 1, 9)
	var s FrameSender
	s.SetKey(key)
	var frames [][]byte
	for i := 0; i < 5; i++ {
		sealed, _ := s.Seal([]byte{byte(i), 1, 2, 3, 4, 5}, 1)
		frames = append(frames, sealed)
	}
	for _, m := range s.Flush() {
		f.Add(m, uint8(5))
	}
	f.Add([]byte{0x21, 1, 0, 0, 0, 1}, uint8(2))
	f.Fuzz(func(t *testing.T, b []byte, open uint8) {
		var r FrameReceiver
		now := time.UnixMilli(1_790_000_000_000)
		r.Now = func() time.Time { return now }
		r.SetKey(key)
		n := int(open) % (len(frames) + 1)
		for _, fr := range frames[:n] {
			r.Open("p", fr, 1)
		}
		r.AcceptAuth("p", b)
		st := r.Status("p")
		if st.Verified+st.Forged > uint64(n) {
			t.Fatal("judged more frames than were opened")
		}
		now = now.Add(2 * AuthDeadline)
		st = r.Status("p")
		if st.Verified+st.Forged+st.Missing != uint64(n) {
			t.Fatalf("frames lost track of: %+v of %d", st, n)
		}
	})
}

func FuzzComputeSAS(f *testing.F) {
	f.Add("call", uint32(1), "alice", "bob", []byte("0123456789abcdef0123456789abcdef"), []byte("n"))
	f.Fuzz(func(t *testing.T, call string, gen uint32, a, b string, idk, nonce []byte) {
		n1 := bytes.Repeat([]byte{1}, NonceSize)
		n2 := make([]byte, NonceSize)
		copy(n2, nonce)
		k1 := bytes.Repeat([]byte{7}, IdentitySize)
		members := []Member{{IDK: k1, Acct: a, Peer: "p1", Nonce: n1}, {IDK: idk, Acct: b, Peer: "p2", Nonce: n2}}
		c1, err := ComputeSAS(call, gen, members)
		if err != nil {
			return
		}
		c2, err := ComputeSAS(call, gen, []Member{members[1], members[0]})
		if err != nil || !bytes.Equal(c1.Transcript, c2.Transcript) || c1.QR != c2.QR {
			t.Fatal("code depends on member order")
		}
		q, err := ParseQR(c1.QR)
		if err != nil || !bytes.Equal(q, c1.Transcript) {
			t.Fatal("QR does not round-trip")
		}
	})
}

func FuzzParseQR(f *testing.F) {
	f.Add(sasFromTranscript(nil, bytes.Repeat([]byte{5}, 32)).QR)
	f.Add("FMCALL2:")
	f.Fuzz(func(t *testing.T, text string) {
		q, err := ParseQR(text)
		if err != nil {
			return
		}
		if sasFromTranscript(nil, q).QR != text {
			t.Fatal("QR text is not canonical")
		}
	})
}
