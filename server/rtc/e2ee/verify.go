package e2ee

import (
	"bytes"
	"crypto/ed25519"
	"crypto/subtle"
	"io"
	"sort"
)

type VerifyStatus struct {
	Round      []byte
	Gen        uint32
	NeedCommit bool
	Code       *SAS
	Waiting    []string
	Late       bool
	Rounds     int
	Unfinished int
	Verified   bool
	Newcomers  []string
}

func (s *Session) rosterLocked() []Member {
	roster := make([]Member, 0, len(s.members)+1)
	self := Member{IDK: s.idk, Acct: s.acct, Peer: s.peer}
	if s.round != nil {
		self.Nonce = s.round.nonce
	}
	roster = append(roster, self)
	for _, peer := range s.peersLocked() {
		m := s.members[peer]
		roster = append(roster, Member{IDK: m.presence.IDK, Acct: m.presence.Acct, Peer: peer, Nonce: m.nonce})
	}
	return roster
}

func (s *Session) liveRoundLocked() *ownRound {
	r := s.round
	if r == nil || r.stale {
		return nil
	}
	id, err := RoundID(s.call, r.gen, s.rosterLocked())
	if err != nil || !bytes.Equal(id, r.id) {
		return nil
	}
	return r
}

func (s *Session) retireRoundLocked() {
	r := s.round
	if r == nil {
		return
	}
	if r.reveal != nil && r.code == nil && !r.counted {
		s.unfinish++
	}
	wipe(r.nonce)
	s.round = nil
}

func (s *Session) Commit() ([]byte, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.closed {
		return nil, ErrClosed
	}
	if r := s.liveRoundLocked(); r != nil {
		return r.commit, nil
	}
	if s.gen == maxEpoch {
		return nil, ErrExhausted
	}
	roster := s.rosterLocked()
	gen := s.gen + 1
	for _, m := range s.members {
		if c := m.commit; c != nil && c.Gen > gen {
			if id, err := RoundID(s.call, c.Gen, roster); err == nil && bytes.Equal(id, c.Round) {
				gen = c.Gen
			}
		}
	}
	id, err := RoundID(s.call, gen, roster)
	if err != nil {
		return nil, err
	}
	nonce := make([]byte, NonceSize)
	if _, err := io.ReadFull(s.rand, nonce); err != nil {
		return nil, err
	}
	s.retireRoundLocked()
	s.gen = gen
	c := &Commit{Call: s.call, Peer: s.peer, IDK: s.idk, Round: id, Gen: gen, TS: s.millis()}
	c.C = commitHash(c.Call, c.Peer, c.IDK, c.Round, c.Gen, nonce)
	c.Sig = ed25519.Sign(s.id, c.SignedMessage())
	s.round = &ownRound{id: id, gen: gen, nonce: nonce, commit: c.Marshal(), started: s.now()}
	return s.round.commit, nil
}

