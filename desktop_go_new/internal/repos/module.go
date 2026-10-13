package repos

import (
	"context"
	"encoding/json"
	"errors"
	"path/filepath"
	"strconv"
	"strings"
	"sync/atomic"

	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/git"
	"forkmesh.com/desktop-core/internal/rpc"
)

const (
	DefaultLogLimit    = 50
	MaxLogLimit        = 1000
	MaxDesktopBranches = 5000
	maxErrorChars      = 500
)

type Location struct {
	Dir      string `json:"dir"`
	Checkout bool   `json:"checkout"`
}

type Locator func(ctx context.Context, peer rpc.Peer, method, repo string) (Location, error)

type Module struct {
	Git      *git.Runner
	Locate   Locator
	uiLocate bool
	core     *daemon.Core
	inflight atomic.Int64
}

func New() *Module { return &Module{} }

func NewWithUILocate() *Module { return &Module{uiLocate: true} }

func (m *Module) Name() string { return "repos" }

func (m *Module) Busy() bool { return m.inflight.Load() > 0 }

func (m *Module) Start(ctx context.Context, core *daemon.Core) error {
	m.core = core
	if m.Git == nil {
		m.Git = &git.Runner{Logger: core.Logger}
	}
	core.Set(git.SharedRunnerKey, m.Git)
	core.Registry.Handle("core.git.status", m.Name(), m.track(m.gitStatus))
	core.Registry.Handle("core.git.branches", m.Name(), m.track(m.gitBranches))
	core.Registry.Handle("core.git.log", m.Name(), m.track(m.gitLog))
	m.startDesktopPages(core)
	if m.Locate == nil && m.uiLocate {
		m.Locate = m.locateWithUI
	}
	if m.Locate != nil {
		core.Registry.Handle("repos.commits", m.Name(), m.track(m.phoneCommits))
		core.Registry.Handle("repos.branches", m.Name(), m.track(m.phoneBranches))
	}
	return nil
}

func (m *Module) track(handler rpc.Handler) rpc.Handler {
	return func(ctx context.Context, req *rpc.Request) (any, error) {
		m.inflight.Add(1)
		defer m.inflight.Add(-1)
		return handler(ctx, req)
	}
}

func rawParams(req *rpc.Request) map[string]json.RawMessage {
	out := map[string]json.RawMessage{}
	if len(req.Params) > 0 {
		_ = json.Unmarshal(req.Params, &out)
	}
	return out
}

func stringParam(params map[string]json.RawMessage, key string) string {
	var text string
	if raw, ok := params[key]; ok && json.Unmarshal(raw, &text) == nil {
		return text
	}
	return ""
}

func (m *Module) locateWithUI(ctx context.Context, peer rpc.Peer, method, repo string) (Location, error) {
	device := rpc.Device{}
	if peer != nil {
		device = peer.Device()
	}
	raw, err := m.core.CallUI(ctx, "ui.repos.locate", map[string]any{"method": method, "repo": repo, "device": device})
	if err != nil {
		code := rpc.Code(err)
		if code == "ui_gone" || code == "ui_timeout" || code == "desktop_unavailable" ||
			errors.Is(err, context.Canceled) || errors.Is(err, context.DeadlineExceeded) {
			return Location{}, rpc.Fail("desktop_unavailable")
		}
		return Location{}, err
	}
	var location Location
	if err := json.Unmarshal(raw, &location); err != nil {
		return Location{}, rpc.Fail("desktop_unavailable")
	}
	if strings.TrimSpace(location.Dir) == "" {
		return Location{}, rpc.Fail("no_checkout")
	}
	return location, nil
}

func (m *Module) capture(ctx context.Context, dir string, args []string) []byte {
	result := m.Git.Run(ctx, dir, args, git.Options{})
	if result.TimedOut {
		return nil
	}
	return result.Output
}

func (m *Module) phoneCommits(ctx context.Context, req *rpc.Request) (any, error) {
	params := rawParams(req)
	repo := stringParam(params, "repo")
	location, err := m.Locate(ctx, req.Peer, req.Method, repo)
	if err != nil {
		return nil, err
	}
	owner, name, ok := parseRepoSlug(repo)
	if !ok {
		return nil, rpc.Fail("bad_request")
	}
	ref := qtTrim(stringParam(params, "ref"))
	if ref != "" && !validBranchName(ref) {
		return nil, rpc.Fail("bad_request")
	}
	limit := clampLimit(params["limit"], DefaultCommits, MaxCommits)
	output := m.capture(ctx, location.Dir, commitsArgs(limit, ref))
	return map[string]any{
		"repo":    owner + "/" + name,
		"ref":     ref,
		"commits": parseCommits(output, limit),
	}, nil
}

