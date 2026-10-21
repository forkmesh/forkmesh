package e2ee

import (
	"bytes"
	"crypto/ed25519"
	"crypto/rand"
	"fmt"
	"testing"
	"time"
)

type authRig struct {
	now    time.Time
	key    FrameKey
	sender FrameSender
	recv   FrameReceiver
	states []AuthState
}

func newAuthRig(t testing.TB) *authRig {
	t.Helper()
	g := &authRig{now: time.UnixMilli(1_790_000_000_000), key: frameKey("alice", 1, 4)}
	g.recv.Now = func() time.Time { return g.now }
	g.recv.OnAuth = func(peer string, st AuthState) { g.states = append(g.states, st) }
	if err := g.sender.SetKey(g.key); err != nil {
		t.Fatal(err)
	}
	recvKey := g.key
	recvKey.AuthSeed = nil
	if err := g.recv.SetKey(recvKey); err != nil {
		t.Fatal(err)
	}
	return g
}

func (g *authRig) seal(t testing.TB, n int) [][]byte {
	t.Helper()
	var out [][]byte
	for i := 0; i < n; i++ {
		f, err := g.sender.Seal([]byte(fmt.Sprintf("frame %d of media", i)), 1)
		if err != nil {
			t.Fatal(err)
		}
		out = append(out, f)
	}
	return out
}

func (g *authRig) open(t testing.TB, frames ...[]byte) {
	t.Helper()
	for _, f := range frames {
		if _, err := g.recv.Open("alice", f, 1); err != nil {
			t.Fatalf("open: %v", err)
		}
	}
}

func (g *authRig) auth(t testing.TB) {
	t.Helper()
	for _, m := range g.sender.Flush() {
		if err := g.recv.AcceptAuth("alice", m); err != nil {
			t.Fatalf("auth: %v", err)
		}
	}
}

func TestMediaAuthVerifiesEveryFrame(t *testing.T) {
	g := newAuthRig(t)
	g.open(t, g.seal(t, 30)...)
	if st := g.recv.Status("alice"); st.State != AuthPending || st.Verified != 0 {
		t.Fatalf("before auth: %+v", st)
	}
	g.auth(t)
	st := g.recv.Status("alice")
	if st.State != AuthVerified || st.Verified != 30 || st.Missing != 0 || st.Forged != 0 || st.Dropping {
		t.Fatalf("after auth: %+v", st)
	}
	if len(g.states) != 1 || g.states[0] != AuthVerified {
		t.Fatalf("events: %v", g.states)
	}
	g.now = g.now.Add(time.Minute)
	if g.recv.Status("alice").State != AuthVerified {
		t.Fatal("a quiet sender lost its verified state")
	}
}

func TestFramesArrivingAfterTheirAuthMessageVerifyAtOnce(t *testing.T) {
	g := newAuthRig(t)
	frames := g.seal(t, 5)
	g.open(t, frames[:2]...)
	g.auth(t)
	g.open(t, frames[2:]...)
	if st := g.recv.Status("alice"); st.Verified != 5 || st.State != AuthVerified {
		t.Fatalf("late frames: %+v", st)
	}
}

func colluder(t testing.TB, g *authRig) *FrameSender {
	t.Helper()
	k := g.key
	k.Auth = nil
	k.AuthSeed = bytes.Repeat([]byte{0xee}, ed25519.SeedSize)
	var s FrameSender
	if err := s.SetKey(k); err != nil {
		t.Fatal(err)
	}
	return &s
}

