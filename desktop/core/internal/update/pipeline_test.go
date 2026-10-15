//go:build unix

package update

import (
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	"forkmesh.com/desktop-core/internal/coretest"
	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/rpc"
)

const fakeCmake = `#!/bin/sh
echo "cmake $*" >> "$FAKE_LOG"
if [ "$1" = "--build" ]; then
  dir="$2"
  if [ -n "$FAKE_BUILD_SLEEP" ]; then sleep "$FAKE_BUILD_SLEEP"; fi
  echo "[1/2] Building CXX object a.o"
  printf '[2/2] Linking CXX executable forkmesh\r\n'
  if [ -n "$FAKE_BUILD_FAIL" ]; then echo "src/x.cpp:1: error: boom" >&2; exit 1; fi
  if [ -n "$FAKE_TOUCH_ONCE" ] && [ ! -e "$FAKE_MARK" ]; then : > "$FAKE_MARK"; sleep 0.05; echo changed > "$FAKE_TOUCH_ONCE"; fi
  printf '#!/bin/sh\necho forkmesh-new\n' > "$dir/forkmesh"
  chmod +x "$dir/forkmesh"
  exit 0
fi
src=""; bld=""; type=""; gen="Unix Makefiles"
while [ $# -gt 0 ]; do
  case "$1" in
    -S) src="$2"; shift;;
    -B) bld="$2"; shift;;
    -G) gen="$2"; shift;;
    -DCMAKE_BUILD_TYPE=*) type="${1#-DCMAKE_BUILD_TYPE=}";;
  esac
  shift
done
mkdir -p "$bld"
printf 'CMAKE_GENERATOR:INTERNAL=%s\nCMAKE_BUILD_TYPE:STRING=%s\nCMAKE_HOME_DIRECTORY:INTERNAL=%s\n' "$gen" "$type" "$src" > "$bld/CMakeCache.txt"
`

const fakeGit = `#!/bin/sh
echo "git $*" >> "$FAKE_LOG"
case "$*" in
  *rev-parse*) echo abc1234;;
  *pull*) if [ -n "$FAKE_PULL_FAIL" ]; then echo "fatal: unable to access: The requested URL returned error: 503" >&2; exit 1; fi;;
  *clone*) exit 1;;
esac
exit 0
`

type sandbox struct {
	root      string
	clientDir string
	appPath   string
	log       string
}

func newSandbox(t *testing.T) *sandbox {
	t.Helper()
	root, err := filepath.EvalSymlinks(t.TempDir())
	if err != nil {
		t.Fatal(err)
	}
	bin := filepath.Join(root, "bin")
	s := &sandbox{root: root, clientDir: filepath.Join(root, "src", "desktop"),
		appPath: filepath.Join(root, "install", "forkmesh"), log: filepath.Join(root, "calls.log")}
	for _, dir := range []string{bin, s.clientDir, filepath.Dir(s.appPath), filepath.Join(root, "src", ".git")} {
		if err := os.MkdirAll(dir, 0o755); err != nil {
			t.Fatal(err)
		}
	}
	write := func(path, body string, mode os.FileMode) {
		if err := os.WriteFile(path, []byte(body), mode); err != nil {
			t.Fatal(err)
		}
	}
	write(filepath.Join(bin, "cmake"), fakeCmake, 0o755)
	write(filepath.Join(bin, "git"), fakeGit, 0o755)
	write(filepath.Join(bin, "ninja"), "#!/bin/sh\nexit 0\n", 0o755)
	write(filepath.Join(s.clientDir, "CMakeLists.txt"), "project(x)\n", 0o644)
	write(s.appPath, "#!/bin/sh\necho old\n", 0o755)
	old := time.Now().Add(-time.Hour)
	_ = os.Chtimes(filepath.Join(s.clientDir, "CMakeLists.txt"), old, old)
	_ = os.Chtimes(s.clientDir, old, old)
	t.Setenv("PATH", bin+string(os.PathListSeparator)+os.Getenv("PATH"))
	t.Setenv("FAKE_LOG", s.log)
	t.Setenv("HOME", filepath.Join(root, "home"))
	for _, name := range []string{"FORKMESH_HOME", "FORKMESH_PROJECTS_DIR", "FORKMESH_PROJECT_DIR", "FORKMESH_DATA_DIR",
		"FORKMESH_CONFIG_DIR", "FAKE_BUILD_SLEEP", "FAKE_BUILD_FAIL", "FAKE_TOUCH_ONCE", "FAKE_PULL_FAIL"} {
		t.Setenv(name, "")
	}
	return s
}