func (m *Module) phoneBranches(ctx context.Context, req *rpc.Request) (any, error) {
	params := rawParams(req)
	repo := stringParam(params, "repo")
	location, err := m.Locate(ctx, req.Peer, req.Method, repo)
	if err != nil {
		return nil, err
	}
	owner, name, ok := parseRepoSlug(repo)
	if !ok {
		return nil, rpc.Fail("bad_request")
	}
	output := m.capture(ctx, location.Dir, branchesArgs(MaxBranches))
	return map[string]any{
		"repo":     owner + "/" + name,
		"checkout": location.Checkout,
		"branches": parseBranches(output, MaxBranches),
	}, nil
}

func local(req *rpc.Request) error {
	if req.Peer == nil || rpc.Remote(req.Peer) {
		return rpc.Fail("forbidden")
	}
	return nil
}

func checkoutPath(params map[string]json.RawMessage) (string, error) {
	path := strings.TrimSpace(stringParam(params, "path"))
	if path == "" || !filepath.IsAbs(path) {
		return "", rpc.Fail("bad_request")
	}
	path = filepath.Clean(path)
	if git.GitDirForCheckout(path) == "" {
		return "", rpc.Fail("not_a_repository")
	}
	return path, nil
}

func gitFailure(result git.Result) error {
	if result.TimedOut {
		return rpc.Fail("git_timeout")
	}
	text := strings.TrimSpace(result.Error)
	if text == "" {
		return rpc.Fail("git_failed")
	}
	if len(text) > maxErrorChars {
		text = text[:maxErrorChars]
	}
	return rpc.Fail(text)
}

func validRef(ref string) bool {
	if ref == "" || len(ref) > 256 || strings.HasPrefix(ref, "-") {
		return false
	}
	for _, r := range ref {
		if r <= ' ' || r == 0x7f {
			return false
		}
	}
	return true
}

type statusReply struct {
	Path string `json:"path"`
	git.Status
}

func (m *Module) gitStatus(ctx context.Context, req *rpc.Request) (any, error) {
	if err := local(req); err != nil {
		return nil, err
	}
	path, err := checkoutPath(rawParams(req))
	if err != nil {
		return nil, err
	}
	status, result := m.Git.Status(ctx, path)
	if !result.OK {
		return nil, gitFailure(result)
	}
	return statusReply{Path: path, Status: status}, nil
}

func (m *Module) gitBranches(ctx context.Context, req *rpc.Request) (any, error) {
	if err := local(req); err != nil {
		return nil, err
	}
	params := rawParams(req)
	path, err := checkoutPath(params)
	if err != nil {
		return nil, err
	}
	limit := clampLimit(params["limit"], MaxBranches, MaxDesktopBranches)
	result := m.Git.Run(ctx, path, branchesArgs(limit), git.Options{})
	if !result.OK {
		return nil, gitFailure(result)
	}
	return map[string]any{"path": path, "branches": parseBranches(result.Output, limit)}, nil
}

func (m *Module) gitLog(ctx context.Context, req *rpc.Request) (any, error) {
	if err := local(req); err != nil {
		return nil, err
	}
	params := rawParams(req)
	path, err := checkoutPath(params)
	if err != nil {
		return nil, err
	}
	ref := strings.TrimSpace(stringParam(params, "ref"))
	if ref != "" && !validRef(ref) {
		return nil, rpc.Fail("bad_request")
	}
	limit := clampLimit(params["limit"], DefaultLogLimit, MaxLogLimit)
	args := []string{"log", commitLogFormat, "-n", strconv.Itoa(limit)}
	if ref != "" {
		args = append(args, "--end-of-options", ref)
	}
	args = append(args, "--")
	result := m.Git.Run(ctx, path, args, git.Options{})
	if !result.OK {
		if ref == "" && strings.Contains(result.Stderr, "does not have any commits yet") {
			return map[string]any{"path": path, "ref": ref, "commits": []map[string]any{}}, nil
		}
		return nil, gitFailure(result)
	}
	return map[string]any{"path": path, "ref": ref, "commits": parseCommits(result.Output, limit)}, nil
}
