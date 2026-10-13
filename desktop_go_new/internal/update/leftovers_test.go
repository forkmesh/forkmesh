//go:build unix

package update

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"forkmesh.com/desktop-core/internal/coretest"
)

const fakeSudo = `#!/bin/sh
printf '%s\n' "$*" >> "$SUDO_LOG"
case "$*" in
  *--version*) if [ -n "$FAKE_SUDO_VERSION_FAIL" ]; then exit 1; fi;;
esac
[ "$1" = "-u" ] && shift 2
[ "$1" = "-H" ] && shift
exec "$@"
`

func installSourceServer(t *testing.T, body string, hits *int32) *httptest.Server {
	t.Helper()
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if hits != nil {
			atomic.AddInt32(hits, 1)
		}
		if r.URL.Path != "/api/install-source" || r.URL.RawQuery != "" {
			http.NotFound(w, r)
			return
		}
		_, _ = w.Write([]byte(body))
	}))
	t.Cleanup(server.Close)
	return server
}

func TestInstallSourceLookupMatchesTheDesktop(t *testing.T) {
	ctx := context.Background()
	cases := []struct {
		body string
		want string
	}{
		{`{"node":"n3","repo":"forkmesh"}`, "/forkmesh/forkmesh"},
		{`{"node":" n1 "}`, "/forkmesh/forkmesh"},
		{`{"node":"n1","repo":7}`, "/forkmesh/forkmesh"},
		{`{"node":"n1","repo":" other "}`, "/forkmesh/other"},
		{`{"node":"n1","repo":""}`, ""},
		{`{"node":"","repo":"forkmesh"}`, ""},
		{`{"repo":"forkmesh"}`, ""},
		{`not json`, ""},
		{`[1,2]`, ""},
	}
	for _, c := range cases {
		server := installSourceServer(t, c.body, nil)
		got := resolveInstallCloneURL(ctx, server.Client(), server.URL+"/ignored?x=1#frag", time.Second)
		want := ""
		if c.want != "" {
			want = server.URL + c.want
		}
		if got != want {
			t.Errorf("%s: got %q, want %q", c.body, got, want)
		}
	}
	slow := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		select {
		case <-r.Context().Done():
		case <-time.After(5 * time.Second):
		}
	}))
	defer slow.Close()
	began := time.Now()
	if got := resolveInstallCloneURL(ctx, slow.Client(), slow.URL, 150*time.Millisecond); got != "" || time.Since(began) > 3*time.Second {
		t.Fatalf("slow lookup %q after %s", got, time.Since(began))
	}
	closed := httptest.NewServer(http.NotFoundHandler())
	address := closed.URL
	closed.Close()
	if got := resolveInstallCloneURL(ctx, nil, address, time.Second); got != "" {
		t.Fatalf("closed server %q", got)
	}
	if got := resolveInstallCloneURL(ctx, nil, "not a url", time.Second); got != "" {
		t.Fatalf("bad base %q", got)
	}
}

func TestUpdateFindsTheMirrorInTheCore(t *testing.T) {
	s := newSandbox(t)
	server := installSourceServer(t, `{"node":"n3","repo":"forkmesh"}`, nil)
	core, _, ui, l := startModule(t)
	var config map[string]any
	coretest.MustCall(t, core, ui, "core.update.config", Config{ClientDir: s.clientDir}, &config)
	if caps, _ := json.Marshal(config["capabilities"]); string(caps) != `["installSource","asUser","prebuilt","clean","quick","autoUpdate"]` {
		t.Fatalf("capabilities %s", caps)
	}
	req := s.request()
	req.Kind = "update"
	req.CatalogURL = server.URL
	req.RepoDir = filepath.Join(s.root, "src")
	coretest.MustCall(t, core, ui, "core.update.start", req, nil)
	result := collect(t, ui, 20*time.Second)
	if result.finish["ok"] != true {
		t.Fatalf("finished %v\n%s", result.finish, result.output.String())
	}
	mirror := server.URL + "/forkmesh/forkmesh"
	text := result.output.String()
	for _, want := range []string{"==> Finding an online ForkMesh mirror...\n", "==> Pulling a fresh copy from " + mirror + "...\n"} {
		if !strings.Contains(text, want) {
			t.Errorf("output lacks %q:\n%s", want, text)
		}
	}
	if !strings.Contains(s.calls(t), "git -C "+req.RepoDir+" remote set-url origin "+mirror) {
		t.Fatalf("calls %s", s.calls(t))
	}
	if len(l.list()) != 1 {
		t.Fatalf("launches %v", l.list())
	}
}

