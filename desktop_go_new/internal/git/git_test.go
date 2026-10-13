package git

import (
	"context"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"
)

func testEnv(t testing.TB) []string {
	t.Helper()
	home := t.TempDir()
	return []string{
		"PATH=" + os.Getenv("PATH"),
		"HOME=" + home,
		"XDG_CONFIG_HOME=" + filepath.Join(home, ".config"),
		"GIT_CONFIG_NOSYSTEM=1",
		"GIT_CONFIG_GLOBAL=" + filepath.Join(home, "gitconfig"),
		"GIT_AUTHOR_NAME=Test",
		"GIT_AUTHOR_EMAIL=test@example.invalid",
		"GIT_COMMITTER_NAME=Test",
		"GIT_COMMITTER_EMAIL=test@example.invalid",
	}
}

func testRunner(t testing.TB) *Runner {
	t.Helper()
	if _, err := exec.LookPath("git"); err != nil {
		t.Skip("git not installed")
	}
	return &Runner{BaseEnv: testEnv(t)}
}

func must(t testing.TB, r *Runner, dir string, args ...string) Result {
	t.Helper()
	result := r.Run(context.Background(), dir, args, Options{})
	if !result.OK {
		t.Fatalf("git %v: %s", args, result.Error)
	}
	return result
}

func initRepo(t testing.TB, r *Runner) string {
	t.Helper()
	dir := t.TempDir()
	must(t, r, dir, "init", "-q", "-b", "main")
	write(t, filepath.Join(dir, "a.txt"), "one\n")
	must(t, r, dir, "add", "a.txt")
	must(t, r, dir, "commit", "-q", "-m", "first")
	return dir
}

func write(t testing.TB, path, text string) {
	t.Helper()
	if err := os.WriteFile(path, []byte(text), 0o644); err != nil {
		t.Fatal(err)
	}
}

func TestClassification(t *testing.T) {
	cases := []struct {
		args   []string
		writes bool
		merge  bool
	}{
		{[]string{"merge", "main"}, true, true},
		{[]string{"-C", "/x", "commit", "-m", "x"}, true, false},
		{[]string{"-c", "a=b", "pull"}, true, true},
		{[]string{"stash", "list"}, false, false},
		{[]string{"stash", "show"}, false, false},
		{[]string{"stash"}, true, false},
		{[]string{"stash", "push", "-m", "x"}, true, false},
		{[]string{"apply", "x.patch"}, false, false},
		{[]string{"apply", "--3way", "x.patch"}, true, false},
		{[]string{"apply", "-3", "x.patch"}, true, false},
		{[]string{"log", "-n", "5"}, false, false},
		{[]string{"status"}, false, false},
		{[]string{"read-tree", "HEAD"}, true, false},
		{[]string{"--git-dir", "x", "update-index", "--refresh"}, true, false},
	}
	for _, c := range cases {
		if got := WritesIndex(c.args); got != c.writes {
			t.Errorf("WritesIndex(%v) = %v", c.args, got)
		}
		if got := MergeLike(c.args); got != c.merge {
			t.Errorf("MergeLike(%v) = %v", c.args, got)
		}
	}
	if Subcommand([]string{"-C", "dir", "-c", "x=y", "log"}) != "log" {
		t.Fatal("subcommand")
	}
	if TimeoutFor([]string{"merge", "main"}, 0) != MergeTimeout {
		t.Fatal("merge timeout")
	}
	if TimeoutFor([]string{"log"}, 0) != ReadTimeout {
		t.Fatal("read timeout")
	}
	if TimeoutFor([]string{"commit", "-m", "x"}, 0) != WriteTimeout {
		t.Fatal("write timeout")
	}
	if TimeoutFor([]string{"worktree", "add"}, 0) != WriteTimeout {
		t.Fatal("worktree timeout")
	}
	if TimeoutFor([]string{"rebase", "main"}, 10*time.Minute) != 10*time.Minute {
		t.Fatal("longer caller timeout kept")
	}
}

