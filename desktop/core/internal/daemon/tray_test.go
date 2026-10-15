package daemon

import (
	"os"
	"path/filepath"
	"reflect"
	"testing"
)

func TestGraphicalSession(t *testing.T) {
	none := func(string) string { return "" }
	wayland := func(name string) string {
		if name == "WAYLAND_DISPLAY" {
			return "wayland-0"
		}
		return ""
	}
	cases := []struct {
		goos string
		env  func(string) string
		want bool
	}{
		{"linux", none, false},
		{"linux", wayland, true},
		{"linux", func(name string) string { return map[string]string{"DISPLAY": ":0"}[name] }, true},
		{"darwin", none, true},
		{"windows", none, true},
	}
	for _, tc := range cases {
		if got := graphicalSession(tc.goos, tc.env); got != tc.want {
			t.Fatalf("%s: %v", tc.goos, got)
		}
	}
}

func TestTrayCommandStartsDesktopBesideEngine(t *testing.T) {
	dir := t.TempDir()
	engine := filepath.Join(dir, "forkmesh-core")
	display := func(name string) string { return map[string]string{"DISPLAY": ":0"}[name] }
	if _, _, ok := trayCommand("linux", display, engine, "work"); ok {
		t.Fatal("tray without a desktop binary")
	}
	desktop := filepath.Join(dir, "forkmesh")
	if err := os.WriteFile(desktop, []byte("#!/bin/sh\n"), 0o755); err != nil {
		t.Fatal(err)
	}
	binary, args, ok := trayCommand("linux", display, engine, "work")
	if !ok || binary != desktop || !reflect.DeepEqual(args, []string{"--tray", "--project", "work"}) {
		t.Fatalf("%v %q %v", ok, binary, args)
	}
	if _, _, ok := trayCommand("linux", func(string) string { return "" }, engine, "work"); ok {
		t.Fatal("tray with no display")
	}
	if _, _, ok := trayCommand("linux", display, engine, "../x"); ok {
		t.Fatal("tray for a bad project")
	}
}

func TestTrayRunsInItsOwnScopeWithoutTheServiceVariables(t *testing.T) {
	program, args := scopedTray("linux", "/usr/bin/systemd-run", "/opt/forkmesh", []string{"--tray", "--project", "work"}, "work")
	want := []string{"--user", "--scope", "--quiet", "--collect", "--unit=forkmesh-tray-work", "--", "/opt/forkmesh",
		"--tray", "--project", "work"}
	if program != "/usr/bin/systemd-run" || !reflect.DeepEqual(args, want) {
		t.Fatalf("%s %v", program, args)
	}
	if program, args := scopedTray("linux", "", "/opt/forkmesh", []string{"--tray"}, "work"); program != "/opt/forkmesh" || !reflect.DeepEqual(args, []string{"--tray"}) {
		t.Fatalf("without systemd-run %s %v", program, args)
	}
	if program, _ := scopedTray("darwin", "/usr/bin/systemd-run", "/opt/forkmesh", nil, "work"); program != "/opt/forkmesh" {
		t.Fatalf("darwin %s", program)
	}
	env := withoutServiceVariables([]string{"DISPLAY=:0", "INVOCATION_ID=abc", "JOURNAL_STREAM=8:9", "HOME=/h", "MANAGERPID=1"})
	if !reflect.DeepEqual(env, []string{"DISPLAY=:0", "HOME=/h"}) {
		t.Fatalf("env %v", env)
	}
}
