package e2ee

import (
	"bytes"
	"crypto/ed25519"
	"crypto/sha256"
	"crypto/subtle"
	"encoding/base32"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"sort"
	"strconv"
	"strings"
	"time"
)

const (
	NonceSize     = 32
	RoundSize     = 32
	SASEmoji      = 4
	RevealTimeout = 10 * time.Second
	RoundTimeout  = 30 * time.Second
	MaxUnfinished = 3
	MaxRounds     = 32
	MaxGenJump    = 1 << 16
	QRPrefix      = "FMCALL2:"
)

var qrEncoding = base32.StdEncoding.WithPadding(base32.NoPadding)

type Emoji struct {
	Text string
	Name string
}

var EmojiTable = func() [64]Emoji {
	raw := [64]struct {
		cp   []rune
		name string
	}{
		{[]rune{0x1F436}, "Dog"}, {[]rune{0x1F431}, "Cat"}, {[]rune{0x1F981}, "Lion"}, {[]rune{0x1F40E}, "Horse"},
		{[]rune{0x1F984}, "Unicorn"}, {[]rune{0x1F437}, "Pig"}, {[]rune{0x1F418}, "Elephant"}, {[]rune{0x1F430}, "Rabbit"},
		{[]rune{0x1F43C}, "Panda"}, {[]rune{0x1F413}, "Rooster"}, {[]rune{0x1F427}, "Penguin"}, {[]rune{0x1F422}, "Turtle"},
		{[]rune{0x1F41F}, "Fish"}, {[]rune{0x1F419}, "Octopus"}, {[]rune{0x1F98B}, "Butterfly"}, {[]rune{0x1F337}, "Flower"},
		{[]rune{0x1F333}, "Tree"}, {[]rune{0x1F335}, "Cactus"}, {[]rune{0x1F344}, "Mushroom"}, {[]rune{0x1F30F}, "Globe"},
		{[]rune{0x1F319}, "Moon"}, {[]rune{0x2601, 0xFE0F}, "Cloud"}, {[]rune{0x1F525}, "Fire"}, {[]rune{0x1F34C}, "Banana"},
		{[]rune{0x1F34E}, "Apple"}, {[]rune{0x1F353}, "Strawberry"}, {[]rune{0x1F33D}, "Corn"}, {[]rune{0x1F355}, "Pizza"},
		{[]rune{0x1F382}, "Cake"}, {[]rune{0x2764, 0xFE0F}, "Heart"}, {[]rune{0x1F600}, "Smiley"}, {[]rune{0x1F916}, "Robot"},
		{[]rune{0x1F3A9}, "Hat"}, {[]rune{0x1F453}, "Glasses"}, {[]rune{0x1F527}, "Spanner"}, {[]rune{0x1F385}, "Santa"},
		{[]rune{0x1F44D}, "Thumbs up"}, {[]rune{0x2602, 0xFE0F}, "Umbrella"}, {[]rune{0x231B}, "Hourglass"}, {[]rune{0x23F0}, "Clock"},
		{[]rune{0x1F381}, "Gift"}, {[]rune{0x1F4A1}, "Light bulb"}, {[]rune{0x1F4D5}, "Book"}, {[]rune{0x270F, 0xFE0F}, "Pencil"},
		{[]rune{0x1F4CE}, "Paperclip"}, {[]rune{0x2702, 0xFE0F}, "Scissors"}, {[]rune{0x1F512}, "Lock"}, {[]rune{0x1F511}, "Key"},
		{[]rune{0x1F528}, "Hammer"}, {[]rune{0x260E, 0xFE0F}, "Telephone"}, {[]rune{0x1F3C1}, "Flag"}, {[]rune{0x1F682}, "Train"},
		{[]rune{0x1F6B2}, "Bicycle"}, {[]rune{0x2708, 0xFE0F}, "Aeroplane"}, {[]rune{0x1F680}, "Rocket"}, {[]rune{0x1F3C6}, "Trophy"},
		{[]rune{0x26BD}, "Ball"}, {[]rune{0x1F3B8}, "Guitar"}, {[]rune{0x1F3BA}, "Trumpet"}, {[]rune{0x1F514}, "Bell"},
		{[]rune{0x2693}, "Anchor"}, {[]rune{0x1F3A7}, "Headphones"}, {[]rune{0x1F4C1}, "Folder"}, {[]rune{0x1F4CC}, "Pin"},
	}
	var out [64]Emoji
	for i, e := range raw {
		out[i] = Emoji{Text: string(e.cp), Name: e.name}
	}
	return out
}()

type Member struct {
	IDK   []byte
	Acct  string
	Peer  string
	Nonce []byte
}

