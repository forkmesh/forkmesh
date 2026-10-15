package actions

import (
	"encoding/json"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
	"time"

	"forkmesh.com/desktop-core/internal/pulls"
)

const pullWorkflow = "name: CI\non: [push, pull_request]\nsteps:\n  - name: Hello\n    run: echo pr-check\n"

func setGitIdentity(t *testing.T) {
	t.Setenv("GIT_AUTHOR_NAME", "Test")
	t.Setenv("GIT_AUTHOR_EMAIL", "t@example.test")
	t.Setenv("GIT_COMMITTER_NAME", "Test")
	t.Setenv("GIT_COMMITTER_EMAIL", "t@example.test")
	t.Setenv("GIT_CONFIG_NOSYSTEM", "1")
	t.Setenv("GIT_CONFIG_GLOBAL", "/dev/null")
}

func writePullRecord(t *testing.T, meta string, number int, fields map[string]string) {
	t.Helper()
	dir := filepath.Join(meta, ".forkmesh", "pulls", strconv.Itoa(number))
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	text := "---\nschema: forkmesh-pull-v1\nnumber: " + strconv.Itoa(number) + "\n"
	for _, key := range []string{"title", "base", "head", "status", "derive", "author", "authorName", "sig"} {
		if value, ok := fields[key]; ok {
			text += key + ": " + value + "\n"
		}
	}
	text += "---\n\nbody\n"
	if err := os.WriteFile(filepath.Join(dir, "pull.md"), []byte(text), 0o644); err != nil {
		t.Fatal(err)
	}
	if mbox, ok := fields["mbox"]; ok {
		if err := os.WriteFile(filepath.Join(dir, "commits.mbox"), []byte(mbox), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	runGit(t, meta, "add", "--all")
	runGit(t, meta, "commit", "-q", "-m", "pull #"+strconv.Itoa(number))
}

func (qf *queueFixture) waitLog(t *testing.T, want string) {
	t.Helper()
	deadline := time.Now().Add(60 * time.Second)
	for time.Now().Before(deadline) {
		if strings.Contains(qf.logText(), want) {
			return
		}
		time.Sleep(50 * time.Millisecond)
	}
	t.Fatalf("missing %q in\n%s", want, qf.logText())
}

func gitOut(dir string, args ...string) string {
	out, _ := exec.Command("git", append([]string{"-C", dir}, args...)...).Output()
	return strings.TrimSpace(string(out))
}

func TestPullChecksResolveAndPublishHeadsInTheCore(t *testing.T) {
	setGitIdentity(t)
	qf := newQueueFixture(t, pullWorkflow, nil)
	data := t.TempDir()
	qf.module.PullsDataRoot = data
	runGit(t, qf.work, "checkout", "-q", "-b", "feature")
	_ = os.WriteFile(filepath.Join(qf.work, "README.md"), []byte("feature\n"), 0o644)
	runGit(t, qf.work, "commit", "-q", "-am", "feature change")
	featureHead := runGit(t, qf.work, "rev-parse", "HEAD")
	runGit(t, qf.work, "checkout", "-q", "main")
	runGit(t, qf.work, "checkout", "-q", "-b", "contrib")
	_ = os.WriteFile(filepath.Join(qf.work, "NEW.md"), []byte("from a patch\n"), 0o644)
	runGit(t, qf.work, "add", "NEW.md")
	runGit(t, qf.work, "commit", "-q", "-m", "patch change")
	mbox := runGit(t, qf.work, "format-patch", "--stdout", "main..contrib") + "\n"
	runGit(t, qf.work, "checkout", "-q", "main")
	runGit(t, qf.work, "branch", "-q", "-D", "contrib")

	store := &pulls.Store{WorkTree: qf.work, IdentityValid: true, DataRoot: data}
	meta := store.MetaWorkTree()
	if meta == "" {
		t.Fatal("no pull ledger checkout")
	}
	writePullRecord(t, meta, 1, map[string]string{"title": "Feature", "base": "main", "head": "feature", "status": "open",
		"derive": "branch", "author": "STRANGERKEY", "authorName": "Mallory", "sig": "s"})
	writePullRecord(t, meta, 2, map[string]string{"title": "Patch", "base": "main", "head": "contrib", "status": "open",
		"author": "OWNERKEY", "authorName": "Owner", "sig": "s", "mbox": mbox})
	writePullRecord(t, meta, 3, map[string]string{"title": "Closed", "base": "main", "head": "gone", "status": "closed",
		"author": "X", "sig": "s"})

	qf.mustCall(qf.ui, "core.actions.queue.attach", qf.snapshot(true))
	params := map[string]any{"owner": "owner", "name": "repo", "trusted": []string{"OWNERKEY"}, "identity": true}
	if _, err := qf.call(qf.phone, "core.actions.queue.pullChecks", params); err == nil {
		t.Fatal("a phone reached core.actions.queue.pullChecks")
	}
	reply := qf.mustCall(qf.ui, "core.actions.queue.pullChecks", params)
	if reply["accepted"] != true {
		t.Fatal(reply)
	}
	qf.waitLog(t, "(pull request #1)")
	qf.waitLog(t, "(pull request #2)")
	if got := gitOut(qf.mirror, "rev-parse", "refs/heads/pr/1"); got != featureHead {
		t.Fatalf("pr/1 in the mirror is %q, want %s", got, featureHead)
	}
	patchHead := gitOut(qf.mirror, "rev-parse", "refs/heads/pr/2")
	if patchHead == "" || gitOut(qf.mirror, "show", "-s", "--format=%s", patchHead) != "patch change" {
		t.Fatalf("the signed patch was not replayed and published: %q", patchHead)
	}
	pullRuns := map[int]Run{}
	deadline := time.Now().Add(30 * time.Second)
	for time.Now().Before(deadline) && len(pullRuns) < 2 {
		select {
		case event := <-qf.ui.Events:
			if event.Name != "actions.run" {
				continue
			}
			var data struct {
				Created bool           `json:"created"`
				Record  map[string]any `json:"record"`
			}
			if json.Unmarshal(event.Data, &data) == nil && data.Created {
				if run := RunFromObject(data.Record); run.PullNumber > 0 {
					pullRuns[run.PullNumber] = run
				}
			}
		case <-time.After(100 * time.Millisecond):
		}
	}
	first := pullRuns[1]
	if first.Commit != featureHead || first.Ref != "refs/heads/pr/1" || !first.FirstTimePullAuthor || first.PullAuthor != "Mallory" {
		t.Fatalf("pull #1 run %+v", first)
	}
	if second := pullRuns[2]; second.FirstTimePullAuthor || second.Commit != patchHead {
		t.Fatalf("pull #2 run %+v", second)
	}
	if strings.Contains(qf.logText(), "#3") {
		t.Fatal("a closed pull was checked")
	}
	before := strings.Count(qf.logText(), "(pull request #1)")
	again := qf.mustCall(qf.ui, "core.actions.queue.pullChecks", params)
	if again["accepted"] != true {
		t.Fatal(again)
	}
	time.Sleep(500 * time.Millisecond)
	if strings.Count(qf.logText(), "(pull request #1)") != before {
		t.Fatal("an unchanged pull list was checked again inside the rescan floor")
	}
	manual := map[string]any{"owner": "owner", "name": "repo", "trusted": []string{"OWNERKEY"}, "identity": true,
		"force": true, "numbers": []int{1}, "trigger": "pull_request_manual"}
	qf.mustCall(qf.ui, "core.actions.queue.pullChecks", manual)
	deadline = time.Now().Add(30 * time.Second)
	for time.Now().Before(deadline) && strings.Count(qf.logText(), "(pull request #1)") == before {
		time.Sleep(50 * time.Millisecond)
	}
	if strings.Count(qf.logText(), "(pull request #1)") == before {
		t.Fatalf("the maintainer's run-checks request queued nothing:\n%s", qf.logText())
	}
	if _, err := qf.call(qf.ui, "core.actions.queue.pullChecks", map[string]any{"owner": "owner", "name": "none"}); err == nil {
		t.Fatal("an unknown repository was accepted")
	}
}

func TestPullAuthorTrust(t *testing.T) {
	history := []*pulls.Pull{
		{Number: 1, Status: "merged", Author: "KEY-A"},
		{Number: 2, Status: "open", Author: "KEY-B"},
		{Number: 3, Status: "merged", AuthorName: "Local Name"},
	}
	cases := []struct {
		pr      *pulls.Pull
		trusted []string
		want    bool
	}{
		{&pulls.Pull{Number: 9, Author: "KEY-A"}, nil, true},
		{&pulls.Pull{Number: 2, Author: "KEY-B"}, nil, false},
		{&pulls.Pull{Number: 9, Author: "KEY-C", AuthorName: "owner"}, []string{"owner"}, false},
		{&pulls.Pull{Number: 9, AuthorName: "OWNER"}, []string{"owner"}, true},
		{&pulls.Pull{Number: 9, Author: "NODE"}, []string{"NODE"}, true},
		{&pulls.Pull{Number: 9, AuthorName: "local name"}, nil, true},
		{&pulls.Pull{Number: 9}, []string{""}, false},
	}
	for index, c := range cases {
		if got := PullAuthorEstablished(c.pr, history, c.trusted); got != c.want {
			t.Fatalf("case %d: got %v", index, got)
		}
	}
	candidate := newPullCheckCandidate(&pulls.Pull{Number: 4, Head: " fork:topic ", Base: " main ", Author: "ABCDEFGHIJKLMN"}, nil, nil)
	if candidate.head != "topic" || candidate.base != "main" || candidate.author != "ABCDEFGHIJ" || !candidate.firstTimeAuthor {
		t.Fatalf("candidate %+v", candidate)
	}
	if refs := refsNamedAsBadObjects("error: bad object refs/heads/x\nfatal: bad object refs/heads/x 'y'\nbad object refs/a..b"); len(refs) != 1 || refs[0] != "refs/heads/x" {
		t.Fatal(refs)
	}
}
