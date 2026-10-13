package mirrornode

import "testing"

func TestPullEventSigningMessageBindsTheRepository(t *testing.T) {
	got := string(pullEventSigningMessage("Mirror9", "Proj", "AUTH", 42, "abc"))
	if got != "forkmesh-pull-event-v2\nmirror9/proj\nAUTH\n42\nabc" {
		t.Fatalf("v2 message = %q", got)
	}
	other := string(pullEventSigningMessage("mallory", "other", "AUTH", 42, "abc"))
	if other == got {
		t.Fatal("messages for different repositories must differ")
	}
	if legacy := string(pullEventSigningMessage("", "proj", "AUTH", 42, "abc")); legacy != "forkmesh-pull-event-v1\nAUTH\n42\nabc" {
		t.Fatalf("v1 fallback = %q", legacy)
	}
	if key := eventSigningRepoKey("a/b", "c"); key != "" {
		t.Fatalf("separator in owner accepted: %q", key)
	}
}
