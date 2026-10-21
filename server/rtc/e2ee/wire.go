package e2ee

import (
	"bytes"
	"crypto/ed25519"
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
	"errors"
	"math"
	"sort"
	"strconv"
	"strings"
	"unicode/utf8"
)

const (
	Version         = 1
	MaxParticipants = 16
	MaxRecipients   = 31
	SecretSize      = 32
	FrameKeySize    = 32
	XKeySize        = 32
	PQKeySize       = 1184
	PQCiphertext    = 1088
	WrappedKeySize  = SecretSize + 16
	SignatureSize   = ed25519.SignatureSize
	IdentitySize    = ed25519.PublicKeySize

	maxCall    = 256
	maxPeer    = 64
	maxAcct    = 128
	maxSafeInt = 1<<53 - 1
	maxEpoch   = math.MaxUint32

	presenceLabel = "forkmesh-call-e2ee-presence-v1"
	keysLabel     = "forkmesh-call-e2ee-keys-v1"
	wrapLabel     = "forkmesh-call-e2ee-wrap-v1"
	frameLabel    = "forkmesh-call-e2ee-frame-v1"
	saltLabel     = "forkmesh-call-e2ee-salt-v1"
	codeLabel     = "forkmesh-call-e2ee-code-v1"
)

var (
	ErrInvalid        = errors.New("e2ee: invalid message")
	ErrSignature      = errors.New("e2ee: bad signature")
	ErrCall           = errors.New("e2ee: message for another call")
	ErrClock          = errors.New("e2ee: timestamp outside the allowed window")
	ErrStale          = errors.New("e2ee: older epoch")
	ErrDuplicate      = errors.New("e2ee: epoch already handled")
	ErrEquivocation   = errors.New("e2ee: two different keys for one epoch")
	ErrPeerBound      = errors.New("e2ee: peer id belongs to another identity")
	ErrUnknownPeer    = errors.New("e2ee: no presence for this peer")
	ErrNotForMe       = errors.New("e2ee: no key wrapped for this participant")
	ErrRecipientEpoch = errors.New("e2ee: key wrapped to a presence key this participant no longer holds")
	ErrFull           = errors.New("e2ee: call is full")
	ErrOwn            = errors.New("e2ee: own message")
	ErrClosed         = errors.New("e2ee: session closed")
	ErrExhausted      = errors.New("e2ee: epoch or counter exhausted")
)

var errorCodes = []struct {
	err  error
	code string
}{
	{ErrInvalid, "invalid"},
	{ErrSignature, "signature"},
	{ErrCall, "call"},
	{ErrClock, "clock"},
	{ErrStale, "stale"},
	{ErrDuplicate, "duplicate"},
	{ErrEquivocation, "equivocation"},
	{ErrPeerBound, "peer-bound"},
	{ErrUnknownPeer, "unknown-peer"},
	{ErrNotForMe, "not-for-me"},
	{ErrRecipientEpoch, "recipient-epoch"},
	{ErrFull, "full"},
	{ErrOwn, "own"},
	{ErrClosed, "closed"},
	{ErrExhausted, "exhausted"},
}

func ErrorCode(err error) string {
	if err == nil {
		return ""
	}
	for _, e := range errorCodes {
		if errors.Is(err, e.err) {
			return e.code
		}
	}
	return "invalid"
}

var b64u = base64.RawURLEncoding.Strict()

func enc(b []byte) string { return b64u.EncodeToString(b) }

func digest(b []byte) string {
	h := sha256.Sum256(b)
	return enc(h[:])
}

func KeyID(idk []byte) string {
	h := sha256.Sum256(idk)
	return enc(h[:])[:16]
}

func validCall(s string) bool {
	if len(s) < 1 || len(s) > maxCall {
		return false
	}
	for i := 0; i < len(s); i++ {
		if s[i] < 0x20 || s[i] > 0x7e {
			return false
		}
	}
	return true
}

func validPeer(s string) bool {
	if len(s) < 1 || len(s) > maxPeer {
		return false
	}
	for i := 0; i < len(s); i++ {
		c := s[i]
		if !(c >= 'a' && c <= 'z' || c >= 'A' && c <= 'Z' || c >= '0' && c <= '9' || c == '-' || c == '_') {
			return false
		}
	}
	return true
}

func validAcct(s string) bool {
	if !utf8.ValidString(s) {
		return false
	}
	n := 0
	for _, r := range s {
		if r < 0x20 || (r >= 0x7f && r <= 0x9f) || r == utf8.RuneError || (r >= 0xd800 && r <= 0xdfff) {
			return false
		}
		n++
	}
	return n >= 1 && n <= maxAcct
}

type fields map[string]json.RawMessage

func parseObject(b []byte) (fields, error) {
	var f fields
	if err := json.Unmarshal(b, &f); err != nil || f == nil {
		return nil, ErrInvalid
	}
	return f, nil
}

