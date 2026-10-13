package mirrornode

import (
	"bufio"
	"crypto/ed25519"
	"crypto/rand"
	"crypto/sha256"
	"crypto/x509"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"encoding/pem"
	"errors"
	"io"
	"os"
	"path/filepath"
	"regexp"
	"strings"
)

const maxHelperRequest = 128 << 10

var (
	nodePattern       = regexp.MustCompile(`^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$`)
	noncePattern      = regexp.MustCompile(`^[A-Za-z0-9_-]{16,128}$`)
	numberPattern     = regexp.MustCompile(`^[1-9][0-9]{0,18}$`)
	repositoryPattern = regexp.MustCompile(`^[A-Za-z0-9._-]{1,100}$`)
	forkParentPattern = regexp.MustCompile(`^[A-Za-z0-9._:-]{1,80}/[A-Za-z0-9._:-]{1,80}$`)
	digestPattern     = regexp.MustCompile(`^[0-9a-f]{64}$`)
	requestIDPattern  = regexp.MustCompile(`^[A-Za-z0-9_-]{12,80}$`)
)

type Identity struct {
	private ed25519.PrivateKey
	public  ed25519.PublicKey
}

// GenerateIdentity creates a new owner-only PKCS#8 Ed25519 identity.  The
// temporary file is fsynced and renamed so a power loss can never leave the
// service with a partial key.
func GenerateIdentity(path string) (*Identity, error) {
	if !filepath.IsAbs(path) {
		return nil, errors.New("identity key path must be absolute")
	}
	if existing, err := LoadIdentity(path); err == nil {
		return existing, nil
	} else if _, statErr := os.Lstat(path); statErr == nil {
		return nil, errors.New("refusing to replace an invalid identity key")
	}
	public, private, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		return nil, err
	}
	encoded, err := x509.MarshalPKCS8PrivateKey(private)
	if err != nil {
		return nil, err
	}
	block := pem.EncodeToMemory(&pem.Block{Type: "PRIVATE KEY", Bytes: encoded})
	if err := writeIdentityPEM(path, block); err != nil {
		return nil, err
	}
	return &Identity{private: private, public: public}, nil
}

// writeIdentityPEM atomically installs an owner-only identity key, replacing
// any existing file at path.
func writeIdentityPEM(path string, block []byte) error {
	if err := os.MkdirAll(filepath.Dir(path), 0700); err != nil {
		return err
	}
	temporary, err := os.CreateTemp(filepath.Dir(path), ".identity-*")
	if err != nil {
		return err
	}
	temporaryPath := temporary.Name()
	defer os.Remove(temporaryPath)
	if err := temporary.Chmod(0600); err != nil {
		temporary.Close()
		return err
	}
	if _, err := temporary.Write(block); err != nil {
		temporary.Close()
		return err
	}
	if err := temporary.Sync(); err != nil {
		temporary.Close()
		return err
	}
	if err := temporary.Close(); err != nil {
		return err
	}
	return os.Rename(temporaryPath, path)
}

func LoadIdentity(path string) (*Identity, error) {
	info, err := os.Lstat(path)
	if err != nil || !info.Mode().IsRegular() || info.Mode()&os.ModeSymlink != 0 || info.Size() > 64<<10 {
		return nil, errors.New("identity key must be a small regular file")
	}
	if info.Mode().Perm()&0077 != 0 {
		return nil, errors.New("identity key must be owner-only")
	}
	raw, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	return parseIdentityPEM(raw)
}

func parseIdentityPEM(raw []byte) (*Identity, error) {
	block, rest := pem.Decode(raw)
	if block == nil || len(strings.TrimSpace(string(rest))) != 0 {
		return nil, errors.New("identity key is not one PEM block")
	}
	parsed, err := x509.ParsePKCS8PrivateKey(block.Bytes)
	if err != nil {
		return nil, errors.New("identity key is not PKCS#8")
	}
	private, ok := parsed.(ed25519.PrivateKey)
	if !ok {
		return nil, errors.New("identity key is not Ed25519")
	}
	public := private.Public().(ed25519.PublicKey)
	return &Identity{private: private, public: public}, nil
}

