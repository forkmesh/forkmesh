package daemon_test

import (
	"context"
	"testing"

	"forkmesh.com/desktop-core/internal/coretest"
	"forkmesh.com/desktop-core/internal/daemon"
)

type namedModule string

func (m namedModule) Name() string                                       { return string(m) }
func (m namedModule) Start(ctx context.Context, core *daemon.Core) error { return nil }

func TestModuleLookup(t *testing.T) {
	core := coretest.New(t)
	if core.Module("control") != nil {
		t.Fatal("a core with no modules finds none")
	}
	coretest.Start(t, core, namedModule("control"), namedModule("mesh"))
	if got := core.Module("mesh"); got == nil || got.Name() != "mesh" {
		t.Fatalf("mesh lookup %v", got)
	}
	if core.Module("absent") != nil {
		t.Fatal("an unknown name finds nothing")
	}
}
