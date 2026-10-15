package stats

import (
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"time"

	"forkmesh.com/desktop-core/internal/qsettings"
	"forkmesh.com/desktop-core/internal/qvariant"
	"forkmesh.com/desktop-core/internal/rpc"
)

const (
	maxShortcutEntries       = 200
	maxShortcutRepos         = 300
	shortcutUsageSetting     = "shortcuts/usageCounts"
	shortcutRunStatsSetting  = "shortcuts/runStats"
	shortcutStateCallTimeout = 3 * time.Second
)

var skillSuffixes = []string{".skill", ".md", ".prompt", ".txt"}

type shortcutMeta struct {
	Name        string
	Description string
	Agent       string
	Model       string
}

func parseShortcutMeta(fileName, text string) shortcutMeta {
	meta := shortcutMeta{Name: fileName}
	lines := strings.Split(text, "\n")
	for i := 0; i < 20 && i < len(lines); i++ {
		line := strings.TrimSpace(lines[i])
		switch {
		case strings.HasPrefix(line, "#!"):
		case strings.HasPrefix(line, "# name:"):
			meta.Name = strings.TrimSpace(line[7:])
		case strings.HasPrefix(line, "# description:"):
			meta.Description = strings.TrimSpace(line[14:])
		case strings.HasPrefix(line, "# agent:"):
			meta.Agent = strings.ToLower(strings.TrimSpace(line[8:]))
		case strings.HasPrefix(line, "# model:"):
			meta.Model = strings.TrimSpace(line[8:])
		case meta.Description == "" && strings.HasPrefix(line, "#"):
			meta.Description = strings.TrimSpace(line[1:])
		}
	}
	return meta
}

func shortcutPrompt(text string) string {
	kept := []string{}
	for _, line := range strings.Split(text, "\n") {
		trimmed := strings.TrimSpace(line)
		if strings.HasPrefix(trimmed, "# name:") || strings.HasPrefix(trimmed, "# description:") ||
			strings.HasPrefix(trimmed, "# agent:") || strings.HasPrefix(trimmed, "# model:") {
			continue
		}
		kept = append(kept, line)
	}
	return strings.TrimSpace(strings.Join(kept, "\n"))
}

func fileSuffix(name string) string {
	base := filepath.Base(name)
	if dot := strings.LastIndexByte(base, '.'); dot >= 0 {
		return base[dot+1:]
	}
	return ""
}

func completeBaseName(name string) string {
	base := filepath.Base(name)
	if dot := strings.LastIndexByte(base, '.'); dot >= 0 {
		return base[:dot]
	}
	return base
}

func shortcutKind(fileName string, executable bool) string {
	switch suffix := strings.ToLower(fileSuffix(fileName)); {
	case suffix == "sh" || suffix == "bash" || (suffix == "" && executable):
		return "script"
	case suffix == "skill":
		return "skill"
	case suffix == "md" || suffix == "txt" || suffix == "prompt":
		return "prompt"
	}
	return "file"
}

type shortcutFile struct {
	name       string
	path       string
	executable bool
}

func listShortcutFiles(dir string, skill bool) []shortcutFile {
	entries, err := os.ReadDir(dir)
	if err != nil {
		return nil
	}
	names := []string{}
	for _, entry := range entries {
		name := entry.Name()
		if strings.HasPrefix(name, ".") {
			continue
		}
		if skill {
			lower := strings.ToLower(name)
			matched := false
			for _, suffix := range skillSuffixes {
				if strings.HasSuffix(lower, suffix) {
					matched = true
					break
				}
			}
			if !matched {
				continue
			}
		}
		names = append(names, name)
	}
	sort.Strings(names)
	out := []shortcutFile{}
	for _, name := range names {
		path := filepath.Join(dir, name)
		info, err := os.Stat(path)
		if err != nil || !info.Mode().IsRegular() || !readableFile(path) {
			continue
		}
		out = append(out, shortcutFile{name: name, path: path, executable: executableFile(path, info)})
	}
	return out
}

func (m *Module) shortcutDirIndex(records []repoRecord, repoKey, scope string) (string, int) {
	if scope == "app" {
		return filepath.Join(m.dataRoot(), "skills"), -1
	}
	for i, repo := range records {
		if !strings.EqualFold(repo.Owner+"/"+repo.Name, repoKey) {
			continue
		}
		if repo.LocalPath == "" {
			return "", -1
		}
		switch scope {
		case "shortcut":
			return repo.LocalPath + "/.forkmesh/shortcuts", i
		case "repository":
			return filepath.Join(repo.LocalPath, ".forkmesh", "skills"), i
		case "organization":
			owner := ownerSegmentPattern.ReplaceAllString(strings.TrimSpace(repo.Owner), "-")
			if owner == "" || owner == "." || owner == ".." {
				owner = "personal"
			}
			return filepath.Join(m.dataRoot(), "organization-skills", owner), i
		}
		return "", i
	}
	return "", -1
}

