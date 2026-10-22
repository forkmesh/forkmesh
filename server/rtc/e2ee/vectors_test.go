package e2ee

import (
	"bytes"
	"crypto/ecdh"
	"crypto/ed25519"
	"crypto/hkdf"
	"crypto/mlkem"
	"crypto/rand"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"flag"
	"os"
	"path/filepath"
	"sort"
	"testing"
	"time"
)

var updateVectors = flag.Bool("update", false, "rewrite testdata/go-vectors.json")

const vectorCall = "chat:private-room-demo"

type vIdentity struct {
	Seed string `json:"seed"`
	IDK  string `json:"idk"`
	Acct string `json:"acct"`
	Peer string `json:"peer"`
}

type vPresence struct {
	Name      string          `json:"name"`
	Epoch     uint32          `json:"epoch"`
	X25519    string          `json:"x25519"`
	MLKEMSeed string          `json:"mlkemSeed"`
	Message   json.RawMessage `json:"message"`
}

type vWrap struct {
	Recipient     string `json:"recipient"`
	PresenceEpoch uint32 `json:"presenceEpoch"`
	SS1           string `json:"ss1"`
	SS2           string `json:"ss2"`
	KEK           string `json:"kek"`
	Nonce         string `json:"nonce"`
	Info          string `json:"info"`
}

type vKeys struct {
	Sender     string          `json:"sender"`
	Secret     string          `json:"secret"`
	FrameKey   string          `json:"frameKey"`
	KeyID      int             `json:"keyId"`
	EphPrivate string          `json:"ephPrivate"`
	AuthSeed   string          `json:"authSeed"`
	Message    json.RawMessage `json:"message"`
	Wraps      []vWrap         `json:"wraps"`
}

type vSAS struct {
	Members    []string `json:"members"`
	Gen        uint32   `json:"gen"`
	Nonces     []string `json:"nonces"`
	Round      string   `json:"round"`
	Transcript string   `json:"transcript"`
	Emoji      []int    `json:"emoji"`
	EmojiText  string   `json:"emojiText"`
	Digits     string   `json:"digits"`
	QR         string   `json:"qr"`
}

type vCommit struct {
	Name   string          `json:"name"`
	Commit json.RawMessage `json:"commit"`
	Reveal json.RawMessage `json:"reveal"`
}

type vRound struct {
	SAS     vSAS      `json:"sas"`
	Commits []vCommit `json:"commits"`
}

type vAuthReject struct {
	Case    string `json:"case"`
	Message string `json:"message"`
	Error   string `json:"error"`
}

type vMedia struct {
	Sender  string        `json:"sender"`
	Key     int           `json:"key"`
	Frames  []string      `json:"frames"`
	Auth    []string      `json:"auth"`
	Forged  string        `json:"forged"`
	Rejects []vAuthReject `json:"rejects"`
}

type vEmoji struct {
	Text string `json:"text"`
	Name string `json:"name"`
}

type vFrame struct {
	Key    string `json:"key"`
	KeyID  int    `json:"keyId"`
	Clear  int    `json:"clear"`
	Plain  string `json:"plain"`
	Sealed string `json:"sealed"`
}

type vReject struct {
	What          string            `json:"what"`
	Case          string            `json:"case"`
	As            string            `json:"as"`
	Before        []json.RawMessage `json:"before"`
	BeforeCommits []json.RawMessage `json:"beforeCommits"`
	Message       json.RawMessage   `json:"message"`
	Error         string            `json:"error"`
}

type vectorFile struct {
	Generator  string               `json:"generator"`
	Call       string               `json:"call"`
	Now        int64                `json:"now"`
	Identities map[string]vIdentity `json:"identities"`
	Presences  []vPresence          `json:"presences"`
	Keys       []vKeys              `json:"keys"`
	Round      vRound               `json:"round"`
	SAS        []vSAS               `json:"sas"`
	Frames     []vFrame             `json:"frames"`
	Media      vMedia               `json:"media"`
	Emoji      []vEmoji             `json:"emoji"`
	Rejects    []vReject            `json:"rejects"`
}

func mustDec(t testing.TB, s string) []byte {
	t.Helper()
	b, err := b64u.DecodeString(s)
	if err != nil {
		t.Fatalf("decode %q: %v", s, err)
	}
	return b
}

func randBytes(n int) []byte {
	b := make([]byte, n)
	rand.Read(b)
	return b
}

type genIdentity struct {
	name string
	seed []byte
	key  ed25519.PrivateKey
	acct string
	peer string
}

type genPresence struct {
	who  *genIdentity
	xs   []byte
	seed []byte
	p    *Presence
}

func makePresence(call string, who *genIdentity, epoch uint32, ts int64, xs, seed []byte) *Presence {
	x, _ := ecdh.X25519().NewPrivateKey(xs)
	dk, _ := mlkem.NewDecapsulationKey768(seed)
	p := &Presence{Call: call, Peer: who.peer, Acct: who.acct, IDK: who.key.Public().(ed25519.PublicKey),
		Epoch: epoch, TS: ts, X: x.PublicKey().Bytes(), PQ: dk.EncapsulationKey().Bytes()}
	p.Sig = ed25519.Sign(who.key, p.SignedMessage())
	return p
}

func makeKeyMessage(call string, who *genIdentity, epoch uint32, ts int64, ephPriv, secret, authSeed []byte, to []*Presence) (*KeyMessage, []*wrapTrace) {
	eph, _ := ecdh.X25519().NewPrivateKey(ephPriv)
	m := &KeyMessage{Call: call, Peer: who.peer, IDK: who.key.Public().(ed25519.PublicKey), Epoch: epoch, TS: ts, Eph: eph.PublicKey().Bytes(),
		Auth: ed25519.NewKeyFromSeed(authSeed).Public().(ed25519.PublicKey)}
	sorted := append([]*Presence(nil), to...)
	sort.Slice(sorted, func(i, j int) bool { return sorted[i].Peer < sorted[j].Peer })
	var traces []*wrapTrace
	for _, r := range sorted {
		w, trace, err := wrapSecret(call, Sender{Peer: m.Peer, IDK: m.IDK, Epoch: epoch}, r, eph, secret)
		if err != nil {
			panic(err)
		}
		m.Keys = append(m.Keys, w)
		traces = append(traces, trace)
	}
	m.Sig = ed25519.Sign(who.key, m.SignedMessage())
	return m, traces
}

