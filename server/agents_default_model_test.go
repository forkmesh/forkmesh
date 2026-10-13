package mirrornode

import (
	"context"
	"io"
	"testing"
)

// A Claude Code job that names no model runs on the user's default, Opus 5.5;
// one that names a model keeps it.
func TestClaudeAgentDefaultsToOpus55(t *testing.T) {
	for _, tc := range []struct{ requested, want string }{
		{"", "claude-opus-5-5"},
		{"claude-sonnet-5", "claude-sonnet-5"},
	} {
		var got []string
		runner := func(ctx context.Context, dir, stdin string, stdout io.Writer,
			name string, args ...string) ([]byte, error) {
			got = args
			return nil, context.Canceled
		}
		runClaudeAgent(context.Background(), runner, "", &agentSession{Model: tc.requested}, t.TempDir(), "hi", "", nil)
		model := ""
		for i := 0; i+1 < len(got); i++ {
			if got[i] == "--model" {
				model = got[i+1]
			}
		}
		if model != tc.want {
			t.Fatalf("requested %q: --model %q, want %q (args %v)", tc.requested, model, tc.want, got)
		}
	}
	if !agentWebsiteModels["claude-opus-5-5"] {
		t.Fatal("claude-opus-5-5 must be on the website model allowlist")
	}
}