func (f fields) str(key string) (string, error) {
	raw, ok := f[key]
	if !ok {
		return "", ErrInvalid
	}
	var s string
	if err := json.Unmarshal(raw, &s); err != nil {
		return "", ErrInvalid
	}
	return s, nil
}

func (f fields) integer(key string, lo, hi int64) (int64, error) {
	raw, ok := f[key]
	if !ok || len(raw) == 0 || !(raw[0] == '-' || raw[0] >= '0' && raw[0] <= '9') {
		return 0, ErrInvalid
	}
	var n json.Number
	if err := json.Unmarshal(raw, &n); err != nil {
		return 0, ErrInvalid
	}
	v, err := strconv.ParseFloat(string(n), 64)
	if err != nil || v != math.Trunc(v) || v < -maxSafeInt || v > maxSafeInt {
		return 0, ErrInvalid
	}
	i := int64(v)
	if i < lo || i > hi {
		return 0, ErrInvalid
	}
	return i, nil
}

func (f fields) bytes(key string, size int) ([]byte, error) {
	s, err := f.str(key)
	if err != nil {
		return nil, err
	}
	if len(s) != base64.RawURLEncoding.EncodedLen(size) {
		return nil, ErrInvalid
	}
	b, err := b64u.DecodeString(s)
	if err != nil || len(b) != size {
		return nil, ErrInvalid
	}
	return b, nil
}

type Presence struct {
	Call  string
	Peer  string
	Acct  string
	IDK   ed25519.PublicKey
	Epoch uint32
	TS    int64
	X     []byte
	PQ    []byte
	Sig   []byte
}

type presenceJSON struct {
	V     int    `json:"v"`
	Call  string `json:"call"`
	Peer  string `json:"peer"`
	Acct  string `json:"acct"`
	KID   string `json:"kid"`
	IDK   string `json:"idk"`
	Epoch uint32 `json:"epoch"`
	TS    int64  `json:"ts"`
	X     string `json:"x"`
	PQ    string `json:"pq"`
	Sig   string `json:"sig"`
}

func (p *Presence) Marshal() []byte {
	b, _ := json.Marshal(presenceJSON{
		V: Version, Call: p.Call, Peer: p.Peer, Acct: p.Acct, KID: KeyID(p.IDK), IDK: enc(p.IDK),
		Epoch: p.Epoch, TS: p.TS, X: enc(p.X), PQ: enc(p.PQ), Sig: enc(p.Sig),
	})
	return b
}

func (p *Presence) SignedMessage() []byte {
	return []byte(strings.Join([]string{
		presenceLabel, p.Call, p.Peer, p.Acct, KeyID(p.IDK), enc(p.IDK),
		strconv.FormatUint(uint64(p.Epoch), 10), strconv.FormatInt(p.TS, 10),
		enc(p.X), digest(p.PQ),
	}, "\n"))
}

func (p *Presence) Verify() error {
	if !ed25519.Verify(p.IDK, p.SignedMessage(), p.Sig) {
		return ErrSignature
	}
	return nil
}

func ParsePresence(b []byte) (*Presence, error) {
	f, err := parseObject(b)
	if err != nil {
		return nil, err
	}
	if v, err := f.integer("v", 0, maxSafeInt); err != nil || v != Version {
		return nil, ErrInvalid
	}
	p := &Presence{}
	var kid string
	var epoch int64
	if p.Call, err = f.str("call"); err != nil || !validCall(p.Call) {
		return nil, ErrInvalid
	}
	if p.Peer, err = f.str("peer"); err != nil || !validPeer(p.Peer) {
		return nil, ErrInvalid
	}
	if p.Acct, err = f.str("acct"); err != nil || !validAcct(p.Acct) {
		return nil, ErrInvalid
	}
	if kid, err = f.str("kid"); err != nil {
		return nil, ErrInvalid
	}
	idk, err := f.bytes("idk", IdentitySize)
	if err != nil {
		return nil, err
	}
	p.IDK = ed25519.PublicKey(idk)
	if kid != KeyID(idk) {
		return nil, ErrInvalid
	}
	if epoch, err = f.integer("epoch", 1, maxEpoch); err != nil {
		return nil, err
	}
	p.Epoch = uint32(epoch)
	if p.TS, err = f.integer("ts", 1, maxSafeInt); err != nil {
		return nil, err
	}
	if p.X, err = f.bytes("x", XKeySize); err != nil {
		return nil, err
	}
	if p.PQ, err = f.bytes("pq", PQKeySize); err != nil {
		return nil, err
	}
	if p.Sig, err = f.bytes("sig", SignatureSize); err != nil {
		return nil, err
	}
	return p, nil
}

type WrappedKey struct {
	Peer  string
	Epoch uint32
	CT    []byte
	Key   []byte
}

type KeyMessage struct {
	Call  string
	Peer  string
	IDK   ed25519.PublicKey
	Epoch uint32
	TS    int64
	Eph   []byte
	Keys  []WrappedKey
	Sig   []byte
}

