package mergequeue

import (
	"context"
	"encoding/json"
	"os"
	"os/exec"
	"path/filepath"
	"reflect"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"

	"forkmesh.com/desktop-core/internal/coretest"
	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/pulls"
	"forkmesh.com/desktop-core/internal/rpc"
)

type fakeDesktop struct {
	mu      sync.Mutex
	view    View
	gone    bool
	applied []map[string]any
	purged  []int
}

func (d *fakeDesktop) record(owner, name string) *Record {
	for i := range d.view.Repos {
		if d.view.Repos[i].Owner == owner && d.view.Repos[i].Name == name {
			return &d.view.Repos[i]
		}
	}
	return nil
}

func (d *fakeDesktop) rows(owner, name string) []string {
	d.mu.Lock()
	defer d.mu.Unlock()
	if r := d.record(owner, name); r != nil {
		return append([]string(nil), r.Rows...)
	}
	return nil
}

func (d *fakeDesktop) call(ctx context.Context, method string, params any) ([]byte, error) {
	d.mu.Lock()
	defer d.mu.Unlock()
	if d.gone {
		return nil, rpc.Fail("desktop_unavailable")
	}
	raw, _ := json.Marshal(params)
	in := map[string]any{}
	_ = json.Unmarshal(raw, &in)
	switch method {
	case "ui.mergequeue.view":
		return json.Marshal(d.view)
	case "ui.mergequeue.apply":
		d.applied = append(d.applied, in)
		owner, _ := in["owner"].(string)
		name, _ := in["name"].(string)
		r := d.record(owner, name)
		if r == nil {
			return nil, rpc.Fail("unknown_repo")
		}
		if v, ok := in["enabled"].(bool); ok {
			r.Enabled = v
		}
		if v, ok := in["paused"].(bool); ok {
			r.Paused = v
		}
		if rows, ok := in["rows"].([]any); ok {
			expect := []string{}
			for _, e := range in["expect"].([]any) {
				expect = append(expect, e.(string))
			}
			current := r.Rows
			if current == nil {
				current = []string{}
			}
			if !reflect.DeepEqual(expect, current) {
				return nil, rpc.Fail("conflict")
			}
			r.Rows = []string{}
			for _, row := range rows {
				r.Rows = append(r.Rows, row.(string))
			}
		}
		return []byte(`{"ok":true}`), nil
	case "ui.mergequeue.purge":
		d.purged = append(d.purged, int(in["number"].(float64)))
		return []byte(`{}`), nil
	}
	return nil, rpc.Fail("unknown_method")
}

func gitEnv(t *testing.T) {
	t.Helper()
	home := t.TempDir()
	t.Setenv("HOME", home)
	t.Setenv("XDG_CONFIG_HOME", filepath.Join(home, ".config"))
	t.Setenv("XDG_DATA_HOME", filepath.Join(home, ".local/share"))
	t.Setenv("XDG_CACHE_HOME", filepath.Join(home, ".cache"))
	t.Setenv("GIT_CONFIG_NOSYSTEM", "1")
	t.Setenv("GIT_AUTHOR_NAME", "Tester")
	t.Setenv("GIT_AUTHOR_EMAIL", "tester@example.com")
	t.Setenv("GIT_COMMITTER_NAME", "Tester")
	t.Setenv("GIT_COMMITTER_EMAIL", "tester@example.com")
}

