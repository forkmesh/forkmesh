package agentmodel

import (
	"context"
	"crypto/rand"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"strconv"
	"strings"
	"time"

	"forkmesh.com/desktop-core/internal/agentrun"
	"forkmesh.com/desktop-core/internal/agents"
	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/ipc"
	"forkmesh.com/desktop-core/internal/phonefamily"
	"forkmesh.com/desktop-core/internal/qtjson"
	"forkmesh.com/desktop-core/internal/rpc"
)

const (
	confirmedMarker         = "_coreConfirmed"
	recycleRetentionSetting = "agents/recycleRetentionHours"
	contextFetchTimeout     = 5 * time.Second
)

type confirmations struct {
	store  *phonefamily.Confirmations
	diag   *phonefamily.Confirmations
	secret string
}

func newConfirmations() *confirmations {
	raw := make([]byte, 16)
	_, _ = rand.Read(raw)
	return &confirmations{store: phonefamily.NewConfirmations(), diag: phonefamily.NewConfirmations(),
		secret: hex.EncodeToString(raw)}
}

func (m *Module) nowMs() int64 { return m.now().UnixMilli() }

func (m *Module) audit(req *rpc.Request, what string) {
	m.logUI("%s, requested by %s.", what, phonefamily.DeviceLabel(req))
}

func (m *Module) contextFor(ctx context.Context, fresh bool) (desktopContext, bool) {
	snapshot, pushed := m.snapshot()
	if (pushed && !fresh) || !m.desktopAttached() {
		return snapshot, pushed
	}
	callCtx, cancel := context.WithTimeout(ctx, contextFetchTimeout)
	defer cancel()
	reply, err := m.callUI(callCtx, "ui.agents.context", map[string]any{"confirm": m.confirms.secret})
	if err != nil {
		return snapshot, pushed
	}
	params, ok := qtjson.ParseObject(reply)
	if !ok {
		return snapshot, pushed
	}
	next := parseContext(params)
	m.mu.Lock()
	if !m.pushed || next.Seq >= m.ctx.Seq {
		m.ctx = next
		m.pushed = true
	}
	m.mu.Unlock()
	return next, true
}

func cleanParams(req *rpc.Request) qtjson.Value {
	params := paramsObject(req)
	params.Remove(confirmedMarker)
	return params
}

func (m *Module) delegate(req *rpc.Request, confirmed bool) error {
	params := cleanParams(req)
	if confirmed {
		params.Set(confirmedMarker, qtjson.NewString(m.confirms.secret))
	}
	return rpc.DelegateWith(json.RawMessage(params.Compact()))
}

func (m *Module) registerOps(core *daemon.Core) {
	ops := map[string]rpc.Handler{
		"agentops.approvals":     m.opsApprovals,
		"agentops.prepare":       m.opsPrepare,
		"agentops.answer":        m.opsAnswer,
		"agentops.merge":         m.opsConfirmedByID("merge"),
		"agentops.delete":        m.opsConfirmedByID("delete"),
		"agentops.bulk":          m.opsBulk,
		"agentops.revoke":        m.opsRevoke,
		"agentops.diff":          m.opsDiff,
		"agentops.file":          m.opsFile,
		"agentops.mergestats":    m.opsMergeStats,
		"agentops.worktrees":     m.opsWorktrees,
		"agentops.usage":         m.opsUsage,
		"agentops.grants":        m.opsGrants,
		"agentops.bin.restore":   m.opsBinRestore,
		"agentops.bin.purge":     m.opsBinPurge,
		"agentops.bin.retention": m.opsBinRetention,
	}
	for method, handler := range ops {
		core.Registry.Handle(method, m.Name(), m.gate.Wrap(m.armed(handler)))
	}
	for _, method := range []string{"agentops.accounts.select", "agentops.accounts.rename", "agentops.accounts.add"} {
		core.Registry.Handle(method, m.Name(), m.gate.Wrap(m.armed(func(ctx context.Context, req *rpc.Request) (any, error) {
			return nil, m.delegate(req, false)
		})))
	}
	m.watchUI(core)
}

func (m *Module) armed(handler rpc.Handler) rpc.Handler {
	return func(ctx context.Context, req *rpc.Request) (any, error) {
		m.armOpsBell()
		return handler(ctx, req)
	}
}

