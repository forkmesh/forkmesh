package mirrornode

import (
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func TestTokenEnvironmentRequiresOwnerOnlyFileAndRedactsAlternates(t *testing.T) {
	path := filepath.Join(t.TempDir(), "token")
	if err := os.WriteFile(path, []byte("abc\n"), 0600); err != nil {
		t.Fatal(err)
	}
	t.Setenv("CF_TOKEN", "leak")
	t.Setenv("TUNNEL_TOKEN", "old")
	env, err := tokenEnvironment(path)
	if err != nil {
		t.Fatal(err)
	}
	joined := strings.Join(env, "\n")
	if strings.Contains(joined, "leak") || strings.Contains(joined, "TUNNEL_TOKEN=old") || !strings.Contains(joined, "TUNNEL_TOKEN=abc") {
		t.Fatalf("unsafe env: %s", joined)
	}
	if err := os.Chmod(path, 0644); err != nil {
		t.Fatal(err)
	}
	if _, err := tokenEnvironment(path); err == nil {
		t.Fatal("weak token permissions accepted")
	}
	if err := os.Chmod(path, 0600); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, []byte("a\nb"), 0600); err != nil {
		t.Fatal(err)
	}
	if _, err := tokenEnvironment(path); err == nil {
		t.Fatal("multiline token accepted")
	}
}

func TestSupervisorRestartsConfigurationChangesWithoutCrashBackoff(t *testing.T) {
	supervisor := NewSupervisor()
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan struct{})
	go func() {
		supervisor.Run(ctx, []ProcessSpec{{
			Name:    "gateway",
			Program: "/usr/bin/python3",
			Args: []string{"-c", `import signal,sys
signal.signal(signal.SIGTERM,lambda *_:sys.exit(0))
while True: signal.pause()`},
		}})
		close(done)
	}()
	defer func() {
		cancel()
		select {
		case <-done:
		case <-time.After(6 * time.Second):
			t.Error("supervisor did not stop its restarted child")
		}
	}()

	deadline := time.Now().Add(time.Second)
	for time.Now().Before(deadline) {
		running, _ := supervisor.Snapshot()
		if running["gateway"] && supervisor.Restart("gateway") {
			break
		}
		time.Sleep(10 * time.Millisecond)
	}

	deadline = time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		running, restarts := supervisor.Snapshot()
		if running["gateway"] && restarts["gateway"] >= 1 {
			return
		}
		time.Sleep(10 * time.Millisecond)
	}
	t.Fatal("intentional restart was held behind the five-second crash backoff")
}

func TestSupervisorReloadSignalsWithoutRestarting(t *testing.T) {
	marker := filepath.Join(t.TempDir(), "reloaded")
	supervisor := NewSupervisor()
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan struct{})
	go func() {
		supervisor.Run(ctx, []ProcessSpec{{
			Name:    "gateway",
			Program: "/usr/bin/python3",
			Args: []string{"-c", `import os,pathlib,signal,sys
marker=pathlib.Path(os.environ["RELOAD_MARKER"])
signal.signal(signal.SIGHUP,lambda *_:marker.write_text("ok"))
signal.signal(signal.SIGTERM,lambda *_:sys.exit(0))
marker.write_text("ready")
while True: signal.pause()`},
			Env: append(os.Environ(), "RELOAD_MARKER="+marker),
		}})
		close(done)
	}()
	defer func() {
		cancel()
		select {
		case <-done:
		case <-time.After(6 * time.Second):
			t.Error("supervisor did not stop its reloaded child")
		}
	}()

	deadline := time.Now().Add(2 * time.Second)
	ready := false
	for time.Now().Before(deadline) {
		if raw, err := os.ReadFile(marker); err == nil && string(raw) == "ready" {
			ready = true
			break
		}
		time.Sleep(10 * time.Millisecond)
	}
	if !ready {
		t.Fatal("gateway did not install its reload handler")
	}
	deadline = time.Now().Add(2 * time.Second)
	requested := false
	for time.Now().Before(deadline) {
		running, _ := supervisor.Snapshot()
		if running["gateway"] && supervisor.Reload("gateway") {
			requested = true
			break
		}
		time.Sleep(10 * time.Millisecond)
	}
	if !requested {
		t.Fatal("reload signal was not accepted")
	}
	for time.Now().Before(deadline) {
		if raw, err := os.ReadFile(marker); err == nil && string(raw) == "ok" {
			running, restarts := supervisor.Snapshot()
			if !running["gateway"] || restarts["gateway"] != 0 {
				t.Fatalf("reload replaced the process: running=%v restarts=%v",
					running, restarts)
			}
			return
		}
		time.Sleep(10 * time.Millisecond)
	}
	t.Fatal("gateway did not receive the reload signal")
}

