package mirrornode

import (
	"crypto/ed25519"
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"testing"
)

func TestBootstrapCreatesPrivateNativeConfiguration(t *testing.T) {
	root := t.TempDir()
	configPath := filepath.Join(root, "etc", "mirror-node.json")
	publicKey, err := Bootstrap(BootstrapOptions{
		ConfigPath: configPath, StateDirectory: filepath.Join(root, "state"),
		Node: "mirror17", Owner: "forkmesh", Repository: "forkmesh",
		Upstream:        "https://forkmesh.test/forkmesh/forkmesh",
		CatalogURL:      "https://forkmesh.test/api/repositories",
		PublicOrigin:    "https://mirror17.forkmesh.test",
		RouterPublicKey: "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
		GatewayScript:   "/usr/local/share/forkmesh/tools/mirror_gateway.py",
		Python:          "/usr/bin/python3", Cloudflared: "/usr/local/bin/cloudflared",
		Version: "test",
	})
	if err != nil {
		t.Fatal(err)
	}
	info, err := os.Stat(configPath)
	if err != nil || info.Mode().Perm()&0077 != 0 {
		t.Fatalf("configuration is not owner-only: %v %+v", err, info)
	}
	config, err := LoadConfig(configPath)
	if err != nil {
		t.Fatal(err)
	}
	identity, err := LoadIdentity(config.IdentityKey)
	if err != nil || identity.PublicKey() != publicKey {
		t.Fatalf("identity mismatch: %v", err)
	}
	raw, err := os.ReadFile(config.GatewayConfig)
	if err != nil {
		t.Fatal(err)
	}
	var gateway GatewayConfig
	if err := json.Unmarshal(raw, &gateway); err != nil || len(gateway.Repositories) != 1 {
		t.Fatalf("gateway configuration: %v %+v", err, gateway)
	}
	if gateway.Repositories[0].Owner != "forkmesh" ||
		config.CanonicalOwner != "forkmesh" {
		t.Fatalf("canonical repository route missing: %+v %+v", gateway, config)
	}
	releaseStore := filepath.Join(
		root, "state", "repositories", "forkmesh-forkmesh.git",
		"forkmesh-releases")
	for _, repository := range gateway.Repositories {
		if repository.ReleaseStore != releaseStore {
			t.Fatalf("release store missing from gateway repository: %+v", repository)
		}
	}
	var dashboard dashboardReplicaConfig
	if err := json.Unmarshal(gateway.Dashboard, &dashboard); err != nil ||
		!dashboard.Enabled || dashboard.ReplicaPath != filepath.Join(
		root, "state", "mirror-gateway", "dashboard-replica.json") {
		t.Fatalf("public dashboard replica missing from bootstrap: %v %+v", err, dashboard)
	}
	var dashboardValue map[string]any
	if err := json.Unmarshal(gateway.Dashboard, &dashboardValue); err != nil ||
		dashboardValue["assetsRepository"] != "forkmesh/forkmesh" ||
		dashboardValue["canonicalOrigin"] != "https://forkmesh.test" {
		t.Fatalf("dashboard bootstrap is not bound to the catalog origin: %v", dashboardValue)
	}
	releaseInfo, err := os.Stat(releaseStore)
	if err != nil || !releaseInfo.IsDir() || releaseInfo.Mode().Perm()&0077 != 0 {
		t.Fatalf("release store is not an owner-only directory: %v %+v", err, releaseInfo)
	}
	if values := config.Upstreams["forkmesh/forkmesh"]; len(values) != 1 || values[0] == "" ||
		len(config.Upstreams) != 1 {
		t.Fatalf("round-robin upstream missing: %+v", config.Upstreams)
	}
	if len(config.EventDrivenUpstreams) != 1 ||
		config.EventDrivenUpstreams[0] != config.Upstreams["forkmesh/forkmesh"][0] {
		t.Fatalf("event-driven upstream missing: %+v", config.EventDrivenUpstreams)
	}
}

func TestCatalogOriginAdmitsOnlyDefaultPortCredentialFreeHTTPS(t *testing.T) {
	for input, want := range map[string]string{
		"https://APP.ForkMesh.test/api/repositories": "https://app.forkmesh.test",
		"http://app.forkmesh.test/api/repositories":  "",
		"https://user@app.forkmesh.test/api":         "",
		"https://app.forkmesh.test:8443/api":         "",
	} {
		if got := catalogOrigin(input); got != want {
			t.Errorf("catalogOrigin(%q) = %q, want %q", input, got, want)
		}
	}
}

func TestBootstrapCreatesTunnelFreeDirectManifest(t *testing.T) {
	root := t.TempDir()
	configPath := filepath.Join(root, "etc", "mirror-node.json")
	_, err := Bootstrap(BootstrapOptions{
		ConfigPath: configPath, StateDirectory: filepath.Join(root, "state"),
		Node: "mirror17", Owner: "forkmesh", Repository: "forkmesh",
		Upstream:        "https://forkmesh.test/forkmesh/forkmesh",
		CatalogURL:      "https://forkmesh.test/api/repositories",
		PublicOrigin:    "https://git.mirror17.example.test",
		RouterPublicKey: "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
		GatewayScript:   "/usr/local/share/forkmesh/tools/mirror_gateway.py",
		Python:          "/usr/bin/python3", DisableCloudflared: true, Version: "test",
	})
	if err != nil {
		t.Fatal(err)
	}
	config, err := LoadConfig(configPath)
	if err != nil {
		t.Fatal(err)
	}
	if !config.DisableCloudflared || config.MirrorURL !=
		"https://git.mirror17.example.test" {
		t.Fatalf("direct configuration missing: %+v", config)
	}
	gateway, _, err := LoadGatewayConfigForCommand(config.GatewayConfig)
	if err != nil {
		t.Fatal(err)
	}
	raw, err := os.ReadFile(gateway.ManifestPath)
	if err != nil {
		t.Fatal(err)
	}
	var manifest map[string]any
	if err := json.Unmarshal(raw, &manifest); err != nil {
		t.Fatal(err)
	}
	signature := manifest["signature"].(map[string]any)
	delete(manifest, "signature")
	payload, _ := json.Marshal(manifest)
	digest := sha256.Sum256(payload)
	if signature["payloadSha256"] != fmt.Sprintf("%x", digest[:]) {
		t.Fatal("direct manifest digest mismatch")
	}
	public, _ := base64.RawURLEncoding.DecodeString(gateway.Node.PublicKey)
	signed, _ := base64.RawURLEncoding.DecodeString(signature["value"].(string))
	if !ed25519.Verify(ed25519.PublicKey(public), payload, signed) {
		t.Fatal("direct manifest signature mismatch")
	}
	endpoint := manifest["endpoint"].(map[string]any)
	if endpoint["repositoryUrlTemplate"] !=
		"https://git.mirror17.example.test/{owner}/{repository}.git" ||
		endpoint["mainProxyMode"] != "direct" {
		t.Fatalf("unexpected direct endpoint: %+v", endpoint)
	}
}
