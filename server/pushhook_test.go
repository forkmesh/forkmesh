package mirrornode

import (
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestPushWakeHookQueuesExactGenerationPublish(t *testing.T) {
	repository := filepath.Join(t.TempDir(), "repository.git")
	if err := os.MkdirAll(filepath.Join(repository, "hooks"), 0700); err != nil {
		t.Fatal(err)
	}
	changed, err := ensurePushWakeHook(repository, "127.0.0.1:8791")
	if err != nil || !changed {
		t.Fatalf("install hook: changed=%v err=%v", changed, err)
	}
	path := filepath.Join(repository, "hooks", "post-receive")
	raw, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	script := string(raw)
	for _, wanted := range []string{
		pushWakeHookMarker,
		"-X POST http://127.0.0.1:8791/v1/control/sync?local=1",
		"--connect-timeout 2",
		"--max-time 5",
		"|| :",
	} {
		if !strings.Contains(script, wanted) {
			t.Fatalf("hook is missing %q:\n%s", wanted, script)
		}
	}
	info, err := os.Stat(path)
	if err != nil || info.Mode().Perm()&0100 == 0 {
		t.Fatalf("hook is not executable: %v %+v", err, info)
	}
	changed, err = ensurePushWakeHook(repository, "127.0.0.1:8791")
	if err != nil || changed {
		t.Fatalf("identical hook was rewritten: changed=%v err=%v", changed, err)
	}
}

func TestPushWakeHookUpdatesOnlyServiceOwnedHook(t *testing.T) {
	repository := filepath.Join(t.TempDir(), "repository.git")
	hooks := filepath.Join(repository, "hooks")
	if err := os.MkdirAll(hooks, 0700); err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(hooks, "post-receive")
	custom := []byte("#!/bin/sh\necho operator-hook\n")
	if err := os.WriteFile(path, custom, 0700); err != nil {
		t.Fatal(err)
	}
	changed, err := ensurePushWakeHook(repository, "127.0.0.1:8792")
	if err != nil || changed {
		t.Fatalf("operator hook was claimed: changed=%v err=%v", changed, err)
	}
	current, err := os.ReadFile(path)
	if err != nil || string(current) != string(custom) {
		t.Fatalf("operator hook changed: err=%v body=%q", err, current)
	}

	managed := []byte("#!/bin/sh\n" + pushWakeHookMarker + "exit 0\n")
	if err := os.WriteFile(path, managed, 0700); err != nil {
		t.Fatal(err)
	}
	changed, err = ensurePushWakeHook(repository, "127.0.0.1:8792")
	if err != nil || !changed {
		t.Fatalf("managed hook was not upgraded: changed=%v err=%v", changed, err)
	}
	current, err = os.ReadFile(path)
	if err != nil || !strings.Contains(
		string(current), "http://127.0.0.1:8792/v1/control/sync") {
		t.Fatalf("managed hook was not refreshed: err=%v body=%q", err, current)
	}
}

func TestPushWakeHookRejectsSymlinkedHooksDirectory(t *testing.T) {
	repository := filepath.Join(t.TempDir(), "repository.git")
	outside := filepath.Join(t.TempDir(), "outside")
	if err := os.MkdirAll(repository, 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(outside, 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.Symlink(outside, filepath.Join(repository, "hooks")); err != nil {
		t.Fatal(err)
	}
	if changed, err := ensurePushWakeHook(
		repository, "127.0.0.1:8791"); err == nil || changed {
		t.Fatalf("symlinked hooks path accepted: changed=%v err=%v", changed, err)
	}
	if _, err := os.Stat(filepath.Join(outside, "post-receive")); !errors.Is(err, os.ErrNotExist) {
		t.Fatalf("hook escaped repository through symlink: %v", err)
	}
}

func TestDaemonEnsuresPushWakeHookForReadyRepositories(t *testing.T) {
	repository := filepath.Join(t.TempDir(), "repository.git")
	if err := os.MkdirAll(filepath.Join(repository, "objects"), 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(
		filepath.Join(repository, "HEAD"), []byte("ref: refs/heads/main\n"),
		0600); err != nil {
		t.Fatal(err)
	}
	daemon := &Daemon{
		config: Config{Listen: "127.0.0.1:8791"},
		gateway: GatewayConfig{Repositories: []Repository{
			{Owner: "forkmesh", Name: "forkmesh", GitDir: repository},
			// Gateway aliases can share the same bare repository.
			{Owner: "alias", Name: "forkmesh", GitDir: repository},
			// A release-store entry is skipped until Git initializes it.
			{Owner: "releases", Name: "artifacts", GitDir: filepath.Join(
				t.TempDir(), "not-initialized.git")},
		}},
	}
	if err := daemon.ensurePushWakeHooks(); err != nil {
		t.Fatal(err)
	}
	raw, err := os.ReadFile(filepath.Join(repository, "hooks", "post-receive"))
	if err != nil || !strings.Contains(string(raw), pushWakeHookMarker) {
		t.Fatalf("daemon did not install its push hook: err=%v body=%q", err, raw)
	}
}

func TestPushWakeURLStaysOnValidatedLoopback(t *testing.T) {
	for input, wanted := range map[string]string{
		"127.0.0.1:8791": "http://127.0.0.1:8791/v1/control/sync?local=1",
		"localhost:9000": "http://localhost:9000/v1/control/sync?local=1",
		"[::1]:8123":     "http://[::1]:8123/v1/control/sync?local=1",
	} {
		got, err := pushWakeURL(input)
		if err != nil || got != wanted {
			t.Fatalf("pushWakeURL(%q)=%q,%v want %q", input, got, err, wanted)
		}
	}
	for _, input := range []string{
		"0.0.0.0:8791", "mirror.example:8791", "127.0.0.1:not-a-port",
	} {
		if got, err := pushWakeURL(input); err == nil || got != "" {
			t.Fatalf("unsafe control endpoint accepted: %q -> %q", input, got)
		}
	}
}
