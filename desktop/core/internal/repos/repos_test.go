package repos

import (
	"context"
	"encoding/json"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
	"time"

	"forkmesh.com/desktop-core/internal/coretest"
	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/git"
	"forkmesh.com/desktop-core/internal/rpc"
)

func runner(t testing.TB) *git.Runner {
	t.Helper()
	if _, err := exec.LookPath("git"); err != nil {
		t.Skip("git not installed")
	}
	home := t.TempDir()
	return &git.Runner{BaseEnv: []string{
		"PATH=" + os.Getenv("PATH"),
		"HOME=" + home,
		"XDG_CONFIG_HOME=" + filepath.Join(home, ".config"),
		"GIT_CONFIG_NOSYSTEM=1",
		"GIT_CONFIG_GLOBAL=" + filepath.Join(home, "gitconfig"),
		"GIT_AUTHOR_NAME=Ada  Lovelace",
		"GIT_AUTHOR_EMAIL=ada@example.invalid",
		"GIT_COMMITTER_NAME=Ada",
		"GIT_COMMITTER_EMAIL=ada@example.invalid",
	}}
}

func must(t testing.TB, r *git.Runner, dir string, env []string, args ...string) {
	t.Helper()
	if result := r.Run(context.Background(), dir, args, git.Options{Env: env}); !result.OK {
		t.Fatalf("git %v: %s", args, result.Error)
	}
}

func commit(t testing.TB, r *git.Runner, dir, file, message string, at int64) {
	t.Helper()
	if err := os.WriteFile(filepath.Join(dir, file), []byte(message), 0o644); err != nil {
		t.Fatal(err)
	}
	stamp := []string{"GIT_AUTHOR_DATE=@" + strconv.FormatInt(at, 10) + " +0000", "GIT_COMMITTER_DATE=@" + strconv.FormatInt(at, 10) + " +0000"}
	must(t, r, dir, nil, "add", file)
	must(t, r, dir, stamp, "commit", "-q", "-m", message)
}

func fixture(t testing.TB, r *git.Runner) (string, string) {
	t.Helper()
	upstream := t.TempDir()
	must(t, r, upstream, nil, "init", "-q", "-b", "main")
	commit(t, r, upstream, "a.txt", "first  commit\twith\x01 tabs", 1700000000)
	commit(t, r, upstream, "b.txt", "second", 1700000100)
	clone := t.TempDir()
	must(t, r, clone, nil, "clone", "-q", upstream, ".")
	must(t, r, clone, nil, "checkout", "-q", "-b", "feature")
	commit(t, r, clone, "c.txt", "feature work", 1700000200)
	must(t, r, clone, nil, "checkout", "-q", "main")
	commit(t, r, upstream, "d.txt", "upstream moved", 1700000300)
	must(t, r, clone, nil, "fetch", "-q")
	commit(t, r, clone, "e.txt", "local main", 1700000400)
	must(t, r, clone, nil, "branch", "-q", "--set-upstream-to=origin/main", "feature")
	return upstream, clone
}

func TestFlatText(t *testing.T) {
	cases := []struct {
		in   string
		max  int
		want string
	}{
		{"  hello \n\t world  ", 300, "hello world"},
		{"a\x00b\x1fc​d", 300, "a b c d"},
		{"abcdef", 3, "abc"},
		{"ab cdef", 3, "ab"},
		{"ab\U0001F600cd", 3, "ab"},
		{"ab\U0001F600cd", 4, "ab\U0001F600"},
		{"  x　y", 300, "x y"},
		{"", 10, ""},
	}
	for _, c := range cases {
		if got := flatText(c.in, c.max); got != c.want {
			t.Errorf("flatText(%q, %d) = %q, want %q", c.in, c.max, got, c.want)
		}
	}
}

func TestClampLimitAndValidation(t *testing.T) {
	cases := map[string]int{"": 50, "null": 50, `"7"`: 50, "0": 1, "-5": 1, "2.9": 2, "1e9": 200, "200": 200, "12": 12}
	for raw, want := range cases {
		if got := clampLimit(json.RawMessage(raw), 50, 200); got != want {
			t.Errorf("clampLimit(%s) = %d, want %d", raw, got, want)
		}
	}
	for slug, ok := range map[string]bool{"jett/forkmesh": true, " a/b ": true, "a/..": false, "../b": false, "a/b/c": false, "a b/c": false, "": false} {
		if _, _, got := parseRepoSlug(slug); got != ok {
			t.Errorf("parseRepoSlug(%q) = %v", slug, got)
		}
	}
	for branch, ok := range map[string]bool{"main": true, "feature/x-1": true, "-x": false, "/x": false, "x/": false, "a..b": false, "a//b": false, "x.lock": false, "a b": false, "": false} {
		if got := validBranchName(branch); got != ok {
			t.Errorf("validBranchName(%q) = %v", branch, got)
		}
	}
	ahead, behind, gone := parseTrack("[ahead 3, behind 12]")
	if ahead != 3 || behind != 12 || gone {
		t.Fatal("track")
	}
	if _, _, gone := parseTrack("[gone]"); !gone {
		t.Fatal("gone")
	}
}

