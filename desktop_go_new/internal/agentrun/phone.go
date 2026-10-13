package agentrun

import (
	"context"
	"crypto/rand"
	"crypto/sha256"
	"encoding/hex"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"unicode/utf16"
	"unicode/utf8"

	"forkmesh.com/desktop-core/internal/agents"
	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/phonefamily"
	"forkmesh.com/desktop-core/internal/qtjson"
	"forkmesh.com/desktop-core/internal/rpc"
)

const (
	maxTranscriptChars      = 64000
	maxTranscriptDeltaChars = 16000
	maxAttachmentEntries    = 50
	maxBinAgents            = 500
	recycleMinHours         = agents.MinRecycleRetentionHours
	recycleMaxHours         = agents.MaxRecycleRetentionHours
	recycleDefaultHours     = agents.DefaultRecycleRetentionHours
	recycleRetentionSetting = "agents/recycleRetentionHours"
)

func (m *Module) registerPhone(core *daemon.Core) {
	m.salt = make([]byte, 32)
	_, _ = rand.Read(m.salt)
	m.gate = &phonefamily.Gate{Core: core}
	core.Registry.Handle("agents.transcript", m.Name(), m.handlePhoneTranscript)
	core.Registry.Handle("agentops.bin", m.Name(), m.gate.Wrap(m.handlePhoneBin))
	core.Registry.Handle("core.agentrun.salt", m.Name(), local(func(ctx context.Context, req *rpc.Request) (any, error) {
		return map[string]any{"salt": hex.EncodeToString(m.salt)}, nil
	}))
}

func paramsObject(req *rpc.Request) qtjson.Value {
	if params, ok := qtjson.ParseObject(req.Params); ok {
		return params
	}
	return qtjson.NewObject()
}

func qtUTF16(data []byte) []uint16 {
	out := make([]uint16, 0, len(data))
	for i := 0; i < len(data); {
		c := data[i]
		if c < 0x80 {
			out = append(out, uint16(c))
			i++
			continue
		}
		r, size := utf8.DecodeRune(data[i:])
		if r == utf8.RuneError && size <= 1 {
			out = append(out, 0xFFFD)
			i++
			continue
		}
		out = utf16.AppendRune(out, r)
		i += size
	}
	return out
}

func withoutCR(data []byte) []byte {
	if !strings.Contains(string(data), "\r") {
		return data
	}
	out := make([]byte, 0, len(data))
	for _, c := range data {
		if c != '\r' {
			out = append(out, c)
		}
	}
	return out
}

func activeStatus(status string) bool {
	return status == agents.StatusQueued || status == agents.StatusRunning || status == agents.StatusWaiting
}

