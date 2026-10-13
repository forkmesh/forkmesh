package mirrornode

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
)

var (
	testJPEG = append([]byte{0xff, 0xd8, 0xff, 0xe0}, bytes.Repeat([]byte{7}, 60)...)
	testPNG  = append([]byte("\x89PNG\r\n\x1a\n"), bytes.Repeat([]byte{9}, 60)...)
)

// installPromptRecordingClaude is installFakeClaude that also keeps every
// prompt it was given and what the attachment folder held when it ran.
func installPromptRecordingClaude(t *testing.T) (calls, prompts, listing string) {
	t.Helper()
	bin := t.TempDir()
	calls = filepath.Join(bin, "calls.log")
	prompts = filepath.Join(bin, "prompts.log")
	listing = filepath.Join(bin, "listing.log")
	script := `#!/bin/sh
{ cat; echo; echo "--END--"; } >> "` + prompts + `"
echo "$*" >> "` + calls + `"
ls .forkmesh/attachments >> "` + listing + `" 2>&1
echo "--END--" >> "` + listing + `"
echo "agent line" >> README.md
echo '{"type":"result","result":"Looked at the images.","session_id":"sess-1","is_error":false,"num_turns":1}'
`
	if err := os.WriteFile(filepath.Join(bin, "claude"), []byte(script), 0700); err != nil {
		t.Fatal(err)
	}
	t.Setenv("PATH", bin+string(os.PathListSeparator)+os.Getenv("PATH"))
	t.Setenv("HOME", t.TempDir())
	t.Setenv("XDG_CONFIG_HOME", t.TempDir())
	t.Setenv("XDG_DATA_HOME", t.TempDir())
	t.Setenv("XDG_CACHE_HOME", t.TempDir())
	t.Setenv("FORKMESH_HOME", "")
	t.Setenv("FORKMESH_PROJECTS_DIR", "")
	t.Setenv("CLAUDE_CONFIG_DIR", "")
	t.Setenv("ANTHROPIC_API_KEY", "test-key")
	return calls, prompts, listing
}

func attachmentMeta(id, mediaType string, data []byte) map[string]any {
	digest := sha256.Sum256(data)
	return map[string]any{
		"id": id, "name": "photo.jpg", "mediaType": mediaType,
		"size": len(data), "sha256": hex.EncodeToString(digest[:]),
	}
}

var attachmentLine = regexp.MustCompile(
	`^- \.forkmesh/attachments/[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}\.(jpg|png)$`)

func TestAgentAttachmentPromptBlockText(t *testing.T) {
	got := agentAttachmentPromptBlock([]string{".forkmesh/attachments/a.jpg", ".forkmesh/attachments/b.png"})
	want := "\n\nAttached images (open them with your file-reading tool):\n" +
		"- .forkmesh/attachments/a.jpg\n- .forkmesh/attachments/b.png"
	if got != want {
		t.Fatalf("block = %q, want %q", got, want)
	}
	if agentAttachmentPromptBlock(nil) != "" {
		t.Fatal("no images must add nothing to the prompt")
	}
}

