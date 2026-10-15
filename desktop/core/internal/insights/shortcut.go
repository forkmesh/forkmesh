package insights

import (
	"context"
	"io"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"

	"forkmesh.com/desktop-core/internal/phonefamily"
	"forkmesh.com/desktop-core/internal/qsettings"
	"forkmesh.com/desktop-core/internal/rpc"
)

const maxShortcutBytes = 64 * 1024

type repoRecord struct {
	Owner     string
	Name      string
	LocalPath string
}

var ownerSegmentPattern = regexp.MustCompile(`[^A-Za-z0-9._-]+`)

func loadRepoRecords(s *qsettings.Settings) []repoRecord {
	count := int(s.Int(repositoriesArray+"/size", 0))
	out := []repoRecord{}
	for i := 1; i <= count; i++ {
		prefix := repositoriesArray + "/" + strconv.Itoa(i) + "/"
		get := func(key string) string { return s.String(prefix+key, "") }
		record := repoRecord{Owner: get("owner"), Name: get("name"), LocalPath: get("localPath")}
		source := trimmed(record.LocalPath)
		if source == "" {
			source = trimmed(get("cloneUrl"))
		}
		if record.Name == "" || (source == "" && !opaqueIDPattern.MatchString(trimmed(get("publicArchiveId"))) &&
			!opaqueIDPattern.MatchString(trimmed(get("privateReplicaId")))) {
			continue
		}
		out = append(out, record)
	}
	return out
}

func (m *Module) dataRoot() string {
	if m.core == nil {
		return ""
	}
	return m.core.Layout.Data
}

func (m *Module) shortcutDir(records []repoRecord, repoKey, scope string) (string, int) {
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
			owner := ownerSegmentPattern.ReplaceAllString(trimmed(repo.Owner), "-")
			if owner == "" || owner == "." || owner == ".." {
				owner = "personal"
			}
			return filepath.Join(m.dataRoot(), "organization-skills", owner), i
		}
		return "", i
	}
	return "", -1
}

func readShortcutText(path string) string {
	file, err := os.Open(path)
	if err != nil {
		return ""
	}
	defer file.Close()
	data, _ := io.ReadAll(io.LimitReader(file, maxShortcutBytes))
	return strings.ReplaceAll(strings.ToValidUTF8(string(data), "�"), "\r\n", "\n")
}

func shortcutName(fileName, text string) string {
	name := fileName
	lines := strings.Split(text, "\n")
	for i := 0; i < 20 && i < len(lines); i++ {
		line := trimmed(lines[i])
		if strings.HasPrefix(line, "# name:") {
			name = trimmed(line[7:])
		}
	}
	return name
}

func fileSuffix(name string) string {
	base := filepath.Base(name)
	if dot := strings.LastIndexByte(base, '.'); dot >= 0 {
		return base[dot+1:]
	}
	return ""
}

