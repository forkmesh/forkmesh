package actions

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"regexp"
	"strconv"
	"strings"
	"sync"
	"time"

	"forkmesh.com/desktop-core/internal/rpc"
)

const (
	MaxRuns              = 50
	MaxManualBranches    = 100
	MaxLogChars          = 128 * 1024
	confirmationLifetime = 60 * 1000
	maxOutstanding       = 32
)

const (
	actionRunWorkflow     = "workflow.run"
	actionApprove         = "run.approve"
	actionReject          = "run.reject"
	actionRerun           = "run.rerun"
	actionStop            = "run.stop"
	actionSkip            = "run.skip"
	actionEnableWorkflow  = "workflow.enable"
	actionDisableWorkflow = "workflow.disable"
)

var allActions = []string{actionRunWorkflow, actionApprove, actionReject, actionRerun, actionStop, actionSkip, actionEnableWorkflow, actionDisableWorkflow}

type RepoFacts struct {
	Repo              string   `json:"repo"`
	Owner             string   `json:"owner"`
	Name              string   `json:"name"`
	MirrorPath        string   `json:"mirrorPath"`
	LocalPath         string   `json:"localPath"`
	Enabled           bool     `json:"enabled"`
	AutoApprove       bool     `json:"autoApprove"`
	DisabledWorkflows []string `json:"disabledWorkflows"`
	WorkflowNodes     []string `json:"workflowNodes"`
	NodeLabels        []string `json:"nodeLabels"`
}

type FactsLocator func(ctx context.Context, peer rpc.Peer, method, repo string) (RepoFacts, error)

type Intent struct {
	Action string `json:"action"`
	Repo   string `json:"repo"`
	Path   string `json:"path"`
	Branch string `json:"branch"`
	Run    int    `json:"run"`
}

type Performer func(ctx context.Context, intent Intent, who string, peer rpc.Peer) (map[string]any, error)

var (
	repoSlugPattern     = regexp.MustCompile(`^([A-Za-z0-9_.-]{1,100})/([A-Za-z0-9_.-]{1,100})$`)
	workflowPathPattern = regexp.MustCompile(`^\.forkmesh/[A-Za-z0-9_.-]{1,120}\.ya?ml$`)
	branchPattern       = regexp.MustCompile(`^[A-Za-z0-9_./-]{1,200}$`)
)

func ParseRepoSlug(slug string) (string, string, bool) {
	match := repoSlugPattern.FindStringSubmatch(qtTrim(slug))
	if match == nil {
		return "", "", false
	}
	owner, name := match[1], match[2]
	if owner == "." || owner == ".." || name == "." || name == ".." {
		return "", "", false
	}
	return owner, name, true
}

func ValidWorkflowPath(p string) bool {
	return workflowPathPattern.MatchString(p) && !strings.Contains(p, "..")
}

func ValidBranchName(branch string) bool {
	return branchPattern.MatchString(branch) && !strings.HasPrefix(branch, "-") && !strings.HasPrefix(branch, "/") &&
		!strings.HasSuffix(branch, "/") && !strings.Contains(branch, "..") && !strings.Contains(branch, "//") &&
		!strings.HasSuffix(branch, ".lock")
}

func branchFromRef(ref string) string {
	if strings.HasPrefix(ref, "refs/heads/") {
		return ref[11:]
	}
	if strings.HasPrefix(ref, "refs/tags/") {
		return ref[10:]
	}
	return ref
}

func actionTargetsRun(action string) bool {
	return action != actionRunWorkflow && !actionSwitchesWorkflow(action) && containsString(allActions, action)
}

func actionSwitchesWorkflow(action string) bool {
	return action == actionEnableWorkflow || action == actionDisableWorkflow
}

func RunStatusAllows(action, status string) bool {
	switch action {
	case actionApprove, actionReject:
		return status == StatusAwaitingApproval
	case actionStop:
		return status == StatusRunning || status == StatusQueued
	case actionSkip:
		return status == StatusQueued || status == StatusAwaitingApproval
	case actionRerun:
		return status == StatusSuccess || status == StatusFailed || status == StatusCancelled || status == StatusRejected || status == StatusSkipped
	}
	return false
}

func ActionLabel(action string) string {
	switch action {
	case actionRunWorkflow:
		return "Run workflow"
	case actionApprove:
		return "Approve run"
	case actionReject:
		return "Reject run"
	case actionRerun:
		return "Rerun"
	case actionStop:
		return "Stop run"
	case actionSkip:
		return "Skip run"
	case actionEnableWorkflow:
		return "Turn on workflow"
	case actionDisableWorkflow:
		return "Turn off workflow"
	}
	return action
}

