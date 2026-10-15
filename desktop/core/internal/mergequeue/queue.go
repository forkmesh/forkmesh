package mergequeue

import (
	"strconv"
	"strings"
	"unicode"
	"unicode/utf16"
)

const (
	StateQueued   = "queued"
	StateUpdating = "updating"
	StateMerging  = "merging"
	StateBlocked  = "blocked"
	StateFailed   = "failed"

	maxDetailChars = 200
)

type Entry struct {
	Number     int
	QueuedAtMs int64
	State      string
	Detail     string
	StateAtMs  int64
}

type Queue struct {
	entries []Entry
}

func knownState(state string) bool {
	switch state {
	case StateQueued, StateUpdating, StateMerging, StateBlocked, StateFailed:
		return true
	}
	return false
}

func qtTrimmed(text string) string {
	return strings.TrimFunc(text, qtIsSpace)
}

func qtIsSpace(r rune) bool {
	return unicode.IsSpace(r) || r == '\u0085' || r == ' '
}

func qtSimplified(text string) string {
	return strings.Join(strings.FieldsFunc(text, qtIsSpace), " ")
}

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

func leftUTF16(text string, n int) string {
	if n <= 0 {
		return ""
	}
	units := utf16.Encode([]rune(text))
	if len(units) <= n {
		return text
	}
	return string(utf16.Decode(units[:n]))
}

func qtToInt(text string) (int, bool) {
	value, err := strconv.ParseInt(text, 10, 32)
	if err != nil {
		return 0, false
	}
	return int(value), true
}

func qtToLongLong(text string) int64 {
	value, err := strconv.ParseInt(text, 10, 64)
	if err != nil {
		return 0
	}
	return value
}

func SanitizeDetail(detail string) string {
	flat := strings.ReplaceAll(qtSimplified(detail), "\t", " ")
	if utf16Len(flat) > maxDetailChars {
		flat = leftUTF16(flat, maxDetailChars-1) + "…"
	}
	return flat
}

func FromRows(rows []string) Queue {
	var q Queue
	for _, row := range rows {
		parts := strings.Split(row, "\t")
		number, ok := qtToInt(qtTrimmed(parts[0]))
		if !ok || number <= 0 || q.Contains(number) {
			continue
		}
		entry := Entry{Number: number, State: StateQueued}
		if len(parts) > 1 {
			entry.QueuedAtMs = qtToLongLong(qtTrimmed(parts[1]))
		}
		if len(parts) > 2 {
			state := qtTrimmed(parts[2])
			if knownState(state) && state != StateUpdating && state != StateMerging {
				entry.State = state
			}
		}
		if len(parts) > 3 {
			entry.Detail = SanitizeDetail(strings.Join(parts[3:], " "))
		}
		entry.StateAtMs = entry.QueuedAtMs
		q.entries = append(q.entries, entry)
	}
	return q
}

func (q Queue) Rows() []string {
	rows := make([]string, 0, len(q.entries))
	for _, e := range q.entries {
		rows = append(rows, strconv.Itoa(e.Number)+"\t"+strconv.FormatInt(e.QueuedAtMs, 10)+"\t"+e.State+"\t"+SanitizeDetail(e.Detail))
	}
	return rows
}

func (q Queue) Entries() []Entry { return append([]Entry(nil), q.entries...) }
func (q Queue) Size() int        { return len(q.entries) }
func (q Queue) Empty() bool      { return len(q.entries) == 0 }

func (q Queue) IndexOf(number int) int {
	for i, e := range q.entries {
		if e.Number == number {
			return i
		}
	}
	return -1
}

func (q Queue) Contains(number int) bool { return q.IndexOf(number) >= 0 }

func (q *Queue) Enqueue(number int, nowMs int64) bool {
	if number <= 0 || q.Contains(number) {
		return false
	}
	q.entries = append(q.entries, Entry{Number: number, QueuedAtMs: nowMs, StateAtMs: nowMs, State: StateQueued})
	return true
}

func (q *Queue) Remove(number int) bool {
	index := q.IndexOf(number)
	if index < 0 {
		return false
	}
	q.entries = append(q.entries[:index:index], q.entries[index+1:]...)
	return true
}

func (q *Queue) Move(number, delta int) bool {
	index := q.IndexOf(number)
	if index < 0 || delta == 0 {
		return false
	}
	target := index + delta
	if target < 0 {
		target = 0
	}
	if target > len(q.entries)-1 {
		target = len(q.entries) - 1
	}
	if target == index {
		return false
	}
	moved := q.entries[index]
	rest := append(append([]Entry(nil), q.entries[:index]...), q.entries[index+1:]...)
	out := append(append(append([]Entry(nil), rest[:target]...), moved), rest[target:]...)
	q.entries = out
	return true
}

func (q *Queue) Clear() { q.entries = nil }

func (q *Queue) SetState(number int, state, detail string, nowMs int64) bool {
	index := q.IndexOf(number)
	if index < 0 {
		return false
	}
	if !knownState(state) {
		state = StateQueued
	}
	q.entries[index].State = state
	q.entries[index].Detail = SanitizeDetail(detail)
	q.entries[index].StateAtMs = nowMs
	return true
}

func (q Queue) Front() Entry {
	if len(q.entries) == 0 {
		return Entry{State: StateQueued}
	}
	return q.entries[0]
}

func (q Queue) At(number int) Entry {
	index := q.IndexOf(number)
	if index < 0 {
		return Entry{State: StateQueued}
	}
	return q.entries[index]
}

func (q Queue) Summary() string {
	if len(q.entries) == 0 {
		return "Empty"
	}
	blocked, running := 0, 0
	for _, e := range q.entries {
		switch e.State {
		case StateBlocked, StateFailed:
			blocked++
		case StateUpdating, StateMerging:
			running++
		}
	}
	text := strconv.Itoa(len(q.entries)) + " queued"
	if running > 0 {
		text += " · " + strconv.Itoa(running) + " in progress"
	}
	if blocked > 0 {
		text += " · " + strconv.Itoa(blocked) + " blocked"
	}
	return text
}