func (i *Identity) PublicKey() string { return base64.RawURLEncoding.EncodeToString(i.public) }
func (i *Identity) Sign(payload []byte) string {
	return base64.RawURLEncoding.EncodeToString(ed25519.Sign(i.private, payload))
}

type helperRequest struct {
	SchemaVersion int    `json:"schemaVersion"`
	Type          string `json:"type"`
	Algorithm     string `json:"algorithm"`
	Encoding      string `json:"encoding"`
	PublicKey     string `json:"publicKey"`
	MessageBase64 string `json:"messageBase64"`
	MessageSHA256 string `json:"messageSha256"`
	Signature     string `json:"signature,omitempty"`
}

type manifestSigningRequest struct {
	SchemaVersion    int    `json:"schemaVersion"`
	Type             string `json:"type"`
	Algorithm        string `json:"algorithm"`
	Encoding         string `json:"encoding"`
	Canonicalization string `json:"canonicalization"`
	PublicKey        string `json:"publicKey"`
	PayloadBase64    string `json:"payloadBase64"`
	PayloadSHA256    string `json:"payloadSha256"`
}

func RunManifestSigner(input io.Reader, output io.Writer, identity *Identity) error {
	raw, err := io.ReadAll(io.LimitReader(input, maxHelperRequest+1))
	if err != nil || len(raw) == 0 || len(raw) > maxHelperRequest {
		return errors.New("invalid request size")
	}
	var request manifestSigningRequest
	decoder := json.NewDecoder(strings.NewReader(string(raw)))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&request); err != nil || decoder.Decode(&struct{}{}) != io.EOF {
		return errors.New("invalid request JSON")
	}
	if request.SchemaVersion != 1 || request.Type != "forkmesh.mirror-endpoint-signing-request" ||
		request.Algorithm != "Ed25519" || request.Encoding != "base64url-no-padding" ||
		request.Canonicalization != "forkmesh-json-sort-v1" || request.PublicKey != identity.PublicKey() {
		return errors.New("unsupported manifest signing request")
	}
	payload, err := decodeCanonicalBase64(request.PayloadBase64, -1)
	if err != nil || len(payload) == 0 || len(payload) > 64<<10 {
		return errors.New("invalid manifest payload")
	}
	digest := sha256.Sum256(payload)
	if request.PayloadSHA256 != hex.EncodeToString(digest[:]) {
		return errors.New("manifest payload digest mismatch")
	}
	var manifest map[string]any
	if err := json.Unmarshal(payload, &manifest); err != nil || manifest["type"] != "forkmesh.mirror-endpoint" {
		return errors.New("invalid mirror manifest")
	}
	if _, signed := manifest["signature"]; signed {
		return errors.New("manifest is already signed")
	}
	return json.NewEncoder(output).Encode(map[string]any{
		"publicKey": identity.PublicKey(), "signature": identity.Sign(payload),
	})
}

func readHelperRequest(input io.Reader, expectSignature bool) (helperRequest, []byte, error) {
	var req helperRequest
	raw, err := io.ReadAll(io.LimitReader(input, maxHelperRequest+1))
	if err != nil || len(raw) == 0 || len(raw) > maxHelperRequest {
		return req, nil, errors.New("invalid request size")
	}
	decoder := json.NewDecoder(strings.NewReader(string(raw)))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&req); err != nil {
		return req, nil, errors.New("invalid request JSON")
	}
	if decoder.Decode(&struct{}{}) != io.EOF {
		return req, nil, errors.New("trailing request JSON")
	}
	expectedType := "forkmesh.health-challenge-signing"
	if expectSignature {
		expectedType = "forkmesh.request-capability-verification"
	} else if req.Signature != "" {
		return req, nil, errors.New("unexpected signature")
	}
	if req.SchemaVersion != 1 || req.Type != expectedType || req.Algorithm != "Ed25519" || req.Encoding != "base64url-no-padding" {
		return req, nil, errors.New("unsupported request protocol")
	}
	key, err := decodeCanonicalBase64(req.PublicKey, ed25519.PublicKeySize)
	if err != nil || len(key) != ed25519.PublicKeySize {
		return req, nil, errors.New("invalid public key")
	}
	payload, err := decodeCanonicalBase64(req.MessageBase64, -1)
	if err != nil || len(payload) == 0 || len(payload) > 16<<10 {
		return req, nil, errors.New("invalid message")
	}
	digest := sha256.Sum256(payload)
	if req.MessageSHA256 != hex.EncodeToString(digest[:]) {
		return req, nil, errors.New("message digest mismatch")
	}
	if expectSignature {
		if _, err := decodeCanonicalBase64(req.Signature, ed25519.SignatureSize); err != nil {
			return req, nil, errors.New("invalid signature")
		}
	}
	return req, payload, nil
}

