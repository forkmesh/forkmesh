package main

import (
	"encoding/json"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"syscall"
	"testing"
	"time"
)

type sandbox struct {
	t      *testing.T
	home   string
	binary string
}

func buildCore(t *testing.T, dir, version string) string {
	binary := filepath.Join(dir, "forkmesh-core")
	cmd := exec.Command("go", "build", "-o", binary, "-ldflags", "-X main.version="+version, ".")
	cmd.Env = append(os.Environ(), "CGO_ENABLED=0")
	if out, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("build: %v\n%s", err, out)
	}
	return binary
}

func newSandbox(t *testing.T) *sandbox {
	if runtime.GOOS == "windows" {
		t.Skip("unix process control")
	}
	if testing.Short() {
		t.Skip("builds and runs the binary")
	}
	home := t.TempDir()
	s := &sandbox{t: t, home: home, binary: buildCore(t, t.TempDir(), "it-1")}
	t.Cleanup(func() {
		_, _ = s.run("stop")
		if pid := s.pid(); pid > 0 {
			_ = syscall.Kill(pid, syscall.SIGKILL)
		}
	})
	return s
}

func (s *sandbox) env() []string {
	return []string{"PATH=" + os.Getenv("PATH"), "HOME=" + s.home, "XDG_CONFIG_HOME=" + filepath.Join(s.home, ".config"),
		"XDG_DATA_HOME=" + filepath.Join(s.home, ".local/share"), "XDG_CACHE_HOME=" + filepath.Join(s.home, ".cache"),
		"TMPDIR=" + filepath.Join(s.home, "tmp")}
}

func (s *sandbox) run(args ...string) (string, error) {
	_ = os.MkdirAll(filepath.Join(s.home, "tmp"), 0o700)
	cmd := exec.Command(s.binary, args...)
	cmd.Env = s.env()
	out, err := cmd.CombinedOutput()
	return string(out), err
}

func (s *sandbox) state() string {
	return filepath.Join(s.home, "projects", "forkmesh", "state", "core")
}

func (s *sandbox) pid() int {
	raw, err := os.ReadFile(filepath.Join(s.state(), "endpoint.json"))
	if err != nil {
		return 0
	}
	var endpoint struct {
		PID int `json:"pid"`
	}
	_ = json.Unmarshal(raw, &endpoint)
	return endpoint.PID
}

func (s *sandbox) status() map[string]any {
	out, err := s.run("status")
	if err != nil {
		return nil
	}
	var status map[string]any
	_ = json.Unmarshal([]byte(out), &status)
	return status
}

func (s *sandbox) waitFor(what string, check func() bool) {
	s.t.Helper()
	for deadline := time.Now().Add(20 * time.Second); time.Now().Before(deadline); time.Sleep(100 * time.Millisecond) {
		if check() {
			return
		}
	}
	s.t.Fatalf("timed out waiting for %s", what)
}

func alive(pid int) bool { return pid > 0 && syscall.Kill(pid, 0) == nil }

