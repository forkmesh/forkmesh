package mirrornode

import (
	"context"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
)

func TestPullRevisionReplacesThePatchAndRebuildsTheHead(t *testing.T) {
	ctx := context.Background()
	source, bare := makeBareRepo(t)
	run(t, source, "git", "checkout", "-q", "-b", "contrib")
	writeFile(t, filepath.Join(source, "contrib.txt"), "first\n")
	run(t, source, "git", "add", "contrib.txt")
	run(t, source, "git", "commit", "-q", "-m", "first try")
	firstMbox := run(t, source, "git", "format-patch", "--stdout", "main..contrib")
	firstPatch := run(t, source, "git", "diff", "--binary", "main", "contrib")

	repo := Repository{Owner: "forkmesh", Name: "forkmesh", GitDir: bare}
	number, err := materializeDirectCollaboration(ctx, repo, "pulls", collaborationItem{
		Number: 3,
		Pull: map[string]any{
			"title": "Contribute", "base": "main", "head": "carol:contrib",
			"patch": firstPatch + "\n", "commits": firstMbox + "\n", "author": "carol",
			"authorName": "Carol", "ts": 1700000005000, "sig": "sig-one",
		},
	}, "mirror9")
	if err != nil {
		t.Fatal(err)
	}
	firstHead := run(t, ".", "git", "--git-dir="+bare, "rev-parse", "--verify", pullHeadRef(number))

	run(t, source, "git", "commit", "-q", "--amend", "-m", "second try, rewritten")
	writeFile(t, filepath.Join(source, "contrib.txt"), "second\n")
	run(t, source, "git", "commit", "-q", "-am", "follow-up")
	secondMbox := run(t, source, "git", "format-patch", "--stdout", "main..contrib")
	secondPatch := run(t, source, "git", "diff", "--binary", "main", "contrib")
	revision := map[string]any{
		"title": "Contribute (v2)", "base": "main", "head": "carol:contrib-2",
		"patch": secondPatch + "\n", "commits": secondMbox + "\n", "author": "carol",
		"authorName": "Carol", "ts": 1700000009000, "sig": "sig-two",
		"revisionSig": "binding-two",
	}
	if _, err := materializeDirectCollaboration(ctx, repo, "pulls", collaborationItem{
		Number: number, Revision: revision}, "mirror9"); err != nil {
		t.Fatal(err)
	}

	show := func(path string) string {
		return run(t, ".", "git", "--git-dir="+bare, "show",
			"refs/heads/forkmesh/pulls:.forkmesh/pulls/"+strconv.Itoa(number)+"/"+path)
	}
	record := show("pull.md")
	for _, want := range []string{"\nsig: sig-two\n", "\nts: 1700000009000\n", "\nrevision: 2\n",
		"\nhead: carol:contrib-2\n", "\ntitle: Contribute (v2)\n", "\nauthor: carol\n"} {
		if !strings.Contains(record, want) {
			t.Fatalf("revised record lacks %q:\n%s", strings.TrimSpace(want), record)
		}
	}
	if !strings.Contains(show("revisions/1/pull.md"), "\nsig: sig-one\n") {
		t.Fatal("the first revision was not archived")
	}
	if show("changes.patch") != strings.TrimSpace(secondPatch) {
		t.Fatal("the patch was not replaced")
	}
	secondHead := run(t, ".", "git", "--git-dir="+bare, "rev-parse", "--verify", pullHeadRef(number))
	if secondHead == firstHead || !strings.Contains(record, "\nheadOid: "+secondHead+"\n") {
		t.Fatalf("head not rebuilt: first %s second %s\n%s", firstHead, secondHead, record)
	}
	if run(t, ".", "git", "--git-dir="+bare, "show", secondHead+":contrib.txt") != "second" {
		t.Fatal("the rebuilt head lacks the new change")
	}
	listing := run(t, ".", "git", "--git-dir="+bare, "ls-tree", "--name-only",
		"refs/heads/forkmesh/pulls:.forkmesh/pulls/"+strconv.Itoa(number))
	if !strings.Contains(listing, "-revision.md") {
		t.Fatalf("no revision event in the timeline:\n%s", listing)
	}
}

func TestPullRevisionIgnoresStaleForeignAndClosedRevisions(t *testing.T) {
	cases := map[string]struct {
		status string
		change map[string]any
	}{
		"stale":   {"open", map[string]any{"ts": 1}},
		"foreign": {"open", map[string]any{"author": "mallory"}},
		"closed":  {"closed", map[string]any{}},
	}
	for name, tc := range cases {
		t.Run(name, func(t *testing.T) {
			root, pullDir := writeLifecyclePull(t, tc.status)
			revision := map[string]any{"title": "x", "base": "main", "head": "y",
				"patch": "new diff", "author": "authorkey", "ts": 5, "sig": "s2",
				"revisionSig": "b2"}
			for key, value := range tc.change {
				revision[key] = value
			}
			wrote, number, err := writePullItem(root, collaborationItem{Number: 4, Revision: revision})
			if err != nil || !wrote || number != 4 {
				t.Fatalf("wrote=%v number=%d err=%v", wrote, number, err)
			}
			if lifecycleRecord(t, pullDir).values["sig"] != "s" {
				t.Fatal("an inapplicable revision changed the record")
			}
			if _, err := os.Stat(filepath.Join(pullDir, "revisions")); !os.IsNotExist(err) {
				t.Fatal("an inapplicable revision archived the record")
			}
		})
	}
}

func TestPullRevisionWithoutCommitsDropsTheOldMbox(t *testing.T) {
	root, pullDir := writeLifecyclePull(t, "open")
	revision := map[string]any{"title": "Fix", "base": "main", "head": "fix-2",
		"patch": "new diff", "author": "authorkey", "ts": 9, "sig": "s2", "revisionSig": "b2"}
	if _, _, err := writePullItem(root, collaborationItem{Number: 4, Revision: revision}); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(filepath.Join(pullDir, "commits.mbox")); !os.IsNotExist(err) {
		t.Fatal("a stale commits.mbox would be replayed over the new patch")
	}
	again, _, err := writePullItem(root, collaborationItem{Number: 4, Revision: revision})
	if err != nil || !again {
		t.Fatal("a replayed revision must be acknowledged")
	}
	entries, _ := os.ReadDir(filepath.Join(pullDir, "revisions"))
	if len(entries) != 1 {
		t.Fatalf("a replay archived again: %d archives", len(entries))
	}
}