func TestMirrorNodeMaterializesAttachmentsBeforeTheRun(t *testing.T) {
	_, prompts, listing := installPromptRecordingClaude(t)
	daemon, relay, bare := newAgentTestDaemon(t, "")
	jpegID, pngID := strings.Repeat("a", 32), strings.Repeat("b", 32)
	relay.attachments = map[string][]byte{jpegID: testJPEG, pngID: testPNG}
	job := agentTestJob(61, "start")
	job["createPr"] = false
	job["attachments"] = []map[string]any{
		attachmentMeta(jpegID, "image/jpeg", testJPEG),
		attachmentMeta(pngID, "image/png", testPNG),
	}
	relay.enqueue(job)
	drainAgents(t, daemon)

	reports := relay.snapshot()
	if len(reports) != 2 || reports[0]["status"] != "running" || reports[1]["status"] != "completed" {
		t.Fatalf("reports = %#v, want running then completed", reports)
	}
	if fmt.Sprint(relay.attachmentLeases) != "[lease-start lease-start]" {
		t.Fatalf("downloads presented leases %v", relay.attachmentLeases)
	}
	raw, _ := os.ReadFile(prompts)
	prompt := strings.SplitN(string(raw), "\n--END--", 2)[0]
	head := "Add a line to the readme\n\nAttached images (open them with your file-reading tool):\n"
	if !strings.HasPrefix(prompt, head) {
		t.Fatalf("prompt = %q", prompt)
	}
	lines := strings.Split(strings.TrimSpace(strings.TrimPrefix(prompt, head)), "\n")
	if len(lines) != 2 || !attachmentLine.MatchString(lines[0]) || !attachmentLine.MatchString(lines[1]) ||
		!strings.HasSuffix(lines[0], ".jpg") || !strings.HasSuffix(lines[1], ".png") {
		t.Fatalf("attachment lines = %q", lines)
	}
	// The files were in place, with the exact bytes, when the CLI ran.
	workdir := filepath.Join(daemon.agents.dir, "work", agentSessionDirectory("session-a"))
	for i, want := range [][]byte{testJPEG, testPNG} {
		got, err := os.ReadFile(filepath.Join(workdir, strings.TrimPrefix(lines[i], "- ")))
		if err != nil || !bytes.Equal(got, want) {
			t.Fatalf("image %d = %v, %v", i, got, err)
		}
	}
	if seen, _ := os.ReadFile(listing); strings.Count(string(seen), ".") < 2 {
		t.Fatalf("the CLI ran before the images were written: %q", seen)
	}
	exclude, _ := os.ReadFile(filepath.Join(workdir, ".git", "info", "exclude"))
	if !strings.Contains(string(exclude), "\n/.forkmesh/attachments/\n") &&
		!strings.HasPrefix(string(exclude), "/.forkmesh/attachments/\n") {
		t.Fatalf("exclude = %q", exclude)
	}
	// The branch has the agent's change and none of the images.
	info, _ := reports[1]["agentInfo"].(map[string]any)
	branch, _ := info["branchName"].(string)
	tree := gitValue(context.Background(), bare, "ls-tree", "-r", "--name-only", "refs/heads/"+branch)
	if !strings.Contains(tree, "README.md") || strings.Contains(tree, "attachments") {
		t.Fatalf("published tree = %q", tree)
	}

	// A follow-up turn gets its own images the same way.
	steerID := strings.Repeat("c", 32)
	relay.mu.Lock()
	relay.attachments[steerID] = testPNG
	relay.mu.Unlock()
	steer := agentTestJob(62, "steer")
	steer["prompt"] = "Now match the mockup"
	steer["attachments"] = []map[string]any{attachmentMeta(steerID, "image/png", testPNG)}
	relay.enqueue(steer)
	daemon.agents.request()
	drainAgents(t, daemon)
	reports = relay.snapshot()
	if len(reports) != 4 || reports[3]["status"] != "completed" {
		t.Fatalf("follow-up reports = %#v", reports)
	}
	raw, _ = os.ReadFile(prompts)
	second := strings.SplitN(string(raw), "\n--END--", 3)[1]
	secondHead := "\nNow match the mockup\n\nAttached images (open them with your file-reading tool):\n"
	if !strings.HasPrefix(second, secondHead) ||
		!attachmentLine.MatchString(strings.TrimSpace(strings.TrimPrefix(second, secondHead))) {
		t.Fatalf("follow-up prompt = %q", second)
	}
	tree = gitValue(context.Background(), bare, "ls-tree", "-r", "--name-only", "refs/heads/"+branch)
	if strings.Contains(tree, "attachments") {
		t.Fatalf("follow-up committed its images: %q", tree)
	}
}

func TestMirrorNodeFailsAJobWhoseAttachmentCannotBeFetched(t *testing.T) {
	calls, _, _ := installPromptRecordingClaude(t)
	daemon, relay, _ := newAgentTestDaemon(t, "")
	relay.attachments = map[string][]byte{}
	job := agentTestJob(63, "start")
	job["attachments"] = []map[string]any{attachmentMeta(strings.Repeat("d", 32), "image/jpeg", testJPEG)}
	relay.enqueue(job)
	drainAgents(t, daemon)
	reports := relay.snapshot()
	if len(reports) != 1 || reports[0]["status"] != "failed" ||
		!strings.Contains(fmt.Sprint(reports[0]["securityReason"]), "Could not fetch the attached images") ||
		!strings.Contains(fmt.Sprint(reports[0]["result"]), "HTTP 410") {
		t.Fatalf("reports = %#v, want one failed report naming the fetch", reports)
	}
	if _, err := os.Stat(calls); err == nil {
		t.Fatal("the agent ran without its images")
	}
}

