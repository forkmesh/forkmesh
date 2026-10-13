package mirrornode

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"os"
	"path/filepath"
	"time"
)

// A node identity export moves a node's Ed25519 key to a replacement host
// without a backup of everything else. The export is sealed with an operator
// passphrase and bound to the node name, so a key cannot be installed on a
// node that serves a different account by accident.
const (
	nodeIdentityExportSchema   = "forkmesh-node-identity-v1"
	maximumIdentityExportBytes = 1 << 20
)

type nodeIdentityExport struct {
	Schema        string `json:"schema"`
	Node          string `json:"node"`
	PublicKey     string `json:"publicKey"`
	PrivateKeyPEM string `json:"privateKeyPem"`
	ExportedAt    string `json:"exportedAt"`
}

type IdentityExportReport struct {
	Output     string `json:"output"`
	Node       string `json:"node"`
	PublicKey  string `json:"publicKey"`
	ExportedAt string `json:"exportedAt"`
}

func ExportNodeIdentity(cfg Config, node, output string, passphrase []byte, now time.Time) (IdentityExportReport, error) {
	identity, err := LoadIdentity(cfg.IdentityKey)
	if err != nil {
		return IdentityExportReport{}, err
	}
	raw, err := os.ReadFile(cfg.IdentityKey)
	if err != nil {
		return IdentityExportReport{}, err
	}
	report := IdentityExportReport{
		Output: output, Node: node, PublicKey: identity.PublicKey(),
		ExportedAt: now.UTC().Format(time.RFC3339),
	}
	payload, err := json.Marshal(nodeIdentityExport{
		Schema: nodeIdentityExportSchema, Node: node, PublicKey: report.PublicKey,
		PrivateKeyPEM: string(raw), ExportedAt: report.ExportedAt,
	})
	if err != nil {
		return report, err
	}
	if _, _, err := writeSealedFile(output, passphrase, func(out io.Writer) error {
		_, err := out.Write(payload)
		return err
	}); err != nil {
		return report, fmt.Errorf("export identity: %w", err)
	}
	return report, nil
}

type IdentityImportOptions struct {
	Input      string
	Passphrase []byte
	// Replace installs the imported key over a different existing key. The
	// existing key is always preserved beside it, never deleted.
	Replace bool
	DryRun  bool
	Now     time.Time
}

type IdentityImportReport struct {
	Node                 string `json:"node"`
	PublicKey            string `json:"publicKey"`
	PreviousPublicKey    string `json:"previousPublicKey,omitempty"`
	PreservedPreviousKey string `json:"preservedPreviousKey,omitempty"`
	// Action is "install", "replace", or "unchanged".
	Action string `json:"action"`
	DryRun bool   `json:"dryRun"`
}

func ImportNodeIdentity(cfg Config, options IdentityImportOptions) (IdentityImportReport, error) {
	report := IdentityImportReport{DryRun: options.DryRun}
	gateway, _, err := loadGatewayConfig(cfg.GatewayConfig)
	if err != nil {
		return report, fmt.Errorf("gateway config: %w", err)
	}
	report.Node = gateway.Node.Name
	export, identity, err := readIdentityExport(options.Input, options.Passphrase)
	if err != nil {
		return report, err
	}
	report.PublicKey = identity.PublicKey()
	if export.Node != gateway.Node.Name {
		return report, fmt.Errorf("the identity export belongs to node %q, but this configuration serves %q", export.Node, gateway.Node.Name)
	}
	_, statErr := os.Lstat(cfg.IdentityKey)
	if statErr != nil && !errors.Is(statErr, fs.ErrNotExist) {
		return report, statErr
	}
	exists := statErr == nil
	existing, loadErr := LoadIdentity(cfg.IdentityKey)
	sameKey := loadErr == nil && existing.PublicKey() == identity.PublicKey()
	if loadErr == nil && !sameKey {
		report.PreviousPublicKey = existing.PublicKey()
	}
	switch {
	case sameKey:
		report.Action = "unchanged"
	case exists && !options.Replace:
		return report, errors.New("this node already has a different identity key; pass --replace-identity to install the imported key (the current key is kept beside it)")
	case exists:
		report.Action = "replace"
	default:
		report.Action = "install"
	}
	gatewayStale := gateway.Node.PublicKey != identity.PublicKey()
	if options.DryRun || (sameKey && !gatewayStale) {
		return report, nil
	}
	if os.Geteuid() == 0 {
		return report, errors.New("run the import as the node's service account (for example `runuser -u forkmesh-node -- forkmesh-mirror-node ...`) so the key keeps the owner the service needs")
	}
	if exists && !sameKey {
		preserved := fmt.Sprintf("%s.replaced-%d", cfg.IdentityKey, options.Now.Unix())
		if err := os.Link(cfg.IdentityKey, preserved); err != nil {
			return report, fmt.Errorf("preserve the current identity key: %w", err)
		}
		report.PreservedPreviousKey = preserved
	}
	if !sameKey {
		if err := writeIdentityPEM(cfg.IdentityKey, []byte(export.PrivateKeyPEM)); err != nil {
			return report, fmt.Errorf("install identity: %w", err)
		}
	}
	if gatewayStale {
		// The daemon refuses to start when the gateway advertises another key,
		// and a tunnel-free node's direct-origin manifest is signed by it.
		gateway.Node.PublicKey = identity.PublicKey()
		if _, err := writeGatewayConfig(cfg.GatewayConfig, gateway); err != nil {
			return report, fmt.Errorf("identity installed but the gateway config was not updated: %w", err)
		}
		if _, err := os.Lstat(gateway.ManifestPath); cfg.DisableCloudflared && err == nil {
			if err := writeDirectMirrorManifest(gateway.ManifestPath, gateway.Node.Name, gateway.PublicOrigin, identity); err != nil {
				return report, fmt.Errorf("identity installed but the direct mirror manifest was not re-signed: %w", err)
			}
		}
	}
	return report, nil
}

func readIdentityExport(path string, passphrase []byte) (nodeIdentityExport, *Identity, error) {
	var export nodeIdentityExport
	if !filepath.IsAbs(path) {
		return export, nil, errors.New("identity export path must be absolute")
	}
	info, err := os.Lstat(path)
	if err != nil || !info.Mode().IsRegular() || info.Size() > maximumIdentityExportBytes {
		return export, nil, errors.New("identity export must be a small regular file")
	}
	file, err := os.Open(path)
	if err != nil {
		return export, nil, err
	}
	defer file.Close()
	reader, err := newOpenReader(file, passphrase)
	if err != nil {
		return export, nil, err
	}
	raw, err := io.ReadAll(io.LimitReader(reader, maximumIdentityExportBytes))
	if err != nil {
		return export, nil, err
	}
	decoder := json.NewDecoder(bytes.NewReader(raw))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&export); err != nil || export.Schema != nodeIdentityExportSchema {
		return export, nil, errors.New("file is not a ForkMesh node identity export")
	}
	identity, err := parseIdentityPEM([]byte(export.PrivateKeyPEM))
	if err != nil {
		return export, nil, err
	}
	if identity.PublicKey() != export.PublicKey {
		return export, nil, errors.New("identity export key does not match its public key")
	}
	return export, identity, nil
}
