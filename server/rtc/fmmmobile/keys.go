package fmmmobile

import (
	"bytes"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"strings"
	"sync"
	"time"

	"forkmesh.com/mirror-node/rtc/e2ee"
)

const (
	heartbeatEvery = 20 * time.Second
	rotateEvery    = 10 * time.Minute
	forcedEvery    = 10 * time.Second
	unknownAfter   = 3 * time.Second
	debounce       = 250 * time.Millisecond
	installDelay   = time.Second
	resendEvery    = 2 * time.Second
	expireEvery    = time.Second
	verifyEvery    = 250 * time.Millisecond
	maxHeld        = 32
	heldFor        = 30 * time.Second
)

type heldMessage struct {
	kind   string
	data   []byte
	kid    string
	acct   string
	expiry time.Time
}

type keys struct {
	c    *Call
	sess *e2ee.Session

	mu        sync.Mutex
	live      bool
	rekeyAt   time.Time
	commitAt  time.Time
	installAt time.Time
	pending   *e2ee.FrameKey
	beatAt    time.Time
	rotatedAt time.Time
	forcedAt  time.Time
	resentAt  time.Time
	expiredAt time.Time
	verifyAt  time.Time
	heardAt   map[string]time.Time
	unknown   map[string]time.Time
	held      []heldMessage
	snapshot  string
	installed int
}

func newKeys(c *Call, cfg e2ee.Config) (*keys, error) {
	s, err := e2ee.NewSession(cfg)
	if err != nil {
		return nil, err
	}
	return &keys{c: c, sess: s, heardAt: map[string]time.Time{}, unknown: map[string]time.Time{}}, nil
}

func (k *keys) send(kind string, data []byte) {
	if len(data) == 0 {
		return
	}
	k.c.emit(&Event{Kind: EventChat, Name: kind, Data: data})
}

func (k *keys) start(now time.Time) {
	k.mu.Lock()
	defer k.mu.Unlock()
	if k.live {
		return
	}
	k.live = true
	k.rotatedAt, k.beatAt = now, now
	if b, err := k.sess.Presence(); err == nil {
		k.send(ChatPresence, b)
	}
	k.rekeyLocked(now)
	k.schedule(&k.commitAt, now)
}

func (k *keys) close() {
	k.mu.Lock()
	k.live = false
	k.pending = nil
	k.mu.Unlock()
	k.sess.Close()
}

func (k *keys) peers() []string { return k.sess.Peers() }

func (k *keys) rotateLocked(now time.Time) {
	b, err := k.sess.Rotate()
	if err != nil {
		return
	}
	k.rotatedAt, k.beatAt = now, now
	k.send(ChatPresence, b)
}

func (k *keys) rekeyLocked(now time.Time) {
	k.rekeyAt = time.Time{}
	msg, key, err := k.sess.Rekey()
	if err != nil {
		return
	}
	if msg == nil {
		k.installLocked(key)
		return
	}
	k.send(ChatKey, msg)
	k.pending = &key
	k.installAt = now.Add(installDelay)
}

func (k *keys) installLocked(key e2ee.FrameKey) {
	k.pending = nil
	k.installAt = time.Time{}
	if k.c.tx.SetKey(key) == nil {
		k.installed++
	}
}

func (k *keys) commitLocked() {
	k.commitAt = time.Time{}
	if len(k.sess.Peers()) == 0 {
		return
	}
	b, err := k.sess.Commit()
	if err != nil {
		return
	}
	k.send(ChatVerify, b)
	k.revealLocked()
}

func (k *keys) revealLocked() {
	if b, err := k.sess.Reveal(); err == nil {
		k.send(ChatVerify, b)
	}
}

func (k *keys) schedule(at *time.Time, now time.Time) {
	if at.IsZero() {
		*at = now.Add(debounce)
	}
}

func (k *keys) rosterChangedLocked(now time.Time) {
	k.rotateLocked(now)
	k.schedule(&k.rekeyAt, now)
	k.schedule(&k.commitAt, now)
}

