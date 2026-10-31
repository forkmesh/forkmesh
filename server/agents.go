package mirrornode

// Website agent jobs for managed mirror nodes.
//
// The website queues owner and organization agent prompts for one exact node,
// and the catalog makes a mirror selectable as soon as it advertises a provider
// CLI. Only the desktop used to claim those jobs, so a node running just this
// service accepted work it never ran ("The selected mirror has not claimed this
// job"). The daemon now claims the same signed leases, runs the provider CLI
// headlessly on a scratch clone, publishes the work as a branch (and a pull
// when asked), and reports every step on the job's lease.

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"
	"unicode/utf8"
)

const (
	agentJobResponseLimit  = 1 << 20
	agentJobSafetyCheck    = 5 * time.Minute
	agentJobClaimsPerRepo  = 8
	agentMaxConcurrentRuns = 2
	agentRunTimeout        = 2 * time.Hour
	agentPromptLimit       = 8000
	agentResultLimit       = 16000
	agentOutputLimit       = 4 << 20
	// One stream-json line is one event; a tool result quoting a whole file
	// is the largest. Past this a line is dropped, never the run.
	agentStreamLineLimit = 16 << 20
	agentJobRecordLimit  = 500
	// A terminal report the relay keeps refusing belongs to a deleted session.
	agentReportConflictLimit = 6
	agentJobsDirectoryName   = "agent-jobs"
)

var agentJobHTTPClient = &http.Client{Timeout: 30 * time.Second}

// A desktop queues "Move to cloud" the moment it has published the session's
// branch, but the branch reaches this node over the fleet push or the next
// sync, which can land after the claim. The run waits this long for it rather
// than failing the move; variables so tests need not wait.
var (
	agentHandoffBranchWait = 10 * time.Minute
	agentHandoffBranchPoll = 15 * time.Second
)

// The same model allowlist the desktop enforces for website-queued jobs.
var agentWebsiteModels = map[string]bool{
	"claude-haiku-4-5": true, "claude-sonnet-5": true, "claude-opus-5-5": true, "claude-opus-5": true,
	"claude-sonnet-4-6": true, "claude-opus-4-8": true, "claude-fable-5": true,
	"gpt-5.6-sol": true, "gpt-5.6-luna": true, "gpt-5.6-terra": true,
	"gpt-5.3-codex-spark": true,

	"gemma4:e2b": true, "gemma4:e4b": true, "gemma4:12b": true, "gemma4:26b": true, "gemma4:31b": true,
}

const defaultGemmaModel = "gemma4:e4b"

var agentWebsiteProviders = map[string]bool{"claude-code": true, "codex": true, "local-llama": true}

var agentSlugPattern = regexp.MustCompile(`[^a-z0-9]+`)

type agentJob struct {
	JobID           int64  `json:"jobId"`
	LeaseID         string `json:"leaseId"`
	Kind            string `json:"kind"`
	SessionID       string `json:"sessionId"`
	RepositoryOwner string `json:"repositoryOwner"`
	RepositoryName  string `json:"repositoryName"`
	Provider        string `json:"provider"`
	Prompt          string `json:"prompt"`
	Model           string `json:"model"`
	IssueNumber     int    `json:"issueNumber"`
	CreatePr        bool   `json:"createPr"`
	RequestedBy     string `json:"requestedBy"`
	Branch          string `json:"branch"`
	BaseBranch      string `json:"baseBranch"`
	Title           string `json:"title"`
	// Metadata only; the images are downloaded with the lease before the run
	// (see agent_attachments.go).
	Attachments []agentAttachment `json:"attachments"`
	// Runner-tier lease fields (runner.go): where to clone from, the
	// router-signed read and push grants, and the one ref the push grant
	// allows. A mirror node's own queue never carries them.
	Organization   string `json:"organization,omitempty"`
	CloneURL       string `json:"cloneUrl,omitempty"`
	ReadGrant      string `json:"readGrant,omitempty"`
	PushGrant      string `json:"pushGrant,omitempty"`
	PushRef        string `json:"pushRef,omitempty"`
	GrantExpiresAt int64  `json:"grantExpiresAt,omitempty"`
	TimeoutMs      int64  `json:"timeoutMs,omitempty"`
	ModelGrant     string `json:"modelGrant,omitempty"`
}

// agentSession is one website session this node runs. Follow-up prompts resume
// the same branch (and, for Claude Code, the same conversation).
type agentSession struct {
	SessionID        string          `json:"sessionId"`
	RepositoryOwner  string          `json:"repositoryOwner,omitempty"`
	LocalAgentID     int64           `json:"localAgentId"`
	RepositoryName   string          `json:"repositoryName"`
	Provider         string          `json:"provider"`
	Model            string          `json:"model,omitempty"`
	Title            string          `json:"title"`
	Branch           string          `json:"branch"`
	BaseBranch       string          `json:"baseBranch"`
	BaseRef          string          `json:"baseRef"`
	IssueNumber      int             `json:"issueNumber,omitempty"`
	CreatePr         bool            `json:"createPr"`
	PrNumber         int             `json:"prNumber,omitempty"`
	ProviderSession  string          `json:"providerSession,omitempty"`
	Status           string          `json:"status"`
	CreatedAt        int64           `json:"createdAt"`
	StartedAt        int64           `json:"startedAt"`
	FinishedAt       int64           `json:"finishedAt"`
	DurationMs       int64           `json:"durationMs"`
	NumTurns         int64           `json:"numTurns"`
	CostUsd          float64         `json:"costUsd"`
	PromptTokens     int64           `json:"promptTokens"`
	CompletionTokens int64           `json:"completionTokens"`
	LastError        string          `json:"lastError,omitempty"`
	DiffStats        *agentDiffStats `json:"diffStats,omitempty"`
}

// agentJobRecord makes a job idempotent: a job this node already started is
// re-reported, never run again, when the relay hands its lease back.
type agentJobRecord struct {
	JobID          int64  `json:"jobId"`
	LeaseID        string `json:"leaseId"`
	RepositoryName string `json:"repositoryName"`
	SessionID      string `json:"sessionId"`
	Provider       string `json:"provider"`
	Verdict        string `json:"verdict"`
	Status         string `json:"status"`
	Reason         string `json:"reason,omitempty"`
	Result         string `json:"result,omitempty"`
	// What the agent said and ran during this job, when the provider streams
	// it; the website's session page shows it as the run's transcript.
	Transcript  string `json:"transcript,omitempty"`
	GateUnready bool   `json:"gateUnready,omitempty"`
	FailureKind string `json:"failureKind,omitempty"`
	Reported    bool   `json:"reported"`
	Conflicts   int    `json:"conflicts,omitempty"`
	UpdatedAt   int64  `json:"updatedAt"`
}

type agentJobState struct {
	Sessions map[string]*agentSession   `json:"sessions"`
	Jobs     map[string]*agentJobRecord `json:"jobs"`
}

type agentWork struct {
	job         agentJob
	repo        Repository
	failureKind string
}

type agentRunOutcome struct {
	text             string
	transcript       string
	providerSession  string
	numTurns         int64
	durationMs       int64
	costUsd          float64
	promptTokens     int64
	completionTokens int64
}

type agentJobRunner struct {
	daemon *Daemon
	dir    string
	runCtx context.Context

	mu        sync.Mutex
	pending   bool
	lastCheck time.Time
	loaded    bool
	state     agentJobState
	active    map[string]bool
	running   int
	// leases holds the current lease of every accepted job that has no durable
	// record yet; a re-lease of the same job only refreshes it.
	leases  map[int64]string
	waiting map[int64]agentWork
	runs    sync.WaitGroup
	stops   map[string]context.CancelFunc

	lookPath func(string) (string, error)
	getenv   func(string) string
	// cliVersion reads a provider CLI's version; floor holds the versions
	// runs were refused on for being too old.
	cliVersion func(string) string
	floor      agentCLIFloor
	authMu     sync.Mutex
	authFailed map[string]runnerAuthFailure
	authSeen   map[string]string
	authLoaded bool
	now        func() time.Time

	// backend is where jobs come from and where their work goes: the served
	// repositories of a mirror node (nil, the default) or the app's runner
	// lease API (runner.go).
	backend agentBackend
	// maxRuns overrides agentMaxConcurrentRuns when positive.
	maxRuns int
	// command runs a provider CLI; nil runs it directly (runAgentCommandTo).
	command       agentCommandRunner
	mirrorBox     *jobSandbox
	streamTick    time.Duration
	credentialUID int
}

// agentCommandRunner runs one provider CLI invocation in dir, streaming its
// standard output to stdout and returning its captured standard error.
type agentCommandRunner func(ctx context.Context, dir, stdin string, stdout io.Writer,
	name string, args ...string) ([]byte, error)