func TestNoMirrorStopsBeforeTouchingTheCheckout(t *testing.T) {
	s := newSandbox(t)
	server := installSourceServer(t, `{"node":""}`, nil)
	core, _, ui, l := startModule(t)
	req := s.request()
	req.Kind = "update"
	req.CatalogURL = server.URL
	req.AsUser = "bob"
	coretest.MustCall(t, core, ui, "core.update.start", req, nil)
	result := collect(t, ui, 20*time.Second)
	if result.finish["error"] != "No online ForkMesh mirror is available right now. Try again shortly." ||
		result.finish["flash"] != "No online ForkMesh mirror is available right now." {
		t.Fatalf("finished %v", result.finish)
	}
	if !strings.Contains(result.output.String(), "==> Finding an online ForkMesh mirror (installing for bob)...") {
		t.Fatalf("output %s", result.output.String())
	}
	if s.calls(t) != "" || len(l.list()) != 0 {
		t.Fatalf("ran %s %v", s.calls(t), l.list())
	}
}

func sudoSandbox(t *testing.T) (*sandbox, string) {
	s := newSandbox(t)
	sudoLog := filepath.Join(s.root, "sudo.log")
	if err := os.WriteFile(filepath.Join(s.root, "bin", "sudo"), []byte(fakeSudo), 0o755); err != nil {
		t.Fatal(err)
	}
	t.Setenv("SUDO_LOG", sudoLog)
	t.Setenv("FAKE_SUDO_VERSION_FAIL", "")
	return s, sudoLog
}

func readLines(path string) []string {
	data, _ := os.ReadFile(path)
	return strings.Split(strings.TrimSpace(string(data)), "\n")
}

func TestRunAsAnotherUserGoesThroughSudo(t *testing.T) {
	s, sudoLog := sudoSandbox(t)
	core, _, ui, l := startModule(t)
	req := s.request()
	req.Kind = "update"
	req.AsUser = "bob"
	req.InstallURL = "https://mirror.invalid/forkmesh/forkmesh"
	req.RepoDir = filepath.Join(s.root, "src")
	coretest.MustCall(t, core, ui, "core.update.start", req, nil)
	result := collect(t, ui, 20*time.Second)
	if result.finish["ok"] != true {
		t.Fatalf("finished %v\n%s", result.finish, result.output.String())
	}
	build := s.clientDir + "/build"
	scratch := build + "/.forkmesh-tmp"
	built := build + "/forkmesh"
	script := asUserInstallScript(built, s.appPath, false)
	want := []string{
		"-u bob -H git -C " + req.RepoDir + " remote set-url origin " + req.InstallURL,
		"-u bob -H git -C " + req.RepoDir + " pull --ff-only",
		"-u bob -H mkdir -p " + scratch,
		"-u bob -H cmake -S " + s.clientDir + " -B " + build + " -DCMAKE_BUILD_TYPE=RelWithDebInfo -DFORKMESH_BUILD_TESTS=OFF -G Ninja",
		"-u bob -H env TMPDIR=" + scratch + " cmake --build " + build + " -j 4",
		"-u bob -H " + built + " --version",
		"-u bob -H sh -c " + script,
	}
	got := readLines(sudoLog)
	if strings.Join(got, "\n") != strings.Join(want, "\n") {
		t.Fatalf("sudo calls\n%s\nwant\n%s", strings.Join(got, "\n"), strings.Join(want, "\n"))
	}
	wantScript := "mkdir -p '" + filepath.Dir(s.appPath) + "' && { [ ! -e '" + s.appPath + "' ] || cp -f '" + s.appPath + "' '" +
		s.appPath + "'.bak-update; } && cp -f '" + built + "' '" + s.appPath + "' && chmod 0755 '" + s.appPath + "'"
	if script != wantScript {
		t.Fatalf("script %q", script)
	}
	text := result.output.String()
	for _, line := range []string{"==> Installing for bob...\n", "==> Relaunching...\n",
		"$ sudo -u bob -H env TMPDIR=" + scratch + " cmake --build " + build + " -j 4\n"} {
		if !strings.Contains(text, line) {
			t.Errorf("output lacks %q", line)
		}
	}
	if installed, _ := os.ReadFile(s.appPath); !strings.Contains(string(installed), "forkmesh-new") {
		t.Fatalf("installed %q", installed)
	}
	if backup, _ := os.ReadFile(s.appPath + ".bak-update"); !strings.Contains(string(backup), "old") {
		t.Fatalf("backup %q", backup)
	}
	calls := l.list()
	if len(calls) != 1 || strings.Join(calls[0], " ") != "sudo -u bob -H "+s.appPath+" --flag" {
		t.Fatalf("launches %v", calls)
	}
}

