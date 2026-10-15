package files

import (
	"context"
	"encoding/json"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
	"time"

	"forkmesh.com/desktop-core/internal/coretest"
	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/rpc"
)

type fixture struct {
	core   *daemon.Core
	module *Module
	phone  *coretest.Peer
	repo   string
	other  string
	wt     string
}

func iniPath(path string) string {
	return strings.ReplaceAll(filepath.ToSlash(path), `\`, `\\`)
}

func writeSettings(t *testing.T, core *daemon.Core, account string, repos [][3]string) {
	t.Helper()
	var b strings.Builder
	b.WriteString("[account]\nnodeName=" + account + "\n\n[repositories]\n")
	for i, repo := range repos {
		fmt.Fprintf(&b, "items\\%d\\owner=%s\nitems\\%d\\name=%s\nitems\\%d\\localPath=%s\n", i+1, repo[0], i+1, repo[1], i+1, iniPath(repo[2]))
	}
	fmt.Fprintf(&b, "items\\size=%d\n", len(repos))
	path := core.Layout.SettingsFile()
	writeFile(t, path, []byte(b.String()))
	future := time.Now().Add(time.Duration(len(repos)+1) * time.Second)
	_ = os.Chtimes(path, future, future)
}

func newFixture(t *testing.T) *fixture {
	t.Helper()
	core := coretest.New(t)
	dir := t.TempDir()
	repo := filepath.Join(dir, "proj")
	writeFile(t, filepath.Join(repo, ".git", "HEAD"), []byte("ref: refs/heads/main\n"))
	writeFile(t, filepath.Join(repo, "README.md"), []byte("# proj\n"))
	writeFile(t, filepath.Join(repo, "src", "main.go"), []byte("package main\n"))
	writeFile(t, filepath.Join(repo, ".env"), []byte("TOKEN=x\n"))
	writeFile(t, filepath.Join(repo, "blob.bin"), []byte{0, 1, 2, 3})
	wt := filepath.Join(dir, "worktrees", "feature")
	writeFile(t, filepath.Join(wt, ".git"), []byte("gitdir: "+repo+"/.git/worktrees/feature\n"))
	writeFile(t, filepath.Join(wt, "wt.txt"), []byte("worktree\n"))
	writeFile(t, filepath.Join(repo, ".git", "worktrees", "feature", "gitdir"), []byte(wt+"/.git\n"))
	writeFile(t, filepath.Join(repo, ".git", "worktrees", "feature", "HEAD"), []byte("ref: refs/heads/agent/feature\n"))
	other := filepath.Join(dir, "other")
	writeFile(t, filepath.Join(other, "x.txt"), []byte("x"))
	writeSettings(t, core, "alice", [][3]string{
		{"alice", "proj", repo},
		{"bob", "other", other},
		{"", "noowner", other},
		{"carol", "gone", filepath.Join(dir, "missing")},
		{"bad owner", "x", other},
	})
	module := New()
	module.StartDelay = 0
	coretest.Start(t, core, module)
	phone := coretest.NewPeer("p1", rpc.LAN)
	phone.Dev.User = "Alice"
	core.Hub.Add(phone)
	return &fixture{core: core, module: module, phone: phone, repo: repo, other: other, wt: wt}
}

func call(t *testing.T, f *fixture, peer rpc.Peer, method string, params map[string]any) (string, string) {
	t.Helper()
	raw, err := coretest.Call(t, f.core, peer, method, params)
	if err != nil {
		return "", rpc.Code(err)
	}
	return string(raw), ""
}

func TestMethodsRegistered(t *testing.T) {
	f := newFixture(t)
	for _, method := range Methods() {
		if !f.core.Registry.Native(method) {
			t.Errorf("%s not registered", method)
		}
	}
	for _, method := range []string{"files.begin", "files.chunk", "files.browse.other", "file.read", "agentops.file"} {
		if f.core.Registry.Native(method) {
			t.Errorf("%s must stay with the proxy", method)
		}
	}
}

func TestOwnerGate(t *testing.T) {
	f := newFixture(t)
	stranger := coretest.NewPeer("p2", rpc.Relay)
	stranger.Dev.User = "mallory"
	anonymous := coretest.NewPeer("p3", rpc.LAN)
	anonymous.Dev.User = "alice"
	anonymous.Dev.Key = ""
	nouser := coretest.NewPeer("p4", rpc.LAN)
	relayed := coretest.NewPeer("p5", rpc.Relay)
	relayed.Dev.User = " ALICE "
	desktop := coretest.NewPeer("ui", rpc.UI)
	cases := []struct {
		name string
		peer rpc.Peer
		err  string
	}{
		{"owner on lan", f.phone, ""},
		{"owner through relay", relayed, ""},
		{"other user", stranger, "not_owner"},
		{"no device key", anonymous, "not_owner"},
		{"no user", nouser, "not_owner"},
		{"desktop ui", desktop, ""},
	}
	for _, c := range cases {
		for _, method := range Methods() {
			_, err := call(t, f, c.peer, method, map[string]any{"root": "nobody/none"})
			if c.err != "" && err != c.err {
				t.Errorf("%s %s: err %q want %q", c.name, method, err, c.err)
			}
			if c.err == "" && err == "not_owner" {
				t.Errorf("%s %s: refused", c.name, method)
			}
		}
	}
	proxied := 0
	f.core.Registry.SetFallback(func(ctx context.Context, req *rpc.Request) (any, error) {
		proxied++
		return map[string]any{"proxied": req.Method}, nil
	})
	f.module.DesktopAttached = func() bool { return true }
	for _, method := range Methods() {
		got, err := call(t, f, stranger, method, map[string]any{"root": "alice/proj"})
		if err != "" || got != `{"proxied":"`+method+`"}` {
			t.Errorf("unknown user with the desktop attached should be delegated: %s %s", got, err)
		}
		if _, err := call(t, f, anonymous, method, nil); err != "not_owner" {
			t.Errorf("keyless device delegated: %q", err)
		}
		if got, _ := call(t, f, f.phone, method, map[string]any{"root": "alice/proj", "path": "README.md"}); strings.Contains(got, "proxied") {
			t.Errorf("owner %s was delegated", method)
		}
	}
	if proxied != len(Methods()) {
		t.Errorf("proxied %d", proxied)
	}
	f.module.DesktopAttached = nil
	f.module.Owners = func() []string { return []string{"mallory"} }
	if _, err := call(t, f, stranger, "files.browse.roots", nil); err != "" {
		t.Errorf("Owners hook not honoured: %q", err)
	}
	f.module.Owners = nil
	f.core.Set("control.info", map[string]any{"user": "Mallory", "name": "x", "node": "y"})
	if _, err := call(t, f, stranger, "files.browse.roots", nil); err != "" {
		t.Errorf("control.info user not honoured: %q", err)
	}
	f.core.Set("control.info", nil)
	f.core.Set("control.owners", "mallory")
	if _, err := call(t, f, stranger, "files.browse.roots", nil); err != "" {
		t.Errorf("control.owners not honoured: %q", err)
	}
}

func TestRoots(t *testing.T) {
	f := newFixture(t)
	got, err := call(t, f, f.phone, "files.browse.roots", nil)
	if err != "" {
		t.Fatal(err)
	}
	want := `{"roots":[` +
		`{"branch":"main","id":"alice/proj","kind":"repo","label":"alice/proj","repo":"alice/proj"},` +
		`{"branch":"agent/feature","id":"alice/proj#feature","kind":"worktree","label":"agent/feature","repo":"alice/proj"},` +
		`{"branch":"","id":"bob/other","kind":"repo","label":"bob/other","repo":"bob/other"}]}`
	if got != want {
		t.Errorf("roots\n got %s\nwant %s", got, want)
	}
	writeSettings(t, f.core, "alice", nil)
	if got, _ := call(t, f, f.phone, "files.browse.roots", nil); got != `{"roots":[]}` {
		t.Errorf("empty roots %s", got)
	}
}

func TestRootsCap(t *testing.T) {
	core := coretest.New(t)
	dir := t.TempDir()
	repos := [][3]string{}
	for i := 0; i < 210; i++ {
		repos = append(repos, [3]string{"o", fmt.Sprintf("r%03d", i), dir})
	}
	writeSettings(t, core, "alice", repos)
	module := New()
	coretest.Start(t, core, module)
	phone := coretest.NewPeer("p1", rpc.LAN)
	phone.Dev.User = "alice"
	var out struct {
		Roots []map[string]any `json:"roots"`
	}
	coretest.MustCall(t, core, phone, "files.browse.roots", nil, &out)
	if len(out.Roots) != maxRoots {
		t.Errorf("roots %d want %d", len(out.Roots), maxRoots)
	}
}

func TestList(t *testing.T) {
	f := newFixture(t)
	cases := []struct {
		name   string
		params map[string]any
		want   string
		err    string
	}{
		{"unknown root", map[string]any{"root": "zed/none"}, "", "unknown_root"},
		{"invalid root id", map[string]any{"root": "../etc"}, "", "unknown_root"},
		{"missing root param", map[string]any{}, "", "unknown_root"},
		{"root is not a string", map[string]any{"root": 7}, "", "unknown_root"},
		{"missing checkout", map[string]any{"root": "carol/gone"}, "", "root_unavailable"},
		{"unknown worktree", map[string]any{"root": "alice/proj#nope"}, "", "unknown_root"},
		{"traversal", map[string]any{"root": "alice/proj", "path": "../other"}, "", "bad_path"},
		{"git dir", map[string]any{"root": "alice/proj", "path": ".git"}, "", "not_allowed"},
		{"missing dir", map[string]any{"root": "alice/proj", "path": "nope"}, "", "not_found"},
		{"file is not a dir", map[string]any{"root": "alice/proj", "path": "README.md"}, "", "not_a_directory"},
		{"subdir, case-insensitive root, padded", map[string]any{"root": "  ALICE/Proj ", "path": "./src/"}, `{"entries":[{"kind":"file","modifiedMs":M,"name":"main.go","size":13}],"path":"src","root":"ALICE/Proj","total":1,"truncated":false}`, ""},
		{"worktree", map[string]any{"root": "alice/proj#FEATURE"}, `{"entries":[{"kind":"file","modifiedMs":M,"name":"wt.txt","size":9}],"path":"","root":"alice/proj#FEATURE","total":1,"truncated":false}`, ""},
	}
	for _, c := range cases {
		got, err := call(t, f, f.phone, "files.browse.list", c.params)
		if err != c.err {
			t.Errorf("%s: err %q want %q (%s)", c.name, err, c.err, got)
			continue
		}
		if c.want != "" && maskTimes(got) != c.want {
			t.Errorf("%s\n got %s\nwant %s", c.name, maskTimes(got), c.want)
		}
	}
	var top struct {
		Entries []map[string]any `json:"entries"`
		Total   int              `json:"total"`
	}
	coretest.MustCall(t, f.core, f.phone, "files.browse.list", map[string]any{"root": "alice/proj"}, &top)
	names := []string{}
	for _, e := range top.Entries {
		names = append(names, fmt.Sprint(e["name"]))
	}
	if strings.Join(names, ",") != "src,.env,blob.bin,README.md" || top.Total != 4 || top.Entries[1]["sensitive"] != true {
		t.Errorf("top listing %v %d %v", names, top.Total, top.Entries)
	}
}

func maskTimes(s string) string {
	var out strings.Builder
	for {
		i := strings.Index(s, `"modifiedMs":`)
		if i < 0 {
			out.WriteString(s)
			return out.String()
		}
		out.WriteString(s[:i+13])
		s = s[i+13:]
		j := 0
		for j < len(s) && s[j] >= '0' && s[j] <= '9' {
			j++
		}
		out.WriteString("M")
		s = s[j:]
	}
}

func TestRead(t *testing.T) {
	f := newFixture(t)
	cases := []struct {
		name   string
		params map[string]any
		want   string
		err    string
	}{
		{"text", map[string]any{"root": "alice/proj", "path": "README.md"}, `{"binary":false,"eof":true,"length":7,"modifiedMs":M,"offset":0,"path":"README.md","root":"alice/proj","size":7,"text":"# proj\n"}`, ""},
		{"slice", map[string]any{"root": "alice/proj", "path": "./src//main.go", "offset": 8, "length": 4}, `{"binary":false,"eof":false,"length":4,"modifiedMs":M,"offset":8,"path":"src/main.go","root":"alice/proj","size":13,"text":"main"}`, ""},
		{"string numbers fall back", map[string]any{"root": "alice/proj", "path": "README.md", "offset": "3", "length": "1"}, `{"binary":false,"eof":true,"length":7,"modifiedMs":M,"offset":0,"path":"README.md","root":"alice/proj","size":7,"text":"# proj\n"}`, ""},
		{"fractional numbers truncate", map[string]any{"root": "alice/proj", "path": "README.md", "offset": 2.9, "length": 1.9}, `{"binary":false,"eof":false,"length":1,"modifiedMs":M,"offset":2,"path":"README.md","root":"alice/proj","size":7,"text":"p"}`, ""},
		{"binary", map[string]any{"root": "alice/proj", "path": "blob.bin"}, `{"base64":"AAECAw==","binary":true,"eof":true,"length":4,"modifiedMs":M,"offset":0,"path":"blob.bin","root":"alice/proj","size":4}`, ""},
		{"worktree file", map[string]any{"root": "alice/proj#feature", "path": "wt.txt"}, `{"binary":false,"eof":true,"length":9,"modifiedMs":M,"offset":0,"path":"wt.txt","root":"alice/proj#feature","size":9,"text":"worktree\n"}`, ""},
		{"sensitive", map[string]any{"root": "alice/proj", "path": ".env"}, "", "sensitive_file"},
		{"directory", map[string]any{"root": "alice/proj", "path": "src"}, "", "not_a_file"},
		{"root itself", map[string]any{"root": "alice/proj"}, "", "not_a_file"},
		{"git file", map[string]any{"root": "alice/proj", "path": ".git/HEAD"}, "", "not_allowed"},
		{"missing", map[string]any{"root": "alice/proj", "path": "nope.txt"}, "", "not_found"},
		{"absolute", map[string]any{"root": "alice/proj", "path": "/etc/passwd"}, "", "bad_path"},
		{"unknown root", map[string]any{"root": "alice/nope", "path": "README.md"}, "", "unknown_root"},
	}
	for _, c := range cases {
		got, err := call(t, f, f.phone, "files.browse.read", c.params)
		if err != c.err {
			t.Errorf("%s: err %q want %q (%s)", c.name, err, c.err, got)
			continue
		}
		if c.want != "" && maskTimes(got) != c.want {
			t.Errorf("%s\n got %s\nwant %s", c.name, maskTimes(got), c.want)
		}
	}
	if runtime.GOOS != "windows" {
		outside := t.TempDir()
		writeFile(t, filepath.Join(outside, "secret.txt"), []byte("s"))
		symlink(t, filepath.Join(outside, "secret.txt"), filepath.Join(f.repo, "leak.txt"))
		if _, err := call(t, f, f.phone, "files.browse.read", map[string]any{"root": "alice/proj", "path": "leak.txt"}); err != "outside_root" {
			t.Errorf("symlink escape: %q", err)
		}
	}
}

func fakeGit(t *testing.T, script string) string {
	t.Helper()
	if runtime.GOOS == "windows" {
		t.Skip("shell script stand-in for git")
	}
	path := filepath.Join(t.TempDir(), "git")
	writeFile(t, path, []byte("#!/bin/sh\n"+script+"\n"))
	if err := os.Chmod(path, 0o755); err != nil {
		t.Fatal(err)
	}
	return path
}

func TestStatusWithStandIn(t *testing.T) {
	f := newFixture(t)
	f.module.Git = fakeGit(t, `[ "$GIT_TERMINAL_PROMPT$GIT_OPTIONAL_LOCKS$LC_ALL" = "00C" ] || exit 3
[ "$*" = "status --porcelain=v1 -b -z --untracked-files=normal" ] || exit 4
printf '## main...origin/main [behind 3]\000?? new.txt\000'`)
	got, err := call(t, f, f.phone, "files.browse.status", map[string]any{"root": "alice/proj#feature"})
	want := `{"ahead":0,"behind":3,"branch":"main","conflicted":0,"files":[{"index":"?","kind":"untracked","path":"new.txt","worktree":"?"}],"label":"agent/feature","root":"alice/proj#feature","staged":0,"total":1,"truncated":false,"unstaged":0,"untracked":1,"upstream":"origin/main"}`
	if err != "" || got != want {
		t.Errorf("status\n got %s %s\nwant %s", got, err, want)
	}
	f.module.Git = fakeGit(t, "exit 128")
	if _, err := call(t, f, f.phone, "files.browse.status", map[string]any{"root": "alice/proj"}); err != "git_status_failed" {
		t.Errorf("failed status: %q", err)
	}
	f.module.Git = filepath.Join(t.TempDir(), "no-git-here")
	if _, err := call(t, f, f.phone, "files.browse.status", map[string]any{"root": "alice/proj"}); err != "git_unavailable" {
		t.Errorf("missing git: %q", err)
	}
	f.module.Git = fakeGit(t, "exec sleep 5")
	f.module.StatusTimeout = 100 * time.Millisecond
	if _, err := call(t, f, f.phone, "files.browse.status", map[string]any{"root": "alice/proj"}); err != "timeout" {
		t.Errorf("slow status: %q", err)
	}
	if _, err := call(t, f, f.phone, "files.browse.status", map[string]any{"root": "carol/gone"}); err != "git_unavailable" {
		t.Errorf("missing checkout: %q", err)
	}
	if _, err := call(t, f, f.phone, "files.browse.status", map[string]any{"root": "nobody/x"}); err != "unknown_root" {
		t.Errorf("unknown root: %q", err)
	}
}

func TestStatusWithRealGit(t *testing.T) {
	if _, err := exec.LookPath("git"); err != nil {
		t.Skip("git not installed")
	}
	core := coretest.New(t)
	home := t.TempDir()
	t.Setenv("HOME", home)
	t.Setenv("XDG_CONFIG_HOME", filepath.Join(home, ".config"))
	t.Setenv("GIT_CONFIG_NOSYSTEM", "1")
	repo := filepath.Join(t.TempDir(), "r")
	if err := os.MkdirAll(repo, 0o755); err != nil {
		t.Fatal(err)
	}
	run := func(args ...string) {
		cmd := exec.Command("git", args...)
		cmd.Dir = repo
		cmd.Env = append(os.Environ(), "GIT_AUTHOR_NAME=t", "GIT_AUTHOR_EMAIL=t@t", "GIT_COMMITTER_NAME=t", "GIT_COMMITTER_EMAIL=t@t")
		if out, err := cmd.CombinedOutput(); err != nil {
			t.Fatalf("git %v: %v %s", args, err, out)
		}
	}
	run("init", "-q", "-b", "trunk")
	writeFile(t, filepath.Join(repo, "a.txt"), []byte("a"))
	writeFile(t, filepath.Join(repo, "b.txt"), []byte("b"))
	run("add", "a.txt", "b.txt")
	run("commit", "-q", "-m", "one")
	writeFile(t, filepath.Join(repo, "a.txt"), []byte("changed"))
	run("mv", "b.txt", "c.txt")
	writeFile(t, filepath.Join(repo, "new.txt"), []byte("n"))
	writeSettings(t, core, "alice", [][3]string{{"alice", "r", repo}})
	module := New()
	coretest.Start(t, core, module)
	phone := coretest.NewPeer("p1", rpc.LAN)
	phone.Dev.User = "alice"
	raw, err := coretest.Call(t, core, phone, "files.browse.status", map[string]any{"root": "alice/r"})
	if err != nil {
		t.Fatal(err)
	}
	want := `{"ahead":0,"behind":0,"branch":"trunk","conflicted":0,"files":[{"index":" ","kind":"modified","path":"a.txt","worktree":"M"},{"from":"b.txt","index":"R","kind":"renamed","path":"c.txt","worktree":" "},{"index":"?","kind":"untracked","path":"new.txt","worktree":"?"}],"label":"alice/r","root":"alice/r","staged":1,"total":3,"truncated":false,"unstaged":1,"untracked":1,"upstream":""}`
	if string(raw) != want {
		t.Errorf("real status\n got %s\nwant %s", raw, want)
	}
}

func prepare(t *testing.T, f *fixture, peer rpc.Peer, root, action string) map[string]any {
	t.Helper()
	var out map[string]any
	coretest.MustCall(t, f.core, peer, "files.browse.prepare", map[string]any{"root": root, "action": action}, &out)
	return out
}

func TestPrepare(t *testing.T) {
	f := newFixture(t)
	out := prepare(t, f, f.phone, "alice/proj#feature", "pull")
	token, _ := out["token"].(string)
	delete(out, "token")
	got, _ := json.Marshal(out)
	want := `{"action":"pull","expiresInMs":60000,"subject":"alice/proj#feature","summary":"Pull \"agent/feature\" from its upstream, fast-forward only. Nothing is merged or rewound; a diverged branch is left as it is."}`
	if string(got) != want || len(token) != 48 {
		t.Errorf("prepare\n got %s token %q\nwant %s", got, token, want)
	}
	fetch := prepare(t, f, f.phone, "alice/proj", "fetch")
	if fetch["summary"] != `Fetch "alice/proj" from its remote. Nothing in the working tree changes.` {
		t.Errorf("fetch summary %v", fetch["summary"])
	}
	cases := []struct {
		params map[string]any
		err    string
	}{
		{map[string]any{"root": "alice/proj", "action": "push"}, "unsupported_action"},
		{map[string]any{"root": "alice/proj"}, "unsupported_action"},
		{map[string]any{"root": "alice/proj", "action": "Pull"}, "unsupported_action"},
		{map[string]any{"root": "nobody/x", "action": "pull"}, "unknown_root"},
	}
	for _, c := range cases {
		if _, err := call(t, f, f.phone, "files.browse.prepare", c.params); err != c.err {
			t.Errorf("prepare %v: %q want %q", c.params, err, c.err)
		}
	}
}

func TestRun(t *testing.T) {
	f := newFixture(t)
	f.module.Git = fakeGit(t, `[ "$*" = "pull --ff-only --quiet" ] || [ "$*" = "fetch --quiet" ] || exit 9
[ "$(pwd -P)" = "$(cd "$EXPECT_DIR" && pwd -P)" ] || exit 8
echo "  fetched $1 " ; echo "to stderr" >&2`)
	t.Setenv("EXPECT_DIR", f.wt)
	listener := coretest.NewPeer("p9", rpc.Relay)
	listener.Dev.User = "alice"
	f.core.Hub.Add(listener)
	desktop := coretest.NewPeer("ui", rpc.UI)
	f.core.Hub.Add(desktop)

	token := prepare(t, f, f.phone, "alice/proj#feature", "pull")["token"]
	got, err := call(t, f, f.phone, "files.browse.run", map[string]any{"root": "ALICE/proj#feature", "action": "pull", "token": token})
	if err != "" || got != `{"action":"pull","message":"Pulling agent/feature.","started":true,"subject":"ALICE/proj#feature"}` {
		t.Fatalf("run: %s %s", got, err)
	}
	for _, peer := range []*coretest.Peer{f.phone, listener} {
		action := string(peer.Next(t, "files.browse.action", 5*time.Second))
		if action != `{"action":"pull","message":"fetched pull \nto stderr","ok":true,"root":"ALICE/proj#feature"}` {
			t.Errorf("action event %s", action)
		}
		if changed := string(peer.Next(t, "files.browse.changed", time.Second)); changed != `{"root":"ALICE/proj#feature"}` {
			t.Errorf("changed event %s", changed)
		}
	}
	select {
	case e := <-desktop.Events:
		t.Errorf("desktop UI got %s", e.Name)
	default:
	}
	if _, err := call(t, f, f.phone, "files.browse.run", map[string]any{"root": "alice/proj#feature", "action": "pull", "token": token}); err != "confirmation_required" {
		t.Errorf("token reuse: %q", err)
	}

	stranger := coretest.NewPeer("p7", rpc.LAN)
	stranger.Dev.User = "alice"
	stranger.Dev.Key = "another-phone"
	cases := []struct {
		name  string
		peer  rpc.Peer
		issue [2]string
		run   map[string]any
		err   string
	}{
		{"no token", f.phone, [2]string{}, map[string]any{"root": "alice/proj", "action": "fetch"}, "confirmation_required"},
		{"wrong action", f.phone, [2]string{"alice/proj", "fetch"}, map[string]any{"root": "alice/proj", "action": "pull"}, "confirmation_mismatch"},
		{"wrong root", f.phone, [2]string{"alice/proj", "fetch"}, map[string]any{"root": "alice/proj#feature", "action": "fetch"}, "confirmation_mismatch"},
		{"other device", stranger, [2]string{"alice/proj", "fetch"}, map[string]any{"root": "alice/proj", "action": "fetch"}, "confirmation_mismatch"},
		{"unknown root first", f.phone, [2]string{"alice/proj", "fetch"}, map[string]any{"root": "nobody/x", "action": "fetch"}, "unknown_root"},
	}
	for _, c := range cases {
		if c.issue[0] != "" {
			c.run["token"] = prepare(t, f, f.phone, c.issue[0], c.issue[1])["token"]
		}
		if _, err := call(t, f, c.peer, "files.browse.run", c.run); err != c.err {
			t.Errorf("%s: %q want %q", c.name, err, c.err)
		}
	}
	f.module.Now = func() time.Time { return time.Now().Add(-2 * time.Minute) }
	stale := prepare(t, f, f.phone, "alice/proj", "fetch")["token"]
	f.module.Now = nil
	if _, err := call(t, f, f.phone, "files.browse.run", map[string]any{"root": "alice/proj", "action": "fetch", "token": stale}); err != "confirmation_expired" {
		t.Errorf("expired: %q", err)
	}
}

func runAndWait(t *testing.T, f *fixture, root, action string) string {
	t.Helper()
	token := prepare(t, f, f.phone, root, action)["token"]
	if _, err := call(t, f, f.phone, "files.browse.run", map[string]any{"root": root, "action": action, "token": token}); err != "" {
		t.Fatalf("run %s: %s", action, err)
	}
	event := string(f.phone.Next(t, "files.browse.action", 5*time.Second))
	f.phone.Next(t, "files.browse.changed", time.Second)
	return event
}

func TestRunOutcomes(t *testing.T) {
	f := newFixture(t)
	f.module.Git = fakeGit(t, "exit 1")
	if got := runAndWait(t, f, "alice/proj", "fetch"); got != `{"action":"fetch","message":"git fetch failed.","ok":false,"root":"alice/proj"}` {
		t.Errorf("silent failure %s", got)
	}
	f.module.Git = fakeGit(t, "exit 0")
	if got := runAndWait(t, f, "alice/proj", "fetch"); got != `{"action":"fetch","message":"Done.","ok":true,"root":"alice/proj"}` {
		t.Errorf("silent success %s", got)
	}
	f.module.Git = fakeGit(t, `printf 'x%.0s' $(seq 1 2000); echo "fatal: Not possible to fast-forward"; exit 128`)
	got := runAndWait(t, f, "alice/proj", "pull")
	var event map[string]any
	_ = json.Unmarshal([]byte(got), &event)
	message, _ := event["message"].(string)
	if len(message) != 1500 || !strings.HasSuffix(message, "fatal: Not possible to fast-forward") || event["ok"] != false {
		t.Errorf("long failure %d %v", len(message), event["ok"])
	}
	f.module.Git = filepath.Join(t.TempDir(), "absent")
	if got := runAndWait(t, f, "alice/proj", "fetch"); got != `{"action":"fetch","message":"git is not available on the desktop.","ok":false,"root":"alice/proj"}` {
		t.Errorf("no git %s", got)
	}
	f.module.Git = fakeGit(t, "exec sleep 5")
	f.module.GitTimeout = 100 * time.Millisecond
	if got := runAndWait(t, f, "alice/proj", "fetch"); got != `{"action":"fetch","message":"git timed out.","ok":false,"root":"alice/proj"}` {
		t.Errorf("timeout %s", got)
	}
	if f.module.Busy() {
		t.Errorf("busy after every run finished")
	}
}

func TestRunAlreadyRunning(t *testing.T) {
	f := newFixture(t)
	f.module.Git = fakeGit(t, "exec sleep 5")
	first := prepare(t, f, f.phone, "alice/proj", "fetch")["token"]
	if _, err := call(t, f, f.phone, "files.browse.run", map[string]any{"root": "alice/proj", "action": "fetch", "token": first}); err != "" {
		t.Fatal(err)
	}
	if !f.module.Busy() {
		t.Errorf("not busy while git runs")
	}
	second := prepare(t, f, f.phone, "Alice/Proj", "fetch")["token"]
	if _, err := call(t, f, f.phone, "files.browse.run", map[string]any{"root": "Alice/Proj", "action": "fetch", "token": second}); err != "already_running" {
		t.Errorf("second run: %q", err)
	}
	pull := prepare(t, f, f.phone, "alice/proj", "pull")["token"]
	if _, err := call(t, f, f.phone, "files.browse.run", map[string]any{"root": "alice/proj", "action": "pull", "token": pull}); err != "" {
		t.Errorf("a different action runs alongside: %q", err)
	}
	f.module.Stop()
	deadline := time.Now().Add(5 * time.Second)
	for f.module.Busy() && time.Now().Before(deadline) {
		time.Sleep(10 * time.Millisecond)
	}
	if f.module.Busy() {
		t.Errorf("Stop left git running")
	}
}