func resignPresence(p *Presence, key ed25519.PrivateKey) []byte {
	p.Sig = ed25519.Sign(key, p.SignedMessage())
	return p.Marshal()
}

func resignKeys(m *KeyMessage, key ed25519.PrivateKey) []byte {
	m.Sig = ed25519.Sign(key, m.SignedMessage())
	return m.Marshal()
}

func clonePresence(p *Presence) *Presence {
	c := *p
	c.X = append([]byte(nil), p.X...)
	c.PQ = append([]byte(nil), p.PQ...)
	c.Sig = append([]byte(nil), p.Sig...)
	return &c
}

func cloneKeys(m *KeyMessage) *KeyMessage {
	c := *m
	c.Keys = make([]WrappedKey, len(m.Keys))
	for i, k := range m.Keys {
		c.Keys[i] = WrappedKey{Peer: k.Peer, Epoch: k.Epoch, CT: append([]byte(nil), k.CT...), Key: append([]byte(nil), k.Key...)}
	}
	c.Sig = append([]byte(nil), m.Sig...)
	return &c
}

func patchJSON(t testing.TB, b []byte, key string, value any) []byte {
	t.Helper()
	var m map[string]any
	if err := json.Unmarshal(b, &m); err != nil {
		t.Fatal(err)
	}
	if value == nil {
		delete(m, key)
	} else {
		m[key] = value
	}
	out, _ := json.Marshal(m)
	return out
}

func generateVectors(t testing.TB) *vectorFile {
	now := time.Now().UnixMilli()
	ids := map[string]*genIdentity{}
	v := &vectorFile{Generator: "go", Call: vectorCall, Now: now, Identities: map[string]vIdentity{}}
	for _, spec := range []struct{ name, acct, peer string }{
		{"alice", "alice", "0a1b2c3d4e5f60718293a4b5c6d7e8f9"},
		{"bob", "bob", "1f2e3d4c5b6a79880716253443526170"},
		{"carol", "carol ü", "2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c"},
	} {
		seed := randBytes(32)
		id := &genIdentity{name: spec.name, seed: seed, key: ed25519.NewKeyFromSeed(seed), acct: spec.acct, peer: spec.peer}
		ids[spec.name] = id
		v.Identities[spec.name] = vIdentity{Seed: enc(seed), IDK: enc(id.key.Public().(ed25519.PublicKey)), Acct: id.acct, Peer: id.peer}
	}
	pres := map[string][]*genPresence{}
	for _, spec := range []struct {
		name  string
		epoch uint32
	}{{"alice", 1}, {"bob", 1}, {"carol", 1}, {"carol", 2}} {
		gp := &genPresence{who: ids[spec.name], xs: randBytes(32), seed: randBytes(64)}
		gp.p = makePresence(vectorCall, gp.who, spec.epoch, now, gp.xs, gp.seed)
		pres[spec.name] = append(pres[spec.name], gp)
		v.Presences = append(v.Presences, vPresence{Name: spec.name, Epoch: spec.epoch, X25519: enc(gp.xs), MLKEMSeed: enc(gp.seed), Message: gp.p.Marshal()})
	}
	latest := func(name string) *Presence { l := pres[name]; return l[len(l)-1].p }
	keySpecs := []struct {
		sender string
		epoch  uint32
		to     []*Presence
	}{
		{"alice", 1, []*Presence{latest("bob"), latest("carol")}},
		{"bob", 1, []*Presence{latest("alice"), pres["carol"][0].p}},
		{"alice", 257, []*Presence{latest("bob")}},
	}
	for _, spec := range keySpecs {
		who := ids[spec.sender]
		secret, ephPriv, authSeed := randBytes(32), randBytes(32), randBytes(32)
		m, traces := makeKeyMessage(vectorCall, who, spec.epoch, now, ephPriv, secret, authSeed, spec.to)
		fk, err := DeriveFrameKey(vectorCall, Sender{Peer: who.peer, IDK: m.IDK, Epoch: spec.epoch}, secret)
		if err != nil {
			t.Fatal(err)
		}
		entry := vKeys{Sender: spec.sender, Secret: enc(secret), FrameKey: enc(fk.Key), KeyID: int(fk.KeyID), EphPrivate: enc(ephPriv), AuthSeed: enc(authSeed), Message: m.Marshal()}
		for i, w := range m.Keys {
			name := ""
			for n, id := range ids {
				if id.peer == w.Peer {
					name = n
				}
			}
			tr := traces[i]
			entry.Wraps = append(entry.Wraps, vWrap{Recipient: name, PresenceEpoch: w.Epoch, SS1: enc(tr.SS1), SS2: enc(tr.SS2), KEK: enc(tr.KEK), Nonce: enc(tr.Nonce), Info: tr.Info})
		}
		v.Keys = append(v.Keys, entry)
	}
	v.Round = generateRound(t, now, ids)
	for _, spec := range []struct {
		names []string
		gen   uint32
	}{{[]string{"alice", "bob"}, 2}, {[]string{"carol", "bob"}, 7}, {[]string{"bob"}, 1}} {
		v.SAS = append(v.SAS, makeSAS(t, ids, spec.names, spec.gen, nil))
	}
	aliceKey := mustDec(t, v.Keys[0].FrameKey)
	senderKey := FrameKey{Call: vectorCall, Peer: ids["alice"].peer, Epoch: 1, KeyID: byte(v.Keys[0].KeyID), Key: aliceKey, AuthSeed: mustDec(t, v.Keys[0].AuthSeed)}
	var fs FrameSender
	if err := fs.SetKey(senderKey); err != nil {
		t.Fatal(err)
	}
	v.Media = vMedia{Sender: "alice", Key: 0}
	for _, spec := range []struct {
		clear int
		plain []byte
	}{{1, randBytes(60)}, {10, randBytes(400)}, {3, randBytes(120)}, {1, []byte{0xfc}}} {
		sealed, err := fs.Seal(spec.plain, spec.clear)
		if err != nil {
			t.Fatal(err)
		}
		v.Frames = append(v.Frames, vFrame{Key: enc(aliceKey), KeyID: v.Keys[0].KeyID, Clear: spec.clear, Plain: hex.EncodeToString(spec.plain), Sealed: hex.EncodeToString(sealed)})
		v.Media.Frames = append(v.Media.Frames, hex.EncodeToString(sealed))
	}
	for _, m := range fs.Flush() {
		v.Media.Auth = append(v.Media.Auth, hex.EncodeToString(m))
	}
	var forger FrameSender
	forger.SetKey(FrameKey{Call: vectorCall, Peer: ids["alice"].peer, Epoch: 1, KeyID: byte(v.Keys[0].KeyID), Key: aliceKey, AuthSeed: randBytes(32)})
	forger.Seal([]byte("not alice"), 1)
	forged, _ := forger.Seal([]byte("not alice either"), 1)
	v.Media.Forged = hex.EncodeToString(forged)
	v.Media.Rejects = generateAuthRejects(t, v, ids)
	for _, e := range EmojiTable {
		v.Emoji = append(v.Emoji, vEmoji{Text: e.Text, Name: e.Name})
	}
	v.Rejects = generateRejects(t, now, ids, pres, v)
	return v
}

