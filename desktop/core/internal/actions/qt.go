package actions

import (
	"bytes"
	"encoding/json"
	"math"
	"regexp"
	"runtime"
	"sort"
	"strconv"
	"strings"
	"unicode"
	"unicode/utf16"
	"unicode/utf8"
)

func qtSpace(r rune) bool {
	if r == ' ' || (r >= '\t' && r <= '\r') {
		return true
	}
	if r <= 127 || r > 0xFFFF {
		return false
	}
	return r == 0x85 || r == 0xa0 || unicode.In(r, unicode.Zs, unicode.Zl, unicode.Zp)
}

func qtTrim(text string) string {
	return strings.TrimFunc(text, qtSpace)
}

func qtSimplified(text string) string {
	return strings.Join(strings.FieldsFunc(text, qtSpace), " ")
}

func utf16Len(text string) int {
	n := 0
	for _, r := range text {
		if r > 0xFFFF {
			n += 2
		} else {
			n++
		}
	}
	return n
}

func utf16Units(text string) []uint16 {
	return utf16.Encode([]rune(text))
}

func fromUnits(units []uint16) string {
	return string(utf16.Decode(units))
}

func qtLeft(text string, n int) string {
	if n < 0 {
		return text
	}
	units := utf16Units(text)
	if len(units) <= n {
		return text
	}
	return fromUnits(units[:n])
}

func qtRight(text string, n int) string {
	units := utf16Units(text)
	if len(units) <= n {
		return text
	}
	return fromUnits(units[len(units)-n:])
}

func dropped(unit uint16) bool {
	if utf16.IsSurrogate(rune(unit)) {
		return false
	}
	r := rune(unit)
	return qtSpace(r) || unicode.Is(unicode.Cc, r) || unicode.Is(unicode.Cf, r)
}

func flatText(text string, max int) string {
	units := utf16Units(text)
	out := make([]uint16, 0, len(units))
	space := false
	for _, unit := range units {
		if dropped(unit) {
			space = len(out) > 0
			continue
		}
		if space {
			out = append(out, ' ')
		}
		space = false
		out = append(out, unit)
		if len(out) >= max {
			break
		}
	}
	if len(out) > max {
		out = out[:max]
	}
	if n := len(out); n > 0 && out[n-1] >= 0xD800 && out[n-1] < 0xDC00 {
		out = out[:n-1]
	}
	return qtTrim(fromUnits(out))
}

func qtFromUtf8(data []byte) string {
	if utf8.Valid(data) {
		return string(data)
	}
	var out strings.Builder
	out.Grow(len(data))
	for len(data) > 0 {
		r, size := utf8.DecodeRune(data)
		if r == utf8.RuneError && size <= 1 {
			out.WriteRune(utf8.RuneError)
			data = data[1:]
			continue
		}
		out.WriteRune(r)
		data = data[size:]
	}
	return out.String()
}

func stripCarriageReturns(data []byte) []byte {
	if bytes.IndexByte(data, '\r') < 0 {
		return data
	}
	return bytes.ReplaceAll(data, []byte{'\r'}, nil)
}

func equalFoldQt(a, b string) bool {
	return strings.EqualFold(a, b)
}

func containsString(list []string, value string) bool {
	for _, item := range list {
		if item == value {
			return true
		}
	}
	return false
}

