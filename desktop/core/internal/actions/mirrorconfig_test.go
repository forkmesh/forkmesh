package actions

import (
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
	"time"

	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/qsettings"
	"forkmesh.com/desktop-core/internal/rpc"
)

func mirrorConfigFixture(t *testing.T) (MirrorConfigEnv, *iniStore, string) {
	if runtime.GOOS == "windows" {
		t.Skip("POSIX permissions")
	}
	root := t.TempDir()
	source := filepath.Join(root, "source")
	runGit(t, root, "init", "-q", "-b", "main", source)
	_ = os.WriteFile(filepath.Join(source, "README.md"), []byte("x\n"), 0o644)
	runGit(t, source, "add", "--all")
	runGit(t, source, "commit", "-q", "-m", "one")
	gateway := filepath.Join(root, "gateway")
	if err := os.MkdirAll(gateway, 0o700); err != nil {
		t.Fatal(err)
	}
	config, _ := json.Marshal(map[string]any{"schemaVersion": 1, "type": mirrorGatewayType, "sourceRepository": source,
		"repositoryName": "forkmesh", "nodeOwner": "mirror-a", "ownerAliases": []string{"mirror-a", "forkmesh"},
		"catalog": map[string]any{"branch": "main"}})
	gatewayPath := filepath.Join(gateway, "mirror-refresh.json")
	if err := os.WriteFile(gatewayPath, config, 0o600); err != nil {
		t.Fatal(err)
	}
	ini := newIniStore(t, filepath.Join(root, "config", "ForkMesh.conf"))
	ini.set(t, "node/machineName", "mirror-a")
	ini.set(t, "repositories/items/size", "1")
	ini.set(t, "repositories/items/1/owner", "alice")
	ini.set(t, "repositories/items/1/name", "demo")
	ini.set(t, "repositories/items/1/disabledWorkflows", `.forkmesh/a.yml, .forkmesh/b.yml`)
	ini.set(t, "actions/variables", iniQuote(`@ByteArray({"OLD":"1"})`))
	env := MirrorConfigEnv{SettingsPath: ini.path, DataRoot: filepath.Join(root, "data"), Write: ini.writer(t),
		GatewayCandidates: []string{gatewayPath}, LockTimeout: 200 * time.Millisecond}
	return env, ini, gatewayPath
}

func mirrorRequest(id string) map[string]any {
	return map[string]any{"type": mirrorConfigRequestType, "schemaVersion": float64(1), "requestId": id,
		"node": "mirror-a", "actionsEnabled": true,
		"variables": map[string]any{"mode": "replace", "values": map[string]any{"TOKEN": "s3cret"}}}
}

func TestMirrorConfigurationAppliesThroughTheSettingsWriter(t *testing.T) {
	env, _, gatewayPath := mirrorConfigFixture(t)
	if got := ApplyMirrorConfiguration(map[string]any{"type": "x"}, env); got["errorCode"] != "invalid_request" || got["ok"] != false {
		t.Fatal(got)
	}
	wrongNode := mirrorRequest(strings.Repeat("a", 32))
	wrongNode["node"] = "mirror-b"
	if got := ApplyMirrorConfiguration(wrongNode, env); got["errorCode"] != "node_mismatch" {
		t.Fatal(got)
	}
	id := strings.Repeat("b", 32)
	got := ApplyMirrorConfiguration(mirrorRequest(id), env)
	if got["ok"] != true || got["variablesReplaced"] != true || got["variableCount"] != 1 || got["requestId"] != id {
		t.Fatal(got)
	}
	settings, _ := qsettings.Load(env.SettingsPath)
	if settings.String(mirrorGenerationSetting, "") != id || !settings.Bool(mirrorEnabledSetting, false) ||
		settings.String(mirrorNodeSetting, "") != "mirror-a" ||
		settings.String(mirrorStatePathSetting, "") != filepath.Join(filepath.Dir(gatewayPath), stateFileName) {
		t.Fatalf("settings after apply: %v", settings.Keys(""))
	}
	if settings.Int("repositories/items/size", 0) != 2 || settings.String("repositories/items/2/owner", "") != "forkmesh" ||
		settings.String("repositories/items/2/externalActionsRef", "") != "refs/heads/main" ||
		len(settings.Strings("repositories/items/1/disabledWorkflows")) != 2 {
		t.Fatalf("repositories: %v", settings.Keys("repositories/"))
	}
	if Variables(settings)["TOKEN"] != "s3cret" {
		t.Fatal(Variables(settings))
	}
	mirror := settings.String("repositories/items/2/mirrorPath", "")
	if _, err := os.Stat(filepath.Join(mirror, "HEAD")); err != nil {
		t.Fatalf("action mirror not prepared at %s", mirror)
	}
	if pathExists(env.JournalPath()) || pathExists(env.LockPath()) {
		t.Fatal("journal or lock left behind")
	}
}

func TestMirrorConfigurationRollsBackWhenTheWriteFails(t *testing.T) {
	env, ini, _ := mirrorConfigFixture(t)
	before, _ := os.ReadFile(ini.path)
	healthy := env.Write
	calls := 0
	env.Write = func(ctx context.Context, writes []daemon.SettingWrite) error {
		calls++
		if calls == 1 {
			_ = healthy(ctx, writes[:3])
			return rpc.Fail("desktop_unavailable")
		}
		return healthy(ctx, writes)
	}
	got := ApplyMirrorConfiguration(mirrorRequest(strings.Repeat("c", 32)), env)
	if got["ok"] != false || got["errorCode"] != "settings_write_failed" {
		t.Fatal(got)
	}
	after, _ := qsettings.Load(env.SettingsPath)
	original := qsettings.Parse(before)
	for _, key := range []string{"repositories/items/size", "repositories/items/1/owner", "node/machineName"} {
		if after.String(key, "?") != original.String(key, "!") {
			t.Fatalf("%s not restored: %q", key, after.String(key, ""))
		}
	}
	if len(after.Strings("repositories/items/1/disabledWorkflows")) != 2 || Variables(after)["OLD"] != "1" {
		t.Fatal("list or bytes value not restored")
	}
	if _, ok := after.Value(mirrorGenerationSetting); ok || pathExists(env.JournalPath()) {
		t.Fatal("rollback left the new generation or the journal")
	}
}

