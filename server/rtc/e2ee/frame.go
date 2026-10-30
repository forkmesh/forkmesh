package e2ee

import (
	"bytes"
	"crypto/aes"
	"crypto/cipher"
	"crypto/ed25519"
	"crypto/sha256"
	"encoding/binary"
	"hash"
	"math"
	"sync"
	"time"
)

const (
	FrameTrailer  = 9
	FrameTag      = 16
	ReplayWindow  = 1024
	KeepFrameKeys = 3
)

type FrameTrailerInfo struct {
	Counter uint64
	KeyID   byte
}

func ParseFrameTrailer(frame []byte) (FrameTrailerInfo, error) {
	if len(frame) < FrameTrailer+FrameTag+1 {
		return FrameTrailerInfo{}, ErrInvalid
	}
	end := len(frame) - FrameTrailer
	return FrameTrailerInfo{
		Counter: binary.BigEndian.Uint64(frame[end : end+8]),
		KeyID:   frame[len(frame)-1],
	}, nil
}

func frameNonce(counter uint64) []byte {
	nonce := make([]byte, 12)
	binary.BigEndian.PutUint64(nonce[4:], counter)
	return nonce
}

func newAEAD(key []byte) (cipher.AEAD, error) {
	if len(key) != FrameKeySize {
		return nil, ErrInvalid
	}
	block, err := aes.NewCipher(key)
	if err != nil {
		return nil, err
	}
	return cipher.NewGCM(block)
}

func authKeyFor(k FrameKey) (ed25519.PrivateKey, error) {
	if len(k.AuthSeed) != ed25519.SeedSize || !validCall(k.Call) || !validPeer(k.Peer) || k.Epoch == 0 || k.KeyID != byte(k.Epoch) {
		return nil, ErrInvalid
	}
	priv := ed25519.NewKeyFromSeed(k.AuthSeed)
	if k.Auth != nil && !bytes.Equal(priv.Public().(ed25519.PublicKey), k.Auth) {
		return nil, ErrInvalid
	}
	return priv, nil
}

type FrameSender struct {
	mu      sync.Mutex
	aead    cipher.AEAD
	raw     []byte
	keyID   byte
	counter uint64
	call    string
	peer    string
	epoch   uint32
	auth    ed25519.PrivateKey
	win     AuthMessage
	pending [][]byte
	hash    hash.Hash
}

func (s *FrameSender) SetKey(k FrameKey) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.aead != nil && bytes.Equal(s.raw, k.Key) {
		return nil
	}
	aead, err := newAEAD(k.Key)
	if err != nil {
		return err
	}
	priv, err := authKeyFor(k)
	if err != nil {
		return err
	}
	s.closeLocked()
	wipe(s.raw, s.auth)
	s.aead = aead
	s.raw = append([]byte(nil), k.Key...)
	s.keyID = k.KeyID
	s.counter = 0
	s.call, s.peer, s.epoch, s.auth = k.Call, k.Peer, k.Epoch, priv
	return nil
}

func (s *FrameSender) closeLocked() {
	if len(s.win.Digests) == 0 {
		return
	}
	m := &AuthMessage{KeyID: s.keyID, Epoch: s.epoch, First: s.win.First, Digests: s.win.Digests}
	m.Sign(s.call, s.peer, s.auth)
	s.pending = append(s.pending, m.Marshal())
	s.win = AuthMessage{}
}

func (s *FrameSender) Seal(frame []byte, clear int) ([]byte, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.aead == nil {
		return nil, ErrInvalid
	}
	if s.counter == math.MaxUint64 {
		return nil, ErrExhausted
	}
	if clear < 0 {
		clear = 0
	}
	clear = min(clear, len(frame))
	s.counter++
	out := make([]byte, 0, len(frame)+FrameTag+FrameTrailer)
	out = append(out, frame[:clear]...)
	out = s.aead.Seal(out, frameNonce(s.counter), frame[clear:], frame[:clear])
	out = binary.BigEndian.AppendUint64(out, s.counter)
	out = append(out, s.keyID)
	if s.hash == nil {
		s.hash = sha256.New()
	}
	s.hash.Reset()
	s.hash.Write([]byte(digestLabel))
	s.hash.Write(out)
	var sum [sha256.Size]byte
	if len(s.win.Digests) == 0 {
		s.win.First = s.counter
	}
	s.win.Digests = append(s.win.Digests, s.hash.Sum(sum[:0])[:AuthDigestSize]...)
	if s.win.Count() == MaxAuthFrames {
		s.closeLocked()
	}
	return out, nil
}

