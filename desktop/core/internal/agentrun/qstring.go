package agentrun

import (
	"strings"
	"unicode"
	"unicode/utf16"
	"unicode/utf8"
)

func toU16(s string) []uint16 {
	out := make([]uint16, 0, len(s))
	for i := 0; i < len(s); {
		c := s[i]
		if c == 0xED && i+2 < len(s) && s[i+1] >= 0xA0 && s[i+1] <= 0xBF {
			out = append(out, uint16(c&0x0F)<<12|uint16(s[i+1]&0x3F)<<6|uint16(s[i+2]&0x3F))
			i += 3
			continue
		}
		r, size := utf8.DecodeRuneInString(s[i:])
		out = utf16.AppendRune(out, r)
		i += size
	}
	return out
}

func fromU16(u []uint16) string {
	var b strings.Builder
	b.Grow(len(u))
	for i := 0; i < len(u); i++ {
		c := u[i]
		switch {
		case c >= 0xD800 && c <= 0xDBFF && i+1 < len(u) && u[i+1] >= 0xDC00 && u[i+1] <= 0xDFFF:
			b.WriteRune(utf16.DecodeRune(rune(c), rune(u[i+1])))
			i++
		case c >= 0xD800 && c <= 0xDFFF:
			b.WriteByte(0xE0 | byte(c>>12))
			b.WriteByte(0x80 | byte(c>>6)&0x3F)
			b.WriteByte(0x80 | byte(c)&0x3F)
		default:
			b.WriteRune(rune(c))
		}
	}
	return b.String()
}

func QtUTF8(s string) string {
	if !strings.Contains(s, "\xed") {
		return s
	}
	var b strings.Builder
	for i := 0; i < len(s); {
		if s[i] == 0xED && i+2 < len(s) && s[i+1] >= 0xA0 && s[i+1] <= 0xBF {
			b.WriteByte('?')
			i += 3
			continue
		}
		b.WriteByte(s[i])
		i++
	}
	return b.String()
}

func len16(s string) int {
	n := 0
	for i := 0; i < len(s); {
		if s[i] < 0x80 {
			n++
			i++
			continue
		}
		r, size := utf8.DecodeRuneInString(s[i:])
		if r >= 0x10000 {
			n += 2
		} else {
			n++
		}
		i += size
	}
	return n
}

func left16(s string, n int) string {
	if n <= 0 {
		return ""
	}
	u := toU16(s)
	if len(u) <= n {
		return s
	}
	return fromU16(u[:n])
}

func isSpace16(c uint16) bool {
	if c >= 0xD800 && c <= 0xDFFF {
		return false
	}
	return unicode.IsSpace(rune(c))
}

func trimmed(s string) string {
	return strings.TrimFunc(s, unicode.IsSpace)
}

func simplified(s string) string {
	return strings.Join(strings.FieldsFunc(s, unicode.IsSpace), " ")
}
