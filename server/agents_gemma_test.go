package mirrornode

import (
	"context"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"slices"
	"strings"
	"testing"
)

func gemmaLookPath(present ...string) func(string) (string, error) {
	return func(binary string) (string, error) {
		if slices.Contains(present, binary) {
			return "/usr/local/bin/" + binary, nil
		}
		return "", exec.ErrNotFound
	}
}

func TestAgentEnvelopeAcceptsGemmaJobs(t *testing.T) {
	job := agentJob{SessionID: "s1", Prompt: "fix it", Provider: "local-llama", Model: "gemma4:12b", Kind: "start"}
	if agentEnvelopeInvalid(job) {
		t.Fatal("a Gemma job was rejected")
	}
	job.Model = "gemma4:unknown"
	if !agentEnvelopeInvalid(job) {
		t.Fatal("an unlisted Gemma model was accepted")
	}
	job.Model, job.Provider = "", "ollama"
	if !agentEnvelopeInvalid(job) {
		t.Fatal("an unknown provider was accepted")
	}
}

func TestLocalLlamaAvailabilityNeedsCodexAndOllama(t *testing.T) {
	for _, tc := range []struct {
		present []string
		ready   bool
	}{
		{[]string{"codex", "ollama"}, true},
		{[]string{"codex"}, false},
		{[]string{"ollama"}, false},
	} {
		runner := &agentJobRunner{lookPath: gemmaLookPath(tc.present...), getenv: func(string) string { return "" }}
		availability := runner.availability("local-llama")
		ready := availability["binaryFound"] == true && availability["loginState"] == "available"
		if ready != tc.ready || availability["provider"] != "local-llama" {
			t.Fatalf("%v: unexpected availability %v", tc.present, availability)
		}
	}
}

func TestCatalogAdvertisesGemmaWithCodexAndOllama(t *testing.T) {
	identity, _ := testIdentity(t)
	publisher := &CatalogPublisher{Identity: identity, LookPath: gemmaLookPath("claude", "codex", "ollama")}
	if got := publisher.agentProviders(); !slices.Equal(got, []string{"claude-code", "codex", "local-llama"}) {
		t.Fatalf("unexpected providers: %v", got)
	}
	publisher.LookPath = gemmaLookPath("claude", "ollama")
	if got := publisher.agentProviders(); !slices.Equal(got, []string{"claude-code"}) {
		t.Fatalf("Gemma advertised without Codex: %v", got)
	}
}

func TestRunCodexAgentRunsGemmaThroughOllama(t *testing.T) {
	root := t.TempDir()
	workdir := filepath.Join(root, "work")
	if err := os.Mkdir(workdir, 0o755); err != nil {
		t.Fatal(err)
	}
	var gotName string
	var gotArgs []string
	command := func(_ context.Context, _, _ string, _ io.Writer, name string, args ...string) ([]byte, error) {
		gotName, gotArgs = name, args
		return nil, nil
	}
	session := &agentSession{Provider: "local-llama"}
	if _, err := runCodexAgent(context.Background(), command, "mirror9", session, workdir, "hi"); err != nil {
		t.Fatal(err)
	}
	if gotName != "codex" || len(gotArgs) < 7 ||
		gotArgs[0] != "--dangerously-bypass-approvals-and-sandbox" || gotArgs[1] != "-c" ||
		!slices.Equal(gotArgs[3:7], []string{"-m", defaultGemmaModel, "exec", "--oss"}) {
		t.Fatalf("unexpected command: %s %v", gotName, gotArgs)
	}
	session = &agentSession{Provider: "codex"}
	if _, err := runCodexAgent(context.Background(), command, "mirror9", session, workdir, "hi"); err != nil {
		t.Fatal(err)
	}
	if gotArgs[0] != "--dangerously-bypass-approvals-and-sandbox" || gotArgs[1] != "-c" ||
		!strings.Contains(gotArgs[2], "Run autonomously until the task is complete") || slices.Contains(gotArgs, "--sandbox") {
		t.Fatalf("Codex is not configured for autonomous full access: %v", gotArgs)
	}
	if slices.Contains(gotArgs, "--oss") || slices.Contains(gotArgs, "-m") {
		t.Fatalf("a Codex run picked up Gemma flags: %v", gotArgs)
	}
}
