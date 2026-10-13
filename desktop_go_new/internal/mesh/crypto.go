package mesh

import (
	"crypto/aes"
	"crypto/cipher"
	"crypto/ecdh"
	"crypto/hmac"
	"crypto/pbkdf2"
	"crypto/rand"
	"crypto/sha256"
	"encoding/base64"
	"errors"
	"regexp"
	"sort"
	"sync"
)

const (
	appRoomKey       = "forkmesh-shared-room-key-v1"
	pbkdfRounds      = 210000
	keyBytes         = 32
	nonceBytes       = 12
	tagBytes         = 16
	maxPlainBytes    = 4 * 1024 * 1024
	sealVersion      = 2
	keyIDChars       = 16
	maxRecipients    = 64
	sealedType       = "sealed"
	keyFrameType     = "e2ee-key"
	SealKeySetting   = "chat/e2ee/x25519PrivateV2"
	sealPeersSetting = "chat/e2ee/peersV2/"
)

var (
	keyIDPattern     = regexp.MustCompile(`^[A-Za-z0-9_-]{16}$`)
	publicKeyPattern = regexp.MustCompile(`^[A-Za-z0-9_-]{43}$`)
	roomKeys         sync.Map
)

type RoomCrypto struct {
	key []byte
}

func RoomKey(room, passphrase string) ([]byte, error) {
	room = trimmed(room)
	if room == "" || passphrase == "" {
		return nil, errors.New("room name is required")
	}
	cacheID := passphrase + "\x00" + room
	if cached, ok := roomKeys.Load(cacheID); ok {
		return cached.([]byte), nil
	}
	salt := sha256.Sum256([]byte("ForkMesh room:" + room))
	key, err := pbkdf2.Key(sha256.New, passphrase, salt[:16], pbkdfRounds, keyBytes)
	if err != nil {
		return nil, err
	}
	roomKeys.Store(cacheID, key)
	return key, nil
}

func NewRoomCrypto(room string) (*RoomCrypto, error) {
	key, err := RoomKey(room, appRoomKey)
	if err != nil {
		return nil, err
	}
	return &RoomCrypto{key: key}, nil
}

func gcmSeal(key, nonce, plain, aad []byte) ([]byte, []byte, bool) {
	block, err := aes.NewCipher(key)
	if err != nil {
		return nil, nil, false
	}
	aead, err := cipher.NewGCM(block)
	if err != nil {
		return nil, nil, false
	}
	sealed := aead.Seal(nil, nonce, plain, aad)
	return sealed[:len(sealed)-tagBytes], sealed[len(sealed)-tagBytes:], true
}

func gcmOpen(key, nonce, body, tag, aad []byte) ([]byte, bool) {
	if len(key) != keyBytes || len(nonce) != nonceBytes || len(tag) != tagBytes {
		return nil, false
	}
	block, err := aes.NewCipher(key)
	if err != nil {
		return nil, false
	}
	aead, err := cipher.NewGCM(block)
	if err != nil {
		return nil, false
	}
	plain, err := aead.Open(nil, nonce, append(append([]byte(nil), body...), tag...), aad)
	if err != nil {
		return nil, false
	}
	return plain, true
}

func randomBytes(count int) []byte {
	out := make([]byte, count)
	if _, err := rand.Read(out); err != nil {
		return nil
	}
	return out
}

func (c *RoomCrypto) Encrypt(plain Object) Object {
	if c == nil || len(c.key) == 0 {
		return nil
	}
	return c.encryptWithNonce(plain, randomBytes(nonceBytes))
}

func (c *RoomCrypto) encryptWithNonce(plain Object, nonce []byte) Object {
	input := marshal(plain)
	if input == nil || len(input) > maxPlainBytes || len(nonce) != nonceBytes {
		return nil
	}
	body, tag, ok := gcmSeal(c.key, nonce, input, nil)
	if !ok {
		return nil
	}
	return Object{"kind": "cipher", "v": 1, "nonce": stdBase64(nonce), "tag": stdBase64(tag), "body": stdBase64(body)}
}

func (c *RoomCrypto) Decrypt(envelope Object) Object {
	if c == nil || len(c.key) == 0 || str(envelope["kind"]) != "cipher" {
		return nil
	}
	nonce := qtBase64(str(envelope["nonce"]))
	tag := qtBase64(str(envelope["tag"]))
	body := qtBase64(str(envelope["body"]))
	if len(nonce) != nonceBytes || len(tag) != tagBytes || len(body) == 0 || len(body) > maxPlainBytes {
		return nil
	}
	plain, ok := gcmOpen(c.key, nonce, body, tag, nil)
	if !ok {
		return nil
	}
	object, ok := parseObject(plain)
	if !ok {
		return nil
	}
	return object
}

type PeerKey struct {
	KeyID  string `json:"kid"`
	Public string `json:"pub"`
}

