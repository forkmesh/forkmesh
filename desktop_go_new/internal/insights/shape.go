package insights

import (
	"regexp"
	"strconv"
	"strings"
)

func isIdentStart(c byte) bool {
	return c == '_' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
}

func isIdentChar(c byte) bool {
	return isIdentStart(c) || (c >= '0' && c <= '9')
}

func matchHeredoc(text string, from int) (end int, name string, ok bool) {
	for p := from; p+1 < len(text); p++ {
		if text[p] != '<' || text[p+1] != '<' {
			continue
		}
		i := p + 2
		if i < len(text) && text[i] == '-' {
			i++
		}
		for i < len(text) && asciiSpace(rune(text[i])) {
			i++
		}
		var quote byte
		if i < len(text) && (text[i] == '\'' || text[i] == '"') {
			quote = text[i]
			i++
		}
		if i >= len(text) || !isIdentStart(text[i]) {
			continue
		}
		start := i
		for i < len(text) && isIdentChar(text[i]) {
			i++
		}
		ident := text[start:i]
		if quote != 0 {
			if i >= len(text) || text[i] != quote {
				continue
			}
			i++
		}
		return i, ident, true
	}
	return 0, "", false
}

func stripHeredocs(text string) string {
	for from, guard := 0, 0; guard < 8; guard++ {
		end, name, ok := matchHeredoc(text, from)
		if !ok {
			break
		}
		eolRel := strings.IndexByte(text[end:], '\n')
		if eolRel < 0 {
			text = text[:end]
			break
		}
		eol := end + eolRel
		terminator := "\n" + name
		found := indexFrom(text, terminator, eol)
		for found >= 0 && found+len(terminator) < len(text) && text[found+len(terminator)] != '\n' {
			found = indexFrom(text, terminator, found+1)
		}
		if found < 0 {
			text = text[:eol]
			break
		}
		text = text[:eol] + text[found+len(terminator):]
		from = eol
	}
	return text
}

func indexFrom(s, sub string, from int) int {
	if from > len(s) {
		return -1
	}
	i := strings.Index(s[from:], sub)
	if i < 0 {
		return -1
	}
	return from + i
}

func topLevelSegments(text string) []string {
	segments := []string{}
	var current strings.Builder
	depth := 0
	var quote rune
	runes := []rune(text)
	for i := 0; i < len(runes); i++ {
		c := runes[i]
		if quote != 0 {
			current.WriteRune(c)
			if c == '\\' && quote == '"' && i+1 < len(runes) {
				i++
				current.WriteRune(runes[i])
			} else if c == quote {
				quote = 0
			}
			continue
		}
		if c == '\\' && i+1 < len(runes) {
			current.WriteRune(c)
			i++
			current.WriteRune(runes[i])
			continue
		}
		if c == '\'' || c == '"' {
			quote = c
			current.WriteRune(c)
			continue
		}
		if c == '(' || c == '{' {
			depth++
		} else if (c == ')' || c == '}') && depth > 0 {
			depth--
		}
		if depth == 0 && (c == ';' || c == '\n') {
			segments = append(segments, current.String())
			current.Reset()
			continue
		}
		current.WriteRune(c)
	}
	return append(segments, current.String())
}

var (
	shellKeywords   = map[string]bool{"do": true, "done": true, "then": true, "else": true, "elif": true, "fi": true, "esac": true, "}": true, ")": true}
	separatorRe     = regexp.MustCompile(`^(?:&&|\|\||;|\|)`)
	assignmentRe    = regexp.MustCompile(`^[A-Za-z_][A-Za-z0-9_]*=[^\t\n\v\f\r ]*$`)
	interpreterRe   = regexp.MustCompile(`^(python[0-9.]*|node|ruby|perl|bash|sh|zsh|dart)$`)
	setupCommands   = map[string]bool{"cd": true, "set": true, "export": true, "pushd": true, "mkdir": true, "rm": true, "cp": true, "mv": true, "touch": true, "chmod": true, "echo": true, "printf": true, "true": true, "source": true, ".": true, "ulimit": true, "umask": true, "date": true}
	multiVerbTools  = map[string]bool{"git": true, "go": true, "cargo": true, "npm": true, "npx": true, "pip": true, "pip3": true, "flutter": true, "gh": true, "docker": true, "systemctl": true, "cmake": true, "wrangler": true, "gradle": true, "gradlew": true, "ctest": true, "make": true, "ninja": true}
	probeCommands   = map[string]bool{"grep": true, "rg": true, "ls": true, "find": true, "test": true, "[": true, "git grep": true, "diff": true, "cmp": true, "wc": true, "cat": true, "head": true, "tail": true, "sed": true, "awk": true, "command": true, "which": true, "stat": true}
	toolUseErrorTag = regexp.MustCompile(`</?tool_use_error>`)
)