func TestSudoScriptCarriesTheCompanion(t *testing.T) {
	dir := t.TempDir()
	built := filepath.Join(dir, "build", "forkmesh")
	if err := os.MkdirAll(filepath.Dir(built), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "build", "forkmesh-mirror-node"), []byte("x"), 0o755); err != nil {
		t.Fatal(err)
	}
	script := asUserInstallScript(built, "/opt/it's/forkmesh", false)
	want := "mkdir -p '/opt/it'\\''s' && { [ ! -e '/opt/it'\\''s/forkmesh' ] || cp -f '/opt/it'\\''s/forkmesh' '/opt/it'\\''s/forkmesh'.bak-update; } && cp -f '" +
		built + "' '/opt/it'\\''s/forkmesh' && chmod 0755 '/opt/it'\\''s/forkmesh' && { { [ ! -e '/opt/it'\\''s/forkmesh-mirror-node' ] || mv -f '/opt/it'\\''s/forkmesh-mirror-node' '/opt/it'\\''s/forkmesh-mirror-node'.bak-update; } && cp -f '" +
		filepath.Join(dir, "build", "forkmesh-mirror-node") + "' '/opt/it'\\''s/forkmesh-mirror-node' && chmod 0755 '/opt/it'\\''s/forkmesh-mirror-node' || true; }"
	if script != want {
		t.Fatalf("script\n%s\nwant\n%s", script, want)
	}
}

func TestSudoStartCheckFailureKeepsTheInstall(t *testing.T) {
	s, sudoLog := sudoSandbox(t)
	t.Setenv("FAKE_SUDO_VERSION_FAIL", "1")
	core, _, ui, l := startModule(t)
	req := s.request()
	req.AsUser = "bob"
	coretest.MustCall(t, core, ui, "core.update.start", req, nil)
	result := collect(t, ui, 20*time.Second)
	if result.finish["error"] != "Update failed: the new build did not pass its start check; keeping the current version." {
		t.Fatalf("finished %v", result.finish)
	}
	if strings.Contains(strings.Join(readLines(sudoLog), "\n"), "sh -c") || len(l.list()) != 0 {
		t.Fatal("a failed start check went on to install")
	}
	if installed, _ := os.ReadFile(s.appPath); !strings.Contains(string(installed), "old") {
		t.Fatal("the install was replaced")
	}
}

func TestSudoFailedPullNeverDeletesOrClones(t *testing.T) {
	s, sudoLog := sudoSandbox(t)
	t.Setenv("FAKE_PULL_FAIL", "1")
	sentinel := filepath.Join(s.root, "src", "keep.txt")
	if err := os.WriteFile(sentinel, []byte("mine"), 0o644); err != nil {
		t.Fatal(err)
	}
	core, _, ui, _ := startModule(t)
	req := s.request()
	req.Kind = "update"
	req.AsUser = "bob"
	req.InstallURL = "https://mirror.invalid/forkmesh/forkmesh"
	req.RepoDir = filepath.Join(s.root, "src")
	coretest.MustCall(t, core, ui, "core.update.start", req, nil)
	result := collect(t, ui, 20*time.Second)
	reason := "Could not fast-forward from https://mirror.invalid/forkmesh/forkmesh (the mirror may be busy, or this checkout has its own commits)."
	if result.finish["error"] != reason+" Nothing in "+req.RepoDir+" was changed or removed." || result.finish["flash"] != reason {
		t.Fatalf("finished %v", result.finish)
	}
	if calls := strings.Join(readLines(sudoLog), "\n"); strings.Contains(calls, "clone") || strings.Contains(calls, "rm") || strings.Contains(calls, "cmake") {
		t.Fatalf("a failed pull went on: %s", calls)
	}
	if data, err := os.ReadFile(sentinel); err != nil || string(data) != "mine" {
		t.Fatal("the checkout was touched")
	}
	if _, err := os.Stat(filepath.Join(s.root, "src", ".git")); err != nil {
		t.Fatal("the checkout's .git is gone")
	}
}