func TestColludingParticipantCannotForgeMedia(t *testing.T) {
	g := newAuthRig(t)
	m := colluder(t, g)
	real := g.seal(t, 3)
	fake, _ := m.Seal([]byte("words alice never said"), 1)
	if _, err := g.recv.Open("alice", fake, 1); err != nil {
		t.Fatalf("a frame from someone holding the frame key should pass its GCM tag: %v", err)
	}
	g.open(t, real[1:]...)
	g.auth(t)
	st := g.recv.Status("alice")
	if st.State != AuthForged || st.Forged != 1 || st.Verified != 2 || !st.Dropping {
		t.Fatalf("forgery: %+v", st)
	}
	if g.states[len(g.states)-1] != AuthForged {
		t.Fatalf("events: %v", g.states)
	}
	more := g.seal(t, 2)
	if _, err := g.recv.Open("alice", more[0], 1); ErrorCode(err) != "forged" {
		t.Fatalf("stream not dropped: %v", err)
	}
	for _, mm := range m.Flush() {
		if err := g.recv.AcceptAuth("alice", mm); ErrorCode(err) != "signature" {
			t.Fatalf("colluder's auth message: %v", err)
		}
	}
	g.now = g.now.Add(AuthHold + time.Second)
	g.auth(t)
	if _, err := g.recv.Open("alice", more[1], 1); err != nil {
		t.Fatalf("stream did not recover after the hold: %v", err)
	}
	g.open(t, g.seal(t, 3)...)
	g.auth(t)
	g.now = g.now.Add(AuthRecent + time.Second)
	if st := g.recv.Status("alice"); st.State != AuthVerified || st.Dropping {
		t.Fatalf("after hold: %+v", st)
	}
}

func TestForgedFrameAfterItsAuthMessageIsRefusedAtOnce(t *testing.T) {
	g := newAuthRig(t)
	m := colluder(t, g)
	real := g.seal(t, 2)
	g.open(t, real[0])
	g.auth(t)
	m.Seal([]byte("x"), 1)
	fake, _ := m.Seal([]byte("y"), 1)
	if _, err := g.recv.Open("alice", fake, 1); ErrorCode(err) != "forged" {
		t.Fatalf("known-bad frame played: %v", err)
	}
}

func TestMissingAuthTurnsUnverifiedThenDrops(t *testing.T) {
	g := newAuthRig(t)
	var dropped error
	for i := 0; i < 40; i++ {
		f := g.seal(t, 1)[0]
		g.sender.Flush()
		if _, err := g.recv.Open("alice", f, 1); err != nil {
			dropped = err
			break
		}
		g.now = g.now.Add(100 * time.Millisecond)
		if i == 12 {
			if st := g.recv.Status("alice"); st.State != AuthMissing || st.Dropping {
				t.Fatalf("after the deadline: %+v", st)
			}
		}
	}
	if ErrorCode(dropped) != "unverified" {
		t.Fatalf("unauthenticated media kept playing: %v", dropped)
	}
	elapsed := g.recv.Status("alice")
	if !elapsed.Dropping || elapsed.State != AuthMissing {
		t.Fatalf("status: %+v", elapsed)
	}
	f := g.seal(t, 1)[0]
	if _, err := g.recv.Open("alice", f, 1); ErrorCode(err) != "unverified" {
		t.Fatal(err)
	}
	g.auth(t)
	if g.recv.Status("alice").Dropping {
		t.Fatal("a verified frame did not lift the drop")
	}
	g.now = g.now.Add(AuthDeadline + 100*time.Millisecond)
	g.recv.Status("alice")
	g.now = g.now.Add(AuthRecent + time.Second)
	if st := g.recv.Status("alice"); st.State != AuthVerified || st.Dropping {
		t.Fatalf("recovery: %+v", st)
	}
}

func TestAShortAuthGapDoesNotDrop(t *testing.T) {
	g := newAuthRig(t)
	lost := g.seal(t, 5)
	g.open(t, lost...)
	g.sender.Flush()
	g.now = g.now.Add(AuthDeadline + 100*time.Millisecond)
	next := g.seal(t, 5)
	g.open(t, next...)
	g.auth(t)
	st := g.recv.Status("alice")
	if st.Missing != 5 || st.Verified != 5 || st.Dropping || st.State != AuthMissing {
		t.Fatalf("one lost auth message: %+v", st)
	}
}

