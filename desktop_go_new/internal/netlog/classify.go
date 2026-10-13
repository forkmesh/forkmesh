package netlog

import (
	"regexp"
	"strings"
	"unicode"
	"unicode/utf8"
)

type rule struct {
	needle string
	badge  string
}

var prefixRules = []rule{
	{"cloud hit:", "CLOUD"},
	{"cloud:", "CLOUD"},
	{"actions:", "ACTIONS"},
	{"action ", "ACTIONS"},
	{"integrity pin:", "PIN"},
	{"mirror:", "MIRROR"},
	{"mirror ", "MIRROR"},
	{"preview cache:", "MIRROR"},
	{"catalog", "PUBLISH"},
	{"fediverse:", "PUBLISH"},
	{"status:", "STATUS"},
	{"network:", "NETWORK"},
	{"git:", "GIT"},
	{"push to ", "GIT"},
	{"import:", "GIT"},
	{"agents:", "PROMPT"},
	{"control node:", "NODE"},
	{"node connected", "PEER"},
	{"mainnode link", "PEER"},
	{"office chat:", "PEER"},
	{"chat:", "PEER"},
	{"encryption:", "CRYPTO"},
	{"identity:", "IDENTITY"},
	{"account:", "ACCOUNT"},
	{"admin:", "ADMIN"},
	{"session ", "SESSION"},
	{"[startup +", "SESSION"},
	{"[restart +", "SESSION"},
}

var wordRules = []rule{
	{"session started", "SESSION"},
	{"session ended", "SESSION"},
	{"quick update started", "SESSION"},
	{"rebuild & restart started", "SESSION"},
	{"restarting now", "SESSION"},
	{"firewall", "NETWORK"},
	{"not backgrounded", "BGBLOCK"},
	{"backgrounded", "BGTASK"},
	{"background", "BGNOTE"},
	{"integrity pin", "PIN"},
	{"bounty", "BOUNTY"},
	{"escrow", "BOUNTY"},
	{"workflow", "ACTIONS"},
	{"actions", "ACTIONS"},
	{"pull request", "PULL"},
	{"pull #", "PULL"},
	{"pull", "PULL"},
	{"merged", "MERGE"},
	{"merge", "MERGE"},
	{"issue #", "ISSUE"},
	{"issue", "ISSUE"},
	{"mirror", "MIRROR"},
	{"mirroring", "MIRROR"},
	{"catalog", "PUBLISH"},
	{"publish", "PUBLISH"},
	{"published", "PUBLISH"},
	{"publishing", "PUBLISH"},
	{"release", "PUBLISH"},
	{"forked into", "FORKED"},
	{"forked", "FORKED"},
	{"fork of", "FORKED"},
	{"forking", "FORKED"},
	{"new fork", "FORKED"},
	{"account", "ACCOUNT"},
	{"sync", "SYNC"},
	{"synced", "SYNC"},
	{"syncing", "SYNC"},
	{"git", "GIT"},
	{"push", "GIT"},
	{"pushed", "GIT"},
	{"commit", "GIT"},
	{"committed", "GIT"},
	{"patch", "GIT"},
	{"branch", "GIT"},
	{"worktree", "GIT"},
	{"prompt", "PROMPT"},
	{"agent", "PROMPT"},
	{"admin", "ADMIN"},
	{"identity", "IDENTITY"},
	{"encryption", "CRYPTO"},
	{"peer", "PEER"},
	{"node", "NODE"},
	{"connected", "NODE"},
	{"network", "NETWORK"},
	{"status", "STATUS"},
}

var routeRules = []rule{
	{"relay sync", "SYNC"},
	{"repo fetch", "SYNC"},
	{"mirror sync", "MIRROR"},
	{"catalog fetch", "SYNC"},
	{"release fetch", "PUBLISH"},
	{"issue fetch", "ISSUE"},
	{"pull fetch", "PULL"},
	{"agent poll", "PROMPT"},
	{"forkbot chat", "PROMPT"},
	{"chat", "PEER"},
	{"account", "ACCOUNT"},
	{"ping inbox", "ACCOUNT"},
	{"version check", "SESSION"},
	{"security report", "CRYPTO"},
	{"network stats", "NETWORK"},
	{"traffic metrics", "NETWORK"},
	{"request", "NETWORK"},
}

