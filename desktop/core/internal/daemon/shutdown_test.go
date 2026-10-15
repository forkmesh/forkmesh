package daemon_test

import (
	"testing"

	"forkmesh.com/desktop-core/internal/coretest"
	"forkmesh.com/desktop-core/internal/rpc"
)

func TestShutdownHardFlag(t *testing.T) {
	core := coretest.New(t)
	ui := coretest.NewPeer("ui:1", rpc.UI)
	var reply struct {
		Stopping bool `json:"stopping"`
		Hard     bool `json:"hard"`
	}
	coretest.MustCall(t, core, ui, "core.shutdown", nil, &reply)
	if !reply.Stopping || reply.Hard || core.Get("core.shutdown.hard") != nil {
		t.Fatalf("soft shutdown %+v %v", reply, core.Get("core.shutdown.hard"))
	}
	coretest.MustCall(t, core, ui, "core.shutdown", map[string]any{"hard": true}, &reply)
	if !reply.Hard || core.Get("core.shutdown.hard") != true {
		t.Fatalf("hard shutdown %+v %v", reply, core.Get("core.shutdown.hard"))
	}
}
