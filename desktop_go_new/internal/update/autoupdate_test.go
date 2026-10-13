//go:build unix

package update

import (
	"context"
	"encoding/json"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	"forkmesh.com/desktop-core/internal/coretest"
	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/rpc"
)

type autoDesktop struct {
	mu          sync.Mutex
	busy        bool
	rebuildBusy bool
	refuse      bool
	calls       []string
	auto        map[string]any
}

func (d *autoDesktop) call(ctx context.Context, method string, params any) ([]byte, error) {
	d.mu.Lock()
	defer d.mu.Unlock()
	d.calls = append(d.calls, method)
	switch method {
	case "ui.update.agents":
		return json.Marshal(map[string]any{"running": 0, "anyRunning": d.busy, "rebuildBusy": d.rebuildBusy})
	case "ui.update.auto":
		raw, _ := json.Marshal(params)
		_ = json.Unmarshal(raw, &d.auto)
		return json.Marshal(map[string]any{"started": !d.refuse})
	}
	return []byte("{}"), nil
}

func gitIn(t *testing.T, dir string, args ...string) string {
	t.Helper()
	cmd := exec.Command("git", append([]string{"-c", "user.name=t", "-c", "user.email=t@example.invalid", "-c", "init.defaultBranch=main"}, args...)...)
	cmd.Dir = dir
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("git %v: %v\n%s", args, err, out)
	}
	return strings.TrimSpace(string(out))
}

type autoFixture struct {
	core      *daemon.Core
	module    *Module
	desktop   *autoDesktop
	upstream  string
	clientDir string
}