func (s *FrameSender) Flush() [][]byte {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.closeLocked()
	out := s.pending
	s.pending = nil
	return out
}

type replayEntry struct {
	aead    cipher.AEAD
	raw     []byte
	epoch   uint32
	call    string
	auth    ed25519.PublicKey
	started bool
	top     uint64
	seen    [ReplayWindow / 64]uint64
	pending map[uint64]pendingFrame
	windows []authWindow
}

func (e *replayEntry) fresh(v uint64) bool {
	if !e.started || v > e.top {
		return true
	}
	age := e.top - v
	return age < ReplayWindow && e.seen[age/64]&(1<<(age%64)) == 0
}

func (e *replayEntry) shift(n uint64) {
	if n >= ReplayWindow {
		e.seen = [ReplayWindow / 64]uint64{}
		return
	}
	words, bits := int(n/64), n%64
	for i := len(e.seen) - 1; i >= 0; i-- {
		var v uint64
		if j := i - words; j >= 0 {
			v = e.seen[j] << bits
			if bits > 0 && j-1 >= 0 {
				v |= e.seen[j-1] >> (64 - bits)
			}
		}
		e.seen[i] = v
	}
}

func (e *replayEntry) mark(v uint64) {
	if !e.started {
		e.started = true
		e.top = v
		e.seen = [ReplayWindow / 64]uint64{}
		e.seen[0] = 1
		return
	}
	if v > e.top {
		e.shift(v - e.top)
		e.top = v
		e.seen[0] |= 1
		return
	}
	age := e.top - v
	e.seen[age/64] |= 1 << (age % 64)
}

type peerKeys struct {
	order  []byte
	keys   map[byte]*replayEntry
	newest uint32
	auth   peerAuth
}

type FrameReceiver struct {
	mu     sync.Mutex
	peers  map[string]*peerKeys
	hash   hash.Hash
	Now    func() time.Time
	OnAuth func(peer string, state AuthState)
}

func (r *FrameReceiver) now() time.Time {
	if r.Now != nil {
		return r.Now()
	}
	return time.Now()
}

func (r *FrameReceiver) notifyLocked(peer string, p *peerKeys, now time.Time) func() {
	st := p.auth.evaluate(now)
	if st == p.auth.state {
		return nil
	}
	p.auth.state = st
	if cb := r.OnAuth; cb != nil {
		return func() { cb(peer, st) }
	}
	return nil
}

func (r *FrameReceiver) sweepLocked(p *peerKeys, now time.Time, force bool) {
	if !force && now.Sub(p.auth.lastSweep) < sweepEvery {
		return
	}
	p.auth.lastSweep = now
	for _, e := range p.keys {
		e.sweep(&p.auth, now, false)
	}
}

func (r *FrameReceiver) SetKey(k FrameKey) error {
	r.mu.Lock()
	defer r.mu.Unlock()
	if r.peers == nil {
		r.peers = map[string]*peerKeys{}
	}
	if len(k.Auth) != ed25519.PublicKeySize || !validCall(k.Call) || !validPeer(k.Peer) || k.Epoch == 0 || k.KeyID != byte(k.Epoch) {
		return ErrInvalid
	}
	p := r.peers[k.Peer]
	if p == nil {
		p = &peerKeys{keys: map[byte]*replayEntry{}}
		r.peers[k.Peer] = p
	}
	if known := p.keys[k.KeyID]; known != nil && bytes.Equal(known.raw, k.Key) {
		return nil
	}
	if len(p.order) > 0 && k.Epoch <= p.newest {
		return ErrStale
	}
	aead, err := newAEAD(k.Key)
	if err != nil {
		return err
	}
	now := r.now()
	drop := func(id byte) {
		e := p.keys[id]
		e.sweep(&p.auth, now, true)
		wipe(e.raw)
		delete(p.keys, id)
	}
	if old := p.keys[k.KeyID]; old != nil {
		drop(k.KeyID)
		for i, id := range p.order {
			if id == k.KeyID {
				p.order = append(p.order[:i], p.order[i+1:]...)
				break
			}
		}
	}
	p.keys[k.KeyID] = &replayEntry{aead: aead, raw: append([]byte(nil), k.Key...), epoch: k.Epoch, call: k.Call, auth: append(ed25519.PublicKey(nil), k.Auth...)}
	p.order = append(p.order, k.KeyID)
	p.newest = k.Epoch
	for len(p.order) > KeepFrameKeys {
		drop(p.order[0])
		p.order = p.order[1:]
	}
	return nil
}

