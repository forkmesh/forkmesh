package qvariant

import (
	"bytes"
	"encoding/binary"
	"errors"
	"math"
	"sort"
	"strings"
	"unicode/utf16"
)

const (
	typeBool       = 1
	typeInt        = 2
	typeUInt       = 3
	typeLongLong   = 4
	typeULongLong  = 5
	typeDouble     = 6
	typeMap        = 8
	typeList       = 9
	typeString     = 10
	typeStringList = 11
	typeByteArray  = 12
	typeHash       = 28
)

var errTruncated = errors.New("qvariant: truncated")

func isHex(c byte) bool {
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')
}

func hexValue(c byte) int {
	switch {
	case c >= '0' && c <= '9':
		return int(c - '0')
	case c >= 'a' && c <= 'f':
		return int(c-'a') + 10
	}
	return int(c-'A') + 10
}

func UnescapeIni(raw string) []rune {
	var out []rune
	inQuotes := false
	for i := 0; i < len(raw); i++ {
		c := raw[i]
		switch {
		case c == '"':
			inQuotes = !inQuotes
		case c == '\\' && i+1 < len(raw):
			i++
			switch e := raw[i]; e {
			case 'a':
				out = append(out, '\a')
			case 'b':
				out = append(out, '\b')
			case 'f':
				out = append(out, '\f')
			case 'n':
				out = append(out, '\n')
			case 'r':
				out = append(out, '\r')
			case 't':
				out = append(out, '\t')
			case 'v':
				out = append(out, '\v')
			case 'x':
				value := 0
				j := i + 1
				for j < len(raw) && isHex(raw[j]) {
					value = value*16 + hexValue(raw[j])
					j++
				}
				out = append(out, rune(value))
				i = j - 1
			case '0', '1', '2', '3', '4', '5', '6', '7':
				value := 0
				j := i
				for j < len(raw) && raw[j] >= '0' && raw[j] <= '7' {
					value = value*8 + int(raw[j]-'0')
					j++
				}
				out = append(out, rune(value))
				i = j - 1
			case '\n':
			default:
				out = append(out, rune(e))
			}
		default:
			_ = inQuotes
			out = append(out, rune(c))
		}
	}
	return out
}

func FromSettingsRaw(raw string) (any, error) {
	text := UnescapeIni(strings.TrimSpace(raw))
	const prefix = "@Variant("
	if len(text) < len(prefix)+1 || string(text[:len(prefix)]) != prefix || text[len(text)-1] != ')' {
		return nil, errors.New("qvariant: not a @Variant value")
	}
	body := text[len(prefix) : len(text)-1]
	data := make([]byte, len(body))
	for i, r := range body {
		data[i] = byte(r)
	}
	d := &decoder{data: data}
	value, err := d.variant()
	if err != nil {
		return nil, err
	}
	return value, nil
}

type decoder struct {
	data []byte
	pos  int
}

func (d *decoder) take(n int) ([]byte, error) {
	if n < 0 || d.pos+n > len(d.data) {
		return nil, errTruncated
	}
	out := d.data[d.pos : d.pos+n]
	d.pos += n
	return out, nil
}

func (d *decoder) u32() (uint32, error) {
	b, err := d.take(4)
	if err != nil {
		return 0, err
	}
	return binary.BigEndian.Uint32(b), nil
}

func (d *decoder) u64() (uint64, error) {
	b, err := d.take(8)
	if err != nil {
		return 0, err
	}
	return binary.BigEndian.Uint64(b), nil
}

func (d *decoder) str() (string, error) {
	n, err := d.u32()
	if err != nil {
		return "", err
	}
	if n == 0xFFFFFFFF {
		return "", nil
	}
	b, err := d.take(int(n))
	if err != nil {
		return "", err
	}
	units := make([]uint16, len(b)/2)
	for i := range units {
		units[i] = binary.BigEndian.Uint16(b[2*i:])
	}
	return string(utf16.Decode(units)), nil
}

func (d *decoder) variant() (any, error) {
	kind, err := d.u32()
	if err != nil {
		return nil, err
	}
	switch kind {
	case typeBool:
		b, err := d.take(1)
		if err != nil {
			return nil, err
		}
		return b[0] != 0, nil
	case typeInt:
		v, err := d.u32()
		return int64(int32(v)), err
	case typeUInt:
		v, err := d.u32()
		return int64(v), err
	case typeLongLong:
		v, err := d.u64()
		return int64(v), err
	case typeULongLong:
		v, err := d.u64()
		return v, err
	case typeDouble:
		v, err := d.u64()
		return math.Float64frombits(v), err
	case typeString:
		return d.str()
	case typeByteArray:
		n, err := d.u32()
		if err != nil {
			return nil, err
		}
		if n == 0xFFFFFFFF {
			return []byte(nil), nil
		}
		b, err := d.take(int(n))
		return append([]byte(nil), b...), err
	case typeStringList:
		n, err := d.u32()
		if err != nil {
			return nil, err
		}
		out := []string{}
		for i := uint32(0); i < n; i++ {
			s, err := d.str()
			if err != nil {
				return nil, err
			}
			out = append(out, s)
		}
		return out, nil
	case typeList:
		n, err := d.u32()
		if err != nil {
			return nil, err
		}
		out := []any{}
		for i := uint32(0); i < n; i++ {
			v, err := d.variant()
			if err != nil {
				return nil, err
			}
			out = append(out, v)
		}
		return out, nil
	case typeMap, typeHash:
		n, err := d.u32()
		if err != nil {
			return nil, err
		}
		out := map[string]any{}
		for i := uint32(0); i < n; i++ {
			key, err := d.str()
			if err != nil {
				return nil, err
			}
			value, err := d.variant()
			if err != nil {
				return nil, err
			}
			if _, seen := out[key]; !seen {
				out[key] = value
			}
		}
		return out, nil
	}
	return nil, errors.New("qvariant: unsupported type")
}

