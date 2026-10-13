package update

import (
	"regexp"
	"strings"
	"unicode"
	"unicode/utf16"
)

const (
	maxBacklogLines = 300
	maxBatchLines   = 150
	maxBatchChars   = 7000
	maxLineChars    = 400
)

var ansiSequence = regexp.MustCompile("\x1b\\[[0-9;?]*[ -/]*[@-~]")

func utf16Len(text string) int {
	n := 0
	for _, r := range text {
		if r >= 0x10000 {
			n += 2
		} else {
			n++
		}
	}
	return n
}

func utf16Left(text string, units int) string {
	encoded := utf16.Encode([]rune(text))
	if len(encoded) <= units {
		return text
	}
	return string(utf16.Decode(encoded[:units]))
}

func cleanLine(line string) string {
	out := ansiSequence.ReplaceAllString(line, "")
	out = strings.ReplaceAll(out, "\x1b", "")
	out = strings.TrimRightFunc(out, unicode.IsSpace)
	if utf16Len(out) > maxLineChars {
		out = utf16Left(out, maxLineChars-1) + "…"
	}
	return out
}

type Feed struct {
	lines      []string
	seq        int64
	run        int
	carry      string
	status     string
	errorText  string
	relaunched bool
}

func (f *Feed) Reset() {
	f.lines = nil
	f.carry = ""
	f.status = ""
	f.errorText = ""
	f.relaunched = false
	f.run++
}

func (f *Feed) push(line string) {
	clean := cleanLine(line)
	if strings.TrimSpace(clean) == "" {
		return
	}
	f.lines = append(f.lines, clean)
	f.seq++
	if len(f.lines) > maxBacklogLines {
		f.lines = append([]string(nil), f.lines[len(f.lines)-maxBacklogLines:]...)
	}
}

func (f *Feed) Append(chunk string) {
	if chunk == "" {
		return
	}
	text := f.carry + chunk
	start := 0
	for i := 0; i < len(text); i++ {
		if text[i] != '\n' && text[i] != '\r' {
			continue
		}
		f.push(text[start:i])
		start = i + 1
	}
	rest := text[start:]
	if limit := maxLineChars * 4; len(rest) > limit {
		cut := len(rest) - limit
		for cut < len(rest) && !utf8Start(rest[cut]) {
			cut++
		}
		rest = rest[cut:]
	}
	f.carry = rest
}

func utf8Start(b byte) bool { return b&0xC0 != 0x80 }

func (f *Feed) NoteStatus(status string, isError bool) {
	f.status = status
	if isError {
		f.errorText = status
	}
	f.relaunched = !isError && strings.HasPrefix(status, "Relaunching")
}

func (f *Feed) Seq() int64        { return f.seq }
func (f *Feed) Run() int          { return f.run }
func (f *Feed) Status() string    { return f.status }
func (f *Feed) Error() string     { return f.errorText }
func (f *Feed) Relaunching() bool { return f.relaunched }

func (f *Feed) Backlog(maxLines int) []string {
	if maxLines < 0 {
		maxLines = 0
	}
	from := len(f.lines) - maxLines
	if from < 0 {
		from = 0
	}
	return append([]string{}, f.lines[from:]...)
}

type Batch struct {
	First   int64    `json:"first"`
	Seq     int64    `json:"seq"`
	Skipped int64    `json:"skipped"`
	Lines   []string `json:"lines"`
}

func (f *Feed) BatchSince(sentSeq int64) Batch {
	if sentSeq > f.seq || sentSeq < 0 {
		sentSeq = 0
	}
	firstKept := f.seq - int64(len(f.lines)) + 1
	wanted := sentSeq + 1
	floor := wanted
	if firstKept > floor {
		floor = firstKept
	}
	first := f.seq + 1
	chars := 0
	for s := f.seq; s >= floor; s-- {
		line := f.lines[s-firstKept]
		size := utf16Len(line)
		if f.seq-s >= maxBatchLines || (chars+size > maxBatchChars && s != f.seq) {
			break
		}
		chars += size
		first = s
	}
	lines := []string{}
	for s := first; s <= f.seq; s++ {
		lines = append(lines, f.lines[s-firstKept])
	}
	skipped := int64(0)
	if len(lines) > 0 {
		skipped = first - wanted
	}
	return Batch{First: first, Seq: f.seq, Skipped: skipped, Lines: lines}
}

type RunState struct {
	Running  bool
	Kind     string
	Percent  int
	Phase    string
	Activity string
}

func stateObject(f *Feed, run RunState) map[string]any {
	percent := run.Percent
	if percent < 0 {
		percent = 0
	}
	if percent > 100 {
		percent = 100
	}
	return map[string]any{
		"run":         f.Run(),
		"running":     run.Running,
		"kind":        run.Kind,
		"percent":     percent,
		"phase":       run.Phase,
		"activity":    run.Activity,
		"status":      f.Status(),
		"error":       f.Error(),
		"relaunching": f.Relaunching(),
	}
}