func TestCleanRebuildClearsOnlyTheBuildFolder(t *testing.T) {
	s := newSandbox(t)
	build := filepath.Join(s.clientDir, "build")
	outside := filepath.Join(s.root, "outside")
	for _, dir := range []string{filepath.Join(build, "CMakeFiles"), outside} {
		if err := os.MkdirAll(dir, 0o755); err != nil {
			t.Fatal(err)
		}
	}
	stale := filepath.Join(build, "CMakeFiles", "stale.o")
	kept := filepath.Join(outside, "keep.txt")
	for _, path := range []string{stale, kept} {
		if err := os.WriteFile(path, []byte("x"), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	if err := os.Symlink(outside, filepath.Join(build, "linked")); err != nil {
		t.Fatal(err)
	}
	core, _, ui, _ := startModule(t)
	req := s.request()
	req.Clean = true
	coretest.MustCall(t, core, ui, "core.update.start", req, nil)
	result := collect(t, ui, 20*time.Second)
	if result.finish["ok"] != true {
		t.Fatalf("finished %v\n%s", result.finish, result.output.String())
	}
	text := result.output.String()
	if !strings.Contains(text, "==> Clearing build cache...\n") || strings.Index(text, "Clearing build cache") > strings.Index(text, "Configuring...") {
		t.Fatalf("output %s", text)
	}
	if _, err := os.Stat(stale); !os.IsNotExist(err) {
		t.Fatal("the build folder was not cleared")
	}
	if data, err := os.ReadFile(kept); err != nil || string(data) != "x" {
		t.Fatal("the wipe followed a symlink out of the build folder")
	}
	sawPhase := false
	for _, event := range result.events {
		if event.Name == "core.update.state" && strings.Contains(string(event.Data), `"phase":"clearing the build cache"`) {
			sawPhase = true
		}
	}
	if !sawPhase {
		t.Fatal("no clearing phase")
	}
}

func TestCleanRebuildRefusesAProtectedBuildFolder(t *testing.T) {
	s := newSandbox(t)
	build := filepath.Join(s.clientDir, "build")
	if err := os.MkdirAll(filepath.Join(build, ".git"), 0o755); err != nil {
		t.Fatal(err)
	}
	kept := filepath.Join(build, "keep.txt")
	if err := os.WriteFile(kept, []byte("x"), 0o644); err != nil {
		t.Fatal(err)
	}
	core, _, ui, l := startModule(t)
	req := s.request()
	req.Clean = true
	coretest.MustCall(t, core, ui, "core.update.start", req, nil)
	result := collect(t, ui, 20*time.Second)
	want := "Could not clear the build cache: " + build + " is a git checkout (it has a .git folder)"
	if result.finish["error"] != want {
		t.Fatalf("error %q", result.finish["error"])
	}
	if _, err := os.Stat(kept); err != nil || s.calls(t) != "" || len(l.list()) != 0 {
		t.Fatal("a refused wipe went on")
	}
	if err := os.Remove(filepath.Join(build, ".git")); err != nil {
		t.Fatal(err)
	}
	t.Setenv("HOME", build)
	coretest.MustCall(t, core, ui, "core.update.start", req, nil)
	result = collect(t, ui, 20*time.Second)
	want = "Could not clear the build cache: " + build + " is HOME (" + build + ") or contains it"
	if result.finish["error"] != want {
		t.Fatalf("error %q", result.finish["error"])
	}
	if _, err := os.Stat(kept); err != nil || s.calls(t) != "" {
		t.Fatal("a refused wipe went on")
	}
}

type prebuiltSandbox struct {
	root      string
	repo      string
	clientDir string
	appPath   string
	tempDir   string
	cas       string
	commit    string
	artifact  []byte
	hash      string
	companion []byte
	compHash  string
	hits      int32
	server    *httptest.Server
	serveBad  bool
}

func sha(data []byte) string {
	sum := sha256.Sum256(data)
	return hex.EncodeToString(sum[:])
}

func runGit(t *testing.T, dir string, args ...string) string {
	t.Helper()
	cmd := exec.Command("git", append([]string{"-c", "user.name=t", "-c", "user.email=t@t", "-c", "commit.gpgsign=false", "-c", "tag.gpgsign=false"}, args...)...)
	cmd.Dir = dir
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("git %v: %v\n%s", args, err, out)
	}
	return strings.TrimSpace(string(out))
}

func newPrebuiltSandbox(t *testing.T, manifest func(p *prebuiltSandbox) map[string]any) *prebuiltSandbox {
	t.Helper()
	if _, err := exec.LookPath("git"); err != nil {
		t.Skip("git not installed")
	}
	root, err := filepath.EvalSymlinks(t.TempDir())
	if err != nil {
		t.Fatal(err)
	}
	t.Setenv("HOME", filepath.Join(root, "home"))
	t.Setenv("XDG_CONFIG_HOME", filepath.Join(root, "home", ".config"))
	t.Setenv("GIT_CONFIG_NOSYSTEM", "1")
	for _, name := range []string{"FORKMESH_HOME", "FORKMESH_PROJECTS_DIR", "FORKMESH_PROJECT_DIR", "FORKMESH_DATA_DIR", "FORKMESH_CONFIG_DIR"} {
		t.Setenv(name, "")
	}
	p := &prebuiltSandbox{root: root, repo: filepath.Join(root, "src"), clientDir: filepath.Join(root, "src", "desktop"),
		appPath: filepath.Join(root, "install", "forkmesh"), tempDir: filepath.Join(root, "tmp"), cas: filepath.Join(root, "cas")}
	for _, dir := range []string{p.clientDir, filepath.Dir(p.appPath), p.tempDir, filepath.Join(p.repo, ".forkmesh", "releases", "latest")} {
		if err := os.MkdirAll(dir, 0o755); err != nil {
			t.Fatal(err)
		}
	}
	if err := os.WriteFile(p.appPath, []byte("#!/bin/sh\necho old\n"), 0o755); err != nil {
		t.Fatal(err)
	}
	p.artifact = []byte("#!/bin/sh\necho forkmesh-prebuilt\n")
	p.hash = sha(p.artifact)
	p.companion = []byte("#!/bin/sh\necho mirror-node\n")
	p.compHash = sha(p.companion)
	if err := os.WriteFile(filepath.Join(p.clientDir, "CMakeLists.txt"), []byte("project(x)\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	runGit(t, p.repo, "init", "-q")
	runGit(t, p.repo, "add", "-A")
	runGit(t, p.repo, "commit", "-q", "-m", "base")
	p.commit = runGit(t, p.repo, "rev-parse", "HEAD")
	if manifest != nil {
		data, _ := json.Marshal(manifest(p))
		if err := os.WriteFile(filepath.Join(p.repo, ".forkmesh", "releases", "latest", "release.json"), data, 0o644); err != nil {
			t.Fatal(err)
		}
		runGit(t, p.repo, "add", "-A")
		runGit(t, p.repo, "commit", "-q", "-m", "release metadata")
		p.commit = runGit(t, p.repo, "rev-parse", "HEAD")
	}
	p.server = httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		atomic.AddInt32(&p.hits, 1)
		switch r.URL.Path {
		case "/api/repo/forkmesh/forkmesh/releases/blob/sha256/" + p.hash:
			if p.serveBad {
				_, _ = w.Write([]byte("tampered"))
				return
			}
			_, _ = w.Write(p.artifact)
		case "/api/repo/forkmesh/forkmesh/releases/blob/sha256/" + p.compHash:
			_, _ = w.Write(p.companion)
		default:
			http.NotFound(w, r)
		}
	}))
	t.Cleanup(p.server.Close)
	return p
}

