package phonefamily

import (
	"context"
	"os"
	"path/filepath"
	"testing"

	"forkmesh.com/desktop-core/internal/coretest"
	"forkmesh.com/desktop-core/internal/rpc"
)

func TestConfirmationsMatchTheDesktopStore(t *testing.T) {
	c := NewConfirmations()
	token := c.Issue("dev#1", "send", "Owner/Repo", 1000)
	if len(token) != 48 {
		t.Fatalf("token %q", token)
	}
	if v := c.Consume(token, "dev#1", "send", "owner/repo", 2000); v != Ok {
		t.Fatalf("case-insensitive target: %v", v)
	}
	if v := c.Consume(token, "dev#1", "send", "owner/repo", 2000); v != Missing {
		t.Fatalf("reuse: %v", v)
	}
	token = c.Issue("dev#1", "send", "x", 1000)
	if v := c.Consume(token, "dev#2", "send", "x", 2000); v != Mismatch {
		t.Fatalf("holder: %v", v)
	}
	if v := c.Consume(token, "dev#1", "send", "x", 2000); v != Missing {
		t.Fatalf("a wrong guess must spend the token: %v", v)
	}
	token = c.Issue("dev#1", "send", "x", 1000)
	if v := c.Consume(token, "dev#1", "send", "x", 1000+ConfirmationLifetimeMs); v != Expired {
		t.Fatalf("expiry: %v", v)
	}
	token = c.Issue("dev#1", "send", "x", 1000)
	if v := c.Consume(token, "dev#1", "other", "x", 2000); v != Mismatch {
		t.Fatalf("action: %v", v)
	}
	for i := 0; i < 40; i++ {
		c.Issue("dev", "a", "b", int64(5000+i))
	}
	if n := c.Outstanding(); n != maxOutstanding {
		t.Fatalf("outstanding %d", n)
	}
	for v, want := range map[Verdict]string{Ok: "", Missing: "confirmation_required", Expired: "confirmation_expired", Mismatch: "confirmation_mismatch"} {
		if got := ConfirmationError(v); got != want {
			t.Errorf("%v: %q", v, got)
		}
	}
}

func TestParams(t *testing.T) {
	p := ParamsOf(&rpc.Request{Params: []byte(`{"s":"x","n":3,"f":2.5,"b":true,"o":{"k":1},"wrong":7}`)})
	if p.String("s") != "x" || p.String("wrong") != "" || p.String("missing") != "" {
		t.Fatal("strings")
	}
	if p.Int("n", -1) != 3 || p.Int("f", -1) != -1 || p.Int("s", 9) != 9 || p.Int("missing", 4) != 4 {
		t.Fatal("ints")
	}
	if b, ok := p.Bool("b"); !b || !ok {
		t.Fatal("bool")
	}
	if _, ok := p.Bool("n"); ok {
		t.Fatal("bool from number")
	}
	if len(p.Object("o")) != 1 {
		t.Fatal("object")
	}
}

func TestGateUsesPushedOwnersAndTheSavedAccount(t *testing.T) {
	core := coretest.New(t)
	gate := &Gate{Core: core}
	handler := gate.Wrap(func(ctx context.Context, req *rpc.Request) (any, error) { return "ok", nil })
	phone := coretest.NewPeer("p", rpc.LAN)
	phone.Dev.User = "Bob"
	if _, err := handler(context.Background(), &rpc.Request{Peer: phone}); rpc.Code(err) != "not_owner" {
		t.Fatalf("no owner yet: %v", err)
	}
	core.Set("control.owners", []string{"bob"})
	if _, err := handler(context.Background(), &rpc.Request{Peer: phone}); err != nil {
		t.Fatal(err)
	}
	core.Set("control.owners", nil)
	path := core.Layout.SettingsFile()
	_ = os.MkdirAll(filepath.Dir(path), 0o700)
	if err := os.WriteFile(path, []byte("[profile]\nhandle=B.o.b!\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	if _, err := handler(context.Background(), &rpc.Request{Peer: phone}); err != nil {
		t.Fatalf("account from the profile handle: %v", err)
	}
	ui := coretest.NewPeer("ui", rpc.UI)
	ui.Dev = rpc.Device{}
	if _, err := handler(context.Background(), &rpc.Request{Peer: ui}); err != nil {
		t.Fatalf("desktop: %v", err)
	}
	relay := coretest.NewPeer("r", rpc.Relay)
	relay.Dev.Name = ""
	relay.Dev.User = "bob"
	if got := DeviceLabel(&rpc.Request{Peer: relay}); got != "Phone (bob), through the relay" {
		t.Fatalf("label %q", got)
	}
	if got := Holder(&rpc.Request{Peer: relay}); got != relay.Dev.Key+"#r" {
		t.Fatalf("holder %q", got)
	}
}