// agentBackend is the part of a job's life that differs between a mirror
// node running jobs against the repositories it serves and a runner that
// holds no repository and works on a leased clone.
type agentBackend interface {
	validate(work agentWork) string
	prepare(ctx context.Context, work agentWork) (*agentSession, string, error)
	publish(ctx context.Context, work agentWork, session *agentSession, workdir string) (bool, error)
	openPull(ctx context.Context, work agentWork, session *agentSession, summary string) (int, error)
	postReport(ctx context.Context, record agentJobRecord, body []byte) (int, []byte, error)
	materializeAttachments(ctx context.Context, work agentWork, workdir string) ([]string, error)
	// release runs after the job has published and reported its outcome.
	release(work agentWork, workdir string)
}

// mirrorAgentBackend is the original behaviour: claim per served repository
// and publish into the local bare repository.
type mirrorAgentBackend struct{ r *agentJobRunner }

func (m mirrorAgentBackend) validate(work agentWork) string { return m.r.validate(work.repo, work.job) }
func (m mirrorAgentBackend) prepare(ctx context.Context, work agentWork) (*agentSession, string, error) {
	box := m.r.mirrorBox
	if box == nil {
		return m.r.prepare(ctx, work)
	}
	if err := agentWorkOutsideNodeKey(box.workRoot, m.r.daemon.config.IdentityKey); err != nil {
		return nil, "", err
	}
	session, workdir, err := m.r.prepare(ctx, work)
	if err != nil {
		return session, workdir, err
	}
	if err := box.openSession(box.homeFor(workdir)); err != nil {
		return nil, workdir, fmt.Errorf("agent home: %v", err)
	}
	return session, workdir, nil
}
func (m mirrorAgentBackend) publish(ctx context.Context, work agentWork, session *agentSession, workdir string) (bool, error) {
	return m.r.publish(ctx, work.repo, session, workdir)
}
func (m mirrorAgentBackend) openPull(ctx context.Context, work agentWork, session *agentSession, summary string) (int, error) {
	return m.r.openPull(ctx, work.repo, session, summary)
}
func (m mirrorAgentBackend) postReport(ctx context.Context, record agentJobRecord, body []byte) (int, []byte, error) {
	return m.r.postMirrorReport(ctx, record, body)
}
func (m mirrorAgentBackend) materializeAttachments(ctx context.Context, work agentWork, workdir string) ([]string, error) {
	return m.r.materializeAttachments(ctx, work, workdir)
}
func (m mirrorAgentBackend) release(_ agentWork, workdir string) {
	if box := m.r.mirrorBox; box != nil && workdir != "" {
		box.closeSession(box.homeFor(workdir))
	}
}

func (r *agentJobRunner) be() agentBackend {
	if r.backend != nil {
		return r.backend
	}
	return mirrorAgentBackend{r}
}

func (r *agentJobRunner) limit() int {
	if r.maxRuns > 0 {
		return r.maxRuns
	}
	return agentMaxConcurrentRuns
}

func (r *agentJobRunner) runCommand() agentCommandRunner {
	if r.command != nil {
		return r.command
	}
	return runAgentCommandTo
}

func newAgentJobRunner(d *Daemon) *agentJobRunner {
	return &agentJobRunner{
		daemon:     d,
		dir:        filepath.Join(filepath.Dir(d.config.GatewayConfig), agentJobsDirectoryName),
		pending:    true,
		active:     map[string]bool{},
		leases:     map[int64]string{},
		waiting:    map[int64]agentWork{},
		stops:      map[string]context.CancelFunc{},
		lookPath:   exec.LookPath,
		getenv:     os.Getenv,
		cliVersion: cliVersion,
	}
}

// request marks the relay queue as changed; the next sync cycle claims.
func (r *agentJobRunner) request() {
	r.mu.Lock()
	r.pending = true
	r.mu.Unlock()
}

// due keeps the org-agents push as the fast path, with a bounded safety read
// for a frame lost while offline and for results the relay has not accepted.
func (r *agentJobRunner) due(now time.Time) bool {
	r.mu.Lock()
	defer r.mu.Unlock()
	if !r.pending && !r.lastCheck.IsZero() && now.Sub(r.lastCheck) < agentJobSafetyCheck {
		return false
	}
	r.pending = false
	r.lastCheck = now
	return true
}

func (r *agentJobRunner) statePath() string { return filepath.Join(r.dir, "state.json") }

func (r *agentJobRunner) loadLocked() {
	if r.loaded {
		return
	}
	r.loaded = true
	r.state = agentJobState{}
	if raw, err := os.ReadFile(r.statePath()); err == nil {
		_ = json.Unmarshal(raw, &r.state)
	}
	if r.state.Sessions == nil {
		r.state.Sessions = map[string]*agentSession{}
	}
	if r.state.Jobs == nil {
		r.state.Jobs = map[string]*agentJobRecord{}
	}
	// Nothing survives a restart mid-run. Say so on the lease instead of
	// leaving the website showing a run that will never finish.
	now := time.Now().UnixMilli()
	changed := false
	for _, record := range r.state.Jobs {
		if record.Status == "running" {
			record.Status = "failed"
			record.Result = ""
			record.Reason = "The mirror node restarted while this agent was running."
			record.Reported = false
			record.UpdatedAt = now
			changed = true
		}
	}
	for _, session := range r.state.Sessions {
		if session.Status == "running" {
			session.Status = "failed"
			session.FinishedAt = now
			session.LastError = "The mirror node restarted while this agent was running."
			changed = true
		}
	}
	if changed {
		r.saveLocked()
	}
}

func (r *agentJobRunner) saveLocked() {
	if len(r.state.Jobs) > agentJobRecordLimit {
		var reported []*agentJobRecord
		for _, record := range r.state.Jobs {
			if record.Reported {
				reported = append(reported, record)
			}
		}
		sort.Slice(reported, func(i, j int) bool { return reported[i].UpdatedAt < reported[j].UpdatedAt })
		for _, record := range reported {
			if len(r.state.Jobs) <= agentJobRecordLimit {
				break
			}
			delete(r.state.Jobs, strconv.FormatInt(record.JobID, 10))
		}
	}
	if err := os.MkdirAll(r.dir, 0700); err != nil {
		return
	}
	_ = writePrivateJSON(r.statePath(), r.state)
}

func (r *agentJobRunner) node() string { return r.daemon.gateway.Node.Name }

// claimRepositories names each served repository once: the relay keeps one
// queue per node and repository name, whatever owner aliases the gateway has.
func (r *agentJobRunner) claimRepositories(all []Repository) []Repository {
	preferred := r.daemon.config.CanonicalOwner
	if preferred == "" {
		preferred = r.daemon.config.PublishOwner
	}
	index := map[string]int{}
	var result []Repository
	for _, repo := range all {
		if !repo.Enabled || !repositoryPattern.MatchString(repo.Name) {
			continue
		}
		key := strings.ToLower(repo.Name)
		if at, seen := index[key]; seen {
			if strings.EqualFold(repo.Owner, preferred) &&
				!strings.EqualFold(result[at].Owner, preferred) {
				result[at] = repo
			}
			continue
		}
		index[key] = len(result)
		result = append(result, repo)
	}
	return result
}

func (r *agentJobRunner) hasCapacity() bool {
	r.mu.Lock()
	defer r.mu.Unlock()
	return r.running+len(r.waiting) < r.limit()
}

// freeSlots is how many more jobs this node could start or park right now.
func (r *agentJobRunner) freeSlots() int {
	r.mu.Lock()
	defer r.mu.Unlock()
	return max(0, r.limit()-r.running-len(r.waiting))
}

// reportPending re-sends every finished result the relay has not accepted.
func (r *agentJobRunner) reportPending(ctx context.Context) []string {
	r.mu.Lock()
	r.loadLocked()
	var unreported []agentJobRecord
	for _, record := range r.state.Jobs {
		if record.Status != "running" && !record.Reported {
			unreported = append(unreported, *record)
		}
	}
	r.mu.Unlock()
	var failures []string
	sort.Slice(unreported, func(i, j int) bool { return unreported[i].JobID < unreported[j].JobID })
	for i := range unreported {
		if err := r.report(ctx, unreported[i]); err != nil {
			failures = append(failures, fmt.Sprintf("result %d: %s", unreported[i].JobID, err))
		}
	}
	return failures
}

