package plugins

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"os"
	"path/filepath"
	"sort"
	"strings"

	"forkmesh.com/desktop-core/internal/identity"
)

const (
	SignatureFile     = "SIGNATURE"
	installStamp      = ".forkmesh-install.json"
	signatureDomain   = "forkmesh-package-signature-v1"
	maxSignedFiles    = 4096
	maxSignedBytes    = 256 << 20
	signatureSchemaV1 = 1
)

var (
	ErrUnsigned  = errors.New("plugins: package is not signed")
	ErrTampered  = errors.New("plugins: package files do not match their signature")
	ErrBadSigner = errors.New("plugins: signature does not verify")
)

type Signature struct {
	SchemaVersion int               `json:"schemaVersion"`
	Algorithm     string            `json:"algorithm"`
	Package       string            `json:"package"`
	Version       string            `json:"version"`
	Signer        string            `json:"signer"`
	Files         map[string]string `json:"files"`
	Signature     string            `json:"signature"`
}

func Digests(dir string) (map[string]string, error) {
	out := map[string]string{}
	total := int64(0)
	err := filepath.WalkDir(dir, func(full string, entry fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		relative, err := filepath.Rel(dir, full)
		if err != nil {
			return err
		}
		relative = filepath.ToSlash(relative)
		if relative == "." {
			return nil
		}
		if entry.Type()&fs.ModeSymlink != 0 {
			return fmt.Errorf("plugins: %s is a symlink", relative)
		}
		if entry.IsDir() {
			return nil
		}
		if relative == SignatureFile || relative == installStamp {
			return nil
		}
		if !entry.Type().IsRegular() {
			return fmt.Errorf("plugins: %s is not a regular file", relative)
		}
		if len(out) >= maxSignedFiles {
			return errors.New("plugins: too many files to sign")
		}
		file, err := os.Open(full)
		if err != nil {
			return err
		}
		defer file.Close()
		hash := sha256.New()
		written, err := io.Copy(hash, io.LimitReader(file, maxSignedBytes-total+1))
		if err != nil {
			return err
		}
		total += written
		if total > maxSignedBytes {
			return errors.New("plugins: package too large to sign")
		}
		out[relative] = hex.EncodeToString(hash.Sum(nil))
		return nil
	})
	return out, err
}

func signedPayload(packageID, version string, files map[string]string) []byte {
	names := make([]string, 0, len(files))
	for name := range files {
		names = append(names, name)
	}
	sort.Strings(names)
	var builder strings.Builder
	builder.WriteString(signatureDomain + "\n" + packageID + "\n" + version + "\n")
	for _, name := range names {
		builder.WriteString(files[name] + "  " + name + "\n")
	}
	return []byte(builder.String())
}

func Sign(dir string, signer *identity.Identity) (Signature, error) {
	manifest, err := LoadManifest(dir)
	if err != nil && !errors.Is(err, ErrNoService) {
		return Signature{}, err
	}
	files, err := Digests(dir)
	if err != nil {
		return Signature{}, err
	}
	signature := Signature{SchemaVersion: signatureSchemaV1, Algorithm: "ed25519", Package: manifest.ID,
		Version: manifest.Version, Signer: signer.PublicKey(), Files: files}
	signature.Signature = signer.Sign(signedPayload(manifest.ID, manifest.Version, files))
	raw, _ := json.MarshalIndent(signature, "", "  ")
	if err := os.WriteFile(filepath.Join(dir, SignatureFile), append(raw, '\n'), 0o644); err != nil {
		return Signature{}, err
	}
	return signature, nil
}

func Verify(dir string, manifest Manifest) (Signature, error) {
	var signature Signature
	raw, err := os.ReadFile(filepath.Join(dir, SignatureFile))
	if err != nil {
		return signature, ErrUnsigned
	}
	if err := json.Unmarshal(raw, &signature); err != nil || signature.SchemaVersion != signatureSchemaV1 ||
		signature.Algorithm != "ed25519" {
		return signature, ErrUnsigned
	}
	if signature.Package != manifest.ID || signature.Version != manifest.Version {
		return signature, fmt.Errorf("%w: signed for %s %s", ErrTampered, signature.Package, signature.Version)
	}
	if !identity.Verify(signature.Signer, signature.Signature,
		signedPayload(signature.Package, signature.Version, signature.Files)) {
		return signature, ErrBadSigner
	}
	actual, err := Digests(dir)
	if err != nil {
		return signature, fmt.Errorf("%w: %v", ErrTampered, err)
	}
	if len(actual) != len(signature.Files) {
		return signature, ErrTampered
	}
	for name, digest := range actual {
		if signature.Files[name] != digest {
			return signature, fmt.Errorf("%w: %s", ErrTampered, name)
		}
	}
	return signature, nil
}

func (s Signature) Digest() string {
	sum := sha256.Sum256(signedPayload(s.Package, s.Version, s.Files))
	return hex.EncodeToString(sum[:])
}
