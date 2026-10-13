package diag

import (
	"context"
	"strings"
	"unicode"

	"forkmesh.com/desktop-core/internal/rpc"
)

const (
	maxStallEntries = 100
	maxStallChars   = 64000
)

func stallHead(entry string) (string, string) {
	head := entry
	if i := strings.IndexByte(head, '\n'); i >= 0 {
		head = head[:i]
	}
	if strings.HasPrefix(head, "[") {
		if end := strings.IndexByte(head[1:], ']'); end > 0 {
			when := head[1 : 1+end]
			rest := strings.TrimLeftFunc(head[2+end:], unicode.IsSpace)
			return when, rest
		}
	}
	return "", head
}

func stallRow(index int, entry string) map[string]any {
	when, summary := stallHead(entry)
	return map[string]any{
		"index":   index,
		"when":    left(when, 64),
		"summary": redactSecrets(left(summary, 400)),
		"lines":   strings.Count(entry, "\n") + 1,
		"bytes":   len(entry),
	}
}

func stallDetail(index int, entry string, maxChars int) map[string]any {
	row := stallRow(index, entry)
	var kept []string
	used := 0
	truncated := false
	for _, line := range strings.Split(entry, "\n") {
		clean := redactSecrets(left(line, 2000))
		size := utf16Len(clean)
		if used+size+1 > maxChars {
			truncated = true
			break
		}
		used += size + 1
		kept = append(kept, clean)
	}
	row["text"] = strings.Join(kept, "\n")
	row["truncated"] = truncated
	return row
}

func validEntry(entry string) string {
	return strings.ToValidUTF8(entry, "�")
}

func (m *Module) stallSnapshot() []string {
	m.mu.Lock()
	defer m.mu.Unlock()
	return append([]string(nil), m.stalls...)
}

func (m *Module) handleStallPush(ctx context.Context, req *rpc.Request) (any, error) {
	if req.Peer == nil || rpc.Remote(req.Peer) {
		return nil, rpc.Fail("forbidden")
	}
	params := req.Object()
	m.mu.Lock()
	defer m.mu.Unlock()
	if clear, _ := params["clear"].(bool); clear {
		m.stalls = nil
		return map[string]any{"count": 0}, nil
	}
	entry, ok := params["entry"].(string)
	if !ok {
		return nil, rpc.Fail("bad_request")
	}
	m.stalls = append(m.stalls, validEntry(entry))
	if len(m.stalls) > maxStallEntries {
		m.stalls = append([]string(nil), m.stalls[len(m.stalls)-maxStallEntries:]...)
	}
	return map[string]any{"count": len(m.stalls)}, nil
}

func (m *Module) handleStallSync(ctx context.Context, req *rpc.Request) (any, error) {
	if req.Peer == nil || rpc.Remote(req.Peer) {
		return nil, rpc.Fail("forbidden")
	}
	raw, ok := req.Object()["entries"].([]any)
	if !ok {
		return nil, rpc.Fail("bad_request")
	}
	entries := make([]string, 0, len(raw))
	for _, item := range raw {
		if text, ok := item.(string); ok {
			entries = append(entries, validEntry(text))
		}
	}
	if len(entries) > maxStallEntries {
		entries = entries[len(entries)-maxStallEntries:]
	}
	m.mu.Lock()
	m.stalls = entries
	m.mu.Unlock()
	return map[string]any{"count": len(entries)}, nil
}

func (m *Module) handleStalls(ctx context.Context, req *rpc.Request) (any, error) {
	entries := m.stallSnapshot()
	rows := make([]any, 0, len(entries))
	for i := len(entries) - 1; i >= 0; i-- {
		rows = append(rows, stallRow(i, entries[i]))
	}
	canSend := false
	if len(entries) > 0 {
		if state, err := m.desktopState(ctx, false); err == nil {
			canSend = state.CanSendStalls
		}
	}
	return map[string]any{"stalls": rows, "canSend": canSend}, nil
}

func (m *Module) handleStall(ctx context.Context, req *rpc.Request) (any, error) {
	index := jsonInt(req.Object()["index"], -1)
	entries := m.stallSnapshot()
	if index < 0 || index >= len(entries) {
		return nil, rpc.Fail("unknown_stall")
	}
	return stallDetail(index, entries[index], maxStallChars), nil
}