func TestParseCommitsRecords(t *testing.T) {
	out := []byte("\n" + strings.Repeat("a", 40) + "\x1fAda\x1f1700000000\x1fsub\x1fject\x1e\nshort\x1fx\x1f1\x1fy\x1e\n" +
		strings.Repeat("b", 40) + "\x1fBob\x1fnotanumber\x1fz\x1e\n")
	commits := parseCommits(out, 10)
	if len(commits) != 2 {
		t.Fatalf("%v", commits)
	}
	if commits[0]["subject"] != "sub ject" || commits[0]["atMs"] != int64(1700000000000) || commits[1]["atMs"] != int64(0) {
		t.Fatalf("%v", commits)
	}
	if len(parseCommits(out, 1)) != 1 {
		t.Fatal("max")
	}
}

type stubLocator struct {
	location Location
	err      error
	calls    []string
}

func (s *stubLocator) locate(ctx context.Context, peer rpc.Peer, method, repo string) (Location, error) {
	s.calls = append(s.calls, method+" "+repo+" "+peer.Device().Key)
	return s.location, s.err
}

func start(t *testing.T, module *Module) *daemon.Core {
	t.Helper()
	core := coretest.New(t)
	coretest.Start(t, core, module)
	return core
}

func TestPhoneCommitsAndBranches(t *testing.T) {
	r := runner(t)
	_, clone := fixture(t, r)
	stub := &stubLocator{location: Location{Dir: clone, Checkout: true}}
	core := start(t, &Module{Git: r, Locate: stub.locate})
	phone := coretest.NewPeer("p1", rpc.LAN)

	var commits struct {
		Repo    string           `json:"repo"`
		Ref     string           `json:"ref"`
		Commits []map[string]any `json:"commits"`
	}
	coretest.MustCall(t, core, phone, "repos.commits", map[string]any{"repo": " jett/demo ", "limit": 2}, &commits)
	if commits.Repo != "jett/demo" || commits.Ref != "" || len(commits.Commits) != 2 {
		t.Fatalf("%+v", commits)
	}
	if commits.Commits[0]["subject"] != "local main" || commits.Commits[0]["author"] != "Ada Lovelace" || commits.Commits[0]["atMs"] != float64(1700000400000) {
		t.Fatalf("%+v", commits.Commits[0])
	}
	if len(stub.calls) != 1 || stub.calls[0] != "repos.commits  jett/demo  test-key-p1" {
		t.Fatalf("locator calls %v", stub.calls)
	}
	coretest.MustCall(t, core, phone, "repos.commits", map[string]any{"repo": "jett/demo", "ref": " feature "}, &commits)
	if commits.Ref != "feature" || len(commits.Commits) != 3 || commits.Commits[0]["subject"] != "feature work" {
		t.Fatalf("%+v", commits)
	}
	first := commits.Commits[2]["subject"].(string)
	if first != "first commit with tabs" {
		t.Fatalf("flattened subject %q", first)
	}
	coretest.MustCall(t, core, phone, "repos.commits", map[string]any{"repo": "jett/demo", "ref": "missing"}, &commits)
	if len(commits.Commits) != 0 {
		t.Fatal("missing branch has commits")
	}
	if _, err := coretest.Call(t, core, phone, "repos.commits", map[string]any{"repo": "jett/demo", "ref": "../x"}); rpc.Code(err) != "bad_request" {
		t.Fatalf("bad ref: %v", err)
	}

	raw, err := coretest.Call(t, core, phone, "repos.branches", map[string]any{"repo": "jett/demo"})
	if err != nil {
		t.Fatal(err)
	}
	var branches struct {
		Repo     string           `json:"repo"`
		Checkout bool             `json:"checkout"`
		Branches []map[string]any `json:"branches"`
	}
	if err := json.Unmarshal(raw, &branches); err != nil {
		t.Fatal(err)
	}
	if branches.Repo != "jett/demo" || !branches.Checkout || len(branches.Branches) != 2 {
		t.Fatalf("%s", raw)
	}
	main, feature := branches.Branches[0], branches.Branches[1]
	if main["name"] != "main" || main["current"] != true || main["upstream"] != "origin/main" || main["ahead"] != float64(1) || main["behind"] != float64(1) || main["subject"] != "local main" {
		t.Fatalf("main %v", main)
	}
	if feature["name"] != "feature" || feature["current"] != false || feature["ahead"] != float64(1) || feature["behind"] != float64(1) || feature["upstreamGone"] != false || feature["atMs"] != float64(1700000200000) {
		t.Fatalf("feature %v", feature)
	}
	keys := []string{}
	for key := range main {
		keys = append(keys, key)
	}
	if len(keys) != 9 {
		t.Fatalf("branch row keys %v", keys)
	}

	stub.err = rpc.Fail("not_owner")
	if _, err := coretest.Call(t, core, phone, "repos.branches", map[string]any{"repo": "jett/demo"}); rpc.Code(err) != "not_owner" {
		t.Fatalf("locator error: %v", err)
	}
}

