package mesh

import (
	"encoding/base64"
	"errors"
	"io"
	"log"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"forkmesh.com/desktop-core/internal/coretest"
	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/rpc"
)

func restartMeshCore(t *testing.T, previous *meshCore) *meshCore {
	t.Helper()
	core := daemon.NewCore(previous.core.Layout, "test", log.New(io.Discard, "", 0))
	module := New()
	module.Options = previous.module.Options
	coretest.Start(t, core, module)
	ui := coretest.NewPeer("ui:2", rpc.UI)
	core.Hub.Add(ui)
	return &meshCore{core: core, module: module, ui: ui}
}

func TestStartReopensTheRoomWithTheLastSettingsUntilStopped(t *testing.T) {
	a := newMeshCore(t)
	key := randomBytes(32)
	encoded := base64.StdEncoding.EncodeToString(key)
	settings := "[chat]\ne2ee\\x25519PrivateV2=@ByteArray(" + encoded + ")\n"
	if err := os.MkdirAll(filepath.Dir(a.core.Layout.SettingsFile()), 0o700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(a.core.Layout.SettingsFile(), []byte(settings), 0o600); err != nil {
		t.Fatal(err)
	}
	a.call(t, "core.mesh.start", map[string]any{"nodeId": "boot-node", "userName": "ada", "nodeName": "box",
		"ownerUser": "ada", "sealKey": encoded, "control": true})
	a.call(t, "core.mesh.identity", map[string]any{"userName": "ada l", "nodeName": "box", "ownerUser": "ada"})
	path := filepath.Join(a.core.Layout.CoreState(), savedStartFile)
	raw, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	if strings.Contains(string(raw), encoded) || strings.Contains(string(raw), "sealKey") {
		t.Fatalf("the seal key was written to %s: %s", path, raw)
	}
	if info, _ := os.Stat(path); info.Mode().Perm() != 0o600 {
		t.Fatalf("mode %v", info.Mode())
	}
	a.module.Stop()

	b := restartMeshCore(t, a)
	state := b.state(t)
	if !state.Running || state.NodeID != "boot-node" || state.SealKeyID != KeyIDFor(mustSeal(t, RoomName, key).public) {
		t.Fatalf("restarted core did not reopen the room: %+v", state)
	}
	if b.module.saved.UserName != "ada l" || !b.module.saved.Control {
		t.Fatalf("restarted with %+v", b.module.saved)
	}
	b.call(t, "core.mesh.stop", nil)
	if _, err := os.Stat(path); !errors.Is(err, os.ErrNotExist) {
		t.Fatalf("stop kept the saved settings: %v", err)
	}
	b.module.Stop()

	c := restartMeshCore(t, b)
	if c.state(t).Running {
		t.Fatal("a stopped room reopened at start")
	}
}
