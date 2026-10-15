package quality

import (
	"bytes"
	"context"
	"io"
	"os"
	"regexp"
	"strings"

	"forkmesh.com/desktop-core/internal/actions"
)

const (
	maxScannedFileBytes = 1024 * 1024
	maxTodoFindings     = 120
	longFileLines       = 1200
	veryLongFileLines   = 3000
)

var todoMarker = regexp.MustCompile(`\b(TODO|FIXME|HACK|XXX)\b`)

var codeExtensions = map[string]bool{
	"c": true, "cc": true, "cpp": true, "cxx": true, "h": true, "hh": true,
	"hpp": true, "hxx": true, "m": true, "mm": true, "cs": true, "java": true,
	"kt": true, "go": true, "rs": true, "py": true, "rb": true, "js": true,
	"jsx": true, "ts": true, "tsx": true, "mjs": true, "cjs": true, "php": true,
	"swift": true, "scala": true, "sh": true, "bash": true, "pl": true, "lua": true,
	"sql": true, "dart": true, "ex": true, "exs": true, "hs": true, "vue": true,
	"svelte": true,
}

type Input struct {
	Owner      string
	Name       string
	LocalPath  string
	MirrorPath string
	Issues     []string
	Runs       []actions.Run
}

type Result struct {
	Findings []Finding
	Signals  []int
}

type repoFile struct {
	path    string
	content []byte
}

func fileSuffix(p string) string {
	name := fileName(p)
	if i := strings.LastIndexByte(name, '.'); i >= 0 {
		return name[i+1:]
	}
	return ""
}

func looksLikeTestPath(p string) bool {
	lower := strings.ToLower(p)
	if strings.HasPrefix(lower, "test/") || strings.HasPrefix(lower, "tests/") ||
		strings.Contains(lower, "/test/") || strings.Contains(lower, "/tests/") ||
		strings.Contains(lower, "/__tests__/") || strings.Contains(lower, "/spec/") {
		return true
	}
	base := fileName(lower)
	return strings.HasPrefix(base, "test_") || strings.Contains(base, "_test.") ||
		strings.Contains(base, ".test.") || strings.Contains(base, ".spec.") ||
		strings.Contains(base, "_spec.")
}

func isSourceCodePath(p string) bool {
	return codeExtensions[strings.ToLower(fileSuffix(p))]
}

func splitSkipEmpty(text string) []string {
	var out []string
	for _, line := range strings.Split(text, "\n") {
		if line != "" {
			out = append(out, line)
		}
	}
	return out
}

func readLocalFile(localPath, rel string) []byte {
	file, err := os.Open(joinDir(localPath, rel))
	if err != nil {
		return nil
	}
	defer file.Close()
	info, err := file.Stat()
	if err != nil || info.IsDir() || info.Size() > maxScannedFileBytes {
		return nil
	}
	data, err := io.ReadAll(file)
	if err != nil {
		return nil
	}
	return data
}

func (s scanner) readMirrorFiles(ctx context.Context, mirror string, paths []string) map[string][]byte {
	contents := map[string][]byte{}
	if len(paths) == 0 {
		return contents
	}
	var names []byte
	for _, p := range paths {
		names = append(names, "HEAD:"+p+"\n"...)
	}
	checks, ok := s.git.run(ctx, mirror, batchGitTimeout, names, "cat-file", "--batch-check=%(objectname) %(objecttype) %(objectsize)")
	if !ok {
		return contents
	}
	answers := bytes.Split(checks, []byte("\n"))
	pathsByOid := map[string][]string{}
	var oids []byte
	for i := 0; i < len(paths) && i < len(answers); i++ {
		fields := bytes.Split(answers[i], []byte(" "))
		if len(fields) != 3 || string(fields[1]) != "blob" {
			continue
		}
		size, ok := parseLongLong(string(fields[2]))
		if !ok || size > maxScannedFileBytes {
			continue
		}
		oid := string(fields[0])
		if _, seen := pathsByOid[oid]; !seen {
			oids = append(oids, oid+"\n"...)
		}
		pathsByOid[oid] = append(pathsByOid[oid], paths[i])
	}
	if len(oids) == 0 {
		return contents
	}
	batch, ok := s.git.run(ctx, mirror, batchGitTimeout, oids, "cat-file", "--batch")
	if !ok {
		return contents
	}
	for pos := 0; pos < len(batch); {
		nl := bytes.IndexByte(batch[pos:], '\n')
		if nl < 0 {
			break
		}
		header := bytes.Split(batch[pos:pos+nl], []byte(" "))
		pos += nl + 1
		if len(header) != 3 {
			continue
		}
		size, ok := parseLongLong(string(header[2]))
		if !ok || size < 0 || size > int64(len(batch)-pos) {
			break
		}
		body := batch[pos : pos+int(size)]
		for _, p := range pathsByOid[string(header[0])] {
			contents[p] = body
		}
		pos += int(size) + 1
	}
	return contents
}

