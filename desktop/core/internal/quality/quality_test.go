package quality

import (
	"context"
	"encoding/json"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	"forkmesh.com/desktop-core/internal/actions"
	"forkmesh.com/desktop-core/internal/coretest"
	"forkmesh.com/desktop-core/internal/rpc"
)

func runGit(t *testing.T, dir string, args ...string) {
	t.Helper()
	cmd := exec.Command("git", append([]string{"-C", dir, "-c", "user.name=Test", "-c", "user.email=test@example.com", "-c", "commit.gpgsign=false", "-c", "init.defaultBranch=main"}, args...)...)
	if out, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("git %v: %v\n%s", args, err, out)
	}
}

func write(t *testing.T, path, text string) {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(path), 0o700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, []byte(text), 0o600); err != nil {
		t.Fatal(err)
	}
}

func fixtureRepo(t *testing.T) (string, string) {
	t.Helper()
	if _, err := exec.LookPath("git"); err != nil {
		t.Skip("git not installed")
	}
	root := t.TempDir()
	local := filepath.Join(root, "checkout")
	write(t, filepath.Join(local, "README.md"), "# Demo\n")
	write(t, filepath.Join(local, "big.cpp"), strings.Repeat("x\n", 1300))
	write(t, filepath.Join(local, "huge.go"), strings.Repeat("y\n", 3001))
	write(t, filepath.Join(local, "notes.txt"), "TODO: a\nFIXME b\nXXXL not a marker\n  HACK\n\n")
	write(t, filepath.Join(local, "logo.bin"), "TODO\x00binary")
	write(t, filepath.Join(local, "src/app.py"), "print(1)  # TODO later")
	write(t, filepath.Join(local, "tests/test_app.py"), "def test_app():\n    pass\n")
	write(t, filepath.Join(local, ".forkmesh/issues/open/1/issue-1.json"), `{"number":1,"status":"closed"}`)
	write(t, filepath.Join(local, ".forkmesh/issues/closed/2/issue-2.json"), `{"number":2,"status":"open"}`)
	write(t, filepath.Join(local, ".forkmesh/issues/3/issue-3.json"), `{"number":3,"status":"open","events":[{"type":"open"},{"type":"status","status":"closed"}]}`)
	write(t, filepath.Join(local, ".forkmesh/issues/open/4/note.txt"), "no record")
	runGit(t, root, "init", "-q", local)
	runGit(t, local, "add", "-A")
	runGit(t, local, "commit", "-q", "-m", "init")
	mirror := filepath.Join(root, "mirror.git")
	runGit(t, root, "clone", "-q", "--bare", local, mirror)
	return local, mirror
}

func fixtureRuns() []actions.Run {
	failed := actions.NewRun()
	failed.ID, failed.Owner, failed.Name = 2, "alice", "demo"
	failed.WorkflowName, failed.WorkflowPath, failed.Status, failed.CreatedAtMs = "CI", ".forkmesh/ci.yml", actions.StatusFailed, 2000
	passed := actions.NewRun()
	passed.ID, passed.Owner, passed.Name = 1, "alice", "demo"
	passed.WorkflowName, passed.WorkflowPath, passed.Status, passed.CreatedAtMs = "CI", ".forkmesh/ci.yml", actions.StatusSuccess, 1000
	return []actions.Run{failed, passed}
}

const fixtureReport = `{"computedAt":1790978400000,"files":[` +
	`{"findings":["High: CI"],"path":".forkmesh/ci.yml","score":80},` +
	`{"findings":["Warning: huge.go"],"path":"huge.go","score":92},` +
	`{"findings":["Info: Missing LICENSE","Info: Missing CONTRIBUTING","Info: Missing CHANGELOG"],"path":"(repository)","score":94},` +
	`{"findings":["Info: TODO","Info: FIXME","Info: HACK"],"path":"notes.txt","score":94},` +
	`{"findings":["Info: big.cpp"],"path":"big.cpp","score":98},` +
	`{"findings":["Info: TODO"],"path":"src/app.py","score":98}],` +
	`"findingCount":10,"grade":"F","score":56}`