func trimLeadingOpeners(s string, keepOne bool) string {
	for (strings.HasPrefix(s, "(") || strings.HasPrefix(s, "{")) && (!keepOne || len(s) > 1) {
		s = s[1:]
	}
	return s
}

var launcherRe = regexp.MustCompile(`(?s)^[\t\n\v\f\r ]*(?:[^\t\n\v\f\r ]*/)?(?:bash|sh|zsh)[\t\n\v\f\r ]+-l?c[\t\n\v\f\r ]+(.*?)[\t\n\v\f\r ]*$`)

func unwrapShellLauncher(command string) string {
	match := launcherRe.FindStringSubmatch(command)
	if match == nil {
		return command
	}
	rest := match[1]
	if len(rest) >= 2 && strings.HasPrefix(rest, "'") && strings.HasSuffix(rest, "'") {
		inner := rest[1 : len(rest)-1]
		bare := strings.ReplaceAll(strings.ReplaceAll(inner, `'\''`, ""), `'"'"'`, "")
		if strings.Contains(bare, "'") {
			return command
		}
		return strings.ReplaceAll(strings.ReplaceAll(inner, `'\''`, "'"), `'"'"'`, "'")
	}
	if len(rest) >= 2 && strings.HasPrefix(rest, `"`) && strings.HasSuffix(rest, `"`) {
		inner := rest[1 : len(rest)-1]
		bare := strings.ReplaceAll(strings.ReplaceAll(inner, `\\`, ""), `\"`, "")
		if strings.Contains(bare, `"`) {
			return command
		}
		return strings.ReplaceAll(inner, `\"`, `"`)
	}
	if strings.HasPrefix(rest, "'") || strings.HasPrefix(rest, `"`) {
		return command
	}
	return rest
}

