package mirrornode

import (
	"os"
	"strings"
	"unicode"
)

const noreplyEmailDomain = "users.noreply.forkmesh.com"

func commitIdentityName(fallback string) string {
	if name := cleanIdentityValue(os.Getenv("FORKMESH_COMMIT_NAME"), 80); name != "" {
		return name
	}
	return fallback
}

func platformDisplayName() string {
	return commitIdentityName("ForkMesh")
}

func commitIdentityEmail(fallback string) string {
	email := cleanIdentityValue(os.Getenv("FORKMESH_COMMIT_EMAIL"), 254)
	if email != "" && strings.Count(email, "@") == 1 && !strings.ContainsAny(email, " <>") {
		return email
	}
	return fallback + "@" + noreplyEmailDomain
}

func cleanIdentityValue(value string, limit int) string {
	value = strings.TrimSpace(value)
	if value == "" || len(value) > limit || strings.ContainsAny(value, "<>") ||
		strings.IndexFunc(value, unicode.IsControl) >= 0 {
		return ""
	}
	return value
}
