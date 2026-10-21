package e2ee

import (
	"bytes"
	"math/big"
	"math/rand/v2"
	"testing"
)

func frameKey(peer string, epoch uint32, fill byte) FrameKey {
	return FrameKey{Peer: peer, Epoch: epoch, KeyID: byte(epoch), Key: bytes.Repeat([]byte{fill}, FrameKeySize)}
}

func TestFrameRoundTripKeepsClearBytes(t *testing.T) {
	var s FrameSender
	var r FrameReceiver
	k := frameKey("p", 1, 7)
	s.SetKey(k)
	r.SetKey(k)
	for _, clear := range []int{0, 1, 3, 10, 500} {
		plain := bytes.Repeat([]byte{byte(clear)}, 40)
		sealed, err := s.Seal(plain, clear)
		if err != nil {
			t.Fatal(err)
		}
		c := min(clear, len(plain))
		if !bytes.Equal(sealed[:c], plain[:c]) || len(sealed) != len(plain)+FrameTag+FrameTrailer {
			t.Fatal("clear bytes or length wrong")
		}
		got, err := r.Open("p", sealed, clear)
		if err != nil || !bytes.Equal(got, plain) {
			t.Fatalf("clear %d: %v", clear, err)
		}
		tampered := append([]byte(nil), sealed...)
		if c > 0 {
			tampered[0] ^= 1
			if _, err := r.Open("p", tampered, clear); err == nil {
				t.Fatal("clear bytes are not authenticated")
			}
		}
	}
}

func TestFrameReplayWindow(t *testing.T) {
	var s FrameSender
	var r FrameReceiver
	k := frameKey("p", 1, 1)
	s.SetKey(k)
	r.SetKey(k)
	var frames [][]byte
	for i := 0; i < 1100; i++ {
		f, _ := s.Seal([]byte{byte(i), 1, 2, 3}, 1)
		frames = append(frames, f)
	}
	if _, err := r.Open("p", frames[5], 1); err != nil {
		t.Fatal(err)
	}
	if _, err := r.Open("p", frames[5], 1); ErrorCode(err) != "stale" {
		t.Fatal("replay accepted")
	}
	if _, err := r.Open("p", frames[2], 1); err != nil {
		t.Fatal("reordered frame inside the window dropped")
	}
	if _, err := r.Open("p", frames[1099], 1); err != nil {
		t.Fatal(err)
	}
	if _, err := r.Open("p", frames[3], 1); ErrorCode(err) != "stale" {
		t.Fatal("frame older than the window accepted")
	}
	if _, err := r.Open("p", frames[1099-1023], 1); err != nil {
		t.Fatal("oldest frame inside the window dropped")
	}
	forged := append([]byte(nil), frames[1098]...)
	forged[2] ^= 1
	if _, err := r.Open("p", forged, 1); ErrorCode(err) != "invalid" {
		t.Fatal("forged frame accepted")
	}
	if _, err := r.Open("p", frames[1098], 1); err != nil {
		t.Fatal("a forged frame burned the counter")
	}
}

func TestReplayWindowMatchesAReferenceModel(t *testing.T) {
	rng := rand.New(rand.NewPCG(1, 2))
	for trial := 0; trial < 200; trial++ {
		var e replayEntry
		var top uint64
		started := false
		seen := new(big.Int)
		mask := new(big.Int).Sub(new(big.Int).Lsh(big.NewInt(1), ReplayWindow), big.NewInt(1))
		for step := 0; step < 300; step++ {
			var v uint64
			switch rng.IntN(4) {
			case 0:
				v = top + uint64(rng.IntN(3000))
			case 1:
				v = top - uint64(rng.IntN(1100))
			default:
				v = top + uint64(rng.IntN(70)) - 35
			}
			if v == 0 || v > 1<<40 {
				v = 1
			}
			want := !started || v > top
			if started && v <= top {
				age := top - v
				want = age < ReplayWindow && seen.Bit(int(age)) == 0
			}
			if e.fresh(v) != want {
				t.Fatalf("trial %d step %d: fresh(%d) = %v want %v", trial, step, v, !want, want)
			}
			if !want {
				continue
			}
			e.mark(v)
			switch {
			case !started:
				started, top = true, v
				seen.SetInt64(1)
			case v > top:
				seen.Lsh(seen, uint(v-top)).Or(seen, big.NewInt(1)).And(seen, mask)
				top = v
			default:
				seen.SetBit(seen, int(top-v), 1)
			}
		}
	}
}

