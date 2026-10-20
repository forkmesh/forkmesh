package mirrornode

import (
	"context"
	"encoding/base64"
	"errors"
	"net/http/httptest"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"
)

const testPushToken = "ghp_TESTtoken0123456789abcdefABCDEF"

func testPushMirror() hostedPushMirror {
	return hostedPushMirror{Revision: "rev-1", Provider: "github",
		URL: "https://github.com/alice/proj.git", Username: "x-access-token",
		Token: testPushToken}
}

func TestHostedPushMirrorValidation(t *testing.T) {
	hosts := hostedDefaultPushHosts
	cases := []struct {
		name   string
		mutate func(*hostedPushMirror)
		want   bool
	}{
		{"valid github", func(*hostedPushMirror) {}, true},
		{"valid gitlab", func(m *hostedPushMirror) {
			m.Provider, m.Username, m.URL = "gitlab", "oauth2", "https://gitlab.com/acme/Tool_1.git"
		}, true},
		{"dotted name", func(m *hostedPushMirror) { m.URL = "https://github.com/alice/.github.git" }, true},
		{"provider", func(m *hostedPushMirror) { m.Provider = "codeberg" }, false},
		{"username", func(m *hostedPushMirror) { m.Username = "alice" }, false},
		{"empty token", func(m *hostedPushMirror) { m.Token = "" }, false},
		{"long token", func(m *hostedPushMirror) { m.Token = strings.Repeat("a", 513) }, false},
		{"max token", func(m *hostedPushMirror) { m.Token = strings.Repeat("a", 512) }, true},
		{"token space", func(m *hostedPushMirror) { m.Token = "abc def" }, false},
		{"token newline", func(m *hostedPushMirror) { m.Token = "abc\ndef" }, false},
		{"token control", func(m *hostedPushMirror) { m.Token = "abc\x01def" }, false},
		{"token nbsp", func(m *hostedPushMirror) { m.Token = "abc def" }, false},
		{"http", func(m *hostedPushMirror) { m.URL = "http://github.com/alice/proj.git" }, false},
		{"host", func(m *hostedPushMirror) { m.URL = "https://evil.example/alice/proj.git" }, false},
		{"host suffix", func(m *hostedPushMirror) { m.URL = "https://github.com.evil/alice/proj.git" }, false},
		{"upper host", func(m *hostedPushMirror) { m.URL = "https://GitHub.com/alice/proj.git" }, false},
		{"userinfo", func(m *hostedPushMirror) { m.URL = "https://x:y@github.com/alice/proj.git" }, false},
		{"port", func(m *hostedPushMirror) { m.URL = "https://github.com:443/alice/proj.git" }, false},
		{"query", func(m *hostedPushMirror) { m.URL = "https://github.com/alice/proj.git?a=1" }, false},
		{"fragment", func(m *hostedPushMirror) { m.URL = "https://github.com/alice/proj.git#x" }, false},
		{"no .git", func(m *hostedPushMirror) { m.URL = "https://github.com/alice/proj" }, false},
		{"subgroup", func(m *hostedPushMirror) { m.URL = "https://github.com/a/b/proj.git" }, false},
		{"dot dot", func(m *hostedPushMirror) { m.URL = "https://github.com/alice/...git" }, false},
		{"escape", func(m *hostedPushMirror) { m.URL = "https://github.com/alice/pr%20oj.git" }, false},
		{"space", func(m *hostedPushMirror) { m.URL = "https://github.com/alice/pr oj.git" }, false},
		{"dash owner", func(m *hostedPushMirror) { m.URL = "https://github.com/-alice/proj.git" }, false},
	}
	for _, tc := range cases {
		mirror := testPushMirror()
		tc.mutate(&mirror)
		if got := validHostedPushMirror(mirror, hosts); got != tc.want {
			t.Errorf("%s: valid = %v, want %v", tc.name, got, tc.want)
		}
	}
	mirror := testPushMirror()
	mirror.URL = "https://git.example.org/alice/proj.git"
	if !validHostedPushMirror(mirror, []string{"git.example.org"}) {
		t.Error("a configured push host was refused")
	}
	if validHostedPushMirror(testPushMirror(), []string{"git.example.org"}) {
		t.Error("a host left out of pushHosts was accepted")
	}

	cfg := Config{Role: RoleCore, CatalogURL: "https://app.example/api/repositories",
		Hosted: &HostedConfig{Enabled: true, Root: "/var/lib/forkmesh/hosted",
			PushHosts: []string{"GitHub.com"}}}
	if err := cfg.validateHosted(); err == nil {
		t.Fatal("an upper-case push host was accepted")
	}
}

