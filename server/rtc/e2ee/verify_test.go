package e2ee

import (
	"bytes"
	"crypto/ed25519"
	"regexp"
	"testing"
	"time"
)

func mustCommit(t testing.TB, s *Session) []byte {
	t.Helper()
	b, err := s.Commit()
	if err != nil {
		t.Fatal(err)
	}
	return b
}

func sendCommit(t testing.TB, from *Session, to ...*Session) {
	t.Helper()
	b := mustCommit(t, from)
	for _, r := range to {
		if err := r.AcceptCommit(b); err != nil && ErrorCode(err) != "duplicate" {
			t.Fatalf("accept commit: %v", err)
		}
	}
}

func sendReveal(t testing.TB, from *Session, to ...*Session) {
	t.Helper()
	b, err := from.Reveal()
	if err != nil {
		t.Fatalf("reveal: %v", err)
	}
	for _, r := range to {
		if err := r.AcceptReveal(b); err != nil && ErrorCode(err) != "duplicate" {
			t.Fatalf("accept reveal: %v", err)
		}
	}
}

func others(all []*Session, me *Session) []*Session {
	var out []*Session
	for _, s := range all {
		if s != me {
			out = append(out, s)
		}
	}
	return out
}

func runRound(t testing.TB, all ...*Session) *SAS {
	t.Helper()
	for _, s := range all {
		sendCommit(t, s, others(all, s)...)
	}
	for _, s := range all {
		sendReveal(t, s, others(all, s)...)
	}
	var code *SAS
	for _, s := range all {
		st := s.Verification()
		if st.Code == nil || len(st.Waiting) != 0 || st.NeedCommit {
			t.Fatalf("round did not finish: %+v", st)
		}
		if code != nil && !bytes.Equal(code.Transcript, st.Code.Transcript) {
			t.Fatal("participants computed different codes")
		}
		code = st.Code
	}
	return code
}

var digitsShape = regexp.MustCompile(`^\d{3} \d{3}$`)

func TestShortCodeRoundAgrees(t *testing.T) {
	c := newClock()
	a, b, d := newParticipant(t, c, "call-1", "alice"), newParticipant(t, c, "call-1", "bob"), newParticipant(t, c, "call-1", "dave")
	meet(t, a, b, d)
	code := runRound(t, a, b, d)
	if !digitsShape.MatchString(code.Digits) || len(code.QR) != len(QRPrefix)+52 {
		t.Fatalf("code shape: %+v", code)
	}
	for _, e := range code.Emoji {
		if e < 0 || e > 63 {
			t.Fatal("emoji index out of range")
		}
	}
	if q, err := ParseQR(code.QR); err != nil || !bytes.Equal(q, code.Transcript) {
		t.Fatal("QR does not carry the transcript")
	}
	if code.EmojiText() == "" || code.EmojiNames() == "" {
		t.Fatal("emoji text")
	}
	if again := runRound(t, a, b, d); !bytes.Equal(again.Transcript, code.Transcript) {
		t.Fatal("a quiet roster restarted the round")
	}
}

func TestRevealWaitsForEveryCommit(t *testing.T) {
	c := newClock()
	a, b, d := newParticipant(t, c, "call-1", "alice"), newParticipant(t, c, "call-1", "bob"), newParticipant(t, c, "call-1", "dave")
	meet(t, a, b, d)
	if _, err := a.Reveal(); ErrorCode(err) != "waiting" {
		t.Fatalf("reveal before own commit: %v", err)
	}
	sendCommit(t, a, b, d)
	sendCommit(t, b, a, d)
	if _, err := a.Reveal(); ErrorCode(err) != "waiting" {
		t.Fatalf("reveal before dave's commit: %v", err)
	}
	st := a.Verification()
	if len(st.Waiting) != 1 || st.Waiting[0] != d.Peer() || st.Code != nil {
		t.Fatalf("waiting list: %+v", st)
	}
	c.add(RevealTimeout + time.Second)
	if !a.Verification().Late {
		t.Fatal("late round not flagged")
	}
	sendCommit(t, d, a, b)
	if _, err := a.Reveal(); err != nil {
		t.Fatal(err)
	}
}

