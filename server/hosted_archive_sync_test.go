package mirrornode

import (
	"context"
	"errors"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"
)

const (
	testArchiveAccess = "AccessKey0123456"
	testArchiveSecret = "SecretKey9876543"
)

func testArchiveSync() hostedArchiveSync {
	return hostedArchiveSync{Revision: "archive:1", URL: "https://s3.us.archive.org/alice-proj/proj.bundle",
		AccessKey: testArchiveAccess, Secret: testArchiveSecret, IntervalSeconds: 7 * 24 * 3600,
		Collection: "open_source_software", Title: "alice/proj", Source: "https://forkmesh.com/alice/proj"}
}

func TestHostedArchiveSyncValidation(t *testing.T) {
	cases := []struct {
		name   string
		mutate func(*hostedArchiveSync)
		want   bool
	}{
		{"valid", func(*hostedArchiveSync) {}, true},
		{"no revision", func(s *hostedArchiveSync) { s.Revision = "" }, false},
		{"other host", func(s *hostedArchiveSync) { s.URL = "https://s3.evil.example/alice-proj/proj.bundle" }, false},
		{"http", func(s *hostedArchiveSync) { s.URL = "http://s3.us.archive.org/alice-proj/proj.bundle" }, false},
		{"host suffix", func(s *hostedArchiveSync) { s.URL = "https://s3.us.archive.org.evil/alice-proj/proj.bundle" }, false},
		{"short item", func(s *hostedArchiveSync) { s.URL = "https://s3.us.archive.org/abc/proj.bundle" }, false},
		{"not a bundle", func(s *hostedArchiveSync) { s.URL = "https://s3.us.archive.org/alice-proj/proj.zip" }, false},
		{"nested", func(s *hostedArchiveSync) { s.URL = "https://s3.us.archive.org/alice-proj/a/proj.bundle" }, false},
		{"dot dot", func(s *hostedArchiveSync) { s.URL = "https://s3.us.archive.org/alice-proj/..bundle" }, false},
		{"query", func(s *hostedArchiveSync) { s.URL = "https://s3.us.archive.org/alice-proj/proj.bundle?x=1" }, false},
		{"access key", func(s *hostedArchiveSync) { s.AccessKey = "short" }, false},
		{"secret colon", func(s *hostedArchiveSync) { s.Secret = "Secret:Key987654" }, false},
		{"interval short", func(s *hostedArchiveSync) { s.IntervalSeconds = 60 }, false},
		{"interval long", func(s *hostedArchiveSync) { s.IntervalSeconds = 500 * 24 * 3600 }, false},
		{"collection", func(s *hostedArchiveSync) { s.Collection = "Bad Collection" }, false},
		{"title newline", func(s *hostedArchiveSync) { s.Title = "alice\nproj" }, false},
		{"title unicode", func(s *hostedArchiveSync) { s.Title = "alicé/proj" }, false},
		{"no source", func(s *hostedArchiveSync) { s.Source = "" }, false},
	}
	for _, tc := range cases {
		sync := testArchiveSync()
		tc.mutate(&sync)
		if got := validHostedArchiveSync(sync); got != tc.want {
			t.Errorf("%s: valid = %v, want %v", tc.name, got, tc.want)
		}
	}
}