func (s *sandbox) calls(t *testing.T) string {
	data, _ := os.ReadFile(s.log)
	return string(data)
}

type launches struct {
	mu    sync.Mutex
	calls [][]string
	fail  bool
}

func (l *launches) launch(program string, args []string, dir string) error {
	l.mu.Lock()
	defer l.mu.Unlock()
	l.calls = append(l.calls, append([]string{program}, args...))
	if l.fail {
		return os.ErrPermission
	}
	return nil
}

func (l *launches) list() [][]string {
	l.mu.Lock()
	defer l.mu.Unlock()
	return append([][]string{}, l.calls...)
}

func startModule(t *testing.T) (*daemon.Core, *Module, *coretest.Peer, *launches) {
	t.Helper()
	core := coretest.New(t)
	core.Set("control.owners", []string{"alice"})
	module := New()
	env := defaultEnv()
	env.FreeBytes = func(string) int64 { return 100 << 30 }
	env.Memory = func() (int64, int64) { return 64 << 30, 60 << 30 }
	env.CPUs = func() int { return 4 }
	env.Darwin = false
	env.StartCheck = 5 * time.Second
	env.KillGrace = 200 * time.Millisecond
	module.Env = env
	l := &launches{}
	module.Launch = l.launch
	module.HasUI = func() bool { return false }
	coretest.Start(t, core, module)
	ui := coretest.NewPeer("ui", rpc.UI)
	core.Hub.Add(ui)
	return core, module, ui, l
}

type progress struct {
	events  []coretest.Event
	output  strings.Builder
	finish  map[string]any
	logs    []string
	started bool
}

func collect(t *testing.T, ui *coretest.Peer, timeout time.Duration) *progress {
	t.Helper()
	out := &progress{}
	deadline := time.After(timeout)
	for {
		select {
		case event := <-ui.Events:
			out.events = append(out.events, event)
			var data map[string]any
			_ = json.Unmarshal(event.Data, &data)
			switch event.Name {
			case "core.update.output":
				out.output.WriteString(data["text"].(string))
			case "core.update.log":
				out.logs = append(out.logs, data["text"].(string))
			case "core.update.started":
				out.started = true
			case "core.update.finished":
				out.finish = data
				return out
			}
		case <-deadline:
			t.Fatalf("no core.update.finished within %s; output so far:\n%s", timeout, out.output.String())
			return nil
		}
	}
}

func (s *sandbox) request() Request {
	return Request{Kind: "rebuild", ClientDir: s.clientDir, BuildType: "RelWithDebInfo", AppPath: s.appPath,
		Args: []string{"--flag"}, Cwd: s.root}
}

