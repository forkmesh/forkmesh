package mirrornode

import (
	"context"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"sync/atomic"
	"testing"
)

func redirectingUpstream(t *testing.T) (string, *int64) {
	t.Helper()
	var hits int64
	outside := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		atomic.AddInt64(&hits, 1)
		http.NotFound(w, r)
	}))
	t.Cleanup(outside.Close)
	public := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		target := outside.URL + r.URL.Path
		if r.URL.RawQuery != "" {
			target += "?" + r.URL.RawQuery
		}
		http.Redirect(w, r, target, http.StatusTemporaryRedirect)
	}))
	t.Cleanup(public.Close)
	t.Setenv("GIT_SSL_NO_VERIFY", "true")
	return public.URL + "/forkmesh/forkmesh", &hits
}

func TestCoreSyncNeverFollowsARedirectToAnotherHost(t *testing.T) {
	upstream, hits := redirectingUpstream(t)
	source, bare := makeBareRepo(t)
	if err := os.WriteFile(filepath.Join(source, "README.md"), []byte("peer\n"), 0600); err != nil {
		t.Fatal(err)
	}
	run(t, source, "git", "add", "README.md")
	run(t, source, "git", "commit", "-m", "peer")
	repo := Repository{GitDir: bare}
	if err := syncCoreRepository(context.Background(), repo, []string{upstream, source}, ""); err != nil {
		t.Fatal(err)
	}
	if got := atomic.LoadInt64(hits); got != 0 {
		t.Fatalf("core sync followed the redirect %d times", got)
	}
	if got, want := run(t, ".", "git", "--git-dir="+bare, "rev-parse", "main"), run(t, source, "git", "rev-parse", "main"); got != want {
		t.Fatalf("got %s want %s from the listed peer", got, want)
	}
	if err := syncCoreRepository(context.Background(), repo, []string{upstream}, ""); err == nil {
		t.Fatal("a redirecting upstream must fail the core sync")
	}
	if got := atomic.LoadInt64(hits); got != 0 {
		t.Fatalf("core sync followed the redirect %d times", got)
	}
	root := t.TempDir()
	if err := cloneMirrorRepositoryFollowing(context.Background(), upstream, filepath.Join(root, "fresh.git"), "", false); err == nil {
		t.Fatal("a redirecting upstream must fail the core clone")
	}
	if got := atomic.LoadInt64(hits); got != 0 {
		t.Fatalf("core clone followed the redirect %d times", got)
	}
}

func TestMirrorSyncStillFollowsRedirects(t *testing.T) {
	upstream, hits := redirectingUpstream(t)
	_, bare := makeBareRepo(t)
	_ = syncRepository(context.Background(), Repository{GitDir: bare}, []string{upstream}, "")
	if atomic.LoadInt64(hits) == 0 {
		t.Fatal("a user mirror's sync must keep following the relay's redirect")
	}
}