func CommandShape(command string) string {
	text := stripHeredocs(unwrapShellLauncher(command))
	segments := topLevelSegments(text)
	chosen := ""
	for i := len(segments) - 1; i >= 0 && chosen == ""; i-- {
		segment := trimmed(segments[i])
		if segment == "" {
			continue
		}
		firstWord := trimLeadingOpeners(firstSection(segment), true)
		if !shellKeywords[firstWord] {
			chosen = segment
		}
	}
	tokens := splitWhitespace(chosen)
	for len(tokens) > 0 {
		first := trimLeadingOpeners(tokens[0], false)
		if first == "" || separatorRe.MatchString(first) {
			tokens = tokens[1:]
			continue
		}
		tokens[0] = first
		if setupCommands[first] {
			end := 1
			for end < len(tokens) && !separatorRe.MatchString(tokens[end]) && !strings.HasSuffix(tokens[end], ";") {
				end++
			}
			if end+1 >= len(tokens) {
				break
			}
			tokens = tokens[end+1:]
			continue
		}
		if first == "timeout" {
			tokens = tokens[1:]
			for len(tokens) > 0 && strings.HasPrefix(tokens[0], "-") {
				flag := tokens[0]
				tokens = tokens[1:]
				if (flag == "-s" || flag == "-k" || flag == "--signal" || flag == "--kill-after") && len(tokens) > 0 {
					tokens = tokens[1:]
				}
			}
			if len(tokens) > 0 {
				tokens = tokens[1:]
			}
			continue
		}
		if first == "env" || first == "sudo" {
			tokens = tokens[1:]
			for len(tokens) > 0 && strings.HasPrefix(tokens[0], "-") {
				flag := tokens[0]
				tokens = tokens[1:]
				if (flag == "-u" || flag == "-C" || flag == "-g") && len(tokens) > 0 {
					tokens = tokens[1:]
				}
			}
			continue
		}
		if first == "nice" {
			tokens = tokens[1:]
			if len(tokens) >= 2 && tokens[0] == "-n" {
				tokens = tokens[2:]
			} else if len(tokens) > 0 && strings.HasPrefix(tokens[0], "-n") {
				tokens = tokens[1:]
			}
			continue
		}
		if first == "time" || first == "exec" || first == "command" ||
			first == "builtin" || assignmentRe.MatchString(first) {
			tokens = tokens[1:]
			continue
		}
		break
	}
	if len(tokens) == 0 {
		return ""
	}
	verb := tokens[0]
	if strings.Contains(verb, "/") && !strings.HasPrefix(verb, "-") {
		verb = verb[strings.LastIndex(verb, "/")+1:]
	}
	next := ""
	if len(tokens) > 1 {
		next = tokens[1]
	}
	isInlineFlag := func(flag string) bool {
		return flag == "-" || flag == "-c" || flag == "-e" || strings.HasPrefix(flag, "-<<") ||
			strings.HasPrefix(flag, "<<") || flag == "--input-type=module" || flag == "--eval" || flag == "-p"
	}
	if interpreterRe.MatchString(verb) {
		if strings.HasPrefix(verb, "python") {
			verb = "python3"
		}
		if next == "" || isInlineFlag(next) {
			return verb + " inline script"
		}
		if next == "-m" {
			module := ""
			if len(tokens) > 2 {
				module = tokens[2]
			}
			if module == "pytest" {
				return "pytest"
			}
			if module == "" {
				return verb
			}
			return verb + " -m " + module
		}
		if strings.HasPrefix(next, "-") {
			return verb
		}
		return verb + " " + next[strings.LastIndex(next, "/")+1:]
	}
	if verb == "for" || verb == "while" || verb == "until" {
		return verb + " loop"
	}
	if multiVerbTools[verb] && next != "" && !strings.HasPrefix(next, "-") && !strings.Contains(next, "/") &&
		!separatorRe.MatchString(next) {
		return verb + " " + next
	}
	return verb
}

var testRunnerRe = regexp.MustCompile(`(?:^|[\t\n\v\f\r ;&|(])(?:pytest|python[0-9.]*[\t\n\v\f\r ]+-m[\t\n\v\f\r ]+(?:pytest|unittest)|` +
	`go[\t\n\v\f\r ]+(?:test|build|vet)|cargo[\t\n\v\f\r ]+(?:test|build|check)|ctest|` +
	`cmake[\t\n\v\f\r ]+--build|ninja|swift[\t\n\v\f\r ]+(?:test|build)|npm[\t\n\v\f\r ]+test|npm[\t\n\v\f\r ]+run[\t\n\v\f\r ]+test|` +
	`npx[\t\n\v\f\r ]+playwright[\t\n\v\f\r ]+test|npx[\t\n\v\f\r ]+jest|jest|vitest|node[\t\n\v\f\r ]+--test|` +
	`flutter[\t\n\v\f\r ]+test|gradlew?[\t\n\v\f\r ]+test|make[\t\n\v\f\r ]+(?:test|check))(?:[\t\n\v\f\r ]|$)`)

func testRunnerCommand(command string) bool {
	return testRunnerRe.MatchString(command)
}

