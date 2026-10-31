package e2ee

import (
	"bytes"
	"crypto/ed25519"
	"crypto/sha256"
	"encoding/binary"
	"math"
	"time"
)

const (
	AuthDigestSize = 10
	MaxAuthFrames  = 100
	AuthHeaderSize = 15
	MaxAuthSize    = AuthHeaderSize + MaxAuthFrames*AuthDigestSize + SignatureSize
	AuthInterval   = 200 * time.Millisecond
	AuthDeadline   = time.Second
	AuthHold       = 10 * time.Second
	MissingGrace   = 2 * time.Second
	AuthRecent     = 2 * time.Second
	authType       = Version<<4 | 1
	maxAuthWindows = 64
	sweepEvery     = 50 * time.Millisecond
)

type AuthState int

const (
	AuthPending AuthState = iota
	AuthVerified
	AuthMissing
	AuthForged
)

func (a AuthState) String() string {
	switch a {
	case AuthVerified:
		return "verified"
	case AuthMissing:
		return "missing"
	case AuthForged:
		return "forged"
	}
	return "pending"
}

type AuthStatus struct {
	State    AuthState
	Dropping bool
	Verified uint64
	Missing  uint64
	Forged   uint64
}

type AuthMessage struct {
	KeyID   byte
	Epoch   uint32
	First   uint64
	Digests []byte
	Sig     []byte
}

func (m *AuthMessage) Count() int { return len(m.Digests) / AuthDigestSize }

func (m *AuthMessage) Body() []byte {
	b := make([]byte, 0, AuthHeaderSize+len(m.Digests)+SignatureSize)
	b = append(b, authType, m.KeyID)
	b = binary.BigEndian.AppendUint32(b, m.Epoch)
	b = binary.BigEndian.AppendUint64(b, m.First)
	b = append(b, byte(m.Count()))
	return append(b, m.Digests...)
}

func (m *AuthMessage) Marshal() []byte { return append(m.Body(), m.Sig...) }

func authSigned(call, peer string, body []byte) []byte {
	out := make([]byte, 0, len(authLabel)+len(call)+len(peer)+3+len(body))
	out = append(out, authLabel+"\n"+call+"\n"+peer+"\n"...)
	return append(out, body...)
}

func (m *AuthMessage) Sign(call, peer string, key ed25519.PrivateKey) {
	m.Sig = ed25519.Sign(key, authSigned(call, peer, m.Body()))
}

func (m *AuthMessage) Verify(call, peer string, key ed25519.PublicKey) error {
	if len(key) != ed25519.PublicKeySize || !ed25519.Verify(key, authSigned(call, peer, m.Body()), m.Sig) {
		return ErrSignature
	}
	return nil
}

func (m *AuthMessage) digestAt(counter uint64) ([]byte, bool) {
	if counter < m.First || counter-m.First >= uint64(m.Count()) {
		return nil, false
	}
	i := int(counter-m.First) * AuthDigestSize
	return m.Digests[i : i+AuthDigestSize], true
}

func ParseAuthMessage(b []byte) (*AuthMessage, error) {
	if len(b) < AuthHeaderSize+AuthDigestSize+SignatureSize || len(b) > MaxAuthSize || b[0] != authType {
		return nil, ErrInvalid
	}
	n := int(b[14])
	if n < 1 || n > MaxAuthFrames || len(b) != AuthHeaderSize+n*AuthDigestSize+SignatureSize {
		return nil, ErrInvalid
	}
	m := &AuthMessage{KeyID: b[1], Epoch: binary.BigEndian.Uint32(b[2:6]), First: binary.BigEndian.Uint64(b[6:14])}
	if m.Epoch == 0 || m.KeyID != byte(m.Epoch) || m.First == 0 || m.First > math.MaxUint64-uint64(n-1) {
		return nil, ErrInvalid
	}
	m.Digests = append([]byte(nil), b[AuthHeaderSize:AuthHeaderSize+n*AuthDigestSize]...)
	m.Sig = append([]byte(nil), b[AuthHeaderSize+n*AuthDigestSize:]...)
	return m, nil
}

func FrameDigest(sealed []byte) []byte {
	h := sha256.New()
	h.Write([]byte(digestLabel))
	h.Write(sealed)
	return h.Sum(nil)[:AuthDigestSize]
}

type pendingFrame struct {
	digest [AuthDigestSize]byte
	at     time.Time
}

type authWindow struct {
	msg *AuthMessage
	at  time.Time
}

type verdict int