func (m *Module) armOpsBell() {
	m.mu.Lock()
	m.opsArmed = true
	send := !m.opsArmSent
	m.opsArmSent = true
	m.mu.Unlock()
	if send && !m.notifyUI("ui.agents.opsArm", map[string]any{}) {
		m.mu.Lock()
		m.opsArmSent = false
		m.mu.Unlock()
	}
}

func (m *Module) watchUI(core *daemon.Core) {
	if core == nil || core.IPC == nil {
		return
	}
	previous := core.IPC.OnUI
	core.IPC.OnUI = func(conn *ipc.Conn, attached bool) {
		if previous != nil {
			previous(conn, attached)
		}
		m.uiChanged(attached)
	}
}

func (m *Module) uiChanged(attached bool) {
	m.mu.Lock()
	m.opsArmSent = false
	armed := m.opsArmed
	m.mu.Unlock()
	if attached && armed {
		go m.armOpsBell()
	}
	if !attached {
		m.follows.markAllDirty()
		m.ringAgents()
	}
}

func (m *Module) opsApprovals(ctx context.Context, req *rpc.Request) (any, error) {
	snapshot, ok := m.contextFor(ctx, true)
	if !ok {
		return nil, rpc.Fail("desktop_unavailable")
	}
	return raw(qtjson.ObjectOf("approvals", snapshot.Approvals, "count", snapshot.Approvals.Len())), nil
}

func (m *Module) branchBusy(snapshot *desktopContext, sessions []agents.Session, target agents.Session) bool {
	for _, other := range sessions {
		if other.Merged || other.BranchName != target.BranchName || other.Owner != target.Owner || other.Name != target.Name {
			continue
		}
		if sessionActive(other.Status) && snapshot.InFlight[other.ID] {
			return true
		}
	}
	return false
}

func agentMergeBase(s agents.Session) string {
	if s.BaseBranch == "" {
		return "main"
	}
	return s.BaseBranch
}

func (m *Module) opsPrepare(ctx context.Context, req *rpc.Request) (any, error) {
	params := paramsObject(req)
	action := params.Get("action").Str()
	if !isKnownAction(action) {
		return nil, rpc.Fail("unknown_action")
	}
	fresh := action == "approve" || action == "deny" || action == "revoke"
	snapshot, ok := m.contextFor(ctx, fresh)
	if !ok {
		return nil, rpc.Fail("desktop_unavailable")
	}
	target, summary := "", ""
	switch action {
	case "approve", "deny":
		id := params.Get("id").Str()
		allow := action == "approve"
		lifetime := "once"
		if allow {
			lifetime = params.Get("lifetime").StrOr("once")
		}
		if !validApprovalID(id) || !validLifetime(lifetime) {
			return nil, rpc.Fail("bad_request")
		}
		if _, known := snapshot.ApprovalIndex[id]; !known {
			return nil, rpc.Fail("unknown_approval")
		}
		question, title := "", ""
		for _, row := range snapshot.Approvals.Items() {
			if row.Get("id").Str() == id {
				question = row.Get("question").Str()
				title = row.Get("title").Str()
				break
			}
		}
		target = answerTarget(id, allow, lifetime)
		summary = approvalSummary(allow, lifetime, question, title)
	case "merge":
		session, _, found := m.ownSession(params.Get("id").Int())
		if !found || session.BranchName == "" || session.Merged {
			return nil, rpc.Fail("unknown_session")
		}
		if snapshot.repoIndexFor(session.Owner, session.Name) < 0 {
			return nil, rpc.Fail("no_checkout")
		}
		sessions, _ := m.loadSessions()
		if m.branchBusy(&snapshot, sessions, session) {
			return nil, rpc.Fail("agent_running")
		}
		target = strconv.Itoa(session.ID)
		summary = mergeSummary(session.BranchName, agentMergeBase(session), session.Owner+"/"+session.Name)
	case "delete":
		id := params.Get("id").Int()
		if source, outside := snapshot.OutsideSources[id]; outside && id < 0 && snapshot.external(id) {
			if source.OtherUser {
				return nil, rpc.Fail("outside_other_user")
			}
			target = strconv.Itoa(id)
			summary = outsideDeleteSummary(source.Label, source.Cwd)
		} else {
			session, _, found := m.ownSession(id)
			if !found {
				return nil, rpc.Fail("unknown_session")
			}
			branch := session.BranchName
			if session.Merged {
				branch = ""
			}
			target = strconv.Itoa(session.ID)
			summary = deleteSummary(branch, session.Owner+"/"+session.Name, stoppable(session))
		}
	case "stop", "continue":
		ids, valid := canonicalIDs(params.Get("ids"), maxBulkIDs)
		if !valid {
			return nil, rpc.Fail("bad_request")
		}
		target = bulkTarget(action, ids)
		summary = bulkSummary(action, len(ids))
	case "purge":
		ids, valid := canonicalIDs(params.Get("ids"), maxBinAgents)
		if !valid {
			return nil, rpc.Fail("bad_request")
		}
		target = bulkTarget(action, ids)
		summary = purgeSummary(len(ids))
	case "revoke":
		grantID := params.Get("grantId").Str()
		if !validGrantID(grantID) || !snapshot.Grants.IsArray() {
			return nil, rpc.Fail("unknown_grant")
		}
		for _, grant := range snapshot.Grants.Items() {
			if grant.Get("id").Str() == grantID {
				target = grantID
				summary = revokeSummary(grant.Get("capability").Str(), grant.Get("resource").Str())
				break
			}
		}
		if target == "" {
			return nil, rpc.Fail("unknown_grant")
		}
	}
	token := m.confirms.store.Issue(phonefamily.Holder(req), action, target, m.nowMs())
	return raw(qtjson.ObjectOf("token", token, "action", action, "summary", summary,
		"destructive", isDestructiveAction(action), "expiresInMs", phonefamily.ConfirmationLifetimeMs)), nil
}

