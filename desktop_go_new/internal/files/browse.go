package files

import (
	"bytes"
	"encoding/base64"
	"io"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"unicode"
	"unicode/utf16"
	"unicode/utf8"
)

const (
	familyPrefix      = "files.browse."
	maxPathChars      = 1024
	maxComponentChars = 255
	maxReadBytes      = 256 * 1024
)

func utf16Len(s string) int {
	n := 0
	for _, r := range s {
		n += utf16.RuneLen(r)
	}
	return n
}

func utf16Units(s string) []uint16 {
	return utf16.Encode([]rune(s))
}

func utf16String(units []uint16) string {
	return string(utf16.Decode(units))
}

func leftUTF16(s string, n int) string {
	units := utf16Units(s)
	if len(units) <= n {
		return s
	}
	return utf16String(units[:n])
}

func rightUTF16(s string, n int) string {
	units := utf16Units(s)
	if len(units) <= n {
		return s
	}
	return utf16String(units[len(units)-n:])
}

func normalizeRelativePath(path string) (string, bool) {
	if utf16Len(path) > maxPathChars {
		return "", false
	}
	if strings.HasPrefix(path, "/") || strings.Contains(path, "\\") {
		return "", false
	}
	parts := []string{}
	for _, part := range strings.Split(path, "/") {
		if part == "" || part == "." {
			continue
		}
		if part == ".." || utf16Len(part) > maxComponentChars {
			return "", false
		}
		for _, r := range part {
			if r < 0x20 || r == 0x7f {
				return "", false
			}
		}
		parts = append(parts, part)
	}
	return strings.Join(parts, "/"), true
}

var sensitiveSuffixes = []string{".pem", ".key", ".p12", ".pfx", ".keystore", ".jks"}

func isSensitiveName(name string) bool {
	lower := strings.ToLower(name)
	if lower == ".env" || strings.HasPrefix(lower, ".env.") || lower == ".netrc" ||
		lower == ".npmrc" || lower == ".pypirc" || lower == ".git-credentials" ||
		lower == "credentials.json" || strings.HasPrefix(lower, "id_rsa") ||
		strings.HasPrefix(lower, "id_ed25519") || strings.HasPrefix(lower, "id_ecdsa") {
		return true
	}
	for _, suffix := range sensitiveSuffixes {
		if strings.HasSuffix(lower, suffix) {
			return true
		}
	}
	return false
}

func canonicalPath(path string) string {
	if path == "" {
		return ""
	}
	abs, err := filepath.Abs(path)
	if err != nil {
		return ""
	}
	resolved, err := filepath.EvalSymlinks(abs)
	if err != nil {
		return ""
	}
	return filepath.ToSlash(resolved)
}

func insideRoot(rootCanonical, candidate string) bool {
	if rootCanonical == "" || candidate == "" {
		return false
	}
	if candidate == rootCanonical {
		return true
	}
	prefix := rootCanonical
	if !strings.HasSuffix(prefix, "/") {
		prefix += "/"
	}
	return strings.HasPrefix(candidate, prefix)
}

func hasGitComponent(relative string) bool {
	for _, part := range strings.Split(relative, "/") {
		if part != "" && strings.EqualFold(part, ".git") {
			return true
		}
	}
	return false
}

func joinPath(dir, name string) string {
	if name == "" {
		return dir
	}
	if strings.HasSuffix(dir, "/") {
		return dir + name
	}
	return dir + "/" + name
}

func resolveInside(rootPath, relative string) (string, string) {
	rootCanonical := canonicalPath(rootPath)
	if rootCanonical == "" {
		return "", "root_unavailable"
	}
	clean, ok := normalizeRelativePath(relative)
	if !ok {
		return "", "bad_path"
	}
	if hasGitComponent(clean) {
		return "", "not_allowed"
	}
	target := joinPath(rootCanonical, clean)
	canonical := canonicalPath(filepath.FromSlash(target))
	if canonical == "" {
		return "", "not_found"
	}
	if !insideRoot(rootCanonical, canonical) {
		return "", "outside_root"
	}
	if hasGitComponent(canonical[len(rootCanonical):]) {
		return "", "not_allowed"
	}
	return canonical, ""
}

