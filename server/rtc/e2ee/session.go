package e2ee

import (
	"bytes"
	"crypto/ecdh"
	"crypto/ed25519"
	"crypto/mlkem"
	"crypto/rand"
	"io"
	"sort"
	"sync"
	"time"
)

const (
	ClockSkew        = 5 * time.Minute
	PresenceTimeout  = 60 * time.Second
	PresenceGrace    = 30 * time.Second
	KeepPresenceKeys = 2
)

type Event int

const (
	EventSame Event = iota
	EventJoined
	EventUpdated
)

type Config struct {
	Call     string
	Peer     string
	Acct     string
	Identity ed25519.PrivateKey
	Rand     io.Reader
	Now      func() time.Time
}

type presenceKey struct {
	epoch   uint32
	x       *ecdh.PrivateKey
	pq      *mlkem.DecapsulationKey768
	public  *Presence
	retired time.Time
}

type member struct {
	presence *Presence
	heard    time.Time
	opened   uint32
	commit   *Commit
	nonce    []byte
	early    *Reveal
}

type ownRound struct {
	id         []byte
	gen        uint32
	nonce      []byte
	commit     []byte
	reveal     []byte
	started    time.Time
	revealedAt time.Time
	code       *SAS
	counted    bool
	stale      bool
}

type Session struct {
	mu        sync.Mutex
	call      string
	peer      string
	acct      string
	id        ed25519.PrivateKey
	idk       ed25519.PublicKey
	rand      io.Reader
	now       func() time.Time
	keys      []*presenceKey
	sendEpoch uint32
	members   map[string]*member
	closed    bool
	gen       uint32
	round     *ownRound
	unfinish  int
	rounds    int
	verified  map[string]bool
}

func NewSession(c Config) (*Session, error) {
	if !validCall(c.Call) || !validPeer(c.Peer) || !validAcct(c.Acct) || len(c.Identity) != ed25519.PrivateKeySize {
		return nil, ErrInvalid
	}
	s := &Session{
		call: c.Call, peer: c.Peer, acct: c.Acct,
		id: c.Identity, idk: c.Identity.Public().(ed25519.PublicKey),
		rand: c.Rand, now: c.Now, members: map[string]*member{}, verified: map[string]bool{},
	}
	if s.rand == nil {
		s.rand = rand.Reader
	}
	if s.now == nil {
		s.now = time.Now
	}
	if _, err := s.rotateLocked(); err != nil {
		return nil, err
	}
	return s, nil
}

func (s *Session) Peer() string { return s.peer }

func (s *Session) IDK() ed25519.PublicKey { return s.idk }

func (s *Session) millis() int64 { return s.now().UnixMilli() }

func (s *Session) sender(epoch uint32) Sender {
	return Sender{Peer: s.peer, IDK: s.idk, Epoch: epoch}
}

func (s *Session) newPresenceKey(epoch uint32) (*presenceKey, error) {
	xs := make([]byte, 32)
	seed := make([]byte, mlkem.SeedSize)
	defer wipe(xs, seed)
	if _, err := io.ReadFull(s.rand, xs); err != nil {
		return nil, err
	}
	if _, err := io.ReadFull(s.rand, seed); err != nil {
		return nil, err
	}
	x, err := ecdh.X25519().NewPrivateKey(xs)
	if err != nil {
		return nil, err
	}
	pq, err := mlkem.NewDecapsulationKey768(seed)
	if err != nil {
		return nil, err
	}
	return &presenceKey{epoch: epoch, x: x, pq: pq}, nil
}

func (s *Session) signPresence(k *presenceKey) *Presence {
	p := &Presence{
		Call: s.call, Peer: s.peer, Acct: s.acct, IDK: s.idk, Epoch: k.epoch, TS: s.millis(),
		X: k.x.PublicKey().Bytes(), PQ: k.pq.EncapsulationKey().Bytes(),
	}
	p.Sig = ed25519.Sign(s.id, p.SignedMessage())
	k.public = p
	return p
}

