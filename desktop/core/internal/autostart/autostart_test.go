package autostart

import (
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

type recorder struct {
	calls    []string
	fail     map[string]bool
	regValue string
	linger   bool
}

func (r *recorder) run(name string, args ...string) ([]byte, error) {
	call := name + " " + strings.Join(args, " ")
	r.calls = append(r.calls, call)
	for prefix := range r.fail {
		if strings.HasPrefix(call, prefix) {
			return []byte("failed"), errors.New("exit status 1")
		}
	}
	if name == "loginctl" {
		switch {
		case args[0] == "show-user" && r.linger:
			return []byte("yes\n"), nil
		case args[0] == "show-user":
			return []byte("no\n"), nil
		case strings.Contains(call, "enable-linger"):
			r.linger = true
		case strings.Contains(call, "disable-linger"):
			r.linger = false
		}
		return nil, nil
	}
	if name == "reg" && args[0] == "query" {
		if r.regValue == "" {
			return nil, errors.New("not found")
		}
		return []byte("\n" + runKey + "\n    " + args[3] + "    REG_SZ    " + r.regValue + "\n"), nil
	}
	if name == "reg" && args[0] == "add" {
		r.regValue = args[7]
	}
	if name == "reg" && args[0] == "delete" {
		r.regValue = ""
	}
	return nil, nil
}

func fakeBinary(t *testing.T, dir string) string {
	path := filepath.Join(dir, "bin dir", "forkmesh-core")
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, []byte("#!/bin/sh\n"), 0o755); err != nil {
		t.Fatal(err)
	}
	return path
}

func manager(t *testing.T, goos string, systemd bool) (Manager, *recorder, string) {
	home := t.TempDir()
	rec := &recorder{fail: map[string]bool{}}
	if !systemd {
		rec.fail["systemctl --user show-environment"] = true
	}
	return Manager{GOOS: goos, Home: home, UID: 501, Env: func(string) string { return "" }, Run: rec.run,
		LookPath: func(string) (string, error) { return "/usr/bin/systemctl", nil }}, rec, home
}

func TestSystemdUnitRoundTrip(t *testing.T) {
	m, rec, home := manager(t, "linux", true)
	binary := fakeBinary(t, home)
	status, err := m.Enable(binary, "forkmesh")
	if err != nil {
		t.Fatal(err)
	}
	if !status.Enabled || status.Mechanism != MechanismSystemd || status.Binary != binary {
		t.Fatalf("status %+v", status)
	}
	unit, _ := os.ReadFile(filepath.Join(home, ".config/systemd/user/forkmesh-core-forkmesh.service"))
	for _, want := range []string{"Restart=on-failure", "KillMode=process", `"serve" "--project" "forkmesh" "--background"`, "WantedBy=default.target"} {
		if !strings.Contains(string(unit), want) {
			t.Fatalf("unit lacks %q:\n%s", want, unit)
		}
	}
	if !strings.Contains(strings.Join(rec.calls, "\n"), "systemctl --user enable forkmesh-core-forkmesh.service") {
		t.Fatalf("calls %v", rec.calls)
	}
	status, err = m.Disable("forkmesh")
	if err != nil || status.Enabled {
		t.Fatalf("disable %+v %v", status, err)
	}
	if !strings.Contains(strings.Join(rec.calls, "\n"), "systemctl --user disable forkmesh-core-forkmesh.service") {
		t.Fatalf("calls %v", rec.calls)
	}
}

func TestSystemdStartsAtBootThroughLinger(t *testing.T) {
	m, rec, home := manager(t, "linux", true)
	binary := fakeBinary(t, home)
	if status := m.Status("forkmesh"); status.AtBoot {
		t.Fatalf("at boot before enable %+v", status)
	}
	status, err := m.Enable(binary, "forkmesh")
	if err != nil {
		t.Fatal(err)
	}
	if !status.AtBoot || !rec.linger {
		t.Fatalf("status %+v calls %v", status, rec.calls)
	}
	if !strings.Contains(strings.Join(rec.calls, "\n"), "loginctl --no-ask-password enable-linger 501") {
		t.Fatalf("calls %v", rec.calls)
	}
	if _, err := m.Enable(binary, "work"); err != nil {
		t.Fatal(err)
	}
	if _, err := m.Disable("forkmesh"); err != nil || !rec.linger {
		t.Fatalf("linger dropped while another project starts at boot: %v", err)
	}
	if status, err := m.Disable("work"); err != nil || rec.linger || status.AtBoot {
		t.Fatalf("linger kept after the last project: %+v %v", status, err)
	}
	if _, err := os.Stat(m.lingerMarker()); !errors.Is(err, os.ErrNotExist) {
		t.Fatalf("marker left behind: %v", err)
	}
}

