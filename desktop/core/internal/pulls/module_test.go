package pulls

import (
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"reflect"
	"sync"
	"testing"
	"time"

	"forkmesh.com/desktop-core/internal/coretest"
	"forkmesh.com/desktop-core/internal/identity"
	"forkmesh.com/desktop-core/internal/rpc"
)

type moduleHarness struct {
	f        *fixture
	module   *Module
	ui       *coretest.Peer
	cli      *coretest.Peer
	call     func(method string, params map[string]any) map[string]any
	mu       sync.Mutex
	rewrites []map[string]any
	fail     string
}

func newModuleHarness(t *testing.T) *moduleHarness {
	t.Helper()
	f := newFixture(t)
	core := coretest.New(t)
	h := &moduleHarness{f: f}
	id := testIdentity(t)
	h.module = New()
	h.module.LoadIdentity = func() (*identity.Identity, error) { return id, nil }
	h.module.CallUI = func(ctx context.Context, method string, params any) ([]byte, error) {
		h.mu.Lock()
		defer h.mu.Unlock()
		if method != "ui.pulls.rewrite" {
			return nil, rpc.Fail("unknown")
		}
		raw, _ := json.Marshal(params)
		in := map[string]any{}
		_ = json.Unmarshal(raw, &in)
		h.rewrites = append(h.rewrites, in)
		if h.fail != "" {
			return nil, rpc.Fail(h.fail)
		}
		return []byte(`{"ok":true}`), nil
	}
	core.Layout.Data = f.data
	coretest.Start(t, core, h.module)
	h.ui = coretest.NewPeer("ui-1", rpc.UI)
	core.Hub.Add(h.ui)
	h.cli = coretest.NewPeer("worker", rpc.CLI)
	h.call = func(method string, params map[string]any) map[string]any {
		t.Helper()
		if _, ok := params["store"]; !ok {
			params["store"] = h.storeParams(id.PublicKey())
		}
		out := map[string]any{}
		coretest.MustCall(t, core, h.cli, method, params, &out)
		return out
	}
	return h
}

func (h *moduleHarness) storeParams(key string) map[string]any {
	return map[string]any{"workTree": h.f.repo, "mirror": "", "authorName": "Alice", "repoKey": "alice/demo",
		"identityKey": key, "identityValid": true}
}

func TestModuleRefusesPhonesAndForeignIdentities(t *testing.T) {
	h := newModuleHarness(t)
	core := h.module.core
	phone := coretest.NewPeer("phone", rpc.LAN)
	if _, err := coretest.Call(t, core, phone, "core.pulls.load", map[string]any{"store": h.storeParams("")}); rpc.Code(err) != "forbidden" {
		t.Fatalf("phone: %v", err)
	}
	_, err := coretest.Call(t, core, h.cli, "core.pulls.event", map[string]any{"store": h.storeParams("SOMEONEELSE"), "number": 1, "type": "comment", "body": "x"})
	if rpc.Code(err) != "identity_mismatch" {
		t.Fatalf("mismatch: %v", err)
	}
	readOnly := h.storeParams("")
	readOnly["identityValid"] = false
	out := h.call("core.pulls.event", map[string]any{"store": readOnly, "number": 1, "type": "comment", "body": "x"})
	if out["ok"] != false || out["error"] != readOnlyMessage {
		t.Fatalf("read-only %v", out)
	}
}

func TestModuleCreateCommentLoadAndChangedEvents(t *testing.T) {
	h := newModuleHarness(t)
	h.f.branch(t, "feature", "a.txt", "one\nTWO\nthree\n")
	out := h.call("core.pulls.create", map[string]any{"title": "Feature", "description": "Body", "base": "main", "head": "feature", "branchBacked": true})
	if out["ok"] != true || out["number"] != float64(1) {
		t.Fatalf("create %v", out)
	}
	changed := map[string]any{}
	_ = json.Unmarshal(h.ui.Next(t, ChangedEvent, time.Second), &changed)
	if changed["op"] != "create" || !reflect.DeepEqual(changed["numbers"], []any{float64(1)}) {
		t.Fatalf("changed %v", changed)
	}
	out = h.call("core.pulls.event", map[string]any{"number": 1, "type": "review", "state": "approved", "body": "ok"})
	if out["ok"] != true {
		t.Fatalf("review %v", out)
	}
	event := out["event"].(map[string]any)
	ev := EventFromJSON(event)
	if !identity.Verify(ev.Author, ev.Sig, EventCanonical(1, &ev, "alice/demo")) {
		t.Fatal("review signed by the core identity")
	}
	loaded := h.call("core.pulls.load", map[string]any{})
	list := loaded["pulls"].([]any)
	if len(list) != 1 {
		t.Fatalf("load %v", loaded)
	}
	pr := PullFromFullJSON(list[0].(map[string]any))
	if !pr.BranchBacked || len(pr.Events) != 1 || pr.Events[0].State != "approved" || pr.Additions != 1 || pr.PatchHeadOid == "" {
		t.Fatalf("loaded %+v", pr)
	}
	out = h.call("core.pulls.branchEvent", map[string]any{"branch": "feature", "type": "thread-comment", "path": "a.txt", "side": "new", "lineStart": 2, "body": "q"})
	if out["ok"] != true {
		t.Fatalf("branch event %v", out)
	}
	events := h.call("core.pulls.branchEvents", map[string]any{"branch": "feature"})
	if len(events["events"].([]any)) != 1 {
		t.Fatalf("branch events %v", events)
	}
}