func standardManifest(p *prebuiltSandbox) map[string]any {
	return map[string]any{"tag": "v1.2.3", "tag_commit": "", "repo": "forkmesh/forkmesh", "assets": []any{
		map[string]any{"os": "macos", "arch": "x86_64", "name": "forkmesh-macos", "blob_sha256": strings.Repeat("a", 64)},
		map[string]any{"os": "linux", "arch": "x86_64", "name": "forkmesh-mirror-node-linux-x86_64", "blob_sha256": p.compHash},
		map[string]any{"os": "linux", "arch": "x86_64", "name": "forkmesh-linux-x86_64", "blob_sha256": strings.ToUpper(p.hash)},
		map[string]any{"os": "linux", "arch": "x86_64", "name": "second", "blob_sha256": strings.Repeat("b", 64)},
	}}
}

func (p *prebuiltSandbox) request(tagCommit string) PrebuiltRequest {
	return PrebuiltRequest{Request: Request{ClientDir: p.clientDir, AppPath: p.appPath, Args: []string{"--flag"}, Cwd: p.root,
		CatalogURL: p.server.URL, SelfPath: p.appPath}, Tag: "v1.2.3", TagCommit: tagCommit, OS: "linux", Arch: "x86_64",
		TempDir: p.tempDir}
}