var (
	nodePrefixPattern = regexp.MustCompile(`^Node ([nrab][0-9]{1,4}) (core|runner) ([A-Za-z0-9@._:-]+) ([a-z]+): `)
	replayedPattern   = regexp.MustCompile(` \(logged [0-9-]+ [0-9:]+, replayed\)$`)
)

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

func qtLetterOrNumber(r rune) bool {
	return unicode.IsLetter(r) || unicode.IsNumber(r)
}

func qtToInt(s string) (int, bool) {
	s = qtTrim(s)
	if s == "" {
		return 0, false
	}
	negative := false
	switch s[0] {
	case '+':
		s = s[1:]
	case '-':
		negative = true
		s = s[1:]
	}
	if s == "" {
		return 0, false
	}
	value := int64(0)
	for i := 0; i < len(s); i++ {
		c := s[i]
		if c < '0' || c > '9' {
			return 0, false
		}
		value = value*10 + int64(c-'0')
		if value > 1<<31 {
			return 0, false
		}
	}
	if negative {
		value = -value
	}
	if value > 1<<31-1 || value < -(1<<31) {
		return 0, false
	}
	return int(value), true
}

func fieldsSkipEmpty(s string, sep string) []string {
	parts := strings.Split(s, sep)
	out := parts[:0]
	for _, part := range parts {
		if part != "" {
			out = append(out, part)
		}
	}
	return out
}

func nodeLineBadge(message string) string {
	if !strings.HasPrefix(message, "Node ") {
		return ""
	}
	match := nodePrefixPattern.FindStringSubmatch(message)
	if match == nil {
		return ""
	}
	if match[4] == "err" && !replayedPattern.MatchString(message) {
		return "ERROR"
	}
	unit := strings.ToLower(match[3])
	switch {
	case strings.HasPrefix(unit, "rqlite"):
		return "DB"
	case unit == "feed":
		return "CLOUD"
	case unit == "forkmesh-app":
		body := strings.Index(message, ": ")
		if strings.HasPrefix(message[body+2:], "cron") {
			return "CRON"
		}
		return "APP"
	case strings.HasPrefix(unit, "caddy"):
		return "WEB"
	case strings.HasPrefix(unit, "forkmesh"):
		return "DAEMON"
	case strings.HasPrefix(unit, "fmnext-ids"):
		return "IDS"
	case strings.HasPrefix(unit, "fmnext-cert"):
		return "CERT"
	case strings.HasPrefix(unit, "postfix") || strings.HasPrefix(unit, "opendkim") ||
		strings.HasPrefix(unit, "fmnext-mail-events"):
		return "MAIL"
	case unit == "job" || unit == "run" || strings.HasPrefix(unit, "fmnext-job"):
		return "JOB"
	case strings.HasPrefix(unit, "fmnext-") || strings.HasPrefix(unit, "earlyoom") ||
		strings.HasPrefix(unit, "fail2ban"):
		return "OPS"
	}
	return "SYSTEM"
}

func requestStatus(withoutBody string) (string, bool) {
	if !strings.HasPrefix(withoutBody, "net ") {
		return "", false
	}
	parts := fieldsSkipEmpty(withoutBody, " ")
	if len(parts) < 3 {
		return "", false
	}
	return parts[2], true
}

func requestSucceeded(withoutBody, body string) bool {
	status, ok := requestStatus(withoutBody)
	if !ok {
		return false
	}
	code, numeric := qtToInt(status)
	if !numeric || code < 200 || code >= 400 {
		return false
	}
	return strings.Contains(strings.ReplaceAll(body, " ", ""), `"ok":true`)
}

func requestExpected(withoutBody string) bool {
	status, ok := requestStatus(withoutBody)
	return ok && status == "expected"
}