func TestSupervisorInvalidatesCachesWithoutRestarting(t *testing.T) {
	marker := filepath.Join(t.TempDir(), "invalidated")
	supervisor := NewSupervisor()
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan struct{})
	go func() {
		supervisor.Run(ctx, []ProcessSpec{{
			Name:    "gateway",
			Program: "/usr/bin/python3",
			Args: []string{"-c", `import os,pathlib,signal,sys
marker=pathlib.Path(os.environ["INVALIDATE_MARKER"])
signal.signal(signal.SIGUSR1,lambda *_:marker.write_text("ok"))
signal.signal(signal.SIGTERM,lambda *_:sys.exit(0))
marker.write_text("ready")
while True: signal.pause()`},
			Env: append(os.Environ(), "INVALIDATE_MARKER="+marker),
		}})
		close(done)
	}()
	defer func() {
		cancel()
		select {
		case <-done:
		case <-time.After(6 * time.Second):
			t.Error("supervisor did not stop its cache-invalidated child")
		}
	}()

	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		if raw, err := os.ReadFile(marker); err == nil && string(raw) == "ready" {
			break
		}
		time.Sleep(10 * time.Millisecond)
	}
	requested := false
	for time.Now().Before(deadline) {
		running, _ := supervisor.Snapshot()
		if running["gateway"] && supervisor.Invalidate("gateway") {
			requested = true
			break
		}
		time.Sleep(10 * time.Millisecond)
	}
	if !requested {
		t.Fatal("cache invalidation signal was not accepted")
	}
	for time.Now().Before(deadline) {
		if raw, err := os.ReadFile(marker); err == nil && string(raw) == "ok" {
			running, restarts := supervisor.Snapshot()
			if !running["gateway"] || restarts["gateway"] != 0 {
				t.Fatalf("cache invalidation replaced the process: running=%v restarts=%v",
					running, restarts)
			}
			return
		}
		time.Sleep(10 * time.Millisecond)
	}
	t.Fatal("gateway did not receive the cache invalidation signal")
}

func TestSupervisorRestartsAChildKilledByItsControlSignalWithoutBackoff(t *testing.T) {
	supervisor := NewSupervisor()
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan struct{})
	go func() {
		supervisor.Run(ctx, []ProcessSpec{{
			Name:    "gateway",
			Program: "/usr/bin/python3",
			Args: []string{"-c", `import signal,sys
signal.signal(signal.SIGTERM,lambda *_:sys.exit(0))
while True: signal.pause()`},
		}})
		close(done)
	}()
	defer func() {
		cancel()
		select {
		case <-done:
		case <-time.After(6 * time.Second):
			t.Error("supervisor did not stop its child")
		}
	}()

	deadline := time.Now().Add(2 * time.Second)
	requested := false
	for time.Now().Before(deadline) {
		if instance, running := supervisor.Instance("gateway"); running && instance == 1 && supervisor.Invalidate("gateway") {
			requested = true
			break
		}
		time.Sleep(10 * time.Millisecond)
	}
	if !requested {
		t.Fatal("cache invalidation signal was not accepted")
	}
	deadline = time.Now().Add(3 * time.Second)
	for time.Now().Before(deadline) {
		if instance, running := supervisor.Instance("gateway"); running && instance == 2 {
			return
		}
		time.Sleep(10 * time.Millisecond)
	}
	t.Fatal("a child killed by its control signal waited for the crash backoff")
}
