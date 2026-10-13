package quality

import (
	"bytes"
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
)

const (
	issuesRootRel = ".forkmesh/issues"
	openDirName   = "open"
	closedDirName = "closed"
)

func issueJSONFileName(number int) string {
	return "issue-" + strconv.Itoa(number) + ".json"
}

func qtToInt(s string) (int, bool) {
	s = qtTrim(s)
	negative := false
	if strings.HasPrefix(s, "+") {
		s = s[1:]
	} else if strings.HasPrefix(s, "-") {
		negative = true
		s = s[1:]
	}
	if s == "" {
		return 0, false
	}
	value := int64(0)
	for i := 0; i < len(s); i++ {
		if s[i] < '0' || s[i] > '9' {
			return 0, false
		}
		value = value*10 + int64(s[i]-'0')
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

func joinDir(dir, rel string) string {
	if dir == "" {
		return rel
	}
	return filepath.Join(dir, filepath.FromSlash(rel))
}

func isDir(p string) bool {
	info, err := os.Stat(p)
	return err == nil && info.IsDir()
}

func exists(p string) bool {
	_, err := os.Stat(p)
	return err == nil
}

func numericDirEntries(dir string) []int {
	entries, err := os.ReadDir(dir)
	if err != nil {
		return nil
	}
	var numbers []int
	for _, entry := range entries {
		name := entry.Name()
		if strings.HasPrefix(name, ".") || !isDir(filepath.Join(dir, name)) {
			continue
		}
		if n, ok := qtToInt(name); ok {
			numbers = append(numbers, n)
		}
	}
	return numbers
}

func issueStatus(data []byte) (string, bool) {
	var object map[string]any
	if err := json.Unmarshal(data, &object); err != nil || object == nil {
		return "", false
	}
	status := "open"
	if text, ok := object["status"].(string); ok {
		status = text
	}
	events, _ := object["events"].([]any)
	for _, raw := range events {
		event, _ := raw.(map[string]any)
		if kind, _ := event["type"].(string); kind == "status" {
			status, _ = event["status"].(string)
		}
	}
	return status, true
}

type issueReader struct {
	git      gitRunner
	workTree string
	mirror   string
	identity bool
}

func (r issueReader) canWrite() bool {
	return r.workTree != "" && r.identity && exists(r.workTree+"/.git")
}

func (r issueReader) statuses(ctx context.Context) []string {
	if !r.canWrite() {
		return r.mirrorStatuses(ctx)
	}
	issuesDir := joinDir(r.workTree, issuesRootRel)
	roots := []struct {
		dir    string
		status string
	}{
		{issuesDir + "/" + openDirName, "open"},
		{issuesDir + "/" + closedDirName, "closed"},
		{issuesDir, ""},
	}
	seen := map[int]bool{}
	var out []string
	for _, root := range roots {
		for _, number := range numericDirEntries(root.dir) {
			if seen[number] {
				continue
			}
			seen[number] = true
			status, ok := r.readIssueFile(number)
			if !ok {
				status = "open"
			} else if root.status != "" {
				status = root.status
			}
			out = append(out, status)
		}
	}
	return out
}

func (r issueReader) readIssueFile(number int) (string, bool) {
	name := strconv.Itoa(number)
	candidates := []string{
		issuesRootRel + "/" + openDirName + "/" + name,
		issuesRootRel + "/" + closedDirName + "/" + name,
		issuesRootRel + "/" + name,
	}
	dir := joinDir(r.workTree, candidates[0])
	for _, rel := range candidates {
		if abs := joinDir(r.workTree, rel); isDir(abs) {
			dir = abs
			break
		}
	}
	data, err := os.ReadFile(filepath.Join(dir, issueJSONFileName(number)))
	if err != nil {
		return "", false
	}
	return issueStatus(data)
}

func (r issueReader) gitOut(ctx context.Context, stdin []byte, args ...string) ([]byte, bool) {
	return r.git.run(ctx, r.mirror, issueGitTimeout, stdin, args...)
}

func (r issueReader) mirrorRef(ctx context.Context) string {
	if out, ok := r.gitOut(ctx, nil, "rev-parse", "--verify", "-q", "HEAD"); ok && len(bytes.TrimSpace(out)) > 0 {
		return "HEAD"
	}
	if out, ok := r.gitOut(ctx, nil, "for-each-ref", "--format=%(refname)", "--count=1", "refs/heads/"); ok {
		return qtTrim(string(out))
	}
	return ""
}

func (r issueReader) fetchOids(ctx context.Context, wanted map[string]bool) map[string][]byte {
	content := map[string][]byte{}
	if len(wanted) == 0 {
		return content
	}
	var input []byte
	for oid := range wanted {
		input = append(input, oid...)
		input = append(input, '\n')
	}
	batch, ok := r.gitOut(ctx, input, "cat-file", "--batch")
	if !ok {
		return content
	}
	for pos := 0; pos < len(batch); {
		nl := bytes.IndexByte(batch[pos:], '\n')
		if nl < 0 {
			break
		}
		header := bytes.Split(batch[pos:pos+nl], []byte(" "))
		pos += nl + 1
		if len(header) < 3 {
			continue
		}
		size, ok := qtToInt(string(header[2]))
		if !ok || size < 0 || size > len(batch)-pos {
			break
		}
		content[string(header[0])] = batch[pos : pos+size]
		pos += size + 1
	}
	return content
}

func (r issueReader) mirrorStatuses(ctx context.Context) []string {
	if r.mirror == "" {
		return nil
	}
	ref := r.mirrorRef(ctx)
	if ref == "" {
		return nil
	}
	listing, ok := r.gitOut(ctx, nil, "ls-tree", "-r", ref, issuesRootRel+"/")
	if !ok {
		return nil
	}
	oids := map[int]string{}
	var order []int
	folder := map[int]string{}
	wanted := map[string]bool{}
	prefix := issuesRootRel + "/"
	for _, line := range strings.Split(string(bytes.ToValidUTF8(listing, []byte("�"))), "\n") {
		if line == "" {
			continue
		}
		tab := strings.IndexByte(line, '\t')
		if tab < 0 {
			continue
		}
		var meta []string
		for _, field := range strings.Split(line[:tab], " ") {
			if field != "" {
				meta = append(meta, field)
			}
		}
		if len(meta) < 3 || meta[1] != "blob" {
			continue
		}
		p := line[tab+1:]
		if !strings.HasPrefix(p, prefix) {
			continue
		}
		rel := p[len(prefix):]
		relFolder := ""
		if strings.HasPrefix(rel, openDirName+"/") {
			rel = rel[len(openDirName)+1:]
			relFolder = "open"
		} else if strings.HasPrefix(rel, closedDirName+"/") {
			rel = rel[len(closedDirName)+1:]
			relFolder = "closed"
		}
		slash := strings.IndexByte(rel, '/')
		if slash < 0 {
			continue
		}
		number, numeric := qtToInt(rel[:slash])
		if !numeric {
			continue
		}
		if rel[slash+1:] != issueJSONFileName(number) {
			continue
		}
		if _, ok := oids[number]; !ok {
			order = append(order, number)
		}
		oids[number] = meta[2]
		wanted[meta[2]] = true
		if relFolder != "" {
			folder[number] = relFolder
		}
	}
	content := r.fetchOids(ctx, wanted)
	missing := map[string]bool{}
	for _, oid := range oids {
		if _, ok := content[oid]; !ok {
			missing[oid] = true
		}
	}
	if len(missing) > 0 {
		for oid, data := range r.fetchOids(ctx, missing) {
			content[oid] = data
		}
	}
	sort.Ints(order)
	var out []string
	for _, number := range order {
		data, ok := content[oids[number]]
		if !ok {
			out = append(out, "open")
			continue
		}
		status, ok := issueStatus(data)
		if !ok {
			out = append(out, "open")
			continue
		}
		if f := folder[number]; f != "" {
			status = f
		}
		out = append(out, status)
	}
	return out
}
