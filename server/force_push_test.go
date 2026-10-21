package mirrornode

import (
	"context"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func commitFile(t *testing.T, source, name string) string {
	t.Helper()
	if err := os.WriteFile(filepath.Join(source, name), []byte(name+"\n"), 0600); err != nil {
		t.Fatal(err)
	}
	run(t, source, "git", "add", name)
	run(t, source, "git", "commit", "-m", name)
	return run(t, source, "git", "rev-parse", "HEAD")
}

func refValue(t *testing.T, bare, ref string) string {
	t.Helper()
	return gitRefValue(context.Background(), bare, ref)
}

func stageRewoundFeature(t *testing.T) (string, string, string) {
	t.Helper()
	source, bare := makeBareRepo(t)
	run(t, source, "git", "checkout", "-q", "-b", "feature")
	base := commitFile(t, source, "a.txt")
	old := commitFile(t, source, "b.txt")
	run(t, source, "git", "push", bare, "feature")
	run(t, source, "git", "reset", "-q", "--hard", base)
	run(t, source, "git", "push", bare, "+feature:refs/forkmesh/upstream/heads/feature")
	return bare, old, base
}

func TestARewoundBranchIsKeptWithoutAnAdmittedForcedTip(t *testing.T) {
	bare, old, base := stageRewoundFeature(t)
	adoptForcedBranches(context.Background(), bare, nil)
	adoptForcedBranches(context.Background(), bare, map[string]string{"refs/heads/feature": strings.Repeat("a", 40)})
	if err := reconcileUpstreamRefs(context.Background(), bare); err != nil {
		t.Fatal(err)
	}
	if got := refValue(t, bare, "refs/heads/feature"); got != old || got == base {
		t.Fatalf("feature moved to %s without an admitted forced tip", got)
	}
}

func TestAnAdmittedForcedTipIsAdoptedAndTheOldTipKept(t *testing.T) {
	bare, old, base := stageRewoundFeature(t)
	adoptForcedBranches(context.Background(), bare, map[string]string{"refs/heads/feature": base})
	if err := reconcileUpstreamRefs(context.Background(), bare); err != nil {
		t.Fatal(err)
	}
	if got := refValue(t, bare, "refs/heads/feature"); got != base {
		t.Fatalf("feature is %s, want the admitted %s", got, base)
	}
	if got := refValue(t, bare, "refs/forkmesh/history/feature/0000000001"); got != old {
		t.Fatalf("old tip kept as %q, want %s", got, old)
	}
}

func TestAForcedTipNeverMovesTheDefaultOrLedgerBranches(t *testing.T) {
	source, bare := makeBareRepo(t)
	main := refValue(t, bare, "refs/heads/main")
	run(t, source, "git", "checkout", "-q", "--orphan", "other")
	other := commitFile(t, source, "other.txt")
	run(t, source, "git", "push", bare, "other:refs/forkmesh/upstream/heads/main")
	adoptForcedBranches(context.Background(), bare, map[string]string{"refs/heads/main": other})
	if got := refValue(t, bare, "refs/heads/main"); got != main {
		t.Fatalf("default branch moved to %s", got)
	}
}

func TestAStalePeerCannotBringBackASupersededTip(t *testing.T) {
	source, bare := makeBareRepo(t)
	run(t, source, "git", "checkout", "-q", "-b", "feature")
	base := commitFile(t, source, "a.txt")
	old := commitFile(t, source, "b.txt")
	run(t, source, "git", "push", bare, "+"+base+":refs/heads/feature")
	run(t, source, "git", "push", bare, "+"+old+":refs/forkmesh/upstream/heads/feature")
	run(t, ".", "git", "--git-dir="+bare, "update-ref", "refs/forkmesh/history/feature/0000000001", old)
	if err := reconcileUpstreamRefs(context.Background(), bare); err != nil {
		t.Fatal(err)
	}
	if got := refValue(t, bare, "refs/heads/feature"); got != base {
		t.Fatalf("a stale peer moved feature back to %s", got)
	}
}

func TestSupersededTipsArePrunedToTheNewestKept(t *testing.T) {
	source, bare := makeBareRepo(t)
	ctx := context.Background()
	tips := []string{}
	for index := 0; index < pushHistoryKeep+2; index++ {
		tips = append(tips, commitFile(t, source, fmt.Sprintf("f%02d.txt", index)))
	}
	run(t, source, "git", "push", bare, "main")
	for _, tip := range tips {
		if err := keepSupersededTip(ctx, bare, "refs/heads/feature", tip); err != nil {
			t.Fatal(err)
		}
	}
	kept, err := gitRefValues(ctx, bare, "refs/forkmesh/history/feature")
	if err != nil || len(kept) != pushHistoryKeep {
		t.Fatalf("kept %d tips (%v), want %d", len(kept), err, pushHistoryKeep)
	}
	if kept["refs/forkmesh/history/feature/0000000001"] != "" {
		t.Fatal("the oldest tip was not pruned")
	}
	last := fmt.Sprintf("refs/forkmesh/history/feature/%010d", pushHistoryKeep+2)
	if kept[last] != tips[len(tips)-1] {
		t.Fatalf("newest kept tip is %q", kept[last])
	}
}

func TestForcePushPullEventKeepsItsCommits(t *testing.T) {
	text := pullEventMarkdown(map[string]any{
		"type": "force-push", "id": "force-push-1", "author": "bob", "ts": 1,
		"ref": "refs/heads/feature", "before": strings.Repeat("a", 40),
		"after": strings.Repeat("b", 40), "body": "force-pushed", "sig": "",
	})
	for _, line := range []string{"ref: refs/heads/feature", "before: " + strings.Repeat("a", 40), "after: " + strings.Repeat("b", 40)} {
		if !strings.Contains(text, "\n"+line+"\n") {
			t.Fatalf("missing %q in %s", line, text)
		}
	}
}