func variantMap(s *qsettings.Settings, key string) map[string]any {
	value, ok := s.Value(key)
	if !ok {
		return map[string]any{}
	}
	decoded, err := qvariant.FromSettingsRaw(value.Raw)
	if err != nil {
		return map[string]any{}
	}
	if out, ok := decoded.(map[string]any); ok {
		return out
	}
	return map[string]any{}
}

type shortcutRunStats struct {
	LastRunAtMs int64
	Finished    int64
	LastOk      bool
	LastOutput  string
}

func runStatsFor(stored map[string]any, key string) shortcutRunStats {
	row, _ := stored[key].(map[string]any)
	if row == nil {
		return shortcutRunStats{}
	}
	return shortcutRunStats{
		LastRunAtMs: qvariant.Int64(row["lastRunAtMs"]),
		Finished:    int64(int32(qvariant.Int64(row["finished"]))),
		LastOk:      qvariant.Bool(row["lastOk"]),
		LastOutput:  qvariant.String(row["lastOutput"]),
	}
}

type DesktopShortcutState struct {
	Active     bool   `json:"active"`
	Running    bool   `json:"running"`
	Repo       string `json:"repo"`
	File       string `json:"file"`
	LastOutput string `json:"lastOutput"`
}

func (m *Module) desktopShortcutState(ctx context.Context) DesktopShortcutState {
	if m.ShortcutState != nil {
		return m.ShortcutState(ctx)
	}
	if m.core == nil {
		return DesktopShortcutState{}
	}
	callCtx, cancel := context.WithTimeout(ctx, shortcutStateCallTimeout)
	defer cancel()
	raw, err := m.core.CallUI(callCtx, "ui.insights.shortcutState", map[string]any{})
	if err != nil {
		return DesktopShortcutState{}
	}
	var state DesktopShortcutState
	_ = json.Unmarshal(raw, &state)
	return state
}

func (m *Module) handleShortcuts(ctx context.Context, req *rpc.Request) (any, error) {
	params := req.Object()
	repo, _ := params["repo"].(string)
	repo = strings.TrimSpace(repo)
	s := m.settings()
	records := loadRepositories(s)
	repos := []any{}
	for _, record := range records {
		if record.LocalPath != "" && len(repos) < maxShortcutRepos {
			repos = append(repos, record.Owner+"/"+record.Name)
		}
	}
	usage := variantMap(s, shortcutUsageSetting)
	stored := variantMap(s, shortcutRunStatsSetting)
	var state *DesktopShortcutState
	entries := []any{}
	for _, scope := range shortcutScopes {
		dir, repoIndex := m.shortcutDirIndex(records, repo, scope)
		if dir == "" {
			continue
		}
		skill := scope != "shortcut"
		for _, file := range listShortcutFiles(dir, skill) {
			if len(entries) >= maxShortcutEntries {
				break
			}
			if !validShortcutFile(file.name) {
				continue
			}
			text := readShortcutText(file.path)
			meta := parseShortcutMeta(file.name, text)
			kind := "skill"
			if !skill {
				kind = shortcutKind(file.name, file.executable)
			}
			repoKey := repo
			if repoIndex >= 0 && repoIndex < len(records) {
				repoKey = records[repoIndex].Owner + "/" + records[repoIndex].Name
			}
			usageKey := scope + ":" + repoKey + ":" + file.name
			if scope == "app" || scope == "organization" {
				usageKey = scope + ":" + file.name
			}
			stats := runStatsFor(stored, usageKey)
			running := false
			lastOutput := stats.LastOutput
			if !skill {
				if state == nil {
					fetched := m.desktopShortcutState(ctx)
					state = &fetched
				}
				if state.Active && state.Running && state.File == file.name && strings.EqualFold(state.Repo, repoKey) {
					running = true
					lastOutput = state.LastOutput
				}
			}
			name := meta.Name
			if skill && meta.Name == file.name {
				name = completeBaseName(file.name)
			}
			runnable := false
			if kind == "script" {
				runnable = scope == "shortcut"
			} else {
				runnable = kind != "file" && shortcutPrompt(text) != ""
			}
			entries = append(entries, map[string]any{
				"file":        file.name,
				"scope":       scope,
				"name":        name,
				"description": left(meta.Description, 300),
				"kind":        kind,
				"agent":       meta.Agent,
				"model":       meta.Model,
				"uses":        int32(qvariant.Int64(usage[usageKey])),
				"runnable":    runnable,
				"running":     running,
				"lastOutput":  lastOutput,
				"lastOk":      stats.LastOk,
				"finished":    stats.Finished,
				"lastRunAtMs": float64(stats.LastRunAtMs),
			})
		}
	}
	return map[string]any{"repo": repo, "repos": repos, "entries": entries}, nil
}