func generateRejects(t testing.TB, now int64, ids map[string]*genIdentity, pres map[string][]*genPresence, v *vectorFile) []vReject {
	alice, bob, carol := ids["alice"], ids["bob"], ids["carol"]
	ap := pres["alice"][0].p
	bp := pres["bob"][0].p
	var out []vReject
	add := func(what, name, as string, before [][]byte, msg []byte, code string) {
		r := vReject{What: what, Case: name, As: as, Message: msg, Error: code}
		for _, b := range before {
			r.Before = append(r.Before, b)
		}
		out = append(out, r)
	}
	p := clonePresence(ap)
	p.Sig[5] ^= 1
	add("presence", "bad-signature", "bob", nil, p.Marshal(), "signature")
	p = clonePresence(ap)
	p.Call = "chat:another-room"
	add("presence", "wrong-call", "bob", nil, resignPresence(p, alice.key), "call")
	add("presence", "classical-only", "bob", nil, patchJSON(t, ap.Marshal(), "pq", nil), "invalid")
	short := clonePresence(ap)
	add("presence", "short-pq", "bob", nil, patchJSON(t, short.Marshal(), "pq", enc(short.PQ[:PQKeySize-1])), "invalid")
	add("presence", "kid-mismatch", "bob", nil, patchJSON(t, ap.Marshal(), "kid", KeyID(bp.IDK)), "invalid")
	add("presence", "quoted-epoch", "bob", nil, patchJSON(t, ap.Marshal(), "epoch", "1"), "invalid")
	add("presence", "version-1", "bob", nil, patchJSON(t, ap.Marshal(), "v", 1), "invalid")
	add("presence", "version-3", "bob", nil, patchJSON(t, ap.Marshal(), "v", 3), "invalid")
	p = clonePresence(ap)
	p.TS = now - (6 * time.Minute).Milliseconds()
	add("presence", "old-timestamp", "bob", nil, resignPresence(p, alice.key), "clock")
	newer := clonePresence(ap)
	newer.Epoch = 2
	add("presence", "stale-epoch", "bob", [][]byte{resignPresence(newer, alice.key)}, ap.Marshal(), "stale")
	p = clonePresence(ap)
	p.X = pres["bob"][0].p.X
	add("presence", "equivocation", "bob", [][]byte{ap.Marshal()}, resignPresence(p, alice.key), "equivocation")
	p = clonePresence(bp)
	p.Peer = alice.peer
	add("presence", "peer-taken", "carol", [][]byte{ap.Marshal()}, resignPresence(p, bob.key), "peer-bound")
	p = clonePresence(ap)
	p.Peer = bob.peer
	add("presence", "own-peer", "bob", nil, resignPresence(p, alice.key), "peer-bound")

	first, err := ParseKeyMessage(v.Keys[0].Message)
	if err != nil {
		t.Fatal(err)
	}
	m := cloneKeys(first)
	m.Sig[0] ^= 1
	add("keys", "bad-signature", "bob", [][]byte{ap.Marshal()}, m.Marshal(), "signature")
	add("keys", "unknown-sender", "bob", nil, first.Marshal(), "unknown-peer")
	m = cloneKeys(first)
	m.Keys = m.Keys[1:]
	add("keys", "not-for-me", "bob", [][]byte{ap.Marshal()}, resignKeys(m, alice.key), "not-for-me")
	m = cloneKeys(first)
	m.Keys[0].Key[3] ^= 1
	add("keys", "tampered-wrap", "bob", [][]byte{ap.Marshal()}, resignKeys(m, alice.key), "invalid")
	m = cloneKeys(first)
	m.Keys[0].CT = m.Keys[1].CT
	add("keys", "swapped-ciphertext", "bob", [][]byte{ap.Marshal()}, resignKeys(m, alice.key), "invalid")
	m = cloneKeys(first)
	m.Keys[0].Epoch = 9
	add("keys", "retired-recipient-epoch", "bob", [][]byte{ap.Marshal()}, resignKeys(m, alice.key), "recipient-epoch")
	m = cloneKeys(first)
	m.Keys[0], m.Keys[1] = m.Keys[1], m.Keys[0]
	add("keys", "unsorted", "bob", [][]byte{ap.Marshal()}, resignKeys(m, alice.key), "invalid")
	m = cloneKeys(first)
	m.Keys[1].Peer = alice.peer
	add("keys", "sender-as-recipient", "bob", [][]byte{ap.Marshal()}, resignKeys(m, alice.key), "invalid")
	m = cloneKeys(first)
	m.IDK = carol.key.Public().(ed25519.PublicKey)
	add("keys", "other-identity", "bob", [][]byte{ap.Marshal()}, resignKeys(m, carol.key), "peer-bound")
	m = cloneKeys(first)
	m.Peer = bob.peer
	add("keys", "own-peer", "bob", [][]byte{ap.Marshal()}, resignKeys(m, alice.key), "invalid")
	add("keys", "no-auth-key", "bob", [][]byte{ap.Marshal()}, patchJSON(t, first.Marshal(), "ak", nil), "invalid")
	add("keys", "version-1", "bob", [][]byte{ap.Marshal()}, patchJSON(t, first.Marshal(), "v", 1), "invalid")
	add("keys", "auth-key-unsigned", "bob", [][]byte{ap.Marshal()}, patchJSON(t, first.Marshal(), "ak", enc(bp.IDK)), "signature")
	return append(out, generateRoundRejects(t, now, ids, ap)...)
}