func (r *agentJobRunner) drain(ctx context.Context, repositories []Repository) error {
	failures := r.reportPending(ctx)
	for _, repo := range r.claimRepositories(repositories) {
		for attempt := 0; attempt < agentJobClaimsPerRepo && r.hasCapacity(); attempt++ {
			jobs, err := r.claim(ctx, repo)
			if err != nil {
				failures = append(failures, repo.Name+": "+err.Error())
				break
			}
			if len(jobs) == 0 {
				break
			}
			answered := true
			for _, raw := range jobs {
				if !r.accept(ctx, repo, raw) {
					answered = false
				}
			}
			// A started or parked job holds the repository's lease; the relay
			// hands out the next one only after that job reports.
			if !answered {
				break
			}
		}
	}
	if len(failures) != 0 {
		return errors.New(strings.Join(failures, "; "))
	}
	return nil
}

func (r *agentJobRunner) claim(ctx context.Context, repo Repository) ([]json.RawMessage, error) {
	endpoint, err := r.daemon.collaborationURL(
		Repository{Owner: r.node(), Name: repo.Name}, "org-agent-jobs")
	if err != nil {
		return nil, err
	}
	endpoint.RawQuery = r.daemon.signedCollaborationQuery().Encode()
	request, err := http.NewRequestWithContext(ctx, http.MethodGet, endpoint.String(), nil)
	if err != nil {
		return nil, err
	}
	request.Header.Set("Accept", "application/json")
	request.Header.Set("User-Agent", "ForkMesh-Mirror-Node/"+r.daemon.config.Version)
	response, err := agentJobHTTPClient.Do(request)
	if err != nil {
		return nil, err
	}
	defer response.Body.Close()
	body, err := io.ReadAll(io.LimitReader(response.Body, agentJobResponseLimit+1))
	if err != nil {
		return nil, err
	}
	if len(body) > agentJobResponseLimit {
		return nil, errors.New("agent job response is too large")
	}
	if response.StatusCode < 200 || response.StatusCode >= 300 {
		return nil, fmt.Errorf("agent job relay returned HTTP %d: %s",
			response.StatusCode, boundedText(body, 240))
	}
	var inbox struct {
		Jobs []json.RawMessage `json:"jobs"`
	}
	if err := json.Unmarshal(body, &inbox); err != nil {
		return nil, errors.New("agent job relay returned invalid JSON")
	}
	return inbox.Jobs, nil
}

// accept reports whether the job's lease was answered before returning, so
// the caller may claim the next job for the same repository.
func (r *agentJobRunner) accept(ctx context.Context, repo Repository, raw json.RawMessage) bool {
	var job agentJob
	if err := json.Unmarshal(raw, &job); err != nil {
		var minimal struct {
			JobID   int64  `json:"jobId"`
			LeaseID string `json:"leaseId"`
		}
		if json.Unmarshal(raw, &minimal) != nil || minimal.JobID <= 0 || minimal.LeaseID == "" {
			return false
		}
		job = agentJob{JobID: minimal.JobID, LeaseID: minimal.LeaseID}
	}
	if job.JobID <= 0 || job.LeaseID == "" {
		return false
	}
	key := strconv.FormatInt(job.JobID, 10)
	r.mu.Lock()
	r.loadLocked()
	if record, started := r.state.Jobs[key]; started {
		record.LeaseID = job.LeaseID
		copied := *record
		r.saveLocked()
		r.mu.Unlock()
		return r.report(ctx, copied) == nil
	}
	if _, inFlight := r.leases[job.JobID]; inFlight {
		r.leases[job.JobID] = job.LeaseID
		r.mu.Unlock()
		return false
	}
	r.mu.Unlock()

	if job.Kind == "stop" {
		return r.stop(ctx, repo, job)
	}
	work := agentWork{job: job, repo: repo}
	if reason := r.be().validate(work); reason != "" {
		return r.report(ctx, agentJobRecord{
			JobID: job.JobID, LeaseID: job.LeaseID, RepositoryName: repo.Name,
			Provider: job.Provider, Verdict: "rejected", Status: "rejected",
			Reason: reason,
		}) == nil
	}
	r.mu.Lock()
	r.leases[job.JobID] = job.LeaseID
	if r.active[job.SessionID] || r.running >= r.limit() {
		// A follow-up for a session that is still running waits for that run.
		r.waiting[job.JobID] = work
	} else {
		r.startLocked(work)
	}
	r.mu.Unlock()
	return false
}

// agentEnvelopeInvalid holds the checks every job must pass wherever it runs.
func agentEnvelopeInvalid(job agentJob) bool {
	return job.SessionID == "" || len(job.SessionID) > 128 ||
		strings.TrimSpace(job.Prompt) == "" || utf8.RuneCountInString(job.Prompt) > agentPromptLimit ||
		!agentWebsiteProviders[job.Provider] ||
		(job.Model != "" && !agentWebsiteModels[job.Model]) ||
		job.IssueNumber < 0 || job.IssueNumber > 10_000_000 ||
		(job.Kind != "start" && job.Kind != "continue" && job.Kind != "steer") ||
		!validAgentAttachments(job.Attachments)
}

func (r *agentJobRunner) stop(ctx context.Context, repo Repository, job agentJob) bool {
	if job.SessionID == "" || len(job.SessionID) > 128 ||
		!strings.EqualFold(job.RepositoryOwner, r.node()) ||
		!strings.EqualFold(job.RepositoryName, repo.Name) {
		return r.report(ctx, agentJobRecord{
			JobID: job.JobID, LeaseID: job.LeaseID, RepositoryName: repo.Name,
			Provider: job.Provider, Verdict: "rejected", Status: "rejected",
			Reason: "Malformed or unsafe job envelope.",
		}) == nil
	}
	r.mu.Lock()
	cancel := r.stops[job.SessionID]
	var dropped []agentWork
	for id, work := range r.waiting {
		if work.job.SessionID == job.SessionID {
			dropped = append(dropped, work)
			delete(r.waiting, id)
		}
	}
	r.mu.Unlock()
	if cancel != nil {
		cancel()
	}
	for _, work := range dropped {
		r.finalize(ctx, work, nil, "approved", "failed", "Stopped by the owner.",
			"Stopped by the owner before this follow-up ran.", "", false)
	}
	message := "Stopped by the owner."
	if cancel == nil {
		message = "Nothing was running on this node for the session."
	}
	return r.report(ctx, agentJobRecord{
		JobID: job.JobID, LeaseID: job.LeaseID, RepositoryName: repo.Name,
		SessionID: job.SessionID, Provider: job.Provider, Verdict: "approved",
		Status: "failed", Reason: message, Result: message,
	}) == nil
}

func (r *agentJobRunner) validate(repo Repository, job agentJob) string {
	switch {
	case agentEnvelopeInvalid(job),
		!strings.EqualFold(job.RepositoryOwner, r.node()),
		!strings.EqualFold(job.RepositoryName, repo.Name):
		return "Malformed or unsafe job envelope."
	}
	return ""
}

func (r *agentJobRunner) startLocked(work agentWork) {
	r.active[work.job.SessionID] = true
	r.running++
	r.runs.Add(1)
	go func() {
		defer r.runs.Done()
		defer r.finish(work.job.SessionID)
		r.run(work)
	}()
}

func (r *agentJobRunner) finish(sessionID string) {
	r.mu.Lock()
	delete(r.active, sessionID)
	r.running--
	ids := make([]int64, 0, len(r.waiting))
	for id := range r.waiting {
		ids = append(ids, id)
	}
	sort.Slice(ids, func(i, j int) bool { return ids[i] < ids[j] })
	for _, id := range ids {
		if r.running >= r.limit() {
			break
		}
		work := r.waiting[id]
		if r.active[work.job.SessionID] {
			continue
		}
		delete(r.waiting, id)
		r.startLocked(work)
	}
	r.pending = true
	r.mu.Unlock()
	r.daemon.requestSync()
}

func (r *agentJobRunner) wait() { r.runs.Wait() }

func (r *agentJobRunner) context() context.Context {
	if r.runCtx != nil {
		return r.runCtx
	}
	return context.Background()
}