func TestHostedArchivePutSendsTheBundleWithItsMetadata(t *testing.T) {
	var headers http.Header
	var body []byte
	fail := false
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		headers = r.Header.Clone()
		body, _ = io.ReadAll(r.Body)
		if r.Method != http.MethodPut || r.URL.Path != "/alice-proj/proj.bundle" {
			http.Error(w, "wrong request", http.StatusBadRequest)
			return
		}
		if fail {
			w.WriteHeader(http.StatusForbidden)
			_, _ = io.WriteString(w, "<Error><Code>AccessDenied</Code><Message>Access Denied for "+
				testArchiveSecret+"</Message></Error>")
		}
	}))
	defer server.Close()
	path := filepath.Join(t.TempDir(), "proj.bundle")
	if err := os.WriteFile(path, []byte("bundle bytes"), 0o600); err != nil {
		t.Fatal(err)
	}
	sync := testArchiveSync()
	if err := putHostedArchiveTo(context.Background(), server.Client(), server.URL+"/alice-proj/proj.bundle",
		sync, path, 12); err != nil {
		t.Fatal(err)
	}
	if string(body) != "bundle bytes" {
		t.Fatalf("body = %q", body)
	}
	for name, want := range map[string]string{
		"Authorization":             "LOW " + testArchiveAccess + ":" + testArchiveSecret,
		"X-Amz-Auto-Make-Bucket":    "1",
		"X-Archive-Queue-Derive":    "0",
		"X-Archive-Size-Hint":       "12",
		"X-Archive-Meta-Mediatype":  "software",
		"X-Archive-Meta-Collection": "open_source_software",
		"X-Archive-Meta-Title":      "alice/proj",
		"X-Archive-Meta-Source":     "https://forkmesh.com/alice/proj",
	} {
		if got := headers.Get(name); got != want {
			t.Errorf("%s = %q, want %q", name, got, want)
		}
	}
	if !strings.Contains(headers.Get("X-Archive-Meta-Description"), "git clone proj.bundle") {
		t.Errorf("description = %q", headers.Get("X-Archive-Meta-Description"))
	}

	fail = true
	err := putHostedArchiveTo(context.Background(), server.Client(), server.URL+"/alice-proj/proj.bundle",
		sync, path, 12)
	if err == nil || !strings.Contains(err.Error(), "HTTP 403: Access Denied") {
		t.Fatalf("error = %v", err)
	}
	redacted := redactHostedArchiveError(err.Error(), sync)
	if strings.Contains(redacted, testArchiveSecret) || !strings.Contains(redacted, "***") {
		t.Fatalf("redacted = %q", redacted)
	}
}

type fakeArchiveUploader struct {
	t     *testing.T
	mu    sync.Mutex
	calls int
	heads []string
	fail  error
}

func (f *fakeArchiveUploader) upload(_ context.Context, sync hostedArchiveSync, path string, size int64) error {
	info, err := os.Stat(path)
	if err != nil || info.Size() != size || size == 0 {
		f.t.Errorf("bundle %s: size %d, stat %v", path, size, err)
	}
	output, err := exec.Command("git", "bundle", "list-heads", path).CombinedOutput()
	if err != nil {
		f.t.Errorf("not a git bundle: %v: %s", err, output)
	}
	f.mu.Lock()
	defer f.mu.Unlock()
	f.calls++
	f.heads = append(f.heads, string(output))
	return f.fail
}

func (f *fakeArchiveUploader) count() int {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.calls
}

