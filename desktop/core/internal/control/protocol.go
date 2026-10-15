package control

import (
	"bytes"
	"crypto/aes"
	"crypto/cipher"
	"crypto/ecdh"
	"crypto/ed25519"
	"crypto/hmac"
	"crypto/rand"
	"crypto/sha256"
	"encoding/base64"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"math"
	"regexp"
	"strings"
	"unicode"
	"unicode/utf16"
)

const (
	ProtocolVersion = 1

	keyBytes         = 32
	nonceBytes       = 12
	tagBytes         = 16
	clientNonceBytes = 16
	maxNameChars     = 80

	MaxFrameBytes      = 4 * 1024 * 1024
	FileChunkBytes     = 256 * 1024
	MaxRelayFrameBytes = 1024 * 1024
	RelayChunkChars    = 9000

	HelloTag   = "fm-control-hello"
	WelcomeTag = "fm-control-welcome"
	AuthTag    = "fm-control-auth"
)

var protocolLabel = []byte("forkmesh-local-control-v1")

func ProtocolLabel() []byte { return append([]byte(nil), protocolLabel...) }

func join(parts ...[]byte) []byte {
	return bytes.Join(parts, nil)
}

func hmacSHA256(key, message []byte) []byte {
	mac := hmac.New(sha256.New, key)
	mac.Write(message)
	return mac.Sum(nil)
}

func sha256Sum(input []byte) []byte {
	sum := sha256.Sum256(input)
	return sum[:]
}

func nonceFor(counter uint64) []byte {
	nonce := make([]byte, nonceBytes)
	binary.BigEndian.PutUint64(nonce[4:], counter)
	return nonce
}

func IsControlHello(payload []byte) bool {
	if !bytes.Contains(payload, []byte(HelloTag)) {
		return false
	}
	object := parseObject(payload)
	version, _ := jsonInt(object["v"])
	return jsonString(object["t"]) == HelloTag && version == ProtocolVersion
}

func Transcript(clientKey, clientEph, clientNonce, serverKey, serverEph string) []byte {
	nl := []byte{'\n'}
	return join(protocolLabel, nl, latin1(clientKey), nl, latin1(clientEph), nl, latin1(clientNonce), nl,
		latin1(serverKey), nl, latin1(serverEph))
}

func ServerSignedBytes(transcript []byte) []byte {
	return join(protocolLabel, []byte(" server\n"), transcript)
}

func ClientSignedBytes(transcript []byte) []byte {
	return join(protocolLabel, []byte(" client\n"), transcript)
}

type SessionKeys struct {
	ClientToServer []byte
	ServerToClient []byte
}

func (k SessionKeys) Valid() bool {
	return len(k.ClientToServer) == keyBytes && len(k.ServerToClient) == keyBytes
}

func DeriveKeys(sharedSecret, transcript []byte) SessionKeys {
	var keys SessionKeys
	if len(sharedSecret) != keyBytes || len(transcript) == 0 {
		return keys
	}
	prk := hmacSHA256(sha256Sum(transcript), sharedSecret)
	keys.ClientToServer = hmacSHA256(prk, join(protocolLabel, []byte(" c2s"), []byte{1}))
	keys.ServerToClient = hmacSHA256(prk, join(protocolLabel, []byte(" s2c"), []byte{1}))
	return keys
}

func PairingCode(transcript []byte) string {
	digest := sha256Sum(join(protocolLabel, []byte(" pair\n"), transcript))
	value := binary.BigEndian.Uint32(digest[:4])
	return fmt.Sprintf("%06d", value%1000000)
}

func newGCM(key []byte) cipher.AEAD {
	if len(key) != keyBytes {
		return nil
	}
	block, err := aes.NewCipher(key)
	if err != nil {
		return nil
	}
	aead, err := cipher.NewGCM(block)
	if err != nil {
		return nil
	}
	return aead
}