type listing struct {
	entries   []map[string]any
	truncated bool
	total     int
}

func foldUnit(u uint16) uint16 {
	if u >= 0xd800 && u <= 0xdfff {
		return u
	}
	r := unicode.ToLower(rune(u))
	if r > 0xffff {
		return u
	}
	return uint16(r)
}

func compareFold(a, b string) int {
	ua, ub := utf16Units(a), utf16Units(b)
	for i := 0; i < len(ua) && i < len(ub); i++ {
		fa, fb := foldUnit(ua[i]), foldUnit(ub[i])
		if fa != fb {
			if fa < fb {
				return -1
			}
			return 1
		}
	}
	switch {
	case len(ua) < len(ub):
		return -1
	case len(ua) > len(ub):
		return 1
	}
	return 0
}

func listDirectory(rootPath, dirPath string, maxEntries int) listing {
	out := listing{entries: []map[string]any{}}
	rootCanonical := canonicalPath(rootPath)
	dirEntries, _ := os.ReadDir(filepath.FromSlash(dirPath))
	rows := []map[string]any{}
	for _, entry := range dirEntries {
		name := entry.Name()
		if strings.EqualFold(name, ".git") {
			continue
		}
		out.total++
		full := filepath.Join(filepath.FromSlash(dirPath), name)
		link, lerr := os.Lstat(full)
		isLink := lerr == nil && link.Mode()&os.ModeSymlink != 0
		info, err := os.Stat(full)
		isDir := err == nil && info.IsDir()
		var size, modified int64
		if err == nil {
			size = info.Size()
			modified = info.ModTime().UnixMilli()
		}
		kind := "file"
		if isDir {
			kind = "dir"
		}
		escapes := false
		if isLink {
			kind = "symlink"
			target := canonicalPath(full)
			escapes = target == "" || !insideRoot(rootCanonical, target)
			if !escapes {
				if targetInfo, err := os.Stat(filepath.FromSlash(target)); err == nil && targetInfo.IsDir() {
					kind = "dirlink"
				}
			}
		}
		if isDir {
			size = 0
		}
		row := map[string]any{"name": name, "kind": kind, "size": size, "modifiedMs": modified}
		if escapes {
			row["escapes"] = true
		}
		if !isDir && isSensitiveName(name) {
			row["sensitive"] = true
		}
		rows = append(rows, row)
	}
	rank := func(row map[string]any) int {
		kind := row["kind"].(string)
		if kind == "dir" || kind == "dirlink" {
			return 0
		}
		return 1
	}
	sort.SliceStable(rows, func(i, j int) bool {
		ri, rj := rank(rows[i]), rank(rows[j])
		if ri != rj {
			return ri < rj
		}
		ni, nj := rows[i]["name"].(string), rows[j]["name"].(string)
		if c := compareFold(ni, nj); c != 0 {
			return c < 0
		}
		return ni < nj
	})
	out.truncated = len(rows) > maxEntries
	if len(rows) > maxEntries {
		rows = rows[:maxEntries]
	}
	out.entries = append(out.entries, rows...)
	return out
}

func looksBinary(data []byte) bool {
	if bytes.IndexByte(data, 0) >= 0 {
		return true
	}
	if utf8.Valid(data) {
		return false
	}
	invalid := 0
	for _, r := range string(data) {
		if r == utf8.RuneError {
			invalid++
		}
	}
	return invalid > 4
}

func clamp(value, low, high int64) int64 {
	if value < low {
		return low
	}
	if value > high {
		return high
	}
	return value
}