func pipelinePieces(segment string) []string {
	pieces := []string{}
	var current strings.Builder
	depth := 0
	var quote rune
	runes := []rune(segment)
	for i := 0; i < len(runes); i++ {
		c := runes[i]
		if quote != 0 {
			current.WriteRune(c)
			if c == '\\' && quote == '"' && i+1 < len(runes) {
				i++
				current.WriteRune(runes[i])
			} else if c == quote {
				quote = 0
			}
			continue
		}
		if c == '\\' && i+1 < len(runes) {
			current.WriteRune(c)
			i++
			current.WriteRune(runes[i])
			continue
		}
		if c == '\'' || c == '"' {
			quote = c
			current.WriteRune(c)
			continue
		}
		if c == '(' || c == '{' {
			depth++
		} else if (c == ')' || c == '}') && depth > 0 {
			depth--
		}
		var next rune
		if i+1 < len(runes) {
			next = runes[i+1]
		}
		andOr := (c == '&' && next == '&') || (c == '|' && next == '|')
		pipe := c == '|' && !andOr && !(i > 0 && runes[i-1] == '>')
		if depth == 0 && (andOr || pipe) {
			pieces = append(pieces, current.String())
			current.Reset()
			if andOr || next == '&' {
				i++
			}
			continue
		}
		current.WriteRune(c)
	}
	return append(pieces, current.String())
}

func chainShapes(command string) []string {
	shapes := []string{}
	for _, segment := range topLevelSegments(stripHeredocs(unwrapShellLauncher(command))) {
		for _, piece := range pipelinePieces(segment) {
			if shape := CommandShape(piece); shape != "" {
				shapes = append(shapes, shape)
			}
		}
	}
	return shapes
}

var (
	chainReaders = map[string]bool{"sed": true, "rg": true, "grep": true, "cat": true, "head": true, "tail": true,
		"ls": true, "wc": true, "cut": true, "find": true, "nl": true, "sort": true, "uniq": true, "awk": true,
		"printf": true, "echo": true, "test": true, "[": true, "diff": true, "cmp": true, "stat": true,
		"which": true, "command": true, "file": true, "jq": true, "tr": true, "column": true, "pwd": true,
		"cd": true, "true": true, "realpath": true, "readlink": true, "basename": true, "dirname": true,
		"du": true, "df": true, "git grep": true, "git status": true, "git log": true, "git show": true,
		"git diff": true, "git branch": true, "git rev-parse": true, "git ls-files": true, "git worktree": true}
	chainMissable = map[string]bool{"grep": true, "rg": true, "git grep": true, "ls": true, "find": true,
		"test": true, "[": true, "diff": true, "cmp": true, "stat": true, "command": true, "which": true}
)

func readOnlyProbeChainMiss(command string) string {
	miss := ""
	for _, shape := range chainShapes(command) {
		if !chainReaders[shape] {
			return ""
		}
		if chainMissable[shape] {
			miss = shape
		}
	}
	return miss
}

var waitLoopRe = regexp.MustCompile(`\b(?:until|while)\b[^\n]*\bsleep\b|\btail[\t\n\v\f\r ]+(?:-[a-zA-Z]*F|--follow|` +
	`--pid)|\binotifywait\b|\bsleep[\t\n\v\f\r ]+\d+[\t\n\v\f\r ]*(?:;|&&|\n)`)

func waitLoopCommand(command string) bool {
	return waitLoopRe.MatchString(command)
}

var failedCheckRe = regexp.MustCompile(`^(?:FAIL!?[\t\n\v\f\r ]*:|FAIL\b|FAILED\b|--- FAIL\b|not ok \d|` +
	`[^\t\n\v\f\r :]+:\d+:\d+: (?:fatal )?error:|[^\t\n\v\f\r :]+:\d+: (?:trailing whitespace|` +
	`space before tab|indent with spaces|new blank line at EOF|leftover conflict marker))`)

func reportsFailedChecks(lines []string) bool {
	for _, line := range lines {
		if failedCheckRe.MatchString(line) {
			return true
		}
	}
	return false
}

var quickShapes = map[string]bool{
	"tail": true, "head": true, "cat": true, "grep": true, "rg": true, "ls": true, "wc": true,
	"cut": true, "sed": true, "awk": true, "echo": true, "printf": true, "date": true, "uptime": true,
	"sort": true, "uniq": true, "jq": true, "stat": true, "test": true, "[": true, "diff": true,
	"which": true, "df": true, "du": true, "systemctl": true, "git status": true,
	"git rev-parse": true, "true": true,
}