func parseLongLong(s string) (int64, bool) {
	s = qtTrim(s)
	negative := false
	if strings.HasPrefix(s, "+") {
		s = s[1:]
	} else if strings.HasPrefix(s, "-") {
		negative = true
		s = s[1:]
	}
	if s == "" || len(s) > 18 {
		return 0, false
	}
	value := int64(0)
	for i := 0; i < len(s); i++ {
		if s[i] < '0' || s[i] > '9' {
			return 0, false
		}
		value = value*10 + int64(s[i]-'0')
	}
	if negative {
		value = -value
	}
	return value, true
}

type scanner struct {
	git gitRunner
}

func (s scanner) trackedTextFiles(ctx context.Context, in Input) []repoFile {
	useLocal := qtTrim(in.LocalPath) != "" && isDir(in.LocalPath)
	useMirror := !useLocal && qtTrim(in.MirrorPath) != "" && isDir(in.MirrorPath)
	var paths []string
	if useLocal {
		paths = splitSkipEmpty(s.git.text(ctx, in.LocalPath, "ls-files"))
	} else {
		paths = splitSkipEmpty(s.git.text(ctx, in.MirrorPath, "ls-tree", "-r", "--name-only", "HEAD"))
	}
	var mirrorContents map[string][]byte
	if useMirror {
		mirrorContents = s.readMirrorFiles(ctx, in.MirrorPath, paths)
	}
	files := make([]repoFile, 0, len(paths))
	for _, p := range paths {
		var content []byte
		if useLocal {
			content = readLocalFile(in.LocalPath, p)
		} else {
			content = mirrorContents[p]
		}
		if len(content) > maxScannedFileBytes || bytes.IndexByte(content, 0) >= 0 {
			continue
		}
		files = append(files, repoFile{path: p, content: content})
	}
	return files
}

func checksSeverity(runs []actions.Run) int {
	hasRun, hasRunning := false, false
	for _, run := range runs {
		hasRun = true
		switch run.Status {
		case actions.StatusFailed, actions.StatusRejected:
			return SeverityHigh
		case actions.StatusRunning, actions.StatusQueued, actions.StatusAwaitingApproval:
			hasRunning = true
		}
	}
	if !hasRun {
		return SeverityWarning
	}
	if hasRunning {
		return SeverityInfo
	}
	return SeverityPass
}

func lineCount(content []byte) int {
	n := bytes.Count(content, []byte("\n")) + 1
	if len(content) == 0 || content[len(content)-1] == '\n' {
		n--
	}
	return n
}