func newAutoFixture(t *testing.T, enabled bool) *autoFixture {
	t.Helper()
	if _, err := exec.LookPath("git"); err != nil {
		t.Skip("git is not installed")
	}
	root, err := filepath.EvalSymlinks(t.TempDir())
	if err != nil {
		t.Fatal(err)
	}
	t.Setenv("HOME", filepath.Join(root, "home"))
	t.Setenv("GIT_CONFIG_NOSYSTEM", "1")
	for _, name := range []string{"FORKMESH_HOME", "FORKMESH_PROJECTS_DIR", "FORKMESH_PROJECT_DIR", "FORKMESH_DATA_DIR", "FORKMESH_CONFIG_DIR"} {
		t.Setenv(name, "")
	}
	upstream := filepath.Join(root, "upstream")
	if err := os.MkdirAll(filepath.Join(upstream, "desktop"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(upstream, "desktop", "CMakeLists.txt"), []byte("project(x)\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	gitIn(t, upstream, "init", "-q")
	gitIn(t, upstream, "add", ".")
	gitIn(t, upstream, "commit", "-q", "-m", "one")
	gitIn(t, upstream, "tag", "v1.0.0")
	gitIn(t, root, "clone", "-q", upstream, filepath.Join(root, "src"))

	core := coretest.New(t)
	if err := os.MkdirAll(filepath.Dir(core.Layout.SettingsFile()), 0o700); err != nil {
		t.Fatal(err)
	}
	conf := "[update]\nautoUpdate=false\n"
	if enabled {
		conf = "[update]\nautoUpdate=true\n"
	}
	if err := os.WriteFile(core.Layout.SettingsFile(), []byte(conf), 0o600); err != nil {
		t.Fatal(err)
	}
	desktop := &autoDesktop{}
	module := New()
	env := defaultEnv()
	env.AutoFirst = nil
	env.AutoFetch = 20 * time.Second
	module.Env = env
	module.HasUI = func() bool { return true }
	module.CallUI = desktop.call
	module.Launch = func(string, []string, string) error { return nil }
	coretest.Start(t, core, module)
	return &autoFixture{core: core, module: module, desktop: desktop, upstream: upstream,
		clientDir: filepath.Join(root, "src", "desktop")}
}

func (f *autoFixture) configure(t *testing.T, dir string) {
	t.Helper()
	ui := coretest.NewPeer("ui", rpc.UI)
	var reply struct {
		Capabilities []string `json:"capabilities"`
	}
	coretest.MustCall(t, f.core, ui, "core.update.config", Config{ClientDir: f.clientDir, AppPath: "/x/forkmesh",
		AutoUpdateDir: dir, PID: 1, StartedAtMs: 1}, &reply)
	have := strings.Join(reply.Capabilities, ",")
	if !strings.Contains(have, "autoUpdate") || !strings.Contains(have, "quick") {
		t.Fatalf("capabilities %v", reply.Capabilities)
	}
}

func TestAutoUpdateFindsANewReleaseAndAsksTheDesktop(t *testing.T) {
	f := newAutoFixture(t, true)
	if got := f.module.autoUpdateCheck(context.Background()); got != "not_delegated" {
		t.Fatalf("before the desktop delegates: %s", got)
	}
	f.configure(t, f.clientDir)
	if got := f.module.autoUpdateCheck(context.Background()); got != "current" {
		t.Fatalf("no new tag: %s", got)
	}
	if f.desktop.auto != nil {
		t.Fatalf("asked the desktop without a new release")
	}
	if err := os.WriteFile(filepath.Join(f.upstream, "NEWS"), []byte("two\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	gitIn(t, f.upstream, "add", ".")
	t.Setenv("GIT_COMMITTER_DATE", "2030-01-01T00:00:00Z")
	gitIn(t, f.upstream, "commit", "-q", "-m", "two")
	gitIn(t, f.upstream, "tag", "v1.1.0")
	want := gitIn(t, f.upstream, "rev-parse", "HEAD")

	f.desktop.busy = true
	if got := f.module.autoUpdateCheck(context.Background()); got != "busy" {
		t.Fatalf("an agent is running: %s", got)
	}
	f.desktop.busy = false
	f.desktop.rebuildBusy = true
	if got := f.module.autoUpdateCheck(context.Background()); got != "busy" {
		t.Fatalf("a rebuild runs in the desktop: %s", got)
	}
	f.desktop.rebuildBusy = false
	if got := f.module.autoUpdateCheck(context.Background()); got != "started" {
		t.Fatalf("new release: %s", got)
	}
	if f.desktop.auto["tag"] != "v1.1.0" || f.desktop.auto["commit"] != want || f.desktop.auto["clientDir"] != f.clientDir {
		t.Fatalf("ui.update.auto %v", f.desktop.auto)
	}
	if head := gitIn(t, filepath.Dir(f.clientDir), "rev-parse", "HEAD"); head == want {
		t.Fatalf("the check moved the checkout; it only fetches")
	}
	f.desktop.refuse = true
	if got := f.module.autoUpdateCheck(context.Background()); got != "busy" {
		t.Fatalf("the desktop refused at the last moment: %s", got)
	}
}

func TestAutoUpdateStaysOffAndSkipsWithoutUpstream(t *testing.T) {
	f := newAutoFixture(t, false)
	f.configure(t, f.clientDir)
	if got := f.module.autoUpdateCheck(context.Background()); got != "off" {
		t.Fatalf("setting off: %s", got)
	}
	g := newAutoFixture(t, true)
	g.configure(t, g.clientDir)
	gitIn(t, filepath.Dir(g.clientDir), "branch", "--unset-upstream")
	if got := g.module.autoUpdateCheck(context.Background()); got != "no_upstream" {
		t.Fatalf("no upstream: %s", got)
	}
	h := newAutoFixture(t, true)
	h.configure(t, filepath.Join(filepath.Dir(h.clientDir), "missing"))
	if got := h.module.autoUpdateCheck(context.Background()); got != "no_checkout" {
		t.Fatalf("no checkout: %s", got)
	}
	h.module.HasUI = func() bool { return false }
	if got := h.module.autoUpdateCheck(context.Background()); got != "no_desktop" {
		t.Fatalf("no desktop: %s", got)
	}
}

func TestQuickUpdatePullsTheUpstreamThenRebuilds(t *testing.T) {
	s := newSandbox(t)
	core, _, ui, l := startModule(t)
	req := s.request()
	req.Kind = "quick"
	req.Label = "update"
	req.BuildType = "Release"
	coretest.MustCall(t, core, ui, "core.update.start", req, nil)
	result := collect(t, ui, 20*time.Second)
	if result.finish["ok"] != true {
		t.Fatalf("finished %v\n%s", result.finish, result.output.String())
	}
	calls := s.calls(t)
	pull := strings.Index(calls, "git pull --ff-only\n")
	build := strings.Index(calls, "cmake --build")
	if pull < 0 || build < pull || !strings.Contains(calls, "git rev-parse --abbrev-ref --symbolic-full-name @{u}") {
		t.Fatalf("calls:\n%s", calls)
	}
	if !strings.Contains(result.output.String(), "==> Pulling the latest version...") ||
		!strings.Contains(calls, "-DCMAKE_BUILD_TYPE=Release") {
		t.Fatalf("output:\n%s\ncalls:\n%s", result.output.String(), calls)
	}
	if len(l.list()) == 0 {
		t.Fatalf("no relaunch")
	}
}

func TestQuickUpdateFailedPullStopsBeforeBuilding(t *testing.T) {
	s := newSandbox(t)
	t.Setenv("FAKE_PULL_FAIL", "1")
	core, _, ui, _ := startModule(t)
	req := s.request()
	req.Kind = "quick"
	coretest.MustCall(t, core, ui, "core.update.start", req, nil)
	result := collect(t, ui, 20*time.Second)
	if result.finish["ok"] == true || !strings.HasPrefix(result.finish["error"].(string), "Update failed: ") {
		t.Fatalf("finished %v", result.finish)
	}
	if calls := s.calls(t); strings.Contains(calls, "cmake") || strings.Contains(calls, "clone") {
		t.Fatalf("went on after a failed pull:\n%s", calls)
	}
}

func TestQuickUpdateClonesWhenThereIsNoCheckout(t *testing.T) {
	s := newSandbox(t)
	if err := os.Remove(filepath.Join(s.clientDir, "CMakeLists.txt")); err != nil {
		t.Fatal(err)
	}
	core, _, ui, _ := startModule(t)
	req := s.request()
	req.Kind = "quick"
	req.RepoURL = "https://example.invalid/forkmesh.git"
	coretest.MustCall(t, core, ui, "core.update.start", req, nil)
	result := collect(t, ui, 20*time.Second)
	calls := s.calls(t)
	if !strings.Contains(calls, "git clone --depth 1 https://example.invalid/forkmesh.git "+filepath.Join(s.root, "src")) {
		t.Fatalf("calls:\n%s", calls)
	}
	if !strings.Contains(result.output.String(), "==> Downloading the latest version...") || result.finish["ok"] == true {
		t.Fatalf("finished %v\n%s", result.finish, result.output.String())
	}
}