func Int64(value any) int64 {
	switch v := value.(type) {
	case int64:
		return v
	case uint64:
		return int64(v)
	case float64:
		if math.IsNaN(v) || math.IsInf(v, 0) {
			return 0
		}
		return int64(v)
	case bool:
		if v {
			return 1
		}
	case string:
		var n int64
		neg := false
		s := strings.TrimSpace(v)
		if strings.HasPrefix(s, "-") {
			neg, s = true, s[1:]
		}
		if s == "" {
			return 0
		}
		for _, c := range s {
			if c < '0' || c > '9' {
				return 0
			}
			n = n*10 + int64(c-'0')
		}
		if neg {
			return -n
		}
		return n
	}
	return 0
}

func Bool(value any) bool {
	switch v := value.(type) {
	case bool:
		return v
	case int64:
		return v != 0
	case uint64:
		return v != 0
	case float64:
		return v != 0
	case string:
		lower := strings.ToLower(strings.TrimSpace(v))
		return lower != "" && lower != "0" && lower != "false"
	}
	return false
}

func String(value any) string {
	switch v := value.(type) {
	case string:
		return v
	case []byte:
		return string(v)
	}
	return ""
}

func encodeString(b *bytes.Buffer, s string) {
	units := utf16.Encode([]rune(s))
	_ = binary.Write(b, binary.BigEndian, uint32(len(units)*2))
	for _, u := range units {
		_ = binary.Write(b, binary.BigEndian, u)
	}
}

func encodeVariant(b *bytes.Buffer, value any) {
	switch v := value.(type) {
	case bool:
		_ = binary.Write(b, binary.BigEndian, uint32(typeBool))
		if v {
			b.WriteByte(1)
		} else {
			b.WriteByte(0)
		}
	case int32:
		_ = binary.Write(b, binary.BigEndian, uint32(typeInt))
		_ = binary.Write(b, binary.BigEndian, v)
	case int64:
		_ = binary.Write(b, binary.BigEndian, uint32(typeLongLong))
		_ = binary.Write(b, binary.BigEndian, v)
	case float64:
		_ = binary.Write(b, binary.BigEndian, uint32(typeDouble))
		_ = binary.Write(b, binary.BigEndian, math.Float64bits(v))
	case string:
		_ = binary.Write(b, binary.BigEndian, uint32(typeString))
		encodeString(b, v)
	case []string:
		_ = binary.Write(b, binary.BigEndian, uint32(typeStringList))
		_ = binary.Write(b, binary.BigEndian, uint32(len(v)))
		for _, s := range v {
			encodeString(b, s)
		}
	case NullString:
		_ = binary.Write(b, binary.BigEndian, uint32(typeString))
		_ = binary.Write(b, binary.BigEndian, uint32(0xFFFFFFFF))
	case map[string]any:
		_ = binary.Write(b, binary.BigEndian, uint32(typeMap))
		_ = binary.Write(b, binary.BigEndian, uint32(len(v)))
		keys := make([]string, 0, len(v))
		for key := range v {
			keys = append(keys, key)
		}
		sort.Slice(keys, func(i, j int) bool { return utf16Less(keys[i], keys[j]) })
		for _, key := range keys {
			encodeString(b, key)
			encodeVariant(b, v[key])
		}
	}
}

type NullString struct{}

func utf16Less(a, b string) bool {
	ua, ub := utf16.Encode([]rune(a)), utf16.Encode([]rune(b))
	for i := 0; i < len(ua) && i < len(ub); i++ {
		if ua[i] != ub[i] {
			return ua[i] < ub[i]
		}
	}
	return len(ua) < len(ub)
}

func Decode(data []byte) (any, error) {
	d := &decoder{data: data}
	return d.variant()
}

func Encode(value any) []byte {
	var b bytes.Buffer
	encodeVariant(&b, value)
	return b.Bytes()
}

func iniEscaped(data []byte) string {
	var out strings.Builder
	needsQuotes := false
	escapeNextIfDigit := false
	const digits = "0123456789abcdef"
	for _, c := range data {
		if c == ';' || c == ',' || c == '=' {
			needsQuotes = true
		}
		if escapeNextIfDigit && isHex(c) {
			out.WriteString("\\x")
			if c >= 16 {
				out.WriteByte(digits[c>>4])
			}
			out.WriteByte(digits[c&15])
			continue
		}
		escapeNextIfDigit = false
		switch c {
		case 0:
			out.WriteString("\\0")
			escapeNextIfDigit = true
		case '\a':
			out.WriteString("\\a")
		case '\b':
			out.WriteString("\\b")
		case '\f':
			out.WriteString("\\f")
		case '\n':
			out.WriteString("\\n")
		case '\r':
			out.WriteString("\\r")
		case '\t':
			out.WriteString("\\t")
		case '\v':
			out.WriteString("\\v")
		case '"', '\\':
			out.WriteByte('\\')
			out.WriteByte(c)
		default:
			if c <= 0x1f || c >= 0x7f {
				out.WriteString("\\x")
				if c >= 16 {
					out.WriteByte(digits[c>>4])
				}
				out.WriteByte(digits[c&15])
				escapeNextIfDigit = true
			} else {
				out.WriteByte(c)
			}
		}
	}
	if needsQuotes {
		return `"` + out.String() + `"`
	}
	return out.String()
}

func EncodeSettingsRaw(value any) string {
	var b bytes.Buffer
	encodeVariant(&b, value)
	return iniEscaped(append(append([]byte("@Variant("), b.Bytes()...), ')'))
}