func TestRebuildInstallsAndRelaunchesFromTheCore(t *testing.T) {
	s := newSandbox(t)
	core, _, ui, l := startModule(t)
	var started map[string]any
	coretest.MustCall(t, core, ui, "core.update.start", s.request(), &started)
	if started["started"] != true {
		t.Fatalf("start %v", started)
	}
	result := collect(t, ui, 20*time.Second)
	if result.finish["ok"] != true {
		t.Fatalf("finished %v\n%s", result.finish, result.output.String())
	}
	text := result.output.String()
	for _, want := range []string{"==> Configuring...", "==> Rebuilding...", "[1/2] Building CXX object a.o",
		"$ TMPDIR=" + s.clientDir + "/build/.forkmesh-tmp cmake --build " + s.clientDir + "/build -j 4",
		"==> Relaunching...", "Skipping the Go mirror-node companion"} {
		if !strings.Contains(text, want) {
			t.Errorf("output lacks %q:\n%s", want, text)
		}
	}
	if !strings.Contains(s.calls(t), "-G Ninja") || !strings.Contains(s.calls(t), "-DCMAKE_BUILD_TYPE=RelWithDebInfo") {
		t.Fatalf("configure args: %s", s.calls(t))
	}
	installed, _ := os.ReadFile(s.appPath)
	if !strings.Contains(string(installed), "forkmesh-new") {
		t.Fatalf("installed binary %q", installed)
	}
	if backup, _ := os.ReadFile(s.appPath + ".bak-update"); !strings.Contains(string(backup), "old") {
		t.Fatalf("no backup of the old binary: %q", backup)
	}
	calls := l.list()
	if len(calls) != 1 || calls[0][0] != s.appPath || calls[0][1] != "--flag" {
		t.Fatalf("launches %v", calls)
	}
	var state map[string]any
	coretest.MustCall(t, core, ui, "core.update.state", nil, &state)
	if state["running"] != true || state["relaunchPending"] != true || state["relaunching"] != true {
		t.Fatalf("state after relaunch %v", state)
	}
	coretest.MustCall(t, core, ui, "core.update.config", Config{ClientDir: s.clientDir, StartedAtMs: 1, PID: 1}, nil)
	coretest.MustCall(t, core, ui, "core.update.config", Config{ClientDir: s.clientDir, StartedAtMs: 2, PID: 2}, nil)
	coretest.MustCall(t, core, ui, "core.update.state", nil, &state)
	if state["running"] != false {
		t.Fatalf("a new desktop did not end the relaunch: %v", state)
	}
	coretest.MustCall(t, core, ui, "core.update.start", s.request(), nil)
	second := collect(t, ui, 20*time.Second)
	if !strings.Contains(strings.Join(second.logs, "\n"), "build tree already configured; skipping the configure step") {
		t.Fatalf("second run reconfigured: %v", second.logs)
	}
}

func TestBuildFailureKeepsTheInstalledBinary(t *testing.T) {
	s := newSandbox(t)
	t.Setenv("FAKE_BUILD_FAIL", "1")
	core, module, ui, l := startModule(t)
	coretest.MustCall(t, core, ui, "core.update.start", s.request(), nil)
	result := collect(t, ui, 20*time.Second)
	if result.finish["ok"] != false || result.finish["error"] != "Update failed: src/x.cpp:1: error: boom" {
		t.Fatalf("finished %v", result.finish)
	}
	if installed, _ := os.ReadFile(s.appPath); !strings.Contains(string(installed), "old") {
		t.Fatal("a failed build replaced the binary")
	}
	if len(l.list()) != 0 {
		t.Fatal("a failed build relaunched")
	}
	if module.Busy() {
		t.Fatal("still busy after a failure")
	}
}

func TestFailedPullStopsWithoutTouchingTheCheckout(t *testing.T) {
	s := newSandbox(t)
	t.Setenv("FAKE_PULL_FAIL", "1")
	sentinel := filepath.Join(s.root, "src", "keep.txt")
	if err := os.WriteFile(sentinel, []byte("mine"), 0o644); err != nil {
		t.Fatal(err)
	}
	core, _, ui, _ := startModule(t)
	req := s.request()
	req.Kind = "update"
	req.InstallURL = "https://mirror.invalid/forkmesh/forkmesh"
	req.RepoDir = filepath.Join(s.root, "src")
	coretest.MustCall(t, core, ui, "core.update.start", req, nil)
	result := collect(t, ui, 20*time.Second)
	want := "Could not fast-forward from https://mirror.invalid/forkmesh/forkmesh (the mirror may be busy, or this checkout has its own commits). Nothing in " + req.RepoDir + " was changed or removed."
	if result.finish["error"] != want {
		t.Fatalf("error %q", result.finish["error"])
	}
	calls := s.calls(t)
	if strings.Contains(calls, "clone") || strings.Contains(calls, "cmake") {
		t.Fatalf("a failed pull went on: %s", calls)
	}
	if data, err := os.ReadFile(sentinel); err != nil || string(data) != "mine" {
		t.Fatal("the checkout was touched")
	}
	if _, err := os.Stat(filepath.Join(s.root, "src", ".git")); err != nil {
		t.Fatal("the checkout's .git is gone")
	}
}

