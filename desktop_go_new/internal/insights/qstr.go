package insights

import (
	"strings"
	"unicode"
)

func u16len(s string) int {
	n := 0
	for _, r := range s {
		if r >= 0x10000 {
			n += 2
		} else {
			n++
		}
	}
	return n
}

func left16(s string, units int) string {
	if units < 0 {
		return s
	}
	n := 0
	for i, r := range s {
		w := 1
		if r >= 0x10000 {
			w = 2
		}
		if n+w > units {
			return s[:i]
		}
		n += w
	}
	return s
}

func right16(s string, units int) string {
	total := u16len(s)
	if units >= total {
		return s
	}
	skip := total - units
	n := 0
	for i, r := range s {
		if n >= skip {
			return s[i:]
		}
		if r >= 0x10000 {
			n += 2
		} else {
			n++
		}
	}
	return ""
}

func qtSpace(r rune) bool {
	switch r {
	case '\t', '\n', '\v', '\f', '\r', ' ', 0x85, 0xA0:
		return true
	}
	return r > 0xff && unicode.IsSpace(r)
}

func trimmed(s string) string {
	return strings.TrimFunc(s, qtSpace)
}

func simplified(s string) string {
	return strings.Join(strings.FieldsFunc(s, qtSpace), " ")
}

func asciiSpace(r rune) bool {
	switch r {
	case '\t', '\n', '\v', '\f', '\r', ' ':
		return true
	}
	return false
}

func splitWhitespace(s string) []string {
	return strings.FieldsFunc(s, asciiSpace)
}

func firstSection(s string) string {
	end := strings.IndexFunc(s, asciiSpace)
	if end < 0 {
		return s
	}
	return s[:end]
}

func qtLower(s string) string {
	return strings.ToLower(s)
}
