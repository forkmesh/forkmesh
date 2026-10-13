package phonefamily

import (
	"strings"

	"forkmesh.com/desktop-core/internal/qsettings"
)

const (
	accountNameSetting = "account/nodeName"
	handleSetting      = "profile/handle"
	displayNameSetting = "profile/displayName"
)

func accountNameFromInput(value, fallback string) string {
	value = strings.ToLower(strings.TrimSpace(value))
	var b strings.Builder
	for _, r := range value {
		if r >= 128 {
			continue
		}
		if (r >= 'a' && r <= 'z') || (r >= '0' && r <= '9') || r == '-' {
			b.WriteRune(r)
		}
	}
	out := b.String()
	for out != "" && !(out[0] >= 'a' && out[0] <= 'z') {
		out = out[1:]
	}
	if len(out) > 63 {
		out = out[:63]
	}
	out = strings.TrimRight(out, "-")
	if out == "" {
		return fallback
	}
	return out
}

func savedProfileName(s *qsettings.Settings) string {
	for _, key := range []string{accountNameSetting, handleSetting, displayNameSetting} {
		if name := strings.TrimSpace(s.String(key, "")); name != "" {
			return name
		}
	}
	return ""
}

func AccountOwner(s *qsettings.Settings) string {
	if s == nil {
		return "owner"
	}
	if stored := s.String(accountNameSetting, ""); stored != "" {
		return stored
	}
	return accountNameFromInput(strings.ToLower(savedProfileName(s)), "owner")
}