func TestSourcesChangingMidBuildRunAnotherPass(t *testing.T) {
	s := newSandbox(t)
	t.Setenv("FAKE_TOUCH_ONCE", filepath.Join(s.clientDir, "MainWindow.h"))
	t.Setenv("FAKE_MARK", filepath.Join(s.root, "touched"))
	core, _, ui, _ := startModule(t)
	coretest.MustCall(t, core, ui, "core.update.start", s.request(), nil)
	result := collect(t, ui, 20*time.Second)
	if result.finish["ok"] != true {
		t.Fatalf("finished %v", result.finish)
	}
	text := result.output.String()
	if !strings.Contains(text, "==> Rebuilding again (pass 2 of 3): the sources changed during the build...") ||
		!strings.Contains(text, "!! the sources changed while build pass 1 ran (MainWindow.h; HEAD abc1234 -> abc1234); building again") {
		t.Fatalf("no second pass:\n%s", text)
	}
}

func TestCancelStopsTheBuild(t *testing.T) {
	s := newSandbox(t)
	t.Setenv("FAKE_BUILD_SLEEP", "30")
	core, module, ui, l := startModule(t)
	coretest.MustCall(t, core, ui, "core.update.start", s.request(), nil)
	deadline := time.Now().Add(10 * time.Second)
	for !strings.Contains(s.calls(t), "--build") && time.Now().Before(deadline) {
		time.Sleep(20 * time.Millisecond)
	}
	began := time.Now()
	coretest.MustCall(t, core, ui, "core.update.cancel", nil, nil)
	result := collect(t, ui, 10*time.Second)
	if result.finish["cancelled"] != true || time.Since(began) > 5*time.Second {
		t.Fatalf("finished %v after %s", result.finish, time.Since(began))
	}
	if len(l.list()) != 0 || module.Busy() {
		t.Fatal("a cancelled build went on")
	}
}

func TestLowDiskRefusesBeforeTouchingAnything(t *testing.T) {
	s := newSandbox(t)
	core, module, ui, _ := startModule(t)
	module.Env.FreeBytes = func(string) int64 { return 10 }
	coretest.MustCall(t, core, ui, "core.update.start", s.request(), nil)
	result := collect(t, ui, 10*time.Second)
	want := "Update cancelled: only 10 B free on the volume holding " + s.clientDir + "/build, and the rebuild needs at least 3.0 GB. Free some space and try again \u2014 nothing was changed."
	if result.finish["error"] != want {
		t.Fatalf("error %q", result.finish["error"])
	}
	if s.calls(t) != "" {
		t.Fatalf("ran %s", s.calls(t))
	}
}

func TestSupervisedDesktopRestartsItsUnit(t *testing.T) {
	s := newSandbox(t)
	core, _, ui, l := startModule(t)
	req := s.request()
	req.Unit = "forkmesh-source.service"
	req.SelfPath = s.appPath
	coretest.MustCall(t, core, ui, "core.update.start", req, nil)
	if result := collect(t, ui, 20*time.Second); result.finish["ok"] != true {
		t.Fatalf("finished %v", result.finish)
	}
	calls := l.list()
	if len(calls) != 1 || strings.Join(calls[0], " ") != "systemctl --user restart --no-block forkmesh-source.service" {
		t.Fatalf("launches %v", calls)
	}
}