func SealFrame(key []byte, counter uint64, plain []byte) []byte {
	aead := newGCM(key)
	if aead == nil {
		return nil
	}
	return aead.Seal(nil, nonceFor(counter), plain, protocolLabel)
}

func OpenFrame(key []byte, counter uint64, frame []byte) []byte {
	aead := newGCM(key)
	if aead == nil || len(frame) < tagBytes {
		return nil
	}
	plain, err := aead.Open(nil, nonceFor(counter), frame, protocolLabel)
	if err != nil || len(plain) == 0 {
		return nil
	}
	return plain
}

type Ephemeral struct {
	PrivateKey []byte
	PublicKey  []byte
}

func NewEphemeral() Ephemeral {
	key, err := ecdh.X25519().GenerateKey(rand.Reader)
	if err != nil {
		return Ephemeral{}
	}
	return Ephemeral{PrivateKey: key.Bytes(), PublicKey: key.PublicKey().Bytes()}
}

func X25519(privateKey, peerPublic []byte) []byte {
	if len(privateKey) != keyBytes || len(peerPublic) != keyBytes {
		return nil
	}
	mine, err := ecdh.X25519().NewPrivateKey(privateKey)
	if err != nil {
		return nil
	}
	peer, err := ecdh.X25519().NewPublicKey(peerPublic)
	if err != nil {
		return nil
	}
	shared, err := mine.ECDH(peer)
	if err != nil || len(shared) != keyBytes || bytes.Equal(shared, make([]byte, keyBytes)) {
		return nil
	}
	return shared
}

func VerifyEd25519(publicKeyB64url, signatureB64url string, data []byte) bool {
	key := FromBase64Url(publicKeyB64url, keyBytes)
	sig := FromBase64Url(signatureB64url, ed25519.SignatureSize)
	if key == nil || sig == nil {
		return false
	}
	return ed25519.Verify(ed25519.PublicKey(key), data, sig)
}

func ToBase64Url(raw []byte) string {
	return base64.RawURLEncoding.EncodeToString(raw)
}

var base64UrlPattern = regexp.MustCompile(`^[A-Za-z0-9_-]+$`)

func FromBase64Url(text string, size int) []byte {
	if text == "" || utf16Len(text) > 200 || !base64UrlPattern.MatchString(text) {
		return nil
	}
	usable := text
	if len(usable)%4 == 1 {
		usable = usable[:len(usable)-1]
	}
	decoded, err := base64.RawURLEncoding.DecodeString(usable)
	if err != nil || len(decoded) != size {
		return nil
	}
	return decoded
}

func strictBase64(text string) []byte {
	trimmed := strings.TrimRight(text, "=")
	if strings.ContainsAny(trimmed, "=") {
		return nil
	}
	if len(trimmed)%4 == 1 {
		return nil
	}
	decoded, err := base64.RawStdEncoding.DecodeString(trimmed)
	if err != nil {
		return nil
	}
	return decoded
}

func lenientBase64(text string) []byte {
	clean := make([]byte, 0, len(text))
	for i := 0; i < len(text); i++ {
		c := text[i]
		if c == '=' {
			break
		}
		if (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/' {
			clean = append(clean, c)
		}
	}
	if len(clean)%4 == 1 {
		clean = clean[:len(clean)-1]
	}
	decoded, err := base64.RawStdEncoding.DecodeString(string(clean))
	if err != nil {
		return nil
	}
	return decoded
}