func (m *Module) consume(req *rpc.Request, params qtjson.Value, action, target string) error {
	verdict := m.confirms.store.Consume(params.Get("token").Str(), phonefamily.Holder(req), action, target, m.nowMs())
	if verdict != phonefamily.Ok {
		return rpc.Fail(phonefamily.ConfirmationError(verdict))
	}
	return nil
}

func (m *Module) opsAnswer(ctx context.Context, req *rpc.Request) (any, error) {
	params := paramsObject(req)
	id := params.Get("id").Str()
	decision := params.Get("decision").Str()
	allow := decision == "allow"
	lifetime := "once"
	if allow {
		lifetime = params.Get("lifetime").StrOr("once")
	}
	if !validApprovalID(id) || (!allow && decision != "deny") || !validLifetime(lifetime) {
		return nil, rpc.Fail("bad_request")
	}
	action := "deny"
	if allow {
		action = "approve"
	}
	if err := m.consume(req, params, action, answerTarget(id, allow, lifetime)); err != nil {
		return nil, err
	}
	return nil, m.delegate(req, true)
}

func (m *Module) opsConfirmedByID(action string) rpc.Handler {
	return func(ctx context.Context, req *rpc.Request) (any, error) {
		params := paramsObject(req)
		if err := m.consume(req, params, action, strconv.Itoa(params.Get("id").Int())); err != nil {
			return nil, err
		}
		return nil, m.delegate(req, true)
	}
}

func (m *Module) opsBulk(ctx context.Context, req *rpc.Request) (any, error) {
	params := paramsObject(req)
	action := params.Get("action").Str()
	ids, valid := canonicalIDs(params.Get("ids"), maxBulkIDs)
	if (action != "stop" && action != "continue") || !valid {
		return nil, rpc.Fail("bad_request")
	}
	if err := m.consume(req, params, action, bulkTarget(action, ids)); err != nil {
		return nil, err
	}
	return nil, m.delegate(req, true)
}

func (m *Module) opsRevoke(ctx context.Context, req *rpc.Request) (any, error) {
	params := paramsObject(req)
	grantID := params.Get("grantId").Str()
	snapshot, ok := m.contextFor(ctx, true)
	if !validGrantID(grantID) || !ok || !snapshot.Grants.IsArray() {
		return nil, rpc.Fail("unknown_grant")
	}
	if err := m.consume(req, params, "revoke", grantID); err != nil {
		return nil, err
	}
	return nil, m.delegate(req, true)
}

func (m *Module) sessionRepo(snapshot *desktopContext, session agents.Session) (repository, bool) {
	index := snapshot.repoIndexFor(session.Owner, session.Name)
	if index < 0 || snapshot.Repositories[index].LocalPath == "" {
		return repository{}, false
	}
	return snapshot.Repositories[index], true
}

