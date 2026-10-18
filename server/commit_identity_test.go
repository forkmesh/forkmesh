package mirrornode

import "testing"

func TestCommitIdentityDefaultsToTheNode(t *testing.T) {
	t.Setenv("FORKMESH_COMMIT_NAME", "")
	t.Setenv("FORKMESH_COMMIT_EMAIL", "")
	if got := commitIdentityName("box"); got != "box" {
		t.Fatalf("name %q", got)
	}
	if got := commitIdentityEmail("box"); got != "box@users.noreply.forkmesh.com" {
		t.Fatalf("email %q", got)
	}
}

func TestCommitIdentityUsesTheInstanceSettings(t *testing.T) {
	t.Setenv("FORKMESH_COMMIT_NAME", "Acme Code")
	t.Setenv("FORKMESH_COMMIT_EMAIL", "noreply@code.example.org")
	if got := commitIdentityName("box"); got != "Acme Code" {
		t.Fatalf("name %q", got)
	}
	if got := commitIdentityEmail("box"); got != "noreply@code.example.org" {
		t.Fatalf("email %q", got)
	}
}

func TestCommitIdentityRefusesUnsafeSettings(t *testing.T) {
	t.Setenv("FORKMESH_COMMIT_NAME", "Acme\nCode")
	t.Setenv("FORKMESH_COMMIT_EMAIL", "a@b <c@d>")
	if got := commitIdentityName("box"); got != "box" {
		t.Fatalf("name %q", got)
	}
	if got := commitIdentityEmail("box"); got != "box@users.noreply.forkmesh.com" {
		t.Fatalf("email %q", got)
	}
}
