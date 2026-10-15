package agentmodel

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

func qtDecodeUTF8(data []byte) []uint16 {
	out := make([]uint16, 0, len(data))
	for i := 0; i < len(data); {
		c := data[i]
		if c < 0x80 {
			out = append(out, uint16(c))
			i++
			continue
		}
		r, size := utf8.DecodeRune(data[i:])
		if r == utf8.RuneError && size <= 1 {
			out = append(out, 0xFFFD)
			i++
			continue
		}
		out = utf16.AppendRune(out, r)
		i += size
	}
	return out
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
	if len(s) <= n {
		return s
	}
	u := toU16(s)
	if len(u) <= n {
		return s
	}
	return fromU16(u[:n])
}

func trimmed(s string) string {
	return strings.TrimFunc(s, unicode.IsSpace)
}

func simplified(s string) string {
	return strings.Join(strings.FieldsFunc(s, unicode.IsSpace), " ")
}

func completeUTF8Prefix(data []byte) int {
	size := len(data)
	for back := 1; back <= 4 && back <= size; back++ {
		c := data[size-back]
		if c&0xC0 == 0x80 {
			continue
		}
		needed := 1
		switch {
		case c&0xE0 == 0xC0:
			needed = 2
		case c&0xF0 == 0xE0:
			needed = 3
		case c&0xF8 == 0xF0:
			needed = 4
		}
		if needed > back {
			return size - back
		}
		return size
	}
	return size
}

func withoutCR(data []byte) []byte {
	if !strings.Contains(string(data), "\r") {
		return data
	}
	out := make([]byte, 0, len(data))
	for _, c := range data {
		if c != '\r' {
			out = append(out, c)
		}
	}
	return out
}

func equalFoldQt(a, b string) bool {
	return strings.EqualFold(a, b)
}
