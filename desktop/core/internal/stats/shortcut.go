package stats

import (
	"context"
	"io"
	"os"
	"path/filepath"
	"regexp"
	"strings"

	"forkmesh.com/desktop-core/internal/rpc"
)

const maxShortcutBytes = 64 * 1024

var (
	shortcutFilePattern = regexp.MustCompile(`^[A-Za-z0-9_][A-Za-z0-9._ -]{0,127}$`)
	ownerSegmentPattern = regexp.MustCompile(`[^A-Za-z0-9._-]+`)
	shortcutScopes      = []string{"shortcut", "repository", "app", "organization"}
)

func validShortcutFile(name string) bool {
	return shortcutFilePattern.MatchString(name) && !strings.Contains(name, "..")
}

func (m *Module) dataRoot() string {
	if m.core == nil {
		return ""
	}
	return m.core.Layout.Data
}

func (m *Module) shortcutDir(repoKey, scope string) string {
	if scope == "app" {
		return filepath.Join(m.dataRoot(), "skills")
	}
	for _, repo := range loadRepositories(m.settings()) {
		if !strings.EqualFold(repo.Owner+"/"+repo.Name, repoKey) {
			continue
		}
		if repo.LocalPath == "" {
			return ""
		}
		switch scope {
		case "shortcut":
			return repo.LocalPath + "/.forkmesh/shortcuts"
		case "repository":
			return filepath.Join(repo.LocalPath, ".forkmesh", "skills")
		case "organization":
			owner := ownerSegmentPattern.ReplaceAllString(strings.TrimSpace(repo.Owner), "-")
			if owner == "" || owner == "." || owner == ".." {
				owner = "personal"
			}
			return filepath.Join(m.dataRoot(), "organization-skills", owner)
		}
		return ""
	}
	return ""
}

func readShortcutText(path string) string {
	file, err := os.Open(path)
	if err != nil {
		return ""
	}
	defer file.Close()
	out := make([]byte, 0, 4096)
	chunk := make([]byte, 32*1024)
	for len(out) < maxShortcutBytes {
		n, err := file.Read(chunk)
		for _, b := range chunk[:n] {
			if b == '\r' {
				continue
			}
			out = append(out, b)
			if len(out) >= maxShortcutBytes {
				break
			}
		}
		if err != nil {
			if err != io.EOF {
				return string(out)
			}
			break
		}
	}
	return string(out)
}

func (m *Module) handleShortcut(ctx context.Context, req *rpc.Request) (any, error) {
	params := req.Object()
	repo, _ := params["repo"].(string)
	repo = strings.TrimSpace(repo)
	scope, _ := params["scope"].(string)
	scope = strings.TrimSpace(scope)
	file, _ := params["file"].(string)
	dir := ""
	if contains(shortcutScopes, scope) && validShortcutFile(file) {
		dir = m.shortcutDir(repo, scope)
	}
	if dir == "" {
		return nil, rpc.Fail("unknown_shortcut")
	}
	path := filepath.Join(dir, file)
	info, err := os.Stat(path)
	if err != nil || !info.Mode().IsRegular() {
		return nil, rpc.Fail("unknown_shortcut")
	}
	return map[string]any{
		"file":      file,
		"scope":     scope,
		"text":      readShortcutText(path),
		"truncated": info.Size() > maxShortcutBytes,
	}, nil
}
