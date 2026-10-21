package e2ee

import (
	"bytes"
	"crypto/aes"
	"crypto/cipher"
	"encoding/binary"
	"math"
	"sync"
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
	if len(key) != 16 && len(key) != FrameKeySize {
		return nil, ErrInvalid
	}
	block, err := aes.NewCipher(key)
	if err != nil {
		return nil, err
	}
	return cipher.NewGCM(block)
}

type FrameSender struct {
	mu      sync.Mutex
	aead    cipher.AEAD
	raw     []byte
	keyID   byte
	counter uint64
}

func (s *FrameSender) SetKey(k FrameKey) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.aead != nil && bytes.Equal(s.raw, k.Key) {
		s.keyID = k.KeyID
		return nil
	}
	aead, err := newAEAD(k.Key)
	if err != nil {
		return err
	}
	wipe(s.raw)
	s.aead = aead
	s.raw = append([]byte(nil), k.Key...)
	s.keyID = k.KeyID
	s.counter = 0
	return nil
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
	return append(out, s.keyID), nil
}

type replayEntry struct {
	aead    cipher.AEAD
	raw     []byte
	epoch   uint32
	started bool
	top     uint64
	seen    [ReplayWindow / 64]uint64
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
}

type FrameReceiver struct {
	mu    sync.Mutex
	peers map[string]*peerKeys
}

func (r *FrameReceiver) SetKey(k FrameKey) error {
	r.mu.Lock()
	defer r.mu.Unlock()
	if r.peers == nil {
		r.peers = map[string]*peerKeys{}
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
	if old := p.keys[k.KeyID]; old != nil {
		wipe(old.raw)
		for i, id := range p.order {
			if id == k.KeyID {
				p.order = append(p.order[:i], p.order[i+1:]...)
				break
			}
		}
	}
	p.keys[k.KeyID] = &replayEntry{aead: aead, raw: append([]byte(nil), k.Key...), epoch: k.Epoch}
	p.order = append(p.order, k.KeyID)
	p.newest = k.Epoch
	for len(p.order) > KeepFrameKeys {
		drop := p.order[0]
		wipe(p.keys[drop].raw)
		delete(p.keys, drop)
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

func (r *FrameReceiver) Open(peer string, frame []byte, clear int) ([]byte, error) {
	t, err := ParseFrameTrailer(frame)
	if err != nil {
		return nil, err
	}
	r.mu.Lock()
	defer r.mu.Unlock()
	p := r.peers[peer]
	if p == nil {
		return nil, ErrUnknownPeer
	}
	e := p.keys[t.KeyID]
	if e == nil {
		return nil, ErrUnknownPeer
	}
	if !e.fresh(t.Counter) {
		return nil, ErrStale
	}
	body := frame[:len(frame)-FrameTrailer]
	if clear < 0 {
		clear = 0
	}
	clear = min(clear, len(body)-FrameTag)
	plain, err := e.aead.Open(nil, frameNonce(t.Counter), body[clear:], body[:clear])
	if err != nil {
		return nil, ErrInvalid
	}
	e.mark(t.Counter)
	out := make([]byte, 0, clear+len(plain))
	out = append(out, body[:clear]...)
	return append(out, plain...), nil
}
