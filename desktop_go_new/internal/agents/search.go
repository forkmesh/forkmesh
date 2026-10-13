package agents

import (
	"regexp"
	"strings"
	"unicode"
	"unicode/utf16"
)

var attachmentMarker = regexp.MustCompile(`Attached (?:image|video):[ \t]*([^"\\\r\n]+)`)

func AttachmentPathsIn(text string) []string {
	paths := []string{}
	if !strings.Contains(text, "Attached image:") && !strings.Contains(text, "Attached video:") {
		return paths
	}
	for _, match := range attachmentMarker.FindAllStringSubmatch(text, -1) {
		path := trimmed(match[1])
		if path != "" && !containsString(paths, path, false) {
			paths = append(paths, path)
		}
	}
	return paths
}

func foldRune(r rune) rune {
	if r < 0x80 {
		if r >= 'a' && r <= 'z' {
			return r - 'a' + 'A'
		}
		return r
	}
	least := r
	for f := unicode.SimpleFold(r); f != r; f = unicode.SimpleFold(f) {
		if f < least {
			least = f
		}
	}
	if utf16.RuneLen(least) != utf16.RuneLen(r) {
		return r
	}
	return least
}

func toUnits(text string) ([]uint16, []uint16) {
	plain := make([]uint16, 0, len(text))
	folded := make([]uint16, 0, len(text))
	for _, r := range text {
		plain = utf16.AppendRune(plain, r)
		folded = utf16.AppendRune(folded, foldRune(r))
	}
	return plain, folded
}

func foldUnits(text string) []uint16 {
	_, folded := toUnits(text)
	return folded
}

func indexUnits(haystack, needle []uint16, from int) int {
	n := len(needle)
	if n == 0 {
		return -1
	}
	first := needle[0]
	last := len(haystack) - n
	for i := from; i <= last; i++ {
		if haystack[i] != first {
			continue
		}
		match := true
		for j := 1; j < n; j++ {
			if haystack[i+j] != needle[j] {
				match = false
				break
			}
		}
		if match {
			return i
		}
	}
	return -1
}

func hitSnippet(text []uint16, at, length int) string {
	const context = 60
	from := max(0, at-context)
	to := min(len(text), at+length+context)
	snippet := simplified(string(utf16.Decode(text[from:to])))
	if from > 0 {
		snippet = "…" + snippet
	}
	if to < len(text) {
		snippet += "…"
	}
	return snippet
}

type SearchHit struct {
	ID      int    `json:"id"`
	Count   int    `json:"count"`
	Snippet string `json:"snippet"`
}

func (s *Store) SearchTranscript(session Session, needle string) (int, string) {
	query := trimmed(needle)
	if query == "" {
		return 0, ""
	}
	wanted := foldUnits(query)
	hits := 0
	snippet := ""
	for _, path := range []string{s.LogPath(session), s.EventsPath(session)} {
		text := readTranscriptTail(path, TranscriptSearchTailBytes)
		if text == "" {
			continue
		}
		plain, folded := toUnits(text)
		at := indexUnits(folded, wanted, 0)
		for at >= 0 {
			if snippet == "" {
				snippet = hitSnippet(plain, at, len(wanted))
			}
			hits++
			at = indexUnits(folded, wanted, at+len(wanted))
		}
	}
	return hits, snippet
}
