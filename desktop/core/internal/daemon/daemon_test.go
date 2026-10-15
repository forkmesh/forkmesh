package daemon_test

import (
	"testing"

	"forkmesh.com/desktop-core/internal/coretest"
	"forkmesh.com/desktop-core/internal/rpc"
)

func TestBuiltins(t *testing.T) {
	core := coretest.New(t)
	ui := coretest.NewPeer("ui:1", rpc.UI)
	phone := coretest.NewPeer("lan:1", rpc.LAN)
	var status map[string]any
	coretest.MustCall(t, core, ui, "core.status", nil, &status)
	if status["project"] != "forkmesh" || status["version"] != "test" {
		t.Fatalf("status %v", status)
	}
	var owners struct {
		Owners []string `json:"owners"`
	}
	coretest.MustCall(t, core, ui, "core.owners", map[string]any{"names": []string{" jett ", "", "Jett"}}, &owners)
	if len(owners.Owners) != 2 {
		t.Fatalf("owners %v", owners)
	}
	if got, ok := core.Get("control.owners").([]string); !ok || got[0] != "jett" {
		t.Fatalf("stored %v", core.Get("control.owners"))
	}
	for _, method := range []string{"core.owners", "core.shutdown", "core.status", "core.routes"} {
		if _, err := coretest.Call(t, core, phone, method, nil); rpc.Code(err) != "forbidden" {
			t.Fatalf("%s from phone: %v", method, err)
		}
	}
}