func (s *Session) rotateLocked() ([]byte, error) {
	if s.closed {
		return nil, ErrClosed
	}
	epoch := uint32(1)
	if n := len(s.keys); n > 0 {
		if s.keys[n-1].epoch == maxEpoch {
			return nil, ErrExhausted
		}
		epoch = s.keys[n-1].epoch + 1
	}
	k, err := s.newPresenceKey(epoch)
	if err != nil {
		return nil, err
	}
	now := s.now()
	if n := len(s.keys); n > 0 {
		s.keys[n-1].retired = now
	}
	s.keys = append(s.keys, k)
	for len(s.keys) > KeepPresenceKeys {
		s.keys[0] = nil
		s.keys = s.keys[1:]
	}
	return s.signPresence(k).Marshal(), nil
}

func (s *Session) Rotate() ([]byte, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.rotateLocked()
}

func (s *Session) Presence() ([]byte, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.closed {
		return nil, ErrClosed
	}
	return s.signPresence(s.keys[len(s.keys)-1]).Marshal(), nil
}

func (s *Session) PresenceEpoch() uint32 {
	s.mu.Lock()
	defer s.mu.Unlock()
	if len(s.keys) == 0 {
		return 0
	}
	return s.keys[len(s.keys)-1].epoch
}

func (s *Session) fresh(ts int64) bool {
	now := s.millis()
	skew := ClockSkew.Milliseconds()
	return ts >= now-skew && ts <= now+skew
}

func (s *Session) AcceptPresence(b []byte) (Event, error) {
	p, err := ParsePresence(b)
	if err != nil {
		return EventSame, err
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.closed {
		return EventSame, ErrClosed
	}
	if p.Call != s.call {
		return EventSame, ErrCall
	}
	if p.Peer == s.peer {
		if bytes.Equal(p.IDK, s.idk) {
			return EventSame, ErrOwn
		}
		return EventSame, ErrPeerBound
	}
	if !s.fresh(p.TS) {
		return EventSame, ErrClock
	}
	m := s.members[p.Peer]
	if m != nil && !bytes.Equal(m.presence.IDK, p.IDK) {
		return EventSame, ErrPeerBound
	}
	if m == nil && len(s.members)+1 >= MaxParticipants {
		return EventSame, ErrFull
	}
	if err := p.Verify(); err != nil {
		return EventSame, err
	}
	if m == nil {
		s.members[p.Peer] = &member{presence: p, heard: s.now()}
		return EventJoined, nil
	}
	old := m.presence
	if p.Acct != old.Acct {
		return EventSame, ErrPeerBound
	}
	switch {
	case p.Epoch < old.Epoch:
		return EventSame, ErrStale
	case p.Epoch == old.Epoch:
		if !bytes.Equal(p.X, old.X) || !bytes.Equal(p.PQ, old.PQ) {
			return EventSame, ErrEquivocation
		}
		if p.TS < old.TS {
			return EventSame, ErrStale
		}
		m.presence = p
		m.heard = s.now()
		return EventSame, nil
	}
	m.presence = p
	m.heard = s.now()
	return EventUpdated, nil
}

func (s *Session) Leave(peer string) bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	if _, ok := s.members[peer]; !ok {
		return false
	}
	delete(s.members, peer)
	return true
}

func (s *Session) Expire() []string {
	s.mu.Lock()
	defer s.mu.Unlock()
	now := s.now()
	var gone []string
	for peer, m := range s.members {
		if now.Sub(m.heard) > PresenceTimeout {
			gone = append(gone, peer)
			delete(s.members, peer)
		}
	}
	sort.Strings(gone)
	return gone
}

func (s *Session) Prune() {
	s.mu.Lock()
	defer s.mu.Unlock()
	now := s.now()
	kept := s.keys[:0]
	for _, k := range s.keys {
		if !k.retired.IsZero() && now.Sub(k.retired) > PresenceGrace {
			continue
		}
		kept = append(kept, k)
	}
	for i := len(kept); i < len(s.keys); i++ {
		s.keys[i] = nil
	}
	s.keys = kept
}

func (s *Session) Peers() []string {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.peersLocked()
}

func (s *Session) peersLocked() []string {
	peers := make([]string, 0, len(s.members))
	for peer := range s.members {
		peers = append(peers, peer)
	}
	sort.Strings(peers)
	return peers
}

func (s *Session) Member(peer string) (*Presence, bool) {
	s.mu.Lock()
	defer s.mu.Unlock()
	m, ok := s.members[peer]
	if !ok {
		return nil, false
	}
	return m.presence, true
}

