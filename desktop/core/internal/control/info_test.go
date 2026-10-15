package control

import (
	"errors"
	"io"
	"log"
	"os"
	"path/filepath"
	"reflect"
	"testing"

	"forkmesh.com/desktop-core/internal/coretest"
	"forkmesh.com/desktop-core/internal/daemon"
)

func restartControl(t *testing.T, previous *daemon.Core) (*daemon.Core, *Module) {
	t.Helper()
	core := daemon.NewCore(previous.Layout, "test", log.New(io.Discard, "", 0))
	core.Identity = previous.Identity
	module := New()
	module.StorePath = filepath.Join(t.TempDir(), "paired.json")
	coretest.Start(t, core, module)
	return core, module
}

func TestStartKnowsTheNodeTheDesktopLastNamed(t *testing.T) {
	h := newHarness(t, func(m *Module, core *daemon.Core) {
		m.StorePath = filepath.Join(t.TempDir(), "paired.json")
	})
	ui := h.attachUI()
	ui.mustCall("control.info", map[string]any{"name": "desk", "user": "alice", "node": " Desk "})
	path := filepath.Join(h.core.Layout.CoreState(), savedInfoFile)
	if info, err := os.Stat(path); err != nil || info.Mode().Perm() != 0o600 {
		t.Fatalf("saved node name %v %v", info, err)
	}
	h.module.Stop()

	core, module := restartControl(t, h.core)
	got := module.call(func() any { return savedInfo{Name: module.name, User: module.user, Node: module.node} })
	if got != (savedInfo{Name: "desk", User: "alice", Node: "desk"}) {
		t.Fatalf("restarted module knows %+v", got)
	}
	want := map[string]any{"name": "desk", "user": "alice", "node": "desk"}
	if value := core.Get("control.info"); !reflect.DeepEqual(value, want) {
		t.Fatalf("control.info %v", value)
	}
	module.Stop()

	again := newHarness(t, func(m *Module, c *daemon.Core) {
		c.Layout = core.Layout
		m.StorePath = filepath.Join(t.TempDir(), "paired.json")
	})
	again.attachUI().mustCall("control.info", map[string]any{})
	if _, err := os.Stat(path); !errors.Is(err, os.ErrNotExist) {
		t.Fatalf("an empty node name kept the saved one: %v", err)
	}
}
