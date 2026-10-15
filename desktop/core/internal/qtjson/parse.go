package qtjson

import (
	"errors"
	"math"
	"strconv"
	"strings"
	"unicode/utf8"
)

const nestingLimit = 1024

var (
	ErrSyntax  = errors.New("qtjson: syntax error")
	ErrNesting = errors.New("qtjson: too deeply nested")
	ErrUTF8    = errors.New("qtjson: illegal UTF-8")
	ErrNumber  = errors.New("qtjson: illegal number")
	ErrGarbage = errors.New("qtjson: garbage at the end of the document")
)

type parser struct {
	data    []byte
	pos     int
	depth   int
	keepRaw bool
}

func Parse(data []byte) (Value, error) {
	return parseDocument(data, false)
}

func ParseKeepNumbers(data []byte) (Value, error) {
	return parseDocument(data, true)
}

func ParseObject(data []byte) (Value, bool) {
	value, err := parseDocument(data, false)
	if err != nil || value.kind != Object {
		return Value{}, false
	}
	return value, true
}

func parseDocument(data []byte, keepRaw bool) (Value, error) {
	p := &parser{data: data, keepRaw: keepRaw}
	if len(data) >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF {
		p.pos = 3
	}
	p.skip()
	if p.pos >= len(p.data) || (p.data[p.pos] != '{' && p.data[p.pos] != '[') {
		return Value{}, ErrSyntax
	}
	value, err := p.value()
	if err != nil {
		return Value{}, err
	}
	p.skip()
	if p.pos != len(p.data) {
		return Value{}, ErrGarbage
	}
	return value, nil
}

func (p *parser) skip() {
	for p.pos < len(p.data) {
		switch p.data[p.pos] {
		case ' ', '\t', '\n', '\r':
			p.pos++
		default:
			return
		}
	}
}

func (p *parser) value() (Value, error) {
	if p.pos >= len(p.data) {
		return Value{}, ErrSyntax
	}
	switch c := p.data[p.pos]; {
	case c == '{':
		return p.object()
	case c == '[':
		return p.array()
	case c == '"':
		s, err := p.str()
		if err != nil {
			return Value{}, err
		}
		return Value{kind: String, s: s}, nil
	case c == 't':
		return p.literal("true", Value{kind: Bool, b: true})
	case c == 'f':
		return p.literal("false", Value{kind: Bool})
	case c == 'n':
		return p.literal("null", Value{kind: Null})
	case c == '-' || c == '.' || (c >= '0' && c <= '9'):
		return p.number()
	}
	return Value{}, ErrSyntax
}

func (p *parser) literal(word string, value Value) (Value, error) {
	if p.pos+len(word) > len(p.data) || string(p.data[p.pos:p.pos+len(word)]) != word {
		return Value{}, ErrSyntax
	}
	p.pos += len(word)
	return value, nil
}

func (p *parser) object() (Value, error) {
	p.depth++
	if p.depth > nestingLimit {
		return Value{}, ErrNesting
	}
	defer func() { p.depth-- }()
	p.pos++
	out := Value{kind: Object, obj: []Member{}}
	p.skip()
	if p.pos < len(p.data) && p.data[p.pos] == '}' {
		p.pos++
		return out, nil
	}
	for {
		p.skip()
		if p.pos >= len(p.data) || p.data[p.pos] != '"' {
			return Value{}, ErrSyntax
		}
		key, err := p.str()
		if err != nil {
			return Value{}, err
		}
		p.skip()
		if p.pos >= len(p.data) || p.data[p.pos] != ':' {
			return Value{}, ErrSyntax
		}
		p.pos++
		p.skip()
		item, err := p.value()
		if err != nil {
			return Value{}, err
		}
		out.Set(key, item)
		p.skip()
		if p.pos >= len(p.data) {
			return Value{}, ErrSyntax
		}
		switch p.data[p.pos] {
		case ',':
			p.pos++
		case '}':
			p.pos++
			return out, nil
		default:
			return Value{}, ErrSyntax
		}
	}
}

func (p *parser) array() (Value, error) {
	p.depth++
	if p.depth > nestingLimit {
		return Value{}, ErrNesting
	}
	defer func() { p.depth-- }()
	p.pos++
	out := Value{kind: Array, arr: []Value{}}
	p.skip()
	if p.pos < len(p.data) && p.data[p.pos] == ']' {
		p.pos++
		return out, nil
	}
	for {
		p.skip()
		item, err := p.value()
		if err != nil {
			return Value{}, err
		}
		out.arr = append(out.arr, item)
		p.skip()
		if p.pos >= len(p.data) {
			return Value{}, ErrSyntax
		}
		switch p.data[p.pos] {
		case ',':
			p.pos++
		case ']':
			p.pos++
			return out, nil
		default:
			return Value{}, ErrSyntax
		}
	}
}

func isDigit(c byte) bool { return c >= '0' && c <= '9' }

