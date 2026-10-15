package ownergate

import (
	"context"
	"testing"

	"forkmesh.com/desktop-core/internal/coretest"
	"forkmesh.com/desktop-core/internal/rpc"
)

func TestGateRefusesStrangersAndForbidsPhonesOnDesktopMethods(t *testing.T) {
	core := coretest.New(t)
	core.Set("control.owners", []any{"Alice"})
	gate := &Gate{Core: core}
	handler := gate.Wrap(func(ctx context.Context, req *rpc.Request) (any, error) { return "ok", nil })
	owner := coretest.NewPeer("owner", rpc.LAN)
	owner.Dev.User = "alice"
	stranger := coretest.NewPeer("stranger", rpc.Relay)
	stranger.Dev.User = "mallory"
	ui := coretest.NewPeer("ui", rpc.UI)
	for _, c := range []struct {
		peer rpc.Peer
		want string
	}{{owner, ""}, {stranger, "not_owner"}, {ui, ""}} {
		_, err := handler(context.Background(), &rpc.Request{Peer: c.peer})
		if rpc.Code(err) != c.want {
			t.Errorf("%s: %v, want %q", c.peer.ID(), err, c.want)
		}
	}
	only := DesktopOnly(func(ctx context.Context, req *rpc.Request) (any, error) { return "ok", nil })
	if _, err := only(context.Background(), &rpc.Request{Peer: owner}); rpc.Code(err) != "forbidden" {
		t.Fatalf("phone reached a desktop method: %v", err)
	}
	if _, err := only(context.Background(), &rpc.Request{Peer: ui}); err != nil {
		t.Fatal(err)
	}
	if got := DeviceLabel(stranger); got != "stranger (mallory), through the relay" {
		t.Fatalf("label %q", got)
	}
}
