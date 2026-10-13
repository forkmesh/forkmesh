package diag

import (
	"regexp"
	"strings"
	"time"
	"unicode/utf16"
	"unicode/utf8"
)

var (
	errorPattern   = regexp.MustCompile(`\b(error|errors|failed|failure|fatal|crash(ed)?|refused|exception|panic)\b`)
	warningPattern = regexp.MustCompile(`\b(warn(ing)?|retry(ing)?|timed out|timeout|slow|stall(ed)?|froze|degraded)\b`)
	secretPattern  = regexp.MustCompile(`(?i)((?:api[_-]?key|token|secret|password|passwd|authorization|bearer)["']?\s*[:=]?\s*["']?)([^\s"',;]{6,})`)
	privatePattern = regexp.MustCompile(`-----BEGIN [A-Z ]*PRIVATE KEY-----.*`)
	tokenPattern   = regexp.MustCompile(`\b(sk-[A-Za-z0-9_-]{12,}|gh[pousr]_[A-Za-z0-9]{20,})\b`)
)

func fromUTF8(raw []byte) string {
	if utf8.Valid(raw) {
		return string(raw)
	}
	var out strings.Builder
	out.Grow(len(raw))
	for len(raw) > 0 {
		r, size := utf8.DecodeRune(raw)
		out.WriteRune(r)
		raw = raw[size:]
	}
	return out.String()
}

func utf16Len(s string) int {
	n := 0
	for _, r := range s {
		n += utf16.RuneLen(r)
	}
	return n
}

func left(s string, n int) string {
	if n < 0 {
		return s
	}
	used := 0
	for i, r := range s {
		width := utf16.RuneLen(r)
		if width < 1 {
			width = 1
		}
		if used+width > n {
			return s[:i]
		}
		used += width
	}
	return s
}

func severityOf(line string) string {
	lower := strings.ToLower(line)
	if errorPattern.MatchString(lower) {
		return "error"
	}
	if warningPattern.MatchString(lower) {
		return "warning"
	}
	return "info"
}

func timestampOf(line string) int64 {
	if utf16Len(line) < 19 {
		return 0
	}
	when, err := time.ParseInLocation("2006-01-02 15:04:05", left(line, 19), time.Local)
	if err != nil {
		return 0
	}
	return when.UnixMilli()
}

func redactSecrets(line string) string {
	out := secretPattern.ReplaceAllString(line, "${1}[redacted]")
	out = privatePattern.ReplaceAllString(out, "[redacted private key]")
	return tokenPattern.ReplaceAllString(out, "[redacted]")
}

func containsFold(text, needle string) bool {
	return strings.Contains(strings.ToLower(text), strings.ToLower(needle))
}