func (p *parser) number() (Value, error) {
	start := p.pos
	isInt := true
	if p.pos < len(p.data) && p.data[p.pos] == '-' {
		p.pos++
	}
	if p.pos < len(p.data) && p.data[p.pos] == '0' {
		p.pos++
	} else {
		for p.pos < len(p.data) && isDigit(p.data[p.pos]) {
			p.pos++
		}
	}
	if p.pos < len(p.data) && p.data[p.pos] == '.' {
		p.pos++
		for p.pos < len(p.data) && isDigit(p.data[p.pos]) {
			isInt = false
			p.pos++
		}
	}
	if p.pos < len(p.data) && (p.data[p.pos] == 'e' || p.data[p.pos] == 'E') {
		isInt = false
		p.pos++
		if p.pos < len(p.data) && (p.data[p.pos] == '-' || p.data[p.pos] == '+') {
			p.pos++
		}
		for p.pos < len(p.data) && isDigit(p.data[p.pos]) {
			p.pos++
		}
	}
	token := string(p.data[start:p.pos])
	raw := ""
	if p.keepRaw {
		raw = token
	}
	if isInt {
		if n, err := strconv.ParseInt(token, 10, 64); err == nil {
			return Value{kind: Integer, i: n, raw: raw}, nil
		}
	}
	if strings.ContainsAny(token, "xXpP_") || token == "" {
		return Value{}, ErrNumber
	}
	d, err := strconv.ParseFloat(token, 64)
	if err != nil {
		var numErr *strconv.NumError
		if errors.As(err, &numErr) && numErr.Err == strconv.ErrRange && !math.IsInf(d, 0) {
			err = nil
		}
	}
	if err != nil || math.IsInf(d, 0) || math.IsNaN(d) {
		return Value{}, ErrNumber
	}
	if n, ok := doubleToInt64(d); ok {
		return Value{kind: Integer, i: n, raw: raw}, nil
	}
	return Value{kind: Double, f: d, raw: raw}, nil
}

func doubleToInt64(d float64) (int64, bool) {
	if d != math.Trunc(d) || d < -9223372036854775808.0 || d >= 9223372036854775808.0 {
		return 0, false
	}
	return int64(d), true
}

func hexValue(c byte) (rune, bool) {
	switch {
	case c >= '0' && c <= '9':
		return rune(c - '0'), true
	case c >= 'a' && c <= 'f':
		return rune(c-'a') + 10, true
	case c >= 'A' && c <= 'F':
		return rune(c-'A') + 10, true
	}
	return 0, false
}

func (p *parser) hex4() (rune, bool) {
	if p.pos+4 > len(p.data) {
		return 0, false
	}
	var r rune
	for i := 0; i < 4; i++ {
		h, ok := hexValue(p.data[p.pos+i])
		if !ok {
			return 0, false
		}
		r = r<<4 | h
	}
	p.pos += 4
	return r, true
}

func appendWTF8(buf []byte, r rune) []byte {
	if r >= 0xD800 && r <= 0xDFFF {
		return append(buf, 0xE0|byte(r>>12), 0x80|byte(r>>6)&0x3F, 0x80|byte(r)&0x3F)
	}
	return utf8.AppendRune(buf, r)
}

func (p *parser) str() (string, error) {
	p.pos++
	start := p.pos
	for p.pos < len(p.data) {
		c := p.data[p.pos]
		if c == '"' {
			s := string(p.data[start:p.pos])
			p.pos++
			return s, nil
		}
		if c == '\\' || c >= 0x80 {
			break
		}
		p.pos++
	}
	buf := make([]byte, 0, p.pos-start+16)
	buf = append(buf, p.data[start:p.pos]...)
	for p.pos < len(p.data) {
		c := p.data[p.pos]
		switch {
		case c == '"':
			p.pos++
			return string(buf), nil
		case c == '\\':
			p.pos++
			if p.pos >= len(p.data) {
				return "", ErrSyntax
			}
			e := p.data[p.pos]
			p.pos++
			switch e {
			case 'b':
				buf = append(buf, '\b')
			case 'f':
				buf = append(buf, '\f')
			case 'n':
				buf = append(buf, '\n')
			case 'r':
				buf = append(buf, '\r')
			case 't':
				buf = append(buf, '\t')
			case 'u':
				r, ok := p.hex4()
				if !ok {
					return "", ErrSyntax
				}
				if r >= 0xD800 && r <= 0xDBFF && p.pos+6 <= len(p.data) && p.data[p.pos] == '\\' && p.data[p.pos+1] == 'u' {
					save := p.pos
					p.pos += 2
					if low, ok := p.hex4(); ok && low >= 0xDC00 && low <= 0xDFFF {
						r = 0x10000 + (r-0xD800)<<10 + (low - 0xDC00)
					} else {
						p.pos = save
					}
				}
				buf = appendWTF8(buf, r)
			default:
				if e >= 0x80 {
					p.pos--
					r, size := utf8.DecodeRune(p.data[p.pos:])
					if r == utf8.RuneError && size <= 1 {
						return "", ErrUTF8
					}
					buf = append(buf, p.data[p.pos:p.pos+size]...)
					p.pos += size
				} else {
					buf = append(buf, e)
				}
			}
		case c >= 0x80:
			r, size := utf8.DecodeRune(p.data[p.pos:])
			if r == utf8.RuneError && size <= 1 {
				return "", ErrUTF8
			}
			buf = append(buf, p.data[p.pos:p.pos+size]...)
			p.pos += size
		default:
			buf = append(buf, c)
			p.pos++
		}
	}
	return "", ErrSyntax
}
