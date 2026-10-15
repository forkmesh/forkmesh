package vault

import (
	"crypto/aes"
	"crypto/cipher"
	"crypto/hkdf"
	"crypto/hmac"
	"crypto/rand"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"sort"
	"strconv"
	"strings"
)

const (
	Kind          = "forkmesh.vault"
	Version       = 1
	Scheme        = "ed25519-sig-hkdf-sha256-v1"
	labelKEK      = "forkmesh-vault-kek-v1"
	labelWrap     = "forkmesh-vault-wrap-v1"
	labelEntryKey = "forkmesh-vault-entry-key-v1"
	labelEntry    = "forkmesh-vault-entry-v1"
	labelMAC      = "forkmesh-vault-mac-v1"
	labelKeyID    = "forkmesh-vault-key-id-v1"
	keyBytes      = 32
	saltBytes     = 32
	nonceBytes    = 12
	maxWraps      = 8
	MaxNameBytes  = 200
	MaxValueBytes = 1 << 20
	MaxFileBytes  = 32 << 20
	MaxEntries    = 4096
)

var (
	ErrFormat           = errors.New("vault_invalid")
	ErrIdentityMismatch = errors.New("vault_identity_mismatch")
	ErrTampered         = errors.New("vault_tampered")
	ErrNoIdentity       = errors.New("vault_no_identity")
	ErrInvalidName      = errors.New("invalid_name")
	ErrTooLarge         = errors.New("value_too_large")
)

type Signer interface {
	PublicKey() string
	SignBytes(data []byte) []byte
}

type wrapRecord struct {
	Scheme   string `json:"scheme"`
	Identity string `json:"identity"`
	Salt     string `json:"salt"`
	Nonce    string `json:"nonce"`
	Key      string `json:"key"`
}

type entryRecord struct {
	Nonce string `json:"nonce"`
	Data  string `json:"data"`
	At    int64  `json:"at"`
}

type document struct {
	Kind       string                 `json:"kind"`
	V          int                    `json:"v"`
	KeyID      string                 `json:"keyId"`
	Generation int64                  `json:"generation"`
	Wraps      []wrapRecord           `json:"wraps"`
	Entries    map[string]entryRecord `json:"entries"`
	MAC        string                 `json:"mac"`
}

func ValidName(name string) bool {
	if name == "" || len(name) > MaxNameBytes {
		return false
	}
	for i := 0; i < len(name); i++ {
		if name[i] < 0x21 || name[i] > 0x7e {
			return false
		}
	}
	return true
}

func zero(buffer []byte) {
	for i := range buffer {
		buffer[i] = 0
	}
}

func randomBytes(n int) []byte {
	out := make([]byte, n)
	if _, err := rand.Read(out); err != nil {
		panic(err)
	}
	return out
}

func encode(data []byte) string { return base64.StdEncoding.EncodeToString(data) }

func decode(text string, size int) ([]byte, error) {
	out, err := base64.StdEncoding.DecodeString(text)
	if err != nil || (size > 0 && len(out) != size) {
		return nil, ErrFormat
	}
	return out, nil
}

func derive(secret []byte, salt []byte, label string, size int) []byte {
	out, err := hkdf.Key(sha256.New, secret, salt, label, size)
	if err != nil {
		panic(err)
	}
	return out
}

func keyIDOf(dek []byte) string {
	sub := derive(dek, nil, labelKeyID, 8)
	defer zero(sub)
	return hex.EncodeToString(sub)
}

func kekFor(signer Signer, salt []byte) []byte {
	signature := signer.SignBytes([]byte(labelKEK + "\n" + signer.PublicKey()))
	defer zero(signature)
	return derive(signature, salt, labelKEK, keyBytes)
}

func gcm(key []byte) cipher.AEAD {
	block, err := aes.NewCipher(key)
	if err != nil {
		panic(err)
	}
	aead, err := cipher.NewGCM(block)
	if err != nil {
		panic(err)
	}
	return aead
}

func wrapAAD(identity, keyID string) []byte {
	return []byte(labelWrap + "\x00" + identity + "\x00" + keyID)
}

func entryAAD(keyID, name string) []byte {
	return []byte(labelEntry + "\x00" + keyID + "\x00" + name)
}