func (r *agentJobRunner) run(work agentWork) {
	ctx := r.context()
	job := work.job
	reason := "Prompt dispatched."
	provider := job.Provider
	if job.Kind == "steer" {
		r.mu.Lock()
		if session := r.state.Sessions[job.SessionID]; session != nil {
			provider = session.Provider
		}
		r.mu.Unlock()
	}
	if availability := r.availability(provider); availability["binaryFound"] != true ||
		availability["loginState"] != "available" {
		switch availability["loginState"] {
		case "expired":
			work.failureKind = authFailureKind
		case "missing":
			work.failureKind = loginMissingKind
		}
		r.finalize(ctx, work, nil, "approved", "failed",
			fmt.Sprint(availability["message"]), "", "", false)
		return
	}
	model := job.Model
	if job.Kind == "steer" {
		r.mu.Lock()
		if session := r.state.Sessions[job.SessionID]; session != nil {
			model = session.Model
		}
		r.mu.Unlock()
	}
	if outdated := r.outdatedCLI(provider, model); outdated != nil {
		// The reason becomes the session's diagnostic; nothing ran, so there
		// is no result or transcript to repeat it in.
		r.finalize(ctx, work, nil, "approved", "failed", outdated.Error(), "", "", false)
		return
	}
	backend := r.be()
	session, workdir, err := backend.prepare(ctx, work)
	if err != nil {
		r.finalize(ctx, work, nil, "approved", "failed", err.Error(), "", "", false)
		backend.release(work, workdir)
		return
	}
	defer backend.release(work, workdir)
	// Before the running report: the relay deletes its copy of the images
	// once the node reports on the job.
	attachmentPaths, err := backend.materializeAttachments(ctx, work, workdir)
	if err != nil {
		message := "Could not fetch the attached images, so the agent did not run: " + err.Error()
		r.finalize(ctx, work, nil, "approved", "failed", message, message, "", false)
		return
	}
	now := time.Now().UnixMilli()
	session.Status = "running"
	session.StartedAt = now
	session.FinishedAt = 0
	session.LastError = ""
	r.finalize(ctx, work, session, "approved", "running", reason, "", "", false)

	runCtx, cancel := context.WithTimeout(ctx, agentJobTimeout(job, time.Now()))
	gitDir := work.repo.GitDir
	if gitDir == "" {
		gitDir = filepath.Join(workdir, ".git")
	}
	r.mu.Lock()
	if r.stops == nil {
		r.stops = map[string]context.CancelFunc{}
	}
	r.stops[job.SessionID] = cancel
	r.mu.Unlock()
	tap, stopStream := r.startStream(ctx, work)
	live := newAgentLiveReporter(ctx, func(sendCtx context.Context, text string) bool {
		return r.reportLive(sendCtx, job, text)
	})
	outcome, runErr := r.runProvider(withAgentStreamTap(runCtx, tap), session, workdir, r.prompt(ctx, work, session, gitDir)+agentAttachmentPromptBlock(attachmentPaths), live.update)
	live.stop()
	stopStream()
	r.mu.Lock()
	delete(r.stops, job.SessionID)
	r.mu.Unlock()
	stopped := errors.Is(runCtx.Err(), context.Canceled)
	cancel()
	if stopped && runErr == nil {
		runErr = errors.New("stopped by the owner")
	}
	if outcome.providerSession != "" {
		session.ProviderSession = outcome.providerSession
	}
	session.NumTurns += outcome.numTurns
	session.DurationMs += outcome.durationMs
	session.CostUsd += outcome.costUsd
	session.PromptTokens += outcome.promptTokens
	session.CompletionTokens += outcome.completionTokens
	result := outcome.text
	status := "completed"
	var problems []string
	if runErr != nil {
		status = "failed"
		problems = append(problems, runErr.Error())
		if r.recordAuthFailure(session.Provider, job.JobID, runErr.Error()) {
			work.failureKind = authFailureKind
			problems[0] = r.authStatus(session.Provider).Message + " (" + runErr.Error() + ")"
			log.Printf("agent: %s sign-in expired; not claiming %s jobs until it is signed in again", session.Provider, session.Provider)
		}
	}
	// Whatever the agent managed to write is published even when the run
	// failed, so the branch shows exactly what happened.
	if pushed, err := backend.publish(ctx, work, session, workdir); err != nil {
		status = "failed"
		problems = append(problems, "publish branch: "+err.Error())
	} else if pushed && session.CreatePr && session.PrNumber == 0 && runErr == nil {
		if number, err := backend.openPull(ctx, work, session, result); err != nil {
			problems = append(problems, "open pull request: "+err.Error())
		} else {
			session.PrNumber = number
		}
	}
	session.Status = status
	session.FinishedAt = time.Now().UnixMilli()
	session.LastError = boundedText([]byte(strings.Join(problems, "; ")), 1000)
	// The transcript is the run as it happened, and the error is reported
	// once, as lastError: the clients show lastError (and the relay's
	// diagnostic built from it) beside the transcript, so copying it into
	// the transcript or the result showed the same failure two or three times.
	r.finalize(ctx, work, session, "approved", status, reason, result, outcome.transcript, false)
}

// finalize records the job's newest state durably, then reports it.
func (r *agentJobRunner) finalize(ctx context.Context, work agentWork, session *agentSession,
	verdict, status, reason, result, transcript string, gateUnready bool) {
	job := work.job
	key := strconv.FormatInt(job.JobID, 10)
	r.mu.Lock()
	r.loadLocked()
	record := r.state.Jobs[key]
	if record == nil {
		record = &agentJobRecord{JobID: job.JobID, RepositoryName: work.repo.Name,
			SessionID: job.SessionID, Provider: job.Provider}
		r.state.Jobs[key] = record
	}
	if lease, ok := r.leases[job.JobID]; ok {
		record.LeaseID = lease
		delete(r.leases, job.JobID)
	} else if record.LeaseID == "" {
		record.LeaseID = job.LeaseID
	}
	if session != nil {
		stored := *session
		r.state.Sessions[job.SessionID] = &stored
		record.Provider = session.Provider
	}
	record.Verdict, record.Status, record.GateUnready = verdict, status, gateUnready
	record.FailureKind = ""
	if status == "failed" {
		record.FailureKind = work.failureKind
	}
	record.Reason = boundedRunes(reason, 240)
	record.Result = boundedRunes(result, agentResultLimit)
	record.Transcript = tailRunes(transcript, agentResultLimit)
	record.Reported = false
	record.Conflicts = 0
	record.UpdatedAt = time.Now().UnixMilli()
	copied := *record
	r.saveLocked()
	r.mu.Unlock()
	_ = r.report(ctx, copied)
	if status == "running" {
		// The relay hands out this repository's next job only after this one
		// reports, so claim again straight away.
		r.request()
		r.daemon.requestSync()
	}
}

// agentJobTimeout bounds one provider run: the fixed ceiling, the job's own
// timeout, and — for a runner lease — the push grant's expiry less a margin
// to publish the branch before the grant lapses.
func agentJobTimeout(job agentJob, now time.Time) time.Duration {
	timeout := agentRunTimeout
	if job.TimeoutMs > 0 {
		timeout = min(timeout, time.Duration(job.TimeoutMs)*time.Millisecond)
	}
	if job.GrantExpiresAt > 0 {
		left := time.UnixMilli(job.GrantExpiresAt).Sub(now) - agentPublishMargin
		timeout = min(timeout, max(left, time.Second))
	}
	return timeout
}

// agentPublishMargin is kept back from a grant's lifetime for the push.
const agentPublishMargin = 5 * time.Minute

func (r *agentJobRunner) report(ctx context.Context, record agentJobRecord) error {
	localAgentID := int64(0)
	agentInfo := map[string]any{}
	r.mu.Lock()
	session := r.state.Sessions[record.SessionID]
	if session != nil && record.Verdict == "approved" {
		localAgentID = session.LocalAgentID
		agentInfo = session.info(r.node())
	}
	r.mu.Unlock()
	availability := r.availability(record.Provider)
	if record.GateUnready {
		availability = r.availability("claude-code")
		availability["message"] = record.Reason
	}
	body, err := json.Marshal(map[string]any{
		"leaseId":         record.LeaseID,
		"securityVerdict": record.Verdict,
		"securityReason":  record.Reason,
		"status":          record.Status,
		"transcript":      record.Transcript,
		"localAgentId":    localAgentID,
		"result":          record.Result,
		"agentInfo":       agentInfo,
		"availability":    availability,
		"failureKind":     record.FailureKind,
	})
	if err != nil {
		return err
	}
	statusCode, reply, err := r.be().postReport(ctx, record, body)
	if err != nil {
		return err
	}
	accepted := statusCode >= 200 && statusCode < 300
	conflict := statusCode == http.StatusConflict
	if record.JobID > 0 && record.Status != "running" && (accepted || conflict) {
		r.mu.Lock()
		if stored := r.state.Jobs[strconv.FormatInt(record.JobID, 10)]; stored != nil &&
			stored.Status == record.Status && stored.LeaseID == record.LeaseID {
			if accepted {
				stored.Reported = true
			} else if stored.Conflicts++; stored.Conflicts >= agentReportConflictLimit {
				stored.Reported = true
			}
			r.saveLocked()
		}
		r.mu.Unlock()
	}
	if !accepted {
		return fmt.Errorf("agent result returned HTTP %d: %s",
			statusCode, boundedText(reply, 240))
	}
	return nil
}

