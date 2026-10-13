package mirrornode

import (
	"context"
	"encoding/json"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

const roleBaseConfig = `{"schemaVersion":1,"gatewayConfig":"/a","gatewayScript":"/b","python":"/c","cloudflared":"/d","connectorToken":"/e","identityKey":"/k/key.pem","catalogUrl":"https://example.test/api/repositories","publishOwner":"mirror9","upstreams":{}`

func loadRoleConfig(t *testing.T, extra string) (Config, error) {
	t.Helper()
	path := filepath.Join(t.TempDir(), "node.json")
	if err := os.WriteFile(path, []byte(roleBaseConfig+extra+"}"), 0600); err != nil {
		t.Fatal(err)
	}
	return LoadConfig(path)
}

func TestConfigWithoutRoleIsAMirrorAndExistingFieldsKeepWorking(t *testing.T) {
	cfg, err := loadRoleConfig(t, `,"enableActions":true`)
	if err != nil {
		t.Fatal(err)
	}
	if cfg.Role != RoleMirror || !cfg.EnableActions || cfg.DisableAgentJobs || cfg.Runner != nil || cfg.RunnerPool != nil {
		t.Fatalf("legacy config changed meaning: %+v", cfg)
	}
}

func TestCoreRoleForcesAgentJobsAndActionsOff(t *testing.T) {
	cfg, err := loadRoleConfig(t, `,"role":"core","enableActions":true`)
	if err != nil {
		t.Fatal(err)
	}
	if !cfg.DisableAgentJobs || cfg.EnableActions {
		t.Fatalf("core still runs untrusted code: %+v", cfg)
	}
	if strings.Join(cfg.roleOverrides, ",") != "agent jobs,actions" {
		t.Fatalf("overrides to log = %v", cfg.roleOverrides)
	}
	quiet, err := loadRoleConfig(t, `,"role":"core","disableAgentJobs":true`)
	if err != nil || len(quiet.roleOverrides) != 0 {
		t.Fatalf("a core that already had them off logged overrides: %v %v", quiet.roleOverrides, err)
	}
}

func TestRunnerRoleDefaultsAndDisablesServing(t *testing.T) {
	path := filepath.Join(t.TempDir(), "node.json")
	raw := `{"schemaVersion":1,"role":"runner","gatewayConfig":"/s/gw.json","identityKey":"/s/key.pem","runner":{"appUrl":"https://app.next.forkmesh.com","allowedOwners":["Alice","alice","bob"]}}`
	if err := os.WriteFile(path, []byte(raw), 0600); err != nil {
		t.Fatal(err)
	}
	cfg, err := LoadConfig(path)
	if err != nil {
		t.Fatal(err)
	}
	r := cfg.Runner
	if !cfg.DisableCatalog || !cfg.DisableCloudflared || !cfg.DisableMergeExecutor || cfg.DisableAgentJobs {
		t.Fatalf("runner still serves: %+v", cfg)
	}
	if r.MaxConcurrentRuns != 1 || r.WorkRoot != "/s/runner-work" || r.JobMemoryMax != "700M" ||
		r.JobMemorySwapMax != "0" || r.JobCPUQuota != "90%" || r.AppURL != "https://app.next.forkmesh.com" ||
		strings.Join(r.AllowedOwners, ",") != "alice,bob" {
		t.Fatalf("runner defaults = %+v", r)
	}
	// The written-back form stays loadable (UpdateConfigVersion rewrites it).
	if err := UpdateConfigVersion(path, "1.2.3"); err != nil {
		t.Fatal(err)
	}
	if again, err := LoadConfig(path); err != nil || again.Version != "1.2.3" || again.Role != RoleRunner {
		t.Fatalf("reload = %+v %v", again, err)
	}
}

func TestRunnerAndPoolConfigRejectUnsafeValues(t *testing.T) {
	runner := func(extra string) string {
		return `{"schemaVersion":1,"role":"runner","gatewayConfig":"/s/gw.json","identityKey":"/s/key.pem","runner":{"appUrl":"https://app.test"` + extra + `}}`
	}
	cases := map[string]string{
		"bad role":            strings.Replace(roleBaseConfig+"}", `"schemaVersion":1`, `"schemaVersion":1,"role":"boss"`, 1),
		"runner on mirror":    strings.Replace(roleBaseConfig+"}", `"schemaVersion":1`, `"schemaVersion":1,"runner":{}`, 1),
		"pool on mirror":      strings.Replace(roleBaseConfig+"}", `"schemaVersion":1`, `"schemaVersion":1,"runnerPool":{"enabled":true}`, 1),
		"no app url":          `{"schemaVersion":1,"role":"runner","gatewayConfig":"/s/gw.json","identityKey":"/s/key.pem"}`,
		"plain http app":      strings.Replace(runner(""), "https://app.test", "http://app.test", 1),
		"too many runs":       runner(`,"maxConcurrentRuns":9`),
		"relative work":       runner(`,"workRoot":"work"`),
		"root work":           runner(`,"workRoot":"/"`),
		"unclean work":        runner(`,"workRoot":"/s/../etc"`),
		"bad memory":          runner(`,"jobMemoryMax":"lots"`),
		"bad quota":           runner(`,"jobCPUQuota":"90"`),
		"bad swap":            runner(`,"jobMemorySwapMax":"-1"`),
		"bad owner":           runner(`,"allowedOwners":["../x"]`),
		"runner upstreams":    strings.Replace(runner(""), `"role":"runner"`, `"role":"runner","upstreams":{"a/b":["https://x/a/b"]}`, 1),
		"unknown runner key":  runner(`,"sudo":true`),
		"pool without limits": strings.Replace(roleBaseConfig+"}", `"schemaVersion":1`, `"schemaVersion":1,"role":"core","runnerPool":{"enabled":true,"region":"ewr","plan":"p","snapshotId":"s","apiKeyFile":"/k"}`, 1),
		"pool bad provider":   strings.Replace(roleBaseConfig+"}", `"schemaVersion":1`, `"schemaVersion":1,"role":"core","runnerPool":{"provider":"aws"}`, 1),
		"pool too many":       strings.Replace(roleBaseConfig+"}", `"schemaVersion":1`, `"schemaVersion":1,"role":"core","runnerPool":{"maxBurstRunners":11}`, 1),
	}
	for name, raw := range cases {
		t.Run(name, func(t *testing.T) {
			path := filepath.Join(t.TempDir(), "node.json")
			if err := os.WriteFile(path, []byte(raw), 0600); err != nil {
				t.Fatal(err)
			}
			if _, err := LoadConfig(path); err == nil {
				t.Fatal("expected rejection")
			}
		})
	}
	// Loopback HTTP is accepted for local development and tests.
	cfg, err := loadRoleConfigRaw(t, strings.Replace(runner(""), "https://app.test", "http://127.0.0.1:8080", 1))
	if err != nil || cfg.Runner.AppURL != "http://127.0.0.1:8080" {
		t.Fatalf("loopback app = %+v %v", cfg.Runner, err)
	}
	// A disabled pool on a core is accepted and gets its defaults.
	cfg, err = loadRoleConfig(t, `,"role":"core","runnerPool":{}`)
	if err != nil || cfg.RunnerPool.Enabled || cfg.RunnerPool.MaxBurstRunners != 2 ||
		cfg.RunnerPool.IdleTimeout.Duration != 10*time.Minute || cfg.RunnerPool.ScaleUpAfter.Duration != 2*time.Minute {
		t.Fatalf("pool defaults = %+v %v", cfg.RunnerPool, err)
	}
}

func loadRoleConfigRaw(t *testing.T, raw string) (Config, error) {
	t.Helper()
	path := filepath.Join(t.TempDir(), "node.json")
	if err := os.WriteFile(path, []byte(raw), 0600); err != nil {
		t.Fatal(err)
	}
	return LoadConfig(path)
}

// writeRoleGateway writes the node identity document a daemon loads.
func writeRoleGateway(t *testing.T, identity *Identity, repos []Repository) string {
	t.Helper()
	path := filepath.Join(t.TempDir(), "mirror-gateway", "config.json")
	if err := os.MkdirAll(filepath.Dir(path), 0700); err != nil {
		t.Fatal(err)
	}
	var gateway GatewayConfig
	gateway.SchemaVersion = 1
	gateway.Node.Name = "runner1"
	gateway.Node.PublicKey = identity.PublicKey()
	gateway.Repositories = repos
	if _, err := writeGatewayConfig(path, gateway); err != nil {
		t.Fatal(err)
	}
	return path
}

func TestCoreDaemonHasNoAgentsOrActions(t *testing.T) {
	identity, key := testIdentity(t)
	_, bare := makeBareRepo(t)
	path := writeRoleGateway(t, identity, []Repository{{Owner: "alice", Name: "proj",
		Visibility: "public", Enabled: true, GitDir: bare}})
	cfg := Config{Role: RoleCore, GatewayConfig: path, IdentityKey: key, EnableActions: true,
		CatalogURL: "https://app.next.forkmesh.com/api/repositories", PublishOwner: "alice"}
	// Not through LoadConfig: NewDaemon itself keeps a core free of them.
	daemon, err := NewDaemon(cfg)
	if err != nil {
		t.Fatal(err)
	}
	if daemon.agents != nil || daemon.actions != nil || daemon.pool != nil {
		t.Fatal("core daemon would run agent jobs, Actions or a pool")
	}
}

func TestRunnerDaemonKeepsNoRepositoriesAndStartsNoChildren(t *testing.T) {
	identity, key := testIdentity(t)
	_, bare := makeBareRepo(t)
	withRepo := writeRoleGateway(t, identity, []Repository{{Owner: "alice", Name: "proj",
		Visibility: "public", Enabled: true, GitDir: bare}})
	runnerCfg := func(gateway string) Config {
		cfg := Config{Role: RoleRunner, GatewayConfig: gateway, IdentityKey: key,
			Listen: freeAddress(t), Runner: &RunnerConfig{AppURL: "http://127.0.0.1:1",
				WorkRoot: filepath.Join(t.TempDir(), "work"), MaxConcurrentRuns: 1,
				JobMemoryMax: "700M", JobCPUQuota: "90%"}}
		if err := cfg.applyRole(); err != nil {
			t.Fatal(err)
		}
		return cfg
	}
	if _, err := NewDaemon(runnerCfg(withRepo)); err == nil {
		t.Fatal("a runner accepted a gateway document with repositories")
	}
	cfg := runnerCfg(writeRoleGateway(t, identity, nil))
	daemon, err := NewDaemon(cfg)
	if err != nil {
		t.Fatal(err)
	}
	if daemon.agents == nil || daemon.agents.limit() != 1 || daemon.collaborationCatchUp || daemon.forkJobsEnabled {
		t.Fatalf("runner daemon wiring: agents=%v catchUp=%v forks=%v", daemon.agents, daemon.collaborationCatchUp, daemon.forkJobsEnabled)
	}
	if _, ok := daemon.agents.backend.(*runnerBackend); !ok {
		t.Fatal("runner daemon does not use the runner backend")
	}
	before, _ := os.ReadFile(cfg.GatewayConfig)
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan error, 1)
	go func() { done <- daemon.Run(ctx, "/usr/bin/true") }()
	var status Status
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		response, err := http.Get("http://" + cfg.Listen + "/v1/status")
		if err == nil {
			_ = json.NewDecoder(response.Body).Decode(&status)
			response.Body.Close()
			if status.SyncCount > 0 {
				break
			}
		}
		time.Sleep(50 * time.Millisecond)
	}
	cancel()
	if err := <-done; err != nil {
		t.Fatal(err)
	}
	if status.Node != "runner1" || status.SyncCount == 0 {
		t.Fatalf("runner status = %+v", status)
	}
	running, _ := daemon.supervisor.Snapshot()
	if len(running) != 0 {
		t.Fatalf("runner started child processes: %v", running)
	}
	if _, err := os.Stat(filepath.Join(filepath.Dir(cfg.GatewayConfig), "mirror-gateway")); err == nil {
		t.Fatal("runner wrote gateway state")
	}
	if after, _ := os.ReadFile(cfg.GatewayConfig); string(after) != string(before) {
		t.Fatal("runner rewrote its identity document as a serving gateway config")
	}
}