type systemLog struct {
	mu    sync.Mutex
	lines []string
}

func (s *systemLog) add(text string) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.lines = append(s.lines, text)
}

func (s *systemLog) text() string {
	s.mu.Lock()
	defer s.mu.Unlock()
	return strings.Join(s.lines, "\n")
}

func startPrebuiltModule(t *testing.T) (*Module, *coretest.Peer, *launches, *systemLog, func(string, any, any) error) {
	core, module, ui, l := startModule(t)
	logs := &systemLog{}
	module.LogSystem = logs.add
	call := func(method string, params any, into any) error {
		raw, err := coretest.Call(t, core, ui, method, params)
		if err == nil && into != nil {
			_ = json.Unmarshal(raw, into)
		}
		return err
	}
	return module, ui, l, logs, call
}

func taggedManifest(tagCommit func(p *prebuiltSandbox) string) func(p *prebuiltSandbox) map[string]any {
	return func(p *prebuiltSandbox) map[string]any {
		manifest := standardManifest(p)
		manifest["tag_commit"] = tagCommit(p)
		return manifest
	}
}

func TestPrebuiltDownloadInstallsAndRelaunches(t *testing.T) {
	p := newPrebuiltSandbox(t, taggedManifest(func(p *prebuiltSandbox) string { return strings.ToUpper(p.commit) }))
	_, ui, l, logs, call := startPrebuiltModule(t)
	var reply map[string]any
	if err := call("core.update.prebuilt", p.request(p.commit), &reply); err != nil {
		t.Fatal(err)
	}
	if reply["handled"] != true || reply["started"] != true {
		t.Fatalf("reply %v", reply)
	}
	result := collect(t, ui, 20*time.Second)
	if result.finish["ok"] != true {
		t.Fatalf("finished %v\n%s\n%s", result.finish, result.output.String(), logs.text())
	}
	if installed, _ := os.ReadFile(p.appPath); string(installed) != string(p.artifact) {
		t.Fatalf("installed %q", installed)
	}
	if backup, _ := os.ReadFile(p.appPath + ".bak-update"); !strings.Contains(string(backup), "old") {
		t.Fatalf("backup %q", backup)
	}
	if companion, _ := os.ReadFile(filepath.Join(filepath.Dir(p.appPath), "forkmesh-mirror-node")); string(companion) != string(p.companion) {
		t.Fatalf("companion %q", companion)
	}
	wantLogs := "Auto-update: downloading the release v1.2.3 artifact (forkmesh-linux-x86_64)...\n" +
		"Auto-update: downloading the release v1.2.3 Go mirror-node companion...\n" +
		"Auto-update: installed the release v1.2.3 Go mirror-node companion."
	if logs.text() != wantLogs {
		t.Fatalf("system log\n%s", logs.text())
	}
	if !strings.Contains(strings.Join(result.logs, "\n"), "prebuilt update to v1.2.3 staged") ||
		!strings.Contains(result.output.String(), "==> Relaunching...") {
		t.Fatalf("restart log %v\n%s", result.logs, result.output.String())
	}
	calls := l.list()
	if len(calls) != 1 || strings.Join(calls[0], " ") != p.appPath+" --flag" {
		t.Fatalf("launches %v", calls)
	}
	for _, leftover := range []string{"forkmesh-update-" + p.hash[:12] + ".part", "forkmesh-mirror-node-update-" + p.compHash[:12] + ".part"} {
		if _, err := os.Stat(filepath.Join(p.tempDir, leftover)); !os.IsNotExist(err) {
			t.Fatalf("%s left behind", leftover)
		}
	}
	if staged, _ := os.ReadFile(filepath.Join(p.tempDir, "forkmesh-update-staged")); string(staged) != string(p.artifact) {
		t.Fatal("nothing staged")
	}
}