func TestRelaunchAsksTheDesktopToReleaseThenQuit(t *testing.T) {
	s := newSandbox(t)
	core, module, ui, l := startModule(t)
	var mu sync.Mutex
	var order []string
	module.HasUI = func() bool { return true }
	module.CallUI = func(ctx context.Context, method string, params any) ([]byte, error) {
		mu.Lock()
		order = append(order, method)
		mu.Unlock()
		return []byte(`{}`), nil
	}
	module.NotifyUI = func(method string, params any) bool {
		mu.Lock()
		order = append(order, method)
		mu.Unlock()
		return true
	}
	module.Launch = func(program string, args []string, dir string) error {
		mu.Lock()
		order = append(order, "launch "+program)
		mu.Unlock()
		return l.launch(program, args, dir)
	}
	coretest.MustCall(t, core, ui, "core.update.start", s.request(), nil)
	if result := collect(t, ui, 20*time.Second); result.finish["ok"] != true {
		t.Fatalf("finished %v", result.finish)
	}
	mu.Lock()
	defer mu.Unlock()
	if strings.Join(order, ",") != "ui.update.relaunch,launch "+s.appPath+",ui.update.quit" {
		t.Fatalf("order %v", order)
	}
}

func TestPhoneFamilyIsNativeOnceConfigured(t *testing.T) {
	s := newSandbox(t)
	core, _, ui, _ := startModule(t)
	phone := coretest.NewPeer("phone", rpc.LAN)
	phone.Dev.User = "Alice"
	stranger := coretest.NewPeer("stranger", rpc.Relay)
	stranger.Dev.User = "mallory"
	if _, err := coretest.Call(t, core, phone, "update.status", nil); rpc.Code(err) != "unsupported" {
		t.Fatalf("unconfigured core answered update.status: %v", err)
	}
	coretest.MustCall(t, core, ui, "core.update.config", Config{ClientDir: s.clientDir, AppPath: s.appPath,
		BuildType: "RelWithDebInfo", Version: "9.9.9", StartedAtMs: 1234, PID: 1}, nil)
	if _, err := coretest.Call(t, core, stranger, "update.status", nil); rpc.Code(err) != "not_owner" {
		t.Fatalf("stranger: %v", err)
	}
	if _, err := coretest.Call(t, core, phone, "core.update.start", s.request()); rpc.Code(err) != "forbidden" {
		t.Fatalf("phone reached core.update.start: %v", err)
	}
	coretest.MustCall(t, core, ui, "core.update.reset", nil, nil)
	coretest.MustCall(t, core, ui, "core.update.append", map[string]any{"text": "==> ForkMesh update started\n"}, nil)
	var status map[string]any
	coretest.MustCall(t, core, phone, "update.status", nil, &status)
	if status["version"] != "9.9.9" || status["startedAtMs"] != float64(1234) || status["running"] != false ||
		len(status["lines"].([]any)) != 1 || status["seq"] != float64(1) || status["first"] != float64(1) {
		t.Fatalf("status %v", status)
	}
	if _, err := coretest.Call(t, core, phone, "update.prepare", map[string]any{"action": "other"}); rpc.Code(err) != "unknown_action" {
		t.Fatalf("prepare other: %v", err)
	}
	var prepared map[string]any
	coretest.MustCall(t, core, phone, "update.prepare", map[string]any{"action": actionName}, &prepared)
	if prepared["subject"] != "desktop" || prepared["expiresInMs"] != float64(60000) ||
		!strings.Contains(prepared["summary"].(string), "(now on 9.9.9)") {
		t.Fatalf("prepared %v", prepared)
	}
	if _, err := coretest.Call(t, core, stranger, "update.run", map[string]any{"action": actionName, "token": prepared["token"]}); rpc.Code(err) != "not_owner" {
		t.Fatalf("stranger run: %v", err)
	}
	other := coretest.NewPeer("other", rpc.LAN)
	other.Dev.User = "alice"
	if _, err := coretest.Call(t, core, other, "update.run", map[string]any{"action": actionName, "token": prepared["token"]}); rpc.Code(err) != "confirmation_mismatch" {
		t.Fatalf("other device: %v", err)
	}
	coretest.MustCall(t, core, phone, "update.prepare", map[string]any{"action": actionName}, &prepared)
	var ran map[string]any
	coretest.MustCall(t, core, phone, "update.run", map[string]any{"action": actionName, "token": prepared["token"]}, &ran)
	if ran["started"] != true || ran["message"] != "Rebuilding. The desktop restarts when the build is done." {
		t.Fatalf("run %v", ran)
	}
	if _, err := coretest.Call(t, core, phone, "update.run", map[string]any{"action": actionName, "token": prepared["token"]}); rpc.Code(err) != "confirmation_required" {
		t.Fatalf("token reuse: %v", err)
	}
	result := collect(t, ui, 20*time.Second)
	if result.finish["ok"] != true {
		t.Fatalf("phone run %v\n%s", result.finish, result.output.String())
	}
	sawLines := false
	deadline := time.After(5 * time.Second)
	for !sawLines {
		select {
		case event := <-phone.Events:
			if event.Name != "update.progress" {
				continue
			}
			var data map[string]any
			_ = json.Unmarshal(event.Data, &data)
			if lines, ok := data["lines"].([]any); ok && len(lines) > 0 {
				for _, key := range []string{"run", "running", "kind", "percent", "phase", "activity", "status", "error", "relaunching", "first", "seq", "skipped"} {
					if _, ok := data[key]; !ok {
						t.Fatalf("progress lacks %s: %v", key, data)
					}
				}
				sawLines = true
			}
		case <-deadline:
			t.Fatal("the phone got no update.progress lines")
		}
	}
	if _, err := coretest.Call(t, core, phone, "update.prepare", map[string]any{"action": actionName}); rpc.Code(err) != "already_running" {
		t.Fatalf("prepare while relaunching: %v", err)
	}
}