// postMirrorReport posts a result on the node's own per-repository queue.
func (r *agentJobRunner) postMirrorReport(ctx context.Context, record agentJobRecord, body []byte) (int, []byte, error) {
	return r.postMirrorJob(ctx, record.RepositoryName, record.JobID, "result", body)
}

func (r *agentJobRunner) postMirrorJob(ctx context.Context, repository string, jobID int64, action string, body []byte) (int, []byte, error) {
	endpoint, err := r.daemon.collaborationURL(
		Repository{Owner: r.node(), Name: repository},
		"org-agent-jobs/"+strconv.FormatInt(jobID, 10)+"/"+action)
	if err != nil {
		return 0, nil, err
	}
	endpoint.RawQuery = r.daemon.signedCollaborationQuery().Encode()
	request, err := http.NewRequestWithContext(ctx, http.MethodPost, endpoint.String(), bytes.NewReader(body))
	if err != nil {
		return 0, nil, err
	}
	request.Header.Set("Content-Type", "application/json")
	request.Header.Set("Accept", "application/json")
	request.Header.Set("User-Agent", "ForkMesh-Mirror-Node/"+r.daemon.config.Version)
	response, err := agentJobHTTPClient.Do(request)
	if err != nil {
		return 0, nil, err
	}
	defer response.Body.Close()
	reply, _ := io.ReadAll(io.LimitReader(response.Body, 64<<10))
	return response.StatusCode, reply, nil
}

func (s *agentSession) info(node string) map[string]any {
	if s.RepositoryOwner != "" {
		node = s.RepositoryOwner
	}
	return map[string]any{
		"owner": node, "repository": s.RepositoryName, "provider": s.Provider,
		"model": s.Model, "mode": "headless", "status": s.Status,
		"issueNumber": s.IssueNumber, "prNumber": s.PrNumber, "createPr": s.CreatePr,
		"branchName": s.Branch, "baseRef": s.BaseRef, "baseBranch": s.BaseBranch,
		"merged": false, "createdAt": s.CreatedAt, "startedAt": s.StartedAt,
		"finishedAt": s.FinishedAt, "promptTokens": s.PromptTokens,
		"completionTokens": s.CompletionTokens,
		"totalTokens":      s.PromptTokens + s.CompletionTokens,
		"numTurns":         s.NumTurns, "durationMs": s.DurationMs,
		"costUsd": s.CostUsd, "lastError": s.LastError,
		"diffStats": s.DiffStats,
	}
}

type agentDiffStats struct {
	Files   int `json:"files"`
	Added   int `json:"added"`
	Removed int `json:"removed"`
	Ahead   int `json:"ahead"`
}

func parseNumstat(output string) agentDiffStats {
	var stats agentDiffStats
	for _, line := range strings.Split(output, "\n") {
		fields := strings.SplitN(strings.TrimSpace(line), "\t", 3)
		if len(fields) != 3 {
			continue
		}
		stats.Files++
		if added, err := strconv.Atoi(fields[0]); err == nil {
			stats.Added += added
		}
		if removed, err := strconv.Atoi(fields[1]); err == nil {
			stats.Removed += removed
		}
	}
	return stats
}

// availability mirrors the desktop's localCliAvailability for the service
// account: the CLI must be on PATH and hold a device login or an API key.
func (r *agentJobRunner) availability(provider string) map[string]any {
	if provider == "local-llama" {
		return r.localLlamaAvailability()
	}
	codex := provider == "codex"
	label, binary := "Claude Code", "claude"
	if codex {
		label, binary = "Codex", "codex"
		provider = "codex"
	} else {
		provider = "claude-code"
	}
	_, lookErr := r.lookPath(binary)
	binaryFound := lookErr == nil
	source, _ := r.credential(codex)
	loginState := "missing"
	message := label + " is installed and authorized via " + source + "."
	switch {
	case !binaryFound:
		message = label + " binary is missing on this mirror."
		if source != "" {
			loginState = "available"
		}
	case source == "":
		message = label + " login was not found on this mirror."
		if auth := r.authStatus(provider); auth.State == "expired" {
			loginState, message = "expired", auth.Message
		}
	default:
		loginState = "available"
		if auth := r.authStatus(provider); auth.State == "expired" {
			loginState, message = "expired", auth.Message
		}
	}
	return map[string]any{
		"provider": provider, "binaryFound": binaryFound, "loginState": loginState,
		"message": message, "credentialSource": source,
	}
}

func (r *agentJobRunner) localLlamaAvailability() map[string]any {
	_, codexErr := r.lookPath("codex")
	_, ollamaErr := r.lookPath("ollama")
	found := codexErr == nil && ollamaErr == nil
	loginState, message := "available", "Gemma runs through Codex against the local Ollama on this mirror."
	switch {
	case codexErr != nil:
		loginState, message = "missing", "Codex binary is missing on this mirror; it runs Gemma through Ollama."
	case ollamaErr != nil:
		loginState, message = "missing", "Ollama is not installed on this mirror."
	}
	source := ""
	if found {
		source = "Ollama"
	}
	return map[string]any{
		"provider": "local-llama", "binaryFound": found, "loginState": loginState,
		"message": message, "credentialSource": source,
	}
}

// prepare resolves the job's session and branch and checks it out in the
// session's scratch clone. The clone path is stable per session so a Claude
// Code follow-up can resume the conversation it recorded for that directory.
func (r *agentJobRunner) prepare(ctx context.Context, work agentWork) (*agentSession, string, error) {
	job, repo := work.job, work.repo
	r.mu.Lock()
	var existing *agentSession
	if stored := r.state.Sessions[job.SessionID]; stored != nil {
		copied := *stored
		existing = &copied
	}
	r.mu.Unlock()
	defaultBranch := gitValue(ctx, repo.GitDir, "symbolic-ref", "--short", "HEAD")
	if defaultBranch == "" {
		defaultBranch = "main"
	}
	title := strings.TrimSpace(job.Title)
	if title == "" {
		title = strings.TrimSpace(strings.SplitN(strings.TrimSpace(job.Prompt), "\n", 2)[0])
	}
	title = boundedRunes(title, 160)
	now := time.Now().UnixMilli()
	var session *agentSession
	switch job.Kind {
	case "start":
		base := defaultBranch
		if requested := validMergeBase(job.BaseBranch); requested != "" {
			base = requested
		}
		baseRef := gitRefValue(ctx, repo.GitDir, "refs/heads/"+base)
		if baseRef == "" {
			return nil, "", fmt.Errorf("the repository has no branch %s to start the agent from", base)
		}
		branch := fmt.Sprintf("agent/web-%d-%s", job.JobID, agentSlug(title))
		if named := validAgentBranch(job.Branch); named != "" {
			branch = named
		}
		session = &agentSession{
			SessionID: job.SessionID, LocalAgentID: job.JobID, RepositoryName: repo.Name,
			Provider: job.Provider, Model: job.Model, Title: title,
			Branch:     branch,
			BaseBranch: base, BaseRef: baseRef, IssueNumber: job.IssueNumber,
			CreatePr: job.CreatePr, CreatedAt: now,
		}
	case "continue":
		branch := validPullBranch(job.Branch)
		if branch == "" {
			return nil, "", errors.New("the handoff named no branch to continue")
		}
		if !r.awaitHandoffBranch(ctx, repo.GitDir, branch) {
			return nil, "", fmt.Errorf("branch %s has not reached this node yet, so the moved session has no history to continue", branch)
		}
		base := validPullBranch(job.BaseBranch)
		if base == "" {
			base = defaultBranch
		}
		session = &agentSession{
			SessionID: job.SessionID, LocalAgentID: job.JobID, RepositoryName: repo.Name,
			Provider: job.Provider, Model: job.Model, Title: title, Branch: branch,
			BaseBranch: base, BaseRef: gitRefValue(ctx, repo.GitDir, "refs/heads/"+base),
			IssueNumber: job.IssueNumber, CreatePr: job.CreatePr, CreatedAt: now,
		}
	case "steer":
		if existing == nil || !strings.EqualFold(existing.RepositoryName, repo.Name) {
			return nil, "", errors.New("the scoped agent session is unavailable on this node")
		}
		session = existing
	default:
		return nil, "", errors.New("unknown job kind")
	}
	workdir := filepath.Join(r.dir, "work", agentSessionDirectory(job.SessionID))
	if err := prepareAgentWorkspace(ctx, repo.GitDir, workdir, session.Branch,
		session.BaseBranch, r.node()); err != nil {
		return nil, "", err
	}
	return session, workdir, nil
}

