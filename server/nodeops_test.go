package mirrornode

import (
	"bytes"
	"context"
	"crypto/rand"
	"io"
	"io/fs"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func init() { sealedKDFIterations = sealedMinKDFIterations }

var nodeOpsTestPassphrase = []byte("correct horse battery staple")

func openSealed(data, passphrase []byte) ([]byte, error) {
	reader, err := newOpenReader(bytes.NewReader(data), passphrase)
	if err != nil {
		return nil, err
	}
	return io.ReadAll(reader)
}

func TestSealedStreamRoundTripsAndRejectsDamage(t *testing.T) {
	headerBytes := len(sealedMagic) + sealedSaltBytes + 4 + sealedNoncePrefixBytes
	for _, size := range []int{0, 1, sealedChunkBytes, sealedChunkBytes + 1, 2*sealedChunkBytes + 17} {
		plain := make([]byte, size)
		rand.Read(plain)
		var sealed bytes.Buffer
		writer, err := newSealWriter(&sealed, nodeOpsTestPassphrase)
		if err != nil {
			t.Fatal(err)
		}
		for offset := 0; offset < len(plain); {
			n := min(len(plain)-offset, 70001)
			if _, err := writer.Write(plain[offset : offset+n]); err != nil {
				t.Fatal(err)
			}
			offset += n
		}
		if err := writer.Close(); err != nil {
			t.Fatal(err)
		}
		data := sealed.Bytes()
		if bytes.Contains(data, plain[:min(size, 64)]) && size >= 64 {
			t.Fatalf("size %d: plaintext visible in sealed output", size)
		}
		if got, err := openSealed(data, nodeOpsTestPassphrase); err != nil || !bytes.Equal(got, plain) {
			t.Fatalf("size %d: round trip failed: %v", size, err)
		}
		if _, err := openSealed(data, []byte("the wrong passphrase")); err == nil {
			t.Fatalf("size %d: wrong passphrase opened", size)
		}
		if _, err := openSealed(data[:len(data)-1], nodeOpsTestPassphrase); err == nil {
			t.Fatalf("size %d: truncated file opened", size)
		}
		tampered := bytes.Clone(data)
		tampered[len(tampered)-5] ^= 1
		if _, err := openSealed(tampered, nodeOpsTestPassphrase); err == nil {
			t.Fatalf("size %d: tampered file opened", size)
		}
		if size > sealedChunkBytes {
			// Dropping every chunk after the first leaves a valid non-final
			// chunk; the missing final chunk must still be detected.
			firstChunk := data[:headerBytes+sealedChunkBytes+16]
			if _, err := openSealed(firstChunk, nodeOpsTestPassphrase); err == nil {
				t.Fatalf("size %d: file without its final chunk opened", size)
			}
		}
	}
}

func TestReadPassphraseFileRequiresOwnerOnlyStrongPassphrase(t *testing.T) {
	dir := t.TempDir()
	good := filepath.Join(dir, "good")
	os.WriteFile(good, append(bytes.Clone(nodeOpsTestPassphrase), '\n'), 0600)
	if passphrase, err := ReadPassphraseFile(good); err != nil || !bytes.Equal(passphrase, nodeOpsTestPassphrase) {
		t.Fatalf("owner-only passphrase rejected: %v %q", err, passphrase)
	}
	shared := filepath.Join(dir, "shared")
	os.WriteFile(shared, nodeOpsTestPassphrase, 0644)
	os.Chmod(shared, 0644)
	if _, err := ReadPassphraseFile(shared); err == nil {
		t.Fatal("group-readable passphrase file accepted")
	}
	short := filepath.Join(dir, "short")
	os.WriteFile(short, []byte("short\n"), 0600)
	if _, err := ReadPassphraseFile(short); err == nil {
		t.Fatal("short passphrase accepted")
	}
}

func TestRefsListingDigestIgnoresHeadAndOrder(t *testing.T) {
	first, count := refsListingSHA256("aaa refs/heads/main\nbbb refs/tags/v1\nccc HEAD\n")
	second, _ := refsListingSHA256("bbb refs/tags/v1\naaa refs/heads/main\n")
	if first != second || count != 2 {
		t.Fatalf("digest depends on order or HEAD: %s %s %d", first, second, count)
	}
}

type nodeOpsFixture struct {
	configPath string
	cfg        Config
	gateway    GatewayConfig
	gitDir     string
	forkDir    string
	head       string
}

func runTestGit(t *testing.T, dir string, args ...string) string {
	t.Helper()
	command := exec.Command("git", args...)
	command.Dir = dir
	command.Env = append(os.Environ(),
		"GIT_CONFIG_GLOBAL="+os.DevNull, "GIT_CONFIG_NOSYSTEM=1",
		"GIT_AUTHOR_NAME=ForkMesh Test", "GIT_AUTHOR_EMAIL=test@forkmesh.test",
		"GIT_COMMITTER_NAME=ForkMesh Test", "GIT_COMMITTER_EMAIL=test@forkmesh.test")
	out, err := command.CombinedOutput()
	if err != nil {
		t.Fatalf("git %v: %v\n%s", args, err, out)
	}
	return strings.TrimSpace(string(out))
}

// newNodeOpsFixture bootstraps a node with a populated repository, a release
// asset, a connector token, an empty fork repository, and scratch state that
// a backup must leave out.
func newNodeOpsFixture(t *testing.T, node string) nodeOpsFixture {
	t.Helper()
	root := t.TempDir()
	state := filepath.Join(root, "state")
	tools := filepath.Join(root, "tools")
	if err := os.MkdirAll(tools, 0700); err != nil {
		t.Fatal(err)
	}
	script := filepath.Join(tools, "mirror_gateway.py")
	python := filepath.Join(tools, "python3")
	for _, path := range []string{script, python} {
		if err := os.WriteFile(path, []byte("#!/bin/sh\n"), 0700); err != nil {
			t.Fatal(err)
		}
	}
	configPath := filepath.Join(state, "mirror-node.json")
	if _, err := Bootstrap(BootstrapOptions{
		ConfigPath: configPath, StateDirectory: state,
		Node: node, Owner: "forkmesh", Repository: "forkmesh",
		Upstream:        "https://forkmesh.test/forkmesh/forkmesh",
		CatalogURL:      "https://forkmesh.test/api/repositories",
		PublicOrigin:    "https://" + node + ".forkmesh.test",
		RouterPublicKey: "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
		GatewayScript:   script, Python: python,
		Cloudflared: "/usr/local/bin/cloudflared", Version: "0.9.1",
	}); err != nil {
		t.Fatal(err)
	}
	cfg, err := LoadConfig(configPath)
	if err != nil {
		t.Fatal(err)
	}
	gateway, _, err := loadGatewayConfig(cfg.GatewayConfig)
	if err != nil {
		t.Fatal(err)
	}
	gitDir := gateway.Repositories[0].GitDir
	runTestGit(t, "", "init", "--bare", "--quiet", gitDir)
	runTestGit(t, "", "--git-dir="+gitDir, "symbolic-ref", "HEAD", "refs/heads/main")
	work := filepath.Join(root, "work")
	runTestGit(t, "", "init", "--quiet", work)
	os.WriteFile(filepath.Join(work, "README.md"), []byte("hello "+node+"\n"), 0644)
	runTestGit(t, work, "add", ".")
	runTestGit(t, work, "commit", "--quiet", "-m", "initial")
	runTestGit(t, work, "tag", "-a", "v1", "-m", "v1")
	runTestGit(t, work, "push", "--quiet", gitDir,
		"HEAD:refs/heads/main", "HEAD:refs/heads/forkmesh/pulls", "refs/tags/v1")
	head := runTestGit(t, "", "--git-dir="+gitDir, "rev-parse", "refs/heads/main")

	if err := os.WriteFile(filepath.Join(gateway.Repositories[0].ReleaseStore, "asset.bin"), []byte("release bytes"), 0600); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(cfg.ConnectorToken, []byte("connector-token"), 0600); err != nil {
		t.Fatal(err)
	}
	gatewayDir := filepath.Dir(cfg.GatewayConfig)
	for path, content := range map[string]string{
		filepath.Join(gatewayDir, pullJobsDirectoryName, ".apply-123", "scratch"):    "scratch",
		filepath.Join(gatewayDir, agentJobsDirectoryName, "work", "session", "file"): "scratch",
		filepath.Join(gatewayDir, agentJobsDirectoryName, "state.json"):              "{}",
	} {
		os.MkdirAll(filepath.Dir(path), 0700)
		if err := os.WriteFile(path, []byte(content), 0600); err != nil {
			t.Fatal(err)
		}
	}
	forkRoot := filepath.Join(state, "forks")
	forkDir := filepath.Join(forkRoot, "alice-demo.git")
	runTestGit(t, "", "init", "--bare", "--quiet", forkDir)
	gateway.WritableRepositoryRoot = forkRoot
	if _, err := writeGatewayConfig(cfg.GatewayConfig, gateway); err != nil {
		t.Fatal(err)
	}
	return nodeOpsFixture{configPath: configPath, cfg: cfg, gateway: gateway, gitDir: gitDir, forkDir: forkDir, head: head}
}

func (f nodeOpsFixture) backup(t *testing.T) (string, NodeBackupReport) {
	t.Helper()
	output := filepath.Join(t.TempDir(), "node.fmbackup")
	report, err := CreateNodeBackup(context.Background(), NodeBackupOptions{
		ConfigPath: f.configPath, Output: output, Passphrase: nodeOpsTestPassphrase,
	})
	if err != nil {
		t.Fatal(err)
	}
	return output, report
}

func TestNodeBackupRestoresRepositoriesIdentityAndConfiguration(t *testing.T) {
	f := newNodeOpsFixture(t, "mirror17")
	archive, report := f.backup(t)
	if report.Repositories != 2 || report.Refs != 3 || report.Node != "mirror17" ||
		report.PublicKey != f.gateway.Node.PublicKey {
		t.Fatalf("unexpected backup report: %+v", report)
	}
	if info, err := os.Stat(archive); err != nil || info.Mode().Perm() != 0600 {
		t.Fatalf("backup is not owner-only: %v %+v", err, info)
	}
	raw, _ := os.ReadFile(archive)
	if bytes.Contains(raw, []byte("connector-token")) || bytes.Contains(raw, []byte("PRIVATE KEY")) {
		t.Fatal("backup exposes secrets in the clear")
	}
	if _, err := CreateNodeBackup(context.Background(), NodeBackupOptions{
		ConfigPath: f.configPath, Output: archive, Passphrase: nodeOpsTestPassphrase,
	}); err == nil {
		t.Fatal("backup overwrote an existing archive")
	}
	if status := ReadRecoveryStatus(f.cfg.GatewayConfig, time.Now()); status.LastBackupAt != report.CreatedAt ||
		status.LastBackupBytes != report.Bytes {
		t.Fatalf("backup was not recorded: %+v", status)
	}

	work := t.TempDir()
	drill, err := RestoreNodeBackup(context.Background(), NodeRestoreOptions{
		Archive: archive, Passphrase: nodeOpsTestPassphrase, ConfigPath: f.configPath,
		Drill: true, WorkDirectory: work,
	})
	if err != nil {
		t.Fatal(err)
	}
	if !drill.Drill || drill.Refs != 3 || drill.Repositories != 2 || !drill.WithinObjective ||
		drill.PublicKey != f.gateway.Node.PublicKey {
		t.Fatalf("unexpected drill report: %+v", drill)
	}
	if entries, _ := os.ReadDir(work); len(entries) != 0 {
		t.Fatalf("restore drill left scratch state behind: %v", entries)
	}
	if _, err := RestoreNodeBackup(context.Background(), NodeRestoreOptions{
		Archive: archive, Passphrase: []byte("the wrong passphrase"), ConfigPath: f.configPath,
		Drill: true, WorkDirectory: work,
	}); err == nil {
		t.Fatal("restore accepted the wrong passphrase")
	}
	if _, err := RestoreNodeBackup(context.Background(), NodeRestoreOptions{
		Archive: archive, Passphrase: nodeOpsTestPassphrase, ConfigPath: "/etc/forkmesh/other.json",
		Drill: true, WorkDirectory: work,
	}); err == nil {
		t.Fatal("restore accepted a backup of another configuration")
	}

	root := t.TempDir()
	restored, err := RestoreNodeBackup(context.Background(), NodeRestoreOptions{
		Archive: archive, Passphrase: nodeOpsTestPassphrase, ConfigPath: f.configPath,
		Root: root, WorkDirectory: work,
	})
	if err != nil {
		t.Fatal(err)
	}
	if restored.Drill || restored.Root != root || restored.Refs != 3 {
		t.Fatalf("unexpected restore report: %+v", restored)
	}
	target := func(path string) string { return filepath.Join(root, path) }
	identity, err := LoadIdentity(target(f.cfg.IdentityKey))
	if err != nil || identity.PublicKey() != f.gateway.Node.PublicKey {
		t.Fatalf("identity not restored: %v", err)
	}
	if got := runTestGit(t, "", "--git-dir="+target(f.gitDir), "rev-parse", "refs/heads/main"); got != f.head {
		t.Fatalf("main restored at %s, want %s", got, f.head)
	}
	if got := runTestGit(t, "", "--git-dir="+target(f.gitDir), "symbolic-ref", "HEAD"); got != "refs/heads/main" {
		t.Fatalf("HEAD restored as %s", got)
	}
	for _, ref := range []string{"refs/tags/v1", "refs/heads/forkmesh/pulls"} {
		runTestGit(t, "", "--git-dir="+target(f.gitDir), "rev-parse", "--verify", ref)
	}
	if !isBackupRepository(target(f.forkDir)) {
		t.Fatal("empty fork repository not restored")
	}
	gatewayDir := filepath.Dir(f.cfg.GatewayConfig)
	for path, want := range map[string]string{
		filepath.Join(f.gateway.Repositories[0].ReleaseStore, "asset.bin"): "release bytes",
		f.cfg.ConnectorToken: "connector-token",
		filepath.Join(gatewayDir, agentJobsDirectoryName, "state.json"): "{}",
	} {
		if got, err := os.ReadFile(target(path)); err != nil || string(got) != want {
			t.Fatalf("%s restored as %q: %v", path, got, err)
		}
	}
	for _, path := range []string{
		filepath.Join(gatewayDir, pullJobsDirectoryName, ".apply-123"),
		filepath.Join(gatewayDir, agentJobsDirectoryName, "work"),
	} {
		if _, err := os.Lstat(target(path)); !os.IsNotExist(err) {
			t.Fatalf("scratch state %s was restored: %v", path, err)
		}
	}
	if _, err := RestoreNodeBackup(context.Background(), NodeRestoreOptions{
		Archive: archive, Passphrase: nodeOpsTestPassphrase, ConfigPath: f.configPath,
		Root: root, WorkDirectory: work,
	}); err == nil || !strings.Contains(err.Error(), "refusing to overwrite") {
		t.Fatalf("second restore over existing state: %v", err)
	}
	if _, err := LoadIdentity(target(f.cfg.IdentityKey)); err != nil {
		t.Fatalf("refused restore damaged existing state: %v", err)
	}
}

func TestNodeRestoreRollsBackOnlyWhatItCreated(t *testing.T) {
	f := newNodeOpsFixture(t, "mirror17")
	archive, _ := f.backup(t)
	root := t.TempDir()
	blocker := filepath.Join(root, f.cfg.GatewayConfig)
	if err := os.MkdirAll(filepath.Dir(blocker), 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(blocker, []byte("operator data"), 0600); err != nil {
		t.Fatal(err)
	}
	_, err := RestoreNodeBackup(context.Background(), NodeRestoreOptions{
		Archive: archive, Passphrase: nodeOpsTestPassphrase, ConfigPath: f.configPath,
		Root: root, WorkDirectory: t.TempDir(),
	})
	if err == nil || !strings.Contains(err.Error(), "refusing to overwrite") {
		t.Fatalf("restore over existing gateway config: %v", err)
	}
	filepath.WalkDir(root, func(path string, entry fs.DirEntry, walkErr error) error {
		if walkErr != nil {
			t.Fatal(walkErr)
		}
		if path != blocker && !pathWithin(filepath.Dir(blocker), path) {
			t.Errorf("failed restore left %s behind", path)
		}
		return nil
	})
	if got, _ := os.ReadFile(blocker); string(got) != "operator data" {
		t.Fatalf("failed restore changed existing data: %q", got)
	}
}

func TestNodeIdentityExportImportMovesKeyWithoutLoss(t *testing.T) {
	source := newNodeOpsFixture(t, "mirror17")
	export := filepath.Join(t.TempDir(), "identity.fmkey")
	if _, err := ExportNodeIdentity(source.cfg, "mirror17", export, nodeOpsTestPassphrase, time.Now()); err != nil {
		t.Fatal(err)
	}
	if raw, _ := os.ReadFile(export); bytes.Contains(raw, []byte("PRIVATE KEY")) {
		t.Fatal("identity export is not sealed")
	}
	if report, err := ImportNodeIdentity(source.cfg, IdentityImportOptions{
		Input: export, Passphrase: nodeOpsTestPassphrase, Now: time.Now(),
	}); err != nil || report.Action != "unchanged" {
		t.Fatalf("re-import on the source node: %v %+v", err, report)
	}

	target := newNodeOpsFixture(t, "mirror17")
	previous := target.gateway.Node.PublicKey
	if _, err := ImportNodeIdentity(target.cfg, IdentityImportOptions{
		Input: export, Passphrase: nodeOpsTestPassphrase, Now: time.Now(),
	}); err == nil || !strings.Contains(err.Error(), "--replace-identity") {
		t.Fatalf("import silently replaced a different key: %v", err)
	}
	dry, err := ImportNodeIdentity(target.cfg, IdentityImportOptions{
		Input: export, Passphrase: nodeOpsTestPassphrase, Replace: true, DryRun: true, Now: time.Now(),
	})
	if err != nil || dry.Action != "replace" || dry.PreviousPublicKey != previous {
		t.Fatalf("dry run: %v %+v", err, dry)
	}
	if identity, _ := LoadIdentity(target.cfg.IdentityKey); identity.PublicKey() != previous {
		t.Fatal("dry run changed the identity key")
	}
	replaced, err := ImportNodeIdentity(target.cfg, IdentityImportOptions{
		Input: export, Passphrase: nodeOpsTestPassphrase, Replace: true, Now: time.Unix(1700000000, 0),
	})
	if err != nil || replaced.Action != "replace" {
		t.Fatalf("replace: %v %+v", err, replaced)
	}
	if identity, err := LoadIdentity(target.cfg.IdentityKey); err != nil || identity.PublicKey() != source.gateway.Node.PublicKey {
		t.Fatalf("imported key not installed: %v", err)
	}
	if kept, err := LoadIdentity(replaced.PreservedPreviousKey); err != nil || kept.PublicKey() != previous {
		t.Fatalf("previous key not preserved: %v", err)
	}
	if gateway, _, err := loadGatewayConfig(target.cfg.GatewayConfig); err != nil ||
		gateway.Node.PublicKey != source.gateway.Node.PublicKey {
		t.Fatalf("gateway still advertises the old key: %v", err)
	}
	if _, err := ImportNodeIdentity(target.cfg, IdentityImportOptions{
		Input: export, Passphrase: []byte("the wrong passphrase"), Now: time.Now(),
	}); err == nil {
		t.Fatal("import accepted the wrong passphrase")
	}

	other := newNodeOpsFixture(t, "mirror18")
	if _, err := ImportNodeIdentity(other.cfg, IdentityImportOptions{
		Input: export, Passphrase: nodeOpsTestPassphrase, Replace: true, Now: time.Now(),
	}); err == nil || !strings.Contains(err.Error(), "mirror17") {
		t.Fatalf("import accepted another node's key: %v", err)
	}
}

func preflightStatus(report PreflightReport, name string) string {
	for _, check := range report.Checks {
		if check.Name == name {
			return check.Status
		}
	}
	return ""
}

func TestNodePreflightReportsIdentityCompatibilityAndRecovery(t *testing.T) {
	f := newNodeOpsFixture(t, "mirror17")
	options := PreflightOptions{ConfigPath: f.configPath}
	report := RunNodePreflight(context.Background(), options)
	for _, check := range report.Checks {
		// Free space depends on the test machine; everything else must pass.
		if check.Status == "error" && !strings.HasPrefix(check.Name, "capacity") {
			t.Fatalf("unexpected preflight error:\n%s", report.Text())
		}
	}
	for name, want := range map[string]string{
		"identity": "ok", "git": "ok", "version": "ok", "connector-token": "ok",
		"backup": "warn", "restore-drill": "warn",
	} {
		if got := preflightStatus(report, name); got != want {
			t.Fatalf("%s = %q, want %q:\n%s", name, got, want, report.Text())
		}
	}

	archive, _ := f.backup(t)
	drill, err := RestoreNodeBackup(context.Background(), NodeRestoreOptions{
		Archive: archive, Passphrase: nodeOpsTestPassphrase, ConfigPath: f.configPath,
		Drill: true, WorkDirectory: t.TempDir(),
	})
	if err != nil {
		t.Fatal(err)
	}
	if err := RecordRestoreDrill(f.cfg, drill); err != nil {
		t.Fatal(err)
	}
	report = RunNodePreflight(context.Background(), options)
	if preflightStatus(report, "backup") != "ok" || preflightStatus(report, "restore-drill") != "ok" {
		t.Fatalf("recorded backup and drill not recognized:\n%s", report.Text())
	}
	options.Now = func() time.Time { return time.Now().Add(100 * 24 * time.Hour) }
	report = RunNodePreflight(context.Background(), options)
	if preflightStatus(report, "backup") != "warn" || preflightStatus(report, "restore-drill") != "warn" {
		t.Fatalf("stale backup and drill not flagged:\n%s", report.Text())
	}

	f.gateway.Node.PublicKey = "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB"
	if _, err := writeGatewayConfig(f.cfg.GatewayConfig, f.gateway); err != nil {
		t.Fatal(err)
	}
	report = RunNodePreflight(context.Background(), PreflightOptions{ConfigPath: f.configPath})
	if report.OK || preflightStatus(report, "identity") != "error" {
		t.Fatalf("split identity not reported as an error:\n%s", report.Text())
	}
}

func TestNodeRestoreDrillRunsNoHookFromTheArchive(t *testing.T) {
	f := newNodeOpsFixture(t, "mirror18")
	marker := filepath.Join(t.TempDir(), "hook-ran")
	hook := filepath.Join(f.gitDir, "hooks", "reference-transaction")
	if err := os.MkdirAll(filepath.Dir(hook), 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(hook, []byte("#!/bin/sh\ntouch '"+marker+"'\n"), 0700); err != nil {
		t.Fatal(err)
	}
	archive, _ := f.backup(t)
	if _, err := os.Lstat(marker); !os.IsNotExist(err) {
		t.Fatalf("backup ran the repository hook: %v", err)
	}
	if _, err := RestoreNodeBackup(context.Background(), NodeRestoreOptions{
		Archive: archive, Passphrase: nodeOpsTestPassphrase, ConfigPath: f.configPath,
		Drill: true, WorkDirectory: t.TempDir(),
	}); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Lstat(marker); !os.IsNotExist(err) {
		t.Fatalf("restore drill ran a hook taken from the archive: %v", err)
	}
}
