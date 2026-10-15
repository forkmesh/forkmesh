package agentmodel

import (
	"os"
	"path/filepath"
	"sort"
	"strings"

	"forkmesh.com/desktop-core/internal/agentrun"
	"forkmesh.com/desktop-core/internal/agents"
	"forkmesh.com/desktop-core/internal/qtjson"
)

const (
	maxListedSessions       = 200
	kMinMaxRunningAgents    = 1
	kDefaultMaxRunning      = 5
	kQueueMenuMaxRunning    = 20
	maxRunningAgentsSetting = "agents/maxRunning"
)

func sessionActive(status string) bool {
	return status == agents.StatusQueued || status == agents.StatusRunning || status == agents.StatusWaiting
}

func awaitingStart(s agents.Session) bool {
	return s.Held && s.Status == agents.StatusQueued && !s.Merged
}

func stoppable(s agents.Session) bool {
	if s.Merged {
		return false
	}
	return s.Status == agents.StatusRunning || s.Status == agents.StatusWaiting || s.Status == agents.StatusQueued
}

func (m *Module) maxRunningAgents(ctx *desktopContext) int {
	if ctx != nil && ctx.Limit > 0 {
		return max(kMinMaxRunningAgents, ctx.Limit)
	}
	limit := int64(kDefaultMaxRunning)
	if m.core != nil && m.core.Settings != nil {
		if value, ok := m.core.Settings.Get().Value(maxRunningAgentsSetting); ok {
			limit = value.Int(0)
		}
	}
	return max(kMinMaxRunningAgents, int(limit))
}

func indexSessions(sessions []agents.Session) map[int]*agents.Session {
	out := make(map[int]*agents.Session, len(sessions))
	for i := range sessions {
		if _, seen := out[sessions[i].ID]; !seen {
			out[sessions[i].ID] = &sessions[i]
		}
	}
	return out
}

func queueOrder(ctx *desktopContext, byID map[int]*agents.Session) []int {
	order := []int{}
	seen := map[int]bool{}
	for _, id := range ctx.Queue {
		session := byID[id]
		if session == nil || session.Status != agents.StatusQueued || session.Held || session.DeletedAtMs > 0 ||
			ctx.external(id) || seen[id] {
			continue
		}
		seen[id] = true
		order = append(order, id)
	}
	sort.SliceStable(order, func(i, j int) bool {
		return byID[order[i]].Priority < byID[order[j]].Priority
	})
	return order
}

func (m *Module) queuePayload(ctx *desktopContext, byID map[int]*agents.Session) qtjson.Value {
	queue := qtjson.NewArray()
	for _, id := range queueOrder(ctx, byID) {
		queue.Append(qtjson.NewInt(int64(id)))
	}
	limit := m.maxRunningAgents(ctx)
	return qtjson.ObjectOf("running", ctx.Running, "limit", limit, "limitMax", max(kQueueMenuMaxRunning, limit),
		"queue", queue)
}

func (m *Module) promptImagesDir() string {
	if m.core == nil {
		return filepath.Join(os.TempDir(), "agent-images")
	}
	return filepath.Join(m.core.Layout.Data, "agent-images")
}

func (m *Module) resolveImage(path string) string {
	if path == "" {
		return ""
	}
	if _, err := os.Stat(path); err == nil {
		return path
	}
	moved := filepath.Join(m.promptImagesDir(), filepath.Base(path))
	if _, err := os.Stat(moved); err == nil {
		return moved
	}
	return ""
}

func mediaTypeForName(name string) string {
	switch strings.ToLower(strings.TrimPrefix(filepath.Ext(name), ".")) {
	case "jpg", "jpeg":
		return "image/jpeg"
	case "png":
		return "image/png"
	case "gif":
		return "image/gif"
	case "webp":
		return "image/webp"
	case "heic", "heif":
		return "image/heic"
	case "bmp":
		return "image/bmp"
	case "mp4", "m4v":
		return "video/mp4"
	case "mov":
		return "video/quicktime"
	case "webm":
		return "video/webm"
	}
	return "application/octet-stream"
}

type attachmentEntry struct {
	ID        string
	Path      string
	Name      string
	MediaType string
	Size      int64
}

func canonicalFile(raw string) (string, os.FileInfo, bool) {
	info, err := os.Stat(raw)
	if err != nil || !info.Mode().IsRegular() {
		return "", nil, false
	}
	file, err := os.Open(raw)
	if err != nil {
		return "", nil, false
	}
	file.Close()
	canonical, err := filepath.EvalSymlinks(raw)
	if err != nil {
		return "", nil, false
	}
	if abs, err := filepath.Abs(canonical); err == nil {
		canonical = abs
	}
	return canonical, info, true
}

func (m *Module) attachmentEntries(paths []string, sessionID int, limit int) []attachmentEntry {
	out := []attachmentEntry{}
	seen := map[string]bool{}
	salt := m.salt()
	for _, raw := range paths {
		if len(out) >= limit {
			break
		}
		canonical, info, ok := canonicalFile(raw)
		if !ok || seen[canonical] {
			continue
		}
		seen[canonical] = true
		name := filepath.Base(raw)
		out = append(out, attachmentEntry{ID: agentrun.AttachmentID(salt, sessionID, canonical), Path: canonical,
			Name: name, MediaType: mediaTypeForName(name), Size: info.Size()})
	}
	return out
}

