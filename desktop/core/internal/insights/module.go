package insights

import (
	"context"
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"sort"
	"sync"
	"time"

	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/phonefamily"
	"forkmesh.com/desktop-core/internal/qtjson"
	"forkmesh.com/desktop-core/internal/rpc"
)

const (
	ToolLedgerFile      = "tool-efficiency.json"
	ImprovementsFile    = "tool-improvements.jsonl"
	GuardrailLedgerFile = "guardrails.json"
	OffsetsFile         = "insights-offsets.json"
	maxOffsets          = 512
)

type Module struct {
	mu            sync.Mutex
	core          *daemon.Core
	root          string
	ledger        *Ledger
	improvements  *ImprovementLog
	guardrails    *GuardrailLedger
	offsets       map[string]int64
	offsetsPath   string
	toolSave      *time.Timer
	guardSave     *time.Timer
	toolsUnsaved  bool
	guardUnsaved  bool
	push          *time.Timer
	pushSections  map[string]bool
	revision      int64
	stopped       bool
	gate          *phonefamily.Gate
	confirmations *phonefamily.Confirmations
	feed          *feed
	shortcuts     *shortcutRunner

	SaveDelay time.Duration
	PushDelay time.Duration
	FeedEvery time.Duration
	Sample    func(roots []NetRoot) NetSample
	CallUI    func(ctx context.Context, method string, params any) ([]byte, error)
	Attached  func() bool
}

func (m *Module) uiAttached() bool {
	if m.Attached != nil {
		return m.Attached()
	}
	return m.core != nil && m.core.UI() != nil
}

func (m *Module) callUI(ctx context.Context, method string, params any) ([]byte, error) {
	if m.CallUI != nil {
		return m.CallUI(ctx, method, params)
	}
	if m.core == nil {
		return nil, rpc.Fail("desktop_unavailable")
	}
	return m.core.CallUI(ctx, method, params)
}

func New() *Module {
	return &Module{SaveDelay: 5 * time.Second, PushDelay: 150 * time.Millisecond, FeedEvery: time.Second,
		confirmations: phonefamily.NewConfirmations(), pushSections: map[string]bool{}, offsets: map[string]int64{}}
}

func (m *Module) Name() string { return "insights" }

func (m *Module) Root() string { return m.root }

func (m *Module) Start(ctx context.Context, core *daemon.Core) error {
	m.core = core
	m.root = filepath.Join(core.Layout.Data, "agents")
	m.offsetsPath = filepath.Join(core.Layout.State, "core", OffsetsFile)
	m.gate = &phonefamily.Gate{Core: core}
	m.open()
	local := map[string]rpc.Handler{
		"core.insights.ingest":      m.handleIngest,
		"core.insights.abandon":     m.handleAbandon,
		"core.insights.disabled":    m.handleDisabled,
		"core.insights.guardrail":   m.handleGuardrail,
		"core.insights.clear":       m.handleClear,
		"core.insights.archiveSent": m.handleArchiveSent,
		"core.insights.state":       m.handleState,
		"core.insights.reload":      m.handleReload,
		"core.insights.digest":      m.handleDigest,
	}
	for method, handler := range local {
		core.Registry.Handle(method, m.Name(), localOnly(handler))
	}
	m.registerShortcuts(core)
	m.registerPhone(core)
	m.feed = newFeed(m)
	go m.feed.run(ctx)
	return nil
}

func localOnly(handler rpc.Handler) rpc.Handler {
	return func(ctx context.Context, req *rpc.Request) (any, error) {
		if req.Peer == nil || rpc.Remote(req.Peer) {
			return nil, rpc.Fail("forbidden")
		}
		return handler(ctx, req)
	}
}

func (m *Module) open() {
	m.mu.Lock()
	defer m.mu.Unlock()
	m.ledger = NewLedger(filepath.Join(m.root, ToolLedgerFile))
	if err := m.ledger.Load(); err != nil {
		m.logf("insights: agent tool ledger: %v", err)
	}
	m.improvements = NewImprovementLog(filepath.Join(m.root, ImprovementsFile))
	if err := m.improvements.Load(); err != nil {
		m.logf("insights: agent tool improvements: %v", err)
	}
	m.guardrails = NewGuardrailLedger(filepath.Join(m.root, GuardrailLedgerFile))
	if err := m.guardrails.Load(); err != nil {
		m.logf("insights: agent guardrail ledger: %v", err)
	}
	m.ledger.SetDisabledFamilies(m.settingsDisabled())
	m.wireLocked()
	m.loadOffsetsLocked()
}