func TestFrameSenderKeepsCounterForTheSameKey(t *testing.T) {
	var s FrameSender
	k := frameKey("p", 1, 3)
	s.SetKey(k)
	a, _ := s.Seal([]byte("x"), 0)
	s.SetKey(k)
	b, _ := s.Seal([]byte("x"), 0)
	ta, _ := ParseFrameTrailer(a)
	tb, _ := ParseFrameTrailer(b)
	if tb.Counter != ta.Counter+1 {
		t.Fatal("re-installing the same key restarted the counter")
	}
	s.SetKey(frameKey("p", 2, 4))
	c, _ := s.Seal([]byte("x"), 0)
	tc, _ := ParseFrameTrailer(c)
	if tc.Counter != 1 || tc.KeyID != 2 {
		t.Fatal("a new key did not restart the counter")
	}
}

func TestFrameReceiverRejectsRollbackAndKeepsThreeKeys(t *testing.T) {
	var r FrameReceiver
	for e := uint32(1); e <= 4; e++ {
		if err := r.SetKey(frameKey("p", e, byte(e))); err != nil {
			t.Fatal(err)
		}
	}
	if err := r.SetKey(frameKey("p", 2, 9)); ErrorCode(err) != "stale" {
		t.Fatal("older epoch installed")
	}
	if err := r.SetKey(frameKey("p", 4, 4)); err != nil {
		t.Fatal("re-installing the newest key failed")
	}
	var s FrameSender
	s.SetKey(frameKey("p", 1, 1))
	f, _ := s.Seal([]byte("old"), 0)
	if _, err := r.Open("p", f, 0); ErrorCode(err) != "unknown-peer" {
		t.Fatal("dropped key still opens")
	}
	s.SetKey(frameKey("p", 2, 2))
	f, _ = s.Seal([]byte("kept"), 0)
	if _, err := r.Open("p", f, 0); err != nil {
		t.Fatal(err)
	}
}

func TestKeyIDWrapDoesNotConfuseEpochs(t *testing.T) {
	var r FrameReceiver
	r.SetKey(frameKey("p", 1, 1))
	r.SetKey(frameKey("p", 257, 2))
	var s FrameSender
	s.SetKey(frameKey("p", 1, 1))
	f, _ := s.Seal([]byte("epoch one"), 0)
	if _, err := r.Open("p", f, 0); err == nil {
		t.Fatal("epoch 1 frame opened under epoch 257's key id")
	}
	s.SetKey(frameKey("p", 257, 2))
	f, _ = s.Seal([]byte("epoch 257"), 0)
	if _, err := r.Open("p", f, 0); err != nil {
		t.Fatal(err)
	}
}

func TestFramesFromAnotherPeerDoNotOpen(t *testing.T) {
	c := newClock()
	a, b, d := newParticipant(t, c, "call-1", "alice"), newParticipant(t, c, "call-1", "bob"), newParticipant(t, c, "call-1", "dave")
	meet(t, a, b, d)
	ka := distribute(t, a, b, d)
	kb := distribute(t, b, a, d)
	var r FrameReceiver
	r.SetKey(ka)
	r.SetKey(kb)
	var s FrameSender
	s.SetKey(ka)
	f, _ := s.Seal([]byte("from alice"), 1)
	if _, err := r.Open(b.Peer(), f, 1); err == nil {
		t.Fatal("alice's frame opened as bob's")
	}
	if _, err := r.Open(a.Peer(), f, 1); err != nil {
		t.Fatal(err)
	}
}

func TestLegacy128BitKeysStillWork(t *testing.T) {
	var s FrameSender
	var r FrameReceiver
	k := FrameKey{Peer: "p", Epoch: 1, KeyID: 0, Key: bytes.Repeat([]byte{5}, 16)}
	s.SetKey(k)
	r.SetKey(k)
	f, _ := s.Seal([]byte("legacy"), 1)
	if got, err := r.Open("p", f, 1); err != nil || string(got) != "legacy" {
		t.Fatal(err)
	}
	if err := s.SetKey(FrameKey{Key: make([]byte, 24)}); err == nil {
		t.Fatal("AES-192 key accepted")
	}
}