func slowCommandShape(command string) string {
	if waitLoopCommand(command) {
		return "foreground wait"
	}
	for _, segment := range topLevelSegments(stripHeredocs(unwrapShellLauncher(command))) {
		if shape := CommandShape(segment); shape != "" && !quickShapes[shape] {
			return shape
		}
	}
	return CommandShape(command)
}

type failureKind int

const (
	failureOther failureKind = iota
	failureBlocked
	failureRefused
	failureInterrupted
	failureSelfKill
	failureTimedOut
	failureWaitLoop
	failureMissingPath
	failureEmptyRange
	failureProbeMiss
	failureInlineScript
	failureTestRun
	failureUnguardedDelete
	failureMergeConflict
	failureTailCount
	failureMissingCommand
)

type shellFailure struct {
	kind     failureKind
	exitCode int
	message  string
	verb     string
}

func (f shellFailure) isResult() bool {
	return f.kind == failureTestRun || f.kind == failureMergeConflict || f.kind == failureInterrupted
}

var (
	exitLineRe    = regexp.MustCompile(`^Exit code:? (\d+)$`)
	errorMarkerRe = regexp.MustCompile(`(?i)(?:^|\b)(?:error|fatal|failed|blocked|traceback|no such file|` +
		`not found|cannot|can't|permission denied|usage:|unbound variable|` +
		`syntax error|timed out|killed|interrupted|unexpected|unknown|invalid|` +
		`statically analyzed)(?:\b|:)`)
	blockedRe = regexp.MustCompile(`^Blocked:[\t\n\v\f\r ]*(.+?)(?:[\t\n\v\f\r ]+followed by\b|$)`)
)

func qtToInt(s string) int {
	value, err := strconv.ParseInt(s, 10, 32)
	if err != nil {
		return 0
	}
	return int(value)
}