func TestMirrorNodeRefusesAnAttachmentThatDoesNotMatchItsMetadata(t *testing.T) {
	for name, served := range map[string][]byte{
		"digest": append(append([]byte{}, testJPEG[:len(testJPEG)-1]...), 0),
		"size":   append(append([]byte{}, testJPEG...), 1),
		"magic":  testPNG,
	} {
		calls, _, _ := installPromptRecordingClaude(t)
		daemon, relay, _ := newAgentTestDaemon(t, "")
		id := strings.Repeat("e", 32)
		relay.attachments = map[string][]byte{id: served}
		meta := attachmentMeta(id, "image/jpeg", testJPEG)
		if name == "magic" {
			meta = attachmentMeta(id, "image/jpeg", testPNG)
		}
		job := agentTestJob(64, "start")
		job["attachments"] = []map[string]any{meta}
		relay.enqueue(job)
		drainAgents(t, daemon)
		reports := relay.snapshot()
		if len(reports) != 1 || reports[0]["status"] != "failed" {
			t.Fatalf("%s: reports = %#v", name, reports)
		}
		if _, err := os.Stat(calls); err == nil {
			t.Fatalf("%s: the agent ran with a bad image", name)
		}
	}
}

func TestAttachmentPathsNeverComeFromTheClient(t *testing.T) {
	// An id that is not the relay's hex form is an unsafe envelope: rejected
	// before anything is fetched or run.
	calls, _, _ := installPromptRecordingClaude(t)
	daemon, relay, _ := newAgentTestDaemon(t, "")
	relay.attachments = map[string][]byte{}
	bad := attachmentMeta("../../../etc/passwd", "image/jpeg", testJPEG)
	job := agentTestJob(65, "start")
	job["attachments"] = []map[string]any{bad}
	relay.enqueue(job)
	drainAgents(t, daemon)
	reports := relay.snapshot()
	if len(reports) != 1 || reports[0]["status"] != "rejected" || len(relay.attachmentLeases) != 0 {
		t.Fatalf("reports = %#v, downloads = %v", reports, relay.attachmentLeases)
	}
	if _, err := os.Stat(calls); err == nil {
		t.Fatal("an unsafe attachment envelope reached the CLI")
	}

	// A hostile display name changes nothing: the file is named by the node.
	_, prompts, _ := installPromptRecordingClaude(t)
	daemon, relay, _ = newAgentTestDaemon(t, "")
	id := strings.Repeat("f", 32)
	relay.attachments = map[string][]byte{id: testJPEG}
	meta := attachmentMeta(id, "image/jpeg", testJPEG)
	meta["name"] = "../../../../tmp/evil.sh"
	job = agentTestJob(66, "start")
	job["createPr"] = false
	job["attachments"] = []map[string]any{meta}
	relay.enqueue(job)
	drainAgents(t, daemon)
	raw, _ := os.ReadFile(prompts)
	if strings.Contains(string(raw), "evil") ||
		!attachmentLine.MatchString(strings.Split(strings.TrimSpace(strings.SplitN(string(raw), "\n--END--", 2)[0]), "\n")[3]) {
		t.Fatalf("prompt = %q", raw)
	}
	workdir := filepath.Join(daemon.agents.dir, "work", agentSessionDirectory("session-a"))
	entries, _ := os.ReadDir(filepath.Join(workdir, ".forkmesh", "attachments"))
	if len(entries) != 1 || strings.Contains(entries[0].Name(), "evil") {
		t.Fatalf("attachment folder = %v", entries)
	}

	// Too many, an unknown type, or an oversized declaration is malformed.
	for _, attachments := range [][]agentAttachment{
		make([]agentAttachment, agentAttachmentMaxCount+1),
		{{ID: id, MediaType: "image/svg+xml", Size: 10, Sha256: strings.Repeat("0", 64)}},
		{{ID: id, MediaType: "image/png", Size: agentAttachmentMaxBytes + 1, Sha256: strings.Repeat("0", 64)}},
	} {
		if validAgentAttachments(attachments) {
			t.Fatalf("accepted %#v", attachments)
		}
	}
}

func TestWebsiteModelAllowlistOffersBothOpusGenerations(t *testing.T) {
	for _, model := range []string{"claude-opus-5-5", "claude-opus-5"} {
		if !agentWebsiteModels[model] {
			t.Fatalf("%s is not an allowed website model", model)
		}
	}
}
