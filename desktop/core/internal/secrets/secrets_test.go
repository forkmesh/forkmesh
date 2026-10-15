package secrets

import (
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	"forkmesh.com/desktop-core/internal/coretest"
	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/identity"
	"forkmesh.com/desktop-core/internal/rpc"
	"forkmesh.com/desktop-core/internal/vault"
)

type fakeDesktop struct {
	mu        sync.Mutex
	variables map[string]string
	applied   []map[string]any
}

func (d *fakeDesktop) snapshot() map[string]string {
	d.mu.Lock()
	defer d.mu.Unlock()
	out := map[string]string{}
	for k, v := range d.variables {
		out[k] = v
	}
	return out
}

func (d *fakeDesktop) apply(ctx context.Context, params map[string]any) error {
	d.mu.Lock()
	defer d.mu.Unlock()
	d.applied = append(d.applied, params)
	name := params["name"].(string)
	if params["action"] == "delete" {
		if _, ok := d.variables[name]; !ok {
			return rpc.Fail("not_found")
		}
		delete(d.variables, name)
		return nil
	}
	d.variables[name] = params["value"].(string)
	return nil
}

func setup(t *testing.T, vars map[string]string) (*daemon.Core, *Module, *fakeDesktop, *coretest.Peer) {
	t.Helper()
	core := coretest.New(t)
	core.Set("control.owners", []any{"alice"})
	desktop := &fakeDesktop{variables: vars}
	module := New()
	module.Variables = desktop.snapshot
	module.Apply = desktop.apply
	coretest.Start(t, core, module)
	phone := coretest.NewPeer("phone-1", rpc.LAN)
	phone.Dev.User = "Alice"
	core.Hub.Add(phone)
	return core, module, desktop, phone
}

func call(t *testing.T, core *daemon.Core, peer rpc.Peer, method string, params any) (map[string]any, string) {
	t.Helper()
	raw, err := coretest.Call(t, core, peer, method, params)
	if err != nil {
		return nil, rpc.Code(err)
	}
	out := map[string]any{}
	if err := json.Unmarshal(raw, &out); err != nil {
		t.Fatalf("%s: %v", method, err)
	}
	return out, ""
}

func TestListShowsValidNamesOnlyAndNeverValues(t *testing.T) {
	core, _, _, phone := setup(t, map[string]string{"ZETA": "z", "alpha": "secret-a", "9bad": "x", "has-dash": "y", "_ok1": "v"})
	raw, err := coretest.Call(t, core, phone, "secrets.list", nil)
	if err != nil {
		t.Fatal(err)
	}
	if strings.Contains(string(raw), "secret-a") {
		t.Fatalf("list leaked a value: %s", raw)
	}
	want := `{"variables":[{"name":"ZETA"},{"name":"_ok1"},{"name":"alpha"}]}`
	if string(raw) != want {
		t.Fatalf("got %s want %s", raw, want)
	}
}

func TestListCapsRows(t *testing.T) {
	vars := map[string]string{}
	for i := 0; i < 600; i++ {
		vars["V"+strings.Repeat("A", i%5)+itoa(i)] = ""
	}
	core, _, _, phone := setup(t, vars)
	out, code := call(t, core, phone, "secrets.list", nil)
	if code != "" {
		t.Fatal(code)
	}
	if n := len(out["variables"].([]any)); n != maxRows {
		t.Fatalf("rows %d", n)
	}
}

func itoa(i int) string {
	return strings.TrimLeft(strings.Replace(time.Duration(i).String(), "ns", "", 1), " ")
}

func TestOwnerGate(t *testing.T) {
	core, _, _, _ := setup(t, map[string]string{})
	stranger := coretest.NewPeer("stranger", rpc.Relay)
	stranger.Dev.User = "mallory"
	for _, method := range []string{"secrets.list", "secrets.prepare", "secrets.set", "secrets.delete"} {
		if _, code := call(t, core, stranger, method, nil); code != "not_owner" {
			t.Fatalf("%s: %q", method, code)
		}
	}
	keyless := coretest.NewPeer("keyless", rpc.LAN)
	keyless.Dev.User = "alice"
	keyless.Dev.Key = ""
	if _, code := call(t, core, keyless, "secrets.list", nil); code != "not_owner" {
		t.Fatalf("keyless: %q", code)
	}
}

