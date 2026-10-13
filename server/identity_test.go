package mirrornode

import (
	"bytes"
	"crypto/ed25519"
	"crypto/rand"
	"crypto/sha256"
	"crypto/x509"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"encoding/pem"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func testIdentity(t *testing.T) (*Identity, string) {
	t.Helper()
	_, private, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	der, err := x509.MarshalPKCS8PrivateKey(private)
	if err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(t.TempDir(), "ed25519.pem")
	if err := os.WriteFile(path, pem.EncodeToMemory(&pem.Block{Type: "PRIVATE KEY", Bytes: der}), 0600); err != nil {
		t.Fatal(err)
	}
	identity, err := LoadIdentity(path)
	if err != nil {
		t.Fatal(err)
	}
	return identity, path
}

func helperJSON(t *testing.T, kind, publicKey string, payload []byte, private ed25519.PrivateKey) []byte {
	t.Helper()
	digest := sha256.Sum256(payload)
	value := map[string]any{"schemaVersion": 1, "type": kind, "algorithm": "Ed25519", "encoding": "base64url-no-padding", "publicKey": publicKey, "messageBase64": base64.RawURLEncoding.EncodeToString(payload), "messageSha256": hex.EncodeToString(digest[:])}
	if private != nil {
		value["signature"] = base64.RawURLEncoding.EncodeToString(ed25519.Sign(private, payload))
	}
	raw, err := json.Marshal(value)
	if err != nil {
		t.Fatal(err)
	}
	return raw
}

func TestHealthSignerUsesExistingIdentityAndPinsNode(t *testing.T) {
	identity, _ := testIdentity(t)
	payload := []byte("forkmesh-https-health-v1\nmirror9\nabcdefghijklmnop\n123")
	request := helperJSON(t, "forkmesh.health-challenge-signing", identity.PublicKey(), payload, nil)
	var output bytes.Buffer
	if err := RunHealthSigner(bytes.NewReader(request), &output, identity, "mirror9"); err != nil {
		t.Fatal(err)
	}
	var response map[string]string
	if err := json.Unmarshal(output.Bytes(), &response); err != nil {
		t.Fatal(err)
	}
	sig, _ := base64.RawURLEncoding.DecodeString(response["signature"])
	if !ed25519.Verify(identity.public, payload, sig) {
		t.Fatal("signature does not verify")
	}
	wrongNode := bytes.Replace(request, []byte(""), []byte(""), 0)
	_ = wrongNode
	if err := RunHealthSigner(bytes.NewReader(request), &bytes.Buffer{}, identity, "mirror10"); err == nil {
		t.Fatal("cross-node health request accepted")
	}
	var badValue map[string]any
	if err := json.Unmarshal(request, &badValue); err != nil {
		t.Fatal(err)
	}
	badValue["messageSha256"] = strings.Repeat("0", 64)
	bad, _ := json.Marshal(badValue)
	if err := RunHealthSigner(bytes.NewReader(bad), &bytes.Buffer{}, identity, "mirror9"); err == nil {
		t.Fatal("malformed request accepted")
	}
}

func TestCapabilityVerifierPinsRouterAndValidatesCanonical(t *testing.T) {
	public, private, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	pub := base64.RawURLEncoding.EncodeToString(public)
	payload := []byte("forkmesh-masked-proxy-v1\nmirror9\nGET\n/repository\n0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\nabcdefghijkl\n123")
	request := helperJSON(t, "forkmesh.request-capability-verification", pub, payload, private)
	var output bytes.Buffer
	if err := RunCapabilityVerifier(bytes.NewReader(request), &output, "mirror9", pub); err != nil {
		t.Fatal(err)
	}
	var response struct {
		Valid bool `json:"valid"`
	}
	if err := json.Unmarshal(output.Bytes(), &response); err != nil || !response.Valid {
		t.Fatalf("valid request rejected: %v %s", err, output.String())
	}
	request[len(request)-2] ^= 1
	if err := RunCapabilityVerifier(bytes.NewReader(request), &bytes.Buffer{}, "mirror9", pub); err == nil {
		t.Fatal("corrupt request accepted")
	}
	other, _, _ := ed25519.GenerateKey(rand.Reader)
	if err := RunCapabilityVerifier(bytes.NewReader(helperJSON(t, "forkmesh.request-capability-verification", base64.RawURLEncoding.EncodeToString(other), payload, private)), &bytes.Buffer{}, "mirror9", pub); err == nil {
		t.Fatal("unpinned key accepted")
	}
}

func TestLoadIdentityRejectsWeakPermissionsAndWrongKey(t *testing.T) {
	_, path := testIdentity(t)
	if err := os.Chmod(path, 0644); err != nil {
		t.Fatal(err)
	}
	if _, err := LoadIdentity(path); err == nil {
		t.Fatal("weak permissions accepted")
	}
	if err := os.WriteFile(path, []byte("not pem"), 0600); err != nil {
		t.Fatal(err)
	}
	if _, err := LoadIdentity(path); err == nil {
		t.Fatal("invalid key accepted")
	}
}
