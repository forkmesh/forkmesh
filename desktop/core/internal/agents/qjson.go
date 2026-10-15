package agents

import (
	"bytes"
	"encoding/json"
	"math"
	"strconv"
	"strings"
	"unicode"
	"unicode/utf16"
)

var utf8BOM = []byte{0xEF, 0xBB, 0xBF}

func parseObject(data []byte) (map[string]json.RawMessage, bool) {
	data = bytes.TrimPrefix(data, utf8BOM)
	var out map[string]json.RawMessage
	if err := json.Unmarshal(data, &out); err != nil || out == nil {
		return nil, false
	}
	return out, true
}

func decodeValue(raw json.RawMessage) any {
	if len(raw) == 0 {
		return nil
	}
	decoder := json.NewDecoder(bytes.NewReader(raw))
	decoder.UseNumber()
	var value any
	if err := decoder.Decode(&value); err != nil {
		return nil
	}
	return value
}

func numberIsInteger(n json.Number) (int64, bool) {
	text := string(n)
	if strings.ContainsAny(text, ".eE") {
		return 0, false
	}
	value, err := strconv.ParseInt(text, 10, 64)
	if err != nil {
		return 0, false
	}
	return value, true
}

func numberDouble(n json.Number) float64 {
	value, err := n.Float64()
	if err != nil {
		return 0
	}
	return value
}

func qtInt(value any, def int) int {
	n, ok := value.(json.Number)
	if !ok {
		return def
	}
	if i, integer := numberIsInteger(n); integer {
		if i >= math.MinInt32 && i <= math.MaxInt32 {
			return int(i)
		}
		return def
	}
	d := numberDouble(n)
	if !(d >= math.MinInt32 && d <= math.MaxInt32) {
		return def
	}
	if float64(int32(d)) != d {
		return def
	}
	return int(int32(d))
}

func qtLongLong(value any) int64 {
	switch v := value.(type) {
	case bool:
		if v {
			return 1
		}
		return 0
	case json.Number:
		if i, integer := numberIsInteger(v); integer {
			return i
		}
		d := numberDouble(v)
		if math.IsNaN(d) || math.IsInf(d, 0) || d >= math.MaxInt64 || d <= math.MinInt64 {
			return 0
		}
		return int64(math.Round(d))
	case string:
		i, err := strconv.ParseInt(strings.TrimSpace(v), 10, 64)
		if err != nil {
			return 0
		}
		return i
	}
	return 0
}

func qtDouble(value any) float64 {
	if n, ok := value.(json.Number); ok {
		return numberDouble(n)
	}
	return 0
}

func qtBool(value any) bool {
	b, ok := value.(bool)
	return ok && b
}

func qtString(value any, def string) string {
	if s, ok := value.(string); ok {
		return s
	}
	return def
}

func qtArray(value any) []any {
	if a, ok := value.([]any); ok {
		return a
	}
	return nil
}

func trimmed(s string) string {
	return strings.TrimFunc(s, unicode.IsSpace)
}

func simplified(s string) string {
	return strings.Join(strings.FieldsFunc(s, unicode.IsSpace), " ")
}

func utf16Len(s string) int {
	n := 0
	for _, r := range s {
		n += utf16.RuneLen(r)
	}
	return n
}

func left(s string, units int) string {
	if units <= 0 {
		return ""
	}
	n := 0
	for i, r := range s {
		width := utf16.RuneLen(r)
		if width < 0 {
			width = 1
		}
		if n+width > units {
			return s[:i]
		}
		n += width
	}
	return s
}

func clamp(lo, value, hi int) int {
	if value < lo {
		return lo
	}
	if value > hi {
		return hi
	}
	return value
}