type RoomSeal struct {
	room      string
	private   *ecdh.PrivateKey
	public    []byte
	keyID     string
	peers     map[string][]byte
	order     []string
	OnPersist func([]PeerKey)
}

func toBase64URL(raw []byte) string { return base64.RawURLEncoding.EncodeToString(raw) }

func fromBase64URL(text string) []byte {
	if !publicKeyPattern.MatchString(text) {
		return nil
	}
	raw, err := base64.RawURLEncoding.DecodeString(text)
	if err != nil || len(raw) != keyBytes {
		return nil
	}
	return raw
}

func KeyIDFor(public []byte) string {
	if len(public) != keyBytes {
		return ""
	}
	sum := sha256.Sum256(public)
	return toBase64URL(sum[:])[:keyIDChars]
}

func NewRoomSeal(room string, privateKey []byte) (*RoomSeal, error) {
	room = trimmed(room)
	if room == "" || len(privateKey) != keyBytes {
		return nil, errors.New("a room and 32-byte device key are required")
	}
	private, err := ecdh.X25519().NewPrivateKey(privateKey)
	if err != nil {
		return nil, err
	}
	public := private.PublicKey().Bytes()
	return &RoomSeal{room: room, private: private, public: public, keyID: KeyIDFor(public), peers: map[string][]byte{}}, nil
}

func (s *RoomSeal) KeyID() string  { return s.keyID }
func (s *RoomSeal) PeerCount() int { return len(s.peers) }

func IsKeyFrame(frame Object) bool {
	return str(frame["type"]) == keyFrameType && qtInt(frame["v"]) == sealVersion &&
		keyIDPattern.MatchString(str(frame["kid"])) && publicKeyPattern.MatchString(str(frame["pub"]))
}

func IsSealedFrame(frame Object) bool {
	keys, isArray := frame["keys"].([]any)
	return str(frame["type"]) == sealedType && qtInt(frame["v"]) == sealVersion &&
		keyIDPattern.MatchString(str(frame["kid"])) && publicKeyPattern.MatchString(str(frame["pub"])) &&
		isArray && len(keys) > 0 && len(keys) <= maxRecipients
}

func FrameKeyID(frame Object) string {
	if IsSealedFrame(frame) {
		return str(frame["kid"])
	}
	return ""
}

func (s *RoomSeal) KeyFrame() Object {
	return Object{"type": keyFrameType, "v": sealVersion, "kid": s.keyID, "pub": toBase64URL(s.public)}
}

func (s *RoomSeal) rememberPeer(keyID string, public []byte) bool {
	if keyID == s.keyID || len(public) != keyBytes || KeyIDFor(public) != keyID {
		return false
	}
	_, known := s.peers[keyID]
	s.order = removeString(s.order, keyID)
	s.order = append(s.order, keyID)
	s.peers[keyID] = public
	for len(s.order) >= maxRecipients {
		delete(s.peers, s.order[0])
		s.order = s.order[1:]
	}
	return !known
}

func (s *RoomSeal) LoadPeers(entries []PeerKey) {
	first := len(entries) - (maxRecipients - 1)
	if first < 0 {
		first = 0
	}
	for _, entry := range entries[first:] {
		s.rememberPeer(entry.KeyID, fromBase64URL(entry.Public))
	}
}

func (s *RoomSeal) Peers() []PeerKey {
	out := make([]PeerKey, 0, len(s.order))
	for _, keyID := range s.order {
		if public := s.peers[keyID]; len(public) == keyBytes {
			out = append(out, PeerKey{KeyID: keyID, Public: toBase64URL(public)})
		}
	}
	return out
}

func (s *RoomSeal) persist() {
	if s.OnPersist != nil {
		s.OnPersist(s.Peers())
	}
}

func (s *RoomSeal) AcceptKeyFrame(frame Object) bool {
	if !IsKeyFrame(frame) {
		return false
	}
	isNew := s.rememberPeer(str(frame["kid"]), fromBase64URL(str(frame["pub"])))
	if isNew {
		s.persist()
	}
	return isNew
}

func (s *RoomSeal) pairwiseKey(peerPublic []byte, senderKeyID, recipientKeyID string) []byte {
	peer, err := ecdh.X25519().NewPublicKey(peerPublic)
	if err != nil {
		return nil
	}
	shared, err := s.private.ECDH(peer)
	if err != nil || len(shared) != keyBytes {
		return nil
	}
	salt := sha256.Sum256([]byte("ForkMesh chat e2ee salt:" + s.room))
	extract := hmac.New(sha256.New, salt[:])
	extract.Write(shared)
	prk := extract.Sum(nil)
	for i := range shared {
		shared[i] = 0
	}
	expand := hmac.New(sha256.New, prk)
	expand.Write([]byte("forkmesh-chat-e2ee-kek-v2\n" + s.room + "\n" + senderKeyID + "\n" + recipientKeyID))
	expand.Write([]byte{1})
	return expand.Sum(nil)
}