func TestEnviron(t *testing.T) {
	env := Environ([]string{"PATH=/bin", "GIT_DIR=/evil", "GIT_INDEX_FILE=/x", "LC_ALL=de_DE", "KEEP=1"}, "EXTRA=2")
	joined := "\n" + strings.Join(env, "\n") + "\n"
	for _, want := range []string{"PATH=/bin", "KEEP=1", "EXTRA=2", "GIT_TERMINAL_PROMPT=0", "GIT_OPTIONAL_LOCKS=0", "LC_ALL=C"} {
		if !strings.Contains(joined, "\n"+want+"\n") {
			t.Errorf("missing %s in %v", want, env)
		}
	}
	for _, gone := range []string{"GIT_DIR=", "GIT_INDEX_FILE=", "LC_ALL=de_DE"} {
		if strings.Contains(joined, "\n"+gone) {
			t.Errorf("kept %s", gone)
		}
	}
	override := Environ([]string{"PATH=/bin"}, "GIT_INDEX_FILE=/tmp/idx")
	if !strings.Contains(strings.Join(override, "\n"), "GIT_INDEX_FILE=/tmp/idx") {
		t.Fatal("explicit override dropped")
	}
}

func TestRepositoryKeys(t *testing.T) {
	r := testRunner(t)
	dir := initRepo(t, r)
	sub := filepath.Join(dir, "sub", "deeper")
	if err := os.MkdirAll(sub, 0o755); err != nil {
		t.Fatal(err)
	}
	wt := filepath.Join(dir, ".worktrees", "agent")
	must(t, r, dir, "worktree", "add", "-q", "-b", "agent", wt)
	if GitDirForCheckout(dir) != filepath.Join(cleanAbsolute(dir), ".git") {
		t.Fatalf("git dir %s", GitDirForCheckout(dir))
	}
	if !strings.Contains(GitDirForCheckout(wt), filepath.Join(".git", "worktrees")) {
		t.Fatalf("worktree git dir %s", GitDirForCheckout(wt))
	}
	key := RepositoryKey(dir)
	if RepositoryKey(sub) != key || RepositoryKey(wt) != key {
		t.Fatalf("keys differ: %s %s %s", key, RepositoryKey(sub), RepositoryKey(wt))
	}
	other := initRepo(t, r)
	if RepositoryKey(other) == key {
		t.Fatal("two repositories share a key")
	}
	if GitDirForCheckout(t.TempDir()) != "" {
		t.Fatal("plain dir has a git dir")
	}
}

func TestLaneSerializesOneRepository(t *testing.T) {
	r := &Runner{}
	dir := t.TempDir()
	var active, peak int32
	var wg sync.WaitGroup
	for i := 0; i < 8; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			_, lease, err := r.Hold(context.Background(), dir)
			if err != nil {
				t.Error(err)
				return
			}
			defer lease.Release()
			n := atomic.AddInt32(&active, 1)
			for {
				p := atomic.LoadInt32(&peak)
				if n <= p || atomic.CompareAndSwapInt32(&peak, p, n) {
					break
				}
			}
			time.Sleep(5 * time.Millisecond)
			atomic.AddInt32(&active, -1)
		}()
	}
	wg.Wait()
	if peak != 1 {
		t.Fatalf("peak concurrency %d", peak)
	}
	r.lanesMu.Lock()
	left := len(r.lanes)
	r.lanesMu.Unlock()
	if left != 0 {
		t.Fatalf("%d lanes left", left)
	}
}

func TestLaneTwoRepositoriesOverlap(t *testing.T) {
	r := &Runner{}
	_, first, _ := r.Hold(context.Background(), t.TempDir())
	done := make(chan struct{})
	go func() {
		_, second, _ := r.Hold(context.Background(), t.TempDir())
		second.Release()
		close(done)
	}()
	select {
	case <-done:
	case <-time.After(2 * time.Second):
		t.Fatal("second repository waited for the first")
	}
	first.Release()
}