func generateRoundRejects(t testing.TB, now int64, ids map[string]*genIdentity, ap *Presence) []vReject {
	alice, carol := ids["alice"], ids["carol"]
	var out []vReject
	add := func(what, name string, before, commits [][]byte, msg []byte, code string) {
		r := vReject{What: what, Case: name, As: "bob", Message: msg, Error: code, Before: []json.RawMessage{}, BeforeCommits: []json.RawMessage{}}
		for _, b := range before {
			r.Before = append(r.Before, b)
		}
		for _, b := range commits {
			r.BeforeCommits = append(r.BeforeCommits, b)
		}
		out = append(out, r)
	}
	pa := [][]byte{ap.Marshal()}
	round := bytes.Repeat([]byte{0x42}, RoundSize)
	nonce := bytes.Repeat([]byte{0x24}, NonceSize)
	base := &Commit{Call: vectorCall, Peer: alice.peer, IDK: alice.key.Public().(ed25519.PublicKey), Round: round, Gen: 3, TS: now}
	base.C = commitHash(base.Call, base.Peer, base.IDK, base.Round, base.Gen, nonce)
	good := resignCommit(base, alice.key)
	cl := func() *Commit { c := *base; c.Sig = nil; return &c }
	c := cl()
	c.Sig = ed25519.Sign(alice.key, c.SignedMessage())
	c.Sig[9] ^= 1
	add("commit", "bad-signature", pa, nil, c.Marshal(), "signature")
	add("commit", "unknown-sender", nil, nil, good, "unknown-peer")
	c = cl()
	c.Call = "chat:another-room"
	add("commit", "wrong-call", pa, nil, resignCommit(c, alice.key), "call")
	add("commit", "version-1", pa, nil, patchJSON(t, good, "v", 1), "invalid")
	add("commit", "reveal-type", pa, nil, patchJSON(t, good, "t", "reveal"), "invalid")
	add("commit", "quoted-gen", pa, nil, patchJSON(t, good, "gen", "3"), "invalid")
	add("commit", "short-round", pa, nil, patchJSON(t, good, "round", enc(round[:31])), "invalid")
	c = cl()
	c.TS = now - (6 * time.Minute).Milliseconds()
	add("commit", "old-timestamp", pa, nil, resignCommit(c, alice.key), "clock")
	c = cl()
	c.IDK = carol.key.Public().(ed25519.PublicKey)
	add("commit", "other-identity", pa, nil, resignCommit(c, carol.key), "peer-bound")
	c = cl()
	c.C = commitHash(c.Call, c.Peer, c.IDK, c.Round, c.Gen, round)
	add("commit", "equivocation", pa, [][]byte{good}, resignCommit(c, alice.key), "equivocation")
	add("commit", "duplicate", pa, [][]byte{good}, good, "duplicate")
	c = cl()
	c.Gen = 2
	add("commit", "older-gen", pa, [][]byte{good}, resignCommit(c, alice.key), "stale")
	c = cl()
	c.Gen = MaxGenJump + 1
	add("commit", "gen-jump", pa, nil, resignCommit(c, alice.key), "invalid")
	rv := &Reveal{Call: vectorCall, Peer: alice.peer, Round: round, Gen: 3, Nonce: nonce}
	add("reveal", "unknown-sender", nil, nil, rv.Marshal(), "unknown-peer")
	add("reveal", "before-commit", pa, nil, rv.Marshal(), "waiting")
	add("reveal", "version-1", pa, [][]byte{good}, patchJSON(t, rv.Marshal(), "v", 1), "invalid")
	add("reveal", "commit-type", pa, [][]byte{good}, patchJSON(t, rv.Marshal(), "t", "commit"), "invalid")
	add("reveal", "wrong-nonce", pa, [][]byte{good}, (&Reveal{Call: vectorCall, Peer: alice.peer, Round: round, Gen: 3, Nonce: round}).Marshal(), "commitment")
	add("reveal", "wrong-round", pa, [][]byte{good}, (&Reveal{Call: vectorCall, Peer: alice.peer, Round: nonce, Gen: 3, Nonce: nonce}).Marshal(), "commitment")
	add("reveal", "older-gen", pa, [][]byte{good}, (&Reveal{Call: vectorCall, Peer: alice.peer, Round: round, Gen: 2, Nonce: nonce}).Marshal(), "stale")
	add("reveal", "own-peer", pa, nil, (&Reveal{Call: vectorCall, Peer: ids["bob"].peer, Round: round, Gen: 3, Nonce: nonce}).Marshal(), "own")
	return out
}

func makeSAS(t testing.TB, ids map[string]*genIdentity, names []string, gen uint32, nonces [][]byte) vSAS {
	var members []Member
	out := vSAS{Members: names, Gen: gen}
	for i, n := range names {
		id := ids[n]
		nonce := randBytes(NonceSize)
		if nonces != nil {
			nonce = nonces[i]
		}
		members = append(members, Member{IDK: id.key.Public().(ed25519.PublicKey), Acct: id.acct, Peer: id.peer, Nonce: nonce})
		out.Nonces = append(out.Nonces, enc(nonce))
	}
	code, err := ComputeSAS(vectorCall, gen, members)
	if err != nil {
		t.Fatal(err)
	}
	out.Round, out.Transcript = enc(code.Round), enc(code.Transcript)
	out.Emoji, out.EmojiText, out.Digits, out.QR = code.Emoji[:], code.EmojiText(), code.Digits, code.QR
	return out
}