func classifyShellFailure(command, resultText string) shellFailure {
	failure := shellFailure{exitCode: -1, verb: CommandShape(command)}
	lines := []string{}
	for _, part := range strings.Split(resultText, "\n") {
		line := toolUseErrorTag.ReplaceAllLiteralString(trimmed(part), "")
		if trimmed(line) != "" {
			lines = append(lines, simplified(line))
		}
	}
	if len(lines) > 0 {
		if match := exitLineRe.FindStringSubmatch(lines[0]); match != nil {
			failure.exitCode = qtToInt(match[1])
			lines = lines[1:]
		} else if last := exitLineRe.FindStringSubmatch(lines[len(lines)-1]); last != nil {
			failure.exitCode = qtToInt(last[1])
			lines = lines[:len(lines)-1]
		}
	}
	traceback := false
	for _, line := range lines {
		if strings.HasPrefix(line, "Traceback (most recent call last)") {
			traceback = true
			break
		}
	}
	errorLine := false
	if len(lines) > 0 {
		if traceback {
			failure.message = lines[len(lines)-1]
		} else {
			failure.message = lines[0]
		}
		errorLine = traceback
		if !traceback {
			for _, line := range lines {
				if errorMarkerRe.MatchString(line) {
					failure.message = line
					errorLine = true
					break
				}
			}
		}
	}
	lowered := strings.ToLower(strings.Join(lines, "\n"))
	killer := failure.verb == "pkill" || failure.verb == "kill" || failure.verb == "killall" ||
		strings.Contains(command, "pkill ")
	switch {
	case strings.HasPrefix(failure.message, "Blocked:"):
		failure.kind = failureBlocked
	case strings.Contains(lowered, "[request interrupted by user"):
		failure.kind = failureInterrupted
	case strings.Contains(lowered, "dangerous rm operation") ||
		strings.Contains(lowered, "before the removal"):
		failure.kind = failureUnguardedDelete
	case strings.Contains(lowered, "cannot be statically analyzed") ||
		strings.Contains(lowered, "requested permissions to use") ||
		strings.Contains(lowered, "permission to use this tool") ||
		strings.Contains(lowered, "requires approval") ||
		strings.Contains(lowered, "denied by"):
		failure.kind = failureRefused
	case (testRunnerCommand(command) || strings.HasSuffix(failure.verb, "-tests")) && failure.exitCode == 1:
		failure.kind = failureTestRun
	case strings.Contains(lowered, "automatic merge failed") ||
		strings.Contains(lowered, "conflict (content)") ||
		strings.Contains(lowered, "resolve all conflicts manually"):
		failure.kind = failureMergeConflict
	case !traceback && failure.exitCode >= 1 && failure.exitCode < 124 && reportsFailedChecks(lines):
		failure.kind = failureTestRun
	case strings.Contains(lowered, "option used in invalid context"):
		failure.kind = failureTailCount
	case failure.exitCode == 144 || (killer && failure.exitCode >= 128):
		failure.kind = failureSelfKill
	case waitLoopCommand(command) && (failure.exitCode >= 124 || failure.message == "" || !errorLine):
		failure.kind = failureWaitLoop
	case failure.exitCode == 124 || failure.exitCode == 137 || failure.exitCode == 143 ||
		strings.Contains(lowered, "command timed out"):
		failure.kind = failureTimedOut
	case strings.Contains(lowered, "unknown command: `,'") ||
		strings.Contains(lowered, "invalid usage of line address 0") ||
		strings.Contains(lowered, "unexpected `,'"):
		failure.kind = failureEmptyRange
	case strings.Contains(lowered, "no such file or directory") ||
		strings.Contains(lowered, "can't read") ||
		strings.Contains(lowered, "cannot access") ||
		strings.Contains(lowered, "cannot open") ||
		strings.Contains(lowered, "not a directory"):
		failure.kind = failureMissingPath
	case failure.exitCode == 127 || strings.Contains(lowered, "command not found"):
		failure.kind = failureMissingCommand
	case failure.exitCode == 1 && readOnlyProbeChainMiss(command) != "":
		failure.kind = failureProbeMiss
		failure.verb = readOnlyProbeChainMiss(command)
	case strings.HasSuffix(failure.verb, "inline script") && (traceback || failure.exitCode == 1):
		failure.kind = failureInlineScript
	case !errorLine && failure.exitCode >= 1 && failure.exitCode <= 2:
		if probeCommands[failure.verb] {
			failure.kind = failureProbeMiss
		}
	}
	if failure.kind == failureBlocked {
		if match := blockedRe.FindStringSubmatch(failure.message); match != nil {
			if shape := CommandShape(match[1]); shape != "" {
				failure.verb = shape
			}
		}
	} else if failure.kind == failureSelfKill && strings.Contains(command, "pkill") {
		failure.verb = "pkill"
	}
	return failure
}

func shellFailureTopic(failure shellFailure) string {
	topic := ""
	switch failure.kind {
	case failureBlocked:
		topic = "blocked by the harness"
	case failureRefused:
		topic = "refused by the harness"
	case failureInterrupted:
		topic = "interrupted by the user"
	case failureSelfKill:
		topic = "killed its own shell"
	case failureTimedOut:
		topic = "timed out"
	case failureWaitLoop:
		topic = "foreground wait"
	case failureMissingPath:
		topic = "missing path"
	case failureEmptyRange:
		topic = "empty line number"
	case failureProbeMiss:
		topic = "probe found nothing"
	case failureInlineScript:
		topic = "inline script failed"
	case failureTestRun:
		topic = "tests failed"
	case failureUnguardedDelete:
		topic = "unguarded delete"
	case failureMergeConflict:
		topic = "merge conflict"
	case failureTailCount:
		topic = "tail -N on several files"
	case failureMissingCommand:
		topic = "command not found"
	case failureOther:
		if failure.exitCode >= 0 {
			topic = "exit " + strconv.Itoa(failure.exitCode)
		}
	}
	if failure.verb == "" {
		return topic
	}
	if topic == "" {
		return failure.verb
	}
	return failure.verb + " · " + topic
}

const genericFailureSuggestion = "Recurring failures of one kind point at a tool description, path convention, or permission worth fixing in the runtime prompt or host."