func (m *Module) opsDiff(ctx context.Context, req *rpc.Request) (any, error) {
	params := paramsObject(req)
	session, _, found := m.ownSession(params.Get("id").Int())
	if !found || session.BranchName == "" {
		return nil, rpc.Fail("unknown_session")
	}
	snapshot, ok := m.contextFor(ctx, false)
	if !ok {
		return nil, rpc.Fail("desktop_unavailable")
	}
	repo, ok := m.sessionRepo(&snapshot, session)
	if !ok {
		return nil, rpc.Fail("no_checkout")
	}
	read := readAgentDiff(repo.LocalPath, agentMergeBase(session), session.BranchName)
	if read.err != "" {
		return nil, rpc.Fail(read.err)
	}
	credentials, _ := m.credentials()
	untracked := read.untracked
	if untracked == nil {
		untracked = []string{}
	}
	return raw(qtjson.ObjectOf(
		"id", session.ID,
		"branch", session.BranchName,
		"base", read.base,
		"merged", session.Merged,
		"files", read.files,
		"committed", agentrun.RedactText(fromU16(read.committed), credentials),
		"uncommitted", agentrun.RedactText(fromU16(read.uncommitted), credentials),
		"untracked", untracked,
		"hasWorktree", read.worktree != "",
		"truncated", read.truncated,
	)), nil
}

func (m *Module) opsFile(ctx context.Context, req *rpc.Request) (any, error) {
	params := paramsObject(req)
	path := params.Get("path").Str()
	change := params.Get("change").Str()
	side := params.Get("side").Str()
	if !validDiffImagePath(path) || (change != "committed" && change != "uncommitted") || (side != "old" && side != "new") {
		return nil, rpc.Fail("bad_request")
	}
	session, _, found := m.ownSession(params.Get("id").Int())
	if !found || session.BranchName == "" {
		return nil, rpc.Fail("unknown_session")
	}
	snapshot, ok := m.contextFor(ctx, false)
	if !ok {
		return nil, rpc.Fail("desktop_unavailable")
	}
	repo, ok := m.sessionRepo(&snapshot, session)
	if !ok {
		return nil, rpc.Fail("no_checkout")
	}
	read := readAgentDiffImage(repo.LocalPath, agentMergeBase(session), session.BranchName, path,
		change == "uncommitted", side == "new")
	if read.err != "" {
		return nil, rpc.Fail(read.err)
	}
	return raw(qtjson.ObjectOf("path", path, "mime", diffImageMime(path), "size", float64(len(read.data)),
		"data", base64.StdEncoding.EncodeToString(read.data))), nil
}

func (m *Module) opsMergeStats(ctx context.Context, req *rpc.Request) (any, error) {
	params := paramsObject(req)
	snapshot, ok := m.contextFor(ctx, false)
	if !ok {
		return nil, rpc.Fail("desktop_unavailable")
	}
	repoIndex := -1
	base := ""
	sessionID := params.Get("id").Int()
	owner := trimmed(params.Get("owner").Str())
	name := trimmed(params.Get("name").Str())
	hasCheckout := func(index int) bool {
		return index >= 0 && index < len(snapshot.Repositories) && snapshot.Repositories[index].LocalPath != ""
	}
	switch {
	case sessionID > 0:
		session, _, found := m.ownSession(sessionID)
		if !found {
			return nil, rpc.Fail("unknown_session")
		}
		repoIndex = snapshot.repoIndexFor(session.Owner, session.Name)
		base = agentMergeBase(session)
	case owner != "" || name != "":
		repoIndex = snapshot.repoIndexFor(owner, name)
	case hasCheckout(snapshot.DetailRepo):
		repoIndex = snapshot.DetailRepo
	default:
		newest := int64(-1)
		sessions, _ := m.loadSessions()
		for _, session := range sessions {
			if snapshot.external(session.ID) || session.DeletedAtMs > 0 || session.CreatedAtMs <= newest {
				continue
			}
			index := snapshot.repoIndexFor(session.Owner, session.Name)
			if !hasCheckout(index) {
				continue
			}
			newest = session.CreatedAtMs
			repoIndex = index
		}
	}
	if !hasCheckout(repoIndex) {
		return nil, rpc.Fail("no_checkout")
	}
	repo := snapshot.Repositories[repoIndex]
	into := mergeInto(repo.LocalPath, base)
	stats, branch := readLandedMergeStats(repo.LocalPath, into, "", true, m.now().Unix())
	if stats.Base == "" {
		return nil, rpc.Fail("no_base_branch")
	}
	return raw(qtjson.ObjectOf("repo", repo.Owner+"/"+repo.Name, "branch", branch, "stats", mergeStatsJSON(stats))), nil
}