func TestSplitViewGivesDifferentCodes(t *testing.T) {
	c := newClock()
	a, b := newParticipant(t, c, "call-1", "alice"), newParticipant(t, c, "call-1", "bob")
	fakeBob, fakeAlice := newParticipant(t, c, "call-1", "bob"), newParticipant(t, c, "call-1", "alice")
	fakeBob.peer, fakeAlice.peer = b.peer, a.peer
	meet(t, a, fakeBob)
	meet(t, b, fakeAlice)
	ca := runRound(t, a, fakeBob)
	cb := runRound(t, b, fakeAlice)
	if bytes.Equal(ca.Transcript, cb.Transcript) || bytes.Equal(ca.Round, cb.Round) {
		t.Fatal("a split view produced the same code")
	}
}

func TestAttackerCannotRecommitAfterSeeingANonce(t *testing.T) {
	c := newClock()
	a, m := newParticipant(t, c, "call-1", "alice"), newParticipant(t, c, "call-1", "mallory")
	meet(t, a, m)
	sendCommit(t, m, a)
	sendCommit(t, a, m)
	sendReveal(t, a, m)
	forged := clonedCommit(t, mustCommit(t, m))
	forged.C = commitHash(forged.Call, forged.Peer, forged.IDK, forged.Round, forged.Gen, bytes.Repeat([]byte{7}, NonceSize))
	if err := a.AcceptCommit(resignCommit(forged, m.id)); ErrorCode(err) != "equivocation" {
		t.Fatalf("second commit for the same round: %v", err)
	}
	if err := a.AcceptReveal((&Reveal{Call: forged.Call, Peer: forged.Peer, Round: forged.Round, Gen: forged.Gen, Nonce: bytes.Repeat([]byte{7}, NonceSize)}).Marshal()); ErrorCode(err) != "commitment" {
		t.Fatalf("reveal of a nonce never committed: %v", err)
	}
	if a.Verification().Rounds != 1 {
		t.Fatal("round count")
	}
	bumped := clonedCommit(t, mustCommit(t, m))
	bumped.Gen++
	bumped.Round, _ = RoundID("call-1", bumped.Gen, a.rosterLocked())
	if err := a.AcceptCommit(resignCommit(bumped, m.id)); err != nil {
		t.Fatal(err)
	}
	st := a.Verification()
	if !st.NeedCommit || st.Code != nil {
		t.Fatalf("a higher generation did not restart the round: %+v", st)
	}
	mustCommit(t, a)
	if a.Verification().Unfinished != 1 {
		t.Fatal("abandoned revealed round not counted")
	}
}

func clonedCommit(t testing.TB, b []byte) *Commit {
	t.Helper()
	c, err := ParseCommit(b)
	if err != nil {
		t.Fatal(err)
	}
	return c
}

func resignCommit(c *Commit, key ed25519.PrivateKey) []byte {
	c.Sig = ed25519.Sign(key, c.SignedMessage())
	return c.Marshal()
}

func TestTransientRosterChangeConvergesWithoutLivelock(t *testing.T) {
	c := newClock()
	a, b := newParticipant(t, c, "call-1", "alice"), newParticipant(t, c, "call-1", "bob")
	ghost := newParticipant(t, c, "call-1", "ghost")
	meet(t, a, b)
	first := runRound(t, a, b)
	b.AcceptPresence(mustPresence(t, ghost))
	mustCommit(t, b)
	b.Leave(ghost.Peer())
	sendCommit(t, b, a)
	if !a.Verification().NeedCommit {
		t.Fatal("alice did not follow bob's higher generation")
	}
	for i := 0; i < 4; i++ {
		ca, err := a.Commit()
		if err != nil {
			t.Fatal(err)
		}
		cb, _ := b.Commit()
		b.AcceptCommit(ca)
		a.AcceptCommit(cb)
		if ra, err := a.Reveal(); err == nil {
			b.AcceptReveal(ra)
		}
		if rb, err := b.Reveal(); err == nil {
			a.AcceptReveal(rb)
		}
	}
	sa, sb := a.Verification(), b.Verification()
	if sa.Code == nil || sb.Code == nil || !bytes.Equal(sa.Code.Transcript, sb.Code.Transcript) {
		t.Fatalf("did not converge: %+v %+v", sa, sb)
	}
	if bytes.Equal(sa.Code.Transcript, first.Transcript) || sa.Gen <= 1 {
		t.Fatal("new generation reused the old code")
	}
}