func generateRound(t testing.TB, now int64, ids map[string]*genIdentity) vRound {
	names := []string{"alice", "bob", "carol"}
	var nonces [][]byte
	for range names {
		nonces = append(nonces, randBytes(NonceSize))
	}
	r := vRound{SAS: makeSAS(t, ids, names, 1, nonces)}
	round := mustDec(t, r.SAS.Round)
	for i, n := range names {
		id := ids[n]
		c := &Commit{Call: vectorCall, Peer: id.peer, IDK: id.key.Public().(ed25519.PublicKey), Round: round, Gen: 1, TS: now}
		c.C = commitHash(c.Call, c.Peer, c.IDK, c.Round, c.Gen, nonces[i])
		rv := &Reveal{Call: vectorCall, Peer: id.peer, Round: round, Gen: 1, Nonce: nonces[i]}
		r.Commits = append(r.Commits, vCommit{Name: n, Commit: resignCommit(c, id.key), Reveal: rv.Marshal()})
	}
	return r
}

func generateAuthRejects(t testing.TB, v *vectorFile, ids map[string]*genIdentity) []vAuthReject {
	good := mustHex(t, v.Media.Auth[0])
	m, err := ParseAuthMessage(good)
	if err != nil {
		t.Fatal(err)
	}
	var out []vAuthReject
	add := func(name string, b []byte, code string) {
		out = append(out, vAuthReject{Case: name, Message: hex.EncodeToString(b), Error: code})
	}
	bad := append([]byte(nil), good...)
	bad[len(bad)-3] ^= 1
	add("bad-signature", bad, "signature")
	bad = append([]byte(nil), good...)
	bad[AuthHeaderSize] ^= 1
	add("tampered-digest", bad, "signature")
	other := *m
	other.Sign(vectorCall, ids["bob"].peer, ed25519.NewKeyFromSeed(mustDec(t, v.Keys[0].AuthSeed)))
	add("signed-for-another-peer", other.Marshal(), "signature")
	stranger := *m
	stranger.Sign(vectorCall, ids["alice"].peer, ed25519.NewKeyFromSeed(bytes.Repeat([]byte{3}, 32)))
	add("other-auth-key", stranger.Marshal(), "signature")
	add("truncated", good[:len(good)-1], "invalid")
	bad = append([]byte(nil), good...)
	bad[0] = 1<<4 | 1
	add("version-1", bad, "invalid")
	bad = append([]byte(nil), good...)
	bad[14] = 0
	add("zero-count", bad, "invalid")
	bad = append([]byte(nil), good...)
	bad[1] ^= 1
	add("key-id-not-epoch", bad, "invalid")
	e2 := *m
	e2.Epoch, e2.KeyID = 2, 2
	e2.Sign(vectorCall, ids["alice"].peer, ed25519.NewKeyFromSeed(mustDec(t, v.Keys[0].AuthSeed)))
	add("unknown-key", e2.Marshal(), "unknown-peer")
	e3 := *m
	e3.Epoch = 257
	e3.Sign(vectorCall, ids["alice"].peer, ed25519.NewKeyFromSeed(mustDec(t, v.Keys[0].AuthSeed)))
	add("other-epoch-same-key-id", e3.Marshal(), "stale")
	add("duplicate", good, "duplicate")
	return out
}

func mustHex(t testing.TB, s string) []byte {
	t.Helper()
	b, err := hex.DecodeString(s)
	if err != nil {
		t.Fatal(err)
	}
	return b
}

type queueReader struct{ b []byte }

func (q *queueReader) Read(p []byte) (int, error) {
	if len(q.b) < len(p) {
		return 0, os.ErrClosed
	}
	n := copy(p, q.b)
	q.b = q.b[n:]
	return n, nil
}

func sessionFor(t testing.TB, v *vectorFile, name string, extra ...[]byte) *Session {
	t.Helper()
	id := v.Identities[name]
	var stream []byte
	epochs := 0
	for _, p := range v.Presences {
		if p.Name == name {
			stream = append(stream, mustDec(t, p.X25519)...)
			stream = append(stream, mustDec(t, p.MLKEMSeed)...)
			epochs++
		}
	}
	for _, b := range extra {
		stream = append(stream, b...)
	}
	now := time.UnixMilli(v.Now)
	s, err := NewSession(Config{Call: v.Call, Peer: id.Peer, Acct: id.Acct, Identity: ed25519.NewKeyFromSeed(mustDec(t, id.Seed)),
		Rand: &queueReader{stream}, Now: func() time.Time { return now }})
	if err != nil {
		t.Fatalf("session for %s: %v", name, err)
	}
	for i := 1; i < epochs; i++ {
		if _, err := s.Rotate(); err != nil {
			t.Fatal(err)
		}
	}
	return s
}

