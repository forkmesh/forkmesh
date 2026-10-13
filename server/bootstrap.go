package mirrornode

import (
	"crypto/sha256"
	"encoding/json"
	"errors"
	"fmt"
	"net/url"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"time"
)

type BootstrapOptions struct {
	ConfigPath         string
	StateDirectory     string
	Node               string
	Owner              string
	Repository         string
	Upstream           string
	CatalogURL         string
	PublicOrigin       string
	MirrorURL          string
	RouterPublicKey    string
	GatewayScript      string
	Python             string
	Cloudflared        string
	DisableCloudflared bool
	Version            string
}

// Bootstrap writes the minimum native state for a fresh server.
func Bootstrap(options BootstrapOptions) (string, error) {
	for label, value := range map[string]string{
		"config": options.ConfigPath, "state": options.StateDirectory,
		"gateway script": options.GatewayScript, "python": options.Python,
	} {
		if !filepath.IsAbs(value) {
			return "", errors.New(label + " path must be absolute")
		}
	}
	if !options.DisableCloudflared && !filepath.IsAbs(options.Cloudflared) {
		return "", errors.New("cloudflared path must be absolute")
	}
	if !nodePattern.MatchString(options.Node) || !nodePattern.MatchString(options.Owner) ||
		!repositoryPattern.MatchString(options.Repository) || options.Upstream == "" ||
		options.CatalogURL == "" || options.PublicOrigin == "" || options.RouterPublicKey == "" {
		return "", errors.New("bootstrap identity, repository, or endpoint is invalid")
	}
	publicOrigin, err := normalizeHTTPSOrigin(options.PublicOrigin)
	if err != nil {
		return "", errors.New("bootstrap public origin is invalid")
	}
	mirrorURL := options.MirrorURL
	if options.DisableCloudflared && mirrorURL == "" {
		mirrorURL = publicOrigin
	}
	if mirrorURL != "" {
		mirrorURL, err = normalizeHTTPSOrigin(mirrorURL)
		if err != nil {
			return "", errors.New("bootstrap mirror URL is invalid")
		}
	}
	if options.DisableCloudflared && mirrorURL != publicOrigin {
		return "", errors.New("a tunnel-free mirror URL must match publicOrigin")
	}
	if err := os.MkdirAll(options.StateDirectory, 0700); err != nil {
		return "", err
	}
	identityPath := filepath.Join(options.StateDirectory, "identity", "ed25519.pem")
	identity, err := GenerateIdentity(identityPath)
	if err != nil {
		return "", err
	}
	repositoryPath := filepath.Join(options.StateDirectory, "repositories", options.Owner+"-"+options.Repository+".git")
	releaseStore := filepath.Join(repositoryPath, "forkmesh-releases")
	gatewayPath := filepath.Join(options.StateDirectory, "mirror-gateway", "config.json")
	manifestPath := filepath.Join(options.StateDirectory, "mirror-gateway", "forkmesh-mirror.json")
	tokenPath := filepath.Join(options.StateDirectory, "mirror-gateway", "connector.token")
	if err := os.MkdirAll(filepath.Dir(gatewayPath), 0700); err != nil {
		return "", err
	}
	if err := os.MkdirAll(releaseStore, 0700); err != nil {
		return "", err
	}
	var gateway GatewayConfig
	gateway.SchemaVersion = 1
	gateway.Node.Name = options.Node
	gateway.Node.PublicKey = identity.PublicKey()
	gateway.RouterPublicKey = options.RouterPublicKey
	gateway.PublicOrigin = publicOrigin
	gateway.Listen = json.RawMessage(`{"host":"127.0.0.1","port":8790}`)
	gateway.CollaborationControlURL =
		"http://127.0.0.1:8791/v1/control/collaboration"
	gateway.ManifestPath = manifestPath
	gateway.Limits = json.RawMessage(`{"maxReleaseBytes":2147483648,"maxPrivateReplicaBytes":536870912}`)
	// A fresh ForkMesh node already has the application repository needed to
	// serve the byte-identical dashboard assets. Enable its public-only replica
	// automatically when the catalog is on a safe HTTPS origin. Other projects
	// keep the historical Git-only bootstrap behavior.
	if options.Repository == "forkmesh" {
		if dashboardOrigin := catalogOrigin(options.CatalogURL); dashboardOrigin != "" {
			dashboard, marshalErr := json.Marshal(map[string]any{
				"enabled":          true,
				"assetsRepository": options.Owner + "/" + options.Repository,
				"canonicalOrigin":  dashboardOrigin,
				"replicaPath": filepath.Join(
					options.StateDirectory, "mirror-gateway", "dashboard-replica.json"),
			})
			if marshalErr != nil {
				return "", marshalErr
			}
			gateway.Dashboard = dashboard
		}
	}
	operations := append(append([]string(nil), defaultGatewayOperations...), writeGatewayOperations...)
	sort.Strings(operations)
	// A mirror is another origin for the canonical repository, not a second
	// repository owned by the machine name. Serve only /{owner}/{repository};
	// the node name remains the catalog/lease identity.
	repository := Repository{Owner: options.Owner, Name: options.Repository, Visibility: "public", Enabled: true, GitDir: repositoryPath, ReleaseStore: releaseStore, Operations: operations}
	gateway.Repositories = append(gateway.Repositories, repository)
	if _, err := writeGatewayConfig(gatewayPath, gateway); err != nil {
		return "", err
	}
	if options.DisableCloudflared {
		if err := writeDirectMirrorManifest(
			manifestPath, options.Node, publicOrigin, identity); err != nil {
			return "", err
		}
	}
	config := map[string]any{
		"schemaVersion": 1, "listen": "127.0.0.1:8791", "gatewayProbe": "127.0.0.1:8790",
		"gatewayConfig": gatewayPath, "gatewayScript": options.GatewayScript,
		"python": options.Python, "cloudflared": options.Cloudflared,
		"connectorToken": tokenPath, "identityKey": identityPath,
		"catalogUrl": options.CatalogURL, "publishOwner": options.Node,
		"canonicalOwner": options.Owner,
		"version":        options.Version, "syncInterval": "15s", "syncTimeout": "5m",
		"upstreams": map[string][]string{
			options.Owner + "/" + options.Repository: {options.Upstream},
		},
		// The relay broadcasts commit generations for bootstrap-managed sources.
		// An exact allowlist keeps third-party upstreams on their configured poll.
		"eventDrivenUpstreams": []string{options.Upstream},
	}
	if mirrorURL != "" {
		config["mirrorUrl"] = mirrorURL
	}
	if options.DisableCloudflared {
		config["disableCloudflared"] = true
	}
	if err := writePrivateJSON(options.ConfigPath, config); err != nil {
		return "", err
	}
	return identity.PublicKey(), nil
}