func TestAuthEquivocationAndDuplicates(t *testing.T) {
	g := newAuthRig(t)
	frames := g.seal(t, 4)
	g.open(t, frames...)
	msgs := g.sender.Flush()
	if err := g.recv.AcceptAuth("alice", msgs[0]); err != nil {
		t.Fatal(err)
	}
	if err := g.recv.AcceptAuth("alice", msgs[0]); ErrorCode(err) != "duplicate" {
		t.Fatalf("duplicate: %v", err)
	}
	m, _ := ParseAuthMessage(msgs[0])
	overlap := &AuthMessage{KeyID: m.KeyID, Epoch: m.Epoch, First: m.First + 2, Digests: append([]byte(nil), m.Digests[2*AuthDigestSize:]...)}
	overlap.Digests = append(overlap.Digests, bytes.Repeat([]byte{1}, AuthDigestSize)...)
	overlap.Sign("call-1", "alice", ed25519.NewKeyFromSeed(g.key.AuthSeed))
	if err := g.recv.AcceptAuth("alice", overlap.Marshal()); err != nil {
		t.Fatalf("consistent overlap: %v", err)
	}
	lie := &AuthMessage{KeyID: m.KeyID, Epoch: m.Epoch, First: m.First, Digests: bytes.Repeat([]byte{2}, AuthDigestSize)}
	lie.Sign("call-1", "alice", ed25519.NewKeyFromSeed(g.key.AuthSeed))
	if err := g.recv.AcceptAuth("alice", lie.Marshal()); ErrorCode(err) != "equivocation" {
		t.Fatalf("equivocation: %v", err)
	}
	if g.recv.Status("alice").State != AuthForged {
		t.Fatal("sender equivocation not flagged")
	}
}

func TestAuthWindowsSplitAndFollowKeyChanges(t *testing.T) {
	g := newAuthRig(t)
	frames := g.seal(t, 2*MaxAuthFrames+50)
	next := frameKey("alice", 2, 5)
	g.sender.SetKey(next)
	recvNext := next
	recvNext.AuthSeed = nil
	g.recv.SetKey(recvNext)
	after := g.seal(t, 3)
	msgs := g.sender.Flush()
	if len(msgs) != 4 {
		t.Fatalf("got %d auth messages", len(msgs))
	}
	sizes := []int{MaxAuthFrames, MaxAuthFrames, 50, 3}
	for i, b := range msgs {
		m, err := ParseAuthMessage(b)
		if err != nil || m.Count() != sizes[i] || len(b) > MaxAuthSize {
			t.Fatalf("message %d: %v %d", i, err, len(b))
		}
		if (i < 3 && m.Epoch != 1) || (i == 3 && (m.Epoch != 2 || m.First != 1)) {
			t.Fatalf("message %d names epoch %d", i, m.Epoch)
		}
	}
	g.open(t, frames...)
	g.open(t, after...)
	for _, b := range msgs {
		if err := g.recv.AcceptAuth("alice", b); err != nil {
			t.Fatal(err)
		}
	}
	if st := g.recv.Status("alice"); st.Verified != uint64(len(frames)+len(after)) {
		t.Fatalf("status: %+v", st)
	}
	if g.sender.Flush() != nil {
		t.Fatal("flush with nothing sealed returned messages")
	}
}

func TestAuthFromTheWrongPlaceIsRefused(t *testing.T) {
	g := newAuthRig(t)
	g.open(t, g.seal(t, 2)...)
	msgs := g.sender.Flush()
	if err := g.recv.AcceptAuth("bob", msgs[0]); ErrorCode(err) != "unknown-peer" {
		t.Fatal(err)
	}
	bob := frameKey("bob", 1, 4)
	bob.AuthSeed = nil
	bob.Auth = frameKey("bob", 1, 8).Auth
	g.recv.SetKey(bob)
	if err := g.recv.AcceptAuth("bob", msgs[0]); ErrorCode(err) != "signature" {
		t.Fatalf("alice's auth message as bob's: %v", err)
	}
	m, _ := ParseAuthMessage(msgs[0])
	m.Sign("call-2", "alice", ed25519.NewKeyFromSeed(g.key.AuthSeed))
	if err := g.recv.AcceptAuth("alice", m.Marshal()); ErrorCode(err) != "signature" {
		t.Fatalf("auth message from another call: %v", err)
	}
}

type mediaProfile struct {
	name  string
	sizes []int
	fps   []int
}