func (m *Module) opsWorktrees(ctx context.Context, req *rpc.Request) (any, error) {
	params := paramsObject(req)
	owner := trimmed(params.Get("owner").Str())
	name := trimmed(params.Get("name").Str())
	snapshot, ok := m.contextFor(ctx, false)
	if !ok {
		return nil, rpc.Fail("desktop_unavailable")
	}
	index := snapshot.repoIndexFor(owner, name)
	if index < 0 || snapshot.Repositories[index].LocalPath == "" {
		return nil, rpc.Fail("no_checkout")
	}
	repo := snapshot.Repositories[index]
	branches := map[string]int{}
	sessions, _ := m.loadSessions()
	for _, session := range sessions {
		if snapshot.external(session.ID) || session.DeletedAtMs > 0 || session.BranchName == "" ||
			!equalFoldQt(session.Owner, owner) || !equalFoldQt(session.Name, name) {
			continue
		}
		if branches[session.BranchName] < session.ID {
			branches[session.BranchName] = session.ID
		}
	}
	base, rows := worktreeScan(repo.LocalPath, branches)
	return raw(qtjson.ObjectOf("repo", repo.Owner+"/"+repo.Name, "base", base, "worktrees", rows)), nil
}

func (m *Module) opsUsage(ctx context.Context, req *rpc.Request) (any, error) {
	usage := qtjson.Value{}
	if m.desktopAttached() {
		callCtx, cancel := context.WithTimeout(ctx, contextFetchTimeout)
		reply, err := m.callUI(callCtx, "ui.agents.usage", map[string]any{})
		cancel()
		if err == nil {
			if parsed, ok := qtjson.ParseObject(reply); ok {
				usage = parsed
				m.mu.Lock()
				m.usage = parsed
				m.mu.Unlock()
			}
		}
	}
	if !usage.IsObject() {
		m.mu.Lock()
		usage = m.usage
		m.mu.Unlock()
	}
	if !usage.IsObject() {
		return nil, rpc.Fail("desktop_unavailable")
	}
	return raw(usageReply(usage, m.nowMs())), nil
}

func usageReply(usage qtjson.Value, nowMs int64) qtjson.Value {
	claude, openAi := -1.0, -1.0
	if value := usage.Get("claudeSpendUsd"); value.IsNumber() {
		claude = value.Float()
	}
	if value := usage.Get("openAiSpendUsd"); value.IsNumber() {
		openAi = value.Float()
	}
	summary := usageSummary(usageProvidersFrom(usage.Get("providers")), claude, openAi, nowMs)
	summary.Set("activeProvider", qtjson.NewString(usage.Get("activeProvider").Str()))
	return summary
}

func (m *Module) opsGrants(ctx context.Context, req *rpc.Request) (any, error) {
	snapshot, ok := m.contextFor(ctx, true)
	if !ok {
		return nil, rpc.Fail("desktop_unavailable")
	}
	grants := qtjson.NewArray()
	if snapshot.Grants.IsArray() {
		grants = grantRows(snapshot.Grants, maxGrants)
	}
	return raw(qtjson.ObjectOf("grants", grants)), nil
}

func (m *Module) retentionHours() int {
	hours := int64(agents.DefaultRecycleRetentionHours)
	if m.core != nil && m.core.Settings != nil {
		if value, ok := m.core.Settings.Get().Value(recycleRetentionSetting); ok {
			hours = value.Int(0)
		}
	}
	return int(min(max(hours, agents.MinRecycleRetentionHours), agents.MaxRecycleRetentionHours))
}

func (m *Module) binReply(hours int) qtjson.Value {
	entries := []binEntry{}
	if writer := m.writer(); writer != nil {
		for _, session := range writer.Store().LoadRecycledSessions() {
			entries = append(entries, binEntry{ID: session.ID, Title: binTitle(session.IssueNumber, session.IssueTitle,
				session.Prompt), Owner: session.Owner, Name: session.Name, Branch: session.BranchName,
				Provider: session.Provider, DeletedAtMs: session.DeletedAtMs})
		}
	}
	m.notifyReload(false, true)
	return binPayload(entries, hours, agents.MinRecycleRetentionHours, agents.MaxRecycleRetentionHours, m.nowMs())
}