func ShortcutKind(fileName string, executable bool) string {
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

var (
	shortcutFilePattern = regexp.MustCompile(`^[A-Za-z0-9_][A-Za-z0-9._ -]{0,127}$`)
	familyIDPattern     = regexp.MustCompile(`^[a-z0-9][a-z0-9_-]{0,63}$`)
	repoKeyPattern      = regexp.MustCompile(`^[A-Za-z0-9][A-Za-z0-9._-]{0,63}/[A-Za-z0-9._][A-Za-z0-9._-]{0,99}$`)
	shortcutScopes      = map[string]bool{"shortcut": true, "repository": true, "app": true, "organization": true}
)

func validShortcutFile(name string) bool {
	return shortcutFilePattern.MatchString(name) && !strings.Contains(name, "..")
}

func subjectFor(action string, params map[string]any) (string, string) {
	switch action {
	case "toggleFamily":
		family, _ := params["family"].(string)
		family = trimmed(family)
		enabled, isBool := params["enabled"].(bool)
		if !familyIDPattern.MatchString(family) || !isBool {
			return "", "bad_family"
		}
		if enabled {
			return family + "|on", ""
		}
		return family + "|off", ""
	case "runShortcut":
		repo, _ := params["repo"].(string)
		scope, _ := params["scope"].(string)
		file, _ := params["file"].(string)
		repo, scope = trimmed(repo), trimmed(scope)
		if !shortcutScopes[scope] || !validShortcutFile(file) {
			return "", "bad_shortcut"
		}
		if !repoKeyPattern.MatchString(repo) || strings.Contains(repo, "..") {
			return "", "bad_repo"
		}
		return repo + "|" + scope + "|" + file, ""
	}
	return "", "unsupported_action"
}

func section(subject string, index int) string {
	parts := strings.Split(subject, "|")
	if index < len(parts) {
		return parts[index]
	}
	return ""
}

func sectionFrom(subject string, index int) string {
	parts := strings.Split(subject, "|")
	if index < len(parts) {
		return strings.Join(parts[index:], "|")
	}
	return ""
}

func actionSummary(action, subject, label string, script bool) string {
	switch {
	case action == "toggleFamily":
		name := label
		if name == "" {
			name = section(subject, 0)
		}
		if strings.HasSuffix(subject, "|on") {
			return "Switch the " + name + " tool family back on for agents. Running agents get it from their next launch or resume."
		}
		return "Switch the " + name + " tool family off for agents. Running agents keep what they launched with until their next launch or resume."
	case action == "runShortcut":
		name := label
		if name == "" {
			name = section(subject, 2)
		}
		if script {
			return "Run the \"" + name + "\" script on the desktop in " + section(subject, 0) + ". Its output streams here while it runs."
		}
		return "Start an agent on the desktop with the \"" + name + "\" shortcut in " + section(subject, 0) + "."
	}
	return ""
}

func isScriptFile(path string) bool {
	info, err := os.Stat(path)
	if err != nil || !info.Mode().IsRegular() {
		return false
	}
	return ShortcutKind(filepath.Base(path), executableFile(path, info)) == "script"
}

func (m *Module) settings() *qsettings.Settings {
	if m.core == nil || m.core.Settings == nil {
		return qsettings.Parse(nil)
	}
	return m.core.Settings.Get()
}

func (m *Module) handlePrepare(ctx context.Context, req *rpc.Request) (any, error) {
	params := req.Object()
	action, _ := params["action"].(string)
	subject, failure := subjectFor(action, params)
	if subject == "" {
		return nil, rpc.Fail(failure)
	}
	label := ""
	script := false
	if action == "toggleFamily" {
		family := FamilyByID(section(subject, 0))
		if family == nil {
			return nil, rpc.Fail("bad_family")
		}
		label = family.DisplayName
	} else {
		label, _ = params["file"].(string)
		scope := section(subject, 1)
		dir, _ := m.shortcutDir(loadRepoRecords(m.settings()), section(subject, 0), scope)
		if dir != "" && scope == "shortcut" {
			script = isScriptFile(filepath.Join(dir, label))
		}
	}
	token := m.confirmations.Issue(phonefamily.DeviceOf(req).Key, action, subject, nowMs())
	return map[string]any{
		"token":       token,
		"action":      action,
		"subject":     subject,
		"summary":     actionSummary(action, subject, label, script),
		"expiresInMs": float64(phonefamily.ConfirmationLifetimeMs),
	}, nil
}

func (m *Module) handleRun(ctx context.Context, req *rpc.Request) (any, error) {
	params := req.Object()
	action, _ := params["action"].(string)
	subject, failure := subjectFor(action, params)
	token, _ := params["token"].(string)
	verdict := m.confirmations.Consume(token, phonefamily.DeviceOf(req).Key, action, subject, nowMs())
	if subject == "" || verdict != phonefamily.Ok {
		why := failure
		if subject != "" {
			why = phonefamily.ConfirmationError(verdict)
		}
		if m.core != nil {
			m.core.LogUI("Refused an insights %s from %s: %s.", action, phonefamily.DeviceLabel(req), why)
		}
		return nil, rpc.Fail(why)
	}
	who := phonefamily.DeviceLabel(req)
	switch action {
	case "toggleFamily":
		return m.runToggleFamily(ctx, subject, who)
	case "runShortcut":
		return m.runShortcutAction(ctx, subject, who)
	}
	return nil, rpc.Fail("unsupported_action")
}

func (m *Module) runToggleFamily(ctx context.Context, subject, who string) (any, error) {
	family := section(subject, 0)
	enabled := strings.HasSuffix(subject, "|on")
	if FamilyByID(family) == nil {
		return nil, rpc.Fail("bad_family")
	}
	if err := m.callDesktop(ctx, "ui.insights.toggleFamily", map[string]any{"family": family, "enabled": enabled}, nil); err != nil {
		if phonefamily.DesktopUnavailable(err) {
			return nil, rpc.Fail("desktop_unavailable")
		}
		return nil, err
	}
	state := "off"
	message := "Switched off."
	if enabled {
		state = "on"
		message = "Switched on."
	}
	if m.core != nil {
		m.core.LogUI("Tool family %s switched %s by %s.", family, state, who)
	}
	phonefamily.BroadcastPhones(m.core, insightsChanged, nil)
	return map[string]any{"started": true, "action": "toggleFamily", "subject": subject, "message": message}, nil
}

func (m *Module) runShortcutAction(ctx context.Context, subject, who string) (any, error) {
	repo, scope, file := section(subject, 0), section(subject, 1), sectionFrom(subject, 2)
	records := loadRepoRecords(m.settings())
	dir, repoIndex := m.shortcutDir(records, repo, scope)
	path := ""
	if dir != "" {
		path = filepath.Join(dir, file)
	}
	info, err := os.Stat(path)
	if repoIndex < 0 || path == "" || err != nil || !info.Mode().IsRegular() {
		return nil, rpc.Fail("unknown_shortcut")
	}
	if scope == "shortcut" && ShortcutKind(file, executableFile(path, info)) == "script" {
		var started struct {
			Run     int    `json:"run"`
			Restart bool   `json:"restart"`
			Name    string `json:"name"`
		}
		if err := m.callDesktop(ctx, "ui.insights.runShortcutScript", map[string]any{"repo": repo, "file": file}, &started); err != nil {
			if phonefamily.DesktopUnavailable(err) {
				return nil, rpc.Fail("desktop_unavailable")
			}
			return nil, err
		}
		verb, message := "started", "Running "+started.Name+" on the desktop."
		if started.Restart {
			verb, message = "restarted", "Stopping "+started.Name+" to run it again on the desktop."
		}
		if m.core != nil {
			m.core.LogUI("Shortcut %s in %s %s on the desktop for %s.", file, repo, verb, who)
		}
		phonefamily.BroadcastPhones(m.core, insightsChanged, nil)
		return map[string]any{"started": true, "action": "runShortcut", "subject": subject, "run": started.Run,
			"message": message}, nil
	}
	var reply struct {
		Session int    `json:"session"`
		Name    string `json:"name"`
	}
	if err := m.callDesktop(ctx, "ui.insights.runShortcutAgent", map[string]any{"repo": repo, "scope": scope, "file": file}, &reply); err != nil {
		if phonefamily.DesktopUnavailable(err) {
			return nil, rpc.Fail("desktop_unavailable")
		}
		return nil, err
	}
	if m.core != nil {
		m.core.LogUI("Shortcut %s in %s started agent #%d for %s.", file, repo, reply.Session, who)
	}
	phonefamily.BroadcastPhones(m.core, insightsChanged, nil)
	phonefamily.BroadcastPhones(m.core, "agents.changed", nil)
	return map[string]any{"started": true, "action": "runShortcut", "subject": subject, "session": reply.Session,
		"message": "Started agent #" + strconv.Itoa(reply.Session) + " for " + reply.Name + "."}, nil
}