func TestScanCheckoutAndMirrorGolden(t *testing.T) {
	local, mirror := fixtureRepo(t)
	ctx := context.Background()
	git := gitRunner{}
	statuses := issueReader{git: git, workTree: local, mirror: mirror, identity: true}.statuses(ctx)
	if strings.Join(statuses, ",") != "open,open,closed,closed" {
		t.Fatalf("local issues = %v", statuses)
	}
	mirrorStatuses := issueReader{git: git, workTree: local, mirror: mirror, identity: false}.statuses(ctx)
	if strings.Join(mirrorStatuses, ",") != "open,closed,closed" {
		t.Fatalf("mirror issues = %v", mirrorStatuses)
	}
	for _, in := range []Input{
		{Owner: "alice", Name: "demo", LocalPath: local, MirrorPath: mirror, Issues: statuses, Runs: fixtureRuns()},
		{Owner: "alice", Name: "demo", MirrorPath: mirror, Issues: mirrorStatuses, Runs: fixtureRuns()},
	} {
		result := scanner{git: git}.Scan(ctx, in)
		got, _ := json.Marshal(ReportFor(result, scanRoot(in), 1790978400000))
		if string(got) != fixtureReport {
			t.Fatalf("report for %+v\n got %s\nwant %s", in.LocalPath, got, fixtureReport)
		}
	}
}

func TestScanEmptyRepository(t *testing.T) {
	result := scanner{}.Scan(context.Background(), Input{LocalPath: filepath.Join(t.TempDir(), "missing")})
	got, _ := json.Marshal(ReportFor(result, "", 5))
	want := `{"computedAt":5,"files":[{"findings":["Warning: Missing README","Info: Missing LICENSE","Info: Missing CONTRIBUTING","Info: Missing CHANGELOG"],"path":"(repository)","score":86}],"findingCount":4,"grade":"B","score":82}`
	if string(got) != want {
		t.Fatalf("empty\n got %s\nwant %s", got, want)
	}
}

func TestRelativeFindingPath(t *testing.T) {
	cases := []struct{ path, root, want string }{
		{"", "/r", RepositoryLabel},
		{"  ", "/r", RepositoryLabel},
		{"src/a.go", "/r", "src/a.go"},
		{"./src//a.go", "/r", "src/a.go"},
		{"/r/src/a.go", "/r", "src/a.go"},
		{"/r/src/a.go", "/r/", "src/a.go"},
		{"/elsewhere/a.go", "/r", "a.go"},
		{"/r/src/a.go", "", "a.go"},
		{"C:\\work\\x.yml", "/r", "x.yml"},
		{"../../etc/passwd", "/r", "passwd"},
		{"..", "/r", ".."},
		{".", "/r", RepositoryLabel},
		{strings.Repeat("a", 400), "/r", strings.Repeat("a", 300)},
	}
	for _, c := range cases {
		if got := RelativeFindingPath(c.path, c.root); got != c.want {
			t.Errorf("RelativeFindingPath(%q, %q) = %q, want %q", c.path, c.root, got, c.want)
		}
	}
}

func TestFlatTextAndReportLimits(t *testing.T) {
	if got := flatText("  Info:\t two\n\u200blines \x01 ", 200); got != "Info: two lines" {
		t.Fatalf("flat = %q", got)
	}
	if got := flatText(strings.Repeat("b", 199)+"\U0001F600x", 200); got != strings.Repeat("b", 199) {
		t.Fatalf("surrogate cut = %q", got)
	}
	var findings []Finding
	for i := 0; i < 12; i++ {
		findings = append(findings, Finding{Path: "a", Severity: SeverityInfo, Title: "i"})
	}
	findings = append(findings, Finding{Path: "a", Severity: SeverityCritical, Title: "c"}, Finding{Path: "z", Severity: SeverityPass, Title: "p"})
	report := Report(findings, []int{SeverityCritical, SeverityCritical, SeverityCritical, SeverityCritical, SeverityCritical}, 1)
	files := report["files"].([]any)
	first := files[0].(map[string]any)
	notes := first["findings"].([]any)
	if len(files) != 1 || len(notes) != 10 || notes[0] != "Critical: c" || first["score"] != 41 || report["score"] != 0 || report["findingCount"] != 14 {
		t.Fatalf("report = %v", report)
	}
}

