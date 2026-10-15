package agentmodel

import (
	"context"
	"encoding/json"
	"os"
	"os/exec"
	"path/filepath"
	"sync"
	"testing"
	"time"

	"forkmesh.com/desktop-core/internal/agentrun"
	"forkmesh.com/desktop-core/internal/agents"
	"forkmesh.com/desktop-core/internal/coretest"
	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/qtjson"
	"forkmesh.com/desktop-core/internal/rpc"
)

type fakeDesktop struct {
	mu       sync.Mutex
	attached bool
	calls    []string
	notes    []string
	context  qtjson.Value
	usage    qtjson.Value
	writes   []json.RawMessage
}

func (d *fakeDesktop) call(ctx context.Context, method string, params any) ([]byte, error) {
	d.mu.Lock()
	defer d.mu.Unlock()
	d.calls = append(d.calls, method)
	if !d.attached {
		return nil, rpc.Fail("desktop_unavailable")
	}
	switch method {
	case "ui.agents.context":
		return d.context.Compact(), nil
	case "ui.agents.usage":
		return d.usage.Compact(), nil
	case "ui.settings.write":
		raw, _ := json.Marshal(params)
		d.writes = append(d.writes, raw)
		return []byte(`{}`), nil
	}
	return []byte(`{}`), nil
}

func (d *fakeDesktop) notify(method string, params any) bool {
	d.mu.Lock()
	defer d.mu.Unlock()
	if d.attached {
		d.notes = append(d.notes, method)
	}
	return d.attached
}

func (h *harness) attach(attached bool) {
	h.desktop.mu.Lock()
	h.desktop.attached = attached
	h.desktop.mu.Unlock()
}

func (d *fakeDesktop) isAttached() bool {
	d.mu.Lock()
	defer d.mu.Unlock()
	return d.attached
}

func (d *fakeDesktop) noted(method string) int {
	d.mu.Lock()
	defer d.mu.Unlock()
	n := 0
	for _, note := range d.notes {
		if note == method {
			n++
		}
	}
	return n
}

type harness struct {
	t       *testing.T
	core    *daemon.Core
	store   *agents.Module
	run     *agentrun.Module
	model   *Module
	desktop *fakeDesktop
	ui      *coretest.Peer
	phone   *coretest.Peer
}

func newHarness(t *testing.T) *harness {
	t.Helper()
	core := coretest.New(t)
	store := agents.New()
	run := agentrun.New()
	model := New()
	desktop := &fakeDesktop{context: qtjson.NewObject()}
	model.CallUI = desktop.call
	model.NotifyUI = desktop.notify
	model.Attached = desktop.isAttached
	coretest.Start(t, core, store, run, model)
	h := &harness{t: t, core: core, store: store, run: run, model: model, desktop: desktop,
		ui: coretest.NewPeer("ui:1", rpc.UI), phone: coretest.NewPeer("lan:1", rpc.LAN)}
	h.phone.Dev = rpc.Device{Key: "k1", Name: "Pixel", User: "alice"}
	core.Hub.Add(h.phone)
	core.Set("control.owners", []string{"alice"})
	return h
}

func (h *harness) call(peer rpc.Peer, method string, params any, into any) {
	h.t.Helper()
	coretest.MustCall(h.t, h.core, peer, method, params, into)
}

func (h *harness) fails(peer rpc.Peer, method string, params any, code string) {
	h.t.Helper()
	_, err := coretest.Call(h.t, h.core, peer, method, params)
	if rpc.Code(err) != code {
		h.t.Fatalf("%s: got %v, want %s", method, err, code)
	}
}

func (h *harness) delegatedParams(peer rpc.Peer, method string, params any) qtjson.Value {
	h.t.Helper()
	raw, _ := json.Marshal(params)
	handler, ok := h.core.Registry.Resolve(method)
	if !ok {
		h.t.Fatalf("%s not registered", method)
	}
	_, err := handler(context.Background(), &rpc.Request{Peer: peer, ID: 1, Method: method, Params: raw})
	forwarded, delegated := rpc.DelegatedParams(err)
	if !delegated {
		if err == rpc.ErrDelegate {
			value, _ := qtjson.ParseObject(raw)
			return value
		}
		h.t.Fatalf("%s: not delegated: %v", method, err)
	}
	value, _ := qtjson.ParseObject(forwarded)
	return value
}

func (h *harness) credentials(values ...string) {
	h.call(h.ui, "core.agentrun.credentials", map[string]any{"values": values}, nil)
}

func (h *harness) push(context qtjson.Value) {
	h.t.Helper()
	h.call(h.ui, "core.agents.model.push", json.RawMessage(context.Compact()), nil)
}

func (h *harness) create(fields ...any) agents.Session {
	h.t.Helper()
	record := qtjson.ObjectOf(append([]any{"schemaVersion", 2, "provider", "claude-code"}, fields...)...)
	created, err := h.store.Writer().CreateRecord(record)
	if err != nil {
		h.t.Fatal(err)
	}
	session, ok := h.store.Writer().Store().FindSession(created.Get("id").Int())
	if !ok {
		h.t.Fatalf("created session missing")
	}
	return session
}

func (h *harness) save(session agents.Session, mutate func(*qtjson.Value)) {
	h.t.Helper()
	record, ok := h.model.rawRecord(h.store.Writer().Store(), session)
	if !ok {
		h.t.Fatalf("record %d missing", session.ID)
	}
	mutate(&record)
	if _, err := h.store.Writer().SaveRecord(record); err != nil {
		h.t.Fatal(err)
	}
}

func decode(t *testing.T, raw json.RawMessage) qtjson.Value {
	t.Helper()
	value, err := qtjson.Parse(raw)
	if err != nil {
		t.Fatalf("decode %s: %v", raw, err)
	}
	return value
}

func (h *harness) value(peer rpc.Peer, method string, params any) qtjson.Value {
	h.t.Helper()
	raw, err := coretest.Call(h.t, h.core, peer, method, params)
	if err != nil {
		h.t.Fatalf("%s: %v", method, err)
	}
	return decode(h.t, raw)
}

func gitRepo(t *testing.T) string {
	t.Helper()
	dir := filepath.Join(t.TempDir(), "repo")
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	runGit(t, dir, "init", "-q", "-b", "main")
	runGit(t, dir, "config", "user.email", "t@example.com")
	runGit(t, dir, "config", "user.name", "T")
	writeFile(t, filepath.Join(dir, "a.txt"), "one\n")
	runGit(t, dir, "add", ".")
	runGit(t, dir, "commit", "-q", "-m", "first")
	return dir
}

func runGit(t *testing.T, dir string, args ...string) string {
	t.Helper()
	cmd := exec.Command("git", args...)
	cmd.Dir = dir
	cmd.Env = append(os.Environ(), "GIT_AUTHOR_DATE=2026-10-01T10:00:00Z", "GIT_COMMITTER_DATE=2026-10-01T10:00:00Z")
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("git %v: %v\n%s", args, err, out)
	}
	return string(out)
}

func writeFile(t *testing.T, path, text string) {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, []byte(text), 0o644); err != nil {
		t.Fatal(err)
	}
}

func waitFor(t *testing.T, what string, check func() bool) {
	t.Helper()
	deadline := time.Now().Add(5 * time.Second)
	for !check() {
		if time.Now().After(deadline) {
			t.Fatalf("timed out waiting for %s", what)
		}
		time.Sleep(10 * time.Millisecond)
	}
}