func TestPrepareValidation(t *testing.T) {
	core, _, _, phone := setup(t, map[string]string{"EXISTING": "v"})
	cases := []struct {
		params map[string]any
		code   string
	}{
		{map[string]any{"action": "rename", "name": "A"}, "bad_request"},
		{map[string]any{"name": "A"}, "bad_request"},
		{map[string]any{"action": "set", "name": "1A"}, "invalid_name"},
		{map[string]any{"action": "set", "name": strings.Repeat("A", 129)}, "invalid_name"},
		{map[string]any{"action": "set", "name": ""}, "invalid_name"},
		{map[string]any{"action": "delete", "name": "MISSING"}, "not_found"},
	}
	for _, c := range cases {
		if _, code := call(t, core, phone, "secrets.prepare", c.params); code != c.code {
			t.Errorf("%v: %q want %q", c.params, code, c.code)
		}
	}
	out, code := call(t, core, phone, "secrets.prepare", map[string]any{"action": "set", "name": strings.Repeat("A", 128)})
	if code != "" {
		t.Fatal(code)
	}
	if out["expiresInMs"].(float64) != 60000 || out["action"] != "set" || len(out["token"].(string)) != 48 {
		t.Fatalf("prepare %v", out)
	}
	out, _ = call(t, core, phone, "secrets.prepare", map[string]any{"action": "delete", "name": "EXISTING"})
	if out["summary"] != "Delete the Actions variable EXISTING from this desktop." {
		t.Fatalf("summary %q", out["summary"])
	}
}

func TestSetAndDeleteNeedTheirOwnToken(t *testing.T) {
	core, _, desktop, phone := setup(t, map[string]string{"OLD": "1"})
	prep, _ := call(t, core, phone, "secrets.prepare", map[string]any{"action": "set", "name": "NEW"})
	token := prep["token"].(string)
	if prep["summary"] != "Set the Actions variable NEW on this desktop." {
		t.Fatalf("summary %q", prep["summary"])
	}
	if _, code := call(t, core, phone, "secrets.set", map[string]any{"name": "OTHER", "value": "x", "token": token}); code != "confirmation_mismatch" {
		t.Fatalf("other name: %q", code)
	}
	if _, code := call(t, core, phone, "secrets.set", map[string]any{"name": "NEW", "value": "x", "token": token}); code != "confirmation_required" {
		t.Fatalf("spent token: %q", code)
	}
	prep, _ = call(t, core, phone, "secrets.prepare", map[string]any{"action": "set", "name": "NEW"})
	if _, code := call(t, core, phone, "secrets.delete", map[string]any{"name": "NEW", "token": prep["token"]}); code != "confirmation_mismatch" {
		t.Fatalf("delete with a set token: %q", code)
	}
	prep, _ = call(t, core, phone, "secrets.prepare", map[string]any{"action": "set", "name": "NEW"})
	other := coretest.NewPeer("phone-2", rpc.LAN)
	other.Dev.User = "alice"
	if _, code := call(t, core, other, "secrets.set", map[string]any{"name": "NEW", "value": "x", "token": prep["token"]}); code != "confirmation_mismatch" {
		t.Fatalf("other phone: %q", code)
	}
	prep, _ = call(t, core, phone, "secrets.prepare", map[string]any{"action": "set", "name": "NEW"})
	out, code := call(t, core, phone, "secrets.set", map[string]any{"name": "NEW", "value": "s3cret", "token": prep["token"]})
	if code != "" || out["ok"] != true {
		t.Fatalf("set: %v %q", out, code)
	}
	if desktop.snapshot()["NEW"] != "s3cret" {
		t.Fatalf("not applied: %v", desktop.snapshot())
	}
	phone.Next(t, "secrets.changed", 2*time.Second)
	prep, _ = call(t, core, phone, "secrets.prepare", map[string]any{"action": "delete", "name": "OLD"})
	if _, code := call(t, core, phone, "secrets.delete", map[string]any{"name": "OLD", "token": prep["token"]}); code != "" {
		t.Fatalf("delete: %q", code)
	}
	if _, ok := desktop.snapshot()["OLD"]; ok {
		t.Fatal("OLD still there")
	}
}

func TestSetRejectsBeforeSpendingTheToken(t *testing.T) {
	core, _, desktop, phone := setup(t, map[string]string{})
	prep, _ := call(t, core, phone, "secrets.prepare", map[string]any{"action": "set", "name": "BIG"})
	if _, code := call(t, core, phone, "secrets.set", map[string]any{"name": "BIG", "value": strings.Repeat("x", maxValueBytes+1), "token": prep["token"]}); code != "value_too_large" {
		t.Fatalf("too large: %q", code)
	}
	if _, code := call(t, core, phone, "secrets.set", map[string]any{"name": "BIG", "value": strings.Repeat("x", maxValueBytes), "token": prep["token"]}); code != "" {
		t.Fatalf("token was spent by the size check: %q", code)
	}
	if len(desktop.applied) != 1 {
		t.Fatalf("applied %d", len(desktop.applied))
	}
}

