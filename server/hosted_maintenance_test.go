package mirrornode

import (
	"context"
	"crypto/rand"
	"encoding/json"
	"errors"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"syscall"
	"testing"
	"time"
)

func hostedBloatedRepo(t *testing.T) (string, string) {
	t.Helper()
	if _, err := exec.LookPath("git"); err != nil {
		t.Skip("git is not installed")
	}
	root := t.TempDir()
	source := filepath.Join(root, "source")
	gitDir := filepath.Join(root, testHostedID+".git")
	run(t, root, "git", "init", "-q", "-b", "main", source)
	run(t, source, "git", "config", "user.email", "test@example.test")
	run(t, source, "git", "config", "user.name", "Test")
	for index := 0; index < 3; index++ {
		data := make([]byte, 256<<10)
		if _, err := rand.Read(data[:128<<10]); err != nil {
			t.Fatal(err)
		}
		name := filepath.Join(source, "blob"+string(rune('a'+index)))
		if err := os.WriteFile(name, data, 0o600); err != nil {
			t.Fatal(err)
		}
		run(t, source, "git", "add", ".")
		run(t, source, "git", "commit", "-q", "-m", "commit")
	}
	run(t, root, "git", "init", "-q", "--bare", gitDir)
	run(t, root, "git", "--git-dir="+gitDir, "fetch", "-q", source, "+refs/heads/*:refs/heads/*")
	for _, extra := range [][]string{{"--window=0"}, {"--window=0", "--compression=0", "--no-reuse-object"}} {
		args := append(append([]string{"--git-dir=" + gitDir, "pack-objects", "-q", "--all", "--revs"},
			extra...), filepath.Join(gitDir, "objects", "pack", "pack"))
		command := exec.Command("git", args...)
		command.Stdin = strings.NewReader("")
		if output, err := command.CombinedOutput(); err != nil {
			t.Fatalf("pack-objects: %v %s", err, output)
		}
	}
	return gitDir, run(t, ".", "git", "--git-dir="+gitDir, "rev-parse", "refs/heads/main")
}

func TestHostedRepositoryBytesCountsObjectsOnly(t *testing.T) {
	gitDir, _ := hostedBloatedRepo(t)
	ctx := context.Background()
	if err := os.WriteFile(filepath.Join(gitDir, "unrelated"), make([]byte, 4<<20), 0o600); err != nil {
		t.Fatal(err)
	}
	bytes := hostedRepositoryBytes(ctx, gitDir)
	if bytes <= 0 || bytes > hostedDirectoryBytes(gitDir)-(3<<20) {
		t.Fatalf("repository bytes %d, directory %d", bytes, hostedDirectoryBytes(gitDir))
	}
	if missing := hostedRepositoryBytes(ctx, filepath.Join(t.TempDir(), "none.git")); missing != 0 {
		t.Fatalf("missing repository = %d", missing)
	}
}

func TestHostedMaintenanceRollsUpDuplicatePacks(t *testing.T) {
	gitDir, head := hostedBloatedRepo(t)
	ctx := context.Background()
	packs, _ := hostedObjectCounts(gitDir)
	if packs < 2 {
		t.Fatalf("packs before = %d", packs)
	}
	before := hostedRepositoryBytes(ctx, gitDir)
	if hostedMaintenanceDue(gitDir, false) {
		t.Fatal("due without a marker or a scan")
	}
	if err := os.WriteFile(filepath.Join(gitDir, hostedMaintenanceMarker), nil, 0o600); err != nil {
		t.Fatal(err)
	}
	if !hostedMaintenanceDue(gitDir, false) {
		t.Fatal("a marker must make the repository due")
	}
	if err := maintainHostedRepository(ctx, gitDir); err != nil {
		t.Fatal(err)
	}
	packs, loose := hostedObjectCounts(gitDir)
	after := hostedRepositoryBytes(ctx, gitDir)
	if packs != 1 || loose != 0 || after*3 > before*2 {
		t.Fatalf("after maintenance: packs=%d loose=%d bytes %d -> %d", packs, loose, before, after)
	}
	if _, err := os.Lstat(filepath.Join(gitDir, hostedMaintenanceMarker)); !errors.Is(err, os.ErrNotExist) {
		t.Fatalf("marker left behind: %v", err)
	}
	if got := run(t, ".", "git", "--git-dir="+gitDir, "rev-parse", "refs/heads/main"); got != head {
		t.Fatalf("main moved: %s", got)
	}
	run(t, ".", "git", "--git-dir="+gitDir, "fsck", "--no-dangling")
	for _, setting := range hostedRepositorySettings {
		if got := gitValue(ctx, gitDir, "config", "--local", "--get", setting[0]); got != setting[1] {
			t.Fatalf("%s = %q", setting[0], got)
		}
	}
}

