package mirrornode

import (
	"os"
	"path/filepath"
	"slices"
	"testing"
	"time"
)

func TestExpiredProviderSignInStopsClaimsUntilSignedInAgain(t *testing.T) {
	home := t.TempDir()
	creds := filepath.Join(home, ".claude", ".credentials.json")
	if err := os.MkdirAll(filepath.Dir(creds), 0o700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(creds, []byte(`{"claudeAiOauth":{"refreshToken":"r"}}`), 0o600); err != nil {
		t.Fatal(err)
	}
	runner := &agentJobRunner{
		lookPath: gemmaLookPath("claude"),
		getenv: func(name string) string {
			if name == "HOME" {
				return home
			}
			return ""
		},
	}
	if got := runner.availableProviders(); !slices.Equal(got, []string{"claude-code"}) {
		t.Fatalf("signed-in runner offers %v", got)
	}
	if runner.noteAuthFailure("claude-code", "The agent changed three files.", "exit status 1") {
		t.Fatal("an ordinary failure was taken for an expired sign-in")
	}
	if !runner.noteAuthFailure("claude-code", "Failed to authenticate: OAuth session expired and could not be refreshed", "exit status 1") {
		t.Fatal("the expired sign-in was not noticed")
	}
	availability := runner.availability("claude-code")
	if availability["loginState"] != "expired" || availability["binaryFound"] != true {
		t.Fatalf("unexpected availability %v", availability)
	}
	if got := runner.availableProviders(); len(got) != 0 {
		t.Fatalf("expired runner still offers %v", got)
	}
	later := time.Now().Add(time.Minute)
	if err := os.WriteFile(creds, []byte(`{"claudeAiOauth":{"refreshToken":"new"}}`), 0o600); err != nil {
		t.Fatal(err)
	}
	if err := os.Chtimes(creds, later, later); err != nil {
		t.Fatal(err)
	}
	if got := runner.availability("claude-code")["loginState"]; got != "available" {
		t.Fatalf("a fresh sign-in still reads %v", got)
	}
}