func (m *Module) wireLocked() {
	m.ledger.Changed = func() { m.toolsChangedLocked("tools") }
	m.improvements.Changed = func() { m.toolsChangedLocked("improvements") }
	m.ledger.CallObserved = func(call Observation) { m.improvements.Observe(call) }
	m.guardrails.Changed = m.guardrailsChangedLocked
}

func (m *Module) logf(format string, args ...any) {
	if m.core != nil && m.core.Logger != nil {
		m.core.Logger.Printf(format, args...)
	}
}

func (m *Module) settingsDisabled() []string {
	if m.core == nil || m.core.Settings == nil {
		return nil
	}
	return NormalizeDisabledFamilies(m.core.Settings.Get().Strings(DisabledFamiliesSetting))
}

func (m *Module) toolsChangedLocked(section string) {
	m.toolsUnsaved = true
	if m.toolSave == nil && !m.stopped {
		m.toolSave = time.AfterFunc(m.SaveDelay, m.saveTools)
	}
	m.schedulePushLocked(section)
}

func (m *Module) guardrailsChangedLocked() {
	m.guardUnsaved = true
	if m.guardSave == nil && !m.stopped {
		m.guardSave = time.AfterFunc(m.SaveDelay, m.saveGuardrails)
	}
	m.schedulePushLocked("guardrails")
}

func (m *Module) schedulePushLocked(section string) {
	m.pushSections[section] = true
	if m.push == nil && !m.stopped {
		m.push = time.AfterFunc(m.PushDelay, m.pushChanges)
	}
}

func (m *Module) saveTools() {
	m.mu.Lock()
	m.toolSave = nil
	path := m.ledger.Path()
	data := m.ledger.Encoded()
	offsets := m.offsetsJSONLocked()
	m.toolsUnsaved = false
	m.mu.Unlock()
	if err := writeAtomic(path, data); err != nil {
		m.logf("insights: cannot write %s: %v", path, err)
	}
	m.saveOffsets(offsets)
}

func (m *Module) saveGuardrails() {
	m.mu.Lock()
	m.guardSave = nil
	if !m.guardrails.HasEvents() {
		m.mu.Unlock()
		return
	}
	path := m.guardrails.Path()
	data := m.guardrails.Encoded()
	m.guardUnsaved = false
	m.mu.Unlock()
	if err := writeAtomic(path, data); err != nil {
		m.logf("insights: cannot write %s: %v", path, err)
	}
}

func (m *Module) pushChanges() {
	m.mu.Lock()
	m.push = nil
	sections := m.pushSections
	m.pushSections = map[string]bool{}
	payload := m.snapshotLocked(sections)
	m.mu.Unlock()
	if m.core != nil {
		m.core.BroadcastUI("core.insights.changed", payload)
	}
}

func (m *Module) snapshotLocked(sections map[string]bool) map[string]any {
	m.revision++
	payload := map[string]any{"revision": m.revision, "disabled": nonNil(m.ledger.DisabledFamilies()),
		"root": m.root}
	if sections["tools"] {
		payload["tools"] = json.RawMessage(m.ledger.SnapshotJSON().Compact())
	}
	if sections["improvements"] {
		payload["improvements"] = map[string]any{
			"signals":     json.RawMessage(m.improvements.SignalsJSON().Compact()),
			"occurrences": m.improvements.OccurrenceCount(),
			"path":        m.improvements.Path(),
		}
	}
	if sections["guardrails"] {
		payload["guardrails"] = json.RawMessage(m.guardrails.ToJSON().Compact())
	}
	return payload
}

func nonNil(values []string) []string {
	if values == nil {
		return []string{}
	}
	return values
}