func TestPrebuiltChecksumMismatchFallsBackAndKeepsTheInstall(t *testing.T) {
	p := newPrebuiltSandbox(t, taggedManifest(func(p *prebuiltSandbox) string { return p.commit }))
	p.serveBad = true
	module, ui, l, logs, call := startPrebuiltModule(t)
	module.HasUI = func() bool { return true }
	module.CallUI = func(ctx context.Context, method string, params any) ([]byte, error) { return []byte(`{}`), nil }
	if err := call("core.update.prebuilt", p.request(p.commit), nil); err != nil {
		t.Fatal(err)
	}
	result := collect(t, ui, 20*time.Second)
	if result.finish["ok"] != false || result.finish["fallback"] != "source" {
		t.Fatalf("finished %v", result.finish)
	}
	if !strings.Contains(logs.text(), "Auto-update: artifact download for v1.2.3 failed (checksum mismatch); falling back to a source build.") {
		t.Fatalf("system log %s", logs.text())
	}
	if installed, _ := os.ReadFile(p.appPath); !strings.Contains(string(installed), "old") {
		t.Fatal("a mismatched artifact replaced the install")
	}
	if _, err := os.Stat(p.appPath + ".bak-update"); !os.IsNotExist(err) {
		t.Fatal("the install was moved aside")
	}
	entries, _ := os.ReadDir(p.tempDir)
	if len(entries) != 0 {
		t.Fatalf("temp files left: %v", entries)
	}
	if len(l.list()) != 0 || module.Busy() {
		t.Fatal("the fallback relaunched or stayed busy")
	}
}

func TestPrebuiltFallbackWithoutADesktopRunsTheSourceUpdate(t *testing.T) {
	p := newPrebuiltSandbox(t, taggedManifest(func(p *prebuiltSandbox) string { return p.commit }))
	p.serveBad = true
	_, ui, _, _, call := startPrebuiltModule(t)
	if err := call("core.update.prebuilt", p.request(p.commit), nil); err != nil {
		t.Fatal(err)
	}
	if first := collect(t, ui, 20*time.Second); first.finish["fallback"] != "source" {
		t.Fatalf("first %v", first.finish)
	}
	second := collect(t, ui, 20*time.Second)
	if !second.started || second.finish["error"] != "No online ForkMesh mirror is available right now. Try again shortly." {
		t.Fatalf("second %v", second.finish)
	}
	if _, err := os.Stat(filepath.Join(p.repo, ".git")); err != nil {
		t.Fatal("the checkout's .git is gone")
	}
	if installed, _ := os.ReadFile(p.appPath); !strings.Contains(string(installed), "old") {
		t.Fatal("the install was replaced")
	}
}