func TestDaemonRunsWithoutAnyUI(t *testing.T) {
	s := newSandbox(t)
	if out, err := s.run("start"); err != nil || !strings.Contains(out, "started") {
		t.Fatalf("start: %v %s", err, out)
	}
	status := s.status()
	if status == nil || status["background"] != true || status["ui"] != false || status["version"] != "it-1" {
		t.Fatalf("status %+v", status)
	}
	if exit, _ := status["lastExit"].(map[string]any); exit["kind"] != "first" {
		t.Fatalf("lastExit %+v", status["lastExit"])
	}
	if out, _ := s.run("start"); !strings.Contains(out, "already running") {
		t.Fatalf("second start: %s", out)
	}
	pid := s.pid()
	if out, err := s.run("serve", "--background"); err != nil || !strings.Contains(out, "already running") {
		t.Fatalf("a second background serve must exit 0 quietly: %v %s", err, out)
	}
	if out, err := s.run("call", "core.cancel", `{"id":12345}`); err != nil || !strings.Contains(out, `"cancelled": false`) {
		t.Fatalf("cancel of an unknown request: %v %s", err, out)
	}
	if out, err := s.run("plugins", "list"); err != nil || !strings.Contains(out, `"plugins"`) {
		t.Fatalf("plugins list: %v %s", err, out)
	}
	time.Sleep(1500 * time.Millisecond)
	if !alive(pid) || s.pid() != pid {
		t.Fatal("background core did not stay up with no UI")
	}
	if _, err := s.run("stop"); err != nil {
		t.Fatal(err)
	}
	s.waitFor("clean stop", func() bool { return !alive(pid) })
	if _, err := os.Stat(filepath.Join(s.state(), "endpoint.json")); !os.IsNotExist(err) {
		t.Fatalf("endpoint left after a clean stop: %v", err)
	}
	if _, err := s.run("start"); err != nil {
		t.Fatal(err)
	}
	if exit, _ := s.status()["lastExit"].(map[string]any); exit["kind"] != "clean" {
		t.Fatalf("lastExit after a clean stop %+v", exit)
	}
}

func TestDaemonRecordsCrashAndRecovers(t *testing.T) {
	s := newSandbox(t)
	if _, err := s.run("start"); err != nil {
		t.Fatal(err)
	}
	pid := s.pid()
	_ = syscall.Kill(pid, syscall.SIGKILL)
	s.waitFor("crash", func() bool { return !alive(pid) })
	if _, err := s.run("start"); err != nil {
		t.Fatal(err)
	}
	exit, _ := s.status()["lastExit"].(map[string]any)
	if exit["kind"] != "crash" || int(exit["pid"].(float64)) != pid {
		t.Fatalf("lastExit after kill -9: %+v", exit)
	}
}

func TestBackgroundSettingSurvivesRestartAndLingerHonoursIt(t *testing.T) {
	s := newSandbox(t)
	if out, err := s.run("background", "on"); err != nil || !strings.Contains(out, `"background": true`) {
		t.Fatalf("background on while stopped: %v %s", err, out)
	}
	cmd := exec.Command(s.binary, "serve", "--linger", "1s")
	cmd.Env = s.env()
	logFile, _ := os.Create(filepath.Join(s.home, "serve.log"))
	defer logFile.Close()
	cmd.Stdout, cmd.Stderr = logFile, logFile
	t.Cleanup(func() {
		if t.Failed() {
			raw, _ := os.ReadFile(filepath.Join(s.home, "serve.log"))
			t.Logf("serve output: %s", raw)
		}
	})
	if err := cmd.Start(); err != nil {
		t.Fatal(err)
	}
	go cmd.Wait()
	s.waitFor("serve", func() bool { return s.status() != nil })
	time.Sleep(3 * time.Second)
	if s.status() == nil {
		t.Fatal("core with background on exited after linger")
	}
	if out, err := s.run("background", "off"); err != nil || !strings.Contains(out, `"background": false`) {
		t.Fatalf("background off: %v %s", err, out)
	}
	pid := s.pid()
	s.waitFor("linger exit", func() bool { return !alive(pid) })
}

func TestDaemonRestartsOnReplacedBinary(t *testing.T) {
	s := newSandbox(t)
	if _, err := s.run("start"); err != nil {
		t.Fatal(err)
	}
	pid := s.pid()
	next := buildCore(t, t.TempDir(), "it-2")
	raw, err := os.ReadFile(next)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(s.binary+".new", raw, 0o755); err != nil {
		t.Fatal(err)
	}
	future := time.Now().Add(5 * time.Second)
	_ = os.Chtimes(s.binary+".new", future, future)
	if err := os.Rename(s.binary+".new", s.binary); err != nil {
		t.Fatal(err)
	}
	s.waitFor("upgrade", func() bool {
		status := s.status()
		return status != nil && status["version"] == "it-2"
	})
	if s.pid() != pid {
		t.Fatalf("upgrade changed the pid %d -> %d; the unix restart should exec in place", pid, s.pid())
	}
}
