package actions

import (
	"bytes"
	"encoding/json"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
	"testing"
	"time"
)

const fakeSystemdRun = `#!/bin/sh
printf '%s\n' "$@" > "$FORKMESH_FAKE_DIR/systemd-run.args"
while [ "$#" -gt 0 ]; do
  if [ "$1" = "--" ]; then shift; exec "$@"; fi
  shift
done
exit 98
`

const fakeBwrap = `#!/bin/bash
printf '%s\n' "$@" > "$FORKMESH_FAKE_DIR/bwrap.args"
env > "$FORKMESH_FAKE_DIR/bwrap.env"
args=("$@")
workspace=""; home=""; tmp=""; release=""; cache=""; shell=""
for ((i=0; i<${#args[@]}; i++)); do
  a="${args[$i]}"
  if [ "$a" = "--bind" ] || [ "$a" = "--ro-bind" ]; then
    host="${args[$((i+1))]}"; inside="${args[$((i+2))]}"
    case "$inside" in
      /workspace) workspace="$host" ;;
      /home/forkmesh) home="$host" ;;
      /tmp) tmp="$host" ;;
      /release-cas) release="$host" ;;
      /cache) cache="$host" ;;
    esac
    i=$((i+2))
  elif [ "$a" = "--chdir" ]; then
    shell="${args[$((i+2))]}"
    break
  fi
done
if [ -z "$workspace" ] || [ -z "$home" ] || [ -z "$tmp" ] || [ -z "$shell" ]; then exit 99; fi
export HOME="$home" TMPDIR="$tmp" FAKE_RELEASE="$release" FAKE_CACHE="$cache"
cd "$workspace" || exit 97
exec "$shell" -se
`

const fakeSystemctl = `#!/bin/sh
printf '%s\n' "$*" >> "$FORKMESH_FAKE_DIR/systemctl.calls"
exit 0
`

type fakeHost struct {
	st       *Store
	mu       sync.Mutex
	events   []map[string]any
	names    []string
	stopping bool
	done     chan struct{}
	ok       bool
	landed   bool
	gone     bool
}

func (h *fakeHost) store() *Store { return h.st }
func (h *fakeHost) emit(event string, data map[string]any) {
	h.mu.Lock()
	h.names = append(h.names, event)
	h.events = append(h.events, data)
	h.mu.Unlock()
}
func (h *fakeHost) logf(format string, args ...any) {}
func (h *fakeHost) shuttingDown() bool {
	h.mu.Lock()
	defer h.mu.Unlock()
	return h.stopping
}
func (h *fakeHost) finishedRun(r *runner, ok, landed, prepared bool) {
	h.ok, h.landed = ok, landed
	close(h.done)
}
func (h *fakeHost) interruptedRun(r *runner) {
	h.gone = true
	close(h.done)
}

func (h *fakeHost) eventNames() []string {
	h.mu.Lock()
	defer h.mu.Unlock()
	return append([]string(nil), h.names...)
}

func (h *fakeHost) logText() string {
	h.mu.Lock()
	defer h.mu.Unlock()
	var out strings.Builder
	for index, name := range h.names {
		if name == "actions.log" {
			out.WriteString(h.events[index]["text"].(string))
			out.WriteByte('\n')
		}
	}
	return out.String()
}

type fixture struct {
	t      *testing.T
	fake   string
	mirror string
	work   string
	store  *Store
	commit string
}

func runGit(t *testing.T, dir string, args ...string) string {
	t.Helper()
	cmd := exec.Command("git", append([]string{"-C", dir}, args...)...)
	cmd.Env = append(os.Environ(), "GIT_AUTHOR_NAME=Test", "GIT_AUTHOR_EMAIL=t@example.test", "GIT_COMMITTER_NAME=Test",
		"GIT_COMMITTER_EMAIL=t@example.test", "GIT_CONFIG_NOSYSTEM=1", "GIT_CONFIG_GLOBAL=/dev/null")
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("git %v: %v\n%s", args, err, out)
	}
	return strings.TrimSpace(string(out))
}