func TestModuleDeleteAsksTheDesktopToRewrite(t *testing.T) {
	h := newModuleHarness(t)
	h.f.addPull(t, &Pull{Number: 1, Title: "P", Base: "main", Head: "h", Status: "open", Author: "A", Sig: "S", Patch: "p"})
	h.f.addPull(t, &Pull{Number: 2, Title: "P", Base: "main", Head: "h2", Status: "open", Author: "A", Sig: "S", Patch: "p"})
	out := h.call("core.pulls.delete", map[string]any{"number": 1, "rewriteHistory": true})
	if out["ok"] != true || len(h.rewrites) != 1 {
		t.Fatalf("delete %v %v", out, h.rewrites)
	}
	want := map[string]any{"workTree": h.f.repo, "mirror": "", "number": float64(1), "paths": []any{".forkmesh/pulls/1"}}
	if !reflect.DeepEqual(h.rewrites[0], want) {
		t.Fatalf("rewrite request %v", h.rewrites[0])
	}
	h.fail = "git history rewrite failed: boom"
	out = h.call("core.pulls.setStatus", map[string]any{"number": 2, "status": "merged"})
	if out["ok"] != false || out["error"] != "git history rewrite failed: boom" {
		t.Fatalf("status %v", out)
	}
	out = h.call("core.pulls.delete", map[string]any{"number": 1, "rewriteHistory": false})
	if out["error"] != "Pull request #1 not found." {
		t.Fatalf("missing %v", out)
	}
}

func TestModuleSessionsEndWithTheirDesktop(t *testing.T) {
	h := newModuleHarness(t)
	f := h.f
	f.branch(t, "feature", "a.txt", "one\nFEATURE\nthree\n")
	mbox := sh(t, f.repo, "format-patch", "--stdout", "main..feature")
	f.advanceMain(t, "a.txt", "one\nMAIN\nthree\n")
	f.addPull(t, &Pull{Number: 1, Title: "C", Base: "main", Head: "node:feature", Status: "open", Author: "A", Sig: "S", Patch: "x", Commits: mbox + "\n"})
	out := h.call("core.pulls.session.start", map[string]any{"number": 1, "mode": "conflict"})
	if out["ok"] != true || !reflect.DeepEqual(out["conflicted"], []any{"a.txt"}) {
		t.Fatalf("start %v", out)
	}
	state := h.call("core.pulls.session.state", map[string]any{})
	if state["inProgress"] != true || state["active"] != true {
		t.Fatalf("state %v", state)
	}
	again := h.call("core.pulls.session.start", map[string]any{"number": 1, "mode": "conflict"})
	if again["error"] != anotherOperationMessage {
		t.Fatalf("second start %v", again)
	}
	h.module.dropSessions("")
	if SessionLive(f.repo) {
		t.Fatal("sessions end with the desktop that owned them")
	}
	state = h.call("core.pulls.session.state", map[string]any{})
	if state["active"] != false {
		t.Fatalf("state after detach %v", state)
	}
	if err := (&Store{WorkTree: f.repo, IdentityValid: true, DataRoot: f.data}).MergePull(1, false); err == nil {
		t.Fatal("the stale am state is healed, then the conflicting merge fails on its own")
	}
	if dir := (&Store{WorkTree: f.repo}).amStateDir(f.repo); dir != "" {
		t.Fatalf("a merge after the desktop left heals the stale am state: %s", dir)
	}
}

func TestModuleLoadAnswersUnchangedForASettledStamp(t *testing.T) {
	h := newModuleHarness(t)
	h.f.addPull(t, &Pull{Number: 1, Title: "P", Base: "main", Head: "h", Status: "open", Author: "A", Sig: "S", Patch: "p"})
	h.call("core.pulls.load", map[string]any{})
	past := time.Now().Add(-time.Minute)
	_ = filepath.Walk(filepath.Dir(h.f.repo), func(path string, info os.FileInfo, err error) error {
		if err == nil {
			_ = os.Chtimes(path, past, past)
		}
		return nil
	})
	first := h.call("core.pulls.load", map[string]any{})
	stamp, _ := first["stamp"].(string)
	if stamp == "" || len(first["pulls"].([]any)) != 1 {
		t.Fatalf("first %v", first)
	}
	second := h.call("core.pulls.load", map[string]any{"stamp": stamp})
	if second["unchanged"] != true || second["pulls"] != nil {
		t.Fatalf("second %v", second)
	}
	third := h.call("core.pulls.load", map[string]any{"stamp": "stale"})
	if third["unchanged"] == true || len(third["pulls"].([]any)) != 1 {
		t.Fatalf("third %v", third)
	}
}
