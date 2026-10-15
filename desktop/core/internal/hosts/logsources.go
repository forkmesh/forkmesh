package hosts

import (
	"strconv"
	"strings"
	"sync"
	"unicode"
	"unicode/utf16"
	"unicode/utf8"
)

const (
	fallbackLogSource     = 5
	forwardedLinesPerHost = 2048
)

const mainLogFeedScript = `
log="${XDG_DATA_HOME:-$HOME/.local/share}/forkmesh/node.log"
if [ ! -f "$log" ] && [ -f "$HOME/.forkmesh/node.log" ]; then log="$HOME/.forkmesh/node.log"; fi
me=$$
if [ -f "$log" ]; then
  { tail --pid="$me" -n 80 -F "$log" 2>/dev/null || tail -n 80 -F "$log"; } 2>&1 |
    while IFS= read -r l; do printf 'L node %s\n' "$l"; done &
fi
if command -v journalctl >/dev/null 2>&1; then
  if journalctl --no-pager -q -n 1 -o cat >/dev/null 2>&1; then
    { journalctl --no-pager -q -n 60 -f -o short-iso 2>/dev/null &
      jp=$!
      while kill -0 "$me" 2>/dev/null && kill -0 "$jp" 2>/dev/null; do sleep 5; done
      kill "$jp" 2>/dev/null; } |
      while IFS= read -r l; do printf 'J %s\n' "$l"; done &
  else
    printf 'M host journalctl is installed but %s cannot read the journal here\n' "$(id -un 2>/dev/null)"
  fi
elif [ ! -f "$log" ]; then
  printf 'M host Neither a ForkMesh node log (%s) nor journalctl is here to follow\n' "$log"
fi
while :; do
  echo K
  sleep 30
done
`

var webLogIdentifiers = []string{"caddy", "nginx", "cloudflared", "apache", "httpd", "haproxy", "traefik"}

func logSourceIndex(id string) int {
	for i, candidate := range logSourceIDs {
		if candidate == id {
			return i
		}
	}
	return -1
}

func logSourceID(index int) string {
	if index >= 0 && index < len(logSourceIDs) {
		return logSourceIDs[index]
	}
	return logSourceIDs[fallbackLogSource]
}

func logSourceForIdentifier(identifier string) string {
	ident := strings.ToLower(identifier)
	if strings.HasPrefix(ident, "forkmesh") {
		return "mirror"
	}
	for _, name := range webLogIdentifiers {
		if strings.HasPrefix(ident, name) {
			return "web"
		}
	}
	if strings.HasPrefix(ident, "ssh") {
		return "ssh"
	}
	return "system"
}

func journalLogSource(raw string) (string, string) {
	line := raw
	for strings.HasSuffix(line, "\r") || strings.HasSuffix(line, " ") {
		line = line[:len(line)-1]
	}
	display := line
	if line == "" || strings.HasPrefix(line, "-- ") {
		return "system", display
	}
	cursor := 0
	nextField := func() string {
		for cursor < len(line) && line[cursor] == ' ' {
			cursor++
		}
		start := cursor
		for cursor < len(line) && line[cursor] != ' ' {
			cursor++
		}
		return line[start:cursor]
	}
	stamp := nextField()
	first, _ := utf8.DecodeRuneInString(stamp)
	if stamp == "" || !unicode.IsDigit(first) {
		return "system", display
	}
	clock := ""
	units := utf16.Encode([]rune(stamp))
	t := -1
	for i, unit := range units {
		if unit == 'T' {
			t = i
			break
		}
	}
	if t > 0 && len(units) >= t+9 {
		clock = string(utf16.Decode(units[t+1 : t+9]))
	}
	ident := nextField()
	if !strings.HasSuffix(ident, ":") {
		ident = nextField()
	}
	if !strings.HasSuffix(ident, ":") {
		return "system", display
	}
	from := cursor + 1
	if from > len(line) {
		from = len(line)
	}
	message := line[from:]
	ident = ident[:len(ident)-1]
	if bracket := strings.IndexByte(ident, '['); bracket > 0 {
		ident = ident[:bracket]
	}
	if ident == "" {
		return "system", display
	}
	if clock == "" {
		display = ident + ": " + message
	} else {
		display = clock + " " + ident + ": " + message
	}
	return logSourceForIdentifier(ident), display
}

func feedLogLine(line string) (int, string) {
	if strings.HasPrefix(line, "J ") {
		id, display := journalLogSource(line[2:])
		return logSourceIndex(id), display
	}
	if !strings.HasPrefix(line, "L ") && !strings.HasPrefix(line, "M ") {
		return -1, ""
	}
	tagged := line[2:]
	space := strings.IndexByte(tagged, ' ')
	source := -1
	if space > 0 {
		source = logSourceIndex(tagged[:space])
	}
	text := tagged
	if source >= 0 {
		text = tagged[space+1:]
	} else {
		source = fallbackLogSource
	}
	return source, text
}

func forwardedLineKey(source int, text string) string { return strconv.Itoa(source) + text }

type seenHostLines struct {
	lines map[string]bool
	order []string
}

type forwardedHostLines struct {
	mu      sync.Mutex
	perHost int
	hosts   map[string]*seenHostLines
}

func newForwardedHostLines(perHost int) *forwardedHostLines {
	return &forwardedHostLines{perHost: perHost, hosts: map[string]*seenHostLines{}}
}

func (f *forwardedHostLines) record(node, line string, fromFeed bool) bool {
	seen := f.hosts[node]
	if seen == nil {
		seen = &seenHostLines{lines: map[string]bool{}}
		f.hosts[node] = seen
	}
	if _, ok := seen.lines[line]; ok {
		return false
	}
	seen.lines[line] = fromFeed
	seen.order = append(seen.order, line)
	for len(seen.order) > f.perHost {
		delete(seen.lines, seen.order[0])
		seen.order = seen.order[1:]
	}
	return true
}

func (f *forwardedHostLines) firstSighting(node, line string) bool {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.record(node, line, true)
}

func (f *forwardedHostLines) desktopSighting(node, line string) bool {
	f.mu.Lock()
	defer f.mu.Unlock()
	if seen := f.hosts[node]; seen != nil {
		if fromFeed, ok := seen.lines[line]; ok {
			return !fromFeed
		}
	}
	f.record(node, line, false)
	return true
}