func TestHostedPushMirrorRedaction(t *testing.T) {
	mirror := testPushMirror()
	header := base64.StdEncoding.EncodeToString([]byte("x-access-token:" + testPushToken))
	raw := "fatal: unable to access with " + testPushToken + " and header Basic " + header +
		"\n" + strings.Repeat("x", 600)
	got := redactHostedPushError(raw, mirror)
	if strings.Contains(got, testPushToken) || strings.Contains(got, header) {
		t.Fatalf("secret leaked: %q", got)
	}
	if !strings.Contains(got, "***") || len(got) > hostedPushMaxError {
		t.Fatalf("redacted = %q (%d chars)", got, len(got))
	}
	if hostedPushRefCount("To https://github.com/alice/proj.git\n*\trefs/heads/main:refs/heads/main\t[new branch]\n"+
		"=\trefs/tags/v1:refs/tags/v1\t[up to date]\n+\trefs/heads/x:refs/heads/x\tabc...def (forced update)\n"+
		"-\t:refs/heads/old\t[deleted]\nDone\n") != 3 {
		t.Fatal("porcelain ref count is wrong")
	}
}

type recordedPush struct {
	args []string
	env  []string
}

type fakePushRunner struct {
	t      *testing.T
	remote string
	mu     sync.Mutex
	calls  []recordedPush
	fail   error
}

func (f *fakePushRunner) run(ctx context.Context, gitDir string, args, env []string) (string, string, error) {
	f.mu.Lock()
	f.calls = append(f.calls, recordedPush{args: append([]string(nil), args...), env: append([]string(nil), env...)})
	fail := f.fail
	f.mu.Unlock()
	if fail != nil {
		return "", "fatal: Authentication failed for token " + testPushToken, fail
	}
	local := make([]string, 0, len(args))
	for _, arg := range args {
		if strings.HasPrefix(arg, "https://") {
			arg = f.remote
		}
		local = append(local, arg)
	}
	command := exec.CommandContext(ctx, "git", append([]string{"-C", gitDir}, local...)...)
	command.Env = append(append([]string(nil), env...), "GIT_ALLOW_PROTOCOL=file")
	var stdout, stderr strings.Builder
	command.Stdout = &stdout
	command.Stderr = &stderr
	err := command.Run()
	return stdout.String(), stderr.String(), err
}

func (f *fakePushRunner) count() int {
	f.mu.Lock()
	defer f.mu.Unlock()
	return len(f.calls)
}

func (f *fakePushRunner) last() recordedPush {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.calls[len(f.calls)-1]
}