func IntentSubject(intent Intent) string {
	if intent.Action == actionRunWorkflow {
		return intent.Repo + ":" + intent.Path + "@" + intent.Branch
	}
	if actionSwitchesWorkflow(intent.Action) {
		return intent.Repo + ":" + intent.Path
	}
	return intent.Repo + "#" + strconv.Itoa(intent.Run)
}

func ActionSummary(intent Intent, workflowName string) string {
	name := flatText(workflowName, 160)
	quoted := fmt.Sprintf("\"%s\"", name)
	if name == "" {
		quoted = fmt.Sprintf("\"%s\"", intent.Path)
	}
	switch intent.Action {
	case actionRunWorkflow:
		what := intent.Path
		if name != "" {
			what = fmt.Sprintf("%s (%s)", quoted, intent.Path)
		}
		return fmt.Sprintf("Run %s on %s of %s. It runs on this desktop in the Actions sandbox; a workflow that is not approved yet waits for approval.", what, intent.Branch, intent.Repo)
	case actionEnableWorkflow:
		return fmt.Sprintf("Turn on %s in %s. Its push, pull request and release triggers start runs again.", quoted, intent.Repo)
	case actionDisableWorkflow:
		return fmt.Sprintf("Turn off %s in %s. It does not run until it is turned on again.", quoted, intent.Repo)
	}
	run := fmt.Sprintf("run #%d, %s of %s", intent.Run, quoted, intent.Repo)
	switch intent.Action {
	case actionApprove:
		return fmt.Sprintf("Approve %s. The desktop checks the repository has not changed, then runs it.", run)
	case actionReject:
		return fmt.Sprintf("Reject %s. It will not run.", run)
	case actionRerun:
		return fmt.Sprintf("Run %s of %s again on the same commit as run #%d.", quoted, intent.Repo, intent.Run)
	case actionStop:
		return fmt.Sprintf("Stop %s.", run)
	case actionSkip:
		return fmt.Sprintf("Skip %s. It will not run.", run)
	}
	return ""
}

type LogSlice struct {
	Text  string
	From  int64
	Next  int64
	Reset bool
}

func LogSliceOf(log string, since, maxChars int64) LogSlice {
	units := utf16Units(log)
	total := int64(len(units))
	var slice LogSlice
	if since >= 0 && since <= total {
		slice.From = since
		if total-slice.From > maxChars {
			slice.From = total - maxChars
		}
	} else {
		slice.Reset = since >= 0
		if total > maxChars {
			slice.From = total - maxChars
		}
	}
	if slice.From > 0 && slice.From < total && units[slice.From] >= 0xDC00 && units[slice.From] <= 0xDFFF {
		slice.From++
	}
	slice.Text = fromUnits(units[slice.From:])
	slice.Next = total
	return slice
}

type confirmEntry struct {
	holder    string
	intent    Intent
	expiresAt int64
}

type confirmations struct {
	mu      sync.Mutex
	entries map[string]confirmEntry
}

func newConfirmations() *confirmations {
	return &confirmations{entries: map[string]confirmEntry{}}
}

func newToken() string {
	var raw [24]byte
	_, _ = rand.Read(raw[:])
	return hex.EncodeToString(raw[:])
}

func (c *confirmations) prune(nowMs int64) {
	for token, entry := range c.entries {
		if entry.expiresAt <= nowMs {
			delete(c.entries, token)
		}
	}
}

func (c *confirmations) issue(holder string, intent Intent, nowMs int64) (string, int64) {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.prune(nowMs)
	for len(c.entries) >= maxOutstanding {
		oldest := ""
		var oldestAt int64
		for token, entry := range c.entries {
			if oldest == "" || entry.expiresAt < oldestAt {
				oldest, oldestAt = token, entry.expiresAt
			}
		}
		delete(c.entries, oldest)
	}
	token := newToken()
	expires := nowMs + confirmationLifetime
	c.entries[token] = confirmEntry{holder: holder, intent: intent, expiresAt: expires}
	return token, expires
}

func (c *confirmations) consume(token, holder string, nowMs int64) (Intent, string) {
	c.mu.Lock()
	defer c.mu.Unlock()
	entry, ok := c.entries[token]
	if token == "" || !ok || entry.intent.Action == "" {
		delete(c.entries, token)
		c.prune(nowMs)
		return Intent{}, "confirmation_required"
	}
	delete(c.entries, token)
	c.prune(nowMs)
	if entry.expiresAt <= nowMs {
		return Intent{}, "confirmation_expired"
	}
	if entry.holder != holder {
		return Intent{}, "confirmation_mismatch"
	}
	return entry.intent, ""
}

