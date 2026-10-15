package files

import (
	"context"
	"fmt"
	"math"
	"os"
	"strings"
	"sync"
	"time"

	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/git"
	"forkmesh.com/desktop-core/internal/qsettings"
	"forkmesh.com/desktop-core/internal/rpc"
)

const (
	maxRoots           = 200
	maxWorktreesPerRep = 40
	maxEntries         = 500
	maxStatusFiles     = 500
	maxGitOutputBytes  = 4 * 1024 * 1024
	repositoriesArray  = "repositories/items"
	accountNameSetting = "account/nodeName"
)

type Module struct {
	Owners          func() []string
	DesktopAttached func() bool
	Now             func() time.Time
	Git             string
	StatusTimeout   time.Duration
	GitTimeout      time.Duration
	StartDelay      time.Duration
	Lanes           *git.Runner

	core    *daemon.Core
	confirm *confirmations
	mu      sync.Mutex
	running map[string]*gitRun
}

func New() *Module {
	return &Module{Git: "git", StatusTimeout: 15 * time.Second, GitTimeout: 120 * time.Second,
		StartDelay: 50 * time.Millisecond, confirm: newConfirmations(), running: map[string]*gitRun{}}
}

func (m *Module) Name() string { return "files" }

var methods = []string{"roots", "list", "read", "status", "prepare", "run"}

func Methods() []string {
	out := make([]string, 0, len(methods))
	for _, method := range methods {
		out = append(out, familyPrefix+method)
	}
	return out
}

func (m *Module) Start(ctx context.Context, core *daemon.Core) error {
	m.core = core
	handlers := map[string]func(context.Context, *rpc.Request, map[string]any) (any, error){
		"roots":   m.handleRoots,
		"list":    m.handleList,
		"read":    m.handleRead,
		"status":  m.handleStatus,
		"prepare": m.handlePrepare,
		"run":     m.handleRun,
	}
	for _, method := range methods {
		core.Registry.Handle(familyPrefix+method, m.Name(), m.guard(handlers[method]))
	}
	return nil
}

func (m *Module) Busy() bool {
	m.mu.Lock()
	defer m.mu.Unlock()
	return len(m.running) > 0
}

func (m *Module) Stop() {
	m.mu.Lock()
	runs := make([]*gitRun, 0, len(m.running))
	for _, run := range m.running {
		runs = append(runs, run)
	}
	m.mu.Unlock()
	for _, run := range runs {
		run.kill()
	}
}

func (m *Module) now() int64 {
	if m.Now != nil {
		return m.Now().UnixMilli()
	}
	return time.Now().UnixMilli()
}

func (m *Module) settings() *qsettings.Settings {
	return m.core.Settings.Get()
}

func (m *Module) logf(format string, args ...any) {
	if m.core != nil && m.core.Logger != nil {
		m.core.Logger.Printf("files: "+format, args...)
	}
}

func (m *Module) ownerNames() []string {
	if m.Owners != nil {
		return m.Owners()
	}
	names := []string{}
	switch owners := m.core.Get("control.owners").(type) {
	case string:
		names = append(names, owners)
	case []string:
		names = append(names, owners...)
	case []any:
		for _, owner := range owners {
			if text, ok := owner.(string); ok {
				names = append(names, text)
			}
		}
	}
	switch info := m.core.Get("control.info").(type) {
	case map[string]any:
		if user, ok := info["user"].(string); ok {
			names = append(names, user)
		}
	case map[string]string:
		names = append(names, info["user"])
	}
	names = append(names, m.settings().String(accountNameSetting, ""))
	return names
}

func (m *Module) desktopAttached() bool {
	if m.DesktopAttached != nil {
		return m.DesktopAttached()
	}
	return m.core.IPC != nil && m.core.UI() != nil
}

func ownerMayManage(deviceUser, account string) bool {
	device := strings.TrimSpace(deviceUser)
	account = strings.TrimSpace(account)
	return device != "" && account != "" && strings.EqualFold(device, account)
}

func (m *Module) ownerAllowed(device rpc.Device) bool {
	if device.Key == "" {
		return false
	}
	for _, name := range m.ownerNames() {
		if ownerMayManage(device.User, name) {
			return true
		}
	}
	return false
}

