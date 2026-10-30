package e2ee

import (
	"bytes"
	"crypto/ed25519"
	"fmt"
	"sync"
	"testing"
	"time"
)

type clock struct {
	mu sync.Mutex
	t  time.Time
}

func (c *clock) now() time.Time {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.t
}

func (c *clock) add(d time.Duration) {
	c.mu.Lock()
	c.t = c.t.Add(d)
	c.mu.Unlock()
}

func newClock() *clock { return &clock{t: time.UnixMilli(1_790_000_000_000)} }

func newParticipant(t testing.TB, c *clock, call, name string) *Session {
	t.Helper()
	_, key, _ := ed25519.GenerateKey(nil)
	peer := fmt.Sprintf("%032x", []byte(name))
	if len(peer) > 32 {
		peer = peer[:32]
	}
	s, err := NewSession(Config{Call: call, Peer: peer, Acct: name, Identity: key, Now: c.now})
	if err != nil {
		t.Fatal(err)
	}
	return s
}

func mustPresence(t testing.TB, s *Session) []byte {
	t.Helper()
	b, err := s.Presence()
	if err != nil {
		t.Fatal(err)
	}
	return b
}

func meet(t testing.TB, all ...*Session) {
	t.Helper()
	for _, a := range all {
		for _, b := range all {
			if a != b {
				if _, err := a.AcceptPresence(mustPresence(t, b)); err != nil {
					t.Fatalf("accept presence: %v", err)
				}
			}
		}
	}
}

func distribute(t testing.TB, from *Session, to ...*Session) FrameKey {
	t.Helper()
	msg, own, err := from.Rekey()
	if err != nil {
		t.Fatal(err)
	}
	for _, r := range to {
		got, err := r.OpenKeys(msg)
		if err != nil {
			t.Fatalf("open keys: %v", err)
		}
		if !bytes.Equal(got.Key, own.Key) || got.KeyID != own.KeyID || got.Peer != from.Peer() || !bytes.Equal(got.Auth, own.Auth) || got.AuthSeed != nil {
			t.Fatal("receiver derived a different frame key")
		}
	}
	return own
}

func TestThreeWayCallAgreesOnKeys(t *testing.T) {
	c := newClock()
	a, b, d := newParticipant(t, c, "call-1", "alice"), newParticipant(t, c, "call-1", "bob"), newParticipant(t, c, "call-1", "dave")
	meet(t, a, b, d)
	ka := distribute(t, a, b, d)
	kb := distribute(t, b, a, d)
	kd := distribute(t, d, a, b)
	if bytes.Equal(ka.Key, kb.Key) || bytes.Equal(kb.Key, kd.Key) || bytes.Equal(ka.Auth, kb.Auth) {
		t.Fatal("two senders share a frame key or auth key")
	}
	if len(ka.AuthSeed) != ed25519.SeedSize || len(ka.Auth) != ed25519.PublicKeySize {
		t.Fatal("send key has no media auth key")
	}
}

func TestLeaverCannotOpenTheNextEpoch(t *testing.T) {
	c := newClock()
	a, b, d := newParticipant(t, c, "call-1", "alice"), newParticipant(t, c, "call-1", "bob"), newParticipant(t, c, "call-1", "dave")
	meet(t, a, b, d)
	distribute(t, a, b, d)
	a.Leave(d.Peer())
	msg, _, err := a.Rekey()
	if err != nil {
		t.Fatal(err)
	}
	if _, err := d.OpenKeys(msg); ErrorCode(err) != "not-for-me" {
		t.Fatalf("leaver: %v", err)
	}
	if _, err := b.OpenKeys(msg); err != nil {
		t.Fatal(err)
	}
}

func TestJoinerCannotOpenEarlierEpochs(t *testing.T) {
	c := newClock()
	a, b := newParticipant(t, c, "call-1", "alice"), newParticipant(t, c, "call-1", "bob")
	meet(t, a, b)
	old, _, _ := a.Rekey()
	b.OpenKeys(old)
	d := newParticipant(t, c, "call-1", "dave")
	meet(t, a, b, d)
	if _, err := d.OpenKeys(old); ErrorCode(err) != "not-for-me" {
		t.Fatalf("joiner opened an old epoch: %v", err)
	}
	distribute(t, a, b, d)
}

