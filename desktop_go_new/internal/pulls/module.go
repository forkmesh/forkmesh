package pulls

import (
	"context"
	"encoding/json"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/git"
	"forkmesh.com/desktop-core/internal/identity"
	"forkmesh.com/desktop-core/internal/ipc"
	"forkmesh.com/desktop-core/internal/ownergate"
	"forkmesh.com/desktop-core/internal/rpc"
)

const (
	moduleName     = "pulls"
	rewriteTimeout = 11 * time.Minute
	ChangedEvent   = "core.pulls.changed"
	maxEnvEntries  = 1024
	maxEnvBytes    = 1 << 20
)

type Module struct {
	CallUI       func(ctx context.Context, method string, params any) ([]byte, error)
	Runner       *git.Runner
	LoadIdentity func() (*identity.Identity, error)

	core     *daemon.Core
	ctx      context.Context
	inflight atomic.Int64

	mu       sync.Mutex
	sessions map[string]*sessionEntry
}

type sessionEntry struct {
	mu       sync.Mutex
	workTree string
	session  *Session
	owner    string
}

type StoreParams struct {
	WorkTree      string   `json:"workTree"`
	Mirror        string   `json:"mirror"`
	AuthorName    string   `json:"authorName"`
	RepoKey       string   `json:"repoKey"`
	IdentityKey   string   `json:"identityKey"`
	IdentityValid bool     `json:"identityValid"`
	NativeAmLive  bool     `json:"nativeAmLive"`
	Env           []string `json:"env"`
}

func New() *Module { return &Module{sessions: map[string]*sessionEntry{}} }

func (m *Module) Name() string { return moduleName }

func (m *Module) Busy() bool { return m.inflight.Load() > 0 }

func (m *Module) Start(ctx context.Context, core *daemon.Core) error {
	m.core = core
	m.ctx = ctx
	if m.sessions == nil {
		m.sessions = map[string]*sessionEntry{}
	}
	if core.IPC != nil {
		previous := core.IPC.OnUI
		core.IPC.OnUI = func(conn *ipc.Conn, attached bool) {
			if previous != nil {
				previous(conn, attached)
			}
			if !attached && conn != nil {
				m.dropSessions(conn.ID())
			}
		}
	}
	handle := func(method string, handler rpc.Handler) {
		core.Registry.Handle(method, moduleName, ownergate.DesktopOnly(m.counted(handler)))
	}
	handle("core.pulls.load", m.handleLoad)
	handle("core.pulls.loadArchived", m.handleLoadArchived)
	handle("core.pulls.restoreArchived", m.handleRestoreArchived)
	handle("core.pulls.create", m.handleCreate)
	handle("core.pulls.setStatus", m.handleSetStatus)
	handle("core.pulls.recordReward", m.handleRecordReward)
	handle("core.pulls.behind", m.handleBehind)
	handle("core.pulls.update", m.handleUpdate)
	handle("core.pulls.merge", m.handleMerge)
	handle("core.pulls.checkMergeable", m.handleCheckMergeable)
	handle("core.pulls.prefetchForeign", m.handlePrefetch)
	handle("core.pulls.adoptForeign", m.handleAdopt)
	handle("core.pulls.reconcile", m.handleReconcile)
	handle("core.pulls.applyPull", m.handleApplyPull)
	handle("core.pulls.event", m.handleEvent)
	handle("core.pulls.applyEvent", m.handleApplyEvent)
	handle("core.pulls.branchEvent", m.handleBranchEvent)
	handle("core.pulls.applyBranchEvent", m.handleApplyBranchEvent)
	handle("core.pulls.branchEvents", m.handleBranchEvents)
	handle("core.pulls.delete", m.handleDelete)
	handle("core.pulls.deleteMany", m.handleDeleteMany)
	handle("core.pulls.ensureBranch", m.handleEnsureBranch)
	handle("core.pulls.ensureHeadRef", m.handleEnsureHeadRef)
	handle("core.pulls.deleteFile", m.handleDeleteFile)
	handle("core.pulls.reviewCheckout", m.handleReviewCheckout)
	handle("core.pulls.discardReview", m.handleDiscardReview)
	handle("core.pulls.session.start", m.handleSessionStart)
	handle("core.pulls.session.finish", m.handleSessionFinish)
	handle("core.pulls.session.abort", m.handleSessionAbort)
	handle("core.pulls.session.state", m.handleSessionState)
	return nil
}

