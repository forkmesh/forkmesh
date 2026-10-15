package teammates

import (
	"testing"

	"forkmesh.com/desktop-core/internal/coretest"
	"forkmesh.com/desktop-core/internal/rpc"
)

func TestOptionsMatchTheDesktop(t *testing.T) {
	core := coretest.New(t)
	core.Set("control.owners", []any{"alice"})
	coretest.Start(t, core, New())
	phone := coretest.NewPeer("phone", rpc.LAN)
	phone.Dev.User = "alice"
	raw, err := coretest.Call(t, core, phone, "teammates.options", nil)
	if err != nil {
		t.Fatal(err)
	}
	want := `{"editable":["name","avatar","specialty","focus","description","instructions","provider","model","mode","skills","backupModels","taskAgents","schedule","relayChannel","enabled","selfImproving"],` +
		`"modes":["","Auto","Plan","Ask"],"providers":["codex","claude-code","openai","claude-api"],` +
		`"scheduleModes":["on-demand","continuous","hourly","daily","weekly","weekdays","custom","events"],` +
		`"taskTypes":[{"id":"scheduled","label":"Scheduled work"},{"id":"new-issues","label":"New issues"},{"id":"new-pulls","label":"New pull requests"},` +
		`{"id":"assignments","label":"Assignments"},{"id":"mentions","label":"Mentions"},{"id":"ui-stalls","label":"UI stall repair"},{"id":"error-logs","label":"Error-log repair"}]}`
	if string(raw) != want {
		t.Fatalf("got  %s\nwant %s", raw, want)
	}
	stranger := coretest.NewPeer("stranger", rpc.Relay)
	stranger.Dev.User = "mallory"
	if _, err := coretest.Call(t, core, stranger, "teammates.options", nil); rpc.Code(err) != "not_owner" {
		t.Fatalf("stranger: %v", err)
	}
	if core.Registry.Native("teammates.list") {
		t.Fatal("teammates.list must stay with the desktop")
	}
}