func (m *Module) notifyReload(sessions, bin bool) {
	m.notifyUI("ui.agents.reload", map[string]any{"sessions": sessions, "bin": bin})
}

func joinIDs(ids []int) string {
	parts := make([]string, len(ids))
	for i, id := range ids {
		parts[i] = strconv.Itoa(id)
	}
	return strings.Join(parts, ", ")
}

func intsJSON(ids []int) qtjson.Value {
	out := qtjson.NewArray()
	for _, id := range ids {
		out.Append(qtjson.NewInt(int64(id)))
	}
	return out
}

func (m *Module) opsBinRestore(ctx context.Context, req *rpc.Request) (any, error) {
	writer := m.writer()
	if writer == nil {
		return nil, rpc.Fail("unavailable")
	}
	ids, valid := canonicalIDs(paramsObject(req).Get("ids"), maxBinAgents)
	if !valid {
		return nil, rpc.Fail("bad_request")
	}
	m.audit(req, "Restoring deleted agents "+joinIDs(ids))
	restored, failed := []int{}, []int{}
	for _, id := range ids {
		if ok, _ := writer.RestoreRecycled(id); ok {
			restored = append(restored, id)
		} else {
			failed = append(failed, id)
		}
	}
	if len(restored) > 0 {
		m.notifyReload(true, false)
	}
	reply := m.binReply(m.retentionHours())
	reply.Set("restored", intsJSON(restored))
	reply.Set("failed", intsJSON(failed))
	m.ringAgents()
	return raw(reply), nil
}

func (m *Module) opsBinPurge(ctx context.Context, req *rpc.Request) (any, error) {
	writer := m.writer()
	if writer == nil {
		return nil, rpc.Fail("unavailable")
	}
	params := paramsObject(req)
	ids, valid := canonicalIDs(params.Get("ids"), maxBinAgents)
	if !valid {
		return nil, rpc.Fail("bad_request")
	}
	if err := m.consume(req, params, "purge", bulkTarget("purge", ids)); err != nil {
		return nil, err
	}
	m.audit(req, "Permanently deleting deleted agents "+joinIDs(ids))
	purged, skipped := []int{}, []int{}
	for _, id := range ids {
		if ok, _ := writer.PurgeRecycled(id); ok {
			purged = append(purged, id)
		} else {
			skipped = append(skipped, id)
		}
	}
	reply := m.binReply(m.retentionHours())
	reply.Set("purged", intsJSON(purged))
	reply.Set("skipped", intsJSON(skipped))
	return raw(reply), nil
}

func (m *Module) opsBinRetention(ctx context.Context, req *rpc.Request) (any, error) {
	writer := m.writer()
	if writer == nil {
		return nil, rpc.Fail("unavailable")
	}
	value := paramsObject(req).Get("hours")
	hours := -1.0
	if value.IsNumber() {
		hours = value.Float()
	}
	if !value.IsNumber() || hours != float64(int64(hours)) || hours < agents.MinRecycleRetentionHours ||
		hours > agents.MaxRecycleRetentionHours {
		return nil, rpc.Fail("bad_request")
	}
	if !m.desktopAttached() {
		return nil, rpc.Fail("desktop_unavailable")
	}
	m.audit(req, "Keeping deleted agents for "+strconv.Itoa(int(hours))+" hours")
	if _, err := m.callUI(ctx, "ui.settings.write", map[string]any{"writes": []daemon.SettingWrite{{Key: recycleRetentionSetting, Type: "int", Value: int(hours)}}}); err != nil {
		return nil, rpc.Fail("desktop_unavailable")
	}
	retentionMs := int64(hours) * 60 * 60 * 1000
	if purged := writer.PurgeExpired(retentionMs, m.nowMs()); purged > 0 {
		suffix := "ies"
		if purged == 1 {
			suffix = "y"
		}
		m.logUI("Removed %d expired agent recycle bin entr%s (retention %d hours).", purged, suffix, int(hours))
	}
	return raw(m.binReply(int(hours))), nil
}
