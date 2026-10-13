package mirrornode

import (
	"bytes"
	"context"
	"crypto/ed25519"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"fmt"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func TestRunnerFetchesImagesOverItsLeaseAndNamesThemInThePrompt(t *testing.T) {
	_, prompts, listing := installPromptRecordingClaude(t)
	f := newRunnerFixture(t, nil)
	jpegID, pngID := strings.Repeat("a", 32), strings.Repeat("b", 32)
	f.app.attachments = map[string][]byte{jpegID: testJPEG, pngID: testPNG}
	job := f.job(t, 21, "start", "refs/heads/agent/sess-a")
	job["attachments"] = []map[string]any{
		attachmentMeta(jpegID, "image/jpeg", testJPEG),
		attachmentMeta(pngID, "image/png", testPNG),
	}
	f.app.queue = append(f.app.queue, job)
	f.drain(t)

	_, reports := f.app.snapshot()
	if len(reports) != 2 || reports[0]["status"] != "running" || reports[1]["status"] != "completed" {
		t.Fatalf("reports = %#v", reports)
	}
	if len(f.app.fetches) != 2 {
		t.Fatalf("fetches = %#v", f.app.fetches)
	}
	for i, id := range []string{jpegID, pngID} {
		fetch := f.app.fetches[i]
		if fetch["jobId"] != float64(21) || fetch["leaseId"] != "lease-21" || fetch["attachmentId"] != id {
			t.Fatalf("fetch %d = %#v", i, fetch)
		}
	}
	raw, _ := os.ReadFile(prompts)
	prompt := strings.SplitN(string(raw), "\n--END--", 2)[0]
	head := "Add a line to the readme\n\nAttached images (open them with your file-reading tool):\n"
	if !strings.Contains(prompt, head) {
		t.Fatalf("prompt = %q", prompt)
	}
	lines := strings.Split(strings.TrimSpace(prompt[strings.Index(prompt, head)+len(head):]), "\n")
	if len(lines) != 2 || !attachmentLine.MatchString(lines[0]) || !attachmentLine.MatchString(lines[1]) {
		t.Fatalf("attachment lines = %q", lines)
	}
	seen, _ := os.ReadFile(listing)
	for _, line := range lines {
		if !strings.Contains(string(seen), filepath.Base(line)) {
			t.Fatalf("the CLI did not see %s: %q", line, seen)
		}
	}
	tree := gitValue(context.Background(), f.bare, "ls-tree", "-r", "--name-only", "refs/heads/agent/sess-a")
	if !strings.Contains(tree, "README.md") || strings.Contains(tree, "attachments") {
		t.Fatalf("published tree = %q", tree)
	}
}

func TestRunnerFailsAJobWhoseImageTheAppNoLongerHas(t *testing.T) {
	calls, _, _ := installPromptRecordingClaude(t)
	f := newRunnerFixture(t, nil)
	f.app.attachments = map[string][]byte{}
	job := f.job(t, 22, "start", "refs/heads/agent/sess-a")
	job["attachments"] = []map[string]any{attachmentMeta(strings.Repeat("c", 32), "image/jpeg", testJPEG)}
	f.app.queue = append(f.app.queue, job)
	f.drain(t)
	_, reports := f.app.snapshot()
	if len(reports) != 1 || reports[0]["status"] != "failed" ||
		!strings.Contains(fmt.Sprint(reports[0]["result"]), "HTTP 410") {
		t.Fatalf("reports = %#v", reports)
	}
	if _, err := os.Stat(calls); err == nil {
		t.Fatal("the agent ran without its images")
	}
}

type inProcessTransport struct{ handler http.Handler }

func (t inProcessTransport) RoundTrip(request *http.Request) (*http.Response, error) {
	recorder := httptest.NewRecorder()
	t.handler.ServeHTTP(recorder, request)
	return recorder.Result(), nil
}

func newInProcessRunnerBackend(t *testing.T, app *fakeRunnerApp) (*runnerBackend, agentWork, string) {
	t.Helper()
	identity, _ := testIdentity(t)
	public, _ := base64.RawURLEncoding.DecodeString(identity.PublicKey())
	app.t, app.public = t, ed25519.PublicKey(public)
	r := &agentJobRunner{}
	backend := &runnerBackend{r: r, cfg: &RunnerConfig{AppURL: "http://app.test"}, identity: identity,
		node: "runner1", version: "test", client: &http.Client{Transport: inProcessTransport{app}}, now: time.Now}
	workdir := filepath.Join(t.TempDir(), "work")
	run(t, ".", "git", "init", "--quiet", workdir)
	return backend, agentWork{job: agentJob{JobID: 31, LeaseID: "lease-31"}}, workdir
}

func TestRunnerBackendWritesLeasedImagesIntoTheWorkdir(t *testing.T) {
	app := &fakeRunnerApp{}
	jpegID, pngID := strings.Repeat("a", 32), strings.Repeat("b", 32)
	app.attachments = map[string][]byte{jpegID: testJPEG, pngID: testPNG}
	backend, work, workdir := newInProcessRunnerBackend(t, app)
	work.job.Attachments = []agentAttachment{
		testAttachment(jpegID, "image/jpeg", testJPEG), testAttachment(pngID, "image/png", testPNG)}
	paths, err := backend.materializeAttachments(context.Background(), work, workdir)
	if err != nil {
		t.Fatal(err)
	}
	if len(paths) != 2 || !attachmentLine.MatchString("- "+paths[0]) || !attachmentLine.MatchString("- "+paths[1]) {
		t.Fatalf("paths = %q", paths)
	}
	for i, want := range [][]byte{testJPEG, testPNG} {
		got, err := os.ReadFile(filepath.Join(workdir, paths[i]))
		if err != nil || !bytes.Equal(got, want) {
			t.Fatalf("image %d = %v, %v", i, got, err)
		}
	}
	if len(app.fetches) != 2 || app.fetches[0]["leaseId"] != "lease-31" || app.fetches[0]["jobId"] != float64(31) ||
		app.fetches[1]["attachmentId"] != pngID {
		t.Fatalf("fetches = %#v", app.fetches)
	}
	exclude, _ := os.ReadFile(filepath.Join(workdir, ".git", "info", "exclude"))
	if !strings.HasSuffix(string(exclude), "\n/.forkmesh/attachments/\n") {
		t.Fatalf("exclude = %q", exclude)
	}
	status := gitValue(context.Background(), workdir, "status", "--porcelain", "--untracked-files=all")
	if strings.Contains(status, "attachments") {
		t.Fatalf("git sees the images: %q", status)
	}
	if again, err := backend.materializeAttachments(context.Background(), work, workdir); err != nil || len(again) != 2 {
		t.Fatalf("second turn = %q, %v", again, err)
	}
	if exclude, _ := os.ReadFile(filepath.Join(workdir, ".git", "info", "exclude")); strings.Count(string(exclude), "/.forkmesh/attachments/") != 1 {
		t.Fatalf("exclude repeated: %q", exclude)
	}
}

func TestRunnerBackendRefusesAnImageTheAppDoesNotServeIntact(t *testing.T) {
	id := strings.Repeat("c", 32)
	for name, served := range map[string][]byte{"missing": nil, "digest": append(append([]byte{}, testJPEG[:len(testJPEG)-1]...), 0),
		"size": append(append([]byte{}, testJPEG...), 1)} {
		app := &fakeRunnerApp{attachments: map[string][]byte{}}
		if served != nil {
			app.attachments[id] = served
		}
		backend, work, workdir := newInProcessRunnerBackend(t, app)
		work.job.Attachments = []agentAttachment{testAttachment(id, "image/jpeg", testJPEG)}
		if _, err := backend.materializeAttachments(context.Background(), work, workdir); err == nil {
			t.Fatalf("%s: accepted", name)
		} else if name == "missing" && !strings.Contains(err.Error(), "HTTP 410") {
			t.Fatalf("%s: %v", name, err)
		}
	}
}

func TestRunnerBackendRefusesToWriteImagesThroughASymlink(t *testing.T) {
	id := strings.Repeat("d", 32)
	for _, link := range []string{".forkmesh", ".forkmesh/attachments"} {
		app := &fakeRunnerApp{attachments: map[string][]byte{id: testJPEG}}
		backend, work, workdir := newInProcessRunnerBackend(t, app)
		work.job.Attachments = []agentAttachment{testAttachment(id, "image/jpeg", testJPEG)}
		outside := t.TempDir()
		if err := os.MkdirAll(filepath.Dir(filepath.Join(workdir, link)), 0700); err != nil {
			t.Fatal(err)
		}
		if err := os.Symlink(outside, filepath.Join(workdir, link)); err != nil {
			t.Fatal(err)
		}
		if _, err := backend.materializeAttachments(context.Background(), work, workdir); err == nil {
			t.Fatalf("%s: wrote through a symlink", link)
		}
		if entries, _ := os.ReadDir(outside); len(entries) != 0 {
			t.Fatalf("%s: wrote outside the workdir: %v", link, entries)
		}
	}
}

func testAttachment(id, mediaType string, data []byte) agentAttachment {
	digest := sha256.Sum256(data)
	return agentAttachment{ID: id, Name: "photo", MediaType: mediaType, Size: int64(len(data)),
		Sha256: hex.EncodeToString(digest[:])}
}