func TestEarlyRevealIsHeldUntilItsCommit(t *testing.T) {
	c := newClock()
	a, b := newParticipant(t, c, "call-1", "alice"), newParticipant(t, c, "call-1", "bob")
	meet(t, a, b)
	cb := mustCommit(t, b)
	sendCommit(t, a, b)
	rb, err := b.Reveal()
	if err != nil {
		t.Fatal(err)
	}
	if err := a.AcceptReveal(rb); ErrorCode(err) != "waiting" {
		t.Fatalf("reveal before commit: %v", err)
	}
	if err := a.AcceptCommit(cb); err != nil {
		t.Fatal(err)
	}
	sendReveal(t, a, b)
	if a.Verification().Code == nil || b.Verification().Code == nil {
		t.Fatal("held reveal was not applied")
	}
	if err := a.AcceptReveal(rb); ErrorCode(err) != "duplicate" {
		t.Fatalf("replayed reveal: %v", err)
	}
}

func TestCommitAndRevealRejects(t *testing.T) {
	c := newClock()
	a, b, m := newParticipant(t, c, "call-1", "alice"), newParticipant(t, c, "call-1", "bob"), newParticipant(t, c, "call-1", "mallory")
	meet(t, a, b)
	cb := mustCommit(t, b)
	if err := m.AcceptCommit(cb); ErrorCode(err) != "unknown-peer" {
		t.Fatalf("unknown sender: %v", err)
	}
	if err := a.AcceptCommit(mustCommit(t, a)); ErrorCode(err) != "own" {
		t.Fatalf("own commit: %v", err)
	}
	stolen := clonedCommit(t, mustCommit(t, m))
	stolen.Peer = b.Peer()
	if err := a.AcceptCommit(resignCommit(stolen, m.id)); ErrorCode(err) != "peer-bound" {
		t.Fatalf("other identity: %v", err)
	}
	bad := clonedCommit(t, cb)
	bad.Sig[3] ^= 1
	if err := a.AcceptCommit(bad.Marshal()); ErrorCode(err) != "signature" {
		t.Fatalf("bad signature: %v", err)
	}
	old := clonedCommit(t, cb)
	old.TS -= (6 * time.Minute).Milliseconds()
	if err := a.AcceptCommit(resignCommit(old, b.id)); ErrorCode(err) != "clock" {
		t.Fatalf("old commit: %v", err)
	}
	far := clonedCommit(t, cb)
	far.Gen = MaxGenJump + 5
	if err := a.AcceptCommit(resignCommit(far, b.id)); ErrorCode(err) != "invalid" {
		t.Fatalf("generation jump: %v", err)
	}
	other := clonedCommit(t, cb)
	other.Call = "call-2"
	if err := a.AcceptCommit(resignCommit(other, b.id)); ErrorCode(err) != "call" {
		t.Fatalf("other call: %v", err)
	}
	if err := a.AcceptCommit(cb); err != nil {
		t.Fatal(err)
	}
	if err := a.AcceptCommit(cb); ErrorCode(err) != "duplicate" {
		t.Fatalf("duplicate: %v", err)
	}
	for _, raw := range [][]byte{
		patchJSON(t, cb, "v", 1), patchJSON(t, cb, "t", "reveal"), patchJSON(t, cb, "gen", 0),
		patchJSON(t, cb, "round", "AAAA"), patchJSON(t, cb, "c", nil), patchJSON(t, cb, "gen", "1"),
	} {
		if err := a.AcceptCommit(raw); ErrorCode(err) != "invalid" {
			t.Fatalf("malformed commit %s: %v", raw, err)
		}
	}
	rb := (&Reveal{Call: "call-1", Peer: b.Peer(), Round: clonedCommit(t, cb).Round, Gen: 1, Nonce: make([]byte, NonceSize)}).Marshal()
	if err := a.AcceptReveal(rb); ErrorCode(err) != "commitment" {
		t.Fatalf("wrong nonce: %v", err)
	}
	if err := a.AcceptReveal(patchJSON(t, rb, "t", "commit")); ErrorCode(err) != "invalid" {
		t.Fatalf("mistyped reveal: %v", err)
	}
}