func checkVectors(t *testing.T, v *vectorFile) {
	if v.Call == "" || len(v.Identities) < 3 || len(v.Keys) == 0 || len(v.SAS) == 0 || len(v.Round.Commits) == 0 || len(v.Frames) == 0 || len(v.Media.Frames) == 0 || len(v.Rejects) == 0 {
		t.Fatalf("incomplete vectors from %s", v.Generator)
	}
	byPeer := map[string]string{}
	for name, id := range v.Identities {
		key := ed25519.NewKeyFromSeed(mustDec(t, id.Seed))
		if enc(key.Public().(ed25519.PublicKey)) != id.IDK {
			t.Fatalf("%s: idk does not match seed", name)
		}
		byPeer[id.Peer] = name
	}
	type secrets struct {
		x  *ecdh.PrivateKey
		dk *mlkem.DecapsulationKey768
		p  *Presence
	}
	held := map[string]map[uint32]secrets{}
	for _, vp := range v.Presences {
		p, err := ParsePresence(vp.Message)
		if err != nil {
			t.Fatalf("presence %s/%d: %v", vp.Name, vp.Epoch, err)
		}
		if err := p.Verify(); err != nil {
			t.Fatalf("presence %s/%d: %v", vp.Name, vp.Epoch, err)
		}
		if !bytes.Equal(p.Marshal(), compact(t, vp.Message)) {
			t.Fatalf("presence %s/%d does not re-encode identically:\n%s\n%s", vp.Name, vp.Epoch, p.Marshal(), vp.Message)
		}
		x, _ := ecdh.X25519().NewPrivateKey(mustDec(t, vp.X25519))
		dk, _ := mlkem.NewDecapsulationKey768(mustDec(t, vp.MLKEMSeed))
		if !bytes.Equal(x.PublicKey().Bytes(), p.X) || !bytes.Equal(dk.EncapsulationKey().Bytes(), p.PQ) {
			t.Fatalf("presence %s/%d keys do not match secrets", vp.Name, vp.Epoch)
		}
		if held[vp.Name] == nil {
			held[vp.Name] = map[uint32]secrets{}
		}
		held[vp.Name][p.Epoch] = secrets{x, dk, p}
	}
	for _, vk := range v.Keys {
		m, err := ParseKeyMessage(vk.Message)
		if err != nil {
			t.Fatalf("keys from %s: %v", vk.Sender, err)
		}
		if err := m.Verify(); err != nil {
			t.Fatalf("keys from %s: %v", vk.Sender, err)
		}
		if !bytes.Equal(m.Marshal(), compact(t, vk.Message)) {
			t.Fatalf("keys from %s do not re-encode identically", vk.Sender)
		}
		eph, _ := ecdh.X25519().NewPrivateKey(mustDec(t, vk.EphPrivate))
		if !bytes.Equal(eph.PublicKey().Bytes(), m.Eph) {
			t.Fatal("eph does not match its private key")
		}
		sender := Sender{Peer: m.Peer, IDK: m.IDK, Epoch: m.Epoch}
		fk, err := DeriveFrameKey(v.Call, sender, mustDec(t, vk.Secret))
		if err != nil || enc(fk.Key) != vk.FrameKey || int(fk.KeyID) != vk.KeyID {
			t.Fatalf("frame key from %s/%d differs", vk.Sender, m.Epoch)
		}
		if !bytes.Equal(ed25519.NewKeyFromSeed(mustDec(t, vk.AuthSeed)).Public().(ed25519.PublicKey), m.Auth) {
			t.Fatalf("auth key from %s/%d does not match its seed", vk.Sender, m.Epoch)
		}
		if len(vk.Wraps) != len(m.Keys) {
			t.Fatal("wrap count differs")
		}
		for i, w := range m.Keys {
			tr := vk.Wraps[i]
			sec := held[tr.Recipient][w.Epoch]
			if sec.p == nil || byPeer[w.Peer] != tr.Recipient || tr.PresenceEpoch != w.Epoch {
				t.Fatalf("wrap %d names an unknown presence", i)
			}
			pub, _ := ecdh.X25519().NewPublicKey(m.Eph)
			ss1, _ := sec.x.ECDH(pub)
			rpub, _ := ecdh.X25519().NewPublicKey(sec.p.X)
			ss1b, _ := eph.ECDH(rpub)
			ss2, err := sec.dk.Decapsulate(w.CT)
			if err != nil || enc(ss1) != tr.SS1 || enc(ss1b) != tr.SS1 || enc(ss2) != tr.SS2 {
				t.Fatalf("wrap %d to %s: shared secrets differ", i, tr.Recipient)
			}
			info := wrapInfo(v.Call, sender, sec.p, m.Eph, w.CT)
			if info != tr.Info {
				t.Fatalf("wrap %d info differs:\n%s\n%s", i, info, tr.Info)
			}
			kek, nonce, _ := kekFrom(v.Call, info, ss1, ss2)
			if enc(kek) != tr.KEK || enc(nonce) != tr.Nonce {
				t.Fatalf("wrap %d kek differs", i)
			}
			secret, err := unwrapSecret(v.Call, sender, m.Eph, w, sec.p, sec.x, sec.dk)
			if err != nil || enc(secret) != vk.Secret {
				t.Fatalf("wrap %d does not unwrap: %v", i, err)
			}
			s := sessionFor(t, v, tr.Recipient)
			senderPresence := held[vk.Sender]
			var newest *Presence
			for _, sp := range senderPresence {
				if newest == nil || sp.p.Epoch > newest.Epoch {
					newest = sp.p
				}
			}
			if _, err := s.AcceptPresence(newest.Marshal()); err != nil {
				t.Fatalf("session %s accepts %s: %v", tr.Recipient, vk.Sender, err)
			}
			got, err := s.OpenKeys(vk.Message)
			if err != nil || enc(got.Key) != vk.FrameKey || got.Peer != m.Peer || got.Epoch != m.Epoch || !bytes.Equal(got.Auth, m.Auth) || got.Call != v.Call {
				t.Fatalf("session %s opens keys from %s: %v", tr.Recipient, vk.Sender, err)
			}
			if _, err := s.OpenKeys(vk.Message); ErrorCode(err) != "duplicate" {
				t.Fatalf("replayed keys: %v", err)
			}
		}
	}
	checkRound(t, v)
	for _, c := range v.SAS {
		checkSAS(t, v, c)
	}
	if len(v.Emoji) != len(EmojiTable) {
		t.Fatal("emoji table size differs")
	}
	for i, e := range v.Emoji {
		if e.Text != EmojiTable[i].Text || e.Name != EmojiTable[i].Name {
			t.Fatalf("emoji %d differs: %q %q", i, e.Text, e.Name)
		}
	}
	checkMedia(t, v)
	kinds := map[string]bool{}
	for _, r := range v.Rejects {
		kinds[r.What] = true
		s := sessionFor(t, v, r.As)
		for _, b := range r.Before {
			if _, err := s.AcceptPresence(b); err != nil {
				t.Fatalf("reject %s/%s: before: %v", r.What, r.Case, err)
			}
		}
		for _, b := range r.BeforeCommits {
			if err := s.AcceptCommit(b); err != nil {
				t.Fatalf("reject %s/%s: before commit: %v", r.What, r.Case, err)
			}
		}
		var err error
		switch r.What {
		case "presence":
			_, err = s.AcceptPresence(r.Message)
		case "keys":
			_, err = s.OpenKeys(r.Message)
		case "commit":
			err = s.AcceptCommit(r.Message)
		case "reveal":
			err = s.AcceptReveal(r.Message)
		default:
			t.Fatalf("unknown reject kind %q", r.What)
		}
		if ErrorCode(err) != r.Error {
			t.Fatalf("reject %s/%s: got %q (%v) want %q", r.What, r.Case, ErrorCode(err), err, r.Error)
		}
	}
	if len(kinds) != 4 {
		t.Fatalf("reject kinds: %v", kinds)
	}
}