func newFixture(t *testing.T, workflow string, extra map[string]string) *fixture {
	t.Helper()
	if runtime.GOOS != "linux" {
		t.Skip("the Actions sandbox is Linux-only")
	}
	if _, err := exec.LookPath("git"); err != nil {
		t.Skip("git missing")
	}
	root := t.TempDir()
	fake := filepath.Join(root, "fakebin")
	if err := os.MkdirAll(fake, 0o755); err != nil {
		t.Fatal(err)
	}
	for name, body := range map[string]string{"systemd-run": fakeSystemdRun, "bwrap": fakeBwrap, "systemctl": fakeSystemctl} {
		body = strings.ReplaceAll(body, "$FORKMESH_FAKE_DIR", fake)
		if err := os.WriteFile(filepath.Join(fake, name), []byte(body), 0o755); err != nil {
			t.Fatal(err)
		}
	}
	t.Setenv("PATH", fake+string(os.PathListSeparator)+os.Getenv("PATH"))
	t.Setenv("HOME", filepath.Join(root, "home"))
	resetSandboxProbe()
	work := filepath.Join(root, "work")
	if err := os.MkdirAll(filepath.Join(work, ".forkmesh"), 0o755); err != nil {
		t.Fatal(err)
	}
	runGit(t, work, "init", "-q", "-b", "main")
	if err := os.WriteFile(filepath.Join(work, ".forkmesh", "ci.yml"), []byte(workflow), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(work, "README.md"), []byte("hello\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	for name, body := range extra {
		path := filepath.Join(work, name)
		_ = os.MkdirAll(filepath.Dir(path), 0o755)
		if err := os.WriteFile(path, []byte(body), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	runGit(t, work, "add", "--all")
	runGit(t, work, "commit", "-q", "-m", "first")
	runGit(t, work, "commit", "-q", "--allow-empty", "-m", "second")
	commit := runGit(t, work, "rev-parse", "HEAD")
	mirror := filepath.Join(root, "mirror.git")
	runGit(t, root, "clone", "-q", "--bare", work, mirror)
	return &fixture{t: t, fake: fake, mirror: mirror, work: work, store: NewStore(filepath.Join(root, "actions")), commit: commit}
}

func (f *fixture) run(id int, workflow string) Run {
	run := NewRun()
	run.ID = id
	run.Owner = "owner"
	run.Name = "repo"
	run.WorkflowPath = ".forkmesh/ci.yml"
	run.WorkflowName = Parse(".forkmesh/ci.yml", workflow).Name
	run.WorkflowContent = workflow
	run.Commit = f.commit
	run.Ref = "refs/heads/main"
	run.CreatedAtMs = nowMs()
	tree, digest, failure := RepositoryStateDigest(f.mirror, f.commit)
	if failure != "" {
		f.t.Fatal(failure)
	}
	run.RepositoryTree, run.ExecutionDigest = tree, digest
	return run
}

func (f *fixture) start(run Run, variables map[string]string, limits Limits) (*fakeHost, *runner) {
	host := &fakeHost{st: f.store, done: make(chan struct{})}
	request := StartRequest{Run: run, Workflow: Parse(run.WorkflowPath, run.WorkflowContent), Mirror: f.mirror,
		RepoWorkTree: f.work, Variables: variables, Limits: limits}
	r := newRunner(host, request)
	r.post(func() { r.start(request) })
	return host, r
}

func (f *fixture) wait(host *fakeHost) {
	f.t.Helper()
	select {
	case <-host.done:
	case <-time.After(60 * time.Second):
		f.t.Fatalf("run did not finish; log:\n%s", host.logText())
	}
}

func (f *fixture) stored(id int) Run {
	f.t.Helper()
	run, ok := f.store.FindRun(id)
	if !ok {
		f.t.Fatalf("run %d not stored", id)
	}
	return run
}

func (f *fixture) fakeFile(name string) string {
	data, _ := os.ReadFile(filepath.Join(f.fake, name))
	return string(data)
}

func testLimits() Limits {
	limits := DefaultLimits()
	limits.MaxWorkspaceBytes = 64 << 20
	return limits
}

func TestRunnerSucceedsInFakeSandbox(t *testing.T) {
	workflow := "name: CI\non: push\nenv:\n  GREETING: hi ${{ vars.TOKEN }}\nsteps:\n  - name: Say\n    run: |\n      echo \"$GREETING\"\n      echo commit=$FORKMESH_COMMIT\n      mkdir -p \"$HOME/forkmesh-artifacts\"\n      echo out > \"$HOME/forkmesh-artifacts/result.txt\"\n  - run: test -f README.md && echo second\n"
	f := newFixture(t, workflow, nil)
	run := f.run(41, workflow)
	host, _ := f.start(run, map[string]string{"TOKEN": "supersecretvalue", "UNUSED": "never-exposed"}, testLimits())
	f.wait(host)
	log := host.logText()
	if !host.ok {
		t.Fatalf("run failed:\n%s", log)
	}
	stored := f.stored(41)
	if stored.Status != StatusSuccess || len(stored.StepStatuses) != 2 || stored.StepStatuses[0] != StepSuccess || stored.CurrentStepIndex != -1 {
		t.Fatalf("stored: %+v", stored)
	}
	for _, want := range []string{"==> \U0001F680 Workflow: CI (.forkmesh/ci.yml)", "==> \U0001F527 Say", "hi ***", "commit=" + f.commit,
		"==> \u2705 Approval bound to tree", "==> Network namespace isolated (egress denied).", "second", "==> \u2705 SUCCESS: All steps completed.", "==> Artifacts landed at"} {
		if !strings.Contains(log, want) {
			t.Fatalf("log misses %q:\n%s", want, log)
		}
	}
	if strings.Contains(log, "supersecretvalue") || strings.Contains(f.store.ReadLog(stored), "supersecretvalue") {
		t.Fatal("secret leaked into the log")
	}
	disk := f.store.ReadLog(stored)
	if !strings.Contains(disk, "==> \U0001F527 Say\n") || !strings.HasSuffix(disk, "All steps completed.\n") {
		t.Fatalf("disk log:\n%s", disk)
	}
	artifact := filepath.Join(f.store.ArtifactsDir(), stored.RepoKey(), "41", "result.txt")
	if data, err := os.ReadFile(artifact); err != nil || string(data) != "out\n" {
		t.Fatalf("artifact: %v %q", err, data)
	}
	if info, err := os.Stat(artifact); err != nil || info.Mode().Perm() != 0o600 {
		t.Fatalf("artifact mode: %v", info.Mode())
	}
	args := f.fakeFile("systemd-run.args")
	for _, want := range []string{"--user", "--scope", "--quiet", "--property=MemorySwapMax=0", "--property=CPUWeight=20", "--property=RuntimeMaxSec=5460"} {
		if !strings.Contains(args, want+"\n") {
			t.Fatalf("systemd-run args miss %q:\n%s", want, args)
		}
	}
	if !strings.Contains(args, "--unit=forkmesh-action-") || !strings.Contains(args, "-41-1.scope\n") {
		t.Fatalf("unit name:\n%s", args)
	}
	bwrap := f.fakeFile("bwrap.args")
	for _, want := range []string{"--die-with-parent", "--unshare-net", "--cap-drop\nALL", "--uid\n65534", "/source\n", "/workspace\n",
		"--unsetenv\nXDG_RUNTIME_DIR", "--chdir\n/workspace\n/bin/bash\n-se"} {
		if !strings.Contains(bwrap, want) {
			t.Fatalf("bwrap args miss %q:\n%s", want, bwrap)
		}
	}
	env := f.fakeFile("bwrap.env")
	for _, want := range []string{"CI=true", "FORKMESH_RUN_ID=41", "GIT_CONFIG_GLOBAL=/dev/null", "TOKEN=supersecretvalue", "GREETING=hi supersecretvalue", "PATH=/home/forkmesh/.local/bin:"} {
		if !strings.Contains(env, want) {
			t.Fatalf("step env misses %q:\n%s", want, env)
		}
	}
	if strings.Contains(env, "UNUSED=") || strings.Contains(env, "FORKMESH_HOME=") {
		t.Fatalf("step env leaks variables:\n%s", env)
	}
	names := host.eventNames()
	if names[0] != "actions.status" || names[len(names)-1] != "actions.finished" {
		t.Fatalf("events: %v", names)
	}
	deadline := time.Now().Add(10 * time.Second)
	for time.Now().Before(deadline) {
		entries, _ := os.ReadDir(f.store.SandboxDir())
		if len(entries) == 0 {
			break
		}
		time.Sleep(50 * time.Millisecond)
	}
	if entries, _ := os.ReadDir(f.store.SandboxDir()); len(entries) != 0 {
		t.Fatalf("sandbox left behind: %v", entries)
	}
}

func TestRunnerStepFailureAndStop(t *testing.T) {
	workflow := "name: Fail\non: push\nsteps:\n  - run: exit 3\n  - run: echo never\n"
	f := newFixture(t, workflow, nil)
	host, _ := f.start(f.run(5, workflow), nil, testLimits())
	f.wait(host)
	stored := f.stored(5)
	if host.ok || stored.Status != StatusFailed || stored.StepStatuses[0] != StepFailed || stored.StepStatuses[1] != StepPending {
		t.Fatalf("failure: %+v", stored)
	}
	if !strings.Contains(host.logText(), "==> \u274C FAILED: Step failed with exit code 3.") {
		t.Fatal(host.logText())
	}

	slow := "name: Slow\non: push\nsteps:\n  - name: Wait\n    run: echo started; sleep 30\n  - run: echo after\n"
	g := newFixture(t, slow, nil)
	host, r := g.start(g.run(6, slow), nil, testLimits())
	deadline := time.Now().Add(30 * time.Second)
	for !strings.Contains(host.logText(), "started") && time.Now().Before(deadline) {
		time.Sleep(20 * time.Millisecond)
	}
	r.post(r.stop)
	g.wait(host)
	stored = g.stored(6)
	killDeadline := time.Now().Add(5 * time.Second)
	for !strings.Contains(g.fakeFile("systemctl.calls"), "kill") && time.Now().Before(killDeadline) {
		time.Sleep(20 * time.Millisecond)
	}
	if stored.Status != StatusCancelled || stored.StepStatuses[0] != StepCancelled || stored.StepStatuses[1] != StepSkipped {
		t.Fatalf("stopped: %+v\n%s", stored, host.logText())
	}
	if !strings.Contains(host.logText(), "==> \U0001F6D1 STOPPED: Stopped by user.") || !strings.Contains(g.fakeFile("systemctl.calls"), "--user kill --kill-whom=all forkmesh-action-") {
		t.Fatalf("stop log:\n%s\ncalls: %s", host.logText(), g.fakeFile("systemctl.calls"))
	}
}

func TestRunnerTimeoutsAndVerification(t *testing.T) {
	slow := "name: Slow\non: push\nsteps:\n  - name: Hang\n    run: sleep 30\n"
	f := newFixture(t, slow, nil)
	limits := testLimits()
	limits.StepTimeoutMs = 300
	host, _ := f.start(f.run(8, slow), nil, limits)
	f.wait(host)
	if !strings.Contains(host.logText(), "==> \u274C FAILED: Hang exceeded the process time limit.") {
		t.Fatal(host.logText())
	}

	bad := f.run(9, slow)
	bad.ExecutionDigest = strings.Repeat("0", 64)
	host, _ = f.start(bad, nil, testLimits())
	f.wait(host)
	if !strings.Contains(host.logText(), "Repository approval verification failed: the checked-out repository state no longer matches the approved execution digest.") {
		t.Fatal(host.logText())
	}

	missing := f.run(10, slow)
	host = &fakeHost{st: f.store, done: make(chan struct{})}
	request := StartRequest{Run: missing, Workflow: Parse(missing.WorkflowPath, missing.WorkflowContent), Mirror: filepath.Join(f.fake, "gone"), Limits: testLimits()}
	r := newRunner(host, request)
	r.post(func() { r.start(request) })
	f.wait(host)
	if !strings.Contains(host.logText(), "The served mirror for owner/repo is no longer available at "+filepath.Join(f.fake, "gone")+".") {
		t.Fatal(host.logText())
	}
}

func TestRunnerNoCheckoutAndShutdownInterrupt(t *testing.T) {
	workflow := "name: Bare\non: push\ncheckout: false\nsteps:\n  - run: echo plain; sleep 30\n"
	f := newFixture(t, workflow, nil)
	host, r := f.start(f.run(11, workflow), nil, testLimits())
	deadline := time.Now().Add(30 * time.Second)
	for !strings.Contains(host.logText(), "plain") && time.Now().Before(deadline) {
		time.Sleep(20 * time.Millisecond)
	}
	if !strings.Contains(host.logText(), "No local checkout (checkout: false)") {
		t.Fatal(host.logText())
	}
	host.mu.Lock()
	host.stopping = true
	host.mu.Unlock()
	r.post(r.interruptNow)
	f.wait(host)
	stored := f.stored(11)
	if !host.gone || stored.Status != StatusRunning || stored.FinishedAtMs != 0 {
		t.Fatalf("interrupt: %+v", stored)
	}
	if !strings.Contains(host.logText(), "==> \u26A0 INTERRUPTED: ForkMesh received a stop signal and Step 1 ") {
		t.Fatal(host.logText())
	}
	if !stored.RequeueAfterDesktopInterruption() || stored.Status != StatusQueued {
		t.Fatal("an interrupted run requeues")
	}
}

func TestRunnerOutputCapAndCache(t *testing.T) {
	workflow := "name: Loud\non: push\nsteps:\n  - run: head -c 1300000 /dev/zero | tr '\\0' 'a'; echo; echo tail-marker\n"
	f := newFixture(t, workflow, nil)
	host, _ := f.start(f.run(12, workflow), nil, testLimits())
	f.wait(host)
	log := host.logText()
	if !host.ok {
		t.Fatal(log)
	}
	for _, want := range []string{"!! Action output exceeded 1024 KiB.", "!! Showing the final", "tail-marker", "!! End of suppressed process output.", "...[long line wrapped for display]..."} {
		if !strings.Contains(log, want) {
			t.Fatalf("missing %q", want)
		}
	}
	if !strings.Contains(log, "Build cache kept between runs at /cache") {
		t.Fatal("cache not mounted")
	}
	if !strings.Contains(f.fakeFile("bwrap.args"), "/cache\n") || !strings.Contains(f.fakeFile("bwrap.env"), "CCACHE_MAXSIZE=4G") {
		t.Fatal("cache env")
	}
	lock, ok := tryLock(filepath.Join(f.store.BuildCacheDir(f.run(13, workflow)), ".lock"))
	if !ok {
		t.Fatal("cache lock not released")
	}
	lock.release()
}

func TestRunnerReleaseLanding(t *testing.T) {
	workflow := "name: Release\non: [release]\nsteps:\n  - run: |\n      mkdir -p .forkmesh/releases\n      echo manifest > .forkmesh/releases/v1.json\n      echo blob > \"$FAKE_RELEASE/blob\"\n      sed -i 's/VERSION 0.1.0/VERSION 0.2.0/' desktop/CMakeLists.txt\n"
	f := newFixture(t, workflow, map[string]string{"desktop/CMakeLists.txt": "project(ForkMesh VERSION 0.1.0)\n"})
	run := f.run(14, workflow)
	run.Ref = "refs/tags/v0.2.0"
	host, _ := f.start(run, nil, testLimits())
	f.wait(host)
	log := host.logText()
	if !host.ok || !host.landed {
		t.Fatalf("release failed:\n%s", log)
	}
	if !strings.Contains(log, "==> \U0001F4E6 Attached release metadata for v0.2.0; publishing\u2026") {
		t.Fatal(log)
	}
	if data, _ := os.ReadFile(filepath.Join(f.mirror, "forkmesh-releases", "blob")); string(data) != "blob\n" {
		t.Fatal("release CAS not landed")
	}
	subject := runGit(t, f.work, "log", "-1", "--format=%s")
	if subject != "release: publish v0.2.0 artifacts, bump version header" {
		t.Fatalf("commit: %q", subject)
	}
	header, _ := os.ReadFile(filepath.Join(f.work, "desktop", "CMakeLists.txt"))
	if !bytes.Contains(header, []byte("VERSION 0.2.0")) {
		t.Fatal(string(header))
	}
	names := strings.Join(host.eventNames(), ",")
	if !strings.Contains(names, "actions.release,actions.log") || !strings.HasSuffix(names, "actions.status,actions.finished,actions.release") {
		t.Fatalf("release events: %s", names)
	}
	var phases []string
	host.mu.Lock()
	for index, name := range host.names {
		if name == "actions.release" {
			phases = append(phases, host.events[index]["phase"].(string))
		}
	}
	host.mu.Unlock()
	if strings.Join(phases, ",") != "prepared,landed" {
		t.Fatalf("phases: %v", phases)
	}
}

func TestSandboxClaimsAcrossProcesses(t *testing.T) {
	if runtime.GOOS != "linux" {
		t.Skip("flock claims are exercised on Linux")
	}
	base := t.TempDir()
	old := filepath.Join(base, runPrefix+"1-1000")
	held := filepath.Join(base, runPrefix+"2-1000")
	young := filepath.Join(base, runPrefix+"3-"+jsonNumber(nowMs()))
	for _, dir := range []string{old, held, young, filepath.Join(base, "unrelated")} {
		if err := os.MkdirAll(filepath.Join(dir, "x"), 0o700); err != nil {
			t.Fatal(err)
		}
	}
	other := exec.Command("flock", filepath.Join(held, claimFileName), "sleep", "30")
	if err := other.Start(); err != nil {
		t.Skip("flock(1) missing")
	}
	defer func() { _ = other.Process.Kill(); _ = other.Wait() }()
	deadline := time.Now().Add(5 * time.Second)
	for !lockHeldElsewhere(filepath.Join(held, claimFileName)) && time.Now().Before(deadline) {
		time.Sleep(20 * time.Millisecond)
	}
	sweep := ReclaimSandboxes(base, -1)
	if sweep.Removed != 1 || pathExists(old) || !pathExists(held) || !pathExists(young) || !pathExists(filepath.Join(base, "unrelated")) {
		t.Fatalf("sweep: %+v", sweep)
	}
	if reason := RemoveUnclaimedSandbox(base, held); !strings.Contains(reason, "a running Action still uses") {
		t.Fatalf("held: %q", reason)
	}
	if reason := RemoveUnclaimedSandbox(base, filepath.Join(base, "unrelated")); !strings.Contains(reason, "is not an Actions sandbox tree") {
		t.Fatalf("unrelated: %q", reason)
	}
}

func jsonNumber(value int64) string {
	raw, _ := json.Marshal(value)
	return string(raw)
}