func sh(t *testing.T, dir string, args ...string) string {
	t.Helper()
	out, err := exec.Command("git", append([]string{"-C", dir}, args...)...).CombinedOutput()
	if err != nil {
		t.Fatalf("git %v: %v\n%s", args, err, out)
	}
	return strings.TrimSpace(string(out))
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

type harness struct {
	core    *daemon.Core
	module  *Module
	desktop *fakeDesktop
	phone   *coretest.Peer
	ui      *coretest.Peer
	repo    string
	data    string
}

func newRepo(t *testing.T) string {
	t.Helper()
	repo := filepath.Join(t.TempDir(), "repo")
	if err := os.MkdirAll(repo, 0o755); err != nil {
		t.Fatal(err)
	}
	sh(t, repo, "init", "-q", "-b", "main")
	writeFile(t, filepath.Join(repo, "a.txt"), "one\ntwo\nthree\n")
	sh(t, repo, "add", "-A")
	sh(t, repo, "commit", "-q", "-m", "base")
	return repo
}

func setup(t *testing.T, records ...Record) *harness {
	t.Helper()
	gitEnv(t)
	core := coretest.New(t)
	core.Set("control.owners", []any{"alice"})
	repo := newRepo(t)
	for i := range records {
		if records[i].LocalPath == "" {
			records[i].LocalPath = repo
		}
		records[i].ActiveRelay = true
	}
	desktop := &fakeDesktop{view: View{Identity: true, Open: -1, Repos: records}}
	module := New()
	module.CallUI = desktop.call
	coretest.Start(t, core, module)
	phone := coretest.NewPeer("phone-1", rpc.LAN)
	phone.Dev.User = "Alice"
	core.Hub.Add(phone)
	ui := coretest.NewPeer("desktop", rpc.UI)
	core.Hub.Add(ui)
	return &harness{core: core, module: module, desktop: desktop, phone: phone, ui: ui, repo: repo, data: core.Layout.Data}
}

func (h *harness) configure(t *testing.T) {
	t.Helper()
	h.desktop.mu.Lock()
	view := h.desktop.view
	h.desktop.mu.Unlock()
	if _, err := coretest.Call(t, h.core, h.ui, "core.mergequeue.view", view); err != nil {
		t.Fatal(err)
	}
}

func (h *harness) setOpen(index int) {
	h.desktop.mu.Lock()
	h.desktop.view.Open = index
	h.desktop.mu.Unlock()
}

func (h *harness) call(t *testing.T, method string, params any) (map[string]any, string) {
	t.Helper()
	raw, err := coretest.Call(t, h.core, h.phone, method, params)
	if err != nil {
		return nil, rpc.Code(err)
	}
	out := map[string]any{}
	if err := json.Unmarshal(raw, &out); err != nil {
		t.Fatalf("%s: %v", method, err)
	}
	return out, ""
}

func (h *harness) run(t *testing.T, params map[string]any) (map[string]any, string) {
	t.Helper()
	prepared, code := h.call(t, "mergequeue.prepare", params)
	if code != "" {
		return nil, code
	}
	withToken := map[string]any{"token": prepared["token"]}
	for k, v := range params {
		withToken[k] = v
	}
	return h.call(t, "mergequeue.run", withToken)
}

func (h *harness) store() *pulls.Store {
	return &pulls.Store{WorkTree: h.repo, IdentityValid: true, DataRoot: h.data}
}

func (h *harness) branch(t *testing.T, name, file, text string) {
	t.Helper()
	sh(t, h.repo, "checkout", "-q", "-b", name, "main")
	writeFile(t, filepath.Join(h.repo, file), text)
	sh(t, h.repo, "commit", "-q", "-am", "change on "+name)
	sh(t, h.repo, "checkout", "-q", "main")
}

func (h *harness) addPull(t *testing.T, number int, title, head, status string) {
	t.Helper()
	meta := h.store().MetaWorkTree()
	if meta == "" {
		t.Fatal("no ledger checkout")
	}
	text := "---\nschema: forkmesh-pull-v1\nnumber: " + strconv.Itoa(number) + "\ntitle: " + title +
		"\nbase: main\nhead: " + head + "\nstatus: " + status + "\nderive: branch\nts: 1700000000000\nauthor: AUTHOR\nauthorName: Bob\nsig: SIG\n---\n\nBody\n"
	writeFile(t, filepath.Join(meta, ".forkmesh/pulls", strconv.Itoa(number), "pull.md"), text)
	sh(t, meta, "add", "-A")
	sh(t, meta, "commit", "-q", "-m", "pull #"+strconv.Itoa(number)+": open")
}

func TestDelegatesUntilTheDesktopConfiguresIt(t *testing.T) {
	h := setup(t, Record{Owner: "alice", Name: "demo", Enabled: true})
	if _, code := h.call(t, "mergequeue.list", map[string]any{}); code != "unsupported" {
		t.Fatalf("list before configure: %q", code)
	}
	h.configure(t)
	if _, code := h.call(t, "mergequeue.list", map[string]any{}); code != "" {
		t.Fatalf("list after configure: %q", code)
	}
}

func TestPhoneOfAnotherUserIsRefused(t *testing.T) {
	h := setup(t, Record{Owner: "alice", Name: "demo", Enabled: true})
	h.configure(t)
	h.phone.Dev.User = "mallory"
	if _, code := h.call(t, "mergequeue.list", map[string]any{}); code != "not_owner" {
		t.Fatalf("got %q", code)
	}
}

func TestDesktopMethodsRefusePhones(t *testing.T) {
	h := setup(t)
	if _, code := h.call(t, "core.mergequeue.schedule", map[string]any{"delayMs": 0}); code != "forbidden" {
		t.Fatalf("got %q", code)
	}
}

func TestListMatchesTheDesktopRows(t *testing.T) {
	h := setup(t,
		Record{Owner: "alice", Name: "demo", Enabled: true, Rows: []string{"3\t100\tqueued\t", "7\t200\tblocked\tWaiting for an independent peer approval"}},
		Record{Owner: "alice", Name: "quiet"},
		Record{Owner: "alice", Name: "leftover", Rows: []string{"9\t5\tmerging\tMerging"}},
	)
	h.branch(t, "feature", "a.txt", "one\nTWO\nthree\n")
	h.addPull(t, 3, "Fix the thing", "feature", "open")
	h.configure(t)
	h.setOpen(0)
	got, code := h.call(t, "mergequeue.list", map[string]any{})
	if code != "" {
		t.Fatal(code)
	}
	raw, _ := json.Marshal(got)
	want := `{"repos":[` +
		`{"enabled":true,"entries":[{"detail":"","number":3,"queuedAtMs":100,"state":"queued","stateAtMs":100,"title":"Fix the thing"},` +
		`{"detail":"Waiting for an independent peer approval","number":7,"queuedAtMs":200,"state":"blocked","stateAtMs":200,"title":""}],` +
		`"name":"demo","open":true,"owner":"alice","paused":false,"summary":"2 queued · 1 blocked","writable":true},` +
		`{"enabled":false,"entries":[{"detail":"Merging","number":9,"queuedAtMs":5,"state":"queued","stateAtMs":5,"title":""}],` +
		`"name":"leftover","open":false,"owner":"alice","paused":false,"summary":"Off","writable":true}]}`
	if string(raw) != want {
		t.Fatalf("list\n got %s\nwant %s", raw, want)
	}
	one, _ := h.call(t, "mergequeue.list", map[string]any{"owner": "ALICE", "name": " quiet "})
	if repos := one["repos"].([]any); len(repos) != 1 || repos[0].(map[string]any)["summary"] != "Off" {
		t.Fatalf("filtered list: %v", one)
	}
}

func TestEnqueueMoveRemoveClear(t *testing.T) {
	h := setup(t, Record{Owner: "alice", Name: "demo", Enabled: true})
	h.configure(t)
	now := time.UnixMilli(1700000000000)
	h.module.Now = func() time.Time { return now }
	prepared, code := h.call(t, "mergequeue.prepare", map[string]any{"owner": "alice", "name": "demo", "action": "enqueue", "number": 4})
	if code != "" {
		t.Fatal(code)
	}
	if prepared["action"] != "enqueue" || prepared["expiresInMs"] != float64(60000) ||
		prepared["summary"] != "Add pull request #4 to the merge queue of alice/demo. The desktop merges it when its turn comes and the review and checks allow it." {
		t.Fatalf("prepare: %v", prepared)
	}
	result, code := h.call(t, "mergequeue.run", map[string]any{"owner": "alice", "name": "demo", "action": "enqueue", "number": 4, "token": prepared["token"]})
	if code != "" {
		t.Fatal(code)
	}
	if result["message"] != "Pull request #4 joined the merge queue at place 1." {
		t.Fatalf("run: %v", result)
	}
	if rows := h.desktop.rows("alice", "demo"); !reflect.DeepEqual(rows, []string{"4\t1700000000000\tqueued\t"}) {
		t.Fatalf("rows: %q", rows)
	}
	changed := h.phone.Next(t, "mergequeue.changed", 2*time.Second)
	if string(changed) != `{"name":"demo","owner":"alice"}` {
		t.Fatalf("changed: %s", changed)
	}
	if _, code := h.run(t, map[string]any{"owner": "alice", "name": "demo", "action": "enqueue", "number": 4}); code != "already_queued" {
		t.Fatalf("second enqueue: %q", code)
	}
	if _, code := h.run(t, map[string]any{"owner": "alice", "name": "demo", "action": "enqueue", "number": 5}); code != "" {
		t.Fatal(code)
	}
	moved, code := h.run(t, map[string]any{"owner": "alice", "name": "demo", "action": "move", "number": 5, "delta": -1})
	if code != "" || moved["message"] != "Pull request #5 is now at place 1." {
		t.Fatalf("move: %v %q", moved, code)
	}
	if _, code := h.run(t, map[string]any{"owner": "alice", "name": "demo", "action": "remove", "number": 8}); code != "not_queued" {
		t.Fatalf("remove missing: %q", code)
	}
	removed, code := h.run(t, map[string]any{"owner": "alice", "name": "demo", "action": "remove", "number": 4})
	if code != "" || removed["message"] != "Pull request #4 left the merge queue." {
		t.Fatalf("remove: %v %q", removed, code)
	}
	cleared, code := h.run(t, map[string]any{"owner": "alice", "name": "demo", "action": "clear"})
	if code != "" || cleared["message"] != "Merge queue cleared." {
		t.Fatalf("clear: %v %q", cleared, code)
	}
	if rows := h.desktop.rows("alice", "demo"); len(rows) != 0 {
		t.Fatalf("rows after clear: %q", rows)
	}
}

func TestRunErrors(t *testing.T) {
	h := setup(t,
		Record{Owner: "alice", Name: "demo", Enabled: true, Paused: true},
		Record{Owner: "alice", Name: "off"},
		Record{Owner: "alice", Name: "nocheckout", Enabled: true, LocalPath: "/nonexistent/checkout"},
	)
	h.branch(t, "feature", "a.txt", "one\nTWO\nthree\n")
	h.addPull(t, 2, "Done already", "feature", "merged")
	h.configure(t)
	cases := []struct {
		params map[string]any
		want   string
	}{
		{map[string]any{"owner": "alice", "name": "demo", "action": "explode"}, "bad_request"},
		{map[string]any{"owner": "alice", "name": "demo", "action": "enqueue", "number": 0}, "bad_request"},
		{map[string]any{"owner": "alice", "name": "demo", "action": "move", "number": 1, "delta": 0}, "bad_request"},
		{map[string]any{"owner": "../x", "name": "demo", "action": "clear"}, "bad_request"},
		{map[string]any{"owner": "alice", "name": "missing", "action": "clear"}, "unknown_repo"},
		{map[string]any{"owner": "alice", "name": "nocheckout", "action": "clear"}, "read_only"},
		{map[string]any{"owner": "alice", "name": "off", "action": "clear"}, "queue_off"},
		{map[string]any{"owner": "alice", "name": "demo", "action": "process"}, "paused"},
	}
	for _, c := range cases {
		if _, code := h.run(t, c.params); code != c.want {
			t.Fatalf("%v: got %q want %q", c.params, code, c.want)
		}
	}
	if _, code := h.call(t, "mergequeue.run", map[string]any{"owner": "alice", "name": "demo", "action": "clear", "token": "nope"}); code != "confirmation_required" {
		t.Fatalf("bad token: %q", code)
	}
	h.setOpen(0)
	if _, code := h.run(t, map[string]any{"owner": "alice", "name": "demo", "action": "enqueue", "number": 2}); code != "not_open_pull" {
		t.Fatalf("merged pull: %q", code)
	}
	h.setOpen(-1)
	if _, code := h.run(t, map[string]any{"owner": "alice", "name": "demo", "action": "resume"}); code != "" {
		t.Fatal(code)
	}
	if _, code := h.run(t, map[string]any{"owner": "alice", "name": "demo", "action": "process"}); code != "repo_not_open" {
		t.Fatalf("process closed repo: %q", code)
	}
	enabled, code := h.run(t, map[string]any{"owner": "alice", "name": "off", "action": "enable"})
	if code != "" || enabled["message"] != "Merge queue turned on." {
		t.Fatalf("enable: %v %q", enabled, code)
	}
}

func TestWritesWithoutADesktopAreRefused(t *testing.T) {
	h := setup(t, Record{Owner: "alice", Name: "demo", Enabled: true})
	h.configure(t)
	prepared, _ := h.call(t, "mergequeue.prepare", map[string]any{"owner": "alice", "name": "demo", "action": "enqueue", "number": 3})
	h.desktop.mu.Lock()
	h.desktop.gone = true
	h.desktop.mu.Unlock()
	if _, code := h.call(t, "mergequeue.run", map[string]any{"owner": "alice", "name": "demo", "action": "enqueue", "number": 3, "token": prepared["token"]}); code != "desktop_unavailable" {
		t.Fatalf("got %q", code)
	}
	list, code := h.call(t, "mergequeue.list", map[string]any{})
	if code != "" || len(list["repos"].([]any)) != 1 || list["repos"].([]any)[0].(map[string]any)["open"] != false {
		t.Fatalf("list without desktop: %v %q", list, code)
	}
}

func waitFor(t *testing.T, what string, check func() bool) {
	t.Helper()
	deadline := time.Now().Add(20 * time.Second)
	for !check() {
		if time.Now().After(deadline) {
			t.Fatalf("timed out waiting for %s", what)
		}
		time.Sleep(20 * time.Millisecond)
	}
}

func TestExecutorMergesTheFrontOfTheQueue(t *testing.T) {
	h := setup(t, Record{Owner: "alice", Name: "demo", Enabled: true, Rows: []string{"1\t10\tqueued\t"}})
	h.branch(t, "feature", "a.txt", "one\nTWO\nthree\n")
	h.addPull(t, 1, "Shout two", "feature", "open")
	h.configure(t)
	h.setOpen(0)
	if _, err := coretest.Call(t, h.core, h.ui, "core.mergequeue.schedule", map[string]any{"delayMs": 0}); err != nil {
		t.Fatal(err)
	}
	merged := h.ui.Next(t, "core.mergequeue.merged", 20*time.Second)
	var event struct {
		Owner  string         `json:"owner"`
		Name   string         `json:"name"`
		Number int            `json:"number"`
		Pull   map[string]any `json:"pull"`
	}
	if err := json.Unmarshal(merged, &event); err != nil {
		t.Fatal(err)
	}
	if event.Owner != "alice" || event.Name != "demo" || event.Number != 1 || event.Pull["title"] != "Shout two" {
		t.Fatalf("merged event: %s", merged)
	}
	if rows := h.desktop.rows("alice", "demo"); len(rows) != 0 {
		t.Fatalf("queue after merge: %q", rows)
	}
	pr, ok := h.store().ReadPull(1, false)
	if !ok || pr.Status != "merged" {
		t.Fatalf("pull after merge: %+v", pr)
	}
	if got := sh(t, h.repo, "show", "main:a.txt"); got != "one\nTWO\nthree" {
		t.Fatalf("main after merge: %q", got)
	}
	h.desktop.mu.Lock()
	purged := append([]int(nil), h.desktop.purged...)
	h.desktop.mu.Unlock()
	if !reflect.DeepEqual(purged, []int{1}) {
		t.Fatalf("purge requests: %v", purged)
	}
	waitFor(t, "the pass to end", func() bool { return !h.module.Busy() })
}

func TestExecutorBlocksOnReviewAndConflicts(t *testing.T) {
	h := setup(t,
		Record{Owner: "alice", Name: "demo", Enabled: true, RequirePeerApproval: true, Rows: []string{"1\t10\tqueued\t"}},
	)
	h.branch(t, "feature", "a.txt", "one\nTWO\nthree\n")
	h.addPull(t, 1, "Needs review", "feature", "open")
	h.configure(t)
	h.setOpen(0)
	h.module.Schedule(0)
	state := h.ui.Next(t, "core.mergequeue.state", 20*time.Second)
	if string(state) != `{"name":"demo","owner":"alice","pending":true}` {
		t.Fatalf("state event: %s", state)
	}
	rows := h.desktop.rows("alice", "demo")
	if len(rows) != 1 || !strings.HasPrefix(rows[0], "1\t10\tblocked\tWaiting for an independent peer approval") {
		t.Fatalf("blocked row: %q", rows)
	}
	h.module.Stop()

	c := setup(t, Record{Owner: "alice", Name: "demo", Enabled: true, Rows: []string{"1\t10\tqueued\t"}})
	c.branch(t, "feature", "a.txt", "one\nFEATURE\nthree\n")
	writeFile(t, filepath.Join(c.repo, "a.txt"), "one\nMAIN\nthree\n")
	sh(t, c.repo, "commit", "-q", "-am", "main moves")
	c.addPull(t, 1, "Clashes", "feature", "open")
	c.configure(t)
	c.setOpen(0)
	c.module.Schedule(0)
	c.ui.Next(t, "core.mergequeue.state", 20*time.Second)
	rows = c.desktop.rows("alice", "demo")
	if len(rows) != 1 || !strings.HasPrefix(rows[0], "1\t10\tfailed\tCould not update from main: ") {
		t.Fatalf("conflict row: %q", rows)
	}
	c.module.Stop()
}

func TestExecutorDropsMissingAndDecidedPulls(t *testing.T) {
	h := setup(t, Record{Owner: "alice", Name: "demo", Enabled: true, Rows: []string{"5\t10\tqueued\t", "2\t11\tqueued\t"}})
	h.branch(t, "feature", "a.txt", "one\nTWO\nthree\n")
	h.addPull(t, 2, "Closed one", "feature", "closed")
	h.configure(t)
	h.setOpen(0)
	h.module.Schedule(0)
	h.ui.Next(t, "core.mergequeue.state", 20*time.Second)
	if rows := h.desktop.rows("alice", "demo"); len(rows) != 0 {
		t.Fatalf("rows: %q", rows)
	}
}

func TestExecutorWaitsWhileTheDesktopIsBusy(t *testing.T) {
	h := setup(t, Record{Owner: "alice", Name: "demo", Enabled: true, Rows: []string{"1\t10\tqueued\t"}})
	h.branch(t, "feature", "a.txt", "one\nTWO\nthree\n")
	h.addPull(t, 1, "Later", "feature", "open")
	h.desktop.view.AiFix = true
	h.configure(t)
	h.setOpen(0)
	h.module.process(context.Background())
	var state map[string]any
	raw, _ := coretest.Call(t, h.core, h.ui, "core.mergequeue.state", map[string]any{})
	_ = json.Unmarshal(raw, &state)
	if state["busy"] != false || state["pendingMs"].(float64) < 15000 {
		t.Fatalf("expected a 20 s retry: %v", state)
	}
	if rows := h.desktop.rows("alice", "demo"); !reflect.DeepEqual(rows, []string{"1\t10\tqueued\t"}) {
		t.Fatalf("rows changed: %q", rows)
	}
}