func TestPhoneCanRestartComputer(t *testing.T) {
	s := newSandbox(t)
	core, module, ui, _ := startModule(t)
	module.HasUI = func() bool { return true }
	called := ""
	module.CallUI = func(ctx context.Context, method string, params any) ([]byte, error) {
		called = method
		return []byte(`{"started":true}`), nil
	}
	coretest.MustCall(t, core, ui, "core.update.config", Config{ClientDir: s.clientDir}, nil)
	phone := coretest.NewPeer("phone", rpc.LAN)
	phone.Dev.User = "alice"
	var prepared map[string]any
	coretest.MustCall(t, core, phone, "update.prepare", map[string]any{"action": computerRestartActionName}, &prepared)
	if prepared["subject"] != computerRestartSubjectName || !strings.Contains(prepared["summary"].(string), "entire computer") {
		t.Fatalf("prepared %v", prepared)
	}
	var ran map[string]any
	coretest.MustCall(t, core, phone, "update.run", map[string]any{"action": computerRestartActionName, "token": prepared["token"]}, &ran)
	if called != "ui.update.reboot" || ran["message"] != "The computer is restarting." {
		t.Fatalf("called %q, ran %v", called, ran)
	}
}

func TestDesktopRunStateReachesPhones(t *testing.T) {
	s := newSandbox(t)
	core, _, ui, _ := startModule(t)
	coretest.MustCall(t, core, ui, "core.update.config", Config{ClientDir: s.clientDir}, nil)
	coretest.MustCall(t, core, ui, "core.update.desktop", desktopState{Running: true, Kind: "update", Percent: 20, Phase: "pulling the source", Activity: "20% pulling"}, nil)
	coretest.MustCall(t, core, ui, "core.update.note", map[string]any{"status": "Pulling...", "error": false}, nil)
	phone := coretest.NewPeer("phone", rpc.LAN)
	phone.Dev.User = "alice"
	var status map[string]any
	coretest.MustCall(t, core, phone, "update.status", nil, &status)
	if status["running"] != true || status["kind"] != "update" || status["activity"] != "20% pulling" || status["status"] != "Pulling..." {
		t.Fatalf("status %v", status)
	}
	if _, err := coretest.Call(t, core, phone, "update.prepare", map[string]any{"action": actionName}); rpc.Code(err) != "already_running" {
		t.Fatalf("prepare during a desktop run: %v", err)
	}
	if _, err := coretest.Call(t, core, ui, "core.update.start", s.request()); err != nil {
		t.Fatalf("the desktop's own handoff was refused: %v", err)
	}
	collect(t, ui, 20*time.Second)
}