func (c *confirmations) outstanding() int {
	c.mu.Lock()
	defer c.mu.Unlock()
	return len(c.entries)
}

func peerDevice(req *rpc.Request) rpc.Device {
	if req.Peer == nil {
		return rpc.Device{}
	}
	return req.Peer.Device()
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

func uiUnavailable(err error) bool {
	code := rpc.Code(err)
	return code == "ui_gone" || code == "ui_timeout" || code == "desktop_unavailable" ||
		errors.Is(err, context.Canceled) || errors.Is(err, context.DeadlineExceeded)
}

func (m *Module) factsFromUI(ctx context.Context, peer rpc.Peer, method, repo string) (RepoFacts, error) {
	device := rpc.Device{}
	if peer != nil {
		device = peer.Device()
	}
	if m.core == nil {
		return RepoFacts{}, rpc.Fail("desktop_unavailable")
	}
	raw, err := m.core.CallUI(ctx, "ui.actions.repo", map[string]any{"method": method, "repo": repo, "device": device})
	if err != nil {
		if uiUnavailable(err) {
			return RepoFacts{}, rpc.Fail("desktop_unavailable")
		}
		return RepoFacts{}, err
	}
	var facts RepoFacts
	if err := json.Unmarshal(raw, &facts); err != nil || facts.Owner == "" || facts.Name == "" {
		return RepoFacts{}, rpc.Fail("desktop_unavailable")
	}
	return facts, nil
}

func (m *Module) performWithUI(ctx context.Context, intent Intent, who string, peer rpc.Peer) (map[string]any, error) {
	if m.core == nil {
		return nil, rpc.Fail("desktop_unavailable")
	}
	device := rpc.Device{}
	if peer != nil {
		device = peer.Device()
	}
	callCtx, cancel := context.WithTimeout(ctx, 2*time.Minute)
	defer cancel()
	raw, err := m.core.CallUI(callCtx, "ui.actions.perform", map[string]any{"intent": intent, "who": who, "device": device})
	if err != nil {
		if uiUnavailable(err) {
			return nil, rpc.Fail("desktop_unavailable")
		}
		return nil, err
	}
	out := map[string]any{}
	if err := json.Unmarshal(raw, &out); err != nil {
		return nil, rpc.Fail("desktop_unavailable")
	}
	return out, nil
}

func stringValue(params map[string]any, key string) string {
	text, _ := params[key].(string)
	return text
}

func ownerAllowed(names []string, device rpc.Device) bool {
	if device.Key == "" {
		return false
	}
	user := strings.TrimSpace(device.User)
	for _, name := range names {
		name = strings.TrimSpace(name)
		if user != "" && name != "" && strings.EqualFold(user, name) {
			return true
		}
	}
	return false
}

func (m *Module) knownOwners() ([]string, bool) {
	if m.core == nil {
		return nil, false
	}
	switch value := m.core.Get("control.owners").(type) {
	case []string:
		return value, len(value) > 0
	case []any:
		var out []string
		for _, item := range value {
			if text, ok := item.(string); ok {
				out = append(out, text)
			}
		}
		return out, len(out) > 0
	}
	return nil, false
}

func runFactsRow(run Run, stepNames []string) map[string]any {
	steps := make([]map[string]any, 0, len(stepNames))
	for i, name := range stepNames {
		status := StepPending
		if i < len(run.StepStatuses) {
			status = run.StepStatuses[i]
		}
		steps = append(steps, map[string]any{"name": flatText(name, 160), "status": status})
	}
	row := map[string]any{
		"id":           run.ID,
		"workflow":     flatText(run.WorkflowName, 160),
		"path":         run.WorkflowPath,
		"status":       run.Status,
		"commit":       run.Commit,
		"branch":       branchFromRef(run.Ref),
		"createdAtMs":  float64(run.CreatedAtMs),
		"startedAtMs":  float64(run.StartedAtMs),
		"finishedAtMs": float64(run.FinishedAtMs),
		"currentStep":  run.CurrentStepIndex,
		"steps":        steps,
	}
	if run.PullNumber > 0 {
		row["pull"] = run.PullNumber
		row["pullAuthor"] = flatText(run.PullAuthor, 120)
	}
	return row
}

func ActionsSummary(runs []Run) map[string]any {
	running, queued, awaiting := 0, 0, 0
	var latest *Run
	for index := range runs {
		run := &runs[index]
		switch run.Status {
		case StatusRunning:
			running++
		case StatusQueued:
			queued++
		case StatusAwaitingApproval:
			awaiting++
		}
		if latest == nil || run.CreatedAtMs > latest.CreatedAtMs {
			latest = run
		}
	}
	summary := map[string]any{"running": running, "queued": queued, "awaiting": awaiting}
	if latest != nil {
		summary["latestStatus"] = latest.Status
		summary["latestWorkflow"] = flatText(latest.WorkflowName, 160)
		summary["latestAtMs"] = float64(latest.CreatedAtMs)
	}
	return summary
}

func RepoActionsSummary(store *Store, owner, name string) map[string]any {
	return ActionsSummary(store.RepoRuns(owner, name))
}

func (m *Module) RepoSummary(owner, name string) map[string]any {
	return RepoActionsSummary(m.st, owner, name)
}

func SummariesByRepo(store *Store) map[string]map[string]any {
	grouped := map[string][]Run{}
	for _, run := range store.LoadAllRuns() {
		key := strings.ToLower(run.Owner + "/" + run.Name)
		grouped[key] = append(grouped[key], run)
	}
	out := make(map[string]map[string]any, len(grouped))
	for key, runs := range grouped {
		out[key] = ActionsSummary(runs)
	}
	return out
}

func (m *Module) Summaries() map[string]map[string]any {
	if m.st == nil {
		return map[string]map[string]any{}
	}
	return SummariesByRepo(m.st)
}

func workflowTriggers(wf Workflow) []string {
	triggers := []string{}
	if wf.TriggersOnPush() {
		triggers = append(triggers, "push")
	}
	if wf.TriggersOnPullRequest() {
		triggers = append(triggers, "pull_request")
	}
	if wf.TriggersOnRelease() {
		triggers = append(triggers, "release")
	}
	if wf.AllowsManualRun() {
		triggers = append(triggers, "manual")
	}
	return triggers
}

func workflowRow(facts RepoFacts, wf Workflow) map[string]any {
	name := wf.Name
	if name == "" {
		name = wf.Path
	}
	here := !wf.Valid || WorkflowRunsOnThisNode(facts.WorkflowNodes, wf, facts.NodeLabels)
	row := map[string]any{
		"path":     wf.Path,
		"name":     flatText(name, 160),
		"valid":    wf.Valid,
		"triggers": workflowTriggers(wf),
		"manual":   wf.Valid && wf.AllowsManualRun(),
		"disabled": containsString(facts.DisabledWorkflows, wf.Path),
		"here":     here,
	}
	if wf.Error != "" {
		row["error"] = flatText(wf.Error, 300)
	}
	if wf.Valid {
		if runsOn := strings.Join(WorkflowRunsOnLabels(facts.WorkflowNodes, wf), ", "); runsOn != "" {
			row["runsOn"] = runsOn
		}
	}
	return row
}

func stepNamesFor(run Run, cache map[string][]string) []string {
	key := run.WorkflowPath + "\n" + run.WorkflowContent
	if names, ok := cache[key]; ok {
		return names
	}
	names := []string{}
	for _, step := range Parse(run.WorkflowPath, run.WorkflowContent).Steps {
		if step.Name == "" {
			names = append(names, firstLine(step.Run))
		} else {
			names = append(names, step.Name)
		}
	}
	cache[key] = names
	return names
}

func mirrorBranches(mirror string) []string {
	branches := []string{}
	if mirror == "" || !dirExists(mirror) {
		return branches
	}
	out, _ := gitStdoutAny(mirror, 60*time.Second, "for-each-ref", "--sort=-committerdate", "--format=%(refname:short)", "refs/heads")
	for _, line := range splitNonEmpty(qtFromUtf8(out), "\n") {
		if len(branches) >= MaxManualBranches {
			break
		}
		branches = append(branches, line)
	}
	return branches
}

func (m *Module) phoneActions(ctx context.Context, req *rpc.Request) (any, error) {
	params := req.Object()
	facts, err := m.Facts(ctx, req.Peer, req.Method, stringValue(params, "repo"))
	if err != nil {
		return nil, err
	}
	all := m.st.RepoRuns(facts.Owner, facts.Name)
	if len(all) > MaxRuns {
		all = all[:MaxRuns]
	}
	workflows := WorkflowsInRepoPaths(facts.MirrorPath, facts.LocalPath)
	rows := make([]map[string]any, 0, len(workflows))
	for _, wf := range workflows {
		rows = append(rows, workflowRow(facts, wf))
	}
	names := map[string][]string{}
	runs := make([]map[string]any, 0, len(all))
	for _, run := range all {
		runs = append(runs, runFactsRow(run, stepNamesFor(run, names)))
	}
	return map[string]any{
		"repo":        facts.Owner + "/" + facts.Name,
		"enabled":     facts.Enabled,
		"autoApprove": facts.AutoApprove,
		"mirror":      facts.MirrorPath != "",
		"workflows":   rows,
		"branches":    mirrorBranches(facts.MirrorPath),
		"runs":        runs,
		"summary":     ActionsSummary(all),
	}, nil
}

func slugOf(params map[string]any) string {
	owner, name, ok := ParseRepoSlug(stringValue(params, "repo"))
	if !ok {
		return ""
	}
	return owner + "/" + name
}

func (m *Module) findRepoRun(facts RepoFacts, id int) (Run, bool) {
	run, ok := m.st.FindRun(id)
	if !ok || strings.ToLower(run.Owner+"/"+run.Name) != strings.ToLower(facts.Owner+"/"+facts.Name) {
		return Run{}, false
	}
	return run, true
}

func (m *Module) phoneActionsLog(ctx context.Context, req *rpc.Request) (any, error) {
	params := req.Object()
	facts, err := m.Facts(ctx, req.Peer, req.Method, stringValue(params, "repo"))
	if err != nil {
		return nil, err
	}
	run, ok := m.findRepoRun(facts, numberParam(params, "run"))
	if !ok {
		return nil, rpc.Fail("unknown_run")
	}
	since := int64(-1)
	if _, present := params["since"]; present {
		since = int64(floatParam(params, "since"))
	}
	slice := LogSliceOf(m.st.ReadLog(run), since, MaxLogChars)
	return map[string]any{
		"repo":   slugOf(params),
		"id":     run.ID,
		"status": run.Status,
		"text":   slice.Text,
		"from":   float64(slice.From),
		"next":   float64(slice.Next),
		"reset":  slice.Reset,
	}, nil
}

func (m *Module) phonePrepare(ctx context.Context, req *rpc.Request) (any, error) {
	params := req.Object()
	facts, err := m.Facts(ctx, req.Peer, req.Method, stringValue(params, "repo"))
	if err != nil {
		return nil, err
	}
	slug := slugOf(params)
	intent := Intent{Action: stringValue(params, "action"), Repo: slug}
	if !containsString(allActions, intent.Action) {
		return nil, rpc.Fail("unsupported_action")
	}
	workflowName := ""
	switch {
	case actionTargetsRun(intent.Action):
		intent.Run = numberParam(params, "run")
		run, ok := m.findRepoRun(facts, intent.Run)
		if !ok {
			return nil, rpc.Fail("unknown_run")
		}
		if !RunStatusAllows(intent.Action, run.Status) {
			return nil, rpc.Fail("wrong_state")
		}
		workflowName = run.WorkflowName
	case actionSwitchesWorkflow(intent.Action):
		intent.Path = qtTrim(stringValue(params, "path"))
		if !ValidWorkflowPath(intent.Path) {
			return nil, rpc.Fail("bad_request")
		}
	default:
		intent.Path = qtTrim(stringValue(params, "path"))
		intent.Branch = qtTrim(stringValue(params, "branch"))
		if !ValidWorkflowPath(intent.Path) || !ValidBranchName(intent.Branch) {
			return nil, rpc.Fail("bad_request")
		}
		if !facts.Enabled {
			return nil, rpc.Fail("actions_disabled")
		}
		if facts.MirrorPath == "" {
			return nil, rpc.Fail("no_mirror")
		}
		if containsString(facts.DisabledWorkflows, intent.Path) {
			return nil, rpc.Fail("workflow_disabled")
		}
	}
	token, expires := m.confirmations.issue(peerDevice(req).Key, intent, nowMs())
	return map[string]any{
		"token":       token,
		"action":      intent.Action,
		"subject":     slug,
		"summary":     ActionSummary(intent, workflowName),
		"expiresAt":   float64(expires),
		"expiresInMs": float64(confirmationLifetime),
	}, nil
}

func (m *Module) phoneRun(ctx context.Context, req *rpc.Request) (any, error) {
	if names, known := m.knownOwners(); known && req.Peer != nil && rpc.Remote(req.Peer) && !ownerAllowed(names, req.Peer.Device()) {
		return nil, rpc.Fail("not_owner")
	}
	params := req.Object()
	intent, failure := m.confirmations.consume(stringValue(params, "token"), peerDevice(req).Key, nowMs())
	who := deviceLabel(req.Peer)
	if failure != "" {
		m.logUI("Refused a repository action from %s: %s.", who, failure)
		return nil, rpc.Fail(failure)
	}
	return m.Perform(ctx, intent, who, req.Peer)
}
