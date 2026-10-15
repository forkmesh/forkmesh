package control

import (
	"os"
	"path/filepath"
	"testing"

	"forkmesh.com/desktop-core/internal/daemon"
)

func TestCodePairedPhoneCannotRenameItselfToTheOwner(t *testing.T) {
	phoneKey := newSigner(t)
	store := filepath.Join(t.TempDir(), "paired.json")
	if err := os.WriteFile(store, storeSample(map[string]any{"key": phoneKey.public, "name": "Guest phone",
		"user": "bob", "pairedAtMs": 1, "approval": "code"}), 0o600); err != nil {
		t.Fatal(err)
	}
	h := newHarness(t, func(m *Module, core *daemon.Core) { m.StorePath = store })
	ui := h.attachUI()
	ui.mustCall("control.info", map[string]any{"name": "desk", "user": "alice", "node": "desk"})
	addr := h.startMesh()
	phone := dialPhone(t, addr, phoneKey, false)
	if phone.event("session")["paired"] != true {
		t.Fatal("a code-paired phone reconnects on the LAN")
	}
	opened := ui.waitNote("ui.control.opened", nil)
	connID := intOf(t, opened["connection"])
	var user string
	h.do(func() { user = h.module.Peer(connID).Device().User })
	if user != "bob" {
		t.Fatalf("the phone's claimed user replaced the approved one: %q", user)
	}
	if stored := LoadDevices(store)[phoneKey.public]; stored.User != "bob" {
		t.Fatalf("the stored user was rewritten: %#v", stored)
	}
}