func readFileSlice(path string, offset, maxBytes int64) (map[string]any, string) {
	file, err := os.Open(filepath.FromSlash(path))
	if err != nil {
		return nil, "unreadable"
	}
	defer file.Close()
	info, err := file.Stat()
	if err != nil {
		return nil, "unreadable"
	}
	size := info.Size()
	start := clamp(offset, 0, size)
	length := clamp(maxBytes, 0, maxReadBytes)
	if _, err := file.Seek(start, io.SeekStart); err != nil {
		return nil, "unreadable"
	}
	buffer := make([]byte, length)
	n, err := io.ReadFull(file, buffer)
	if err != nil && err != io.EOF && err != io.ErrUnexpectedEOF {
		return nil, "unreadable"
	}
	data := buffer[:n]
	eof := start+int64(len(data)) >= size
	result := map[string]any{"size": size, "offset": start, "length": int64(len(data)), "eof": eof}
	if looksBinary(data) {
		result["binary"] = true
		result["base64"] = base64.StdEncoding.EncodeToString(data)
		return result, ""
	}
	if !eof {
		cut := len(data)
		for cut > 0 && cut > len(data)-4 && data[cut-1]&0xC0 == 0x80 {
			cut--
		}
		if cut > 0 && data[cut-1]&0xC0 == 0xC0 {
			cut--
		}
		if cut > 0 && cut < len(data) {
			data = data[:cut]
			result["length"] = int64(len(data))
		}
	}
	result["binary"] = false
	result["text"] = string(data)
	return result, ""
}

var rootIDPattern = regexp.MustCompile(`^[A-Za-z0-9][A-Za-z0-9._-]{0,63}/[A-Za-z0-9._][A-Za-z0-9._-]{0,99}(#[A-Za-z0-9._][A-Za-z0-9._-]{0,127})?$`)

func validRootID(id string) bool {
	return utf16Len(id) <= 300 && rootIDPattern.MatchString(id) && !strings.Contains(id, "..")
}

func worktreeRootID(repoKey, folder string) string {
	return repoKey + "#" + folder
}

type root struct {
	id     string
	label  string
	kind   string
	path   string
	branch string
	repo   string
}

func (r root) row() map[string]any {
	return map[string]any{"id": r.id, "label": r.label, "kind": r.kind, "branch": r.branch, "repo": r.repo}
}

func readSmallFile(path string, limit int) (string, bool) {
	file, err := os.Open(path)
	if err != nil {
		return "", false
	}
	defer file.Close()
	if info, err := file.Stat(); err != nil || info.IsDir() {
		return "", false
	}
	buffer := make([]byte, limit)
	n, _ := io.ReadFull(file, buffer)
	return string(buffer[:n]), true
}

func trimmed(s string) string {
	return strings.TrimSpace(s)
}

var worktreeFolderPattern = regexp.MustCompile(`^[A-Za-z0-9._][A-Za-z0-9._-]{0,127}$`)

func isDir(path string) bool {
	if path == "" {
		return false
	}
	info, err := os.Stat(path)
	return err == nil && info.IsDir()
}

func discoverWorktrees(repoKey, repoPath string, maxWorktrees int) []root {
	roots := []root{}
	gitDir := filepath.Join(repoPath, ".git")
	if !isDir(gitDir) {
		return roots
	}
	worktrees := filepath.Join(gitDir, "worktrees")
	entries, _ := os.ReadDir(worktrees)
	folders := []string{}
	for _, entry := range entries {
		name := entry.Name()
		if strings.HasPrefix(name, ".") {
			continue
		}
		if isDir(filepath.Join(worktrees, name)) {
			folders = append(folders, name)
		}
	}
	sort.Slice(folders, func(i, j int) bool { return folders[i] < folders[j] })
	for _, folder := range folders {
		if len(roots) >= maxWorktrees {
			break
		}
		if !worktreeFolderPattern.MatchString(folder) {
			continue
		}
		meta := filepath.Join(worktrees, folder)
		text, _ := readSmallFile(filepath.Join(meta, "gitdir"), 4096)
		pointer := trimmed(text)
		if pointer == "" {
			continue
		}
		absolute, err := filepath.Abs(pointer)
		if err != nil {
			continue
		}
		checkout := filepath.ToSlash(filepath.Dir(absolute))
		if !isDir(checkout) {
			continue
		}
		head, _ := readSmallFile(filepath.Join(meta, "HEAD"), 4096)
		branch := trimmed(head)
		if strings.HasPrefix(branch, "ref: refs/heads/") {
			branch = branch[16:]
		} else if utf16Len(branch) >= 7 {
			branch = leftUTF16(branch, 7)
		}
		label := branch
		if label == "" {
			label = folder
		}
		roots = append(roots, root{id: worktreeRootID(repoKey, folder), label: label, kind: "worktree",
			path: checkout, branch: branch, repo: repoKey})
	}
	return roots
}