func membersOf(t testing.TB, v *vectorFile, names []string, nonces []string) []Member {
	var out []Member
	for i, n := range names {
		id := v.Identities[n]
		m := Member{IDK: mustDec(t, id.IDK), Acct: id.Acct, Peer: id.Peer}
		if nonces != nil {
			m.Nonce = mustDec(t, nonces[i])
		}
		out = append(out, m)
	}
	return out
}

func checkSAS(t *testing.T, v *vectorFile, c vSAS) {
	t.Helper()
	members := membersOf(t, v, c.Members, c.Nonces)
	round, err := RoundID(v.Call, c.Gen, members)
	if err != nil || enc(round) != c.Round {
		t.Fatalf("round for %v differs", c.Members)
	}
	code, err := ComputeSAS(v.Call, c.Gen, members)
	if err != nil || enc(code.Transcript) != c.Transcript || code.Digits != c.Digits || code.QR != c.QR || code.EmojiText() != c.EmojiText {
		t.Fatalf("code for %v differs: %+v", c.Members, code)
	}
	for i, e := range c.Emoji {
		if code.Emoji[i] != e {
			t.Fatalf("emoji %d for %v differs", i, c.Members)
		}
	}
	if q, err := ParseQR(c.QR); err != nil || enc(q) != c.Transcript {
		t.Fatal("QR does not decode to the transcript")
	}
}

func checkRound(t *testing.T, v *vectorFile) {
	t.Helper()
	r := v.Round
	checkSAS(t, v, r.SAS)
	if len(r.Commits) != len(r.SAS.Members) {
		t.Fatal("round has the wrong number of commits")
	}
	for i, vc := range r.Commits {
		c, err := ParseCommit(vc.Commit)
		if err != nil || c.Verify() != nil {
			t.Fatalf("commit from %s: %v", vc.Name, err)
		}
		if !bytes.Equal(c.Marshal(), compact(t, vc.Commit)) {
			t.Fatalf("commit from %s does not re-encode identically", vc.Name)
		}
		rv, err := ParseReveal(vc.Reveal)
		if err != nil || !bytes.Equal(rv.Marshal(), compact(t, vc.Reveal)) {
			t.Fatalf("reveal from %s: %v", vc.Name, err)
		}
		if enc(c.Round) != r.SAS.Round || c.Gen != r.SAS.Gen || !c.Opens(rv.Nonce) || enc(rv.Nonce) != r.SAS.Nonces[i] || c.Opens(mustDec(t, r.SAS.Round)) {
			t.Fatalf("commit from %s does not open to its nonce", vc.Name)
		}
	}
	me := r.Commits[0].Name
	s := sessionFor(t, v, me, mustDec(t, r.SAS.Nonces[0]))
	for _, name := range r.SAS.Members[1:] {
		var newest json.RawMessage
		var epoch uint32
		for _, p := range v.Presences {
			if p.Name == name && p.Epoch > epoch {
				newest, epoch = p.Message, p.Epoch
			}
		}
		if _, err := s.AcceptPresence(newest); err != nil {
			t.Fatalf("round session accepts %s: %v", name, err)
		}
	}
	own, err := s.Commit()
	if err != nil || !bytes.Equal(own, compact(t, r.Commits[0].Commit)) {
		t.Fatalf("session commit differs from the vector:\n%s\n%s", own, r.Commits[0].Commit)
	}
	for _, vc := range r.Commits[1:] {
		if err := s.AcceptCommit(vc.Commit); err != nil {
			t.Fatalf("session accepts commit from %s: %v", vc.Name, err)
		}
	}
	reveal, err := s.Reveal()
	if err != nil || !bytes.Equal(reveal, compact(t, r.Commits[0].Reveal)) {
		t.Fatalf("session reveal differs: %v", err)
	}
	for _, vc := range r.Commits[1:] {
		if err := s.AcceptReveal(vc.Reveal); err != nil {
			t.Fatalf("session accepts reveal from %s: %v", vc.Name, err)
		}
	}
	st := s.Verification()
	if st.Code == nil || enc(st.Code.Transcript) != r.SAS.Transcript || st.Code.Digits != r.SAS.Digits {
		t.Fatalf("session code differs: %+v", st)
	}
}