func contentAAD(room, senderKeyID string) []byte {
	return []byte("forkmesh-chat-e2ee-content-v2\n" + room + "\n" + senderKeyID)
}

func wrapAAD(room, senderKeyID, recipientKeyID string, bodyNonce []byte) []byte {
	return []byte("forkmesh-chat-e2ee-wrap-v2\n" + room + "\n" + senderKeyID + "\n" + recipientKeyID + "\n" + toBase64URL(bodyNonce))
}

func (s *RoomSeal) Seal(plain Object) Object {
	input := marshal(plain)
	if len(input) == 0 || len(input) > maxPlainBytes {
		return nil
	}
	contentKey := randomBytes(keyBytes)
	bodyNonce := randomBytes(nonceBytes)
	if contentKey == nil || bodyNonce == nil {
		return nil
	}
	defer func() {
		for i := range contentKey {
			contentKey[i] = 0
		}
	}()
	body, bodyTag, ok := gcmSeal(contentKey, bodyNonce, input, contentAAD(s.room, s.keyID))
	if !ok {
		return nil
	}
	recipients := map[string][]byte{}
	for _, keyID := range s.order {
		recipients[keyID] = s.peers[keyID]
	}
	recipients[s.keyID] = s.public
	ids := make([]string, 0, len(recipients))
	for keyID := range recipients {
		ids = append(ids, keyID)
	}
	sort.Strings(ids)
	wrapped := []any{}
	for _, keyID := range ids {
		kek := s.pairwiseKey(recipients[keyID], s.keyID, keyID)
		if len(kek) != keyBytes {
			continue
		}
		nonce := randomBytes(nonceBytes)
		if nonce == nil {
			continue
		}
		key, tag, ok := gcmSeal(kek, nonce, contentKey, wrapAAD(s.room, s.keyID, keyID, bodyNonce))
		if !ok {
			continue
		}
		wrapped = append(wrapped, Object{"kid": keyID, "nonce": stdBase64(nonce), "tag": stdBase64(tag), "key": stdBase64(key)})
	}
	if len(wrapped) == 0 {
		return nil
	}
	return Object{"type": sealedType, "v": sealVersion, "kid": s.keyID, "pub": toBase64URL(s.public),
		"nonce": stdBase64(bodyNonce), "tag": stdBase64(bodyTag), "body": stdBase64(body), "keys": wrapped}
}

func (s *RoomSeal) Unseal(frame Object) Object {
	if !IsSealedFrame(frame) {
		return nil
	}
	senderKeyID := str(frame["kid"])
	senderPublic := fromBase64URL(str(frame["pub"]))
	if len(senderPublic) != keyBytes || KeyIDFor(senderPublic) != senderKeyID {
		return nil
	}
	var wrapped Object
	for _, value := range frame["keys"].([]any) {
		candidate, _ := value.(Object)
		if str(candidate["kid"]) == s.keyID {
			wrapped = candidate
			break
		}
	}
	if len(wrapped) == 0 {
		return nil
	}
	bodyNonce := qtBase64(str(frame["nonce"]))
	bodyTag := qtBase64(str(frame["tag"]))
	body := qtBase64(str(frame["body"]))
	keyNonce := qtBase64(str(wrapped["nonce"]))
	keyTag := qtBase64(str(wrapped["tag"]))
	wrappedKey := qtBase64(str(wrapped["key"]))
	if len(bodyNonce) != nonceBytes || len(bodyTag) != tagBytes || len(body) == 0 || len(body) > maxPlainBytes ||
		len(keyNonce) != nonceBytes || len(keyTag) != tagBytes || len(wrappedKey) != keyBytes {
		return nil
	}
	kek := s.pairwiseKey(senderPublic, senderKeyID, s.keyID)
	if len(kek) != keyBytes {
		return nil
	}
	contentKey, ok := gcmOpen(kek, keyNonce, wrappedKey, keyTag, wrapAAD(s.room, senderKeyID, s.keyID, bodyNonce))
	if !ok || len(contentKey) != keyBytes {
		return nil
	}
	clear, ok := gcmOpen(contentKey, bodyNonce, body, bodyTag, contentAAD(s.room, senderKeyID))
	for i := range contentKey {
		contentKey[i] = 0
	}
	if !ok || len(clear) == 0 {
		return nil
	}
	object, ok := parseObject(clear)
	if !ok {
		return nil
	}
	if senderKeyID != s.keyID && s.rememberPeer(senderKeyID, senderPublic) {
		s.persist()
	}
	return object
}

func removeString(list []string, value string) []string {
	out := list[:0]
	for _, item := range list {
		if item != value {
			out = append(out, item)
		}
	}
	return out
}