func TestLaneArrivalOrderAndReentry(t *testing.T) {
	r := &Runner{}
	dir := t.TempDir()
	ctx, holder, _ := r.Hold(context.Background(), dir)
	nestedCtx, nested, err := r.Hold(ctx, filepath.Join(dir, "."))
	if err != nil || !nested.Holds() || nestedCtx != ctx {
		t.Fatal("owner could not re-enter")
	}
	nested.Release()
	var mu sync.Mutex
	order := []int{}
	var wg sync.WaitGroup
	for i := 0; i < 5; i++ {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			_, lease, _ := r.Hold(context.Background(), dir)
			mu.Lock()
			order = append(order, i)
			mu.Unlock()
			lease.Release()
		}(i)
		for {
			r.lanesMu.Lock()
			l := r.lanes[RepositoryKey(dir)]
			r.lanesMu.Unlock()
			l.mu.Lock()
			n := len(l.waiters)
			l.mu.Unlock()
			if n == i+1 {
				break
			}
			time.Sleep(time.Millisecond)
		}
	}
	holder.Release()
	wg.Wait()
	for i, got := range order {
		if got != i {
			t.Fatalf("order %v", order)
		}
	}
}

func TestLaneBypassAfterWait(t *testing.T) {
	r := &Runner{QueueWait: 50 * time.Millisecond}
	dir := t.TempDir()
	_, holder, _ := r.Hold(context.Background(), dir)
	defer holder.Release()
	_, lease, err := r.Hold(context.Background(), dir)
	if err != nil {
		t.Fatal(err)
	}
	if !lease.Bypassed() || lease.Holds() {
		t.Fatal("expected bypass")
	}
	lease.Release()
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if _, _, err := r.Hold(ctx, dir); err == nil {
		t.Fatal("cancelled wait succeeded")
	}
}

func age(t testing.TB, path string, by time.Duration) {
	t.Helper()
	old := time.Now().Add(-by)
	if err := os.Chtimes(path, old, old); err != nil {
		t.Fatal(err)
	}
}

func TestLockStates(t *testing.T) {
	r := testRunner(t)
	dir := initRepo(t, r)
	lock := filepath.Join(dir, ".git", "index.lock")
	if InspectLock(lock, LockStale) != LockMissing {
		t.Fatal("missing")
	}
	write(t, lock, "")
	if InspectLock(lock, LockStale) != LockFresh {
		t.Fatal("fresh")
	}
	if RemoveLockIfStale(lock, LockStale) {
		t.Fatal("fresh lock removed")
	}
	age(t, lock, 2*time.Minute)
	if InspectLock(lock, LockStale) != LockStaleState {
		t.Fatal("stale")
	}
	if runtime.GOOS == "linux" {
		file, err := os.Open(lock)
		if err != nil {
			t.Fatal(err)
		}
		if InspectLock(lock, LockStale) != LockHeld {
			t.Fatal("open lock not held")
		}
		if RemoveLockIfStale(lock, LockStale) {
			t.Fatal("held lock removed")
		}
		file.Close()
	}
	if !RemoveLockIfStale(lock, LockStale) {
		t.Fatal("stale lock kept")
	}
	if exists(lock) {
		t.Fatal("lock still there")
	}
	other := filepath.Join(dir, "notes.txt")
	write(t, other, "x")
	age(t, other, time.Hour)
	if RemoveLockIfStale(other, LockStale) || !exists(other) {
		t.Fatal("removed a non-lock file")
	}
}

