package quality

import (
	"context"
	"encoding/json"
	"path/filepath"
	"sync"
	"time"

	"forkmesh.com/desktop-core/internal/actions"
	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/phonefamily"
	"forkmesh.com/desktop-core/internal/rpc"
)

const factsTimeout = 30 * time.Second

type Facts struct {
	Owner              string  `json:"owner"`
	Name               string  `json:"name"`
	LocalPath          string  `json:"localPath"`
	MirrorPath         string  `json:"mirrorPath"`
	WritableLocalPath  *string `json:"writableLocalPath"`
	WritableMirrorPath *string `json:"writableMirrorPath"`
}

type FactsLocator func(ctx context.Context, req *rpc.Request, repo string) (Facts, error)

type Module struct {
	Owners        func() []string
	Now           func() time.Time
	Git           string
	Facts         FactsLocator
	IdentityValid func() bool
	ActionsRoot   string

	core     *daemon.Core
	gate     *phonefamily.Gate
	store    *actions.Store
	mu       sync.Mutex
	inFlight map[string]bool
	active   int
}

func New() *Module { return &Module{inFlight: map[string]bool{}} }

func (m *Module) Name() string { return "quality" }

func (m *Module) Busy() bool {
	m.mu.Lock()
	defer m.mu.Unlock()
	return m.active > 0
}

func (m *Module) now() time.Time {
	if m.Now != nil {
		return m.Now()
	}
	return time.Now()
}

func (m *Module) Start(ctx context.Context, core *daemon.Core) error {
	m.core = core
	if m.inFlight == nil {
		m.inFlight = map[string]bool{}
	}
	root := m.ActionsRoot
	if root == "" {
		root = filepath.Join(core.Layout.Data, "actions")
	}
	m.store = actions.NewStore(root)
	if m.Facts == nil {
		m.Facts = m.factsFromUI
	}
	if m.IdentityValid == nil {
		m.IdentityValid = func() bool {
			_, err := core.RequireIdentity()
			return err == nil
		}
	}
	m.gate = &phonefamily.Gate{Core: core, Owners: m.Owners}
	core.Registry.Handle("stats.quality", m.Name(), m.gate.Wrap(m.handleQuality))
	return nil
}

func (m *Module) factsFromUI(ctx context.Context, req *rpc.Request, repo string) (Facts, error) {
	if m.core == nil {
		return Facts{}, rpc.Fail("desktop_unavailable")
	}
	callCtx, cancel := context.WithTimeout(ctx, factsTimeout)
	defer cancel()
	raw, err := m.core.CallUI(callCtx, "ui.actions.repo", map[string]any{
		"method": "stats.quality",
		"repo":   repo,
		"device": phonefamily.DeviceOf(req),
	})
	if err != nil {
		if phonefamily.DesktopUnavailable(err) {
			return Facts{}, rpc.Fail("desktop_unavailable")
		}
		return Facts{}, err
	}
	var facts Facts
	if err := json.Unmarshal(raw, &facts); err != nil || facts.Owner == "" || facts.Name == "" {
		return Facts{}, rpc.Fail("desktop_unavailable")
	}
	return facts, nil
}

func (m *Module) begin(key string) bool {
	m.mu.Lock()
	defer m.mu.Unlock()
	if m.inFlight[key] {
		return false
	}
	m.inFlight[key] = true
	m.active++
	return true
}

func (m *Module) end(key string) {
	m.mu.Lock()
	delete(m.inFlight, key)
	m.active--
	m.mu.Unlock()
}

func (m *Module) handleQuality(ctx context.Context, req *rpc.Request) (any, error) {
	var params map[string]any
	if len(req.Params) > 0 {
		_ = json.Unmarshal(req.Params, &params)
	}
	text, _ := params["repo"].(string)
	owner, name, ok := actions.ParseRepoSlug(qtTrim(text))
	if !ok {
		return nil, rpc.Fail("bad_request")
	}
	facts, err := m.Facts(ctx, req, owner+"/"+name)
	if err != nil {
		return nil, err
	}
	key := facts.Owner + "/" + facts.Name
	if !m.begin(key) {
		return nil, rpc.Fail("busy")
	}
	defer m.end(key)
	ctx = context.WithoutCancel(ctx)
	in := Input{Owner: facts.Owner, Name: facts.Name, LocalPath: facts.LocalPath, MirrorPath: facts.MirrorPath}
	if facts.WritableLocalPath != nil {
		in.LocalPath = *facts.WritableLocalPath
	}
	if facts.WritableMirrorPath != nil && *facts.WritableMirrorPath != "" {
		in.MirrorPath = *facts.WritableMirrorPath
	}
	for _, run := range m.store.LoadAllRuns() {
		if run.Owner == facts.Owner && run.Name == facts.Name {
			in.Runs = append(in.Runs, run)
		}
	}
	git := gitRunner{binary: m.Git}
	in.Issues = issueReader{git: git, workTree: in.LocalPath, mirror: in.MirrorPath, identity: m.IdentityValid()}.statuses(ctx)
	at := m.now().UnixMilli()
	result := scanner{git: git}.Scan(ctx, in)
	return ReportFor(result, scanRoot(in), at), nil
}