func (m *Module) Stop() {
	m.mu.Lock()
	m.stopped = true
	for _, timer := range []*time.Timer{m.toolSave, m.guardSave, m.push} {
		if timer != nil {
			timer.Stop()
		}
	}
	m.toolSave, m.guardSave, m.push = nil, nil, nil
	saveTools := m.ledger != nil && (m.ledger.HasSamples() || fileExists(m.ledger.Path()))
	var toolData, guardData, offsets []byte
	if saveTools {
		toolData = m.ledger.Encoded()
		offsets = m.offsetsJSONLocked()
	}
	if m.guardrails != nil && m.guardUnsaved && m.guardrails.HasEvents() {
		guardData = m.guardrails.Encoded()
	}
	toolPath, guardPath := "", ""
	if m.ledger != nil {
		toolPath = m.ledger.Path()
	}
	if m.guardrails != nil {
		guardPath = m.guardrails.Path()
	}
	m.mu.Unlock()
	if m.shortcuts != nil {
		m.shortcuts.stopAll()
	}
	if toolData != nil {
		if err := writeAtomic(toolPath, toolData); err != nil {
			m.logf("insights: cannot write %s: %v", toolPath, err)
		}
		m.saveOffsets(offsets)
	}
	if guardData != nil {
		if err := writeAtomic(guardPath, guardData); err != nil {
			m.logf("insights: cannot write %s: %v", guardPath, err)
		}
	}
}

func (m *Module) Busy() bool {
	return m.shortcuts != nil && m.shortcuts.busy()
}

func fileExists(path string) bool {
	_, err := os.Stat(path)
	return err == nil
}

func (m *Module) loadOffsetsLocked() {
	data, err := os.ReadFile(m.offsetsPath)
	if err != nil {
		return
	}
	parsed := map[string]int64{}
	if json.Unmarshal(data, &parsed) == nil {
		m.offsets = parsed
	}
}

func (m *Module) offsetsJSONLocked() []byte {
	if len(m.offsets) > maxOffsets {
		type entry struct {
			key string
			so  int64
		}
		entries := make([]entry, 0, len(m.offsets))
		for key, so := range m.offsets {
			entries = append(entries, entry{key, so})
		}
		sort.Slice(entries, func(i, j int) bool { return entries[i].key > entries[j].key })
		kept := map[string]int64{}
		for _, e := range entries[:maxOffsets] {
			kept[e.key] = e.so
		}
		m.offsets = kept
	}
	data, _ := json.Marshal(m.offsets)
	return data
}

func (m *Module) saveOffsets(data []byte) {
	if data == nil || m.offsetsPath == "" {
		return
	}
	if err := writeAtomic(m.offsetsPath, data); err != nil {
		m.logf("insights: cannot write %s: %v", m.offsetsPath, err)
	}
}

func (m *Module) Ingest(context SessionContext, event qtjson.Value) {
	m.mu.Lock()
	defer m.mu.Unlock()
	m.ingestLocked(context, event)
}

func (m *Module) ingestLocked(context SessionContext, event qtjson.Value) {
	switch event.Get("type").Str() {
	case "result", "_codex_agent_complete":
		m.ledger.AbandonSession(context.SessionID, -1)
		m.improvements.ForgetSession(context.SessionID)
	case "assistant", "user":
		m.ledger.Ingest(context, event, -1)
	}
}

type ingestParams struct {
	SessionID int             `json:"sessionId"`
	Provider  string          `json:"provider"`
	Model     string          `json:"model"`
	Workdir   string          `json:"workdir"`
	Event     json.RawMessage `json:"event"`
}

func (m *Module) handleIngest(ctx context.Context, req *rpc.Request) (any, error) {
	var params ingestParams
	if err := req.Bind(&params); err != nil {
		return nil, err
	}
	event, err := qtjson.Parse(params.Event)
	if err != nil || !event.IsObject() {
		return nil, rpc.Fail("bad_event")
	}
	m.Ingest(SessionContext{SessionID: params.SessionID, Provider: params.Provider, Model: params.Model,
		Workdir: params.Workdir}, event)
	return map[string]any{"ok": true}, nil
}

func (m *Module) handleAbandon(ctx context.Context, req *rpc.Request) (any, error) {
	var params struct {
		SessionID int `json:"sessionId"`
	}
	if err := req.Bind(&params); err != nil {
		return nil, err
	}
	m.mu.Lock()
	m.ledger.AbandonSession(params.SessionID, -1)
	m.improvements.ForgetSession(params.SessionID)
	m.mu.Unlock()
	return map[string]any{"ok": true}, nil
}

func (m *Module) handleDisabled(ctx context.Context, req *rpc.Request) (any, error) {
	var params struct {
		Families []string `json:"families"`
	}
	if err := req.Bind(&params); err != nil {
		return nil, err
	}
	m.mu.Lock()
	m.ledger.SetDisabledFamilies(params.Families)
	disabled := m.ledger.DisabledFamilies()
	m.mu.Unlock()
	return map[string]any{"disabled": nonNil(disabled)}, nil
}