func TestLockHeldByGitInCheckout(t *testing.T) {
	if runtime.GOOS != "linux" {
		t.Skip("linux only")
	}
	r := testRunner(t)
	dir := initRepo(t, r)
	lock := filepath.Join(dir, ".git", "index.lock")
	write(t, lock, "")
	age(t, lock, 2*time.Minute)
	bin := t.TempDir()
	gitPath := filepath.Join(bin, "git")
	if err := os.Symlink("/bin/sleep", gitPath); err != nil {
		t.Skip(err)
	}
	cmd := exec.Command(gitPath, "3")
	cmd.Dir = dir
	if err := cmd.Start(); err != nil {
		t.Skip(err)
	}
	defer func() { _ = cmd.Process.Kill(); _ = cmd.Wait() }()
	deadline := time.Now().Add(2 * time.Second)
	for readFirstLine(filepath.Join("/proc", strconv.Itoa(cmd.Process.Pid), "comm")) != "git" && time.Now().Before(deadline) {
		time.Sleep(10 * time.Millisecond)
	}
	if InspectLock(lock, LockStale) != LockHeld {
		t.Fatal("git working in the checkout does not hold the lock")
	}
	nested := filepath.Join(dir, ".worktrees", "x")
	if err := os.MkdirAll(nested, 0o755); err != nil {
		t.Fatal(err)
	}
	must(t, r, dir, "worktree", "add", "-q", "-b", "x2", filepath.Join(dir, ".worktrees", "x2"))
	wtLock := filepath.Join(GitDirForCheckout(filepath.Join(dir, ".worktrees", "x2")), "index.lock")
	write(t, wtLock, "")
	age(t, wtLock, 2*time.Minute)
	if InspectLock(wtLock, LockStale) == LockHeld {
		t.Fatal("main checkout git holds the linked worktree's lock")
	}
}

func TestRetryPolicy(t *testing.T) {
	r := testRunner(t)
	dir := initRepo(t, r)
	lock := filepath.Join(dir, ".git", "index.lock")
	message := "fatal: Unable to create '" + lock + "': File exists.\n\nAnother git process seems to be running"

	fresh := RetryPolicy{Dir: dir}
	write(t, lock, "")
	waits := []time.Duration{}
	for {
		next := fresh.Next(message)
		if !next.Retry {
			break
		}
		waits = append(waits, next.Wait)
	}
	want := []time.Duration{25, 50, 100, 200, 400}
	if len(waits) != len(want) {
		t.Fatalf("waits %v", waits)
	}
	for i := range want {
		if waits[i] != want[i]*time.Millisecond {
			t.Fatalf("waits %v", waits)
		}
	}
	if !exists(lock) {
		t.Fatal("fresh lock removed")
	}

	age(t, lock, 2*time.Minute)
	stale := RetryPolicy{Dir: dir}
	first := stale.Next(message)
	if !first.Retry || first.RemovedLock != lock || first.Wait != 0 || exists(lock) {
		t.Fatalf("stale: %+v", first)
	}

	if runtime.GOOS == "linux" {
		write(t, lock, "")
		file, _ := os.Open(lock)
		held := RetryPolicy{Dir: dir}
		if next := held.Next(message); !next.Retry || next.Wait != HeldLockWait {
			t.Fatalf("held first: %+v", next)
		}
		if next := held.Next(message); next.Retry {
			t.Fatalf("held second: %+v", next)
		}
		file.Close()
		_ = os.Remove(lock)
	}

	other := RetryPolicy{Dir: dir}
	packed := "error: Unable to create '" + filepath.Join(dir, ".git", "packed-refs.lock") + "': File exists."
	if next := other.Next(packed); !next.Retry || next.Wait != 25*time.Millisecond {
		t.Fatalf("other lock: %+v", next)
	}

	refused := RetryPolicy{Dir: dir}
	if next := refused.Next("fatal: Unable to write index."); next.Retry {
		t.Fatal("merge refusal without a lock retried")
	}
	write(t, lock, "")
	if next := refused.Next("fatal: Unable to write index."); !next.Retry || next.Wait != 25*time.Millisecond {
		t.Fatalf("merge refusal with a lock: %+v", next)
	}
	_ = os.Remove(lock)
	never := RetryPolicy{Dir: dir}
	if next := never.Next("fatal: unable to write index file"); next.Retry {
		t.Fatal("lost lock retried")
	}
	if next := never.Next("CONFLICT (content): Merge conflict in a.txt"); next.Retry {
		t.Fatal("conflict retried")
	}
}