func headBranch(repoPath string) string {
	text, ok := readSmallFile(filepath.Join(repoPath, ".git", "HEAD"), 512)
	if !ok {
		return ""
	}
	text = trimmed(text)
	if strings.HasPrefix(text, "ref: refs/heads/") {
		return text[16:]
	}
	return leftUTF16(text, 7)
}

var (
	aheadPattern  = regexp.MustCompile(`ahead (\d+)`)
	behindPattern = regexp.MustCompile(`behind (\d+)`)
)

func qtToInt(s string) int {
	n, err := strconv.ParseInt(s, 10, 32)
	if err != nil {
		return 0
	}
	return int(n)
}

func parsePorcelainStatus(output []byte, maxFiles int) map[string]any {
	fields := bytes.Split(output, []byte{0})
	branch, upstream := "", ""
	ahead, behind := 0, 0
	files := []map[string]any{}
	total, staged, unstaged, untracked, conflicted := 0, 0, 0, 0, 0
	for i := 0; i < len(fields); i++ {
		entry := string([]rune(string(fields[i])))
		if entry == "" {
			continue
		}
		if strings.HasPrefix(entry, "## ") {
			head := entry[3:]
			counts := ""
			if bracket := strings.Index(head, " ["); bracket >= 0 {
				counts = head[bracket:]
				head = head[:bracket]
			}
			if dots := strings.Index(head, "..."); dots >= 0 {
				branch = head[:dots]
				upstream = head[dots+3:]
			} else {
				branch = head
				upstream = ""
			}
			if strings.HasPrefix(branch, "No commits yet on ") {
				branch = branch[18:]
			}
			ahead, behind = 0, 0
			if m := aheadPattern.FindStringSubmatch(counts); m != nil {
				ahead = qtToInt(m[1])
			}
			if m := behindPattern.FindStringSubmatch(counts); m != nil {
				behind = qtToInt(m[1])
			}
			continue
		}
		units := utf16Units(entry)
		if len(units) < 4 {
			continue
		}
		x := units[0]
		y := units[1]
		path := utf16String(units[3:])
		from := ""
		if x == 'R' || x == 'C' {
			if i+1 < len(fields) {
				i++
				from = string([]rune(string(fields[i])))
			}
		}
		total++
		kind := ""
		xs, ys := utf16String([]uint16{x}), utf16String([]uint16{y})
		xy := xs + ys
		switch {
		case xy == "??":
			kind = "untracked"
			untracked++
		case x == 'U' || y == 'U' || xy == "AA" || xy == "DD":
			kind = "conflicted"
			conflicted++
		default:
			if x != ' ' && x != '?' {
				staged++
			}
			if y != ' ' {
				unstaged++
			}
			code := x
			if x == ' ' {
				code = y
			}
			switch code {
			case 'A':
				kind = "added"
			case 'D':
				kind = "deleted"
			case 'R':
				kind = "renamed"
			case 'C':
				kind = "copied"
			default:
				kind = "modified"
			}
		}
		if len(files) >= maxFiles {
			continue
		}
		row := map[string]any{"path": leftUTF16(path, 1024), "index": xs, "worktree": ys, "kind": kind}
		if from != "" {
			row["from"] = leftUTF16(from, 1024)
		}
		files = append(files, row)
	}
	return map[string]any{"branch": branch, "upstream": upstream, "ahead": ahead, "behind": behind,
		"total": total, "staged": staged, "unstaged": unstaged, "untracked": untracked,
		"conflicted": conflicted, "truncated": total > len(files), "files": files}
}

func gitActions() []string {
	return []string{"fetch", "pull"}
}

func validGitAction(action string) bool {
	for _, known := range gitActions() {
		if known == action {
			return true
		}
	}
	return false
}

func gitArguments(action string) []string {
	switch action {
	case "fetch":
		return []string{"fetch", "--quiet"}
	case "pull":
		return []string{"pull", "--ff-only", "--quiet"}
	}
	return nil
}

func gitActionSummary(action, label string) string {
	switch action {
	case "fetch":
		return "Fetch \"" + label + "\" from its remote. Nothing in the working tree changes."
	case "pull":
		return "Pull \"" + label + "\" from its upstream, fast-forward only. Nothing is merged or rewound; a diverged branch is left as it is."
	}
	return ""
}