type guardrailParams struct {
	Op         string `json:"op"`
	Repository string `json:"repository"`
	Tool       string `json:"tool"`
	Path       string `json:"path"`
	Outside    bool   `json:"outside"`
	Approved   bool   `json:"approved"`
	Count      int    `json:"count"`
}

func (m *Module) handleGuardrail(ctx context.Context, req *rpc.Request) (any, error) {
	var params guardrailParams
	if err := req.Bind(&params); err != nil {
		return nil, err
	}
	m.mu.Lock()
	defer m.mu.Unlock()
	switch params.Op {
	case "blocked":
		m.guardrails.RecordBlocked(params.Repository, params.Tool, params.Path, params.Outside, -1)
	case "decision":
		m.guardrails.RecordDecision(params.Repository, params.Approved, -1)
	case "stopped":
		m.guardrails.RecordAgentsStopped(params.Repository, params.Count, -1)
	default:
		return nil, rpc.Fail("bad_op")
	}
	return map[string]any{"ok": true}, nil
}

func (m *Module) handleClear(ctx context.Context, req *rpc.Request) (any, error) {
	var params struct {
		What string `json:"what"`
	}
	if err := req.Bind(&params); err != nil {
		return nil, err
	}
	switch params.What {
	case "tools":
		m.mu.Lock()
		m.ledger.Clear()
		path, data := m.ledger.Path(), m.ledger.Encoded()
		m.mu.Unlock()
		if err := writeAtomic(path, data); err != nil {
			m.logf("insights: cannot write %s: %v", path, err)
		}
	case "improvements":
		m.mu.Lock()
		m.improvements.Clear()
		m.mu.Unlock()
	case "guardrails":
		m.mu.Lock()
		m.guardrails.Clear()
		path, data := m.guardrails.Path(), m.guardrails.Encoded()
		m.mu.Unlock()
		if err := writeAtomic(path, data); err != nil {
			m.logf("insights: cannot write %s: %v", path, err)
		}
	default:
		return nil, rpc.Fail("bad_request")
	}
	return map[string]any{"ok": true}, nil
}

func (m *Module) handleArchiveSent(ctx context.Context, req *rpc.Request) (any, error) {
	m.mu.Lock()
	ok := m.improvements.ArchiveSent()
	m.mu.Unlock()
	return map[string]any{"ok": ok}, nil
}

func (m *Module) handleDigest(ctx context.Context, req *rpc.Request) (any, error) {
	var params struct {
		Limit       int    `json:"limit"`
		FullLogPath string `json:"fullLogPath"`
	}
	params.Limit = 25
	if err := req.Bind(&params); err != nil {
		return nil, err
	}
	m.mu.Lock()
	text := m.improvements.DigestMarkdown(params.Limit, params.FullLogPath)
	m.mu.Unlock()
	return map[string]any{"markdown": text}, nil
}

func (m *Module) handleState(ctx context.Context, req *rpc.Request) (any, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	return m.snapshotLocked(map[string]bool{"tools": true, "improvements": true, "guardrails": true}), nil
}

func (m *Module) handleReload(ctx context.Context, req *rpc.Request) (any, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	reloaded := []string{}
	if !m.toolsUnsaved && m.ledger.PendingCount() == 0 {
		fresh := NewLedger(m.ledger.Path())
		if err := fresh.Load(); err == nil {
			fresh.SetDisabledFamilies(m.ledger.DisabledFamilies())
			m.ledger = fresh
			fresh2 := NewImprovementLog(m.improvements.Path())
			if fresh2.Load() == nil {
				m.improvements = fresh2
			}
			reloaded = append(reloaded, "tools", "improvements")
		}
	}
	if !m.guardUnsaved {
		fresh := NewGuardrailLedger(m.guardrails.Path())
		if err := fresh.Load(); err == nil {
			m.guardrails = fresh
			reloaded = append(reloaded, "guardrails")
		}
	}
	m.wireLocked()
	payload := m.snapshotLocked(map[string]bool{"tools": true, "improvements": true, "guardrails": true})
	payload["reloaded"] = reloaded
	return payload, nil
}

var errNoLedger = errors.New("insights: no ledger")

func (m *Module) withLedger(fn func(l *Ledger, g *ImprovementLog, r *GuardrailLedger)) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	if m.ledger == nil {
		return errNoLedger
	}
	fn(m.ledger, m.improvements, m.guardrails)
	return nil
}