func TestRunClearsOrphanedLockAndKeepsFreshOne(t *testing.T) {
	r := testRunner(t)
	dir := initRepo(t, r)
	lock := filepath.Join(dir, ".git", "index.lock")
	write(t, filepath.Join(dir, "b.txt"), "b\n")
	write(t, lock, "")
	age(t, lock, 2*time.Minute)
	result := r.Run(context.Background(), dir, []string{"add", "b.txt"}, Options{})
	if !result.OK || result.Attempts != 2 {
		t.Fatalf("stale lock: %+v", result)
	}
	write(t, filepath.Join(dir, "c.txt"), "c\n")
	write(t, lock, "")
	result = r.Run(context.Background(), dir, []string{"add", "c.txt"}, Options{})
	if result.OK || result.Attempts != LockBackoffTries || !exists(lock) {
		t.Fatalf("fresh lock: ok=%v attempts=%d", result.OK, result.Attempts)
	}
	_ = os.Remove(lock)
}

func TestRunWaitsOutBriefLock(t *testing.T) {
	r := testRunner(t)
	dir := initRepo(t, r)
	lock := filepath.Join(dir, ".git", "index.lock")
	write(t, filepath.Join(dir, "b.txt"), "b\n")
	write(t, lock, "")
	go func() {
		time.Sleep(300 * time.Millisecond)
		_ = os.Remove(lock)
	}()
	result := r.Run(context.Background(), dir, []string{"add", "b.txt"}, Options{})
	if !result.OK || result.Attempts < 2 {
		t.Fatalf("%+v", result)
	}
}

func TestParallelWorktreesThroughQueue(t *testing.T) {
	r := testRunner(t)
	dir := initRepo(t, r)
	var wg sync.WaitGroup
	errs := make(chan string, 64)
	for i := 0; i < 4; i++ {
		name := "agent" + strconv.Itoa(i)
		wt := filepath.Join(dir, ".worktrees", name)
		must(t, r, dir, "worktree", "add", "-q", "-b", name, wt)
		wg.Add(1)
		go func(wt string, i int) {
			defer wg.Done()
			for j := 0; j < 3; j++ {
				write(t, filepath.Join(wt, "f"+strconv.Itoa(i)+"-"+strconv.Itoa(j)), "x\n")
				for _, args := range [][]string{{"add", "-A"}, {"commit", "-q", "-m", "w"}, {"merge", "-q", "--no-edit", "main"}} {
					if res := r.Run(context.Background(), wt, args, Options{}); !res.OK {
						errs <- strings.Join(args, " ") + ": " + res.Error
					}
				}
			}
		}(wt, i)
	}
	wg.Add(1)
	go func() {
		defer wg.Done()
		for j := 0; j < 3; j++ {
			write(t, filepath.Join(dir, "main"+strconv.Itoa(j)), "m\n")
			for _, args := range [][]string{{"add", "-A", "--", "main" + strconv.Itoa(j)}, {"commit", "-q", "-m", "m"}} {
				if res := r.Run(context.Background(), dir, args, Options{}); !res.OK {
					errs <- strings.Join(args, " ") + ": " + res.Error
				}
			}
		}
	}()
	wg.Wait()
	close(errs)
	for err := range errs {
		t.Error(err)
	}
}