func TestPresenceRotationKeepsPreviousKeyForTheGrace(t *testing.T) {
	c := newClock()
	a, b := newParticipant(t, c, "call-1", "alice"), newParticipant(t, c, "call-1", "bob")
	meet(t, a, b)
	late, _, _ := a.Rekey()
	rotated, err := b.Rotate()
	if err != nil {
		t.Fatal(err)
	}
	if ev, err := a.AcceptPresence(rotated); err != nil || ev != EventUpdated {
		t.Fatalf("rotation: %v %v", ev, err)
	}
	if _, err := b.OpenKeys(late); err != nil {
		t.Fatalf("late key message inside the grace: %v", err)
	}
	later, _, _ := a.Rekey()
	c.add(PresenceGrace + time.Second)
	b.Prune()
	if _, err := b.OpenKeys(later); err != nil {
		t.Fatalf("message to the new epoch: %v", err)
	}
	stale := append([]byte(nil), mustPresence(t, b)...)
	b.Rotate()
	b.Rotate()
	a3 := newParticipant(t, c, "call-1", "alice3")
	a3.AcceptPresence(stale)
	b.AcceptPresence(mustPresence(t, a3))
	msg, _, _ := a3.Rekey()
	if _, err := b.OpenKeys(msg); ErrorCode(err) != "recipient-epoch" {
		t.Fatalf("wrap to a dropped presence key: %v", err)
	}
}

func TestPostCompromiseStateCannotOpenNewEpochs(t *testing.T) {
	c := newClock()
	a, b := newParticipant(t, c, "call-1", "alice"), newParticipant(t, c, "call-1", "bob")
	meet(t, a, b)
	b.mu.Lock()
	stolen := b.keys[len(b.keys)-1]
	b.mu.Unlock()
	b.Rotate()
	b.Rotate()
	meet(t, a, b)
	msg, _, err := a.Rekey()
	if err != nil {
		t.Fatal(err)
	}
	m, _ := ParseKeyMessage(msg)
	w, _ := m.KeyFor(b.Peer())
	if w.Epoch == stolen.epoch {
		t.Fatal("sender wrapped to the stolen epoch")
	}
	current, _ := a.Member(b.Peer())
	if _, err := unwrapSecret("call-1", Sender{Peer: a.Peer(), IDK: a.IDK(), Epoch: m.Epoch}, m.Eph, w, current, stolen.x, stolen.pq); err == nil {
		t.Fatal("stolen presence key opened a later epoch")
	}
	if _, err := b.OpenKeys(msg); err != nil {
		t.Fatal(err)
	}
}

func TestPresenceReplayAndBinding(t *testing.T) {
	c := newClock()
	a, b, m := newParticipant(t, c, "call-1", "alice"), newParticipant(t, c, "call-1", "bob"), newParticipant(t, c, "call-1", "mallory")
	first := mustPresence(t, a)
	if ev, err := b.AcceptPresence(first); err != nil || ev != EventJoined {
		t.Fatal(err)
	}
	c.add(20 * time.Second)
	if ev, err := b.AcceptPresence(mustPresence(t, a)); err != nil || ev != EventSame {
		t.Fatalf("heartbeat: %v %v", ev, err)
	}
	if _, err := b.AcceptPresence(first); ErrorCode(err) != "stale" {
		t.Fatalf("replayed heartbeat: %v", err)
	}
	newer, _ := a.Rotate()
	b.AcceptPresence(newer)
	if _, err := b.AcceptPresence(first); ErrorCode(err) != "stale" {
		t.Fatalf("replayed old epoch: %v", err)
	}
	p, _ := ParsePresence(mustPresence(t, m))
	p.Peer = a.Peer()
	if _, err := b.AcceptPresence(resignPresence(p, m.id)); ErrorCode(err) != "peer-bound" {
		t.Fatalf("stolen peer id: %v", err)
	}
	p, _ = ParsePresence(newer)
	p.Acct = "bob"
	if _, err := b.AcceptPresence(resignPresence(p, a.id)); ErrorCode(err) != "peer-bound" {
		t.Fatalf("renamed peer: %v", err)
	}
	if _, err := b.AcceptPresence(mustPresence(t, b)); ErrorCode(err) != "own" {
		t.Fatalf("own presence: %v", err)
	}
	c.add(6 * time.Minute)
	if _, err := newParticipant(t, c, "call-1", "zed").AcceptPresence(newer); ErrorCode(err) != "clock" {
		t.Fatalf("old presence in a new session: %v", err)
	}
	if _, err := newParticipant(t, c, "call-2", "zed").AcceptPresence(mustPresence(t, a)); ErrorCode(err) != "call" {
		t.Fatalf("presence from another call: %v", err)
	}
}