var profiles = []mediaProfile{
	{"audio only (Opus 20 ms)", []int{40}, []int{50}},
	{"audio + one video layer at 30 fps", []int{60, 3000}, []int{50, 30}},
	{"audio + simulcast 2 layers at 30 fps", []int{60, 3000, 900}, []int{50, 30, 30}},
}

func TestAuthOverheadPerSecond(t *testing.T) {
	for _, p := range profiles {
		g := newAuthRig(t)
		frames := 0
		for _, fps := range p.fps {
			frames += fps
		}
		var bytesOut int
		var msgs int
		for tick := 0; tick < int(time.Second/AuthInterval); tick++ {
			for i, fps := range p.fps {
				for n := 0; n < fps*int(AuthInterval/time.Millisecond)/1000; n++ {
					g.sender.Seal(make([]byte, p.sizes[i]), 1)
				}
			}
			for _, m := range g.sender.Flush() {
				bytesOut += len(m)
				msgs++
			}
		}
		perFrame := 64 * frames
		t.Logf("%s: %d frames/s, %d auth messages/s, %d B/s (%.1f kbit/s) vs %d B/s for a 64-byte signature per frame",
			p.name, frames, msgs, bytesOut, float64(bytesOut*8)/1000, perFrame)
		if bytesOut >= perFrame/3 {
			t.Fatalf("auth overhead %d B/s is not well below per-frame signatures", bytesOut)
		}
	}
}

func benchmarkSeal(b *testing.B, size int) {
	g := newAuthRig(b)
	frame := make([]byte, size)
	rand.Read(frame)
	b.SetBytes(int64(size))
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		g.sender.Seal(frame, 1)
		if i%10 == 9 {
			g.sender.Flush()
		}
	}
}

func BenchmarkSealAudioWithAuth(b *testing.B)    { benchmarkSeal(b, 60) }
func BenchmarkSealVideoWithAuth(b *testing.B)    { benchmarkSeal(b, 3000) }
func BenchmarkSealKeyframeWithAuth(b *testing.B) { benchmarkSeal(b, 30000) }

func BenchmarkSealAudioWithoutAuthDigest(b *testing.B) {
	g := newAuthRig(b)
	frame := make([]byte, 60)
	for i := 0; i < b.N; i++ {
		g.sender.aead.Seal(nil, frameNonce(uint64(i+1)), frame[1:], frame[:1])
	}
}

func benchmarkOpen(b *testing.B, size int) {
	g := newAuthRig(b)
	frame := make([]byte, size)
	sealed := make([][]byte, 0, 10)
	b.SetBytes(int64(size))
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		b.StopTimer()
		if len(sealed) == 0 {
			for j := 0; j < 10; j++ {
				f, _ := g.sender.Seal(frame, 1)
				sealed = append(sealed, f)
			}
		}
		f := sealed[0]
		sealed = sealed[1:]
		b.StartTimer()
		g.recv.Open("alice", f, 1)
		if len(sealed) == 0 {
			for _, m := range g.sender.Flush() {
				g.recv.AcceptAuth("alice", m)
			}
		}
	}
}

func BenchmarkOpenAudioWithAuth(b *testing.B) { benchmarkOpen(b, 60) }
func BenchmarkOpenVideoWithAuth(b *testing.B) { benchmarkOpen(b, 3000) }

func BenchmarkAuthMessageSign(b *testing.B) {
	key := ed25519.NewKeyFromSeed(bytes.Repeat([]byte{1}, 32))
	m := &AuthMessage{KeyID: 1, Epoch: 1, First: 1, Digests: make([]byte, 22*AuthDigestSize)}
	for i := 0; i < b.N; i++ {
		m.Sign("call-1", "alice", key)
	}
}

func BenchmarkAuthMessageVerify(b *testing.B) {
	key := ed25519.NewKeyFromSeed(bytes.Repeat([]byte{1}, 32))
	m := &AuthMessage{KeyID: 1, Epoch: 1, First: 1, Digests: make([]byte, 22*AuthDigestSize)}
	m.Sign("call-1", "alice", key)
	pub := key.Public().(ed25519.PublicKey)
	for i := 0; i < b.N; i++ {
		m.Verify("call-1", "alice", pub)
	}
}
