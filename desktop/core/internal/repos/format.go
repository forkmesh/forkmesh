package repos

import (
	"bytes"
	"encoding/json"
	"regexp"
	"strconv"
	"strings"
	"unicode"
	"unicode/utf16"
)

const (
	DefaultCommits  = 50
	MaxCommits      = 200
	MaxBranches     = 300
	MaxSubjectChars = 300
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

func dropped(unit uint16) bool {
	if utf16.IsSurrogate(rune(unit)) {
		return false
	}
	r := rune(unit)
	return qtSpace(r) || unicode.Is(unicode.Cc, r) || unicode.Is(unicode.Cf, r)
}

func qtTrim(text string) string {
	return strings.TrimFunc(text, qtSpace)
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

func flatText(text string, max int) string {
	units := utf16.Encode([]rune(text))
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
	return qtTrim(string(utf16.Decode(out)))
}

func qtLongLong(text string) int64 {
	n, err := strconv.ParseInt(qtTrim(text), 10, 64)
	if err != nil {
		return 0
	}
	return n
}

func clampLimit(value json.RawMessage, fallback, max int) int {
	wanted := float64(fallback)
	var number float64
	trimmed := bytes.TrimSpace(value)
	if len(trimmed) > 0 && (trimmed[0] == '-' || (trimmed[0] >= '0' && trimmed[0] <= '9')) && json.Unmarshal(trimmed, &number) == nil {
		wanted = number
	}
	if wanted >= float64(max) {
		return max
	}
	if wanted < 1 {
		return 1
	}
	n := int(wanted)
	if n < 1 {
		return 1
	}
	return n
}

var slugPattern = regexp.MustCompile(`^([A-Za-z0-9_.-]{1,100})/([A-Za-z0-9_.-]{1,100})$`)

func parseRepoSlug(slug string) (string, string, bool) {
	match := slugPattern.FindStringSubmatch(qtTrim(slug))
	if match == nil {
		return "", "", false
	}
	owner, name := match[1], match[2]
	if owner == "." || owner == ".." || name == "." || name == ".." {
		return "", "", false
	}
	return owner, name, true
}

var branchPattern = regexp.MustCompile(`^[A-Za-z0-9_./-]{1,200}$`)

func validBranchName(branch string) bool {
	return branchPattern.MatchString(branch) && !strings.HasPrefix(branch, "-") &&
		!strings.HasPrefix(branch, "/") && !strings.HasSuffix(branch, "/") &&
		!strings.Contains(branch, "..") && !strings.Contains(branch, "//") &&
		!strings.HasSuffix(branch, ".lock")
}

const commitLogFormat = "--format=%H%x1f%an%x1f%at%x1f%s%x1e"

const branchRefFormat = "--format=%(HEAD)%1f%(refname:short)%1f%(objectname)%1f" +
	"%(committerdate:unix)%1f%(upstream:short)%1f%(upstream:track)%1f" +
	"%(contents:subject)"

func parseCommits(output []byte, max int) []map[string]any {
	commits := []map[string]any{}
	for _, raw := range strings.Split(string(output), "\x1e") {
		if len(commits) >= max {
			break
		}
		record := qtTrim(raw)
		if record == "" {
			continue
		}
		fields := strings.Split(record, "\x1f")
		if len(fields) < 4 || utf16Len(fields[0]) < 7 {
			continue
		}
		commits = append(commits, map[string]any{
			"sha":     fields[0],
			"author":  flatText(fields[1], 120),
			"atMs":    qtLongLong(fields[2]) * 1000,
			"subject": flatText(strings.Join(fields[3:], "\x1f"), MaxSubjectChars),
		})
	}
	return commits
}

var (
	aheadPattern  = regexp.MustCompile(`ahead (\d+)`)
	behindPattern = regexp.MustCompile(`behind (\d+)`)
)

func parseTrack(track string) (int, int, bool) {
	count := func(pattern *regexp.Regexp) int {
		match := pattern.FindStringSubmatch(track)
		if match == nil {
			return 0
		}
		n, err := strconv.ParseInt(match[1], 10, 32)
		if err != nil {
			return 0
		}
		return int(n)
	}
	return count(aheadPattern), count(behindPattern), strings.Contains(track, "gone")
}

func parseBranches(output []byte, max int) []map[string]any {
	branches := []map[string]any{}
	for _, line := range strings.Split(string(output), "\n") {
		if len(branches) >= max {
			break
		}
		fields := strings.Split(line, "\x1f")
		if len(fields) < 7 || qtTrim(fields[1]) == "" {
			continue
		}
		ahead, behind, gone := parseTrack(fields[5])
		branches = append(branches, map[string]any{
			"name":         qtTrim(fields[1]),
			"current":      qtTrim(fields[0]) == "*",
			"sha":          qtTrim(fields[2]),
			"atMs":         qtLongLong(fields[3]) * 1000,
			"upstream":     qtTrim(fields[4]),
			"ahead":        ahead,
			"behind":       behind,
			"upstreamGone": gone,
			"subject":      flatText(strings.Join(fields[6:], "\x1f"), MaxSubjectChars),
		})
	}
	return branches
}

func commitsArgs(limit int, ref string) []string {
	args := []string{"log", commitLogFormat, "-n", strconv.Itoa(limit)}
	if ref != "" {
		args = append(args, "refs/heads/"+ref)
	}
	return append(args, "--")
}

func branchesArgs(limit int) []string {
	return []string{"for-each-ref", "--sort=-committerdate", "--count=" + strconv.Itoa(limit), branchRefFormat, "refs/heads"}
}