type wrappedJSON struct {
	Peer  string `json:"peer"`
	Epoch uint32 `json:"epoch"`
	CT    string `json:"ct"`
	Key   string `json:"key"`
}

type keyMessageJSON struct {
	V     int           `json:"v"`
	Call  string        `json:"call"`
	Peer  string        `json:"peer"`
	KID   string        `json:"kid"`
	IDK   string        `json:"idk"`
	Epoch uint32        `json:"epoch"`
	TS    int64         `json:"ts"`
	Eph   string        `json:"eph"`
	Keys  []wrappedJSON `json:"keys"`
	Sig   string        `json:"sig"`
}

func (m *KeyMessage) Marshal() []byte {
	keys := make([]wrappedJSON, len(m.Keys))
	for i, k := range m.Keys {
		keys[i] = wrappedJSON{Peer: k.Peer, Epoch: k.Epoch, CT: enc(k.CT), Key: enc(k.Key)}
	}
	b, _ := json.Marshal(keyMessageJSON{
		V: Version, Call: m.Call, Peer: m.Peer, KID: KeyID(m.IDK), IDK: enc(m.IDK),
		Epoch: m.Epoch, TS: m.TS, Eph: enc(m.Eph), Keys: keys, Sig: enc(m.Sig),
	})
	return b
}

func (m *KeyMessage) SignedMessage() []byte {
	var b bytes.Buffer
	b.WriteString(strings.Join([]string{
		keysLabel, m.Call, m.Peer, KeyID(m.IDK), enc(m.IDK),
		strconv.FormatUint(uint64(m.Epoch), 10), strconv.FormatInt(m.TS, 10), enc(m.Eph),
	}, "\n"))
	for _, k := range m.Keys {
		b.WriteString("\n" + k.Peer + "|" + strconv.FormatUint(uint64(k.Epoch), 10) + "|" + digest(k.CT) + "|" + enc(k.Key))
	}
	return b.Bytes()
}

func (m *KeyMessage) Verify() error {
	if !ed25519.Verify(m.IDK, m.SignedMessage(), m.Sig) {
		return ErrSignature
	}
	return nil
}

func (m *KeyMessage) KeyFor(peer string) (WrappedKey, bool) {
	i := sort.Search(len(m.Keys), func(i int) bool { return m.Keys[i].Peer >= peer })
	if i < len(m.Keys) && m.Keys[i].Peer == peer {
		return m.Keys[i], true
	}
	return WrappedKey{}, false
}

func ParseKeyMessage(b []byte) (*KeyMessage, error) {
	f, err := parseObject(b)
	if err != nil {
		return nil, err
	}
	if v, err := f.integer("v", 0, maxSafeInt); err != nil || v != Version {
		return nil, ErrInvalid
	}
	m := &KeyMessage{}
	var kid string
	var epoch int64
	if m.Call, err = f.str("call"); err != nil || !validCall(m.Call) {
		return nil, ErrInvalid
	}
	if m.Peer, err = f.str("peer"); err != nil || !validPeer(m.Peer) {
		return nil, ErrInvalid
	}
	if kid, err = f.str("kid"); err != nil {
		return nil, ErrInvalid
	}
	idk, err := f.bytes("idk", IdentitySize)
	if err != nil {
		return nil, err
	}
	m.IDK = ed25519.PublicKey(idk)
	if kid != KeyID(idk) {
		return nil, ErrInvalid
	}
	if epoch, err = f.integer("epoch", 1, maxEpoch); err != nil {
		return nil, err
	}
	m.Epoch = uint32(epoch)
	if m.TS, err = f.integer("ts", 1, maxSafeInt); err != nil {
		return nil, err
	}
	if m.Eph, err = f.bytes("eph", XKeySize); err != nil {
		return nil, err
	}
	if m.Sig, err = f.bytes("sig", SignatureSize); err != nil {
		return nil, err
	}
	raw, ok := f["keys"]
	if !ok {
		return nil, ErrInvalid
	}
	var list []json.RawMessage
	if err := json.Unmarshal(raw, &list); err != nil || len(list) < 1 || len(list) > MaxRecipients {
		return nil, ErrInvalid
	}
	m.Keys = make([]WrappedKey, len(list))
	for i, item := range list {
		kf, err := parseObject(item)
		if err != nil {
			return nil, err
		}
		k := &m.Keys[i]
		if k.Peer, err = kf.str("peer"); err != nil || !validPeer(k.Peer) || k.Peer == m.Peer {
			return nil, ErrInvalid
		}
		if i > 0 && k.Peer <= m.Keys[i-1].Peer {
			return nil, ErrInvalid
		}
		if epoch, err = kf.integer("epoch", 1, maxEpoch); err != nil {
			return nil, err
		}
		k.Epoch = uint32(epoch)
		if k.CT, err = kf.bytes("ct", PQCiphertext); err != nil {
			return nil, err
		}
		if k.Key, err = kf.bytes("key", WrappedKeySize); err != nil {
			return nil, err
		}
	}
	return m, nil
}