// awaitHandoffBranch reports whether the moved session's branch is here,
// nudging a sync while it waits (see agentHandoffBranchWait).
func (r *agentJobRunner) awaitHandoffBranch(ctx context.Context, gitDir, branch string) bool {
	deadline := time.Now().Add(agentHandoffBranchWait)
	for {
		if gitRefValue(ctx, gitDir, "refs/heads/"+branch) != "" {
			return true
		}
		if !time.Now().Before(deadline) {
			return false
		}
		if r.daemon != nil {
			r.daemon.requestSync()
		}
		select {
		case <-ctx.Done():
			return false
		case <-time.After(agentHandoffBranchPoll):
		}
	}
}

func agentSessionDirectory(sessionID string) string {
	digest := sha256.Sum256([]byte(sessionID))
	return hex.EncodeToString(digest[:12])
}

func agentSlug(title string) string {
	slug := strings.Trim(agentSlugPattern.ReplaceAllString(strings.ToLower(title), "-"), "-")
	if len(slug) > 40 {
		slug = strings.Trim(slug[:40], "-")
	}
	if slug == "" {
		slug = "task"
	}
	return slug
}

func boundedRunes(value string, maximum int) string {
	if utf8.RuneCountInString(value) <= maximum {
		return value
	}
	return string([]rune(value)[:maximum])
}

func agentGit(ctx context.Context, dir string, args ...string) (string, error) {
	return agentGitEnv(ctx, dir, nil, args...)
}

func agentGitEnv(ctx context.Context, dir string, env []string, args ...string) (string, error) {
	command := exec.CommandContext(ctx, "git", args...)
	command.Dir = dir
	command.Env = append(append(os.Environ(), "GIT_TERMINAL_PROMPT=0"), env...)
	output, err := command.CombinedOutput()
	if err != nil {
		return "", fmt.Errorf("git %s: %s", args[0], boundedText(output, 240))
	}
	return strings.TrimSpace(string(output)), nil
}

func (r *agentJobRunner) prompt(ctx context.Context, work agentWork, session *agentSession, gitDir string) string {
	prompt := work.job.Prompt
	if work.job.Kind == "steer" && session.ProviderSession == "" && session.Branch != "" {
		prompt = agentResumePreamble(session) + prompt
	}
	if work.job.Kind != "start" || session.IssueNumber <= 0 {
		return prompt
	}
	context := fmt.Sprintf("This task is for issue #%d of the repository.", session.IssueNumber)
	if issue := agentIssueText(ctx, gitDir, session.BaseRef, session.IssueNumber); issue != "" {
		context += "\n\n" + issue
	}
	return context + "\n\n" + prompt
}

func agentResumePreamble(session *agentSession) string {
	text := "You are continuing earlier work on branch " + session.Branch
	if session.BaseBranch != "" {
		text += " (cut from " + session.BaseBranch + ")"
	}
	text += ". Its commits are checked out"
	if session.BaseBranch != "" {
		text += "; run `git log " + session.BaseBranch + "..HEAD` to see them"
	}
	text += "."
	if title := strings.TrimSpace(session.Title); title != "" {
		text += " The task so far: " + title + "."
	}
	return text + "\n\n"
}

// agentIssueText reads the issue record committed at the session's base.
func agentIssueText(ctx context.Context, gitDir, ref string, number int) string {
	if ref == "" {
		return ""
	}
	want := strconv.Itoa(number)
	for _, path := range gitLines(ctx, gitDir, "ls-tree", "-r", "--name-only", ref, "--", ".forkmesh/issues") {
		if !strings.HasSuffix(path, ".json") {
			continue
		}
		named := false
		for _, segment := range strings.Split(strings.TrimSuffix(path, ".json"), "/") {
			if segment == want {
				named = true
			}
		}
		if !named {
			continue
		}
		raw, err := exec.CommandContext(ctx, "git", "--git-dir="+gitDir, "show", ref+":"+path).Output()
		if err != nil {
			continue
		}
		var issue map[string]any
		decoder := json.NewDecoder(bytes.NewReader(raw))
		decoder.UseNumber()
		if decoder.Decode(&issue) != nil || issueNumber(issue["number"]) != number {
			continue
		}
		text := "Issue: " + stringValue(issue["title"])
		if events, _ := issue["events"].([]any); len(events) > 0 {
			if first, _ := events[0].(map[string]any); first != nil {
				if body := stringValue(first["body"]); body != "" {
					text += "\n\n" + boundedRunes(body, 4000)
				}
			}
		}
		return text
	}
	return ""
}

func (r *agentJobRunner) runProvider(ctx context.Context, session *agentSession, workdir, prompt string, live func(string)) (agentRunOutcome, error) {
	outcome, err := r.runProviderOnce(ctx, session, workdir, prompt, live)
	var outdated *agentCLIOutdated
	if errors.As(err, &outdated) {
		r.floor.learn(session.Provider, outdated)
	}
	return outcome, err
}

func (r *agentJobRunner) runProviderOnce(ctx context.Context, session *agentSession, workdir, prompt string, live func(string)) (agentRunOutcome, error) {
	command := r.runCommand()
	if session.Provider == "codex" || session.Provider == "local-llama" {
		return runCodexAgent(ctx, command, r.node(), session, workdir, prompt)
	}
	outcome, err := runClaudeAgent(ctx, command, r.node(), session, workdir, prompt, session.ProviderSession, live)
	var outdated *agentCLIOutdated
	if err != nil && session.ProviderSession != "" && ctx.Err() == nil && !errors.As(err, &outdated) {
		// A conversation the CLI no longer has is not a reason to drop the
		// follow-up: the branch still carries the work, so start fresh on it.
		// An outdated CLI fails the same way however it is started.
		return runClaudeAgent(ctx, command, r.node(), session, workdir, prompt, "", live)
	}
	return outcome, err
}

// outdatedCLI is the refusal a run for provider and model already met on this
// host's CLI version, or nil. Retrying cannot help (the binary is the same one
// until an operator updates the host), so the run fails at once with the same
// message instead of cloning and failing again.
func (r *agentJobRunner) outdatedCLI(provider, model string) *agentCLIOutdated {
	binary := "claude"
	if provider == "codex" || provider == "local-llama" {
		binary = "codex"
	}
	return r.floor.check(provider, model, func() string { return r.cliVersion(binary) })
}

// claudeResult is the run's closing record: what `--output-format json`
// prints on its own and what stream-json emits as its last, "result", event.
type claudeResult struct {
	Result       string  `json:"result"`
	SessionID    string  `json:"session_id"`
	IsError      bool    `json:"is_error"`
	NumTurns     int64   `json:"num_turns"`
	DurationMs   int64   `json:"duration_ms"`
	TotalCostUsd float64 `json:"total_cost_usd"`
	Usage        struct {
		InputTokens         int64 `json:"input_tokens"`
		OutputTokens        int64 `json:"output_tokens"`
		CacheReadTokens     int64 `json:"cache_read_input_tokens"`
		CacheCreationTokens int64 `json:"cache_creation_input_tokens"`
	} `json:"usage"`
}

// claudeStreamEvent is one line of `claude -p --output-format stream-json`:
// an assistant turn as it happens, a tool result, or (last) the result record.
type claudeStreamEvent struct {
	Type    string `json:"type"`
	Message struct {
		Content json.RawMessage `json:"content"`
	} `json:"message"`
	// An assistant message the CLI wrote for a failed API call ("API Error:
	// 400 ...") carries isApiErrorMessage and/or an error code; a result that
	// ended in error lists its reasons in errors. Raw, so a shape this code
	// does not expect never costs the whole event.
	IsAPIErrorMessage bool            `json:"isApiErrorMessage"`
	Error             json.RawMessage `json:"error"`
	Errors            json.RawMessage `json:"errors"`
	claudeResult
}

// claudeStream folds the CLI's event lines into the run's result and a
// readable transcript - what the agent said and which tools it ran - so the
// website's session page can show the run rather than only its last message.
// The transcript keeps its most recent entries within the report limit; a
// long run's opening is the part a reader can most afford to lose.
type claudeStream struct {
	result  claudeResult
	parsed  bool
	entries []string
	size    int
	dropped bool
	// apiError is the last assistant message that reported a failed API
	// call; resultErrors are the result event's listed reasons.
	apiError     string
	resultErrors []string
	onChange     func(string)
}