func TestVerifiedSurvivesLeavesButNotNewcomers(t *testing.T) {
	c := newClock()
	a, b, d := newParticipant(t, c, "call-1", "alice"), newParticipant(t, c, "call-1", "bob"), newParticipant(t, c, "call-1", "dave")
	meet(t, a, b, d)
	if st := a.Verification(); st.Verified || len(st.Newcomers) != 0 {
		t.Fatal("verified before anyone compared")
	}
	code := runRound(t, a, b, d)
	if err := a.MarkVerified(bytes.Repeat([]byte{1}, RoundSize)); ErrorCode(err) != "waiting" {
		t.Fatal("marked a round that is not current")
	}
	if err := a.MarkVerified(code.Round); err != nil || !a.Verification().Verified {
		t.Fatal("mark verified")
	}
	a.Leave(d.Peer())
	b.Leave(d.Peer())
	if !a.Verification().Verified {
		t.Fatal("a leave cleared verified")
	}
	e := newParticipant(t, c, "call-1", "erin")
	meet(t, a, b, e)
	st := a.Verification()
	if st.Verified || len(st.Newcomers) != 1 || st.Newcomers[0] != e.Peer() {
		t.Fatalf("newcomer: %+v", st)
	}
	code = runRound(t, a, b, e)
	ok, err := a.CheckQR(code.QR)
	if err != nil || !ok || !a.Verification().Verified {
		t.Fatalf("QR verification: %v %v", ok, err)
	}
	wrong := sasFromTranscript(code.Round, bytes.Repeat([]byte{9}, 32)).QR
	if ok, err := b.CheckQR(wrong); ok || err != nil {
		t.Fatal("a different QR matched")
	}
	if _, err := b.CheckQR("FMCALL2:abc"); ErrorCode(err) != "invalid" {
		t.Fatal("malformed QR accepted")
	}
	a.Unverify()
	if a.Verification().Verified {
		t.Fatal("unverify")
	}
}

func TestRevealedRoundThatNeverFinishesIsCounted(t *testing.T) {
	c := newClock()
	a, b := newParticipant(t, c, "call-1", "alice"), newParticipant(t, c, "call-1", "bob")
	meet(t, a, b)
	sendCommit(t, b, a)
	sendCommit(t, a, b)
	sendReveal(t, a, b)
	c.add(RoundTimeout + time.Second)
	st := a.Verification()
	if st.Unfinished != 1 || st.Code != nil || !st.Late {
		t.Fatalf("withheld reveal: %+v", st)
	}
	a.Verification()
	if a.Verification().Unfinished != 1 {
		t.Fatal("counted twice")
	}
}

func TestSASIgnoresMemberOrderAndBindsEverything(t *testing.T) {
	id := func(b byte) []byte { return bytes.Repeat([]byte{b}, IdentitySize) }
	n := func(b byte) []byte { return bytes.Repeat([]byte{b}, NonceSize) }
	ms := []Member{{IDK: id(1), Acct: "alice", Peer: "p1", Nonce: n(1)}, {IDK: id(2), Acct: "bob", Peer: "p2", Nonce: n(2)}}
	base, err := ComputeSAS("call", 1, ms)
	if err != nil {
		t.Fatal(err)
	}
	swapped, _ := ComputeSAS("call", 1, []Member{ms[1], ms[0]})
	if !bytes.Equal(base.Transcript, swapped.Transcript) {
		t.Fatal("order matters")
	}
	variants := []func([]Member) (string, uint32, []Member){
		func(m []Member) (string, uint32, []Member) { return "call2", 1, m },
		func(m []Member) (string, uint32, []Member) { return "call", 2, m },
		func(m []Member) (string, uint32, []Member) { m[0].Acct = "alicf"; return "call", 1, m },
		func(m []Member) (string, uint32, []Member) { m[0].Peer = "p3"; return "call", 1, m },
		func(m []Member) (string, uint32, []Member) { m[1].IDK = id(3); return "call", 1, m },
		func(m []Member) (string, uint32, []Member) { m[1].Nonce = n(3); return "call", 1, m },
	}
	for i, v := range variants {
		cp := append([]Member(nil), ms...)
		call, gen, list := v(cp)
		got, err := ComputeSAS(call, gen, list)
		if err != nil || bytes.Equal(got.Transcript, base.Transcript) {
			t.Fatalf("variant %d does not change the code", i)
		}
	}
	if _, err := ComputeSAS("call", 1, []Member{ms[0], ms[0]}); err == nil {
		t.Fatal("duplicate peer accepted")
	}
	if _, err := ComputeSAS("call", 1, []Member{{IDK: id(1), Acct: "a", Peer: "p"}}); err == nil {
		t.Fatal("missing nonce accepted")
	}
}

func TestEmojiTableIsDistinct(t *testing.T) {
	seen := map[string]bool{}
	for _, e := range EmojiTable {
		if e.Text == "" || e.Name == "" || seen[e.Text] || seen[e.Name] {
			t.Fatalf("emoji table entry %+v", e)
		}
		seen[e.Text], seen[e.Name] = true, true
	}
}
