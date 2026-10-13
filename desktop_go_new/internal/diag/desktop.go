package diag

import (
	"context"
	"encoding/json"
	"regexp"
	"strconv"
	"strings"
	"time"

	"forkmesh.com/desktop-core/internal/agentproc"
	"forkmesh.com/desktop-core/internal/agents"
	"forkmesh.com/desktop-core/internal/rpc"
)

const desktopStateTimeout = 3 * time.Second

type agentCounts struct {
	Running int `json:"running"`
	Queued  int `json:"queued"`
	Total   int `json:"total"`
}

type agentRoot struct {
	Pid       int64  `json:"pid"`
	Session   int    `json:"session"`
	Label     string `json:"label"`
	Stoppable bool   `json:"stoppable"`
}

type desktopState struct {
	Desktop       string      `json:"desktop"`
	CanSendStalls bool        `json:"canSendStalls"`
	Agents        agentCounts `json:"agents"`
	Roots         []agentRoot `json:"roots"`
	FromDesktop   bool        `json:"-"`
}

func (m *Module) desktopState(ctx context.Context, roots bool) (desktopState, error) {
	if m.UIState != nil {
		return m.UIState(ctx, roots)
	}
	if m.core == nil || m.core.UI() == nil {
		return desktopState{}, rpc.Fail("desktop_unavailable")
	}
	callCtx, cancel := context.WithTimeout(ctx, desktopStateTimeout)
	defer cancel()
	raw, err := m.core.CallUI(callCtx, "ui.diag.state", map[string]any{"roots": roots})
	if err != nil {
		return desktopState{}, err
	}
	var state desktopState
	if err := json.Unmarshal(raw, &state); err != nil {
		return desktopState{}, rpc.Fail("desktop_unavailable")
	}
	state.FromDesktop = true
	return state, nil
}

func (m *Module) desktopPID() int {
	if m.DesktopPID != nil {
		return m.DesktopPID()
	}
	if m.core == nil {
		return 0
	}
	if ui := m.core.UI(); ui != nil {
		return ui.PID()
	}
	return 0
}

func sessionLabel(session agents.Session) string {
	title := strings.TrimSpace(session.IssueTitle)
	if title == "" {
		first := session.Prompt
		if i := strings.IndexByte(first, '\n'); i >= 0 {
			first = first[:i]
		}
		title = strings.TrimSpace(first)
	}
	if title == "" {
		title = "Agent run #" + strconv.Itoa(session.ID)
	}
	return left(title, 120)
}

func stoppable(session agents.Session) bool {
	if session.Merged {
		return false
	}
	return session.Status == agents.StatusRunning || session.Status == agents.StatusWaiting ||
		session.Status == agents.StatusQueued
}

var agentProcessKey = regexp.MustCompile(`^agent-([0-9]+)-[0-9]+$`)

func (m *Module) storeSessions() []agents.Session {
	if m.Sessions != nil {
		return m.Sessions()
	}
	if m.core == nil {
		return nil
	}
	if module, ok := m.core.Module("agents").(*agents.Module); ok && module.Store() != nil {
		return module.Store().LoadAllSessions()
	}
	return nil
}

func (m *Module) agentProcesses() []agentproc.Process {
	if m.AgentProcesses != nil {
		return m.AgentProcesses()
	}
	if m.core == nil {
		return nil
	}
	if module, ok := m.core.Module("agentproc").(*agentproc.Module); ok {
		return module.Processes()
	}
	return nil
}

func (m *Module) coreState(roots bool) desktopState {
	state := desktopState{}
	if info, ok := m.core.Get("control.info").(map[string]any); ok {
		state.Desktop, _ = info["name"].(string)
	}
	sessions := m.storeSessions()
	byID := map[int]agents.Session{}
	for _, session := range sessions {
		byID[session.ID] = session
		state.Agents.Total++
		lower := strings.ToLower(session.Status)
		if stoppable(session) {
			state.Agents.Running++
		} else if strings.Contains(lower, "queue") || strings.Contains(lower, "scheduled") {
			state.Agents.Queued++
		}
	}
	if !roots {
		return state
	}
	for _, process := range m.agentProcesses() {
		if !process.Running || process.Pid <= 0 {
			continue
		}
		match := agentProcessKey.FindStringSubmatch(process.Key)
		if match == nil {
			continue
		}
		id, err := strconv.Atoi(match[1])
		if err != nil {
			continue
		}
		session, ok := byID[id]
		if !ok {
			continue
		}
		state.Roots = append(state.Roots, agentRoot{Pid: int64(process.Pid), Session: id,
			Label: sessionLabel(session), Stoppable: stoppable(session)})
	}
	return state
}

func (m *Module) stateForPhone(ctx context.Context, roots bool) desktopState {
	if state, err := m.desktopState(ctx, roots); err == nil {
		return state
	}
	return m.coreState(roots)
}

func (m *Module) handleHealth(ctx context.Context, req *rpc.Request) (any, error) {
	state := m.stateForPhone(ctx, false)
	pid := m.desktopPID()
	proc := unknownProcess()
	if pid > 0 {
		proc = m.readProc(pid)
	}
	m.mu.Lock()
	r := m.latest
	cpu := m.cpu
	stalls := len(m.stalls)
	m.mu.Unlock()
	summary := systemSummary(r, cpu, proc)
	summary["agents"] = state.Agents
	summary["stalls"] = stalls
	summary["desktop"] = state.Desktop
	summary["sampledAtMs"] = time.Now().UnixMilli()
	return summary, nil
}