func (m *Module) guard(handler func(context.Context, *rpc.Request, map[string]any) (any, error)) rpc.Handler {
	return func(ctx context.Context, req *rpc.Request) (any, error) {
		if req.Peer != nil && rpc.Remote(req.Peer) {
			device := req.Peer.Device()
			if device.Key == "" {
				return nil, rpc.Fail("not_owner")
			}
			if !m.ownerAllowed(device) {
				if m.desktopAttached() {
					return nil, rpc.ErrDelegate
				}
				return nil, rpc.Fail("not_owner")
			}
		}
		return handler(ctx, req, req.Object())
	}
}

func stringParam(params map[string]any, key string) string {
	text, _ := params[key].(string)
	return text
}

func numberParam(params map[string]any, key string, fallback float64) int64 {
	value, ok := params[key].(float64)
	if !ok {
		value = fallback
	}
	switch {
	case math.IsNaN(value):
		return 0
	case value >= math.MaxInt64:
		return math.MaxInt64
	case value <= math.MinInt64:
		return math.MinInt64
	}
	return int64(value)
}

func deviceKey(req *rpc.Request) string {
	if req.Peer == nil {
		return ""
	}
	return req.Peer.Device().Key
}

func deviceLabel(peer rpc.Peer) string {
	if peer == nil {
		return "a phone"
	}
	device := peer.Device()
	label := device.Name
	if label == "" {
		label = "Phone"
	}
	if device.User != "" {
		label += " (" + device.User + ")"
	}
	if peer.Transport() == rpc.Relay {
		label += ", through the relay"
	} else {
		label += ", on the local network"
	}
	return label
}

type repoRecord struct {
	owner     string
	name      string
	localPath string
}

func hex64(value string) bool {
	if len(value) != 64 {
		return false
	}
	for i := 0; i < len(value); i++ {
		c := value[i]
		if !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) {
			return false
		}
	}
	return true
}

func repositories(settings *qsettings.Settings) []repoRecord {
	count := settings.Int(repositoriesArray+"/size", 0)
	out := []repoRecord{}
	for i := int64(1); i <= count; i++ {
		prefix := fmt.Sprintf("%s/%d/", repositoriesArray, i)
		record := repoRecord{
			owner:     settings.String(prefix+"owner", ""),
			name:      settings.String(prefix+"name", ""),
			localPath: settings.String(prefix+"localPath", ""),
		}
		source := strings.TrimSpace(record.localPath)
		if source == "" {
			source = strings.TrimSpace(settings.String(prefix+"cloneUrl", ""))
		}
		if record.name == "" {
			continue
		}
		if source != "" || hex64(strings.TrimSpace(settings.String(prefix+"publicArchiveId", ""))) ||
			hex64(strings.TrimSpace(settings.String(prefix+"privateReplicaId", ""))) {
			out = append(out, record)
		}
	}
	return out
}

func (m *Module) fileRoots() []map[string]any {
	roots := []map[string]any{}
	for _, repo := range repositories(m.settings()) {
		if len(roots) >= maxRoots {
			break
		}
		if repo.owner == "" || repo.name == "" || repo.localPath == "" || !isDir(repo.localPath) {
			continue
		}
		key := repo.owner + "/" + repo.name
		if !validRootID(key) {
			continue
		}
		roots = append(roots, root{id: key, label: key, kind: "repo", branch: headBranch(repo.localPath), repo: key}.row())
		for _, worktree := range discoverWorktrees(key, repo.localPath, maxWorktreesPerRep) {
			if len(roots) >= maxRoots {
				break
			}
			roots = append(roots, worktree.row())
		}
	}
	return roots
}

func (m *Module) fileRootPath(rootID string) (string, string) {
	if !validRootID(rootID) {
		return "", ""
	}
	repoKey := rootID
	if hash := strings.IndexByte(rootID, '#'); hash >= 0 {
		repoKey = rootID[:hash]
	}
	for _, repo := range repositories(m.settings()) {
		key := repo.owner + "/" + repo.name
		if repo.localPath == "" || !strings.EqualFold(key, repoKey) {
			continue
		}
		if !strings.Contains(rootID, "#") {
			return repo.localPath, repoKey
		}
		for _, worktree := range discoverWorktrees(key, repo.localPath, maxWorktreesPerRep) {
			if strings.EqualFold(worktree.id, rootID) {
				return worktree.path, worktree.label
			}
		}
		return "", ""
	}
	return "", ""
}