func shellFailureSuggestion(failure shellFailure) string {
	switch failure.kind {
	case failureBlocked:
		return "The harness refuses a foreground sleep or wait loop; run the work with run_in_background and wait with Monitor."
	case failureRefused:
		return "The harness would not run this unreviewed: it could not parse the quoting or a permission rule denies it. Simplify the quoting, split the pipeline, or move the script into a file."
	case failureInterrupted:
		return "The user interrupted the call; nothing to change in the tooling."
	case failureSelfKill:
		return "pkill -f matched this call's own shell, whose command line contains the pattern; bracket its first character ([f]oo) or kill by pid."
	case failureTimedOut:
		return "The command outlived its bound and was killed; run it with run_in_background, tee its output to a file, and wait with Monitor."
	case failureWaitLoop:
		return "A foreground poll loop, sleep chain, or tail -F holds the turn until its bound kills it; run the work with run_in_background and wait with Monitor instead."
	case failureMissingPath:
		return "The working directory persists between calls, so an earlier cd moves later relative paths: use absolute paths, git -C, or (cd dir && …) in a subshell, and Glob (or rg --files) a path before assuming it rather than guessing."
	case failureEmptyRange:
		return "sed -n was handed an empty line number from a grep -n that found nothing; check the number is non-empty before using it, or Grep then Read a range."
	case failureProbeMiss:
		return "A probe that finds nothing exits non-zero and cancels its parallel siblings; end the chain with || true so a miss is a result, not an error."
	case failureInlineScript:
		return "A failing inline script costs its whole body again; write anything longer than a few lines to a file under the build dir, run it by path, and fix it with Edit."
	case failureTestRun:
		return "A failing test run is the tool working; nothing to change in the tooling."
	case failureUnguardedDelete:
		return "The harness refuses rm on a $VAR path or a relative glob after cd, since an empty variable widens it; spell the absolute path out, or delete with fm_safe_rm_tree <expected parent> <path> from tools/forkmesh-safe-rm.sh."
	case failureMergeConflict:
		return "A merge that stops on conflicts is the tool working; nothing to change in the tooling."
	case failureTailCount:
		return "GNU tail rejects the obsolete -N count when given more than one file; write tail -n N."
	case failureMissingCommand:
		return "The command is not installed on this host; probe with command -v name || true before relying on it, and report it missing instead of retrying."
	}
	return genericFailureSuggestion
}

func readOnlyFamily(family string) bool {
	switch family {
	case "read", "grep", "glob", "ast_grep", "lsp", "web_search", "github":
		return true
	}
	return false
}

func mutatingFamily(family string) bool {
	switch family {
	case "write", "edit", "ast_edit", "bash", "eval", "rewind":
		return true
	}
	return false
}

func longRunningFamily(family string) bool {
	switch family {
	case "task", "hub", "ask", "yield", "goal":
		return true
	}
	return false
}

func oversizedSuggestion(family string) string {
	switch family {
	case "bash":
		return "Bound command output (head/tail, grep -m, --quiet) and line width (cut -c, rg --max-columns) for logs, JSONL or minified files, or write it to a file and read the relevant range."
	case "grep", "ast_grep":
		return "Narrow the pattern or path, or list matching files first and read only the hits that matter."
	case "glob":
		return "Use a narrower glob; a path listing this large crowds out working context."
	case "read":
		return "Read a line range (offset/limit) around the code that matters."
	}
	return "Bound this tool's output; results this large crowd out working context."
}

func oversizedShellTopic(command string) string {
	if command == "" {
		return ""
	}
	if testRunnerCommand(command) {
		return "test run"
	}
	for _, shape := range chainShapes(command) {
		switch shape {
		case "rg", "grep", "git grep", "find":
			return "search"
		}
	}
	return ""
}

func oversizedShellSuggestion(command string) string {
	switch oversizedShellTopic(command) {
	case "test run":
		return "Run tests quietly (pytest -q --tb=short, go test without -v) and pipe the run through tail -n 80; rerun one failing test verbosely only when the summary is not enough."
	case "search":
		return "rg -m and grep -m cap matches per file, not in total: list files first with rg -l, then pipe the search through head -n 60 and add --max-columns 200; leave data, logs and build trees out."
	}
	return oversizedSuggestion("bash")
}