func TestRunTimeoutOutputCapAndEnv(t *testing.T) {
	if runtime.GOOS == "windows" {
		t.Skip("shell script fake git")
	}
	bin := t.TempDir()
	fake := filepath.Join(bin, "fake-git")
	write(t, fake, "#!/bin/sh\nif [ \"$3\" = sleep ]; then exec sleep 5; fi\nif [ \"$3\" = env ]; then env; exit 0; fi\nprintf 'abcdefghijklmnopqrstuvwxyz'\n")
	if err := os.Chmod(fake, 0o755); err != nil {
		t.Fatal(err)
	}
	r := &Runner{Binary: fake, BaseEnv: []string{"PATH=" + os.Getenv("PATH"), "GIT_DIR=/nope"}}
	started := time.Now()
	result := r.Run(context.Background(), t.TempDir(), []string{"sleep"}, Options{Timeout: 200 * time.Millisecond})
	if !result.TimedOut || result.OK || !strings.HasPrefix(result.Error, "git timed out after 0s: git sleep") {
		t.Fatalf("timeout: %+v", result)
	}
	if time.Since(started) > 3*time.Second {
		t.Fatal("SIGTERM did not stop the child")
	}
	capped := r.Run(context.Background(), t.TempDir(), []string{"print"}, Options{MaxOutput: 10})
	if !capped.OK || string(capped.Output) != "abcdefghij" || !capped.Truncated {
		t.Fatalf("cap: %+v", capped)
	}
	env := r.Run(context.Background(), t.TempDir(), []string{"env"}, Options{})
	text := string(env.Output)
	if !strings.Contains(text, "GIT_TERMINAL_PROMPT=0") || strings.Contains(text, "GIT_DIR=") || !strings.Contains(text, "LC_ALL=C") {
		t.Fatalf("env: %s", text)
	}
}

func TestStatus(t *testing.T) {
	r := testRunner(t)
	upstream := initRepo(t, r)
	clone := t.TempDir()
	must(t, r, clone, "clone", "-q", upstream, ".")
	status, result := r.Status(context.Background(), clone)
	if !result.OK || status.Branch != "main" || status.Upstream != "origin/main" || !status.Clean || status.Ahead != 0 || status.Behind != 0 || len(status.Head) < 40 {
		t.Fatalf("clean: %+v %s", status, result.Error)
	}
	write(t, filepath.Join(upstream, "u.txt"), "u\n")
	must(t, r, upstream, "add", "u.txt")
	must(t, r, upstream, "commit", "-q", "-m", "up")
	must(t, r, clone, "fetch", "-q")
	write(t, filepath.Join(clone, "local.txt"), "l\n")
	must(t, r, clone, "add", "local.txt")
	must(t, r, clone, "commit", "-q", "-m", "local")
	write(t, filepath.Join(clone, "staged.txt"), "s\n")
	must(t, r, clone, "add", "staged.txt")
	write(t, filepath.Join(clone, "a.txt"), "changed\n")
	write(t, filepath.Join(clone, "new.txt"), "n\n")
	status, _ = r.Status(context.Background(), clone)
	if status.Ahead != 1 || status.Behind != 1 || status.Staged != 1 || status.Unstaged != 1 || status.Untracked != 1 || status.Clean {
		t.Fatalf("dirty: %+v", status)
	}
	must(t, r, clone, "mv", "staged.txt", "renamed.txt")
	status, _ = r.Status(context.Background(), clone)
	if status.Staged != 1 || status.Untracked != 1 {
		t.Fatalf("rename: %+v", status)
	}
	must(t, r, clone, "checkout", "-q", "--detach")
	status, _ = r.Status(context.Background(), clone)
	if !status.Detached || status.Branch != "" {
		t.Fatalf("detached: %+v", status)
	}
	empty := t.TempDir()
	must(t, r, empty, "init", "-q", "-b", "trunk")
	status, _ = r.Status(context.Background(), empty)
	if !status.Unborn || status.Branch != "trunk" || !status.Clean {
		t.Fatalf("unborn: %+v", status)
	}
	gone := ParseStatus([]byte("# branch.oid abc\x00# branch.head x\x00# branch.upstream origin/x\x00"))
	if !gone.UpstreamGone {
		t.Fatal("gone upstream")
	}
	conflict := ParseStatus([]byte("u UU N... 100644 100644 100644 100644 a b c f\x00"))
	if conflict.Conflicted != 1 || conflict.Clean {
		t.Fatal("conflict")
	}
}