func catalogOrigin(value string) string {
	parsed, err := url.Parse(strings.TrimSpace(value))
	if err != nil || parsed.Scheme != "https" || parsed.Hostname() == "" ||
		parsed.User != nil || parsed.Port() != "" {
		return ""
	}
	host := strings.ToLower(parsed.Hostname())
	if strings.Contains(host, ":") {
		host = "[" + host + "]"
	}
	return "https://" + host
}

func writeDirectMirrorManifest(
	path, node, origin string, identity *Identity,
) error {
	parsedOrigin, err := url.Parse(origin)
	if err != nil || parsedOrigin.Hostname() == "" {
		return errors.New("direct manifest origin is invalid")
	}
	unsigned := map[string]any{
		"schemaVersion": 1,
		"type":          "forkmesh.mirror-endpoint",
		"generatedAt":   time.Now().UTC().Format(time.RFC3339Nano),
		"node": map[string]any{
			"name": node, "publicKey": identity.PublicKey(),
		},
		"endpoint": map[string]any{
			"origin":                origin,
			"healthUrl":             origin + "/health",
			"manifestUrl":           origin + "/forkmesh-mirror.json",
			"repositoryUrlTemplate": origin + "/{owner}/{repository}.git",
			"transport":             "direct-https", "mainProxyMode": "direct",
		},
		"dns": map[string]any{
			"recordName": parsedOrigin.Hostname(),
			"recordType": "A/AAAA", "proxied": false,
		},
		"edge": map[string]any{
			"provider": "independent", "kind": "public-https-origin",
			"originExposure": "public-https",
		},
		"storage": map[string]any{
			"repositoryBytesInD1": false,
			"repositoryByteOwner": "independent-mirror-host",
			"d1Purpose": []string{
				"service-discovery", "health-metadata", "routing-metadata",
				"signed-manifests",
			},
		},
	}
	payload, err := json.Marshal(unsigned)
	if err != nil {
		return err
	}
	digest := sha256.Sum256(payload)
	manifest := make(map[string]any, len(unsigned)+1)
	for key, value := range unsigned {
		manifest[key] = value
	}
	manifest["signature"] = map[string]any{
		"algorithm": "Ed25519", "encoding": "base64url-no-padding",
		"canonicalization": "forkmesh-json-sort-v1",
		"payloadSha256":    fmt.Sprintf("%x", digest[:]),
		"value":            identity.Sign(payload),
	}
	return writePrivateJSON(path, manifest)
}

func writePrivateJSON(path string, value any) error {
	if !filepath.IsAbs(path) {
		return errors.New("output path must be absolute")
	}
	if err := os.MkdirAll(filepath.Dir(path), 0700); err != nil {
		return err
	}
	raw, err := json.MarshalIndent(value, "", "  ")
	if err != nil {
		return err
	}
	raw = append(raw, '\n')
	temporary, err := os.CreateTemp(filepath.Dir(path), ".config-*")
	if err != nil {
		return err
	}
	temporaryPath := temporary.Name()
	defer os.Remove(temporaryPath)
	if err := temporary.Chmod(0600); err != nil {
		temporary.Close()
		return err
	}
	if _, err := temporary.Write(raw); err != nil {
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
