package e2ee

import (
	"bytes"
	"crypto/ed25519"
	"encoding/json"
	"os"
	"path/filepath"
	"testing"
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
	key := FrameKey{Peer: "p", Epoch: 1, KeyID: 1, Key: bytes.Repeat([]byte{9}, FrameKeySize)}
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

func FuzzVerificationCode(f *testing.F) {
	f.Add("call", "alice", "bob", []byte("0123456789abcdef0123456789abcdef"))
	f.Fuzz(func(t *testing.T, call, a, b string, idk []byte) {
		_, k1, _ := ed25519.GenerateKey(bytes.NewReader(bytes.Repeat([]byte{1}, 32)))
		members := []Member{{IDK: k1.Public().(ed25519.PublicKey), Acct: a}, {IDK: idk, Acct: b}}
		c1, err := VerificationCode(call, members)
		if err != nil {
			return
		}
		c2, err := VerificationCode(call, []Member{members[1], members[0]})
		if err != nil || c1 != c2 {
			t.Fatal("code depends on member order")
		}
	})
}
