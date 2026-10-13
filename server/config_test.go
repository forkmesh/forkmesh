package mirrornode

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func TestLoadConfigDefaultsAndRejectsUnsafeInputs(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "node.json")
	good := `{"schemaVersion":1,"gatewayConfig":"/a","gatewayScript":"/b","python":"/c","cloudflared":"/d","connectorToken":"/e","identityKey":"/f","catalogUrl":"https://example.test/api/repositories","publishOwner":"mirror9","upstreams":{}}`
	if err := os.WriteFile(path, []byte(good), 0600); err != nil {
		t.Fatal(err)
	}
	cfg, err := LoadConfig(path)
	if err != nil {
		t.Fatal(err)
	}
	if cfg.Listen != "127.0.0.1:8791" || cfg.SyncInterval.Duration != 15*time.Second || cfg.SyncTimeout.Duration != 5*time.Minute {
		t.Fatalf("defaults not applied: %+v", cfg)
	}
	for name, raw := range map[string]string{
		"unknown":                strings.Replace(good, `"schemaVersion":1`, `"schemaVersion":1,"wat":true`, 1),
		"public listen":          strings.Replace(good, `"schemaVersion":1`, `"schemaVersion":1,"listen":"0.0.0.0:1"`, 1),
		"relative":               strings.Replace(good, `"gatewayConfig":"/a"`, `"gatewayConfig":"a"`, 1),
		"bad duration":           strings.Replace(good, `"upstreams":{}`, `"syncInterval":"1s","upstreams":{}`, 1),
		"desktop intake remnant": strings.Replace(good, `"upstreams":{}`, `"intakeProgram":"/usr/local/bin/forkmesh","upstreams":{}`, 1),
		"unknown event upstream": strings.Replace(good, `"upstreams":{}`, `"eventDrivenUpstreams":["https://mirror.test/a/b.git"],"upstreams":{}`, 1),
	} {
		t.Run(name, func(t *testing.T) {
			if err := os.WriteFile(path, []byte(raw), 0600); err != nil {
				t.Fatal(err)
			}
			if _, err := LoadConfig(path); err == nil {
				t.Fatal("expected rejection")
			}
		})
	}
	link := filepath.Join(dir, "link.json")
	if err := os.Symlink(path, link); err != nil {
		t.Fatal(err)
	}
	if _, err := LoadConfig(link); err == nil {
		t.Fatal("symlink config accepted")
	}
}

func TestDurationRejectsNonStringAndInvalid(t *testing.T) {
	for _, raw := range []string{`1`, `""`, `"nope"`, `"-1s"`} {
		var d Duration
		if err := d.UnmarshalJSON([]byte(raw)); err == nil {
			t.Fatalf("accepted %s", raw)
		}
	}
}

func TestUpdateConfigVersionIsAtomicAndRemainsStrictlyLoadable(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "node.json")
	good := `{"schemaVersion":1,"gatewayConfig":"/a","gatewayScript":"/b","python":"/c","cloudflared":"/d","connectorToken":"/e","identityKey":"/f","catalogUrl":"https://example.test/api/repositories","publishOwner":"mirror9","syncInterval":"15s","syncTimeout":"5m","version":"0.7.24","upstreams":{"forkmesh/forkmesh":["https://forkmesh.com/forkmesh/forkmesh"]},"eventDrivenUpstreams":["https://forkmesh.com/forkmesh/forkmesh"]}`
	if err := os.WriteFile(path, []byte(good), 0600); err != nil {
		t.Fatal(err)
	}
	if err := UpdateConfigVersion(path, "0.7.26"); err != nil {
		t.Fatal(err)
	}
	cfg, err := LoadConfig(path)
	if err != nil {
		t.Fatalf("updated config is not loadable: %v", err)
	}
	if cfg.Version != "0.7.26" || cfg.SyncInterval.Duration != 15*time.Second ||
		cfg.SyncTimeout.Duration != 5*time.Minute ||
		len(cfg.Upstreams["forkmesh/forkmesh"]) != 1 ||
		len(cfg.EventDrivenUpstreams) != 1 {
		t.Fatalf("version update changed unrelated configuration: %+v", cfg)
	}
	info, err := os.Stat(path)
	if err != nil {
		t.Fatal(err)
	}
	if info.Mode().Perm() != 0600 {
		t.Fatalf("updated config permissions = %v", info.Mode().Perm())
	}
	if err := UpdateConfigVersion(path, "latest;bad"); err == nil {
		t.Fatal("invalid release version accepted")
	}
}
