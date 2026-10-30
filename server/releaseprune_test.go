package mirrornode

import (
	"context"
	"encoding/json"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

type releasePruneFixture struct {
	bare  string
	work  string
	store string
	now   time.Time
}

func releasePruneGit(t *testing.T, gitDir, workTree string, args ...string) string {
	t.Helper()
	base := []string{"-c", "user.name=ForkMesh Test", "-c", "user.email=test@example.invalid",
		"-c", "commit.gpgsign=false", "--git-dir=" + gitDir}
	if workTree != "" {
		base = append(base, "--work-tree="+workTree)
	}
	command := exec.Command("git", append(base, args...)...)
	command.Dir = workTree
	command.Env = append(os.Environ(), "GIT_CONFIG_NOSYSTEM=1", "GIT_CONFIG_GLOBAL="+os.DevNull)
	out, err := command.CombinedOutput()
	if err != nil {
		t.Fatalf("git %v: %v\n%s", args, err, out)
	}
	return string(out)
}

func newReleasePruneFixture(t *testing.T) *releasePruneFixture {
	t.Helper()
	root := t.TempDir()
	f := &releasePruneFixture{
		bare: filepath.Join(root, "repo.git"),
		work: filepath.Join(root, "work"),
		now:  time.Now(),
	}
	f.store = filepath.Join(f.bare, "forkmesh-releases")
	command := exec.Command("git", "init", "--bare", "--initial-branch=main", f.bare)
	command.Env = append(os.Environ(), "GIT_CONFIG_NOSYSTEM=1", "GIT_CONFIG_GLOBAL="+os.DevNull)
	if out, err := command.CombinedOutput(); err != nil {
		t.Fatalf("git init: %v\n%s", err, out)
	}
	if err := os.MkdirAll(f.work, 0o755); err != nil {
		t.Fatal(err)
	}
	return f
}

func releaseManifest(t *testing.T, digests ...string) string {
	t.Helper()
	assets := []map[string]any{}
	for i, digest := range digests {
		assets = append(assets, map[string]any{
			"name":        "asset-" + string(rune('a'+i)),
			"blob_sha256": digest,
			"size":        4,
		})
	}
	raw, err := json.Marshal(map[string]any{
		"schema": "forkmesh-release-v2",
		"tag":    "v1.0.0",
		"assets": assets,
	})
	if err != nil {
		t.Fatal(err)
	}
	return string(raw)
}

func (f *releasePruneFixture) commit(t *testing.T, files map[string]string) {
	t.Helper()
	for path, content := range files {
		full := filepath.Join(f.work, filepath.FromSlash(path))
		if err := os.MkdirAll(filepath.Dir(full), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(full, []byte(content), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	releasePruneGit(t, f.bare, f.work, "add", "-A")
	releasePruneGit(t, f.bare, f.work, "commit", "-q", "-m", "release metadata")
}

func (f *releasePruneFixture) blob(t *testing.T, shard, hash string, age time.Duration) string {
	t.Helper()
	dir := filepath.Join(f.store, "sha256", shard, hash)
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	data := filepath.Join(dir, "data")
	if err := os.WriteFile(data, []byte("blob"), 0o644); err != nil {
		t.Fatal(err)
	}
	stamp := f.now.Add(-age)
	for _, path := range []string{data, dir} {
		if err := os.Chtimes(path, stamp, stamp); err != nil {
			t.Fatal(err)
		}
	}
	return dir
}

func (f *releasePruneFixture) put(t *testing.T, digest string, age time.Duration) string {
	t.Helper()
	return f.blob(t, digest[:2], digest, age)
}

func (f *releasePruneFixture) prune(t *testing.T, limit int) (releasePruneResult, error) {
	t.Helper()
	return pruneReleaseStore(context.Background(), f.store, []string{f.bare},
		defaultReleasePruneMinAge, limit, f.now)
}

func releasePruneExists(path string) bool {
	_, err := os.Lstat(path)
	return err == nil
}

const releasePruneOld = 30 * 24 * time.Hour

func TestPruneReleaseStoreDeletesOnlyOldUnreferencedBlobs(t *testing.T) {
	f := newReleasePruneFixture(t)
	refA := strings.Repeat("a", 64)
	refB := strings.Repeat("b", 64)
	oldShared := "aa" + strings.Repeat("f", 62)
	oldAlone := strings.Repeat("c", 64)
	young := strings.Repeat("d", 64)
	busy := strings.Repeat("e", 64)
	f.commit(t, map[string]string{
		".forkmesh/releases/latest/release.json":   releaseManifest(t, refA),
		".forkmesh/releases/beta-1/release.json":   releaseManifest(t, refA, strings.ToUpper(refB)),
		".forkmesh/releases/beta-1/SHASUMS.txt":    "ignored",
		".forkmesh/releases/beta-1/x/release.json": "ignored",
	})
	keepA := f.put(t, refA, releasePruneOld)
	keepB := f.put(t, refB, releasePruneOld)
	gone := []string{f.put(t, oldShared, releasePruneOld), f.put(t, oldAlone, releasePruneOld)}
	kept := []string{keepA, keepB, f.put(t, young, 24*time.Hour)}
	busyDir := f.put(t, busy, releasePruneOld)
	if err := os.WriteFile(filepath.Join(busyDir, "data.tmp"), []byte("x"), 0o644); err != nil {
		t.Fatal(err)
	}
	kept = append(kept, busyDir)
	malformed := []string{
		f.blob(t, "aa", strings.ToUpper("aa"+strings.Repeat("1", 62)), releasePruneOld),
		f.blob(t, "ab", "cd"+strings.Repeat("2", 62), releasePruneOld),
		f.blob(t, "zz", "zz"+strings.Repeat("3", 62), releasePruneOld),
		f.blob(t, "abc", "abc"+strings.Repeat("4", 61), releasePruneOld),
		f.blob(t, "aa", "aa"+strings.Repeat("5", 61), releasePruneOld),
	}
	fileBlob := filepath.Join(f.store, "sha256", "aa", "aa"+strings.Repeat("6", 62))
	if err := os.WriteFile(fileBlob, []byte("not a dir"), 0o644); err != nil {
		t.Fatal(err)
	}
	malformed = append(malformed, fileBlob)
	result, err := f.prune(t, releasePruneLimit)
	if err != nil {
		t.Fatal(err)
	}
	if result.Deleted != 2 || result.Bytes != 8 || result.Referenced != 2 {
		t.Fatalf("result = %+v", result)
	}
	for _, path := range gone {
		if releasePruneExists(path) {
			t.Fatalf("%s survived", path)
		}
	}
	for _, path := range append(kept, malformed...) {
		if !releasePruneExists(path) {
			t.Fatalf("%s was deleted", path)
		}
	}
	if releasePruneExists(filepath.Join(f.store, "sha256", "cc")) {
		t.Fatal("empty shard directory survived")
	}
	if !releasePruneExists(filepath.Join(f.store, "sha256", "aa")) {
		t.Fatal("shard with a referenced blob was removed")
	}
}

func TestPruneReleaseStoreKeepsEverythingWhenHeadIsStale(t *testing.T) {
	f := newReleasePruneFixture(t)
	referenced := strings.Repeat("a", 64)
	stamp := f.now.Add(-2 * defaultReleasePruneMinAge).Format(time.RFC3339)
	t.Setenv("GIT_AUTHOR_DATE", stamp)
	t.Setenv("GIT_COMMITTER_DATE", stamp)
	f.commit(t, map[string]string{
		".forkmesh/releases/latest/release.json": releaseManifest(t, referenced),
	})
	f.put(t, referenced, releasePruneOld)
	unreferenced := f.put(t, strings.Repeat("c", 64), releasePruneOld)
	result, err := f.prune(t, releasePruneLimit)
	if err == nil || !strings.Contains(err.Error(), "older than the minimum blob age") {
		t.Fatalf("err = %v", err)
	}
	if result.Deleted != 0 || !releasePruneExists(unreferenced) {
		t.Fatalf("a stale mirror pruned: %+v", result)
	}
}

func TestPruneReleaseStorePrunesOldBlobsWhileReferencedBlobsAreMissing(t *testing.T) {
	f := newReleasePruneFixture(t)
	present := strings.Repeat("a", 64)
	missing := strings.Repeat("b", 64)
	f.commit(t, map[string]string{
		".forkmesh/releases/latest/release.json": releaseManifest(t, present, missing),
	})
	kept := f.put(t, present, releasePruneOld)
	stale := f.put(t, strings.Repeat("c", 64), releasePruneOld)
	landing := f.put(t, strings.Repeat("d", 64), time.Hour)
	result, err := f.prune(t, releasePruneLimit)
	if err != nil {
		t.Fatal(err)
	}
	if result.Deleted != 1 || result.Missing != 1 || result.Referenced != 2 {
		t.Fatalf("result = %+v", result)
	}
	if releasePruneExists(stale) {
		t.Fatal("an old unreferenced blob survived because another blob was missing")
	}
	if !releasePruneExists(kept) || !releasePruneExists(landing) {
		t.Fatal("a referenced or recently landed blob was deleted")
	}
}

func TestPruneReleaseStoreKeepsEverythingWhenManifestsUnusable(t *testing.T) {
	cases := []struct {
		name  string
		files map[string]string
	}{
		{"malformed json", map[string]string{
			".forkmesh/releases/latest/release.json": releaseManifest(t, strings.Repeat("a", 64)),
			".forkmesh/releases/beta-1/release.json": "{not json",
		}},
		{"no manifests", map[string]string{".forkmesh/releases/README.md": "nothing yet"}},
		{"no assets", map[string]string{".forkmesh/releases/latest/release.json": releaseManifest(t)}},
		{"malformed asset", map[string]string{
			".forkmesh/releases/latest/release.json": releaseManifest(t, "not-a-digest"),
		}},
		{"unborn HEAD", nil},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			f := newReleasePruneFixture(t)
			if tc.files != nil {
				f.commit(t, tc.files)
			}
			f.put(t, strings.Repeat("a", 64), releasePruneOld)
			stale := f.put(t, strings.Repeat("c", 64), releasePruneOld)
			result, err := f.prune(t, releasePruneLimit)
			if err == nil {
				t.Fatal("expected an error")
			}
			if result.Deleted != 0 || !releasePruneExists(stale) {
				t.Fatalf("pruned with unusable manifests: %+v", result)
			}
		})
	}
}

func TestPruneReleaseStoreCapsDeletionsPerRun(t *testing.T) {
	f := newReleasePruneFixture(t)
	referenced := strings.Repeat("a", 64)
	f.commit(t, map[string]string{
		".forkmesh/releases/latest/release.json": releaseManifest(t, referenced),
	})
	f.put(t, referenced, releasePruneOld)
	for _, c := range []string{"1", "2", "3"} {
		f.put(t, strings.Repeat(c, 64), releasePruneOld)
	}
	result, err := f.prune(t, 2)
	if err != nil || result.Deleted != 2 {
		t.Fatalf("result = %+v, err = %v", result, err)
	}
	result, err = f.prune(t, 2)
	if err != nil || result.Deleted != 1 {
		t.Fatalf("second run result = %+v, err = %v", result, err)
	}
}

func TestDaemonPruneReleaseStoresSharesOneStoreAcrossAliases(t *testing.T) {
	f := newReleasePruneFixture(t)
	referenced := strings.Repeat("a", 64)
	f.commit(t, map[string]string{
		".forkmesh/releases/latest/release.json": releaseManifest(t, referenced),
	})
	keep := f.put(t, referenced, releasePruneOld)
	stale := f.put(t, strings.Repeat("c", 64), releasePruneOld)
	young := f.put(t, strings.Repeat("d", 64), time.Hour)
	d := &Daemon{config: Config{SyncTimeout: Duration{time.Minute}}}
	d.gateway.Repositories = []Repository{
		{Owner: "canonical", Name: "repo", GitDir: f.bare, ReleaseStore: f.store},
		{Owner: "node", Name: "repo", GitDir: f.bare, ReleaseStore: f.store},
		{Owner: "nostore", Name: "repo", GitDir: f.bare},
	}
	d.pruneReleaseStores(context.Background())
	if releasePruneExists(stale) || !releasePruneExists(keep) || !releasePruneExists(young) {
		t.Fatalf("stale=%v keep=%v young=%v", releasePruneExists(stale),
			releasePruneExists(keep), releasePruneExists(young))
	}
}

func TestReleasePruneConfig(t *testing.T) {
	var cfg Config
	if cfg.releasePruneInterval() != 6*time.Hour || cfg.releasePruneMinAge() != 7*24*time.Hour {
		t.Fatal("unexpected defaults")
	}
	if err := cfg.validateReleasePrune(); err != nil {
		t.Fatal(err)
	}
	raw, err := json.Marshal(cfg)
	if err != nil || strings.Contains(string(raw), "releasePrune") {
		t.Fatalf("unset prune settings were serialized: %s %v", raw, err)
	}
	cfg.ReleasePruneInterval = &Duration{time.Hour}
	cfg.ReleasePruneMinAge = &Duration{14 * 24 * time.Hour}
	if err := cfg.validateReleasePrune(); err != nil {
		t.Fatal(err)
	}
	if cfg.releasePruneInterval() != time.Hour || cfg.releasePruneMinAge() != 14*24*time.Hour {
		t.Fatal("configured values ignored")
	}
	cfg.ReleasePruneInterval = &Duration{time.Minute}
	if cfg.validateReleasePrune() == nil {
		t.Fatal("accepted a one-minute interval")
	}
	cfg.ReleasePruneInterval = nil
	cfg.ReleasePruneMinAge = &Duration{time.Hour}
	if cfg.validateReleasePrune() == nil {
		t.Fatal("accepted a one-hour minimum age")
	}
}