func memberKey(idk []byte, acct string) string {
	return string(idk) + "\x00" + acct
}

func rosterEntries(members []Member) ([][]byte, []int, error) {
	if len(members) == 0 || len(members) > MaxParticipants {
		return nil, nil, ErrInvalid
	}
	entries := make([][]byte, len(members))
	order := make([]int, len(members))
	peers := map[string]bool{}
	for i, m := range members {
		if len(m.IDK) != IdentitySize || !validAcct(m.Acct) || !validPeer(m.Peer) || peers[m.Peer] {
			return nil, nil, ErrInvalid
		}
		peers[m.Peer] = true
		e := make([]byte, 0, IdentitySize+3+len(m.Acct)+len(m.Peer))
		e = append(e, m.IDK...)
		e = binary.BigEndian.AppendUint16(e, uint16(len(m.Acct)))
		e = append(e, m.Acct...)
		e = append(e, byte(len(m.Peer)))
		e = append(e, m.Peer...)
		entries[i] = e
		order[i] = i
	}
	sort.Slice(order, func(a, b int) bool { return bytes.Compare(entries[order[a]], entries[order[b]]) < 0 })
	return entries, order, nil
}

func RoundID(call string, gen uint32, members []Member) ([]byte, error) {
	if !validCall(call) || gen == 0 {
		return nil, ErrInvalid
	}
	entries, order, err := rosterEntries(members)
	if err != nil {
		return nil, err
	}
	h := sha256.New()
	h.Write([]byte(roundLabel + "\n" + call + "\n"))
	h.Write(binary.BigEndian.AppendUint32(nil, gen))
	h.Write([]byte{byte(len(order))})
	for _, i := range order {
		h.Write(entries[i])
	}
	return h.Sum(nil), nil
}

type SAS struct {
	Round      []byte
	Transcript []byte
	Emoji      [SASEmoji]int
	Digits     string
	QR         string
}

func (c SAS) EmojiText() string {
	parts := make([]string, SASEmoji)
	for i, e := range c.Emoji {
		parts[i] = EmojiTable[e].Text
	}
	return strings.Join(parts, " ")
}

func (c SAS) EmojiNames() string {
	parts := make([]string, SASEmoji)
	for i, e := range c.Emoji {
		parts[i] = EmojiTable[e].Name
	}
	return strings.Join(parts, ", ")
}

func ComputeSAS(call string, gen uint32, members []Member) (SAS, error) {
	round, err := RoundID(call, gen, members)
	if err != nil {
		return SAS{}, err
	}
	_, order, _ := rosterEntries(members)
	h := sha256.New()
	h.Write([]byte(sasLabel + "\n"))
	h.Write(round)
	for _, i := range order {
		if len(members[i].Nonce) != NonceSize {
			return SAS{}, ErrInvalid
		}
		h.Write(members[i].Nonce)
	}
	t := h.Sum(nil)
	return sasFromTranscript(round, t), nil
}

func sasFromTranscript(round, t []byte) SAS {
	c := SAS{Round: round, Transcript: t}
	v := uint32(t[0])<<16 | uint32(t[1])<<8 | uint32(t[2])
	for i := range c.Emoji {
		c.Emoji[i] = int(v>>(18-6*i)) & 63
	}
	d := binary.BigEndian.Uint32(t[3:7]) % 1000000
	c.Digits = fmt.Sprintf("%03d %03d", d/1000, d%1000)
	c.QR = QRPrefix + qrEncoding.EncodeToString(t)
	return c
}

func ParseQR(text string) ([]byte, error) {
	if !strings.HasPrefix(text, QRPrefix) || len(text) != len(QRPrefix)+qrEncoding.EncodedLen(sha256.Size) {
		return nil, ErrInvalid
	}
	t, err := qrEncoding.DecodeString(text[len(QRPrefix):])
	if err != nil || len(t) != sha256.Size || qrEncoding.EncodeToString(t) != text[len(QRPrefix):] {
		return nil, ErrInvalid
	}
	return t, nil
}

func commitHash(call, peer string, idk, round []byte, gen uint32, nonce []byte) []byte {
	h := sha256.New()
	h.Write([]byte(strings.Join([]string{nonceLabel, call, peer, enc(idk), enc(round), strconv.FormatUint(uint64(gen), 10)}, "\n") + "\n"))
	h.Write(nonce)
	return h.Sum(nil)
}

type Commit struct {
	Call  string
	Peer  string
	IDK   ed25519.PublicKey
	Round []byte
	Gen   uint32
	TS    int64
	C     []byte
	Sig   []byte
}

