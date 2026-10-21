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
	Message    json.RawMessage `json:"message"`
	Wraps      []vWrap         `json:"wraps"`
}

type vCode struct {
	Members []string `json:"members"`
	Code    string   `json:"code"`
}

type vFrame struct {
	Key    string `json:"key"`
	KeyID  int    `json:"keyId"`
	Clear  int    `json:"clear"`
	Plain  string `json:"plain"`
	Sealed string `json:"sealed"`
}

type vReject struct {
	What    string            `json:"what"`
	Case    string            `json:"case"`
	As      string            `json:"as"`
	Before  []json.RawMessage `json:"before"`
	Message json.RawMessage   `json:"message"`
	Error   string            `json:"error"`
}

type vectorFile struct {
	Generator  string               `json:"generator"`
	Call       string               `json:"call"`
	Now        int64                `json:"now"`
	Identities map[string]vIdentity `json:"identities"`
	Presences  []vPresence          `json:"presences"`
	Keys       []vKeys              `json:"keys"`
	Codes      []vCode              `json:"codes"`
	Frames     []vFrame             `json:"frames"`
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

func makeKeyMessage(call string, who *genIdentity, epoch uint32, ts int64, ephPriv, secret []byte, to []*Presence) (*KeyMessage, []*wrapTrace) {
	eph, _ := ecdh.X25519().NewPrivateKey(ephPriv)
	m := &KeyMessage{Call: call, Peer: who.peer, IDK: who.key.Public().(ed25519.PublicKey), Epoch: epoch, TS: ts, Eph: eph.PublicKey().Bytes()}
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
		secret, ephPriv := randBytes(32), randBytes(32)
		m, traces := makeKeyMessage(vectorCall, who, spec.epoch, now, ephPriv, secret, spec.to)
		fk, err := DeriveFrameKey(vectorCall, Sender{Peer: who.peer, IDK: m.IDK, Epoch: spec.epoch}, secret)
		if err != nil {
			t.Fatal(err)
		}
		entry := vKeys{Sender: spec.sender, Secret: enc(secret), FrameKey: enc(fk.Key), KeyID: int(fk.KeyID), EphPrivate: enc(ephPriv), Message: m.Marshal()}
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
	for _, set := range [][]string{{"alice", "bob", "carol"}, {"alice", "bob"}, {"bob", "carol"}, {"carol", "alice", "bob"}} {
		var members []Member
		for _, n := range set {
			members = append(members, Member{IDK: ids[n].key.Public().(ed25519.PublicKey), Acct: ids[n].acct})
		}
		code, err := VerificationCode(vectorCall, members)
		if err != nil {
			t.Fatal(err)
		}
		v.Codes = append(v.Codes, vCode{Members: set, Code: code})
	}
	aliceKey := mustDec(t, v.Keys[0].FrameKey)
	var fs FrameSender
	fs.SetKey(FrameKey{Peer: ids["alice"].peer, Epoch: 1, KeyID: byte(v.Keys[0].KeyID), Key: aliceKey})
	for _, spec := range []struct {
		clear int
		plain []byte
	}{{1, randBytes(60)}, {10, randBytes(400)}, {3, randBytes(120)}, {1, []byte{0xfc}}} {
		sealed, err := fs.Seal(spec.plain, spec.clear)
		if err != nil {
			t.Fatal(err)
		}
		v.Frames = append(v.Frames, vFrame{Key: enc(aliceKey), KeyID: v.Keys[0].KeyID, Clear: spec.clear, Plain: hex.EncodeToString(spec.plain), Sealed: hex.EncodeToString(sealed)})
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
	add("presence", "version-2", "bob", nil, patchJSON(t, ap.Marshal(), "v", 2), "invalid")
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
	return out
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

func sessionFor(t testing.TB, v *vectorFile, name string) *Session {
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
	if v.Call == "" || len(v.Identities) < 3 || len(v.Keys) == 0 || len(v.Codes) == 0 || len(v.Frames) == 0 || len(v.Rejects) == 0 {
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
			if err != nil || enc(got.Key) != vk.FrameKey || got.Peer != m.Peer || got.Epoch != m.Epoch {
				t.Fatalf("session %s opens keys from %s: %v", tr.Recipient, vk.Sender, err)
			}
			if _, err := s.OpenKeys(vk.Message); ErrorCode(err) != "duplicate" {
				t.Fatalf("replayed keys: %v", err)
			}
		}
	}
	for _, c := range v.Codes {
		var members []Member
		for _, n := range c.Members {
			id := v.Identities[n]
			members = append(members, Member{IDK: mustDec(t, id.IDK), Acct: id.Acct})
		}
		code, err := VerificationCode(v.Call, members)
		if err != nil || code != c.Code {
			t.Fatalf("code for %v: got %q want %q (%v)", c.Members, code, c.Code, err)
		}
	}
	var fs FrameSender
	var fr FrameReceiver
	for i, f := range v.Frames {
		key := FrameKey{Peer: "p", Epoch: 1, KeyID: byte(f.KeyID), Key: mustDec(t, f.Key)}
		fs.SetKey(key)
		fr.SetKey(key)
		plain, _ := hex.DecodeString(f.Plain)
		sealed, _ := hex.DecodeString(f.Sealed)
		got, err := fr.Open("p", sealed, f.Clear)
		if err != nil || !bytes.Equal(got, plain) {
			t.Fatalf("frame %d does not open: %v", i, err)
		}
		again, err := fs.Seal(plain, f.Clear)
		if err != nil || !bytes.Equal(again, sealed) {
			t.Fatalf("frame %d seals differently", i)
		}
	}
	for _, r := range v.Rejects {
		s := sessionFor(t, v, r.As)
		for _, b := range r.Before {
			if _, err := s.AcceptPresence(b); err != nil {
				t.Fatalf("reject %s/%s: before: %v", r.What, r.Case, err)
			}
		}
		var err error
		if r.What == "presence" {
			_, err = s.AcceptPresence(r.Message)
		} else {
			_, err = s.OpenKeys(r.Message)
		}
		if ErrorCode(err) != r.Error {
			t.Fatalf("reject %s/%s: got %q (%v) want %q", r.What, r.Case, ErrorCode(err), err, r.Error)
		}
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
