package qtjson

import (
	"math"
	"strconv"
	"strings"
	"unicode/utf8"
)

const hexDigits = "0123456789abcdef"

func (v Value) Compact() []byte {
	return v.AppendCompact(nil)
}

func (v Value) AppendCompact(buf []byte) []byte {
	return v.appendTo(buf, 0, true)
}

func (v Value) Indented() []byte {
	switch v.kind {
	case Object:
		buf := []byte("{\n")
		buf = v.appendMembers(buf, 1, false)
		return append(buf, "}\n"...)
	case Array:
		buf := []byte("[\n")
		buf = v.appendItems(buf, 1, false)
		return append(buf, "]\n"...)
	}
	return v.appendTo(nil, 0, false)
}

func (v Value) String() string { return string(v.Compact()) }

func indent(buf []byte, level int) []byte {
	for i := 0; i < level; i++ {
		buf = append(buf, "    "...)
	}
	return buf
}

func (v Value) appendTo(buf []byte, level int, compact bool) []byte {
	switch v.kind {
	case Undefined, Null:
		return append(buf, "null"...)
	case Bool:
		if v.b {
			return append(buf, "true"...)
		}
		return append(buf, "false"...)
	case Integer:
		if v.raw != "" {
			return append(buf, v.raw...)
		}
		return strconv.AppendInt(buf, v.i, 10)
	case Double:
		if v.raw != "" {
			return append(buf, v.raw...)
		}
		return AppendDouble(buf, v.f)
	case String:
		buf = append(buf, '"')
		buf = AppendEscaped(buf, v.s)
		return append(buf, '"')
	case Array:
		if compact {
			buf = append(buf, '[')
			buf = v.appendItems(buf, level, true)
			return append(buf, ']')
		}
		buf = append(buf, "[\n"...)
		buf = v.appendItems(buf, level+1, false)
		buf = indent(buf, level)
		return append(buf, ']')
	case Object:
		if compact {
			buf = append(buf, '{')
			buf = v.appendMembers(buf, level, true)
			return append(buf, '}')
		}
		buf = append(buf, "{\n"...)
		buf = v.appendMembers(buf, level+1, false)
		buf = indent(buf, level)
		return append(buf, '}')
	}
	return buf
}

func (v Value) appendItems(buf []byte, level int, compact bool) []byte {
	for i, item := range v.arr {
		if !compact {
			buf = indent(buf, level)
		}
		buf = item.appendTo(buf, level, compact)
		if i+1 < len(v.arr) {
			if compact {
				buf = append(buf, ',')
			} else {
				buf = append(buf, ",\n"...)
			}
		} else if !compact {
			buf = append(buf, '\n')
		}
	}
	return buf
}

func (v Value) appendMembers(buf []byte, level int, compact bool) []byte {
	for i, member := range v.obj {
		if !compact {
			buf = indent(buf, level)
		}
		buf = append(buf, '"')
		buf = AppendEscaped(buf, member.Key)
		if compact {
			buf = append(buf, '"', ':')
		} else {
			buf = append(buf, '"', ':', ' ')
		}
		buf = member.Value.appendTo(buf, level, compact)
		if i+1 < len(v.obj) {
			if compact {
				buf = append(buf, ',')
			} else {
				buf = append(buf, ",\n"...)
			}
		} else if !compact {
			buf = append(buf, '\n')
		}
	}
	return buf
}

func AppendEscaped(buf []byte, s string) []byte {
	start := 0
	for i := 0; i < len(s); {
		c := s[i]
		if c < 0x80 {
			if c >= 0x20 && c != '"' && c != '\\' {
				i++
				continue
			}
			buf = append(buf, s[start:i]...)
			buf = append(buf, '\\')
			switch c {
			case '"':
				buf = append(buf, '"')
			case '\\':
				buf = append(buf, '\\')
			case '\b':
				buf = append(buf, 'b')
			case '\f':
				buf = append(buf, 'f')
			case '\n':
				buf = append(buf, 'n')
			case '\r':
				buf = append(buf, 'r')
			case '\t':
				buf = append(buf, 't')
			default:
				buf = append(buf, 'u', '0', '0', hexDigits[c>>4], hexDigits[c&0xF])
			}
			i++
			start = i
			continue
		}
		if c == 0xED && i+2 < len(s) && s[i+1] >= 0xA0 && s[i+1] <= 0xBF {
			r := rune(c&0x0F)<<12 | rune(s[i+1]&0x3F)<<6 | rune(s[i+2]&0x3F)
			buf = append(buf, s[start:i]...)
			buf = append(buf, '\\', 'u', hexDigits[r>>12&0xF], hexDigits[r>>8&0xF], hexDigits[r>>4&0xF], hexDigits[r&0xF])
			i += 3
			start = i
			continue
		}
		_, size := utf8.DecodeRuneInString(s[i:])
		i += size
	}
	return append(buf, s[start:]...)
}

const maxSafeInteger = 9007199254740992.0

func AppendDouble(buf []byte, d float64) []byte {
	if math.IsNaN(d) || math.IsInf(d, 0) {
		return append(buf, "null"...)
	}
	if d == math.Trunc(d) && math.Abs(d) <= maxSafeInteger {
		return strconv.AppendInt(buf, int64(d), 10)
	}
	if d < 0 {
		buf = append(buf, '-')
		d = -d
	}
	text := strconv.FormatFloat(d, 'e', -1, 64)
	mark := strings.IndexByte(text, 'e')
	mantissa, exponent := text[:mark], text[mark+1:]
	digits := strings.Replace(mantissa, ".", "", 1)
	exp, _ := strconv.Atoi(exponent)
	decpt := exp + 1
	var decimal []byte
	switch {
	case decpt <= 0:
		decimal = append(decimal, "0."...)
		decimal = append(decimal, strings.Repeat("0", -decpt)...)
		decimal = append(decimal, digits...)
	case decpt >= len(digits):
		decimal = append(decimal, digits...)
		decimal = append(decimal, strings.Repeat("0", decpt-len(digits))...)
	default:
		decimal = append(decimal, digits[:decpt]...)
		decimal = append(decimal, '.')
		decimal = append(decimal, digits[decpt:]...)
	}
	var expForm []byte
	expForm = append(expForm, digits[0])
	if len(digits) > 1 {
		expForm = append(expForm, '.')
		expForm = append(expForm, digits[1:]...)
	}
	expForm = append(expForm, 'e')
	if exp < 0 {
		expForm = append(expForm, '-')
		exp = -exp
	} else {
		expForm = append(expForm, '+')
	}
	if exp < 10 {
		expForm = append(expForm, '0')
	}
	expForm = strconv.AppendInt(expForm, int64(exp), 10)
	if len(decimal) <= len(expForm) {
		return append(buf, decimal...)
	}
	return append(buf, expForm...)
}

func FormatDouble(d float64) string { return string(AppendDouble(nil, d)) }

func Reindent(compact []byte) ([]byte, error) {
	value, err := ParseKeepNumbers(compact)
	if err != nil {
		return nil, err
	}
	return value.Indented(), nil
}