const (
	verdictPending verdict = iota
	verdictVerified
	verdictForged
	verdictMissing
)

type peerAuth struct {
	verified, missing, forged             uint64
	lastVerified, lastMissing, lastForged time.Time
	missingRun                            time.Time
	lastSweep                             time.Time
	state                                 AuthState
}

func (p *peerAuth) record(v verdict, now time.Time) {
	switch v {
	case verdictVerified:
		p.verified++
		p.lastVerified = now
		p.missingRun = time.Time{}
	case verdictForged:
		p.forged++
		p.lastForged = now
	case verdictMissing:
		p.missing++
		p.lastMissing = now
		if p.missingRun.IsZero() {
			p.missingRun = now
		}
	}
}

func (p *peerAuth) evaluate(now time.Time) AuthState {
	switch {
	case !p.lastForged.IsZero() && now.Sub(p.lastForged) < AuthHold:
		return AuthForged
	case !p.lastMissing.IsZero() && now.Sub(p.lastMissing) < AuthRecent:
		return AuthMissing
	case !p.lastVerified.IsZero():
		return AuthVerified
	}
	return AuthPending
}

func (p *peerAuth) drop(now time.Time) error {
	switch p.evaluate(now) {
	case AuthForged:
		return ErrForged
	case AuthMissing:
		if !p.missingRun.IsZero() && now.Sub(p.missingRun) >= MissingGrace {
			return ErrUnverified
		}
	}
	return nil
}

func (p *peerAuth) status(now time.Time) AuthStatus {
	return AuthStatus{State: p.evaluate(now), Dropping: p.drop(now) != nil, Verified: p.verified, Missing: p.missing, Forged: p.forged}
}

func (e *replayEntry) judge(counter uint64, digest []byte, now time.Time) verdict {
	for i := len(e.windows) - 1; i >= 0; i-- {
		if want, ok := e.windows[i].msg.digestAt(counter); ok {
			if bytes.Equal(want, digest) {
				return verdictVerified
			}
			return verdictForged
		}
	}
	if e.pending == nil {
		e.pending = map[uint64]pendingFrame{}
	}
	var pf pendingFrame
	copy(pf.digest[:], digest)
	pf.at = now
	e.pending[counter] = pf
	return verdictPending
}

func (e *replayEntry) sweep(p *peerAuth, now time.Time, force bool) {
	for c, pf := range e.pending {
		if force || now.Sub(pf.at) > AuthDeadline {
			delete(e.pending, c)
			at := pf.at.Add(AuthDeadline)
			if at.After(now) {
				at = now
			}
			p.record(verdictMissing, at)
		}
	}
	kept := e.windows[:0]
	for _, w := range e.windows {
		if now.Sub(w.at) <= AuthDeadline {
			kept = append(kept, w)
		}
	}
	for i := len(kept); i < len(e.windows); i++ {
		e.windows[i] = authWindow{}
	}
	e.windows = kept
}

func (e *replayEntry) trimPending(p *peerAuth, now time.Time) {
	for len(e.pending) > ReplayWindow {
		var oldest uint64 = math.MaxUint64
		for c := range e.pending {
			oldest = min(oldest, c)
		}
		delete(e.pending, oldest)
		p.record(verdictMissing, now)
	}
}

func (e *replayEntry) addWindow(p *peerAuth, m *AuthMessage, now time.Time) error {
	last := m.First + uint64(m.Count()-1)
	for _, w := range e.windows {
		o := w.msg
		lo, hi := max(o.First, m.First), min(o.First+uint64(o.Count()-1), last)
		if lo > hi {
			continue
		}
		oa, ma, n := int(lo-o.First)*AuthDigestSize, int(lo-m.First)*AuthDigestSize, int(hi-lo+1)*AuthDigestSize
		if !bytes.Equal(o.Digests[oa:oa+n], m.Digests[ma:ma+n]) {
			p.record(verdictForged, now)
			return ErrEquivocation
		}
		if o.First == m.First && o.Count() == m.Count() {
			return ErrDuplicate
		}
	}
	e.windows = append(e.windows, authWindow{msg: m, at: now})
	if len(e.windows) > maxAuthWindows {
		e.windows[0] = authWindow{}
		e.windows = e.windows[1:]
	}
	for c, pf := range e.pending {
		if want, ok := m.digestAt(c); ok {
			delete(e.pending, c)
			if bytes.Equal(want, pf.digest[:]) {
				p.record(verdictVerified, now)
			} else {
				p.record(verdictForged, now)
			}
		}
	}
	return nil
}
