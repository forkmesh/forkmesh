package qsettings

import (
	"bufio"
	"bytes"
	"os"
	"strconv"
	"strings"
	"sync"
	"time"
)

type Value struct {
	Raw   string
	Text  string
	List  []string
	Bytes []byte
	Kind  string
}

func (v Value) String() string {
	if v.Kind == "bytes" {
		return string(v.Bytes)
	}
	if v.Kind == "list" {
		return strings.Join(v.List, ", ")
	}
	return v.Text
}

func (v Value) Bool(fallback bool) bool {
	switch strings.ToLower(strings.TrimSpace(v.String())) {
	case "true", "1", "yes", "on":
		return true
	case "false", "0", "no", "off":
		return false
	}
	return fallback
}

func (v Value) Int(fallback int64) int64 {
	n, err := strconv.ParseInt(strings.TrimSpace(v.String()), 10, 64)
	if err != nil {
		return fallback
	}
	return n
}

func (v Value) Strings() []string {
	switch v.Kind {
	case "list":
		return append([]string(nil), v.List...)
	case "bytes":
		return []string{string(v.Bytes)}
	}
	if v.Text == "" {
		return nil
	}
	return []string{v.Text}
}

type Settings struct {
	values map[string]Value
}

func Parse(data []byte) *Settings {
	settings := &Settings{values: map[string]Value{}}
	group := ""
	scanner := bufio.NewScanner(bytes.NewReader(data))
	scanner.Buffer(make([]byte, 1<<20), 64<<20)
	for scanner.Scan() {
		line := strings.TrimRight(scanner.Text(), "\r")
		trimmed := strings.TrimSpace(line)
		if trimmed == "" || strings.HasPrefix(trimmed, ";") || strings.HasPrefix(trimmed, "#") {
			continue
		}
		if strings.HasPrefix(trimmed, "[") && strings.HasSuffix(trimmed, "]") {
			group = unescapeKey(trimmed[1 : len(trimmed)-1])
			if group == "General" {
				group = ""
			}
			continue
		}
		eq := strings.IndexByte(line, '=')
		if eq < 0 {
			continue
		}
		key := unescapeKey(strings.TrimSpace(line[:eq]))
		key = strings.ReplaceAll(key, "\\", "/")
		if group != "" {
			key = group + "/" + key
		}
		settings.values[key] = parseValue(strings.TrimSpace(line[eq+1:]))
	}
	return settings
}

func Load(path string) (*Settings, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	return Parse(data), nil
}

func (s *Settings) Value(key string) (Value, bool) {
	value, ok := s.values[key]
	return value, ok
}

func (s *Settings) String(key, fallback string) string {
	if value, ok := s.values[key]; ok {
		return value.String()
	}
	return fallback
}

func (s *Settings) Bool(key string, fallback bool) bool {
	if value, ok := s.values[key]; ok {
		return value.Bool(fallback)
	}
	return fallback
}

func (s *Settings) Int(key string, fallback int64) int64 {
	if value, ok := s.values[key]; ok {
		return value.Int(fallback)
	}
	return fallback
}

func (s *Settings) Strings(key string) []string {
	if value, ok := s.values[key]; ok {
		return value.Strings()
	}
	return nil
}

func (s *Settings) Keys(prefix string) []string {
	out := []string{}
	for key := range s.values {
		if prefix == "" || strings.HasPrefix(key, prefix) {
			out = append(out, key)
		}
	}
	return out
}

func unescapeKey(key string) string {
	var out strings.Builder
	for i := 0; i < len(key); i++ {
		if key[i] == '%' && i+2 < len(key) {
			if n, err := strconv.ParseUint(key[i+1:i+3], 16, 8); err == nil {
				out.WriteByte(byte(n))
				i += 2
				continue
			}
		}
		if key[i] == '%' && i+1 < len(key) && key[i+1] == 'U' && i+5 < len(key) {
			if n, err := strconv.ParseUint(key[i+2:i+6], 16, 16); err == nil {
				out.WriteRune(rune(n))
				i += 5
				continue
			}
		}
		out.WriteByte(key[i])
	}
	return out.String()
}

func parseValue(raw string) Value {
	parts := splitValue(raw)
	if len(parts) == 1 {
		text := parts[0]
		switch {
		case strings.HasPrefix(text, "@ByteArray(") && strings.HasSuffix(text, ")"):
			return Value{Raw: raw, Kind: "bytes", Bytes: latin1(text[len("@ByteArray(") : len(text)-1])}
		case strings.HasPrefix(text, "@Variant("):
			return Value{Raw: raw, Kind: "variant", Text: text}
		case text == "@Invalid()":
			return Value{Raw: raw, Kind: "invalid"}
		case strings.HasPrefix(text, "@@"):
			text = text[1:]
		}
		return Value{Raw: raw, Kind: "text", Text: text}
	}
	return Value{Raw: raw, Kind: "list", List: parts}
}

func latin1(text string) []byte {
	out := make([]byte, 0, len(text))
	for _, r := range text {
		if r > 0xff {
			out = append(out, []byte(string(r))...)
			continue
		}
		out = append(out, byte(r))
	}
	return out
}

func splitValue(raw string) []string {
	var parts []string
	var current strings.Builder
	inQuotes := false
	sawQuoted := false
	for i := 0; i < len(raw); i++ {
		ch := raw[i]
		switch {
		case ch == '"':
			inQuotes = !inQuotes
			sawQuoted = true
		case ch == '\\' && i+1 < len(raw):
			i++
			switch raw[i] {
			case 'n':
				current.WriteByte('\n')
			case 'r':
				current.WriteByte('\r')
			case 't':
				current.WriteByte('\t')
			case 'a':
				current.WriteByte('\a')
			case 'b':
				current.WriteByte('\b')
			case 'f':
				current.WriteByte('\f')
			case 'v':
				current.WriteByte('\v')
			case '0':
				current.WriteByte(0)
			case 'x':
				j := i + 1
				for j < len(raw) && j < i+5 && isHex(raw[j]) {
					j++
				}
				if j > i+1 {
					n, _ := strconv.ParseUint(raw[i+1:j], 16, 32)
					current.WriteRune(rune(n))
					i = j - 1
				}
			case '\n':
			default:
				current.WriteByte(raw[i])
			}
		case ch == ',' && !inQuotes:
			parts = append(parts, strings.TrimSpace(current.String()))
			current.Reset()
			sawQuoted = false
		case (ch == ' ' || ch == '\t') && !inQuotes && !sawQuoted && current.Len() == 0:
		default:
			current.WriteByte(ch)
		}
	}
	last := current.String()
	if !sawQuoted {
		last = strings.TrimSpace(last)
	}
	parts = append(parts, last)
	return parts
}

func isHex(ch byte) bool {
	return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F')
}

type Cache struct {
	Path    string
	mu      sync.Mutex
	loaded  *Settings
	modTime time.Time
	size    int64
}

func (c *Cache) Get() *Settings {
	c.mu.Lock()
	defer c.mu.Unlock()
	info, err := os.Stat(c.Path)
	if err != nil {
		if c.loaded == nil {
			c.loaded = &Settings{values: map[string]Value{}}
		}
		return c.loaded
	}
	if c.loaded != nil && info.ModTime().Equal(c.modTime) && info.Size() == c.size {
		return c.loaded
	}
	if settings, err := Load(c.Path); err == nil {
		c.loaded = settings
		c.modTime = info.ModTime()
		c.size = info.Size()
	} else if c.loaded == nil {
		c.loaded = &Settings{values: map[string]Value{}}
	}
	return c.loaded
}