type commitJSON struct {
	V     int    `json:"v"`
	T     string `json:"t"`
	Call  string `json:"call"`
	Peer  string `json:"peer"`
	KID   string `json:"kid"`
	IDK   string `json:"idk"`
	Round string `json:"round"`
	Gen   uint32 `json:"gen"`
	TS    int64  `json:"ts"`
	C     string `json:"c"`
	Sig   string `json:"sig"`
}

func (c *Commit) Marshal() []byte {
	b, _ := json.Marshal(commitJSON{
		V: Version, T: "commit", Call: c.Call, Peer: c.Peer, KID: KeyID(c.IDK), IDK: enc(c.IDK),
		Round: enc(c.Round), Gen: c.Gen, TS: c.TS, C: enc(c.C), Sig: enc(c.Sig),
	})
	return b
}

func (c *Commit) SignedMessage() []byte {
	return []byte(strings.Join([]string{
		commitLabel, c.Call, c.Peer, KeyID(c.IDK), enc(c.IDK), enc(c.Round),
		strconv.FormatUint(uint64(c.Gen), 10), strconv.FormatInt(c.TS, 10), enc(c.C),
	}, "\n"))
}

func (c *Commit) Verify() error {
	if !ed25519.Verify(c.IDK, c.SignedMessage(), c.Sig) {
		return ErrSignature
	}
	return nil
}

func (c *Commit) Opens(nonce []byte) bool {
	return len(nonce) == NonceSize && subtle.ConstantTimeCompare(commitHash(c.Call, c.Peer, c.IDK, c.Round, c.Gen, nonce), c.C) == 1
}

func messageHead(b []byte, kind string) (fields, error) {
	f, err := parseObject(b)
	if err != nil {
		return nil, err
	}
	if v, err := f.integer("v", 0, maxSafeInt); err != nil || v != Version {
		return nil, ErrInvalid
	}
	if t, err := f.str("t"); err != nil || t != kind {
		return nil, ErrInvalid
	}
	return f, nil
}

func ParseCommit(b []byte) (*Commit, error) {
	f, err := messageHead(b, "commit")
	if err != nil {
		return nil, err
	}
	c := &Commit{}
	var kid string
	var n int64
	if c.Call, err = f.str("call"); err != nil || !validCall(c.Call) {
		return nil, ErrInvalid
	}
	if c.Peer, err = f.str("peer"); err != nil || !validPeer(c.Peer) {
		return nil, ErrInvalid
	}
	if kid, err = f.str("kid"); err != nil {
		return nil, ErrInvalid
	}
	idk, err := f.bytes("idk", IdentitySize)
	if err != nil {
		return nil, err
	}
	c.IDK = ed25519.PublicKey(idk)
	if kid != KeyID(idk) {
		return nil, ErrInvalid
	}
	if c.Round, err = f.bytes("round", RoundSize); err != nil {
		return nil, err
	}
	if n, err = f.integer("gen", 1, maxEpoch); err != nil {
		return nil, err
	}
	c.Gen = uint32(n)
	if c.TS, err = f.integer("ts", 1, maxSafeInt); err != nil {
		return nil, err
	}
	if c.C, err = f.bytes("c", sha256.Size); err != nil {
		return nil, err
	}
	if c.Sig, err = f.bytes("sig", SignatureSize); err != nil {
		return nil, err
	}
	return c, nil
}

type Reveal struct {
	Call  string
	Peer  string
	Round []byte
	Gen   uint32
	Nonce []byte
}

type revealJSON struct {
	V     int    `json:"v"`
	T     string `json:"t"`
	Call  string `json:"call"`
	Peer  string `json:"peer"`
	Round string `json:"round"`
	Gen   uint32 `json:"gen"`
	Nonce string `json:"nonce"`
}

func (r *Reveal) Marshal() []byte {
	b, _ := json.Marshal(revealJSON{V: Version, T: "reveal", Call: r.Call, Peer: r.Peer, Round: enc(r.Round), Gen: r.Gen, Nonce: enc(r.Nonce)})
	return b
}

func ParseReveal(b []byte) (*Reveal, error) {
	f, err := messageHead(b, "reveal")
	if err != nil {
		return nil, err
	}
	r := &Reveal{}
	var n int64
	if r.Call, err = f.str("call"); err != nil || !validCall(r.Call) {
		return nil, ErrInvalid
	}
	if r.Peer, err = f.str("peer"); err != nil || !validPeer(r.Peer) {
		return nil, ErrInvalid
	}
	if r.Round, err = f.bytes("round", RoundSize); err != nil {
		return nil, err
	}
	if n, err = f.integer("gen", 1, maxEpoch); err != nil {
		return nil, err
	}
	r.Gen = uint32(n)
	if r.Nonce, err = f.bytes("nonce", NonceSize); err != nil {
		return nil, err
	}
	return r, nil
}