func TestHostedArchiveSyncUploadsOnItsInterval(t *testing.T) {
	var app fakeHostedApp
	appServer := httptest.NewServer(&app)
	defer appServer.Close()
	node := hostedTestDaemon(t, "fmnext-n1", appServer.URL)
	uploader := &fakeArchiveUploader{t: t}
	node.hosted.uploadArchive = uploader.upload
	now := time.Unix(1790000000, 0)
	node.hosted.clock = func() time.Time { return now }

	var mu sync.Mutex
	archive := testArchiveSync()
	primary := true
	visibility := "public"
	app.answer = func(string, map[string]hostedCopyState) hostedReplicaResponse {
		mu.Lock()
		defer mu.Unlock()
		copied := archive
		item := hostedDesired{ID: testHostedID, Owner: "alice", Name: "proj", Visibility: visibility,
			Primary: primary, Init: testInit(), ArchiveSync: &copied}
		return hostedReplicaResponse{Repositories: []hostedDesired{item}}
	}
	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()
	step := func() {
		t.Helper()
		if err := node.hosted.reconcile(ctx); err != nil {
			t.Fatal(err)
		}
		node.hosted.archiveWork.Wait()
	}

	step()
	if uploader.count() != 1 || !strings.Contains(uploader.heads[0], "refs/heads/main") ||
		!strings.Contains(uploader.heads[0], "HEAD") {
		t.Fatalf("uploads = %d, heads %v", uploader.count(), uploader.heads)
	}
	gitDir := node.hosted.gitDir(testHostedID)
	digest, _ := refsSHA256(ctx, gitDir)
	step()
	app.mu.Lock()
	reports := app.archive["fmnext-n1"]
	app.mu.Unlock()
	report, ok := reports[len(reports)-1][testHostedID]
	if !ok || !report.OK || report.Revision != "archive:1" || report.Digest != digest ||
		report.Bytes == 0 || report.At != now.UnixMilli() {
		t.Fatalf("report = %+v (found %v)", report, ok)
	}
	if uploader.count() != 1 {
		t.Fatal("an unchanged repository was uploaded again")
	}
	entries, _ := os.ReadDir(filepath.Join(node.hosted.cfg.Root, hostedArchiveDir))
	if len(entries) != 0 {
		t.Fatalf("the bundle was left behind: %v", entries)
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
	now = now.Add(24 * time.Hour)
	step()
	if uploader.count() != 1 {
		t.Fatal("a change was uploaded before the interval passed")
	}
	now = now.Add(7 * 24 * time.Hour)
	step()
	if uploader.count() != 2 {
		t.Fatal("a change was not uploaded once the interval passed")
	}
	step()
	if uploader.count() != 2 {
		t.Fatal("the same refs were uploaded twice")
	}

	mu.Lock()
	archive.Revision = "archive:2"
	mu.Unlock()
	uploader.mu.Lock()
	uploader.fail = errors.New("HTTP 403: Access Denied for " + testArchiveSecret)
	uploader.mu.Unlock()
	step()
	if uploader.count() != 3 {
		t.Fatal("a new revision (Back up now) did not upload")
	}
	failed := node.hosted.archiveSyncReports()[testHostedID]
	if failed.OK || strings.Contains(failed.Error, testArchiveSecret) || !strings.Contains(failed.Error, "***") {
		t.Fatalf("failed report = %+v", failed)
	}
	now = now.Add(10 * time.Minute)
	step()
	if uploader.count() != 3 {
		t.Fatal("a failed upload was retried before the backoff")
	}
	uploader.mu.Lock()
	uploader.fail = nil
	uploader.mu.Unlock()
	now = now.Add(time.Hour)
	step()
	if uploader.count() != 4 || !node.hosted.archiveSyncReports()[testHostedID].OK {
		t.Fatal("a failed upload was not retried after the backoff")
	}

	mu.Lock()
	archive.Revision = "archive:3"
	primary = false
	mu.Unlock()
	step()
	mu.Lock()
	primary = true
	visibility = "private"
	mu.Unlock()
	step()
	if uploader.count() != 4 {
		t.Fatal("a secondary core or a private repository uploaded")
	}

	node.hosted.mu.Lock()
	node.hosted.archiveState = map[string]hostedArchiveState{}
	node.hosted.mu.Unlock()
	before := uploader.count()
	latest, _ := refsSHA256(ctx, gitDir)
	mu.Lock()
	visibility = "public"
	archive.Uploaded = &hostedArchiveUploaded{Revision: "archive:3", Digest: latest, At: now.UnixMilli()}
	mu.Unlock()
	step()
	if uploader.count() != before {
		t.Fatal("an upload the app already recorded was repeated")
	}

	mu.Lock()
	archive.URL = "https://evil.example/alice-proj/proj.bundle"
	archive.Revision = "archive:4"
	mu.Unlock()
	step()
	if uploader.count() != before || node.hosted.archiveSyncReports()[testHostedID].Error != hostedArchiveInvalid {
		t.Fatal("an invalid archive sync was not refused")
	}
}