func (k *keys) tick(now time.Time) {
	k.mu.Lock()
	defer k.mu.Unlock()
	if !k.live {
		return
	}
	if !k.rekeyAt.IsZero() && !now.Before(k.rekeyAt) {
		k.rekeyLocked(now)
	}
	if k.pending != nil && !now.Before(k.installAt) {
		k.installLocked(*k.pending)
	}
	if !k.commitAt.IsZero() && !now.Before(k.commitAt) {
		k.commitLocked()
	}
	if now.Sub(k.rotatedAt) >= rotateEvery {
		k.rotateLocked(now)
	} else if now.Sub(k.beatAt) >= heartbeatEvery {
		k.beatAt = now
		if b, err := k.sess.Presence(); err == nil {
			k.send(ChatPresence, b)
		}
	}
	if now.Sub(k.expiredAt) >= expireEvery {
		k.expiredAt = now
		gone := k.sess.Expire()
		for _, p := range gone {
			k.forgetLocked(p)
		}
		if len(gone) > 0 {
			k.rosterChangedLocked(now)
		}
		k.sess.Prune()
		k.dropHeldLocked(now)
	}
	if now.Sub(k.verifyAt) >= verifyEvery {
		k.verifyAt = now
		st := k.sess.Verification()
		if st.NeedCommit && k.commitAt.IsZero() && len(k.sess.Peers()) > 0 {
			k.schedule(&k.commitAt, now)
		}
		if len(st.Waiting) > 0 && now.Sub(k.resentAt) >= resendEvery {
			k.resentAt = now
			if b, err := k.sess.Commit(); err == nil {
				k.send(ChatVerify, b)
			}
			k.revealLocked()
		}
		k.reportLocked(st)
	}
	for peer, since := range k.unknown {
		if now.Sub(since) < unknownAfter {
			continue
		}
		delete(k.unknown, peer)
		if heard, ok := k.heardAt[peer]; ok && now.Sub(heard) >= unknownAfter && now.Sub(k.forcedAt) >= forcedEvery {
			k.forcedAt = now
			k.rotateLocked(now)
		}
	}
}

func (k *keys) forgetLocked(peer string) {
	k.c.rx.Forget(peer)
	delete(k.heardAt, peer)
	delete(k.unknown, peer)
}

func (k *keys) unknownKey(peer string, now time.Time) {
	k.mu.Lock()
	defer k.mu.Unlock()
	if _, ok := k.unknown[peer]; !ok {
		k.unknown[peer] = now
	}
}

func (k *keys) reportLocked(st e2ee.VerifyStatus) {
	v := verification(st)
	b, _ := json.Marshal(v)
	if string(b) == k.snapshot {
		return
	}
	k.snapshot = string(b)
	k.c.emit(&Event{Kind: EventVerify, Text: v.Emoji, Name: v.Digits, State: boolInt(v.Verified)})
}

func boolInt(b bool) int {
	if b {
		return 1
	}
	return 0
}

func verification(st e2ee.VerifyStatus) *Verification {
	v := &Verification{
		Round: base64.RawURLEncoding.EncodeToString(st.Round), Gen: int(st.Gen), Waiting: strings.Join(st.Waiting, "\n"),
		Late: st.Late, Rounds: st.Rounds, Unfinished: st.Unfinished, Verified: st.Verified,
		Newcomers: strings.Join(st.Newcomers, "\n"), NeedCommit: st.NeedCommit,
	}
	if st.Code != nil {
		v.HasCode = true
		v.Round = base64.RawURLEncoding.EncodeToString(st.Code.Round)
		v.Emoji, v.EmojiNames, v.Digits, v.QR = st.Code.EmojiText(), st.Code.EmojiNames(), st.Code.Digits, st.Code.QR
		parts := make([]string, len(st.Code.Emoji))
		for i, e := range st.Code.Emoji {
			parts[i] = fmt.Sprint(e)
		}
		v.Indices = strings.Join(parts, " ")
	}
	return v
}

func (k *keys) dropHeldLocked(now time.Time) {
	keep := k.held[:0]
	for _, h := range k.held {
		if now.Before(h.expiry) {
			keep = append(keep, h)
		}
	}
	k.held = keep
}

func (k *keys) holdLocked(kind string, data []byte, kid, acct string, now time.Time) {
	if len(k.held) >= maxHeld {
		k.held = k.held[1:]
	}
	k.held = append(k.held, heldMessage{kind: kind, data: append([]byte(nil), data...), kid: kid, acct: acct, expiry: now.Add(heldFor)})
}

func (k *keys) retryHeldLocked(now time.Time) {
	held := k.held
	k.held = nil
	for _, h := range held {
		if err := k.handleLocked(h.kind, h.data, h.kid, h.acct, now); errors.Is(err, e2ee.ErrUnknownPeer) && now.Before(h.expiry) {
			k.held = append(k.held, h)
		}
	}
}

func (k *keys) handle(kind string, data []byte, kid, acct string, now time.Time) error {
	k.mu.Lock()
	defer k.mu.Unlock()
	err := k.handleLocked(kind, data, kid, acct, now)
	if errors.Is(err, e2ee.ErrUnknownPeer) && kind != ChatPresence {
		k.holdLocked(kind, data, kid, acct, now)
	}
	return err
}