func (s *Session) AcceptCommit(b []byte) error {
	c, err := ParseCommit(b)
	if err != nil {
		return err
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.closed {
		return ErrClosed
	}
	if c.Call != s.call {
		return ErrCall
	}
	if c.Peer == s.peer {
		if bytes.Equal(c.IDK, s.idk) {
			return ErrOwn
		}
		return ErrPeerBound
	}
	m := s.members[c.Peer]
	if m == nil {
		return ErrUnknownPeer
	}
	if !bytes.Equal(m.presence.IDK, c.IDK) {
		return ErrPeerBound
	}
	if !s.fresh(c.TS) {
		return ErrClock
	}
	if uint64(c.Gen) > uint64(s.gen)+MaxGenJump {
		return ErrInvalid
	}
	if err := c.Verify(); err != nil {
		return err
	}
	if old := m.commit; old != nil {
		if c.Gen < old.Gen {
			return ErrStale
		}
		if c.Gen == old.Gen {
			if bytes.Equal(c.Round, old.Round) && bytes.Equal(c.C, old.C) {
				return ErrDuplicate
			}
			return ErrEquivocation
		}
	}
	m.commit = c
	m.nonce = nil
	if e := m.early; e != nil && e.Gen <= c.Gen {
		m.early = nil
		if e.Gen == c.Gen && bytes.Equal(e.Round, c.Round) && c.Opens(e.Nonce) {
			m.nonce = e.Nonce
		}
	}
	if r := s.round; r != nil && c.Gen > r.gen {
		if id, err := RoundID(s.call, c.Gen, s.rosterLocked()); err == nil && bytes.Equal(id, c.Round) {
			r.stale = true
		}
	}
	return nil
}

func (s *Session) Reveal() ([]byte, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.closed {
		return nil, ErrClosed
	}
	r := s.liveRoundLocked()
	if r == nil {
		return nil, ErrWaiting
	}
	if r.reveal != nil {
		return r.reveal, nil
	}
	for _, m := range s.members {
		if m.commit == nil || m.commit.Gen != r.gen || !bytes.Equal(m.commit.Round, r.id) {
			return nil, ErrWaiting
		}
	}
	out := &Reveal{Call: s.call, Peer: s.peer, Round: r.id, Gen: r.gen, Nonce: r.nonce}
	r.reveal = out.Marshal()
	r.revealedAt = s.now()
	s.rounds++
	return r.reveal, nil
}

func (s *Session) AcceptReveal(b []byte) error {
	rv, err := ParseReveal(b)
	if err != nil {
		return err
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.closed {
		return ErrClosed
	}
	if rv.Call != s.call {
		return ErrCall
	}
	if rv.Peer == s.peer {
		return ErrOwn
	}
	m := s.members[rv.Peer]
	if m == nil {
		return ErrUnknownPeer
	}
	if m.commit == nil || rv.Gen > m.commit.Gen {
		if m.early == nil || rv.Gen >= m.early.Gen {
			m.early = rv
		}
		return ErrWaiting
	}
	if rv.Gen < m.commit.Gen {
		return ErrStale
	}
	if !bytes.Equal(rv.Round, m.commit.Round) || !m.commit.Opens(rv.Nonce) {
		return ErrCommitment
	}
	if m.nonce != nil {
		return ErrDuplicate
	}
	m.nonce = rv.Nonce
	return nil
}

func (s *Session) Verification() VerifyStatus {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.verificationLocked()
}

func (s *Session) verificationLocked() VerifyStatus {
	var st VerifyStatus
	if s.closed {
		return st
	}
	roster := s.rosterLocked()
	r := s.liveRoundLocked()
	st.NeedCommit = r == nil
	if r != nil {
		st.Round = r.id
		st.Gen = r.gen
		for _, peer := range s.peersLocked() {
			m := s.members[peer]
			if m.commit == nil || m.commit.Gen != r.gen || !bytes.Equal(m.commit.Round, r.id) || (r.reveal != nil && m.nonce == nil) {
				st.Waiting = append(st.Waiting, peer)
			}
		}
		if r.reveal != nil && len(st.Waiting) == 0 && r.code == nil {
			if code, err := ComputeSAS(s.call, r.gen, roster); err == nil {
				r.code = &code
			}
		}
		st.Code = r.code
		now := s.now()
		st.Late = st.Code == nil && now.Sub(r.started) > RevealTimeout
		if r.reveal != nil && r.code == nil && !r.counted && now.Sub(r.revealedAt) > RoundTimeout {
			r.counted = true
			s.unfinish++
		}
	}
	st.Rounds = s.rounds
	st.Unfinished = s.unfinish
	if len(s.verified) > 0 {
		for _, m := range roster {
			if !s.verified[memberKey(m.IDK, m.Acct)] {
				st.Newcomers = append(st.Newcomers, m.Peer)
			}
		}
		sort.Strings(st.Newcomers)
		st.Verified = len(st.Newcomers) == 0
	}
	return st
}

func (s *Session) markLocked() {
	for _, m := range s.rosterLocked() {
		s.verified[memberKey(m.IDK, m.Acct)] = true
	}
}

func (s *Session) MarkVerified(round []byte) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	st := s.verificationLocked()
	if st.Code == nil || !bytes.Equal(round, st.Code.Round) {
		return ErrWaiting
	}
	s.markLocked()
	return nil
}

func (s *Session) CheckQR(text string) (bool, error) {
	t, err := ParseQR(text)
	if err != nil {
		return false, err
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	st := s.verificationLocked()
	if st.Code == nil {
		return false, ErrWaiting
	}
	if subtle.ConstantTimeCompare(t, st.Code.Transcript) != 1 {
		return false, nil
	}
	s.markLocked()
	return true, nil
}

func (s *Session) Unverify() {
	s.mu.Lock()
	defer s.mu.Unlock()
	clear(s.verified)
}