func TestSystemdKeepsLingerItDidNotTurnOn(t *testing.T) {
	m, rec, home := manager(t, "linux", true)
	rec.linger = true
	if _, err := m.Enable(fakeBinary(t, home), "forkmesh"); err != nil {
		t.Fatal(err)
	}
	if _, err := m.Disable("forkmesh"); err != nil || !rec.linger {
		t.Fatalf("turned off linger the user had on: %v calls %v", err, rec.calls)
	}
	if strings.Contains(strings.Join(rec.calls, "\n"), "enable-linger") {
		t.Fatalf("calls %v", rec.calls)
	}
}

func TestSystemdStartsAtLoginWhenLingerRefused(t *testing.T) {
	m, rec, home := manager(t, "linux", true)
	rec.fail["loginctl --no-ask-password enable-linger"] = true
	status, err := m.Enable(fakeBinary(t, home), "forkmesh")
	if err != nil || !status.Enabled || status.AtBoot {
		t.Fatalf("status %+v %v", status, err)
	}
	if _, err := os.Stat(m.lingerMarker()); !errors.Is(err, os.ErrNotExist) {
		t.Fatalf("marker written after refusal: %v", err)
	}
}

func TestXDGFallbackWithoutSystemd(t *testing.T) {
	m, _, home := manager(t, "linux", false)
	binary := fakeBinary(t, home)
	status, err := m.Enable(binary, "work")
	if err != nil {
		t.Fatal(err)
	}
	if status.Mechanism != MechanismXDG || !status.Enabled || status.Binary != binary {
		t.Fatalf("status %+v", status)
	}
	if _, err := m.Disable("work"); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(filepath.Join(home, ".config/autostart/forkmesh-core-work.desktop")); !errors.Is(err, os.ErrNotExist) {
		t.Fatalf("entry left behind: %v", err)
	}
}

func TestLaunchAgent(t *testing.T) {
	m, rec, home := manager(t, "darwin", false)
	binary := fakeBinary(t, home)
	status, err := m.Enable(binary, "forkmesh")
	if err != nil {
		t.Fatal(err)
	}
	if status.Mechanism != MechanismLaunchAgent || !status.Enabled || status.Binary != binary {
		t.Fatalf("status %+v", status)
	}
	plist, _ := os.ReadFile(status.Location)
	if !strings.Contains(string(plist), "<key>SuccessfulExit</key>") || !strings.Contains(string(plist), "<string>--background</string>") {
		t.Fatalf("plist:\n%s", plist)
	}
	if !strings.Contains(strings.Join(rec.calls, "\n"), "launchctl bootstrap gui/501 ") {
		t.Fatalf("calls %v", rec.calls)
	}
	if status, _ := m.Disable("forkmesh"); status.Enabled {
		t.Fatal("still enabled")
	}
}

func TestWindowsRunKey(t *testing.T) {
	m, rec, home := manager(t, "windows", false)
	binary := fakeBinary(t, home)
	status, err := m.Enable(binary, "forkmesh")
	if err != nil {
		t.Fatal(err)
	}
	if !status.Enabled || status.Binary != binary || !strings.HasSuffix(rec.regValue, "--hide-console") {
		t.Fatalf("status %+v value %q", status, rec.regValue)
	}
	if status, _ := m.Disable("forkmesh"); status.Enabled {
		t.Fatal("still enabled")
	}
}

func TestRefusesBadInput(t *testing.T) {
	m, _, home := manager(t, "linux", true)
	binary := fakeBinary(t, home)
	if _, err := m.Enable(binary, "../evil"); !errors.Is(err, ErrBadProject) {
		t.Fatalf("slug: %v", err)
	}
	if _, err := m.Enable("relative/forkmesh-core", "forkmesh"); !errors.Is(err, ErrBadBinary) {
		t.Fatalf("relative: %v", err)
	}
	if _, err := m.Enable(filepath.Join(home, "missing"), "forkmesh"); !errors.Is(err, ErrBadBinary) {
		t.Fatalf("missing: %v", err)
	}
}

func TestQuotingSurvivesHostilePaths(t *testing.T) {
	for _, binary := range []string{`/opt/a b/forkmesh-core`, `/opt/100%/"x"/$HOME\core`} {
		if got := execStartBinary(SystemdUnit(binary, "forkmesh")); got != binary {
			t.Fatalf("systemd %q -> %q", binary, got)
		}
		if got := desktopExecBinary(DesktopEntry(binary, "forkmesh")); got != binary {
			t.Fatalf("desktop %q -> %q", binary, got)
		}
		if got := plistBinary(LaunchAgentPlist(binary, "forkmesh", "/Users/x")); got != binary {
			t.Fatalf("plist %q -> %q", binary, got)
		}
	}
}