func (m *Module) handlePhoneTranscript(ctx context.Context, req *rpc.Request) (any, error) {
	params := paramsObject(req)
	id := params.Get("id").IntOr(0)
	writer := m.writer()
	if id < 0 || writer == nil || !m.haveCredentials() {
		return nil, rpc.ErrDelegate
	}
	if id == 0 {
		return nil, rpc.Fail("unknown_session")
	}
	session, ok := writer.Store().FindSession(id)
	if !ok {
		return nil, rpc.ErrDelegate
	}
	since := int64(-1)
	if params.Has("since") {
		since = int64(params.Get("since").Float())
	}
	dir := writer.Store().SessionDir(session)
	writer.FlushDir(dir)
	data, _ := os.ReadFile(filepath.Join(dir, "transcript.txt"))
	text := qtUTF16(withoutCR(data))
	total := int64(len(text))
	from := int64(0)
	reset := false
	if since >= 0 && since <= total {
		from = since
		if total-from > maxTranscriptDeltaChars {
			from = total - maxTranscriptDeltaChars
		}
	} else {
		reset = since >= 0
		if total > maxTranscriptChars {
			from = total - maxTranscriptChars
		}
	}
	return map[string]any{
		"id":          id,
		"transcript":  RedactText(fromU16(text[from:]), m.credentialValues()),
		"from":        from,
		"next":        total,
		"reset":       reset,
		"active":      activeStatus(session.Status),
		"status":      session.Status,
		"attachments": m.attachmentEntries(writer.Store(), session),
	}, nil
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

func AttachmentID(salt []byte, sessionID int, path string) string {
	hash := sha256.New()
	hash.Write(salt)
	hash.Write([]byte("\n" + strconv.Itoa(sessionID) + "\n"))
	hash.Write([]byte(path))
	return hex.EncodeToString(hash.Sum(nil))[:32]
}

func (m *Module) attachmentEntries(store *agents.Store, session agents.Session) []map[string]any {
	paths := []string{}
	for _, raw := range store.AttachmentPaths(session) {
		if path := m.resolveImage(raw); path != "" && !containsPath(paths, path) {
			paths = append(paths, path)
		}
	}
	out := []map[string]any{}
	seen := map[string]bool{}
	for _, raw := range paths {
		if len(out) >= maxAttachmentEntries {
			break
		}
		info, err := os.Stat(raw)
		if err != nil || !info.Mode().IsRegular() {
			continue
		}
		file, err := os.Open(raw)
		if err != nil {
			continue
		}
		file.Close()
		canonical, err := filepath.EvalSymlinks(raw)
		if err != nil {
			continue
		}
		if abs, err := filepath.Abs(canonical); err == nil {
			canonical = abs
		}
		if seen[canonical] {
			continue
		}
		seen[canonical] = true
		name := filepath.Base(raw)
		out = append(out, map[string]any{"id": AttachmentID(m.salt, session.ID, canonical), "name": name,
			"mediaType": mediaTypeForName(name), "size": info.Size()})
	}
	return out
}

func containsPath(list []string, value string) bool {
	for _, item := range list {
		if item == value {
			return true
		}
	}
	return false
}

func binTitle(issueNumber int, issueTitle, prompt string) string {
	title := prompt
	if issueNumber > 0 && issueTitle != "" {
		title = issueTitle
	}
	if cut := strings.IndexByte(title, '\n'); cut >= 0 {
		title = title[:cut]
	}
	title = simplified(title)
	if len16(title) > 90 {
		title = left16(title, 89) + "…"
	}
	return title
}

func (m *Module) recycleRetentionHours() int {
	hours := int64(recycleDefaultHours)
	if m.core != nil && m.core.Settings != nil {
		if value, ok := m.core.Settings.Get().Value(recycleRetentionSetting); ok {
			hours = value.Int(0)
		}
	}
	return int(min(max(hours, recycleMinHours), recycleMaxHours))
}

func (m *Module) handlePhoneBin(ctx context.Context, req *rpc.Request) (any, error) {
	writer := m.writer()
	if writer == nil {
		return nil, rpc.Fail("unavailable")
	}
	sessions := writer.Store().LoadRecycledSessions()
	hours := m.recycleRetentionHours()
	retentionMs := int64(hours) * 60 * 60 * 1000
	now := m.now().UnixMilli()
	rows := []map[string]any{}
	for _, session := range sessions {
		if len(rows) >= maxBinAgents {
			break
		}
		removeIn := int64(-1)
		if session.DeletedAtMs > 0 {
			removeIn = max(0, retentionMs-(now-session.DeletedAtMs))
		}
		rows = append(rows, map[string]any{"id": session.ID,
			"title": binTitle(session.IssueNumber, session.IssueTitle, session.Prompt),
			"owner": session.Owner, "name": session.Name, "branch": session.BranchName,
			"provider": session.Provider, "deletedAtMs": session.DeletedAtMs, "removeInMs": removeIn})
	}
	return map[string]any{"agents": rows, "count": len(sessions), "truncated": len(sessions) > len(rows),
		"retentionHours": hours, "minHours": recycleMinHours, "maxHours": recycleMaxHours}, nil
}