func TestPresenceExpiresAndReleasesItsPeer(t *testing.T) {
	c := newClock()
	a, b := newParticipant(t, c, "call-1", "alice"), newParticipant(t, c, "call-1", "bob")
	meet(t, a, b)
	c.add(PresenceTimeout + time.Second)
	if gone := b.Expire(); len(gone) != 1 || gone[0] != a.Peer() {
		t.Fatalf("expire: %v", gone)
	}
	if len(b.Peers()) != 0 {
		t.Fatal("expired peer still present")
	}
}

func TestKeyMessageReplayAndOrdering(t *testing.T) {
	c := newClock()
	a, b := newParticipant(t, c, "call-1", "alice"), newParticipant(t, c, "call-1", "bob")
	meet(t, a, b)
	one, _, _ := a.Rekey()
	two, _, _ := a.Rekey()
	if _, err := b.OpenKeys(two); err != nil {
		t.Fatal(err)
	}
	if _, err := b.OpenKeys(one); ErrorCode(err) != "stale" {
		t.Fatalf("older epoch after newer: %v", err)
	}
	if _, err := b.OpenKeys(two); ErrorCode(err) != "duplicate" {
		t.Fatalf("replayed epoch: %v", err)
	}
	if _, err := a.OpenKeys(two); ErrorCode(err) != "own" {
		t.Fatalf("own keys: %v", err)
	}
}

func TestFullCallRejectsSeventeenth(t *testing.T) {
	c := newClock()
	host := newParticipant(t, c, "call-1", "host")
	for i := 0; i < MaxParticipants-1; i++ {
		p := newParticipant(t, c, "call-1", fmt.Sprintf("p%02d", i))
		if _, err := host.AcceptPresence(mustPresence(t, p)); err != nil {
			t.Fatalf("participant %d: %v", i, err)
		}
	}
	extra := newParticipant(t, c, "call-1", "extra")
	if _, err := host.AcceptPresence(mustPresence(t, extra)); ErrorCode(err) != "full" {
		t.Fatalf("seventeenth: %v", err)
	}
	msg, _, err := host.Rekey()
	if err != nil {
		t.Fatal(err)
	}
	m, err := ParseKeyMessage(msg)
	if err != nil || len(m.Keys) != MaxParticipants-1 {
		t.Fatalf("full call key message: %v", err)
	}
}

func TestLoneParticipantStillGetsASendKey(t *testing.T) {
	c := newClock()
	a := newParticipant(t, c, "call-1", "alice")
	msg, key, err := a.Rekey()
	if err != nil || msg != nil || len(key.Key) != FrameKeySize || key.Epoch != 1 || len(key.AuthSeed) != ed25519.SeedSize {
		t.Fatalf("lone rekey: %v %v", msg, err)
	}
}

func TestClosedSessionRefusesWork(t *testing.T) {
	c := newClock()
	a := newParticipant(t, c, "call-1", "alice")
	a.Close()
	if _, err := a.Presence(); ErrorCode(err) != "closed" {
		t.Fatal(err)
	}
	if _, _, err := a.Rekey(); ErrorCode(err) != "closed" {
		t.Fatal(err)
	}
}

func TestSessionIsSafeForConcurrentUse(t *testing.T) {
	c := newClock()
	a, b := newParticipant(t, c, "call-1", "alice"), newParticipant(t, c, "call-1", "bob")
	meet(t, a, b)
	var wg sync.WaitGroup
	for i := 0; i < 8; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			msg, _, err := a.Rekey()
			if err == nil {
				b.OpenKeys(msg)
			}
			b.AcceptPresence(mustPresence(t, a))
			if cm, err := a.Commit(); err == nil {
				b.AcceptCommit(cm)
			}
			b.Verification()
			a.Prune()
		}()
	}
	wg.Wait()
}