func TestPrebuiltFromTheLocalArtifactStore(t *testing.T) {
	p := newPrebuiltSandbox(t, taggedManifest(func(p *prebuiltSandbox) string { return p.commit }))
	for hash, data := range map[string][]byte{p.hash: p.artifact, p.compHash: p.companion} {
		blob := blobPath(p.cas, hash)
		if err := os.MkdirAll(filepath.Dir(blob), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(blob, data, 0o644); err != nil {
			t.Fatal(err)
		}
	}
	_, ui, l, logs, call := startPrebuiltModule(t)
	req := p.request(p.commit)
	req.Mirrors = []Mirror{{Identity: "other/repo", CasRoot: filepath.Join(p.root, "none")}, {Identity: "ForkMesh/ForkMesh", CasRoot: p.cas}}
	if err := call("core.update.prebuilt", req, nil); err != nil {
		t.Fatal(err)
	}
	result := collect(t, ui, 20*time.Second)
	if result.finish["ok"] != true {
		t.Fatalf("finished %v", result.finish)
	}
	want := "Auto-update: installing release v1.2.3 from this node's own artifact store (forkmesh-linux-x86_64).\n" +
		"Auto-update: installed the release v1.2.3 Go mirror-node companion from this node's own artifact store."
	if logs.text() != want {
		t.Fatalf("system log\n%s", logs.text())
	}
	if atomic.LoadInt32(&p.hits) != 0 {
		t.Fatal("downloaded although the store had the blobs")
	}
	if installed, _ := os.ReadFile(p.appPath); string(installed) != string(p.artifact) || len(l.list()) != 1 {
		t.Fatal("not installed from the store")
	}
}

func TestPrebuiltRefusalsMatchTheDesktop(t *testing.T) {
	t.Run("no manifest", func(t *testing.T) {
		p := newPrebuiltSandbox(t, nil)
		_, _, _, logs, call := startPrebuiltModule(t)
		var reply map[string]any
		if err := call("core.update.prebuilt", p.request(p.commit), &reply); err != nil {
			t.Fatal(err)
		}
		if reply["handled"] != false || logs.text() != "Auto-update: release v1.2.3 publishes no artifact manifest; building from source." {
			t.Fatalf("reply %v log %s", reply, logs.text())
		}
	})
	t.Run("manifest for another tag waits", func(t *testing.T) {
		p := newPrebuiltSandbox(t, func(p *prebuiltSandbox) map[string]any {
			manifest := standardManifest(p)
			manifest["tag"] = "v1.2.2"
			manifest["tag_commit"] = "abc"
			return manifest
		})
		_, _, l, logs, call := startPrebuiltModule(t)
		for range 2 {
			var reply map[string]any
			if err := call("core.update.prebuilt", p.request(p.commit), &reply); err != nil {
				t.Fatal(err)
			}
			if reply["handled"] != true || reply["started"] != false {
				t.Fatalf("reply %v", reply)
			}
		}
		if logs.text() != "Auto-update: release v1.2.3 is tagged but its signed artifacts are not published yet; waiting." || len(l.list()) != 0 {
			t.Fatalf("log %s", logs.text())
		}
	})
	t.Run("tag commit not contained", func(t *testing.T) {
		p := newPrebuiltSandbox(t, taggedManifest(func(p *prebuiltSandbox) string { return strings.Repeat("1", 40) }))
		_, _, _, logs, call := startPrebuiltModule(t)
		var reply map[string]any
		if err := call("core.update.prebuilt", p.request(p.commit), &reply); err != nil {
			t.Fatal(err)
		}
		if reply["handled"] != true || reply["started"] != false || !strings.Contains(logs.text(), "waiting") {
			t.Fatalf("reply %v log %s", reply, logs.text())
		}
	})
	t.Run("the upstream branch carries the promoted manifest", func(t *testing.T) {
		p := newPrebuiltSandbox(t, taggedManifest(func(p *prebuiltSandbox) string { return p.commit }))
		base := runGit(t, p.repo, "rev-parse", "HEAD~1")
		branch := runGit(t, p.repo, "rev-parse", "--abbrev-ref", "HEAD")
		runGit(t, p.repo, "checkout", "-q", "-b", "work", base)
		runGit(t, p.repo, "branch", "--set-upstream-to="+branch)
		_, ui, _, _, call := startPrebuiltModule(t)
		var reply map[string]any
		if err := call("core.update.prebuilt", p.request(base), &reply); err != nil {
			t.Fatal(err)
		}
		if reply["started"] != true {
			t.Fatalf("reply %v", reply)
		}
		if result := collect(t, ui, 20*time.Second); result.finish["ok"] != true {
			t.Fatalf("finished %v", result.finish)
		}
	})
	t.Run("no artifact for this platform", func(t *testing.T) {
		p := newPrebuiltSandbox(t, taggedManifest(func(p *prebuiltSandbox) string { return p.commit }))
		_, _, _, logs, call := startPrebuiltModule(t)
		req := p.request(p.commit)
		req.Arch = "arm64"
		var reply map[string]any
		if err := call("core.update.prebuilt", req, &reply); err != nil {
			t.Fatal(err)
		}
		if reply["handled"] != false || logs.text() != "Auto-update: release v1.2.3 has no prebuilt artifact for linux/arm64; building from source." {
			t.Fatalf("reply %v log %s", reply, logs.text())
		}
	})
}

func TestReleaseBlobURLEncodesSegments(t *testing.T) {
	got := releaseBlobURL("https://app.example/?q=1", "own er", "na/me", "abc")
	if got != "https://app.example/api/repo/own%20er/na%2Fme/releases/blob/sha256/abc?q=1" {
		t.Fatalf("url %q", got)
	}
}