type fakeFacts struct {
	mu      sync.Mutex
	facts   Facts
	err     error
	release chan struct{}
	calls   int
}

func (f *fakeFacts) locate(ctx context.Context, req *rpc.Request, repo string) (Facts, error) {
	f.mu.Lock()
	f.calls++
	facts, err, release := f.facts, f.err, f.release
	f.mu.Unlock()
	if release != nil {
		<-release
	}
	return facts, err
}

func TestModuleQuality(t *testing.T) {
	local, mirror := fixtureRepo(t)
	core := coretest.New(t)
	store := actions.NewStore(filepath.Join(core.Layout.Data, "actions"))
	for _, run := range fixtureRuns() {
		if err := store.SaveRun(run); err != nil {
			t.Fatal(err)
		}
	}
	other := actions.NewRun()
	other.ID, other.Owner, other.Name, other.Status, other.WorkflowPath = 3, "Alice", "demo", actions.StatusFailed, ".forkmesh/x.yml"
	if err := store.SaveRun(other); err != nil {
		t.Fatal(err)
	}
	empty := ""
	facts := &fakeFacts{facts: Facts{Owner: "alice", Name: "demo", LocalPath: "/nowhere", MirrorPath: "/nowhere.git", WritableLocalPath: &local, WritableMirrorPath: &empty}}
	module := New()
	module.Owners = func() []string { return []string{"alice"} }
	module.Now = func() time.Time { return time.UnixMilli(1790978400000) }
	module.Facts = facts.locate
	module.IdentityValid = func() bool { return true }
	coretest.Start(t, core, module)

	owner := coretest.NewPeer("phone", rpc.LAN)
	owner.Dev.User = "alice"
	stranger := coretest.NewPeer("other", rpc.Relay)
	stranger.Dev.User = "mallory"

	if _, err := coretest.Call(t, core, stranger, "stats.quality", map[string]any{"repo": "alice/demo"}); rpc.Code(err) != "not_owner" {
		t.Fatalf("stranger = %v", err)
	}
	for _, repo := range []any{"alice", "alice/..", 5, ""} {
		if _, err := coretest.Call(t, core, owner, "stats.quality", map[string]any{"repo": repo}); rpc.Code(err) != "bad_request" {
			t.Fatalf("repo %v = %v", repo, err)
		}
	}
	facts.err = rpc.Fail("unknown_repo")
	if _, err := coretest.Call(t, core, owner, "stats.quality", map[string]any{"repo": " alice/demo "}); rpc.Code(err) != "unknown_repo" {
		t.Fatalf("unknown = %v", err)
	}
	facts.err = nil
	raw, err := coretest.Call(t, core, owner, "stats.quality", map[string]any{"repo": "alice/demo"})
	if err != nil {
		t.Fatal(err)
	}
	if string(raw) != fixtureReport {
		t.Fatalf("module report\n got %s\nwant %s", raw, fixtureReport)
	}

	module.Facts = func(ctx context.Context, req *rpc.Request, repo string) (Facts, error) {
		return Facts{Owner: "alice", Name: "demo", LocalPath: "", MirrorPath: mirror}, nil
	}
	if raw, err := coretest.Call(t, core, owner, "stats.quality", map[string]any{"repo": "alice/demo"}); err != nil || string(raw) != fixtureReport {
		t.Fatalf("mirror-only module report = %s %v", raw, err)
	}

	facts.release = make(chan struct{})
	module.Facts = facts.locate
	if !module.begin("alice/demo") {
		t.Fatal("begin")
	}
	close(facts.release)
	if _, err := coretest.Call(t, core, owner, "stats.quality", map[string]any{"repo": "alice/demo"}); rpc.Code(err) != "busy" {
		t.Fatalf("busy = %v", err)
	}
	if !module.Busy() {
		t.Fatal("module is busy while a scan runs")
	}
	module.end("alice/demo")
	if module.Busy() {
		t.Fatal("still busy")
	}
}