func decodeCanonicalBase64(value string, size int) ([]byte, error) {
	if value == "" || strings.Contains(value, "=") {
		return nil, errors.New("invalid base64url")
	}
	raw, err := base64.RawURLEncoding.Strict().DecodeString(value)
	if err != nil || base64.RawURLEncoding.EncodeToString(raw) != value || (size >= 0 && len(raw) != size) {
		return nil, errors.New("invalid base64url")
	}
	return raw, nil
}

func validateHealthPayload(payload []byte, ownNode string) bool {
	lines := strings.Split(string(payload), "\n")
	if len(lines) == 4 && lines[0] == "forkmesh-https-health-v1" {
		return lines[1] == ownNode && noncePattern.MatchString(lines[2]) && numberPattern.MatchString(lines[3])
	}
	return len(lines) == 10 && lines[0] == "forkmesh-https-health-repository-v1" && lines[1] == ownNode &&
		noncePattern.MatchString(lines[2]) && numberPattern.MatchString(lines[3]) && nodePattern.MatchString(lines[4]) &&
		repositoryPattern.MatchString(lines[5]) && (lines[6] == "0" || lines[6] == "1") &&
		((lines[6] == "1" && lines[7] == "ok") || (lines[6] == "0" && lines[7] == "unavailable")) &&
		digestPattern.MatchString(lines[8]) && digestPattern.MatchString(lines[9])
}

func validateCapabilityPayload(payload []byte, ownNode string) bool {
	lines := strings.Split(string(payload), "\n")
	if len(lines) == runnerGrantLines && lines[0] == runnerGrantDomain {
		// A runner lease grant (runnergrant.go) is the other router-signed
		// statement this node's gateway verifies.
		grant, ok := parseRunnerGrantMessage(string(payload))
		return ok && grant.Node == ownNode
	}
	return len(lines) == 7 && lines[0] == "forkmesh-masked-proxy-v1" && lines[1] == ownNode &&
		(lines[2] == "GET" || lines[2] == "HEAD" || lines[2] == "POST") && strings.HasPrefix(lines[3], "/") &&
		!strings.Contains(lines[3], "\r") && digestPattern.MatchString(lines[4]) &&
		requestIDPattern.MatchString(lines[5]) && numberPattern.MatchString(lines[6])
}

func RunHealthSigner(input io.Reader, output io.Writer, identity *Identity, node string) error {
	req, payload, err := readHelperRequest(bufio.NewReader(input), false)
	if err != nil || req.PublicKey != identity.PublicKey() || !validateHealthPayload(payload, node) {
		return errors.New("invalid request or local identity")
	}
	return json.NewEncoder(output).Encode(map[string]any{"publicKey": identity.PublicKey(), "signature": identity.Sign(payload)})
}

func RunCapabilityVerifier(input io.Reader, output io.Writer, node, routerPublicKey string) error {
	req, payload, err := readHelperRequest(bufio.NewReader(input), true)
	if err != nil || req.PublicKey != routerPublicKey || !validateCapabilityPayload(payload, node) {
		return errors.New("invalid request")
	}
	key, _ := decodeCanonicalBase64(req.PublicKey, ed25519.PublicKeySize)
	sig, _ := decodeCanonicalBase64(req.Signature, ed25519.SignatureSize)
	valid := ed25519.Verify(ed25519.PublicKey(key), payload, sig)
	return json.NewEncoder(output).Encode(map[string]any{"valid": valid, "publicKey": req.PublicKey})
}