func checkMedia(t *testing.T, v *vectorFile) {
	t.Helper()
	md := v.Media
	vk := v.Keys[md.Key]
	m, err := ParseKeyMessage(vk.Message)
	if err != nil {
		t.Fatal(err)
	}
	key := FrameKey{Call: v.Call, Peer: m.Peer, Epoch: m.Epoch, KeyID: byte(vk.KeyID), Key: mustDec(t, vk.FrameKey), Auth: m.Auth}
	var frames [][]byte
	for _, f := range md.Frames {
		frames = append(frames, mustHex(t, f))
	}
	if len(md.Auth) != 1 || len(frames) < 3 {
		t.Fatal("media vectors are incomplete")
	}
	first, _ := ParseFrameTrailer(frames[0])
	want := &AuthMessage{KeyID: key.KeyID, Epoch: key.Epoch, First: first.Counter}
	for _, f := range frames {
		want.Digests = append(want.Digests, FrameDigest(f)...)
	}
	want.Sign(v.Call, key.Peer, ed25519.NewKeyFromSeed(mustDec(t, vk.AuthSeed)))
	if hex.EncodeToString(want.Marshal()) != md.Auth[0] {
		t.Fatalf("auth message differs:\n%x\n%s", want.Marshal(), md.Auth[0])
	}
	clock := time.UnixMilli(v.Now)
	var r FrameReceiver
	r.Now = func() time.Time { return clock }
	if err := r.SetKey(key); err != nil {
		t.Fatal(err)
	}
	for i, f := range frames {
		if _, err := r.Open(key.Peer, f, v.Frames[i].Clear); err != nil {
			t.Fatalf("media frame %d: %v", i, err)
		}
	}
	if err := r.AcceptAuth(key.Peer, mustHex(t, md.Auth[0])); err != nil {
		t.Fatal(err)
	}
	if st := r.Status(key.Peer); st.State != AuthVerified || st.Verified != uint64(len(frames)) || st.Forged != 0 {
		t.Fatalf("media status: %+v", st)
	}
	for _, rj := range md.Rejects {
		if err := r.AcceptAuth(key.Peer, mustHex(t, rj.Message)); ErrorCode(err) != rj.Error {
			t.Fatalf("auth reject %s: got %q (%v) want %q", rj.Case, ErrorCode(err), err, rj.Error)
		}
	}
	var forged FrameReceiver
	forged.Now = r.Now
	forged.SetKey(key)
	bad := mustHex(t, md.Forged)
	if _, err := forged.Open(key.Peer, bad, 1); err != nil {
		t.Fatalf("forged frame should pass its tag: %v", err)
	}
	forged.AcceptAuth(key.Peer, mustHex(t, md.Auth[0]))
	if st := forged.Status(key.Peer); st.State != AuthForged || st.Forged != 1 || !st.Dropping {
		t.Fatalf("forged frame not caught: %+v", st)
	}
	if _, err := forged.Open(key.Peer, frames[0], v.Frames[0].Clear); ErrorCode(err) != "forged" {
		t.Fatalf("stream not dropped after a forgery: %v", err)
	}
}

func compact(t testing.TB, b []byte) []byte {
	t.Helper()
	var out bytes.Buffer
	if err := json.Compact(&out, b); err != nil {
		t.Fatal(err)
	}
	return out.Bytes()
}

func loadVectors(t *testing.T, path string) *vectorFile {
	t.Helper()
	b, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	var v vectorFile
	if err := json.Unmarshal(b, &v); err != nil {
		t.Fatal(err)
	}
	return &v
}

func writeVectors(t *testing.T, path string, v *vectorFile) {
	t.Helper()
	b, err := json.MarshalIndent(v, "", " ")
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, append(b, '\n'), 0o644); err != nil {
		t.Fatal(err)
	}
}

func TestGoVectors(t *testing.T) {
	fresh := generateVectors(t)
	checkVectors(t, fresh)
	if out := os.Getenv("E2EE_WRITE_GO_VECTORS"); out != "" {
		writeVectors(t, out, fresh)
	}
	path := filepath.Join("testdata", "go-vectors.json")
	if *updateVectors {
		writeVectors(t, path, fresh)
	}
	stored := loadVectors(t, path)
	if stored.Generator != "go" {
		t.Fatal("go-vectors.json was not written by Go")
	}
	checkVectors(t, stored)
}

func TestJSVectors(t *testing.T) {
	path := os.Getenv("E2EE_JS_VECTORS")
	if path == "" {
		path = filepath.Join("testdata", "js-vectors.json")
	}
	v := loadVectors(t, path)
	if v.Generator != "js" {
		t.Fatalf("%s was not written by the JS implementation", path)
	}
	checkVectors(t, v)
}

type fips203 struct {
	KeyGen []struct {
		TcID     int    `json:"tcId"`
		D        string `json:"d"`
		Z        string `json:"z"`
		EkSha256 string `json:"ekSha256"`
	} `json:"keyGen"`
	EncapsulationKeyCheck []struct {
		TcID       int    `json:"tcId"`
		Ek         string `json:"ek"`
		TestPassed bool   `json:"testPassed"`
	} `json:"encapsulationKeyCheck"`
}

func TestFIPS203KeyGenMatchesGo(t *testing.T) {
	b, err := os.ReadFile(filepath.Join("testdata", "fips203-mlkem768.json"))
	if err != nil {
		t.Fatal(err)
	}
	var f fips203
	if err := json.Unmarshal(b, &f); err != nil {
		t.Fatal(err)
	}
	if len(f.KeyGen) != 25 || len(f.EncapsulationKeyCheck) != 10 {
		t.Fatal("unexpected FIPS 203 fixture size")
	}
	for _, tc := range f.KeyGen {
		d, _ := hex.DecodeString(tc.D)
		z, _ := hex.DecodeString(tc.Z)
		dk, err := mlkem.NewDecapsulationKey768(append(d, z...))
		if err != nil {
			t.Fatal(err)
		}
		sum := sha256.Sum256(dk.EncapsulationKey().Bytes())
		if hex.EncodeToString(sum[:]) != lower(tc.EkSha256) {
			t.Fatalf("tcId %d: ek differs", tc.TcID)
		}
	}
	for _, tc := range f.EncapsulationKeyCheck {
		ek, _ := hex.DecodeString(tc.Ek)
		_, err := mlkem.NewEncapsulationKey768(ek)
		if (err == nil) != tc.TestPassed {
			t.Fatalf("tcId %d: accepted=%v want %v", tc.TcID, err == nil, tc.TestPassed)
		}
	}
}

func lower(s string) string {
	b := []byte(s)
	for i, c := range b {
		if c >= 'A' && c <= 'Z' {
			b[i] = c + 32
		}
	}
	return string(b)
}

func TestHKDFMatchesRFC5869(t *testing.T) {
	ikm, _ := hex.DecodeString("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b")
	salt, _ := hex.DecodeString("000102030405060708090a0b0c")
	info, _ := hex.DecodeString("f0f1f2f3f4f5f6f7f8f9")
	okm, err := hkdf.Key(sha256.New, ikm, salt, string(info), 42)
	if err != nil || hex.EncodeToString(okm) != "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865" {
		t.Fatal("HKDF-SHA256 does not match RFC 5869 case 1")
	}
}