func (k *keys) handleLocked(kind string, data []byte, kid, acct string, now time.Time) error {
	switch kind {
	case ChatPresence:
		p, err := e2ee.ParsePresence(data)
		if err != nil {
			return err
		}
		if kid == "" || e2ee.KeyID(p.IDK) != kid || (acct != "" && p.Acct != acct) {
			return ErrSender
		}
		ev, err := k.sess.AcceptPresence(data)
		if err != nil {
			return err
		}
		k.heardAt[p.Peer] = now
		switch ev {
		case e2ee.EventJoined:
			if k.live {
				k.rosterChangedLocked(now)
			}
		case e2ee.EventUpdated:
			if k.live {
				k.schedule(&k.rekeyAt, now)
			}
		}
		if ev != e2ee.EventSame {
			k.retryHeldLocked(now)
		}
		return nil
	case ChatKey:
		m, err := e2ee.ParseKeyMessage(data)
		if err != nil {
			return err
		}
		if kid == "" || e2ee.KeyID(m.IDK) != kid {
			return ErrSender
		}
		key, err := k.sess.OpenKeys(data)
		if err != nil {
			if errors.Is(err, e2ee.ErrRecipientEpoch) && now.Sub(k.forcedAt) >= forcedEvery {
				k.forcedAt = now
				k.rotateLocked(now)
			}
			return err
		}
		delete(k.unknown, key.Peer)
		return k.c.rx.SetKey(key)
	case ChatVerify:
		var head struct {
			T string `json:"t"`
		}
		if err := json.Unmarshal(data, &head); err != nil {
			return e2ee.ErrInvalid
		}
		switch head.T {
		case "commit":
			cm, err := e2ee.ParseCommit(data)
			if err != nil {
				return err
			}
			if kid == "" || e2ee.KeyID(cm.IDK) != kid {
				return ErrSender
			}
			if err := k.sess.AcceptCommit(data); err != nil {
				return err
			}
			k.revealLocked()
		case "reveal":
			r, err := e2ee.ParseReveal(data)
			if err != nil {
				return err
			}
			member, ok := k.sess.Member(r.Peer)
			if !ok {
				return e2ee.ErrUnknownPeer
			}
			if kid == "" || e2ee.KeyID(member.IDK) != kid {
				return ErrSender
			}
			if err := k.sess.AcceptReveal(data); err != nil && !errors.Is(err, e2ee.ErrWaiting) {
				return err
			}
		default:
			return ErrUnknown
		}
		k.reportLocked(k.sess.Verification())
		return nil
	}
	return ErrUnknown
}

func (k *keys) leave(peer string, now time.Time) {
	k.mu.Lock()
	defer k.mu.Unlock()
	if !k.sess.Leave(peer) {
		return
	}
	k.forgetLocked(peer)
	if k.live {
		k.rosterChangedLocked(now)
	}
}

func (k *keys) verify() *Verification {
	return verification(k.sess.Verification())
}

func (k *keys) markVerified(round string) error {
	raw, err := base64.RawURLEncoding.DecodeString(round)
	if err != nil {
		return e2ee.ErrInvalid
	}
	st := k.sess.Verification()
	if st.Code == nil || !bytes.Equal(st.Code.Round, raw) {
		return e2ee.ErrStale
	}
	if err := k.sess.MarkVerified(raw); err != nil {
		return err
	}
	k.mu.Lock()
	k.reportLocked(k.sess.Verification())
	k.mu.Unlock()
	return nil
}

func (c *Call) HandleChat(kind string, data []byte, senderKid, senderAcct string) error {
	return c.keys.handle(kind, data, senderKid, senderAcct, time.Now())
}

func (c *Call) PeerLeft(peer string) {
	c.keys.leave(peer, time.Now())
}

func (c *Call) Verification() *Verification { return c.keys.verify() }

func (c *Call) MarkVerified(round string) error { return c.keys.markVerified(round) }

func (c *Call) CheckQR(text string) (bool, error) {
	ok, err := c.keys.sess.CheckQR(text)
	if err == nil {
		c.keys.mu.Lock()
		c.keys.reportLocked(c.keys.sess.Verification())
		c.keys.mu.Unlock()
	}
	return ok, err
}

func (c *Call) Unverify() {
	c.keys.sess.Unverify()
	c.keys.mu.Lock()
	c.keys.reportLocked(c.keys.sess.Verification())
	c.keys.mu.Unlock()
}

func (c *Call) Presence() ([]byte, error) { return c.keys.sess.Presence() }

func (c *Call) IdentityKeyID() string { return e2ee.KeyID(c.keys.sess.IDK()) }

func (c *Call) MediaKeys() int {
	c.keys.mu.Lock()
	defer c.keys.mu.Unlock()
	return c.keys.installed
}

func ErrorCode(err error) string { return e2ee.ErrorCode(err) }
