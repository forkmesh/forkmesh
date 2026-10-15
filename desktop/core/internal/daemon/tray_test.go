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