func SafeFileName(name string) string {
	normalized := strings.ReplaceAll(name, "\\", "/")
	base := normalized
	if index := strings.LastIndex(base, "/"); index >= 0 {
		base = base[index+1:]
	}
	units := utf16.Encode([]rune(base))
	out := make([]uint16, 0, len(units))
	for _, unit := range units {
		r := rune(unit)
		keep := false
		if !utf16.IsSurrogate(r) {
			keep = unicode.IsLetter(r) || unicode.IsNumber(r) || r == '.' || r == '-' || r == '_' ||
				r == ' ' || r == '(' || r == ')'
		}
		if keep {
			out = append(out, unit)
		} else {
			out = append(out, '_')
		}
	}
	for len(out) > 0 && out[0] == ' ' {
		out = out[1:]
	}
	for len(out) > 0 && out[len(out)-1] == ' ' {
		out = out[:len(out)-1]
	}
	for len(out) > 0 && out[0] == '.' {
		out = out[1:]
	}
	if len(out) > 120 {
		out = out[len(out)-120:]
	}
	if len(out) == 0 {
		return "file"
	}
	return string(utf16.Decode(out))
}

func Frame(payload []byte) []byte {
	out := make([]byte, 4+len(payload))
	binary.BigEndian.PutUint32(out, uint32(len(payload)))
	copy(out[4:], payload)
	return out
}

func takeFrame(buffer *[]byte) ([]byte, int) {
	data := *buffer
	if len(data) < 4 {
		return nil, 0
	}
	length := binary.BigEndian.Uint32(data)
	if length == 0 || int64(length) > MaxFrameBytes {
		return nil, -1
	}
	if len(data) < int(length)+4 {
		return nil, 0
	}
	payload := append([]byte(nil), data[4:4+int(length)]...)
	rest := data[4+int(length):]
	if len(rest) == 0 {
		*buffer = nil
	} else {
		*buffer = append([]byte(nil), rest...)
	}
	return payload, 1
}

func latin1(text string) []byte {
	out := make([]byte, 0, len(text))
	for _, r := range text {
		if r > 0xff {
			out = append(out, '?')
		} else {
			out = append(out, byte(r))
		}
	}
	return out
}

func utf16Len(text string) int {
	n := 0
	for _, r := range text {
		if r >= 0x10000 {
			n += 2
		} else {
			n++
		}
	}
	return n
}

func leftUTF16(text string, max int) string {
	if utf16Len(text) <= max {
		return text
	}
	units := utf16.Encode([]rune(text))
	return string(utf16.Decode(units[:max]))
}

func parseObject(raw []byte) map[string]any {
	var object map[string]any
	if err := json.Unmarshal(raw, &object); err != nil || object == nil {
		return map[string]any{}
	}
	return object
}

func jsonString(value any) string {
	text, _ := value.(string)
	return text
}

func jsonInt(value any) (int, bool) {
	number, ok := value.(float64)
	if !ok || math.IsNaN(number) || math.IsInf(number, 0) {
		return 0, false
	}
	if number < math.MinInt32 || number > math.MaxInt32 || number != math.Trunc(number) {
		return 0, false
	}
	return int(number), true
}

func jsonDouble(value any, fallback float64) float64 {
	number, ok := value.(float64)
	if !ok {
		return fallback
	}
	return number
}

func jsonBool(value any) bool {
	flag, _ := value.(bool)
	return flag
}

func jsonObject(value any) map[string]any {
	object, ok := value.(map[string]any)
	if !ok || object == nil {
		return map[string]any{}
	}
	return object
}

func boundedText(object map[string]any, key string, max int) string {
	return leftUTF16(strings.TrimSpace(jsonString(object[key])), max)
}

func encodeJSON(value any) []byte {
	var buffer bytes.Buffer
	encoder := json.NewEncoder(&buffer)
	encoder.SetEscapeHTML(false)
	if err := encoder.Encode(value); err != nil {
		return nil
	}
	return bytes.TrimRight(buffer.Bytes(), "\n")
}

func newUUID() string {
	raw := make([]byte, 16)
	_, _ = rand.Read(raw)
	raw[6] = (raw[6] & 0x0f) | 0x40
	raw[8] = (raw[8] & 0x3f) | 0x80
	return fmt.Sprintf("%x-%x-%x-%x-%x", raw[0:4], raw[4:6], raw[6:8], raw[8:10], raw[10:16])
}