func wildcardRegexp(pattern string) (*regexp.Regexp, bool) {
	units := []rune(pattern)
	var rx strings.Builder
	windows := runtime.GOOS == "windows"
	star, question := "[^/]*", "[^/]"
	separator := '/'
	if windows {
		star, question = `[^/\\]*`, `[^/\\]`
		separator = '\\'
	}
	i := 0
	for i < len(units) {
		c := units[i]
		i++
		switch c {
		case '*':
			rx.WriteString(star)
		case '?':
			rx.WriteString(question)
		case '\\':
			if windows {
				rx.WriteString(`[/\\]`)
			} else {
				rx.WriteString(`\\`)
			}
		case '/':
			if windows {
				rx.WriteString(`[/\\]`)
			} else {
				rx.WriteRune('/')
			}
		case '$', '(', ')', '+', '.', '^', '{', '|', '}':
			rx.WriteRune('\\')
			rx.WriteRune(c)
		case '[':
			rx.WriteRune(c)
			if i < len(units) {
				if units[i] == '!' {
					rx.WriteRune('^')
					i++
				}
				if i < len(units) && units[i] == ']' {
					rx.WriteRune(units[i])
					i++
				}
				for i < len(units) && units[i] != ']' {
					if units[i] == '/' || units[i] == separator {
						re, err := regexp.Compile(rx.String())
						return re, err == nil
					}
					if units[i] == '\\' {
						rx.WriteRune('\\')
					}
					rx.WriteRune(units[i])
					i++
				}
			}
		default:
			rx.WriteRune(c)
		}
	}
	re, err := regexp.Compile(`\A(?:` + rx.String() + `)\z`)
	return re, err == nil
}

func decodeJSONObject(data []byte) (map[string]any, bool) {
	decoder := json.NewDecoder(bytes.NewReader(data))
	decoder.UseNumber()
	var value any
	if err := decoder.Decode(&value); err != nil {
		return nil, false
	}
	if decoder.More() {
		return nil, false
	}
	object, ok := value.(map[string]any)
	return object, ok
}

func jsonString(object map[string]any, key string) string {
	if text, ok := object[key].(string); ok {
		return text
	}
	return ""
}

func jsonStringOr(object map[string]any, key, fallback string) string {
	if text, ok := object[key].(string); ok {
		return text
	}
	return fallback
}

func jsonBool(object map[string]any, key string) bool {
	value, _ := object[key].(bool)
	return value
}

func jsonInt(object map[string]any, key string, fallback int) int {
	switch value := object[key].(type) {
	case json.Number:
		if n, err := strconv.ParseInt(string(value), 10, 64); err == nil {
			if n >= math.MinInt32 && n <= math.MaxInt32 {
				return int(n)
			}
			return fallback
		}
		if f, err := value.Float64(); err == nil && f == math.Trunc(f) && f >= math.MinInt32 && f <= math.MaxInt32 {
			return int(f)
		}
	case float64:
		if value == math.Trunc(value) && value >= math.MinInt32 && value <= math.MaxInt32 {
			return int(value)
		}
	}
	return fallback
}

func jsonLongLong(object map[string]any, key string) int64 {
	switch value := object[key].(type) {
	case json.Number:
		if n, err := strconv.ParseInt(string(value), 10, 64); err == nil {
			return n
		}
		if f, err := value.Float64(); err == nil {
			return roundHalfAway(f)
		}
	case float64:
		return roundHalfAway(value)
	case string:
		if n, err := strconv.ParseInt(strings.TrimSpace(value), 10, 64); err == nil {
			return n
		}
	case bool:
		if value {
			return 1
		}
	}
	return 0
}

func roundHalfAway(f float64) int64 {
	if math.IsNaN(f) || math.IsInf(f, 0) {
		return 0
	}
	if f >= 0 {
		return int64(f + 0.5)
	}
	return int64(f - 0.5)
}

func jsonArray(object map[string]any, key string) []any {
	value, _ := object[key].([]any)
	return value
}

type qtJSONWriter struct {
	out bytes.Buffer
}

func qtJSONIndented(value map[string]any) []byte {
	var writer qtJSONWriter
	writer.object(value, 0)
	return writer.out.Bytes()
}

func (w *qtJSONWriter) indent(level int) {
	for i := 0; i < level*4; i++ {
		w.out.WriteByte(' ')
	}
}