func (s scanner) Scan(ctx context.Context, in Input) Result {
	var result Result
	result.Signals = append(result.Signals, checksSeverity(in.Runs))
	for _, run := range in.Runs {
		if run.Status != actions.StatusFailed && run.Status != actions.StatusRejected {
			continue
		}
		title := run.WorkflowName
		if title == "" {
			title = run.WorkflowPath
		}
		result.Findings = append(result.Findings, Finding{Path: run.WorkflowPath, Severity: SeverityHigh, Title: title})
	}

	files := s.trackedTextFiles(ctx, in)
	sourceFiles, testFiles, todoCount, longFiles := 0, 0, 0, 0
	hasReadme, hasLicense, hasContributing, hasChangelog := false, false, false, false
	var todoFindings, longFindings []Finding
	longWarning := false
	for _, file := range files {
		lowerPath := strings.ToLower(file.path)
		base := fileName(lowerPath)
		if !strings.Contains(file.path, "/") {
			if strings.HasPrefix(base, "readme") {
				hasReadme = true
			}
			if strings.HasPrefix(base, "license") || strings.HasPrefix(base, "copying") {
				hasLicense = true
			}
			if strings.HasPrefix(base, "contributing") {
				hasContributing = true
			}
			if strings.HasPrefix(base, "changelog") || base == "news" || strings.HasPrefix(base, "news.") {
				hasChangelog = true
			}
		}
		isSource := isSourceCodePath(file.path)
		if isSource {
			sourceFiles++
			if looksLikeTestPath(file.path) {
				testFiles++
			}
		}
		fileLines := lineCount(file.content)
		text := file.content
		for i := 0; i < fileLines; i++ {
			end := bytes.IndexByte(text, '\n')
			line := text
			if end >= 0 {
				line = text[:end]
				text = text[end+1:]
			}
			match := todoMarker.FindSubmatch(line)
			if match == nil {
				continue
			}
			todoCount++
			if len(todoFindings) < maxTodoFindings {
				todoFindings = append(todoFindings, Finding{Path: file.path, Severity: SeverityInfo, Title: string(match[1])})
			}
		}
		if isSource && fileLines > longFileLines {
			longFiles++
			severity := SeverityInfo
			if fileLines > veryLongFileLines {
				severity = SeverityWarning
				longWarning = true
			}
			longFindings = append(longFindings, Finding{Path: file.path, Severity: severity, Title: fileName(file.path)})
		}
	}

	result.Signals = append(result.Signals, SeverityInfo)

	coreDocs := 0
	for _, present := range []bool{hasReadme, hasLicense, hasContributing, hasChangelog} {
		if present {
			coreDocs++
		}
	}
	docsSeverity := SeverityPass
	if !hasReadme {
		docsSeverity = SeverityWarning
	} else if coreDocs < 4 {
		docsSeverity = SeverityInfo
	}
	result.Signals = append(result.Signals, docsSeverity)
	missing := []struct {
		present bool
		name    string
	}{{hasReadme, "README"}, {hasLicense, "LICENSE"}, {hasContributing, "CONTRIBUTING"}, {hasChangelog, "CHANGELOG"}}
	for _, doc := range missing {
		if doc.present {
			continue
		}
		severity := SeverityInfo
		if doc.name == "README" {
			severity = SeverityWarning
		}
		result.Findings = append(result.Findings, Finding{Severity: severity, Title: "Missing " + doc.name})
	}

	testsSeverity := SeverityPass
	if testFiles == 0 {
		if sourceFiles > 0 {
			testsSeverity = SeverityWarning
		} else {
			testsSeverity = SeverityInfo
		}
	}
	result.Signals = append(result.Signals, testsSeverity)

	todoSeverity := SeverityPass
	if todoCount > 50 {
		todoSeverity = SeverityWarning
	} else if todoCount > 0 {
		todoSeverity = SeverityInfo
	}
	result.Signals = append(result.Signals, todoSeverity)
	result.Findings = append(result.Findings, todoFindings...)

	sizeSeverity := SeverityPass
	if longWarning {
		sizeSeverity = SeverityWarning
	} else if longFiles > 0 {
		sizeSeverity = SeverityInfo
	}
	result.Signals = append(result.Signals, sizeSeverity)
	result.Findings = append(result.Findings, longFindings...)

	gitDir := in.MirrorPath
	if qtTrim(in.LocalPath) != "" && isDir(in.LocalPath) {
		gitDir = in.LocalPath
	}
	commits30, _ := qtToInt(s.git.text(ctx, gitDir, "rev-list", "--count", "--since=30 days ago", "HEAD"))
	if commits30 > 0 {
		result.Signals = append(result.Signals, SeverityPass)
	} else {
		result.Signals = append(result.Signals, SeverityInfo)
	}

	open := 0
	for _, status := range in.Issues {
		if status == "open" {
			open++
		}
	}
	if open == 0 {
		result.Signals = append(result.Signals, SeverityPass)
	} else {
		result.Signals = append(result.Signals, SeverityInfo)
	}
	return result
}

func ReportFor(result Result, root string, computedAtMs int64) map[string]any {
	findings := make([]Finding, len(result.Findings))
	for i, finding := range result.Findings {
		findings[i] = Finding{Path: RelativeFindingPath(finding.Path, root), Severity: finding.Severity, Title: finding.Title}
	}
	return Report(findings, result.Signals, computedAtMs)
}

func scanRoot(in Input) string {
	if in.LocalPath == "" {
		return in.MirrorPath
	}
	return in.LocalPath
}