func attachmentsJSON(entries []attachmentEntry) qtjson.Value {
	out := qtjson.NewArray()
	for _, entry := range entries {
		out.Append(qtjson.ObjectOf("id", entry.ID, "name", entry.Name, "mediaType", entry.MediaType,
			"size", float64(entry.Size)))
	}
	return out
}

func (m *Module) sessionAttachmentPaths(store *agents.Store, session agents.Session, known []string) []string {
	paths := []string{}
	contains := func(path string) bool {
		for _, existing := range paths {
			if existing == path {
				return true
			}
		}
		return false
	}
	for _, raw := range store.AttachmentPaths(session) {
		if path := m.resolveImage(raw); path != "" && !contains(path) {
			paths = append(paths, path)
		}
	}
	for _, path := range known {
		if !contains(path) {
			paths = append(paths, path)
		}
	}
	return paths
}

func listedSession(ctx *desktopContext, s agents.Session) bool {
	return !ctx.external(s.ID) && s.DeletedAtMs <= 0 && !s.AssociationOnly
}

func (m *Module) sessionRow(ctx *desktopContext, s agents.Session) qtjson.Value {
	row := qtjson.ObjectOf(
		"id", s.ID,
		"repositoryOwner", s.Owner,
		"repositoryName", s.Name,
		"issueNumber", s.IssueNumber,
		"issueTitle", s.IssueTitle,
		"priority", s.Priority,
		"prompt", left16(s.Prompt, 600),
		"status", s.Status,
		"provider", s.Provider,
		"model", s.Model,
		"mode", s.Mode,
		"branchName", s.BranchName,
		"baseBranch", s.BaseBranch,
		"prNumber", s.PrNumber,
		"merged", s.Merged,
		"createdAtMs", float64(s.CreatedAtMs),
		"startedAtMs", float64(s.StartedAtMs),
		"finishedAtMs", float64(s.FinishedAtMs),
		"numTurns", s.NumTurns,
		"durationMs", float64(s.DurationMs),
		"costUsd", s.CostUsd,
		"totalTokens", s.TotalTokens,
		"lastError", left16(s.LastError, 600),
		"stoppable", stoppable(s),
	)
	if awaitingStart(s) {
		row.Set("held", qtjson.NewBool(true))
	}
	live := ctx.Live[s.ID]
	row.Set("unread", qtjson.NewBool(live.Unread))
	if live.Terminal != "" {
		row.Set("terminal", qtjson.NewString(live.Terminal))
	}
	if live.DiffStats.IsObject() {
		row.Set("diffStats", live.DiffStats)
		row.Set("worktree", qtjson.NewBool(live.Worktree))
	}
	if len(live.Images) > 0 {
		if first := m.attachmentEntries(live.Images, s.ID, 1); len(first) > 0 {
			row.Set("attachments", attachmentsJSON(first))
		}
	}
	return row
}

func (m *Module) detachedRunning(ctx *desktopContext, sessions []agents.Session) {
	if m.desktopAttached() {
		return
	}
	running := 0
	for _, s := range sessions {
		if !s.Merged && s.Status == agents.StatusRunning && !ctx.external(s.ID) {
			running++
		}
	}
	ctx.Running = running
}

func (m *Module) agentsPayload(ctx *desktopContext, sessions []agents.Session, credentials []string) qtjson.Value {
	m.detachedRunning(ctx, sessions)
	listed := []agents.Session{}
	for _, s := range sessions {
		if listedSession(ctx, s) {
			listed = append(listed, s)
		}
	}
	sort.SliceStable(listed, func(i, j int) bool {
		a, b := sessionActive(listed[i].Status), sessionActive(listed[j].Status)
		if a != b {
			return a
		}
		return listed[i].CreatedAtMs > listed[j].CreatedAtMs
	})
	if len(listed) > maxListedSessions {
		listed = listed[:maxListedSessions]
	}
	list := qtjson.NewArray()
	for _, s := range listed {
		list.Append(agentrun.Redact(m.sessionRow(ctx, s), credentials))
	}
	repositories := qtjson.NewArray()
	seen := map[string]bool{}
	for _, repo := range ctx.Repositories {
		if repo.LocalPath == "" || repo.Owner == "" || repo.Name == "" {
			continue
		}
		key := strings.ToLower(repo.Owner) + "/" + strings.ToLower(repo.Name)
		if seen[key] {
			continue
		}
		seen[key] = true
		repositories.Append(qtjson.ObjectOf("owner", repo.Owner, "name", repo.Name))
	}
	folders := ctx.Folders
	if !folders.IsArray() {
		folders = qtjson.NewArray()
	}
	return qtjson.ObjectOf(
		"sessions", list,
		"outside", agentrun.Redact(ctx.Outside, credentials),
		"repositories", repositories,
		"folders", folders,
		"providers", ctx.Providers,
		"defaultProvider", ctx.DefaultProvider,
		"defaultMode", ctx.DefaultMode,
		"running", ctx.Running,
		"queue", m.queuePayload(ctx, indexSessions(sessions)),
	)
}