func (m *Module) handleRoots(ctx context.Context, req *rpc.Request, params map[string]any) (any, error) {
	return map[string]any{"roots": m.fileRoots()}, nil
}

func (m *Module) lookup(params map[string]any) (string, string, string, error) {
	rootID := strings.TrimSpace(stringParam(params, "root"))
	path, label := m.fileRootPath(rootID)
	if path == "" {
		return "", "", "", rpc.Fail("unknown_root")
	}
	return rootID, path, label, nil
}

func (m *Module) handleList(ctx context.Context, req *rpc.Request, params map[string]any) (any, error) {
	rootID, rootPath, _, err := m.lookup(params)
	if err != nil {
		return nil, err
	}
	relative := stringParam(params, "path")
	dir, failure := resolveInside(rootPath, relative)
	if failure != "" {
		return nil, rpc.Fail(failure)
	}
	if !isDir(dir) {
		return nil, rpc.Fail("not_a_directory")
	}
	clean, _ := normalizeRelativePath(relative)
	list := listDirectory(rootPath, dir, maxEntries)
	return map[string]any{"root": rootID, "path": clean, "entries": list.entries, "total": list.total,
		"truncated": list.truncated}, nil
}

func (m *Module) handleRead(ctx context.Context, req *rpc.Request, params map[string]any) (any, error) {
	rootID, rootPath, _, err := m.lookup(params)
	if err != nil {
		return nil, err
	}
	relative := stringParam(params, "path")
	file, failure := resolveInside(rootPath, relative)
	if failure != "" {
		return nil, rpc.Fail(failure)
	}
	info, statErr := os.Stat(file)
	if statErr != nil || !info.Mode().IsRegular() {
		return nil, rpc.Fail("not_a_file")
	}
	if isSensitiveName(info.Name()) {
		return nil, rpc.Fail("sensitive_file")
	}
	offset := numberParam(params, "offset", 0)
	length := numberParam(params, "length", maxReadBytes)
	slice, failure := readFileSlice(file, offset, length)
	if failure != "" {
		return nil, rpc.Fail(failure)
	}
	clean, _ := normalizeRelativePath(relative)
	slice["root"] = rootID
	slice["path"] = clean
	slice["modifiedMs"] = info.ModTime().UnixMilli()
	return slice, nil
}

func (m *Module) handleStatus(ctx context.Context, req *rpc.Request, params map[string]any) (any, error) {
	rootID, rootPath, label, err := m.lookup(params)
	if err != nil {
		return nil, err
	}
	output, failure := m.gitCapture(ctx, rootPath, []string{"status", "--porcelain=v1", "-b", "-z", "--untracked-files=normal"})
	if failure != "" {
		return nil, rpc.Fail(failure)
	}
	result := parsePorcelainStatus(output, maxStatusFiles)
	result["root"] = rootID
	result["label"] = label
	return result, nil
}

func (m *Module) handlePrepare(ctx context.Context, req *rpc.Request, params map[string]any) (any, error) {
	rootID, _, label, err := m.lookup(params)
	if err != nil {
		return nil, err
	}
	action := stringParam(params, "action")
	if !validGitAction(action) {
		return nil, rpc.Fail("unsupported_action")
	}
	token := m.confirm.issue(deviceKey(req), action, rootID, m.now())
	return map[string]any{"token": token, "action": action, "subject": rootID,
		"summary": gitActionSummary(action, label), "expiresInMs": confirmationLifetimeMs}, nil
}

func (m *Module) handleRun(ctx context.Context, req *rpc.Request, params map[string]any) (any, error) {
	rootID, _, _, err := m.lookup(params)
	if err != nil {
		return nil, err
	}
	action := stringParam(params, "action")
	result := m.confirm.consume(stringParam(params, "token"), deviceKey(req), action, rootID, m.now())
	if result != verdictOK {
		m.logf("Refused a git %s of %s from %s: %s.", action, rootID, deviceLabel(req.Peer), confirmationError(result))
		return nil, rpc.Fail(confirmationError(result))
	}
	return m.runGit(req.Peer, action, rootID)
}