func TestHostedMaintenanceWaitsForAPushInFlight(t *testing.T) {
	gitDir, _ := hostedBloatedRepo(t)
	ctx := context.Background()
	if err := ensureHostedMaintenanceFiles(ctx, gitDir); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(gitDir, hostedMaintenanceMarker), nil, 0o600); err != nil {
		t.Fatal(err)
	}
	push, err := os.OpenFile(filepath.Join(gitDir, hostedMaintenanceLock), os.O_RDWR, 0o600)
	if err != nil {
		t.Fatal(err)
	}
	defer push.Close()
	if err := syscall.Flock(int(push.Fd()), syscall.LOCK_SH); err != nil {
		t.Fatal(err)
	}
	packs, _ := hostedObjectCounts(gitDir)
	if err := maintainHostedRepository(ctx, gitDir); !errors.Is(err, errHostedMaintenanceBusy) {
		t.Fatalf("maintenance during a push = %v", err)
	}
	if now, _ := hostedObjectCounts(gitDir); now != packs {
		t.Fatalf("packs changed during a push: %d -> %d", packs, now)
	}
	if !hostedMaintenanceDue(gitDir, false) {
		t.Fatal("the marker must survive a skipped run")
	}
	_ = syscall.Flock(int(push.Fd()), syscall.LOCK_UN)
	if err := maintainHostedRepository(ctx, gitDir); err != nil {
		t.Fatal(err)
	}
}

func TestHostedReplicatorMaintainsDueRepositories(t *testing.T) {
	daemon := hostedTestDaemon(t, "fmnext-n1", "http://127.0.0.1:1")
	gitDir, _ := hostedBloatedRepo(t)
	target := daemon.hosted.gitDir(testHostedID)
	if err := os.Rename(gitDir, target); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(filepath.Join(daemon.hosted.cfg.Root, ".trash", "x.git"), 0o700); err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), time.Minute)
	defer cancel()
	if got := daemon.hosted.maintain(ctx, false); got != 0 {
		t.Fatalf("maintained %d without a marker or scan", got)
	}
	for index := 0; index <= hostedMaintenancePacks; index++ {
		blob := run(t, ".", "sh", "-c", "echo extra-"+string(rune('a'+index))+
			" | git --git-dir="+target+" hash-object -w --stdin")
		command := exec.Command("git", "--git-dir="+target, "pack-objects", "-q",
			filepath.Join(target, "objects", "pack", "pack"))
		command.Stdin = strings.NewReader(blob + "\n")
		if output, err := command.CombinedOutput(); err != nil {
			t.Fatalf("pack-objects: %v %s", err, output)
		}
	}
	if got := daemon.hosted.maintain(ctx, true); got != 1 {
		t.Fatalf("maintained %d repositories on a scan", got)
	}
	if packs, _ := hostedObjectCounts(target); packs > 2 {
		t.Fatalf("packs after the scan = %d", packs)
	}
	if _, err := os.Lstat(filepath.Join(target, hostedMaintenanceLock)); err != nil {
		t.Fatalf("lock file: %v", err)
	}
}

func TestHostedCopyStateReportsTheWholeHistoryCachedPerTip(t *testing.T) {
	daemon := hostedTestDaemon(t, "fmnext-n1", "http://127.0.0.1:1")
	gitDir, head := hostedBloatedRepo(t)
	target := daemon.hosted.gitDir(testHostedID)
	if err := os.Rename(gitDir, target); err != nil {
		t.Fatal(err)
	}
	run(t, ".", "git", "--git-dir="+target, "symbolic-ref", "HEAD", "refs/heads/main")
	ctx := context.Background()
	state := daemon.hosted.copyState(ctx, testHostedID)
	if state.Commit != head || state.CommitCount != 3 || state.MergeCount != 0 {
		t.Fatalf("state = %+v", state)
	}
	total := 0
	for _, week := range state.ActivityWeeks {
		total += week
	}
	if len(state.ActivityWeeks) != 52 || total != 3 || len(state.MergeActivityWeeks) != 52 {
		t.Fatalf("activity = %v", state.ActivityWeeks)
	}
	cached := daemon.hosted.stats[testHostedID]
	cached.CommitCount = 99
	daemon.hosted.stats[testHostedID] = cached
	if again := daemon.hosted.copyState(ctx, testHostedID); again.CommitCount != 99 {
		t.Fatalf("an unchanged tip recounted: %d", again.CommitCount)
	}
	daemon.hosted.clock = func() time.Time { return time.Now().Add(48 * time.Hour) }
	if later := daemon.hosted.copyState(ctx, testHostedID); later.CommitCount != 3 {
		t.Fatalf("a new day kept a stale count: %d", later.CommitCount)
	}
	body, err := json.Marshal(state)
	if err != nil || !strings.Contains(string(body), `"commitCount":3`) ||
		!strings.Contains(string(body), `"commit":"`+head+`"`) {
		t.Fatalf("report = %s %v", body, err)
	}
}
