package actions

import (
	"context"
	"fmt"
	"path/filepath"
	"strings"
	"time"

	"forkmesh.com/desktop-core/internal/git"
)

const (
	autoFixSetting        = "agents/autoFixFailures"
	autoFixLogChars       = 12000
	autoFixCallTimeout    = 30 * time.Second
	maxPendingAutoFix     = 32
	servedFetchTimeout    = 2 * time.Minute
	servedLookupTimeout   = 10 * time.Second
	servedErrorChars      = 300
	autoFixTruncatedLabel = "...(log truncated; showing the tail)...\n"
)

type finishReactor interface {
	reactToFinish(r *runner, landed bool) map[string]any
}

type AutoFixRequest struct {
	Run      int    `json:"run"`
	Owner    string `json:"owner"`
	Name     string `json:"name"`
	Branch   string `json:"branch"`
	Workflow string `json:"workflow"`
	Prompt   string `json:"prompt"`
}

func (m *Module) reactToFinish(r *runner, landed bool) map[string]any {
	extra := map[string]any{}
	if landed {
		if served, attempted := m.landServedMirror(r.run); attempted {
			extra["served"] = served
		}
	}
	if m.autoFixByCore(r.run, r.stopping) {
		extra["autoFix"] = true
	}
	return extra
}

func (m *Module) queuedRun(id int) bool {
	m.mu.Lock()
	defer m.mu.Unlock()
	active, ok := m.runs[id]
	return ok && active.queued
}

func (m *Module) servedRepo(owner, name string) (RepoEntry, bool) {
	q := m.q
	if q == nil {
		return RepoEntry{}, false
	}
	ctx, cancel := context.WithTimeout(context.Background(), servedLookupTimeout)
	defer cancel()
	type found struct {
		repo RepoEntry
		ok   bool
	}
	result, err := await(ctx, q, func(done func(found)) {
		if !q.armed {
			done(found{})
			return
		}
		repo, ok := q.snap.lookup(owner, name)
		done(found{repo, ok})
	})
	if err != nil {
		return RepoEntry{}, false
	}
	return result.repo, result.ok
}

func (m *Module) landServedMirror(run Run) (map[string]any, bool) {
	repo, ok := m.servedRepo(run.Owner, run.Name)
	if !ok {
		return nil, false
	}
	mirror := repo.MirrorPath
	if qtTrim(mirror) == "" || repo.LocalPath == mirror || !dirExists(mirror) {
		return map[string]any{"ok": true, "skipped": true}, true
	}
	ctx, cancel := context.WithTimeout(context.Background(), servedFetchTimeout)
	defer cancel()
	result := m.gitRunner().Run(ctx, mirror, []string{"fetch", repo.LocalPath, "+refs/heads/*:refs/heads/*", "+refs/tags/*:refs/tags/*"},
		git.Options{Timeout: servedFetchTimeout})
	if result.OK {
		m.logf("run #%d: landed the release metadata in the served mirror %s", run.ID, mirror)
		return map[string]any{"ok": true}, true
	}
	reason := qtTrim(result.Stderr)
	if reason == "" {
		reason = qtTrim(result.Error)
	}
	m.logf("run #%d: could not land the release metadata in the served mirror %s: %s", run.ID, mirror, reason)
	return map[string]any{"ok": false, "error": qtLeft(reason, servedErrorChars)}, true
}

func autoFixBranch(ref string) string {
	if strings.HasPrefix(ref, "refs/heads/") {
		return ref[len("refs/heads/"):]
	}
	return ref
}

func (m *Module) autoFixByCore(run Run, stopping bool) bool {
	if stopping || run.Status != StatusFailed || !m.queuedRun(run.ID) {
		return false
	}
	if !m.settings().Bool(autoFixSetting, true) {
		return false
	}
	return autoFixBranch(run.Ref) != ""
}

func AutoFixPrompt(run Run, log, logFile string) string {
	tail := log
	if utf16Len(log) > autoFixLogChars {
		tail = autoFixTruncatedLabel + qtRight(log, autoFixLogChars)
	}
	prompt := fmt.Sprintf("The \"%s\" CI workflow failed for %s/%s (commit %s, ref %s). Find what broke and fix it so the workflow succeeds.\n",
		run.WorkflowName, run.Owner, run.Name, qtLeft(run.Commit, 8), run.Ref)
	if logFile != "" {
		prompt += fmt.Sprintf("\nFull run log: %s\nRead that whole file on this machine; what follows is only its tail.\n", logFile)
	}
	return prompt + "\nLog excerpt:\n\n" + tail
}

func (m *Module) autoFixRequest(run Run) AutoFixRequest {
	logFile := filepath.FromSlash(m.st.LogPath(run))
	return AutoFixRequest{Run: run.ID, Owner: run.Owner, Name: run.Name, Branch: autoFixBranch(run.Ref),
		Workflow: run.WorkflowName, Prompt: AutoFixPrompt(run, m.st.ReadLog(run), logFile)}
}

func (m *Module) sendAutoFix(request AutoFixRequest) error {
	ctx, cancel := context.WithTimeout(context.Background(), autoFixCallTimeout)
	defer cancel()
	if m.AutoFix != nil {
		return m.AutoFix(ctx, request)
	}
	if m.core == nil {
		return errDesktopUnavailable
	}
	_, err := m.core.CallUI(ctx, "ui.actions.autoFix", request)
	return err
}

func (m *Module) deliverAutoFix(request AutoFixRequest) {
	err := m.sendAutoFix(request)
	if err == nil {
		m.logf("run #%d: sent the failure to the agent on %s/%s %s", request.Run, request.Owner, request.Name, request.Branch)
		return
	}
	if !uiUnavailable(err) {
		m.logf("run #%d: the desktop did not take the agent auto-fix: %v", request.Run, err)
		return
	}
	m.autoFixMu.Lock()
	defer m.autoFixMu.Unlock()
	for _, pending := range m.autoFixQueue {
		if pending.Run == request.Run {
			return
		}
	}
	m.autoFixQueue = append(m.autoFixQueue, request)
	if len(m.autoFixQueue) > maxPendingAutoFix {
		m.autoFixQueue = append([]AutoFixRequest(nil), m.autoFixQueue[len(m.autoFixQueue)-maxPendingAutoFix:]...)
	}
	m.logf("run #%d: no desktop to hand the agent auto-fix to; it waits for the next attach", request.Run)
}

func (m *Module) flushAutoFix() {
	m.autoFixMu.Lock()
	pending := m.autoFixQueue
	m.autoFixQueue = nil
	m.autoFixMu.Unlock()
	for _, request := range pending {
		m.deliverAutoFix(request)
	}
}

func (m *Module) pendingAutoFix() []AutoFixRequest {
	m.autoFixMu.Lock()
	defer m.autoFixMu.Unlock()
	return append([]AutoFixRequest(nil), m.autoFixQueue...)
}