func (s *claudeStream) line(raw []byte) {
	raw = bytes.TrimSpace(raw)
	if len(raw) == 0 || raw[0] != '{' {
		return
	}
	var event claudeStreamEvent
	if json.Unmarshal(raw, &event) != nil {
		return
	}
	switch event.Type {
	case "assistant":
		var blocks []struct {
			Type  string          `json:"type"`
			Text  string          `json:"text"`
			Name  string          `json:"name"`
			Input json.RawMessage `json:"input"`
		}
		if json.Unmarshal(event.Message.Content, &blocks) != nil {
			return
		}
		flagged := event.IsAPIErrorMessage || claudeErrorText(event.Error) != ""
		for _, block := range blocks {
			switch block.Type {
			case "text":
				text := strings.TrimSpace(block.Text)
				if text != "" && (flagged || strings.HasPrefix(text, "API Error")) {
					s.apiError = text
				}
				s.add(text)
			case "tool_use":
				if block.Name != "" {
					s.add("\u25b8 " + block.Name + claudeToolSummary(block.Input))
				}
			}
		}
	case "result":
		s.result, s.parsed = event.claudeResult, true
		s.resultErrors = claudeErrorList(event.Errors)
	case "":
		// A bare result object: a CLI still printing `--output-format json`.
		if event.Result != "" || event.SessionID != "" {
			s.result, s.parsed = event.claudeResult, true
		}
	}
}

func (s *claudeStream) add(entry string) {
	if entry == "" {
		return
	}
	s.entries = append(s.entries, entry)
	s.size += utf8.RuneCountInString(entry) + 2
	for len(s.entries) > 1 && s.size > 2*agentResultLimit {
		s.size -= utf8.RuneCountInString(s.entries[0]) + 2
		s.entries = s.entries[1:]
		s.dropped = true
	}
	if s.onChange != nil {
		s.onChange(s.transcript())
	}
}

// claudeErrorText reads an error field that may be a string or an object with
// a message.
func claudeErrorText(raw json.RawMessage) string {
	var text string
	if json.Unmarshal(raw, &text) == nil {
		return strings.TrimSpace(text)
	}
	var object struct {
		Message string `json:"message"`
	}
	if json.Unmarshal(raw, &object) == nil {
		return strings.TrimSpace(object.Message)
	}
	return ""
}

// claudeErrorList reads a result's errors: strings or {message} objects.
func claudeErrorList(raw json.RawMessage) []string {
	var items []json.RawMessage
	if json.Unmarshal(raw, &items) != nil {
		if text := claudeErrorText(raw); text != "" {
			return []string{text}
		}
		return nil
	}
	var texts []string
	for _, item := range items {
		if text := claudeErrorText(item); text != "" {
			texts = append(texts, text)
		}
	}
	return texts
}

// transcript is the run's conversation, ending on its closing message.
func (s *claudeStream) transcript() string {
	text := strings.Join(s.entries, "\n\n")
	final := strings.TrimSpace(s.result.Result)
	switch {
	case text == "":
		text = final
	case final != "" && !strings.HasSuffix(text, final):
		text += "\n\n" + final
	}
	if s.dropped && text != "" {
		text = "\u2026\n\n" + text
	}
	return tailRunes(text, agentResultLimit)
}

// claudeToolSummary names what a tool call was about - the command, the
// path, the pattern - the way the desktop's transcript captions tool rows.
func claudeToolSummary(input json.RawMessage) string {
	var fields map[string]any
	if json.Unmarshal(input, &fields) != nil {
		return ""
	}
	for _, key := range []string{"command", "file_path", "pattern", "query", "url", "path", "description", "prompt"} {
		value, _ := fields[key].(string)
		value = strings.TrimSpace(strings.SplitN(strings.TrimSpace(value), "\n", 2)[0])
		if value != "" {
			return ": " + boundedRunes(value, 200)
		}
	}
	return ""
}

// tailRunes keeps the last maximum runes of value, marking the cut.
func tailRunes(value string, maximum int) string {
	if utf8.RuneCountInString(value) <= maximum {
		return value
	}
	runes := []rune(value)
	return "\u2026" + string(runes[len(runes)-maximum+1:])
}

// defaultClaudeModel is the model a Claude Code job runs on when the job
// names none (the user's default: Opus 5.5).
const defaultClaudeModel = "claude-opus-5-5"

func runClaudeAgent(ctx context.Context, command agentCommandRunner, node string, session *agentSession, workdir, prompt, resume string, live func(string)) (agentRunOutcome, error) {
	// stream-json (which print mode only allows with --verbose) narrates the
	// run event by event, so the transcript is read as it happens instead of
	// the whole output being held to find one result object at its end.
	args := []string{"-p", "--output-format", "stream-json", "--verbose", "--dangerously-skip-permissions"}
	model := session.Model
	if model == "" {
		model = defaultClaudeModel
	}
	args = append(args, "--model", model)
	if resume != "" {
		args = append(args, "--resume", resume)
	}
	stream := &claudeStream{onChange: live}
	sink := &lineSink{fn: stream.line}
	if tap := agentStreamTapFrom(ctx); tap != nil {
		sink.fn = func(line []byte) {
			stream.line(line)
			tap(line)
		}
	}
	stderr, err := command(ctx, workdir, prompt, sink, "claude", args...)
	sink.flush()
	result := stream.result
	outcome := agentRunOutcome{
		text: result.Result, transcript: stream.transcript(),
		providerSession: result.SessionID, numTurns: result.NumTurns,
		durationMs: result.DurationMs, costUsd: result.TotalCostUsd,
		promptTokens: result.Usage.InputTokens + result.Usage.CacheReadTokens +
			result.Usage.CacheCreationTokens,
		completionTokens: result.Usage.OutputTokens,
	}
	if err := ctx.Err(); err != nil && errors.Is(err, context.DeadlineExceeded) {
		return outcome, agentExitError("Claude Code", err, "")
	}
	return outcome, claudeRunError(stream, stderr, err, node, model)
}

func runCodexAgent(ctx context.Context, command agentCommandRunner, node string, session *agentSession, workdir, prompt string) (agentRunOutcome, error) {
	// The last-message file sits beside the workdir's checkout so a job
	// sandbox that runs as its own user can still write it.
	scratch, err := os.MkdirTemp(filepath.Dir(workdir), "forkmesh-agent-codex-*")
	if err != nil {
		return agentRunOutcome{}, err
	}
	defer removeTreeUnder(filepath.Dir(workdir), scratch)
	// The job user owns the workdir; let it write its last message here.
	messageOwner := os.Geteuid()
	if info, err := os.Stat(workdir); err == nil {
		if stat, ok := info.Sys().(*syscall.Stat_t); ok && int(stat.Uid) != os.Geteuid() {
			messageOwner = int(stat.Uid)
			_ = os.Lchown(scratch, int(stat.Uid), int(stat.Gid))
		}
	}
	lastMessage := filepath.Join(scratch, "last-message.txt")
	args := []string{"--dangerously-bypass-approvals-and-sandbox", "-c", `developer_instructions="Run autonomously until the task is complete. Make routine implementation choices using your best judgment and reasonable assumptions. Do not ask the user for approvals, confirmations, or optional choices. Ask a concise question only when essential information is missing and no reasonable assumption can resolve it; continue independent work while waiting. Preserve repository data and follow repository instructions."`}
	model := session.Model
	if model == "" && session.Provider == "local-llama" {
		model = defaultGemmaModel
	}
	if model != "" {
		args = append(args, "-m", model)
	}
	// danger-full-access: workspace-write also blocks loopback sockets, so the
	// agent could not serve a local port (Playwright webServer, dev servers).
	args = append(args, "exec")
	if session.Provider == "local-llama" {
		args = append(args, "--oss")
	}
	args = append(args, "-o", lastMessage, "-")
	started := time.Now()
	stdout := &cappedBuffer{limit: agentOutputLimit}
	stderr, runErr := command(ctx, workdir, prompt, stdout, "codex", args...)
	text, _ := readOwnedFile(lastMessage, messageOwner, agentOutputLimit)
	outcome := agentRunOutcome{
		text:       strings.TrimSpace(string(text)),
		durationMs: time.Since(started).Milliseconds(),
	}
	if err := ctx.Err(); runErr != nil && errors.Is(err, context.DeadlineExceeded) {
		return outcome, agentExitError("Codex", err, "")
	}
	return outcome, codexRunError(stdout.Bytes(), stderr, runErr, node, session.Model,
		func() string { return cliVersion("codex") })
}

type cappedBuffer struct {
	bytes.Buffer
	limit int
}

func (b *cappedBuffer) Write(p []byte) (int, error) {
	if room := b.limit - b.Len(); room > 0 {
		if len(p) > room {
			b.Buffer.Write(p[:room])
		} else {
			b.Buffer.Write(p)
		}
	}
	return len(p), nil
}

