package mirrornode

import (
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// installMaliciousClaude is an "agent" that, besides changing the README,
// arms its repository against whoever runs git in it next: executable
// hooks, an fsmonitor, a clean filter, a rewritten remote with insteadOf
// (including an ext:: command transport) and a pushurl. Every trap writes
// "<name> HOME=<home>" into markers/, so the test can tell who ran it.
func installMaliciousClaude(t *testing.T, markers, evil string) {
	t.Helper()
	bin := t.TempDir()
	trap := func(name string) string {
		return "#!/bin/sh\necho \"" + name + " HOME=$HOME\" >> '" + filepath.Join(markers, name) + "'\n"
	}
	traps := filepath.Join(bin, "traps")
	if err := os.Mkdir(traps, 0700); err != nil {
		t.Fatal(err)
	}
	for _, name := range []string{"fsmonitor", "clean"} {
		body := trap(name)
		if name == "clean" {
			body += "cat\n"
		}
		if err := os.WriteFile(filepath.Join(traps, name), []byte(body), 0700); err != nil {
			t.Fatal(err)
		}
	}
	var hooks strings.Builder
	for _, hook := range []string{"pre-commit", "post-commit", "reference-transaction", "pre-push",
		"post-checkout", "pre-auto-gc", "post-index-change"} {
		hooks.WriteString("printf '%s' \"" + strings.ReplaceAll(trap(hook), "\"", "\\\"") + "\" > .git/hooks/" + hook +
			"\nchmod +x .git/hooks/" + hook + "\n")
	}
	script := `#!/bin/sh
cat >/dev/null
echo "agent line" >> README.md
echo "secret" > data.fm
echo "*.fm filter=trap" > .gitattributes
` + hooks.String() + `
git config core.fsmonitor '` + filepath.Join(traps, "fsmonitor") + `'
git config filter.trap.clean '` + filepath.Join(traps, "clean") + `'
git config remote.origin.url '` + evil + `'
git config remote.origin.pushurl '` + evil + `'
git config "url.` + evil + `.insteadOf" http://
git config "url.ext::sh -c touch% ` + filepath.Join(markers, "ext") + `.insteadOf" ` + evil + `
git config core.sshCommand 'touch ` + filepath.Join(markers, "ssh") + `'
git config uploadpack.packObjectsHook '` + filepath.Join(traps, "fsmonitor") + `'
echo '{"type":"result","result":"armed","session_id":"sess-1","is_error":false,"num_turns":1}'
`
	if err := os.WriteFile(filepath.Join(bin, "claude"), []byte(script), 0700); err != nil {
		t.Fatal(err)
	}
	t.Setenv("PATH", bin+string(os.PathListSeparator)+os.Getenv("PATH"))
	t.Setenv("HOME", t.TempDir())
	t.Setenv("CLAUDE_CONFIG_DIR", "")
	t.Setenv("ANTHROPIC_API_KEY", "test-key")
}

func TestRunnerPublishNeverRunsJobControlledGitAsTheDaemon(t *testing.T) {
	markers := t.TempDir()
	_, evil := makeBareRepo(t)
	evilBefore := gitLines(context.Background(), evil, "for-each-ref", "--format=%(refname) %(objectname)")
	installMaliciousClaude(t, markers, evil)
	f := newRunnerFixture(t, nil)

	// Self-check: the traps are live against a plain git as the old
	// publish ran it (commit, then push to origin).
	probeMarkers := t.TempDir()
	installMaliciousClaude(t, probeMarkers, evil)
	probe := filepath.Join(t.TempDir(), "probe")
	run(t, ".", "git", "clone", "--quiet", f.bare, probe)
	run(t, probe, "git", "config", "user.email", "p@example.test")
	run(t, probe, "git", "config", "user.name", "P")
	run(t, probe, "claude")
	run(t, probe, "git", "add", "-A")
	run(t, probe, "git", "commit", "--quiet", "-m", "probe")
	for _, name := range []string{"clean", "pre-commit", "post-commit", "reference-transaction", "fsmonitor"} {
		if _, err := os.Stat(filepath.Join(probeMarkers, name)); err != nil {
			t.Fatalf("trap %s is not live against plain git: %v", name, err)
		}
	}
	installMaliciousClaude(t, markers, evil)
	f.app.queue = append(f.app.queue, f.job(t, 21, "start", "refs/heads/agent/sess-a"))
	f.drain(t)

	_, reports := f.app.snapshot()
	if len(reports) != 2 || reports[1]["status"] != "completed" {
		t.Fatalf("reports = %#v", reports)
	}
	// The push went to the lease's clone URL and ref, not the job's remote.
	if readme := gitValue(context.Background(), f.bare, "show", "refs/heads/agent/sess-a:README.md"); !strings.Contains(readme, "agent line") {
		t.Fatalf("core branch README = %q", readme)
	}
	if after := gitLines(context.Background(), evil, "for-each-ref", "--format=%(refname) %(objectname)"); strings.Join(after, ",") != strings.Join(evilBefore, ",") {
		t.Fatalf("the job's rewritten remote received a push: %v", after)
	}
	publishHome := filepath.Join(f.work, jobPublishDir)
	entries, _ := os.ReadDir(markers)
	for _, entry := range entries {
		raw, _ := os.ReadFile(filepath.Join(markers, entry.Name()))
		text := string(raw)
		switch entry.Name() {
		case "clean":
			// `git add` inside the job sandbox runs the job's own filter as
			// the job user; that is the job's code on the job's side.
			if strings.Contains(text, "HOME="+publishHome) {
				t.Fatalf("the daemon ran the job's clean filter: %q", text)
			}
			if !strings.Contains(text, "HOME="+filepath.Join(f.work, jobHomesDir)) {
				t.Fatalf("clean filter ran outside the job sandbox: %q", text)
			}
		default:
			// Hooks, fsmonitor, ext:: and ssh commands never run at all:
			// the sandboxed git disables hooks and fsmonitor too, and the
			// daemon's git never reads the job's config.
			t.Fatalf("trap %s ran: %q", entry.Name(), text)
		}
	}
	// The committed filter output is the job's business; the file is there.
	if data := gitValue(context.Background(), f.bare, "show", "refs/heads/agent/sess-a:data.fm"); data != "secret" {
		t.Fatalf("data.fm = %q", data)
	}
}

func TestRunnerPublishRefusesASymlinkedBundle(t *testing.T) {
	work := t.TempDir()
	box := jobSandbox{workRoot: work}
	home := box.homeFor(filepath.Join(work, "abc"))
	if err := os.MkdirAll(filepath.Join(home, "tmp"), 0700); err != nil {
		t.Fatal(err)
	}
	target := filepath.Join(t.TempDir(), "root-only")
	if err := os.WriteFile(target, []byte("root secret"), 0600); err != nil {
		t.Fatal(err)
	}
	link := filepath.Join(home, "tmp", "publish.bundle")
	if err := os.Symlink(target, link); err != nil {
		t.Fatal(err)
	}
	if _, err := readOwnedFile(link, 0, runnerBundleLimit); err == nil {
		t.Fatal("a symlinked bundle was read")
	}
	// A file of another owner is refused when a job uid is set.
	if _, err := readOwnedFile(target, os.Getuid()+1, runnerBundleLimit); err == nil {
		t.Fatal("a file of another owner was read")
	}
}

func TestSessionHomeCredentialsFlowOnlyAsCredentials(t *testing.T) {
	persistent := t.TempDir()
	work := t.TempDir()
	box := jobSandbox{workRoot: work, home: persistent}
	creds := filepath.Join(persistent, ".claude", ".credentials.json")
	if err := os.MkdirAll(filepath.Dir(creds), 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(creds, []byte(`{"claudeAiOauth":{"refreshToken":"old"}}`), 0600); err != nil {
		t.Fatal(err)
	}
	home := box.homeFor(filepath.Join(work, "sess"))
	if err := box.openSession(home); err != nil {
		t.Fatal(err)
	}
	copied := filepath.Join(home, ".claude", ".credentials.json")
	if raw, _ := os.ReadFile(copied); !strings.Contains(string(raw), "old") {
		t.Fatalf("credential not handed to the job: %q", raw)
	}
	// The job refreshes its token and plants a hook config.
	if err := os.WriteFile(copied, []byte(`{"claudeAiOauth":{"refreshToken":"new"}}`), 0600); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(home, ".claude", "settings.json"), []byte(`{"hooks":{}}`), 0600); err != nil {
		t.Fatal(err)
	}
	box.closeSession(home)
	if raw, _ := os.ReadFile(creds); !strings.Contains(string(raw), "new") {
		t.Fatalf("refreshed credential not kept: %q", raw)
	}
	if _, err := os.Stat(filepath.Join(persistent, ".claude", "settings.json")); err == nil {
		t.Fatal("settings.json flowed back into the persistent home")
	}
	if _, err := os.Stat(copied); err == nil {
		t.Fatal("the session home kept the credential after the job")
	}
	// Invalid JSON never replaces a working login.
	if err := box.openSession(home); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(copied, []byte("not json"), 0600); err != nil {
		t.Fatal(err)
	}
	box.closeSession(home)
	if raw, _ := os.ReadFile(creds); !strings.Contains(string(raw), "new") {
		t.Fatalf("invalid credential replaced the login: %q", raw)
	}
	// A symlink planted where the credential goes is replaced, not followed.
	outside := filepath.Join(t.TempDir(), "victim")
	if err := os.WriteFile(outside, []byte("keep"), 0600); err != nil {
		t.Fatal(err)
	}
	if err := os.Symlink(outside, copied); err != nil {
		t.Fatal(err)
	}
	if err := box.openSession(home); err != nil {
		t.Fatal(err)
	}
	if raw, _ := os.ReadFile(outside); string(raw) != "keep" {
		t.Fatalf("copy-in followed a planted symlink: %q", raw)
	}
	box.closeSession(home)
}