func TestPhoneMethodsNeedALocator(t *testing.T) {
	core := start(t, New())
	for _, method := range []string{"repos.commits", "repos.branches", "repos.list", "repos.actions", "repos.actions.log", "repos.prepare", "repos.run"} {
		if core.Registry.Native(method) {
			t.Errorf("%s registered without a locator", method)
		}
	}
	for _, method := range []string{"core.git.status", "core.git.branches", "core.git.log"} {
		if !core.Registry.Native(method) {
			t.Errorf("%s not registered", method)
		}
	}
}

func TestUILocatorWithoutDesktop(t *testing.T) {
	core := start(t, NewWithUILocate())
	phone := coretest.NewPeer("p1", rpc.LAN)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	_, err := core.Registry.Dispatch(ctx, &rpc.Request{Peer: phone, ID: 1, Method: "repos.commits", Params: json.RawMessage(`{"repo":"a/b"}`)})
	if rpc.Code(err) != "desktop_unavailable" {
		t.Fatalf("%v", err)
	}
}

func TestDesktopGitMethods(t *testing.T) {
	r := runner(t)
	_, clone := fixture(t, r)
	core := start(t, &Module{Git: r})
	ui := coretest.NewPeer("ui", rpc.UI)
	phone := coretest.NewPeer("p", rpc.Relay)

	if _, err := coretest.Call(t, core, phone, "core.git.status", map[string]any{"path": clone}); rpc.Code(err) != "forbidden" {
		t.Fatalf("phone allowed: %v", err)
	}
	if _, err := coretest.Call(t, core, ui, "core.git.status", map[string]any{"path": "relative"}); rpc.Code(err) != "bad_request" {
		t.Fatalf("relative path: %v", err)
	}
	if _, err := coretest.Call(t, core, ui, "core.git.status", map[string]any{"path": t.TempDir()}); rpc.Code(err) != "not_a_repository" {
		t.Fatalf("plain dir: %v", err)
	}
	if err := os.WriteFile(filepath.Join(clone, "untracked.txt"), []byte("u"), 0o644); err != nil {
		t.Fatal(err)
	}
	var status map[string]any
	coretest.MustCall(t, core, ui, "core.git.status", map[string]any{"path": clone}, &status)
	if status["branch"] != "main" || status["upstream"] != "origin/main" || status["ahead"] != float64(1) || status["behind"] != float64(1) ||
		status["untracked"] != float64(1) || status["clean"] != false || status["path"] != clone {
		t.Fatalf("%v", status)
	}

	var branches struct {
		Branches []map[string]any `json:"branches"`
	}
	coretest.MustCall(t, core, coretest.NewPeer("cli", rpc.CLI), "core.git.branches", map[string]any{"path": clone, "limit": 1}, &branches)
	if len(branches.Branches) != 1 || branches.Branches[0]["name"] != "main" {
		t.Fatalf("%v", branches)
	}

	var log struct {
		Ref     string           `json:"ref"`
		Commits []map[string]any `json:"commits"`
	}
	coretest.MustCall(t, core, ui, "core.git.log", map[string]any{"path": clone, "ref": "origin/main", "limit": 10}, &log)
	if log.Ref != "origin/main" || len(log.Commits) != 3 || log.Commits[0]["subject"] != "upstream moved" {
		t.Fatalf("%+v", log)
	}
	if _, err := coretest.Call(t, core, ui, "core.git.log", map[string]any{"path": clone, "ref": "--output=/tmp/x"}); rpc.Code(err) != "bad_request" {
		t.Fatalf("option as ref: %v", err)
	}
	if _, err := coretest.Call(t, core, ui, "core.git.log", map[string]any{"path": clone, "ref": "nope"}); err == nil || rpc.Code(err) == "" {
		t.Fatal("unknown ref succeeded")
	}
	empty := t.TempDir()
	must(t, r, empty, nil, "init", "-q")
	coretest.MustCall(t, core, ui, "core.git.log", map[string]any{"path": empty}, &log)
	if len(log.Commits) != 0 {
		t.Fatal("unborn log")
	}
}