func TestMirrorJournalRecoveryAndTamper(t *testing.T) {
	env, ini, _ := mirrorConfigFixture(t)
	settings, _ := qsettings.Load(env.SettingsPath)
	previous, ok := captureConfiguration(settings)
	if !ok || env.prepareDirectory() != "" || !env.writeJournal(strings.Repeat("d", 32), previous) {
		t.Fatal("journal write")
	}
	info, _ := os.Stat(env.JournalPath())
	if info.Mode().Perm() != 0o600 {
		t.Fatal(info.Mode())
	}
	ini.set(t, "repositories/items/1/owner", "mallory")
	ini.set(t, mirrorGenerationSetting, strings.Repeat("e", 32))
	if code := env.RecoverPending(); code != "" {
		t.Fatal(code)
	}
	restored, _ := qsettings.Load(env.SettingsPath)
	if restored.String("repositories/items/1/owner", "") != "alice" {
		t.Fatal("recovery did not restore the snapshot")
	}
	if _, present := restored.Value(mirrorGenerationSetting); present || pathExists(env.JournalPath()) {
		t.Fatal("recovery left state behind")
	}
	data := EncodeMirrorJournal(strings.Repeat("d", 32), previous)
	if _, _, ok := DecodeMirrorJournal(data); !ok {
		t.Fatal("round trip")
	}
	tampered := []byte(strings.Replace(string(data), "alice", "alicf", 1))
	if _, _, ok := DecodeMirrorJournal(tampered); ok {
		t.Fatal("a tampered journal decoded")
	}
	if err := os.WriteFile(env.JournalPath(), tampered, 0o600); err != nil {
		t.Fatal(err)
	}
	if code := env.RecoverPending(); code != "configuration_journal_invalid" {
		t.Fatal(code)
	}
	if err := os.WriteFile(env.LockPath(), []byte("1\nforkmesh\nelsewhere\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	if code := env.RecoverPending(); code != "configuration_busy" {
		t.Fatal(code)
	}
	if _, code := env.ResetRecoveryFiles(); code != "configuration_busy" {
		t.Fatal("reset pulled a readable lock away", code)
	}
	if err := os.WriteFile(env.LockPath(), []byte("garbage"), 0o644); err != nil {
		t.Fatal(err)
	}
	removed, code := env.ResetRecoveryFiles()
	if code != "" || len(removed) != 2 || pathExists(env.JournalPath()) || pathExists(env.LockPath()) {
		t.Fatal(removed, code)
	}
}

func TestMirrorSummaryAndStateFiles(t *testing.T) {
	if runtime.GOOS == "windows" {
		t.Skip("POSIX permissions")
	}
	dir := filepath.Join(t.TempDir(), "gateway")
	if err := os.MkdirAll(dir, 0o700); err != nil {
		t.Fatal(err)
	}
	store := NewStore(filepath.Join(t.TempDir(), "actions"))
	good := NewRun()
	good.Owner, good.Name, good.WorkflowName = "alice", "demo", "CI\nline"
	good.Commit, good.Ref, good.Status = strings.Repeat("a", 40), "refs/heads/main", StatusSuccess
	created, _ := store.CreateRun(good)
	store.AppendLog(created, "secret-free tail\x07")
	bad := good
	bad.Ref = "refs/heads/../x"
	_, _ = store.CreateRun(bad)
	now := nowMs()
	summary := BuildSummary("mirror-a", store.LoadAllRuns(), store, now)
	runs := summary["runs"].([]any)
	if len(runs) != 1 || runs[0].(map[string]any)["workflow"] != "CI line" ||
		runs[0].(map[string]any)["logTail"] != "secret-free tail\n" {
		t.Fatalf("summary %v", summary)
	}
	path := filepath.Join(dir, summaryFileName)
	if !WriteSummaryFile(path, "mirror-a", store.LoadAllRuns(), store, now) {
		t.Fatal("summary not written")
	}
	info, _ := os.Stat(path)
	if info.Mode().Perm() != 0o600 {
		t.Fatal(info.Mode())
	}
	var decoded map[string]any
	data, _ := os.ReadFile(path)
	if json.Unmarshal(data, &decoded) != nil || decoded["type"] != summaryType || !strings.HasSuffix(string(data), "}\n") {
		t.Fatal(string(data))
	}
	if !WriteStateFile(filepath.Join(dir, stateFileName), "mirror-a", "running", now) {
		t.Fatal("state not written")
	}
	if WriteStateFile(filepath.Join(dir, stateFileName), "mirror-a", "bogus", now) || WriteStateFile(filepath.Join(dir, "other.json"), "mirror-a", "running", now) {
		t.Fatal("invalid state accepted")
	}
	_ = os.Chmod(dir, 0o755)
	if WriteStateFile(filepath.Join(dir, stateFileName), "mirror-a", "enabled", now) {
		t.Fatal("an unprotected parent was accepted")
	}
	if ClassifySummaryWritePolicy(SystemSummaryPath, 0, 1000, 1000, 0o700, true, false, false, false, 0, 0, 0, 0) != summaryRootGatewayHandoff {
		t.Fatal("root hand-off")
	}
}