func requestFailed(withoutBody string) bool {
	status, ok := requestStatus(withoutBody)
	if !ok {
		return false
	}
	if status == "err" {
		return true
	}
	code, numeric := qtToInt(status)
	return numeric && code >= 400
}

func withoutAddressTokens(text string) string {
	if !strings.Contains(text, "/") {
		return text
	}
	tokens := strings.Split(text, " ")
	kept := make([]string, 0, len(tokens))
	for _, token := range tokens {
		if !strings.Contains(token, "/") {
			kept = append(kept, token)
		}
	}
	return strings.Join(kept, " ")
}

var tallyWords = []string{"failed", "failure", "failures", "failing", "fail", "errors", "error"}

func asciiLowerOrDigit(c byte) bool {
	return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
}

func pcreSpace(c byte) bool {
	return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r'
}

func clearedTallyAt(text string, at int) int {
	if at > 0 && asciiLowerOrDigit(text[at-1]) {
		return -1
	}
	pos := at
	switch {
	case text[pos] == '0':
		pos++
	case strings.HasPrefix(text[pos:], "no"):
		pos += 2
	default:
		return -1
	}
	start := pos
	for pos < len(text) && pcreSpace(text[pos]) {
		pos++
	}
	if pos == start {
		return -1
	}
	for _, word := range tallyWords {
		if !strings.HasPrefix(text[pos:], word) {
			continue
		}
		end := pos + len(word)
		if end < len(text) && text[end] >= 'a' && text[end] <= 'z' {
			continue
		}
		return end
	}
	return -1
}

func withoutClearedTallies(lower string) string {
	var out strings.Builder
	last := 0
	for i := 0; i < len(lower); {
		if end := clearedTallyAt(lower, i); end > i {
			out.WriteString(lower[last:i])
			last = end
			i = end
			continue
		}
		i++
	}
	if last == 0 {
		return lower
	}
	out.WriteString(lower[last:])
	return out.String()
}

func containsLogWord(text, word string) bool {
	from := 0
	for {
		idx := strings.Index(text[from:], word)
		if idx < 0 {
			return false
		}
		at := from + idx
		end := at + len(word)
		if end < len(text) && text[end] == 's' {
			end++
		}
		leftClear := true
		if at > 0 {
			r, _ := utf8.DecodeLastRuneInString(text[:at])
			leftClear = !qtLetterOrNumber(r)
		}
		rightClear := true
		if end < len(text) {
			r, _ := utf8.DecodeRuneInString(text[end:])
			rightClear = !qtLetterOrNumber(r)
		}
		if leftClear && rightClear {
			return true
		}
		from = at + 1
	}
}

func routeBadge(netLine string) string {
	event := ""
	if dot := strings.LastIndex(netLine, "·"); dot >= 0 {
		event = qtTrim(netLine[dot+len("·"):])
	}
	if event != "" {
		for _, r := range routeRules {
			if event == r.needle {
				return r.badge
			}
		}
	}
	return "NETWORK"
}