func (s *Session) Rekey() ([]byte, FrameKey, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.closed {
		return nil, FrameKey{}, ErrClosed
	}
	if s.sendEpoch == maxEpoch {
		return nil, FrameKey{}, ErrExhausted
	}
	epoch := s.sendEpoch + 1
	secret := make([]byte, SecretSize)
	defer wipe(secret)
	if _, err := io.ReadFull(s.rand, secret); err != nil {
		return nil, FrameKey{}, err
	}
	authSeed := make([]byte, ed25519.SeedSize)
	if _, err := io.ReadFull(s.rand, authSeed); err != nil {
		return nil, FrameKey{}, err
	}
	auth := ed25519.NewKeyFromSeed(authSeed).Public().(ed25519.PublicKey)
	var out []byte
	if peers := s.peersLocked(); len(peers) > 0 {
		if len(peers) > MaxRecipients {
			return nil, FrameKey{}, ErrFull
		}
		xs := make([]byte, 32)
		if _, err := io.ReadFull(s.rand, xs); err != nil {
			return nil, FrameKey{}, err
		}
		eph, err := ecdh.X25519().NewPrivateKey(xs)
		wipe(xs)
		if err != nil {
			return nil, FrameKey{}, err
		}
		m := &KeyMessage{Call: s.call, Peer: s.peer, IDK: s.idk, Epoch: epoch, TS: s.millis(), Eph: eph.PublicKey().Bytes(), Auth: auth}
		for _, peer := range peers {
			w, trace, err := wrapSecret(s.call, s.sender(epoch), s.members[peer].presence, eph, secret)
			if err != nil {
				return nil, FrameKey{}, err
			}
			wipe(trace.SS1, trace.SS2, trace.KEK)
			m.Keys = append(m.Keys, w)
		}
		m.Sig = ed25519.Sign(s.id, m.SignedMessage())
		out = m.Marshal()
	}
	key, err := DeriveFrameKey(s.call, s.sender(epoch), secret)
	if err != nil {
		return nil, FrameKey{}, err
	}
	key.Auth = auth
	key.AuthSeed = authSeed
	s.sendEpoch = epoch
	return out, key, nil
}

func (s *Session) OpenKeys(b []byte) (FrameKey, error) {
	m, err := ParseKeyMessage(b)
	if err != nil {
		return FrameKey{}, err
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.closed {
		return FrameKey{}, ErrClosed
	}
	if m.Call != s.call {
		return FrameKey{}, ErrCall
	}
	if m.Peer == s.peer {
		return FrameKey{}, ErrOwn
	}
	mem := s.members[m.Peer]
	if mem == nil {
		return FrameKey{}, ErrUnknownPeer
	}
	if !bytes.Equal(mem.presence.IDK, m.IDK) {
		return FrameKey{}, ErrPeerBound
	}
	if !s.fresh(m.TS) {
		return FrameKey{}, ErrClock
	}
	if err := m.Verify(); err != nil {
		return FrameKey{}, err
	}
	if m.Epoch < mem.opened {
		return FrameKey{}, ErrStale
	}
	if m.Epoch == mem.opened {
		return FrameKey{}, ErrDuplicate
	}
	w, ok := m.KeyFor(s.peer)
	if !ok {
		return FrameKey{}, ErrNotForMe
	}
	var k *presenceKey
	for _, candidate := range s.keys {
		if candidate.epoch == w.Epoch {
			k = candidate
		}
	}
	if k == nil {
		return FrameKey{}, ErrRecipientEpoch
	}
	sender := Sender{Peer: m.Peer, IDK: m.IDK, Epoch: m.Epoch}
	secret, err := unwrapSecret(s.call, sender, m.Eph, w, k.public, k.x, k.pq)
	if err != nil {
		return FrameKey{}, err
	}
	defer wipe(secret)
	key, err := DeriveFrameKey(s.call, sender, secret)
	if err != nil {
		return FrameKey{}, err
	}
	key.Auth = append(ed25519.PublicKey(nil), m.Auth...)
	mem.opened = m.Epoch
	return key, nil
}

func (s *Session) Close() {
	s.mu.Lock()
	defer s.mu.Unlock()
	for i := range s.keys {
		s.keys[i] = nil
	}
	s.keys = nil
	s.members = map[string]*member{}
	if s.round != nil {
		wipe(s.round.nonce)
		s.round = nil
	}
	s.closed = true
}
