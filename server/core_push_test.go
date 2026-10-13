package mirrornode

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func stageMovedTag(t *testing.T) (string, string, string) {
	t.Helper()
	source, bare := makeBareRepo(t)
	run(t, source, "git", "tag", "-a", "v1", "-m", "first")
	run(t, source, "git", "push", bare, "refs/tags/v1")
	old := run(t, ".", "git", "--git-dir="+bare, "rev-parse", "refs/tags/v1")
	if err := os.WriteFile(filepath.Join(source, "later.txt"), []byte("later\n"), 0600); err != nil {
		t.Fatal(err)
	}
	run(t, source, "git", "add", "later.txt")
	run(t, source, "git", "commit", "-m", "later")
	run(t, source, "git", "tag", "-f", "-a", "v1", "-m", "moved")
	head := run(t, source, "git", "rev-parse", "main")
	moved := run(t, source, "git", "rev-parse", "refs/tags/v1")
	run(t, source, "git", "push", bare, "+main:refs/forkmesh/upstream/heads/main",
		"+refs/tags/v1:refs/forkmesh/upstream/tags/v1")
	digest := sha256.Sum256([]byte(head + " refs/heads/main\n" + moved + " refs/tags/v1"))
	if old == moved {
		t.Fatal("tag did not move")
	}
	return bare, old, hex.EncodeToString(digest[:])
}

func TestUpstreamTagMoveIsIgnoredWithoutAnAdmittedState(t *testing.T) {
	bare, old, admitted := stageMovedTag(t)
	if err := reconcileUpstreamRefs(context.Background(), bare, strings.Repeat("0", len(admitted))); err != nil {
		t.Fatal(err)
	}
	if got := run(t, ".", "git", "--git-dir="+bare, "rev-parse", "refs/tags/v1"); got != old {
		t.Fatalf("tag moved to %s without an admitted state", got)
	}
	if err := reconcileUpstreamRefs(context.Background(), bare); err != nil {
		t.Fatal(err)
	}
	if got := run(t, ".", "git", "--git-dir="+bare, "rev-parse", "refs/tags/v1"); got != old {
		t.Fatalf("tag moved to %s with no desired state", got)
	}
}

func TestUpstreamTagMoveIsAdoptedWhenItYieldsTheAdmittedState(t *testing.T) {
	bare, old, admitted := stageMovedTag(t)
	if err := reconcileUpstreamRefs(context.Background(), bare, admitted); err != nil {
		t.Fatal(err)
	}
	got := run(t, ".", "git", "--git-dir="+bare, "rev-parse", "refs/tags/v1")
	if got == old {
		t.Fatal("admitted tag move was not adopted")
	}
	digest, err := routingRefsSHA256(context.Background(), bare)
	if err != nil || digest != admitted {
		t.Fatalf("routing digest %s, want the admitted %s (%v)", digest, admitted, err)
	}
	staged := run(t, ".", "git", "--git-dir="+bare, "for-each-ref", "refs/forkmesh/upstream")
	if staged != "" {
		t.Fatalf("staged refs were left behind: %s", staged)
	}
}

func TestTagMoveIsAdoptedFromAnUpstreamServingTheAdmittedStateBehindLocalMain(t *testing.T) {
	bare, old, admitted := stageMovedTag(t)
	upstreamHead := run(t, ".", "git", "--git-dir="+bare, "rev-parse", "refs/forkmesh/upstream/heads/main")
	source := filepath.Join(filepath.Dir(bare), "source")
	if err := os.WriteFile(filepath.Join(source, "ahead.txt"), []byte("ahead\n"), 0600); err != nil {
		t.Fatal(err)
	}
	run(t, source, "git", "add", "ahead.txt")
	run(t, source, "git", "commit", "-m", "ahead")
	run(t, source, "git", "push", bare, "+main:refs/heads/main")
	ahead := run(t, ".", "git", "--git-dir="+bare, "rev-parse", "refs/heads/main")
	if ahead == upstreamHead {
		t.Fatal("local main is not ahead")
	}
	if err := reconcileUpstreamRefs(context.Background(), bare, admitted); err != nil {
		t.Fatal(err)
	}
	if got := run(t, ".", "git", "--git-dir="+bare, "rev-parse", "refs/tags/v1"); got == old {
		t.Fatal("tag served by an admitted upstream was not adopted")
	}
	if got := run(t, ".", "git", "--git-dir="+bare, "rev-parse", "refs/heads/main"); got != ahead {
		t.Fatalf("local main moved to %s", got)
	}
}