func TestHostedPushMirrorPushesAndReports(t *testing.T) {
	var app fakeHostedApp
	appServer := httptest.NewServer(&app)
	defer appServer.Close()
	node := hostedTestDaemon(t, "fmnext-n1", appServer.URL)
	remote := filepath.Join(t.TempDir(), "remote.git")
	run(t, ".", "git", "init", "--quiet", "--bare", remote)
	runner := &fakePushRunner{t: t, remote: "file://" + remote}
	node.hosted.runPush = runner.run
	now := time.Unix(1790000000, 0)
	node.hosted.clock = func() time.Time { return now }

	var mu sync.Mutex
	mirror := testPushMirror()
	primary := true
	removed := false
	app.answer = func(string, map[string]hostedCopyState) hostedReplicaResponse {
		mu.Lock()
		defer mu.Unlock()
		if removed {
			return hostedReplicaResponse{Removed: []hostedRemoved{{ID: testHostedID, Owner: "alice", Name: "proj"}}}
		}
		copied := mirror
		item := hostedDesired{ID: testHostedID, Owner: "alice", Name: "proj", Visibility: "public",
			Primary: primary, Init: testInit(), PushMirror: &copied}
		return hostedReplicaResponse{Repositories: []hostedDesired{item}}
	}
	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()
	if err := node.hosted.reconcile(ctx); err != nil {
		t.Fatal(err)
	}
	if runner.count() != 1 {
		t.Fatalf("pushes = %d, want 1", runner.count())
	}
	gitDir := node.hosted.gitDir(testHostedID)
	head := run(t, ".", "git", "--git-dir="+gitDir, "rev-parse", "refs/heads/main")
	if got := run(t, ".", "git", "--git-dir="+remote, "rev-parse", "refs/heads/main"); got != head {
		t.Fatalf("remote main = %s, want %s", got, head)
	}
	call := runner.last()
	argv := strings.Join(call.args, " ")
	if strings.Contains(argv, testPushToken) || strings.Contains(argv, "@") {
		t.Fatalf("the token reached the command line: %v", call.args)
	}
	want := []string{"push", "--porcelain", mirror.URL, "+refs/heads/*:refs/heads/*", "+refs/tags/*:refs/tags/*"}
	if strings.Join(call.args, " ") != strings.Join(want, " ") {
		t.Fatalf("args = %v", call.args)
	}
	header := "Authorization: Basic " + base64.StdEncoding.EncodeToString([]byte("x-access-token:"+testPushToken))
	environment := strings.Join(call.env, "\n")
	for _, entry := range []string{"GIT_CONFIG_KEY_0=http.extraHeader", "GIT_CONFIG_VALUE_0=" + header,
		"GIT_CONFIG_KEY_1=http.followRedirects", "GIT_CONFIG_VALUE_1=false",
		"GIT_CONFIG_KEY_2=credential.helper", "GIT_CONFIG_VALUE_2=", "GIT_CONFIG_COUNT=3",
		"GIT_TERMINAL_PROMPT=0", "GIT_ALLOW_PROTOCOL=https"} {
		if !strings.Contains("\n"+environment+"\n", "\n"+entry+"\n") {
			t.Fatalf("push environment lacks %q", entry)
		}
	}

	digest, _ := routingRefsSHA256(ctx, gitDir)
	if err := node.hosted.reconcile(ctx); err != nil {
		t.Fatal(err)
	}
	if runner.count() != 1 {
		t.Fatal("an unchanged repository was pushed again")
	}
	app.mu.Lock()
	reports := app.pushes["fmnext-n1"]
	app.mu.Unlock()
	report, ok := reports[len(reports)-1][testHostedID]
	if !ok || !report.OK || report.Revision != "rev-1" || report.Digest != digest ||
		report.Refs != 1 || report.Error != "" || report.At != now.UnixMilli() {
		t.Fatalf("push report = %+v (found %v)", report, ok)
	}

	mu.Lock()
	mirror.Revision = "rev-2"
	mirror.Prune = true
	mu.Unlock()
	if err := node.hosted.reconcile(ctx); err != nil {
		t.Fatal(err)
	}
	if runner.count() != 2 || runner.last().args[2] != "--prune" {
		t.Fatalf("a changed revision did not push with --prune: %d %v", runner.count(), runner.last().args)
	}

	work := filepath.Join(t.TempDir(), "work")
	run(t, ".", "git", "clone", "--quiet", gitDir, work)
	run(t, work, "git", "config", "user.email", "alice@example.test")
	run(t, work, "git", "config", "user.name", "Alice")
	if err := os.WriteFile(filepath.Join(work, "main.go"), []byte("package main\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	run(t, work, "git", "add", "main.go")
	run(t, work, "git", "commit", "--quiet", "-m", "code")
	run(t, work, "git", "push", "--quiet", "origin", "main")
	if err := node.hosted.reconcile(ctx); err != nil {
		t.Fatal(err)
	}
	if runner.count() != 3 {
		t.Fatal("a changed routing digest did not push")
	}
	if got := run(t, ".", "git", "--git-dir="+remote, "rev-parse", "refs/heads/main"); got != run(t, work, "git", "rev-parse", "HEAD") {
		t.Fatal("the remote did not receive the new commit")
	}

	run(t, work, "git", "push", "--quiet", "origin", "main:refs/heads/feature")
	if err := node.hosted.reconcile(ctx); err != nil {
		t.Fatal(err)
	}
	if runner.count() != 4 {
		t.Fatal("a new branch did not push")
	}

	runner.mu.Lock()
	runner.fail = errors.New("exit status 128")
	runner.mu.Unlock()
	mu.Lock()
	mirror.Revision = "rev-3"
	mu.Unlock()
	if err := node.hosted.reconcile(ctx); err != nil {
		t.Fatalf("a failed push marked the replicator unhealthy: %v", err)
	}
	if runner.count() != 5 {
		t.Fatal("the new revision did not push")
	}
	failed := node.hosted.pushMirrorReports()[testHostedID]
	if failed.OK || strings.Contains(failed.Error, testPushToken) || !strings.Contains(failed.Error, "***") {
		t.Fatalf("failed report = %+v", failed)
	}
	now = now.Add(5 * time.Minute)
	if err := node.hosted.reconcile(ctx); err != nil {
		t.Fatal(err)
	}
	if runner.count() != 5 {
		t.Fatal("a failed push was retried before the backoff")
	}
	runner.mu.Lock()
	runner.fail = nil
	runner.mu.Unlock()
	now = now.Add(6 * time.Minute)
	if err := node.hosted.reconcile(ctx); err != nil {
		t.Fatal(err)
	}
	if runner.count() != 6 || !node.hosted.pushMirrorReports()[testHostedID].OK {
		t.Fatal("a failed push was not retried after the backoff")
	}
	now = now.Add(hostedPushRefreshAfter)
	if err := node.hosted.reconcile(ctx); err != nil {
		t.Fatal(err)
	}
	if runner.count() != 7 {
		t.Fatal("the periodic refresh did not push")
	}

	mu.Lock()
	primary = false
	mirror.Revision = "rev-4"
	mu.Unlock()
	if err := node.hosted.reconcile(ctx); err != nil {
		t.Fatal(err)
	}
	if runner.count() != 7 {
		t.Fatal("a replica pushed")
	}

	mu.Lock()
	primary = true
	mirror.URL = "https://evil.example/alice/proj.git"
	mu.Unlock()
	if err := node.hosted.reconcile(ctx); err != nil {
		t.Fatal(err)
	}
	invalid := node.hosted.pushMirrorReports()[testHostedID]
	if runner.count() != 7 || invalid.OK || invalid.Error != hostedPushInvalid || invalid.Revision != "rev-4" {
		t.Fatalf("invalid mirror: pushes %d report %+v", runner.count(), invalid)
	}

	mu.Lock()
	removed = true
	mu.Unlock()
	if err := node.hosted.reconcile(ctx); err != nil {
		t.Fatal(err)
	}
	if _, ok := node.hosted.pushMirrorReports()[testHostedID]; ok {
		t.Fatal("push state outlived the repository")
	}
}

func TestHostedPushMirrorRefsValidation(t *testing.T) {
	for refs, want := range map[string]bool{"": true, "all": true, "default": true,
		"Default": false, "heads": false, "main": false} {
		mirror := testPushMirror()
		mirror.Refs = refs
		if got := validHostedPushMirror(mirror, hostedDefaultPushHosts); got != want {
			t.Errorf("refs %q: valid = %v, want %v", refs, got, want)
		}
	}
}

func TestHostedPushExtraBranchDeletes(t *testing.T) {
	listing := "aaa\trefs/heads/main\n" +
		"bbb\trefs/heads/feature/x\n" +
		"ccc\trefs/heads/old\r\n" +
		"ddd\trefs/heads/old\n" +
		"eee\trefs/tags/v1\n" +
		"fff\trefs/heads/a:b\n" +
		"fff\trefs/heads/wild*\n" +
		"fff\trefs/heads/x..y\n" +
		"fff\trefs/heads/\n" +
		"fff\trefs/heads/sp ace\n" +
		"fff\trefs/heads/ctl\x01\n" +
		"warning: something\n" +
		"\n"
	got := hostedPushExtraBranchDeletes(listing, "refs/heads/main")
	want := []string{":refs/heads/feature/x", ":refs/heads/old"}
	if strings.Join(got, " ") != strings.Join(want, " ") {
		t.Fatalf("deletes = %v, want %v", got, want)
	}
	var many strings.Builder
	for i := 0; i < hostedPushMaxDeletes+5; i++ {
		many.WriteString("abc\trefs/heads/b" + strings.Repeat("x", i%7) + "-" + string(rune('a'+i%26)) +
			"-" + strings.Repeat("y", i/26) + "\n")
	}
	if n := len(hostedPushExtraBranchDeletes(many.String(), "refs/heads/main")); n != hostedPushMaxDeletes {
		t.Fatalf("deletes = %d, want the cap %d", n, hostedPushMaxDeletes)
	}
}

func remoteHeads(t *testing.T, remote string) string {
	t.Helper()
	return run(t, ".", "git", "--git-dir="+remote, "for-each-ref", "--sort=refname",
		"--format=%(refname)", "refs/heads", "refs/tags")
}

func TestHostedPushMirrorDefaultBranchOnly(t *testing.T) {
	var app fakeHostedApp
	appServer := httptest.NewServer(&app)
	defer appServer.Close()
	node := hostedTestDaemon(t, "fmnext-n1", appServer.URL)
	remote := filepath.Join(t.TempDir(), "remote.git")
	run(t, ".", "git", "init", "--quiet", "--bare", remote)
	runner := &fakePushRunner{t: t, remote: "file://" + remote}
	node.hosted.runPush = runner.run
	now := time.Unix(1790000000, 0)
	node.hosted.clock = func() time.Time { return now }

	var mu sync.Mutex
	mirror := testPushMirror()
	mirror.Refs = "default"
	app.answer = func(string, map[string]hostedCopyState) hostedReplicaResponse {
		mu.Lock()
		defer mu.Unlock()
		copied := mirror
		item := hostedDesired{ID: testHostedID, Owner: "alice", Name: "proj", Visibility: "public",
			Primary: true, Init: testInit(), PushMirror: &copied}
		return hostedReplicaResponse{Repositories: []hostedDesired{item}}
	}
	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()
	if err := node.hosted.reconcile(ctx); err != nil {
		t.Fatal(err)
	}
	gitDir := node.hosted.gitDir(testHostedID)
	work := filepath.Join(t.TempDir(), "work")
	run(t, ".", "git", "clone", "--quiet", gitDir, work)
	run(t, work, "git", "config", "user.email", "alice@example.test")
	run(t, work, "git", "config", "user.name", "Alice")
	run(t, work, "git", "push", "--quiet", "origin", "main:refs/heads/feature", "main:refs/heads/stale")
	run(t, work, "git", "tag", "v1")
	run(t, work, "git", "push", "--quiet", "origin", "v1")
	run(t, work, "git", "push", "--quiet", remote, "main:refs/heads/only-there")
	run(t, work, "git", "push", "--quiet", "origin", ":refs/heads/stale")

	mu.Lock()
	mirror.Revision = "rev-2"
	mu.Unlock()
	before := runner.count()
	if err := node.hosted.reconcile(ctx); err != nil {
		t.Fatal(err)
	}
	if runner.count() != before+1 {
		t.Fatalf("calls = %d, want one push without prune", runner.count()-before)
	}
	want := []string{"push", "--porcelain", mirror.URL, "+refs/heads/main:refs/heads/main", "+refs/tags/*:refs/tags/*"}
	if got := runner.last().args; strings.Join(got, " ") != strings.Join(want, " ") {
		t.Fatalf("args = %v", got)
	}
	if got := remoteHeads(t, remote); got != "refs/heads/main\nrefs/heads/only-there\nrefs/tags/v1" {
		t.Fatalf("remote refs = %q", got)
	}
	if report := node.hosted.pushMirrorReports()[testHostedID]; !report.OK {
		t.Fatalf("report = %+v", report)
	}

	run(t, work, "git", "commit", "--quiet", "--allow-empty", "-m", "feature work")
	run(t, work, "git", "push", "--quiet", "origin", "HEAD:refs/heads/feature")
	before = runner.count()
	if err := node.hosted.reconcile(ctx); err != nil {
		t.Fatal(err)
	}
	if runner.count() != before {
		t.Fatal("a non-default branch change pushed a default-branch mirror")
	}

	mu.Lock()
	mirror.Revision = "rev-3"
	mirror.Prune = true
	mu.Unlock()
	before = runner.count()
	if err := node.hosted.reconcile(ctx); err != nil {
		t.Fatal(err)
	}
	if runner.count() != before+2 {
		t.Fatalf("calls = %d, want ls-remote and push", runner.count()-before)
	}
	runner.mu.Lock()
	listCall := runner.calls[len(runner.calls)-2]
	runner.mu.Unlock()
	if strings.Join(listCall.args, " ") != "ls-remote --heads "+mirror.URL {
		t.Fatalf("list args = %v", listCall.args)
	}
	if !strings.Contains(strings.Join(listCall.env, "\n"), "GIT_CONFIG_KEY_0=http.extraHeader") {
		t.Fatal("ls-remote ran without the token header")
	}
	want = []string{"push", "--porcelain", "--prune", mirror.URL, "+refs/heads/main:refs/heads/main",
		"+refs/tags/*:refs/tags/*", ":refs/heads/only-there"}
	if got := runner.last().args; strings.Join(got, " ") != strings.Join(want, " ") {
		t.Fatalf("args = %v", got)
	}
	if got := remoteHeads(t, remote); got != "refs/heads/main\nrefs/tags/v1" {
		t.Fatalf("remote refs after prune = %q", got)
	}
	if report := node.hosted.pushMirrorReports()[testHostedID]; !report.OK || report.Refs != 1 {
		t.Fatalf("report = %+v", report)
	}

	mu.Lock()
	mirror.Refs = "all"
	mu.Unlock()
	before = runner.count()
	if err := node.hosted.reconcile(ctx); err != nil {
		t.Fatal(err)
	}
	if runner.count() != before+1 || runner.last().args[4] != "+refs/heads/*:refs/heads/*" {
		t.Fatalf("changing refs did not push every branch: %v", runner.last().args)
	}
	if got := remoteHeads(t, remote); got != "refs/heads/feature\nrefs/heads/main\nrefs/tags/v1" {
		t.Fatalf("remote refs with all = %q", got)
	}
	mu.Lock()
	mirror.Refs = "default"
	mu.Unlock()
	if err := node.hosted.reconcile(ctx); err != nil {
		t.Fatal(err)
	}
	if got := remoteHeads(t, remote); got != "refs/heads/main\nrefs/tags/v1" {
		t.Fatalf("remote refs back to default = %q", got)
	}

	runner.mu.Lock()
	runner.fail = errors.New("exit status 128")
	runner.mu.Unlock()
	mu.Lock()
	mirror.Revision = "rev-4"
	mu.Unlock()
	before = runner.count()
	if err := node.hosted.reconcile(ctx); err != nil {
		t.Fatal(err)
	}
	failed := node.hosted.pushMirrorReports()[testHostedID]
	if runner.count() != before+1 || runner.last().args[0] != "ls-remote" || failed.OK ||
		strings.Contains(failed.Error, testPushToken) || !strings.Contains(failed.Error, "***") {
		t.Fatalf("failed list: calls %d report %+v", runner.count()-before, failed)
	}
}