func wrapKey(signer Signer, dek []byte, keyID string) wrapRecord {
	salt := randomBytes(saltBytes)
	nonce := randomBytes(nonceBytes)
	kek := kekFor(signer, salt)
	defer zero(kek)
	sealed := gcm(kek).Seal(nil, nonce, dek, wrapAAD(signer.PublicKey(), keyID))
	return wrapRecord{Scheme: Scheme, Identity: signer.PublicKey(), Salt: encode(salt), Nonce: encode(nonce), Key: encode(sealed)}
}

func unwrapKey(signer Signer, wrap wrapRecord, keyID string) ([]byte, error) {
	if wrap.Scheme != Scheme || wrap.Identity != signer.PublicKey() {
		return nil, ErrIdentityMismatch
	}
	salt, err := decode(wrap.Salt, saltBytes)
	if err != nil {
		return nil, err
	}
	nonce, err := decode(wrap.Nonce, nonceBytes)
	if err != nil {
		return nil, err
	}
	sealed, err := decode(wrap.Key, keyBytes+16)
	if err != nil {
		return nil, err
	}
	kek := kekFor(signer, salt)
	defer zero(kek)
	dek, err := gcm(kek).Open(nil, nonce, sealed, wrapAAD(wrap.Identity, keyID))
	if err != nil {
		return nil, ErrTampered
	}
	if keyIDOf(dek) != keyID {
		zero(dek)
		return nil, ErrTampered
	}
	return dek, nil
}

func sealEntry(dek []byte, keyID, name string, value []byte, at int64) entryRecord {
	key := derive(dek, nil, labelEntryKey, keyBytes)
	defer zero(key)
	nonce := randomBytes(nonceBytes)
	sealed := gcm(key).Seal(nil, nonce, value, entryAAD(keyID, name))
	return entryRecord{Nonce: encode(nonce), Data: encode(sealed), At: at}
}

func openEntry(dek []byte, keyID, name string, entry entryRecord) ([]byte, error) {
	nonce, err := decode(entry.Nonce, nonceBytes)
	if err != nil {
		return nil, err
	}
	sealed, err := decode(entry.Data, 0)
	if err != nil {
		return nil, err
	}
	key := derive(dek, nil, labelEntryKey, keyBytes)
	defer zero(key)
	plain, err := gcm(key).Open(nil, nonce, sealed, entryAAD(keyID, name))
	if err != nil {
		return nil, ErrTampered
	}
	return plain, nil
}

func sortedNames(entries map[string]entryRecord) []string {
	names := make([]string, 0, len(entries))
	for name := range entries {
		names = append(names, name)
	}
	sort.Strings(names)
	return names
}

func canonical(doc *document) []byte {
	var b strings.Builder
	b.WriteString(labelMAC + "\n")
	b.WriteString(strconv.Itoa(doc.V) + "\n")
	b.WriteString(doc.KeyID + "\n")
	b.WriteString(strconv.FormatInt(doc.Generation, 10) + "\n")
	for _, name := range sortedNames(doc.Entries) {
		entry := doc.Entries[name]
		b.WriteString(strconv.Itoa(len(name)) + ":" + name + "\n")
		b.WriteString(entry.Nonce + "\n" + entry.Data + "\n")
		b.WriteString(strconv.FormatInt(entry.At, 10) + "\n")
	}
	return []byte(b.String())
}

func macOf(dek []byte, doc *document) string {
	key := derive(dek, nil, labelMAC, keyBytes)
	defer zero(key)
	mac := hmac.New(sha256.New, key)
	mac.Write(canonical(doc))
	return encode(mac.Sum(nil))
}

func verifyMAC(dek []byte, doc *document) bool {
	want, err := decode(doc.MAC, sha256.Size)
	if err != nil {
		return false
	}
	got, _ := decode(macOf(dek, doc), sha256.Size)
	return hmac.Equal(want, got)
}

func parseDocument(raw []byte) (*document, error) {
	var doc document
	if err := json.Unmarshal(raw, &doc); err != nil {
		return nil, ErrFormat
	}
	if doc.Kind != Kind || doc.V != Version || len(doc.KeyID) != 16 || len(doc.Wraps) == 0 || len(doc.Wraps) > maxWraps*2 {
		return nil, ErrFormat
	}
	if doc.Entries == nil {
		doc.Entries = map[string]entryRecord{}
	}
	if len(doc.Entries) > MaxEntries {
		return nil, ErrFormat
	}
	for name := range doc.Entries {
		if !ValidName(name) {
			return nil, ErrFormat
		}
	}
	return &doc, nil
}

func marshalDocument(doc *document) ([]byte, error) {
	return json.MarshalIndent(doc, "", "  ")
}