func (w *qtJSONWriter) object(value map[string]any, level int) {
	w.out.WriteString("{\n")
	keys := make([]string, 0, len(value))
	for key := range value {
		keys = append(keys, key)
	}
	sort.Slice(keys, func(i, j int) bool { return qtKeyLess(keys[i], keys[j]) })
	for index, key := range keys {
		w.indent(level + 1)
		w.out.WriteByte('"')
		w.escaped(key)
		w.out.WriteString("\": ")
		w.value(value[key], level+1)
		if index < len(keys)-1 {
			w.out.WriteString(",\n")
		} else {
			w.out.WriteByte('\n')
		}
	}
	w.indent(level)
	w.out.WriteString("}\n")
}

func qtKeyLess(a, b string) bool {
	ua, ub := utf16Units(a), utf16Units(b)
	for i := 0; i < len(ua) && i < len(ub); i++ {
		if ua[i] != ub[i] {
			return ua[i] < ub[i]
		}
	}
	return len(ua) < len(ub)
}

func (w *qtJSONWriter) value(value any, level int) {
	switch v := value.(type) {
	case string:
		w.out.WriteByte('"')
		w.escaped(v)
		w.out.WriteByte('"')
	case bool:
		if v {
			w.out.WriteString("true")
		} else {
			w.out.WriteString("false")
		}
	case int:
		w.out.WriteString(strconv.FormatInt(int64(v), 10))
	case int64:
		w.out.WriteString(strconv.FormatInt(v, 10))
	case []string:
		w.out.WriteString("[\n")
		for index, item := range v {
			w.indent(level + 1)
			w.value(item, level+1)
			if index < len(v)-1 {
				w.out.WriteString(",\n")
			} else {
				w.out.WriteByte('\n')
			}
		}
		w.indent(level)
		w.out.WriteByte(']')
	case nil:
		w.out.WriteString("null")
	}
}

func (w *qtJSONWriter) escaped(text string) {
	const hex = "0123456789abcdef"
	for _, r := range text {
		if r < 0x80 {
			if r < 0x20 || r == '"' || r == '\\' {
				w.out.WriteByte('\\')
				switch r {
				case '"':
					w.out.WriteByte('"')
				case '\\':
					w.out.WriteByte('\\')
				case '\b':
					w.out.WriteByte('b')
				case '\f':
					w.out.WriteByte('f')
				case '\n':
					w.out.WriteByte('n')
				case '\r':
					w.out.WriteByte('r')
				case '\t':
					w.out.WriteByte('t')
				default:
					w.out.WriteString("u00")
					w.out.WriteByte(hex[r>>4])
					w.out.WriteByte(hex[r&0xf])
				}
				continue
			}
			w.out.WriteByte(byte(r))
			continue
		}
		w.out.WriteRune(r)
	}
}

func qtJSONCompact(value any) []byte {
	var writer qtJSONWriter
	writer.compact(value)
	return writer.out.Bytes()
}

func (w *qtJSONWriter) compact(value any) {
	switch v := value.(type) {
	case map[string]any:
		w.out.WriteByte('{')
		keys := make([]string, 0, len(v))
		for key := range v {
			keys = append(keys, key)
		}
		sort.Slice(keys, func(i, j int) bool { return qtKeyLess(keys[i], keys[j]) })
		for index, key := range keys {
			if index > 0 {
				w.out.WriteByte(',')
			}
			w.out.WriteByte('"')
			w.escaped(key)
			w.out.WriteString("\":")
			w.compact(v[key])
		}
		w.out.WriteByte('}')
	case []any:
		w.out.WriteByte('[')
		for index, item := range v {
			if index > 0 {
				w.out.WriteByte(',')
			}
			w.compact(item)
		}
		w.out.WriteByte(']')
	case []map[string]any:
		items := make([]any, len(v))
		for index, item := range v {
			items[index] = item
		}
		w.compact(items)
	case []string:
		items := make([]any, len(v))
		for index, item := range v {
			items[index] = item
		}
		w.compact(items)
	case float64:
		if v == math.Trunc(v) && math.Abs(v) < (1<<53) {
			w.out.WriteString(strconv.FormatInt(int64(v), 10))
		} else {
			w.out.WriteString(strconv.FormatFloat(v, 'g', -1, 64))
		}
	default:
		w.value(value, 0)
	}
}