func (m *Module) counted(handler rpc.Handler) rpc.Handler {
	return func(ctx context.Context, req *rpc.Request) (any, error) {
		m.inflight.Add(1)
		defer m.inflight.Add(-1)
		return handler(ctx, req)
	}
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

func (m *Module) uiOwner() string {
	if m.core == nil || m.core.IPC == nil {
		return ""
	}
	if ui := m.core.UI(); ui != nil {
		return ui.ID()
	}
	return ""
}

func (m *Module) dropSessions(owner string) {
	m.mu.Lock()
	dropped := []*sessionEntry{}
	for key, entry := range m.sessions {
		if entry.owner == owner {
			dropped = append(dropped, entry)
			delete(m.sessions, key)
		}
	}
	m.mu.Unlock()
	for _, entry := range dropped {
		ClearSessionLive(entry.workTree)
	}
}

func (m *Module) identity() (*identity.Identity, error) {
	if m.LoadIdentity != nil {
		return m.LoadIdentity()
	}
	if m.core == nil {
		return nil, rpc.Fail("identity_unavailable")
	}
	return m.core.RequireIdentity()
}

func (m *Module) freshIdentity() (*identity.Identity, error) {
	if m.LoadIdentity != nil {
		return m.LoadIdentity()
	}
	if m.core == nil {
		return nil, rpc.Fail("identity_unavailable")
	}
	return identity.Load(m.core.Layout.IdentityKey())
}

func (m *Module) signer(p StoreParams) (*Signer, error) {
	if !p.IdentityValid {
		return &Signer{AuthorName: p.AuthorName, RepoKey: p.RepoKey}, nil
	}
	id, err := m.identity()
	if err != nil {
		return nil, rpc.Fail("identity_unavailable")
	}
	if p.IdentityKey == "" || id.PublicKey() != p.IdentityKey {
		fresh, ferr := m.freshIdentity()
		if ferr != nil || p.IdentityKey == "" || fresh.PublicKey() != p.IdentityKey {
			return nil, rpc.Fail("identity_mismatch")
		}
		id = fresh
	}
	return &Signer{Identity: id, AuthorName: p.AuthorName, RepoKey: p.RepoKey}, nil
}

func validEnv(env []string) bool {
	if len(env) > maxEnvEntries {
		return false
	}
	total := 0
	for _, entry := range env {
		total += len(entry)
		eq := strings.IndexByte(entry, '=')
		if eq <= 0 || strings.IndexByte(entry, 0) >= 0 || total > maxEnvBytes {
			return false
		}
	}
	return true
}

func (m *Module) dataRoot() string {
	if m.core == nil {
		return ""
	}
	return m.core.Layout.Data
}

func (m *Module) warn(text string) {
	if m.core != nil && m.core.Logger != nil {
		m.core.Logger.Print(text)
	}
}

func (m *Module) newStore(ctx context.Context, p StoreParams, signer *Signer) (*Store, error) {
	if !validEnv(p.Env) {
		return nil, rpc.Fail("bad_params")
	}
	store := &Store{
		WorkTree:      p.WorkTree,
		Mirror:        p.Mirror,
		IdentityValid: p.IdentityValid,
		DataRoot:      m.dataRoot(),
		Runner:        m.Runner,
		Ctx:           ctx,
		Warn:          m.warn,
		Signer:        signer,
		Env:           p.Env,
	}
	if p.NativeAmLive {
		store.AmLive = func() bool { return true }
	}
	workTree, mirror := p.WorkTree, p.Mirror
	store.Rewrite = func(number int, relPaths []string) error {
		callCtx, cancel := context.WithTimeout(m.baseCtx(), rewriteTimeout)
		defer cancel()
		_, err := m.callUI(callCtx, "ui.pulls.rewrite", map[string]any{
			"workTree": workTree, "mirror": mirror, "number": number, "paths": relPaths,
		})
		return err
	}
	return store, nil
}

func (m *Module) baseCtx() context.Context {
	if m.ctx != nil {
		return m.ctx
	}
	return context.Background()
}

type envelope struct {
	Store StoreParams `json:"store"`
}

func bindStore(req *rpc.Request, target any) (StoreParams, error) {
	var env envelope
	if err := req.Bind(&env); err != nil {
		return StoreParams{}, err
	}
	if target != nil {
		if err := req.Bind(target); err != nil {
			return StoreParams{}, err
		}
	}
	return env.Store, nil
}

func (m *Module) plainStore(ctx context.Context, req *rpc.Request, target any) (*Store, error) {
	p, err := bindStore(req, target)
	if err != nil {
		return nil, err
	}
	return m.newStore(ctx, p, &Signer{AuthorName: p.AuthorName, RepoKey: p.RepoKey})
}

func (m *Module) signingStore(ctx context.Context, req *rpc.Request, target any) (*Store, error) {
	p, err := bindStore(req, target)
	if err != nil {
		return nil, err
	}
	signer, err := m.signer(p)
	if err != nil {
		return nil, err
	}
	return m.newStore(ctx, p, signer)
}

func outcome(err error, extra map[string]any) map[string]any {
	out := map[string]any{"ok": err == nil, "error": ""}
	if err != nil {
		out["error"] = err.Error()
	}
	for key, value := range extra {
		out[key] = value
	}
	return out
}

func (m *Module) changed(store *Store, op string, numbers ...int) {
	if m.core == nil {
		return
	}
	list := []int{}
	for _, n := range numbers {
		if n > 0 {
			list = append(list, n)
		}
	}
	m.core.BroadcastUI(ChangedEvent, map[string]any{"workTree": store.WorkTree, "op": op, "numbers": list})
}

func pullList(list []*Pull) []any {
	out := make([]any, 0, len(list))
	for _, pr := range list {
		out = append(out, pr.FullJSON())
	}
	return out
}

func intList(list []int) []int {
	if list == nil {
		return []int{}
	}
	return list
}

func eventList(list []Event) []any {
	out := make([]any, 0, len(list))
	for i := range list {
		out = append(out, list[i].ToJSON())
	}
	return out
}

type numberParams struct {
	Number int `json:"number"`
}

func (m *Module) handleLoad(ctx context.Context, req *rpc.Request) (any, error) {
	var params struct {
		Stamp string `json:"stamp"`
	}
	store, err := m.plainStore(ctx, req, &params)
	if err != nil {
		return nil, err
	}
	if params.Stamp != "" && store.CurrentLoadStamp() == params.Stamp {
		return map[string]any{"ok": true, "error": "", "unchanged": true, "stamp": params.Stamp}, nil
	}
	list, stamp := store.LoadAllStamped(true)
	return map[string]any{"ok": true, "error": "", "pulls": pullList(list), "stamp": stamp}, nil
}

func (m *Module) handleLoadArchived(ctx context.Context, req *rpc.Request) (any, error) {
	store, err := m.plainStore(ctx, req, nil)
	if err != nil {
		return nil, err
	}
	return map[string]any{"ok": true, "error": "", "pulls": pullList(store.LoadAllIncludingArchived(true))}, nil
}

func (m *Module) handleRestoreArchived(ctx context.Context, req *rpc.Request) (any, error) {
	var params numberParams
	store, err := m.plainStore(ctx, req, &params)
	if err != nil {
		return nil, err
	}
	err = store.RestoreArchivedPull(params.Number)
	if err == nil {
		m.changed(store, "restore", params.Number)
	}
	return outcome(err, nil), nil
}

func (m *Module) handleCreate(ctx context.Context, req *rpc.Request) (any, error) {
	var params struct {
		Title        string `json:"title"`
		Description  string `json:"description"`
		Base         string `json:"base"`
		Head         string `json:"head"`
		Patch        string `json:"patch"`
		Commits      string `json:"commits"`
		BranchBacked bool   `json:"branchBacked"`
	}
	store, err := m.signingStore(ctx, req, &params)
	if err != nil {
		return nil, err
	}
	number, err := store.CreatePull(CreateRequest{Title: params.Title, Description: params.Description, Base: params.Base,
		Head: params.Head, Patch: params.Patch, Commits: params.Commits, BranchBacked: params.BranchBacked})
	if err == nil {
		m.changed(store, "create", number)
	}
	return outcome(err, map[string]any{"number": number}), nil
}

func (m *Module) handleSetStatus(ctx context.Context, req *rpc.Request) (any, error) {
	var params struct {
		Number int    `json:"number"`
		Status string `json:"status"`
	}
	store, err := m.plainStore(ctx, req, &params)
	if err != nil {
		return nil, err
	}
	err = store.SetStatus(params.Number, params.Status)
	m.changed(store, "status", params.Number)
	return outcome(err, nil), nil
}

func (m *Module) handleRecordReward(ctx context.Context, req *rpc.Request) (any, error) {
	var params struct {
		Number    int    `json:"number"`
		RewardID  string `json:"rewardId"`
		Signature string `json:"signature"`
	}
	store, err := m.plainStore(ctx, req, &params)
	if err != nil {
		return nil, err
	}
	err = store.RecordRewardPayment(params.Number, params.RewardID, params.Signature)
	if err == nil {
		m.changed(store, "reward", params.Number)
	}
	return outcome(err, nil), nil
}

func (m *Module) handleBehind(ctx context.Context, req *rpc.Request) (any, error) {
	var params numberParams
	store, err := m.plainStore(ctx, req, &params)
	if err != nil {
		return nil, err
	}
	behind, count, err := store.IsBranchBehindBase(params.Number)
	return outcome(err, map[string]any{"behind": behind, "count": count}), nil
}

func (m *Module) handleUpdate(ctx context.Context, req *rpc.Request) (any, error) {
	var params numberParams
	store, err := m.plainStore(ctx, req, &params)
	if err != nil {
		return nil, err
	}
	err = store.UpdateBranchFromBase(params.Number)
	if err == nil {
		m.changed(store, "update", params.Number)
	}
	return outcome(err, nil), nil
}

func (m *Module) handleMerge(ctx context.Context, req *rpc.Request) (any, error) {
	var params struct {
		Number              int  `json:"number"`
		RequirePeerReview   bool `json:"requirePeerReview"`
		OwnerReviewOverride bool `json:"ownerReviewOverride"`
	}
	store, err := m.plainStore(ctx, req, &params)
	if err != nil {
		return nil, err
	}
	err = store.MergePullWith(params.Number, params.RequirePeerReview, params.OwnerReviewOverride)
	if err == nil {
		m.changed(store, "merge", params.Number)
	}
	return outcome(err, nil), nil
}

func (m *Module) handleCheckMergeable(ctx context.Context, req *rpc.Request) (any, error) {
	var params numberParams
	store, err := m.plainStore(ctx, req, &params)
	if err != nil {
		return nil, err
	}
	clean, conflicts, err := store.CheckMergeable(params.Number)
	return outcome(err, map[string]any{"clean": clean, "conflicts": conflicts}), nil
}

type remoteParams struct {
	RemoteURL  string `json:"remoteUrl"`
	FetchFirst bool   `json:"fetchFirst"`
}

func (m *Module) handlePrefetch(ctx context.Context, req *rpc.Request) (any, error) {
	var params remoteParams
	store, err := m.plainStore(ctx, req, &params)
	if err != nil {
		return nil, err
	}
	ok, err := store.PrefetchForeignLedger(params.RemoteURL)
	out := outcome(err, nil)
	out["ok"] = ok
	return out, nil
}

func (m *Module) handleAdopt(ctx context.Context, req *rpc.Request) (any, error) {
	var params remoteParams
	store, err := m.plainStore(ctx, req, &params)
	if err != nil {
		return nil, err
	}
	adopted, err := store.AdoptForeignPulls(params.RemoteURL, params.FetchFirst)
	if len(adopted) > 0 {
		m.changed(store, "adopt", adopted...)
	}
	out := outcome(err, map[string]any{"adopted": intList(adopted)})
	out["ok"] = true
	return out, nil
}

func (m *Module) handleReconcile(ctx context.Context, req *rpc.Request) (any, error) {
	store, err := m.plainStore(ctx, req, nil)
	if err != nil {
		return nil, err
	}
	result := store.ReconcileMergedPulls()
	if len(result.Flipped)+len(result.Rewards) > 0 {
		m.changed(store, "reconcile", append(append([]int{}, result.Flipped...), result.Rewards...)...)
	}
	return map[string]any{"ok": true, "error": result.Error, "flipped": intList(result.Flipped), "rewards": intList(result.Rewards)}, nil
}

type pullParams struct {
	Pull map[string]any `json:"pull"`
}

func (m *Module) handleApplyPull(ctx context.Context, req *rpc.Request) (any, error) {
	var params pullParams
	store, err := m.plainStore(ctx, req, &params)
	if err != nil {
		return nil, err
	}
	err = store.ApplyRemotePull(PullFromFullJSON(params.Pull))
	if err == nil {
		m.changed(store, "intake")
	}
	return outcome(err, nil), nil
}

type eventInputParams struct {
	Number          int    `json:"number"`
	Branch          string `json:"branch"`
	Type            string `json:"type"`
	Body            string `json:"body"`
	State           string `json:"state"`
	Path            string `json:"path"`
	Side            string `json:"side"`
	Line            int    `json:"line"`
	LineStart       int    `json:"lineStart"`
	LineEnd         int    `json:"lineEnd"`
	SuggestionPatch string `json:"suggestionPatch"`
	ThreadID        string `json:"threadId"`
	ParentID        string `json:"parentId"`
	AppliedCommit   string `json:"appliedCommit"`
	Patchset        string `json:"patchset"`
	AnchorLines     string `json:"anchorLines"`
}

func (m *Module) handleEvent(ctx context.Context, req *rpc.Request) (any, error) {
	var params eventInputParams
	store, err := m.signingStore(ctx, req, &params)
	if err != nil {
		return nil, err
	}
	ev, err := store.AddEvent(params.Number, EventInput{Type: params.Type, Body: params.Body, State: params.State,
		Path: params.Path, Side: params.Side, Line: params.Line, LineStart: params.LineStart, LineEnd: params.LineEnd,
		SuggestionPatch: params.SuggestionPatch, ThreadID: params.ThreadID, ParentID: params.ParentID,
		AppliedCommit: params.AppliedCommit, Patchset: params.Patchset, AnchorLines: params.AnchorLines})
	if err == nil {
		m.changed(store, "event", params.Number)
	}
	extra := map[string]any{}
	if ev.Type != "" {
		extra["event"] = ev.ToJSON()
	}
	return outcome(err, extra), nil
}

type eventParams struct {
	Number int            `json:"number"`
	Branch string         `json:"branch"`
	Event  map[string]any `json:"event"`
}

func (m *Module) handleApplyEvent(ctx context.Context, req *rpc.Request) (any, error) {
	var params eventParams
	store, err := m.plainStore(ctx, req, &params)
	if err != nil {
		return nil, err
	}
	err = store.ApplyRemoteEvent(params.Number, EventFromJSON(params.Event))
	if err == nil {
		m.changed(store, "intake", params.Number)
	}
	return outcome(err, nil), nil
}

func (m *Module) handleBranchEvent(ctx context.Context, req *rpc.Request) (any, error) {
	var params eventInputParams
	store, err := m.signingStore(ctx, req, &params)
	if err != nil {
		return nil, err
	}
	ev, err := store.AddBranchEvent(params.Branch, BranchEventInput{Type: params.Type, Path: params.Path, Side: params.Side,
		LineStart: params.LineStart, LineEnd: params.LineEnd, Body: params.Body, ThreadID: params.ThreadID,
		ParentID: params.ParentID, State: params.State, Patchset: params.Patchset, AnchorLines: params.AnchorLines})
	if err == nil {
		m.changed(store, "branchEvent")
	}
	extra := map[string]any{}
	if ev.Type != "" {
		extra["event"] = ev.ToJSON()
	}
	return outcome(err, extra), nil
}

func (m *Module) handleApplyBranchEvent(ctx context.Context, req *rpc.Request) (any, error) {
	var params eventParams
	store, err := m.plainStore(ctx, req, &params)
	if err != nil {
		return nil, err
	}
	err = store.ApplyRemoteBranchEvent(params.Branch, EventFromJSON(params.Event))
	if err == nil {
		m.changed(store, "branchEvent")
	}
	return outcome(err, nil), nil
}

func (m *Module) handleBranchEvents(ctx context.Context, req *rpc.Request) (any, error) {
	var params struct {
		Branch string `json:"branch"`
	}
	store, err := m.plainStore(ctx, req, &params)
	if err != nil {
		return nil, err
	}
	return map[string]any{"ok": true, "error": "", "events": eventList(store.ReadBranchReviewEvents(params.Branch))}, nil
}

func (m *Module) handleDelete(ctx context.Context, req *rpc.Request) (any, error) {
	var params struct {
		Number         int  `json:"number"`
		RewriteHistory bool `json:"rewriteHistory"`
	}
	store, err := m.plainStore(ctx, req, &params)
	if err != nil {
		return nil, err
	}
	err = store.DeletePull(params.Number, params.RewriteHistory)
	m.changed(store, "delete", params.Number)
	return outcome(err, nil), nil
}

func (m *Module) handleDeleteMany(ctx context.Context, req *rpc.Request) (any, error) {
	var params struct {
		Numbers        []int `json:"numbers"`
		RewriteHistory bool  `json:"rewriteHistory"`
	}
	store, err := m.plainStore(ctx, req, &params)
	if err != nil {
		return nil, err
	}
	deleted, missing, err := store.DeletePulls(params.Numbers, params.RewriteHistory)
	m.changed(store, "delete", append(append([]int{}, deleted...), missing...)...)
	return outcome(err, map[string]any{"deleted": intList(deleted), "missing": intList(missing)}), nil
}

func (m *Module) handleEnsureBranch(ctx context.Context, req *rpc.Request) (any, error) {
	var params struct {
		Branch string `json:"branch"`
	}
	store, err := m.plainStore(ctx, req, &params)
	if err != nil {
		return nil, err
	}
	source, number, err := store.EnsureLocalBranch(params.Branch)
	return outcome(err, map[string]any{"source": source, "pullNumber": number}), nil
}

func (m *Module) handleEnsureHeadRef(ctx context.Context, req *rpc.Request) (any, error) {
	var params pullParams
	store, err := m.plainStore(ctx, req, &params)
	if err != nil {
		return nil, err
	}
	pr := PullFromFullJSON(params.Pull)
	return outcome(store.MaterializePullRef(&pr), nil), nil
}

func (m *Module) handleDeleteFile(ctx context.Context, req *rpc.Request) (any, error) {
	var params struct {
		Number int    `json:"number"`
		Path   string `json:"path"`
	}
	store, err := m.plainStore(ctx, req, &params)
	if err != nil {
		return nil, err
	}
	err = store.DeletePullFile(params.Number, params.Path)
	if err == nil {
		m.changed(store, "deleteFile", params.Number)
	}
	return outcome(err, nil), nil
}

func (m *Module) handleReviewCheckout(ctx context.Context, req *rpc.Request) (any, error) {
	var params struct {
		Pull map[string]any `json:"pull"`
		Dir  string         `json:"dir"`
	}
	store, err := m.plainStore(ctx, req, &params)
	if err != nil {
		return nil, err
	}
	pr := PullFromFullJSON(params.Pull)
	checkout, err := store.CheckoutForReview(&pr, params.Dir)
	raw, _ := json.Marshal(checkout)
	var encoded map[string]any
	_ = json.Unmarshal(raw, &encoded)
	return outcome(err, map[string]any{"checkout": encoded}), nil
}

func (m *Module) handleDiscardReview(ctx context.Context, req *rpc.Request) (any, error) {
	var params struct {
		Dir string `json:"dir"`
	}
	store, err := m.plainStore(ctx, req, &params)
	if err != nil {
		return nil, err
	}
	store.DiscardReviewCheckout(params.Dir)
	return outcome(nil, nil), nil
}

func (m *Module) sessionFor(workTree string, create bool) *sessionEntry {
	key := WorkTreeKey(workTree)
	m.mu.Lock()
	defer m.mu.Unlock()
	entry := m.sessions[key]
	if entry == nil && create {
		entry = &sessionEntry{workTree: workTree, session: &Session{}, owner: m.uiOwner()}
		m.sessions[key] = entry
	}
	return entry
}

func (m *Module) settleSession(entry *sessionEntry) {
	if entry == nil || !entry.session.Empty() {
		return
	}
	m.mu.Lock()
	defer m.mu.Unlock()
	key := WorkTreeKey(entry.workTree)
	if m.sessions[key] == entry {
		delete(m.sessions, key)
	}
}

type sessionParams struct {
	Number  int    `json:"number"`
	Mode    string `json:"mode"`
	Path    string `json:"path"`
	Content string `json:"content"`
	Message string `json:"message"`
}

func (m *Module) withSession(ctx context.Context, req *rpc.Request, params *sessionParams, create bool,
	run func(store *Store) map[string]any) (any, error) {
	store, err := m.plainStore(ctx, req, params)
	if err != nil {
		return nil, err
	}
	entry := m.sessionFor(store.WorkTree, create)
	if entry == nil {
		store.Session = &Session{}
		return run(store), nil
	}
	entry.mu.Lock()
	defer entry.mu.Unlock()
	store.Session = entry.session
	result := run(store)
	m.settleSession(entry)
	return result, nil
}

func (m *Module) handleSessionStart(ctx context.Context, req *rpc.Request) (any, error) {
	var params sessionParams
	return m.withSession(ctx, req, &params, true, func(store *Store) map[string]any {
		var (
			conflicted    = []string{}
			resolvedClean bool
			content       string
			err           error
		)
		switch params.Mode {
		case "conflict":
			conflicted, resolvedClean, err = store.StartConflictMerge(params.Number)
		case "agentConflict":
			conflicted, resolvedClean, err = store.StartConflictAgentEdit(params.Number)
		case "file":
			content, err = store.StartPullFileEdit(params.Number, params.Path)
		case "agentEdit":
			err = store.StartPullAgentEdit(params.Number)
		default:
			err = errorString("bad_request")
		}
		if conflicted == nil {
			conflicted = []string{}
		}
		if resolvedClean && err == nil {
			m.changed(store, "session", params.Number)
		}
		return outcome(err, map[string]any{"conflicted": conflicted, "resolvedClean": resolvedClean, "content": content,
			"editWorkTree": store.AgentEditWorkTree()})
	})
}

func (m *Module) handleSessionFinish(ctx context.Context, req *rpc.Request) (any, error) {
	var params sessionParams
	return m.withSession(ctx, req, &params, false, func(store *Store) map[string]any {
		var err error
		switch params.Mode {
		case "conflict":
			err = store.FinishConflictMerge(params.Number)
		case "file":
			err = store.FinishPullFileEdit(params.Number, params.Path, params.Content)
		case "agentEdit":
			err = store.FinishPullAgentEdit(params.Number, params.Message)
		default:
			err = errorString("bad_request")
		}
		if err == nil {
			m.changed(store, "session", params.Number)
		}
		return outcome(err, nil)
	})
}

func (m *Module) handleSessionAbort(ctx context.Context, req *rpc.Request) (any, error) {
	var params sessionParams
	return m.withSession(ctx, req, &params, false, func(store *Store) map[string]any {
		store.AbortConflictMerge()
		return outcome(nil, nil)
	})
}

func (m *Module) handleSessionState(ctx context.Context, req *rpc.Request) (any, error) {
	var params sessionParams
	return m.withSession(ctx, req, &params, false, func(store *Store) map[string]any {
		return outcome(nil, map[string]any{"inProgress": store.InProgress(), "editWorkTree": store.AgentEditWorkTree(),
			"active": !store.Session.Empty()})
	})
}
