package quality

import (
	"path"
	"path/filepath"
	"sort"
	"strings"
	"unicode"
	"unicode/utf16"
)

const (
	MaxQualityFiles    = 200
	MaxFindingsPerFile = 10
	MaxFindingChars    = 200
	MaxPathChars       = 300
	RepositoryLabel    = "(repository)"
)

const (
	SeverityPass = iota
	SeverityInfo
	SeverityWarning
	SeverityHigh
	SeverityCritical
)

type Finding struct {
	Path     string
	Severity int
	Title    string
}

func qtSpace(r rune) bool {
	switch r {
	case '\t', '\n', '\v', '\f', '\r', ' ', 0x85, 0xa0:
		return true
	}
	return r > 0xff && unicode.In(r, unicode.Zs, unicode.Zl, unicode.Zp)
}

func qtTrim(s string) string {
	return strings.TrimFunc(s, qtSpace)
}

func unitsToString(units []uint16) string {
	return string(utf16.Decode(units))
}

func qtLeft(s string, n int) string {
	units := utf16.Encode([]rune(s))
	if len(units) <= n {
		return s
	}
	units = units[:n]
	if len(units) > 0 && utf16.IsSurrogate(rune(units[len(units)-1])) && units[len(units)-1] < 0xdc00 {
		units = units[:len(units)-1]
	}
	return unitsToString(units)
}

func flatText(text string, max int) string {
	var out []uint16
	space := false
	for _, unit := range utf16.Encode([]rune(text)) {
		r := rune(unit)
		surrogate := unit >= 0xd800 && unit <= 0xdfff
		if !surrogate && (qtSpace(r) || unicode.Is(unicode.Cc, r) || unicode.Is(unicode.Cf, r)) {
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
	if len(out) > 0 && out[len(out)-1] >= 0xd800 && out[len(out)-1] < 0xdc00 {
		out = out[:len(out)-1]
	}
	return qtTrim(unitsToString(out))
}

func qtCleanPath(p string) string {
	if p == "" {
		return ""
	}
	return path.Clean(p)
}

func fileName(p string) string {
	if i := strings.LastIndexByte(p, '/'); i >= 0 {
		return p[i+1:]
	}
	return p
}

func RelativeFindingPath(p, root string) string {
	clean := qtTrim(p)
	if clean == "" {
		return RepositoryLabel
	}
	clean = strings.ReplaceAll(clean, "\\", "/")
	if strings.HasPrefix(clean, "/") || strings.Contains(clean, ":") {
		base := qtCleanPath(filepath.ToSlash(root))
		full := qtCleanPath(clean)
		if root != "" && strings.HasPrefix(full, base+"/") {
			clean = full[len(base)+1:]
		} else {
			clean = fileName(full)
		}
	}
	clean = qtCleanPath(clean)
	if strings.HasPrefix(clean, "../") || clean == ".." {
		clean = fileName(clean)
	}
	if clean == "" || clean == "." {
		return RepositoryLabel
	}
	return qtLeft(clean, MaxPathChars)
}

func FindingPenalty(severity int) int {
	switch severity {
	case 1:
		return 2
	case 2:
		return 8
	case 3:
		return 20
	case 4:
		return 35
	}
	return 0
}

func SignalPenalty(severity int) int {
	switch severity {
	case 1:
		return 1
	case 2:
		return 4
	case 3:
		return 12
	case 4:
		return 25
	}
	return 0
}

func GradeFor(score int) string {
	switch {
	case score >= 90:
		return "A"
	case score >= 80:
		return "B"
	case score >= 70:
		return "C"
	case score >= 60:
		return "D"
	}
	return "F"
}

func SeverityLabel(severity int) string {
	switch severity {
	case 1:
		return "Info"
	case 2:
		return "Warning"
	case 3:
		return "High"
	case 4:
		return "Critical"
	}
	return "Pass"
}

func lessUTF16(a, b string) bool {
	ua, ub := utf16.Encode([]rune(a)), utf16.Encode([]rune(b))
	for i := 0; i < len(ua) && i < len(ub); i++ {
		if ua[i] != ub[i] {
			return ua[i] < ub[i]
		}
	}
	return len(ua) < len(ub)
}

func Report(findings []Finding, signalSeverities []int, computedAtMs int64) map[string]any {
	type note struct {
		severity int
		text     string
	}
	type file struct {
		path    string
		penalty int
		notes   []note
	}
	files := map[string]*file{}
	total := 0
	for _, severity := range signalSeverities {
		total += SignalPenalty(severity)
	}
	for _, finding := range findings {
		if finding.Severity <= 0 {
			continue
		}
		total += SignalPenalty(finding.Severity)
		p := RepositoryLabel
		if finding.Path != "" {
			p = qtLeft(finding.Path, MaxPathChars)
		}
		f := files[p]
		if f == nil {
			f = &file{path: p}
			files[p] = f
		}
		f.penalty += FindingPenalty(finding.Severity)
		f.notes = append(f.notes, note{finding.Severity, flatText(SeverityLabel(finding.Severity)+": "+finding.Title, MaxFindingChars)})
	}
	ordered := make([]*file, 0, len(files))
	for _, f := range files {
		ordered = append(ordered, f)
	}
	sort.Slice(ordered, func(i, j int) bool {
		if ordered[i].penalty != ordered[j].penalty {
			return ordered[i].penalty > ordered[j].penalty
		}
		return lessUTF16(ordered[i].path, ordered[j].path)
	})
	rows := []any{}
	for _, f := range ordered {
		if len(rows) >= MaxQualityFiles {
			break
		}
		sort.SliceStable(f.notes, func(i, j int) bool { return f.notes[i].severity > f.notes[j].severity })
		texts := []any{}
		for _, n := range f.notes {
			if len(texts) >= MaxFindingsPerFile {
				break
			}
			texts = append(texts, n.text)
		}
		score := 100 - f.penalty
		if score < 0 {
			score = 0
		}
		rows = append(rows, map[string]any{"path": f.path, "score": score, "findings": texts})
	}
	score := 100 - total
	if score < 0 {
		score = 0
	}
	if score > 100 {
		score = 100
	}
	return map[string]any{
		"score":        score,
		"grade":        GradeFor(score),
		"computedAt":   float64(computedAtMs),
		"findingCount": len(findings),
		"files":        rows,
	}
}