// lineSink hands each complete line written to it to fn as it arrives, so a
// long event stream is read in bounded memory instead of captured whole. A
// line past agentStreamLineLimit is dropped; the lines after it still arrive.
type lineSink struct {
	fn      func([]byte)
	partial []byte
	dropped bool
}

func (s *lineSink) Write(p []byte) (int, error) {
	rest := p
	for len(rest) > 0 {
		end := bytes.IndexByte(rest, '\n')
		if end < 0 {
			s.keep(rest)
			break
		}
		s.keep(rest[:end])
		s.flush()
		rest = rest[end+1:]
	}
	return len(p), nil
}

func (s *lineSink) keep(p []byte) {
	if s.dropped {
		return
	}
	if len(s.partial)+len(p) > agentStreamLineLimit {
		s.partial, s.dropped = s.partial[:0], true
		return
	}
	s.partial = append(s.partial, p...)
}

// flush delivers the line collected so far (the stream's last line has no
// newline after it) and starts the next one.
func (s *lineSink) flush() {
	if !s.dropped && len(s.partial) > 0 {
		s.fn(s.partial)
	}
	s.partial, s.dropped = s.partial[:0], false
}

// runAgentCommand runs a provider CLI in its own process group so a timeout
// or shutdown also stops the tools the agent spawned.
func runAgentCommand(ctx context.Context, dir, stdin, name string, args ...string) ([]byte, []byte, error) {
	stdout := &cappedBuffer{limit: agentOutputLimit}
	stderr, err := runAgentCommandTo(ctx, dir, stdin, stdout, name, args...)
	return stdout.Bytes(), stderr, err
}

// runAgentCommandTo is runAgentCommand writing standard output to stdout as
// it is produced.
func runAgentCommandTo(ctx context.Context, dir, stdin string, stdout io.Writer, name string, args ...string) ([]byte, error) {
	command := exec.CommandContext(ctx, name, args...)
	command.Dir = dir
	command.Env = append(os.Environ(), "GIT_TERMINAL_PROMPT=0")
	command.Stdin = strings.NewReader(stdin)
	stderr := &cappedBuffer{limit: 64 << 10}
	command.Stdout, command.Stderr = stdout, stderr
	command.SysProcAttr = &syscall.SysProcAttr{Setpgid: true}
	command.Cancel = func() error {
		return syscall.Kill(-command.Process.Pid, syscall.SIGKILL)
	}
	command.WaitDelay = 10 * time.Second
	err := command.Run()
	return stderr.Bytes(), err
}

// publish commits anything the agent left uncommitted and pushes the session
// branch to the served repository. It reports whether the branch moved.
func (r *agentJobRunner) publish(ctx context.Context, repo Repository, session *agentSession, workdir string) (bool, error) {
	status, err := agentGit(ctx, workdir, "status", "--porcelain")
	if err != nil {
		return false, err
	}
	if status != "" {
		if _, err := agentGit(ctx, workdir, "add", "-A"); err != nil {
			return false, err
		}
		message := session.Title
		if message == "" {
			message = "Agent changes"
		}
		if _, err := agentGit(ctx, workdir, "commit", "--quiet", "--no-verify", "-m", message); err != nil {
			return false, err
		}
	}
	tip, err := agentGit(ctx, workdir, "rev-parse", "HEAD")
	if err != nil {
		return false, err
	}
	current := gitRefValue(ctx, repo.GitDir, "refs/heads/"+session.Branch)
	if tip == current {
		return false, nil
	}
	if current == "" && tip == gitRefValue(ctx, repo.GitDir, "refs/heads/"+session.BaseBranch) {
		return false, nil // the agent made no change worth a branch
	}
	if _, err := agentGit(ctx, workdir, "push", "--quiet", repo.GitDir,
		"HEAD:refs/heads/"+session.Branch); err != nil {
		return false, err
	}
	r.daemon.requestSync()
	return true, nil
}

// openPull writes a node-signed, branch-backed pull through the same ledger
// writer mirror intake uses.
func (r *agentJobRunner) openPull(ctx context.Context, repo Repository, session *agentSession, summary string) (int, error) {
	base, head := session.BaseBranch, session.Branch
	baseTip := gitRefValue(ctx, repo.GitDir, "refs/heads/"+base)
	headTip := gitRefValue(ctx, repo.GitDir, "refs/heads/"+head)
	if baseTip == "" || headTip == "" {
		return 0, errors.New("the pull branches are not published")
	}
	if gitValue(ctx, repo.GitDir, "rev-list", "--count", baseTip+".."+headTip) == "0" {
		return 0, nil
	}
	patch, err := exec.CommandContext(ctx, "git", "--git-dir="+repo.GitDir,
		"diff", "--binary", baseTip+"..."+headTip).Output()
	if err != nil {
		return 0, errors.New("could not build the pull patch")
	}
	commits, err := exec.CommandContext(ctx, "git", "--git-dir="+repo.GitDir,
		"format-patch", "--stdout", baseTip+".."+headTip).Output()
	if err != nil {
		return 0, errors.New("could not build the pull commit series")
	}
	if len(patch)+len(commits) > maxPullPayloadBytes {
		// The pull is branch-backed; an oversized replay payload is optional.
		patch, commits = nil, nil
	}
	node := r.node()
	identity := r.daemon.identity
	author := identity.PublicKey()
	ts := time.Now().UnixMilli()
	content := strings.Join([]string{session.Title, base, head, string(patch), string(commits)}, "\x00")
	digest := sha256.Sum256([]byte(content))
	sig := identity.Sign(pullEventSigningMessage(repo.Owner, repo.Name, author, ts,
		hex.EncodeToString(digest[:])))
	description := fmt.Sprintf("Opened by the %s agent on %s for a website session.", session.Provider, node)
	if summary = strings.TrimSpace(summary); summary != "" {
		description += "\n\n" + boundedRunes(summary, 4000)
	}
	raw, err := json.Marshal(map[string]any{
		"id": 1, "number": 0,
		"pull": map[string]any{
			"title": session.Title, "base": base, "head": head,
			"patch": string(patch), "commits": string(commits), "ts": ts,
			"author": author, "authorName": node, "sig": sig,
			"description": description, "creationBaseOid": baseTip,
			"creationHeadOid": headTip,
		},
	})
	if err != nil {
		return 0, err
	}
	r.daemon.collaborationWriteMu.Lock()
	ids, err := materializePullInbox(ctx, repo, []json.RawMessage{raw}, node)
	if err != nil {
		// Concurrent intake can move forkmesh/pulls under the commit; retry once.
		ids, err = materializePullInbox(ctx, repo, []json.RawMessage{raw}, node)
	}
	r.daemon.collaborationWriteMu.Unlock()
	if err != nil {
		return 0, err
	}
	if len(ids) == 0 {
		return 0, errors.New("the pull was not written")
	}
	r.daemon.requestSync()
	return agentPullNumber(ctx, repo.GitDir, sig), nil
}

func eventSigningRepoKey(owner, name string) string {
	owner = strings.ToLower(strings.TrimSpace(owner))
	name = strings.ToLower(strings.TrimSpace(name))
	if owner == "" || name == "" || strings.ContainsAny(owner+name, "/\n\x00") {
		return ""
	}
	return owner + "/" + name
}

func pullEventSigningMessage(owner, name, author string, ts int64, digestHex string) []byte {
	body := author + "\n" + strconv.FormatInt(ts, 10) + "\n" + digestHex
	if key := eventSigningRepoKey(owner, name); key != "" {
		return []byte("forkmesh-pull-event-v2\n" + key + "\n" + body)
	}
	return []byte("forkmesh-pull-event-v1\n" + body)
}

func agentPullNumber(ctx context.Context, gitDir, sig string) int {
	var numbers []int
	for _, path := range gitLines(ctx, gitDir, "ls-tree", "-r", "--name-only",
		"refs/heads/"+pullMetadataBranch, "--", ".forkmesh/pulls") {
		parts := strings.Split(path, "/")
		if len(parts) != 4 || parts[3] != "pull.md" {
			continue
		}
		if number, err := strconv.Atoi(parts[2]); err == nil && number > 0 {
			numbers = append(numbers, number)
		}
	}
	sort.Sort(sort.Reverse(sort.IntSlice(numbers)))
	for _, number := range numbers {
		body := gitValue(ctx, gitDir, "show",
			fmt.Sprintf("refs/heads/%s:.forkmesh/pulls/%d/pull.md", pullMetadataBranch, number))
		if strings.Contains(body, "\nsig: "+sig+"\n") {
			return number
		}
	}
	return 0
}