func (r *FrameReceiver) Forget(peer string) {
	r.mu.Lock()
	defer r.mu.Unlock()
	if p := r.peers[peer]; p != nil {
		for _, e := range p.keys {
			wipe(e.raw)
		}
	}
	delete(r.peers, peer)
}

func (r *FrameReceiver) digestLocked(frame []byte) []byte {
	if r.hash == nil {
		r.hash = sha256.New()
	}
	r.hash.Reset()
	r.hash.Write([]byte(digestLabel))
	r.hash.Write(frame)
	return r.hash.Sum(nil)[:AuthDigestSize]
}

func (r *FrameReceiver) Open(peer string, frame []byte, clear int) ([]byte, error) {
	t, err := ParseFrameTrailer(frame)
	if err != nil {
		return nil, err
	}
	now := r.now()
	r.mu.Lock()
	p := r.peers[peer]
	if p == nil {
		r.mu.Unlock()
		return nil, ErrUnknownPeer
	}
	e := p.keys[t.KeyID]
	if e == nil {
		r.mu.Unlock()
		return nil, ErrUnknownPeer
	}
	if !e.fresh(t.Counter) {
		r.mu.Unlock()
		return nil, ErrStale
	}
	body := frame[:len(frame)-FrameTrailer]
	if clear < 0 {
		clear = 0
	}
	clear = min(clear, len(body)-FrameTag)
	plain, err := e.aead.Open(nil, frameNonce(t.Counter), body[clear:], body[:clear])
	if err != nil {
		r.mu.Unlock()
		return nil, ErrInvalid
	}
	e.mark(t.Counter)
	v := e.judge(t.Counter, r.digestLocked(frame), now)
	p.auth.record(v, now)
	e.trimPending(&p.auth, now)
	r.sweepLocked(p, now, false)
	refused := p.auth.drop(now)
	if v == verdictForged {
		refused = ErrForged
	}
	fire := r.notifyLocked(peer, p, now)
	r.mu.Unlock()
	if fire != nil {
		fire()
	}
	if refused != nil {
		wipe(plain)
		return nil, refused
	}
	out := make([]byte, 0, clear+len(plain))
	out = append(out, body[:clear]...)
	return append(out, plain...), nil
}

func (r *FrameReceiver) AcceptAuth(peer string, b []byte) error {
	m, err := ParseAuthMessage(b)
	if err != nil {
		return err
	}
	now := r.now()
	r.mu.Lock()
	p := r.peers[peer]
	if p == nil {
		r.mu.Unlock()
		return ErrUnknownPeer
	}
	e := p.keys[m.KeyID]
	if e == nil {
		r.mu.Unlock()
		return ErrUnknownPeer
	}
	if e.epoch != m.Epoch {
		r.mu.Unlock()
		return ErrStale
	}
	if err := m.Verify(e.call, peer, e.auth); err != nil {
		r.mu.Unlock()
		return err
	}
	r.sweepLocked(p, now, true)
	err = e.addWindow(&p.auth, m, now)
	fire := r.notifyLocked(peer, p, now)
	r.mu.Unlock()
	if fire != nil {
		fire()
	}
	return err
}

func (r *FrameReceiver) Status(peer string) AuthStatus {
	now := r.now()
	r.mu.Lock()
	p := r.peers[peer]
	if p == nil {
		r.mu.Unlock()
		return AuthStatus{}
	}
	r.sweepLocked(p, now, true)
	st := p.auth.status(now)
	fire := r.notifyLocked(peer, p, now)
	r.mu.Unlock()
	if fire != nil {
		fire()
	}
	return st
}