func TestExpiredToken(t *testing.T) {
	core, module, _, phone := setup(t, map[string]string{})
	now := time.Now()
	module.Now = func() time.Time { return now }
	prep, _ := call(t, core, phone, "secrets.prepare", map[string]any{"action": "set", "name": "A"})
	now = now.Add(61 * time.Second)
	if _, code := call(t, core, phone, "secrets.set", map[string]any{"name": "A", "value": "", "token": prep["token"]}); code != "confirmation_expired" {
		t.Fatalf("expired: %q", code)
	}
}

func TestDeleteRaceReportsNotFound(t *testing.T) {
	core, _, desktop, phone := setup(t, map[string]string{"GONE": "1"})
	prep, _ := call(t, core, phone, "secrets.prepare", map[string]any{"action": "delete", "name": "GONE"})
	desktop.mu.Lock()
	delete(desktop.variables, "GONE")
	desktop.mu.Unlock()
	if _, code := call(t, core, phone, "secrets.delete", map[string]any{"name": "GONE", "token": prep["token"]}); code != "not_found" {
		t.Fatalf("race: %q", code)
	}
}

func TestReadsVariablesFromSettingsAndNeedsTheDesktopToWrite(t *testing.T) {
	core := coretest.New(t)
	core.Set("control.owners", []any{"alice"})
	path := core.Layout.SettingsFile()
	if err := os.MkdirAll(filepath.Dir(path), 0o700); err != nil {
		t.Fatal(err)
	}
	conf := "[actions]\nvariables=\"@ByteArray({\\\"VULTR_API_KEY\\\":\\\"k\\\",\\\"CODEMAGIC_TOKEN\\\":\\\"t\\\"})\"\n"
	if err := os.WriteFile(path, []byte(conf), 0o600); err != nil {
		t.Fatal(err)
	}
	module := New()
	coretest.Start(t, core, module)
	phone := coretest.NewPeer("phone", rpc.LAN)
	phone.Dev.User = "alice"
	raw, err := coretest.Call(t, core, phone, "secrets.list", nil)
	if err != nil {
		t.Fatal(err)
	}
	if string(raw) != `{"variables":[{"name":"CODEMAGIC_TOKEN"},{"name":"VULTR_API_KEY"}]}` {
		t.Fatalf("list %s", raw)
	}
	prep, _ := call(t, core, phone, "secrets.prepare", map[string]any{"action": "delete", "name": "VULTR_API_KEY"})
	if _, code := call(t, core, phone, "secrets.delete", map[string]any{"name": "VULTR_API_KEY", "token": prep["token"]}); code != "desktop_unavailable" {
		t.Fatalf("no desktop: %q", code)
	}
	ui := coretest.NewPeer("ui", rpc.UI)
	if _, code := call(t, core, ui, "secrets.list", nil); code != "" {
		t.Fatalf("desktop list: %q", code)
	}
}

func TestListReadsTheVault(t *testing.T) {
	core := coretest.New(t)
	core.Set("control.owners", []any{"alice"})
	path := core.Layout.SettingsFile()
	if err := os.MkdirAll(filepath.Dir(path), 0o700); err != nil {
		t.Fatal(err)
	}
	conf := "[actions]\nvariables=\"@ByteArray({\\\"VULTR_API_KEY\\\":\\\"" + vault.Marker + "\\\",\\\"GONE\\\":\\\"" + vault.Marker + "\\\"})\"\n"
	if err := os.WriteFile(path, []byte(conf), 0o600); err != nil {
		t.Fatal(err)
	}
	id, err := identity.FromSeed(make([]byte, 32))
	if err != nil {
		t.Fatal(err)
	}
	store := &vault.Module{Identities: func() []vault.Signer { return []vault.Signer{id} }, Interval: time.Hour}
	module := New()
	coretest.Start(t, core, store, module)
	if _, err := store.Apply(vault.ActionsNamespace, map[string]string{"VULTR_API_KEY": "k", "CODEMAGIC_TOKEN": "t"}, nil, false); err != nil {
		t.Fatal(err)
	}
	phone := coretest.NewPeer("phone", rpc.LAN)
	phone.Dev.User = "alice"
	raw, err := coretest.Call(t, core, phone, "secrets.list", nil)
	if err != nil {
		t.Fatal(err)
	}
	if string(raw) != `{"variables":[{"name":"CODEMAGIC_TOKEN"},{"name":"VULTR_API_KEY"}]}` {
		t.Fatalf("list %s", raw)
	}
	if _, code := call(t, core, phone, "secrets.prepare", map[string]any{"action": "delete", "name": "GONE"}); code != "not_found" {
		t.Fatalf("a marker without a vault entry is not a variable: %q", code)
	}
}