func MessageBadge(message string) string {
	if badge := nodeLineBadge(message); badge != "" {
		return badge
	}
	lower := strings.ToLower(message)
	if strings.Contains(lower, "ui stalled") {
		return "STALL"
	}
	if strings.HasPrefix(lower, "cloud hit: ") {
		return "CLOUD"
	}
	if strings.HasPrefix(lower, "cloud d1 ") {
		return "DB"
	}
	if strings.HasPrefix(lower, "git: merged ") && strings.Contains(lower, " in its linked worktree") {
		return "MERGE"
	}
	if strings.HasPrefix(lower, "push to ") && strings.Contains(lower, " on ") && strings.Contains(lower, "→") {
		return "GIT"
	}
	if strings.HasPrefix(lower, "background ") {
		if strings.Contains(lower, " not backgrounded") {
			return "BGBLOCK"
		}
		if strings.Contains(lower, " backgrounded") {
			return "BGTASK"
		}
	}
	forRules := lower
	body := ""
	if bodyStart := strings.Index(forRules, "[body:"); bodyStart >= 0 {
		bodyEnd := strings.LastIndex(forRules, "]")
		length := len(forRules) - bodyStart
		if bodyEnd > bodyStart {
			length = bodyEnd - bodyStart + 1
		}
		body = forRules[bodyStart : bodyStart+length]
		forRules = forRules[:bodyStart] + forRules[bodyStart+length:]
	}
	if requestFailed(forRules) {
		return "ERROR"
	}
	source := lower
	if requestSucceeded(forRules, body) {
		source = forRules
	}
	forErrors := withoutClearedTallies(withoutAddressTokens(source))
	if !requestExpected(forRules) {
		for _, needle := range []string{"fail", "error", "could not", "couldn't", "no live", "denied", "blocks ", "unable"} {
			if strings.Contains(forErrors, needle) {
				return "ERROR"
			}
		}
	}
	if strings.HasPrefix(forRules, "net ") {
		return routeBadge(forRules)
	}
	scan := qtTrim(withoutAddressTokens(forRules))
	for _, r := range prefixRules {
		if strings.HasPrefix(scan, r.needle) {
			return r.badge
		}
	}
	for _, r := range wordRules {
		if containsLogWord(scan, r.needle) {
			return r.badge
		}
	}
	return "INFO"
}

const (
	severityAutomatic = iota
	severityInformational
	severityError
)

func splitLogSource(message string) (string, int) {
	runes := []rune(message)
	at := len(runes) - 1
	if at < 0 || runes[at] != ']' {
		return message, severityAutomatic
	}
	lineEnd := at
	severity := severityAutomatic
	strip := func(suffix string, value int) bool {
		suffixRunes := []rune(suffix)
		start := lineEnd - len(suffixRunes)
		if start >= 0 && string(runes[start:lineEnd]) == suffix {
			lineEnd = start
			severity = value
			return true
		}
		return false
	}
	if !strip("|ERROR", severityError) {
		strip("|INFO", severityInformational)
	}
	at = lineEnd
	for at > 0 && unicode.IsDigit(runes[at-1]) {
		at--
	}
	if at == lineEnd {
		return message, severityAutomatic
	}
	if at == 0 || runes[at-1] != ':' {
		return message, severityAutomatic
	}
	at--
	pathEnd := at
	for at > 0 {
		c := runes[at-1]
		if c == '[' || c == ']' || qtSpace(c) {
			break
		}
		at--
	}
	if at == pathEnd || at == 0 || runes[at-1] != '[' {
		return message, severityAutomatic
	}
	at--
	if at == 0 || !qtSpace(runes[at-1]) {
		return message, severityAutomatic
	}
	bodyEnd := at
	for bodyEnd > 0 && qtSpace(runes[bodyEnd-1]) {
		bodyEnd--
	}
	return string(runes[:bodyEnd]), severity
}

func utf16Offset(s string, units int) (int, bool) {
	count := 0
	for i, r := range s {
		if count == units {
			return i, true
		}
		if r >= 0x10000 {
			count += 2
		} else {
			count++
		}
		if count > units {
			return 0, false
		}
	}
	if count == units {
		return len(s), true
	}
	return 0, false
}

func storedMessage(line string) string {
	if at10, ok := utf16Offset(line, 10); ok && at10 < len(line) && line[at10] == ' ' {
		if at21, ok := utf16Offset(line, 21); ok {
			return line[at21:]
		}
	}
	return line
}

func Badge(storedLine string) string {
	body, severity := splitLogSource(storedMessage(storedLine))
	switch severity {
	case severityInformational:
		return "INFO"
	case severityError:
		return "ERROR"
	}
	return MessageBadge(body)
}

func SeverityForBadge(badge string) string {
	switch strings.ToUpper(qtTrim(badge)) {
	case "ERROR", "CRASH", "FATAL":
		return "error"
	case "STALL", "BGBLOCK", "WARN", "WARNING":
		return "warning"
	}
	return "info"
}
