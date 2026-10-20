package mirrornode

// Runner role (docs/RUNNERS.md, PLAN.md Phase 5).
//
// A runner holds no repository and serves nothing. It claims agent leases
// from the app's runner API (POST /api/runners/claim), clones the one leased
// repository from the core gateway with the lease's read grant into a fresh
// 0700 directory under runner.workRoot, runs the provider CLI in a per-job
// sandbox (sandbox.go), pushes exactly refs/heads/agent/<session> with the
// lease's push grant, reports on POST /api/runners/report, and removes the
// directory. The job lifecycle itself (idempotent records, running/terminal
// reports, transcripts, session follow-ups) is agents.go's.

import (
	"bytes"
	"context"
	"crypto/rand"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	mathrand "math/rand/v2"
	"net"
	"net/http"
	"net/url"
	"os"
	"os/exec"
	"os/user"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"
)

const (
	runnerClaimPath      = "/api/runners/claim"
	runnerReportPath     = "/api/runners/report"
	runnerAttachmentPath = "/api/runners/attachment"
	// A runner has no node event channel (it has no node account), so the
	// claim poll is its only wake-up besides finishing a job: every
	// runnerIdlePoll ±runnerPollJitter, backing off from runnerErrorPoll to
	// runnerErrorCeil while the app keeps failing.
	runnerIdlePoll    = 20 * time.Second
	runnerErrorPoll   = 30 * time.Second
	runnerErrorCeil   = 5 * time.Minute
	runnerPollJitter  = 0.25
	runnerClaimWait   = 20 * time.Second
	runnerClaimGap    = time.Second
	runnerStaleAfter  = 3 * time.Minute
	runnerMaxClaim    = 4
	runnerGrantHeader = "X-ForkMesh-Runner-Grant"
)

// runnerBackend is the agentBackend of the runner role.
type runnerBackend struct {
	r        *agentJobRunner
	cfg      *RunnerConfig
	identity *Identity
	node     string
	version  string
	box      jobSandbox
	client   *http.Client
	now      func() time.Time
	// Web sign-in of the provider CLIs (runnerlogin.go), and the poll
	// interval the app asks for while one is open.
	logins     *runnerLoginManager
	fastPollMs atomic.Int64
	claimHeld  atomic.Bool
	wake       func()
	cacheMu    sync.Mutex
	cacheLocks map[string]*sync.Mutex
	cacheSwept time.Time
	actions    *runnerActions
}

// runnerInbox is a claim or poll response.
type runnerInbox struct {
	Jobs       []json.RawMessage      `json:"jobs"`
	Logins     []runnerLogin          `json:"logins"`
	FastPollMs int64                  `json:"fastPollMs"`
	WaitMs     int64                  `json:"waitMs"`
	Accounts   []runnerAccountRequest `json:"accountRequests"`
}

func newRunnerAgentJobs(d *Daemon) (*agentJobRunner, error) {
	cfg := d.config.Runner
	if cfg.JobUser != "" && cfg.JobUID == 0 {
		account, err := user.Lookup(cfg.JobUser)
		if err != nil {
			return nil, fmt.Errorf("runner.jobUser %q: %v", cfg.JobUser, err)
		}
		uid, err := strconv.Atoi(account.Uid)
		if err != nil || uid <= 0 {
			return nil, fmt.Errorf("runner.jobUser %q has no usable uid", cfg.JobUser)
		}
		cfg.JobUID = uid
	}
	box, err := detectJobSandbox(cfg, exec.LookPath, os.Getenv, os.Geteuid(),
		func(path string) bool { _, err := os.Stat(path); return err == nil })
	if err != nil {
		return nil, err
	}
	if box.uid != 0 {
		account, err := user.LookupId(strconv.Itoa(box.uid))
		if err != nil || account.HomeDir == "" || account.HomeDir == "/" {
			return nil, fmt.Errorf("runner.jobUid %d needs a passwd entry with its own home (e.g. /var/lib/forkmesh-job)", box.uid)
		}
		box.home, box.userName = account.HomeDir, account.Username
		if gid, err := strconv.Atoi(account.Gid); err == nil && gid > 0 {
			box.gid = gid
		}
		if daemonHome, _ := os.UserHomeDir(); daemonHome != "" && filepath.Clean(daemonHome) == filepath.Clean(box.home) {
			return nil, errors.New("the job user must not share the daemon's home")
		}
	} else {
		log.Printf("runner: WARNING jobs run as the daemon's own user and can read its node key; " +
			"run the daemon as root with runner.jobUid/jobUser (docs/RUNNERS.md) before serving other owners")
	}
	if raw, err := os.ReadFile("/etc/resolv.conf"); err == nil {
		box.dnsAllow = loopbackResolvers(string(raw))
	}
	box.accounts = newRunnerAccounts(filepath.Join(cfg.WorkRoot, runnerAccountsDir), box.persistentHome(),
		box.uid, box.gidOrUID())
	box.sweepIdle()
	r := newAgentJobRunner(d)
	r.maxRuns = cfg.MaxConcurrentRuns
	r.command = box.runner()
	if box.uid != 0 {
		r.credentialUID = box.uid
		// Provider logins belong to the job user, so availability reads its
		// home rather than the daemon's, and never the daemon's own
		// CLAUDE_CONFIG_DIR/CODEX_HOME.
		r.getenv = func(name string) string {
			switch name {
			case "HOME":
				return box.home
			case "CLAUDE_CONFIG_DIR", "CODEX_HOME":
				return ""
			}
			return os.Getenv(name)
		}
	}
	backend := &runnerBackend{r: r, cfg: cfg, identity: d.identity,
		node: d.gateway.Node.Name, version: d.config.Version, box: box,
		client: agentJobHTTPClient, now: time.Now}
	backend.logins = newRunnerLoginManager(backend.signedPost, backend.startLogin)
	backend.logins.logf = log.Printf
	// A finished sign-in changes availability: report it on a poll now.
	backend.logins.onDone = d.requestSync
	backend.wake = d.requestSync
	if d.config.EnableActions {
		backend.actions = newRunnerActions(backend)
	}
	r.backend = backend
	if box.direct {
		log.Printf("runner: jobs run as uid %d without systemd; no memory or CPU limits, only the job timeout", box.uid)
	} else if box.sandboxed() {
		log.Printf("runner: jobs run in systemd scopes (MemoryMax=%s CPUQuota=%s user-manager=%v uid=%d ip-filter=%v)",
			box.memoryMax, box.cpuQuota, box.user, box.uid, box.sandboxed() && !box.user)
	} else {
		log.Printf("runner: systemd-run unavailable; jobs run with a timeout and RLIMIT_AS=%d bytes",
			box.addressLimit())
	}
	return r, nil
}

func (b *runnerBackend) signedPost(ctx context.Context, path string, body []byte) (int, []byte, error) {
	return signedNodePost(ctx, b.client, b.cfg.AppURL, b.node, b.identity, "ForkMesh-Runner/"+b.version, b.now(), path, body)
}

// signedNodePost sends body to the app, signed with this node's key over the
// same canonical request message the router uses for capabilities
// (edge_routing.request_message), so the app verifies it with existing code.
// Runners use it for claims and reports; a core uses it for pool status.
func signedNodePost(ctx context.Context, client *http.Client, origin, node string, identity *Identity,
	userAgent string, now time.Time, path string, body []byte) (int, []byte, error) {
	status, reply, err := signedNodeExchange(ctx, client, origin, node, identity, userAgent, now, path, body,
		agentJobResponseLimit)
	if err == nil && len(reply) > agentJobResponseLimit {
		return 0, nil, errors.New("app response is too large")
	}
	return status, reply, err
}

func signedNodeExchange(ctx context.Context, client *http.Client, origin, node string, identity *Identity,
	userAgent string, now time.Time, path string, body []byte, limit int64) (int, []byte, error) {
	digest := sha256.Sum256(body)
	bodyHash := hex.EncodeToString(digest[:])
	nonce := make([]byte, 18)
	if _, err := rand.Read(nonce); err != nil {
		return 0, nil, err
	}
	requestID := base64.RawURLEncoding.EncodeToString(nonce)
	issuedAt := strconv.FormatInt(now.UnixMilli(), 10)
	message := strings.Join([]string{"forkmesh-masked-proxy-v1", node, http.MethodPost,
		path, bodyHash, requestID, issuedAt}, "\n")
	request, err := http.NewRequestWithContext(ctx, http.MethodPost, origin+path, bytes.NewReader(body))
	if err != nil {
		return 0, nil, err
	}
	request.Header.Set("Content-Type", "application/json")
	request.Header.Set("Accept", "application/json")
	request.Header.Set("User-Agent", userAgent)
	request.Header.Set("X-ForkMesh-Node", node)
	request.Header.Set("X-ForkMesh-Request-Id", requestID)
	request.Header.Set("X-ForkMesh-Issued-At", issuedAt)
	request.Header.Set("X-ForkMesh-Body-Sha256", bodyHash)
	request.Header.Set("X-ForkMesh-Signature", identity.Sign([]byte(message)))
	response, err := client.Do(request)
	if err != nil {
		return 0, nil, err
	}
	defer response.Body.Close()
	reply, err := io.ReadAll(io.LimitReader(response.Body, limit+1))
	if err != nil {
		return 0, nil, err
	}
	return response.StatusCode, reply, nil
}

func (b *runnerBackend) postReport(ctx context.Context, record agentJobRecord, body []byte) (int, []byte, error) {
	var fields map[string]any
	if err := json.Unmarshal(body, &fields); err != nil {
		return 0, nil, err
	}
	fields["jobId"] = record.JobID
	wrapped, err := json.Marshal(fields)
	if err != nil {
		return 0, nil, err
	}
	return b.signedPost(ctx, runnerReportPath, wrapped)
}

func (b *runnerBackend) materializeAttachments(ctx context.Context, work agentWork, workdir string) ([]string, error) {
	var own func(string) error
	if b.box.uid != 0 {
		own = func(path string) error { return os.Lchown(path, b.box.uid, b.box.gidOrUID()) }
	}
	return writeAgentAttachments(work.job.Attachments, workdir,
		func() error {
			info, err := plainAgentFolder(workdir, ".git/info", own)
			if err != nil {
				return err
			}
			path := filepath.Join(info, "exclude")
			if err := appendAgentAttachmentExclude(path); err != nil || own == nil {
				return err
			}
			return own(path)
		},
		func(attachment agentAttachment) ([]byte, error) { return b.fetchAttachment(ctx, work, attachment) },
		own)
}

func (b *runnerBackend) fetchAttachment(ctx context.Context, work agentWork, attachment agentAttachment) ([]byte, error) {
	body, err := json.Marshal(map[string]any{"jobId": work.job.JobID,
		"leaseId": b.r.currentLease(work.job), "attachmentId": attachment.ID})
	if err != nil {
		return nil, err
	}
	status, data, err := signedNodeExchange(ctx, b.client, b.cfg.AppURL, b.node, b.identity,
		"ForkMesh-Runner/"+b.version, b.now(), runnerAttachmentPath, body, attachment.Size)
	if err != nil {
		return nil, err
	}
	return checkAgentAttachment(attachment, status, data)
}

// claim asks the app for up to max jobs this runner may serve. It also
// reports provider availability and running sign-ins, and the answer names
// any sign-in session to start (runnerlogin.go).
func (b *runnerBackend) claim(ctx context.Context, max int, providers []string, actionsMax int) (runnerInbox, error) {
	fields := b.statusFields()
	fields["max"], fields["providers"] = max, providers
	if actionsMax > 0 {
		fields["actionsMax"] = actionsMax
	}
	fields["allowedOwners"], fields["burst"] = b.cfg.AllowedOwners, b.cfg.Burst
	fields["waitMs"] = runnerClaimWait.Milliseconds()
	body, err := json.Marshal(fields)
	if err != nil {
		return runnerInbox{}, err
	}
	client := b.client
	if client.Timeout > 0 {
		held := *client
		held.Timeout += runnerClaimWait
		client = &held
	}
	status, reply, err := signedNodePost(ctx, client, b.cfg.AppURL, b.node, b.identity,
		"ForkMesh-Runner/"+b.version, b.now(), runnerClaimPath, body)
	if err != nil {
		return runnerInbox{}, err
	}
	if status < 200 || status >= 300 {
		return runnerInbox{}, fmt.Errorf("runner claim returned HTTP %d: %s", status, boundedText(reply, 240))
	}
	var inbox runnerInbox
	if err := json.Unmarshal(reply, &inbox); err != nil {
		return runnerInbox{}, errors.New("runner claim returned invalid JSON")
	}
	return inbox, nil
}

// validate checks the lease is internally consistent and for an owner this
// runner accepts. The grants are parsed, not verified: the gateway verifies
// them, and a runner that trusted a forged grant could only fail its push.
func (b *runnerBackend) validate(work agentWork) string {
	job := work.job
	if agentEnvelopeInvalid(job) || !nodePattern.MatchString(job.RepositoryOwner) ||
		!repositoryPattern.MatchString(job.RepositoryName) {
		return "Malformed or unsafe job envelope."
	}
	if len(b.cfg.AllowedOwners) != 0 {
		owner := strings.ToLower(job.Organization)
		allowed := false
		for _, candidate := range b.cfg.AllowedOwners {
			allowed = allowed || candidate == owner
		}
		if !allowed {
			return "This runner does not accept jobs for that owner."
		}
	}
	read, readOK := parseRunnerGrantToken(job.ReadGrant)
	push, pushOK := parseRunnerGrantToken(job.PushGrant)
	switch {
	case !readOK || !pushOK || read.Access != "read" || push.Access != "push",
		read.Runner != b.node || push.Runner != b.node,
		read.Node != push.Node || read.Owner != push.Owner || read.Repository != push.Repository,
		!strings.EqualFold(push.Owner, job.RepositoryOwner) || push.Repository != job.RepositoryName,
		push.Ref != job.PushRef || push.Ref != "refs/heads/"+job.Branch,
		!validRunnerAgentRef(job.PushRef):
		return "The lease's grants do not match the job."
	}
	if expires := time.UnixMilli(min(read.ExpiresAt, push.ExpiresAt)); expires.Sub(b.now()) <= agentPublishMargin {
		return "The lease's grants expire before the job could publish."
	}
	if _, err := runnerCloneURL(job); err != nil {
		return err.Error()
	}
	return ""
}

// runnerCloneURL accepts only the gateway's runner route for the job's repo.
func runnerCloneURL(job agentJob) (string, error) {
	parsed, err := url.Parse(job.CloneURL)
	loopback := err == nil && parsed.Scheme == "http" && isLoopbackHost(parsed.Hostname())
	if err != nil || (parsed.Scheme != "https" && !loopback) || parsed.User != nil ||
		parsed.RawQuery != "" || parsed.Fragment != "" ||
		parsed.EscapedPath() != "/v1/runner/"+strings.ToLower(job.RepositoryOwner)+"/"+job.RepositoryName+".git" {
		return "", errors.New("The lease's clone URL is not the core's runner route.")
	}
	return parsed.String(), nil
}

// workdir is the job's directory: stable per session, so a Claude Code
// follow-up can resume the conversation recorded for that path, but always
// recreated empty and removed after the job.
func (b *runnerBackend) workdir(sessionID string) string {
	return filepath.Join(b.cfg.WorkRoot, agentSessionDirectory(sessionID))
}

// removeUnderRoot deletes path only when it is strictly inside root.
func removeUnderRoot(root, path string) error {
	if path == "" {
		return nil
	}
	if !filepath.IsAbs(root) {
		return fmt.Errorf("refusing to remove %s outside %s", path, root)
	}
	return removeOwnCloneUnder(root, path)
}

// runnerBundleLimit caps the bundle a job hands over for publishing.
const runnerBundleLimit = 1 << 30

var runnerObjectID = regexp.MustCompile(`^(?:[0-9a-f]{40}|[0-9a-f]{64})$`)

// daemonGit runs git as the daemon, only ever on repositories the daemon
// created and the job never touched (the publish repository, or a fresh
// workdir before the job starts). It never reads system or global config,
// runs no hooks or fsmonitor, allows only the transports it needs, and
// carries the grant (if any) as an HTTP header through GIT_CONFIG_*, so it
// is neither on the command line nor written into any repository's config.
//
// fileAllowed permits the local file transport (the workdir clone from the
// publish repository and the bundle fetch); a clone or push to the core
// never allows it. http is allowed only for a loopback core (tests).
func (b *runnerBackend) daemonGit(ctx context.Context, dir, grant, remote string, fileAllowed bool, args ...string) (string, error) {
	fileAllow := "never"
	if fileAllowed {
		fileAllow = "always"
	}
	httpAllow := "never"
	if parsed, err := url.Parse(remote); err == nil && parsed.Scheme == "http" && isLoopbackHost(parsed.Hostname()) {
		httpAllow = "always"
	}
	hardened := []string{
		"-c", "core.hooksPath=/dev/null", "-c", "core.fsmonitor=false",
		"-c", "protocol.allow=never", "-c", "protocol.https.allow=always",
		"-c", "protocol.http.allow=" + httpAllow, "-c", "protocol.file.allow=" + fileAllow,
		"-c", "credential.helper=", "-c", "http.followRedirects=false",
	}
	command := exec.CommandContext(ctx, "git", append(hardened, args...)...)
	command.Dir = dir
	home := filepath.Join(b.cfg.WorkRoot, jobPublishDir)
	command.Env = []string{"PATH=" + os.Getenv("PATH"), "HOME=" + home, "LANG=C",
		"GIT_CONFIG_NOSYSTEM=1", "GIT_CONFIG_GLOBAL=/dev/null", "GIT_TERMINAL_PROMPT=0",
		"GIT_ATTR_NOSYSTEM=1"}
	for _, name := range []string{"HTTPS_PROXY", "https_proxy", "NO_PROXY", "no_proxy"} {
		if value := os.Getenv(name); value != "" {
			command.Env = append(command.Env, name+"="+value)
		}
	}
	if grant != "" {
		command.Env = append(command.Env, "GIT_CONFIG_COUNT=1",
			"GIT_CONFIG_KEY_0=http.extraHeader", "GIT_CONFIG_VALUE_0="+runnerGrantHeader+": "+grant)
	}
	output, err := command.CombinedOutput()
	if err != nil {
		// Never echo the command line or environment: the grant is there.
		return "", fmt.Errorf("git %s: %s", firstGitVerb(args), boundedText(output, 240))
	}
	return strings.TrimSpace(string(output)), nil
}

func firstGitVerb(args []string) string {
	for i := 0; i < len(args); i++ {
		if args[i] == "-c" {
			i++
			continue
		}
		if !strings.HasPrefix(args[i], "-") {
			return args[i]
		}
	}
	return "command"
}

// publishRepo is the daemon-owned bare repository of a workdir.
func (b *runnerBackend) publishRepo(workdir string) string {
	return filepath.Join(b.cfg.WorkRoot, jobPublishDir, filepath.Base(workdir)+".git")
}

func (b *runnerBackend) publishRef(ctx context.Context, repo, ref string) string {
	value, err := b.daemonGit(ctx, "", "", "", false, "--git-dir="+repo, "rev-parse", "--verify", "-q", ref+"^{commit}")
	if err != nil || !runnerObjectID.MatchString(value) {
		return ""
	}
	return value
}

// jobGit runs git in the workdir inside the job sandbox, as the job user:
// the only git that ever reads the job-controlled repository and its config.
func (b *runnerBackend) jobGit(ctx context.Context, workdir string, args ...string) (string, error) {
	stdout := &cappedBuffer{limit: 1 << 20}
	full := append([]string{"-c", "core.hooksPath=/dev/null", "-c", "core.fsmonitor=false"}, args...)
	stderr, err := b.box.runner()(ctx, workdir, "", stdout, "git", full...)
	if err != nil {
		return "", fmt.Errorf("git %s (job sandbox): %s", args[0], firstNonEmpty(boundedText(stderr, 240), err.Error()))
	}
	return strings.TrimSpace(stdout.String()), nil
}

func (b *runnerBackend) prepare(ctx context.Context, work agentWork) (*agentSession, string, error) {
	job := work.job
	cloneURL, err := runnerCloneURL(job)
	if err != nil {
		return nil, "", err
	}
	if err := os.MkdirAll(b.cfg.WorkRoot, 0700); err != nil {
		return nil, "", err
	}
	_ = os.Chmod(b.cfg.WorkRoot, 0711)
	if err := ensureDaemonDir(filepath.Join(b.cfg.WorkRoot, jobPublishDir), 0700); err != nil {
		return nil, "", err
	}
	dir := b.workdir(job.SessionID)
	repo := b.publishRepo(dir)
	for _, path := range []string{dir, repo, repo + ".bundle"} {
		if err := removeUnderRoot(b.cfg.WorkRoot, path); err != nil {
			return nil, "", err
		}
	}
	// The daemon's own copy of the core: every later ref read and the push
	// happen here, never in the job's repository.
	if err := b.seedPublishRepo(ctx, job, cloneURL, repo); err != nil {
		return nil, dir, err
	}
	if err := os.Mkdir(dir, 0700); err != nil {
		return nil, dir, err
	}
	// The job's worktree is a plain copy of it (no hardlinks: the job user
	// will own every file of the workdir).
	if _, err := b.daemonGit(ctx, "", "", "", true, "clone", "--quiet", "--no-checkout", "--no-hardlinks",
		"--", repo, dir); err != nil {
		return nil, dir, err
	}
	base := validPullBranch(job.BaseBranch)
	if base == "" {
		head, _ := b.daemonGit(ctx, "", "", "", false, "--git-dir="+repo, "symbolic-ref", "--short", "HEAD")
		if b.publishRef(ctx, repo, "refs/heads/"+head) != "" {
			base = validPullBranch(head)
		}
	}
	if base == "" {
		base = "main"
	}
	branch := strings.TrimPrefix(job.PushRef, "refs/heads/")
	r := b.r
	r.mu.Lock()
	var existing *agentSession
	if stored := r.state.Sessions[job.SessionID]; stored != nil {
		copied := *stored
		existing = &copied
	}
	r.mu.Unlock()
	title := strings.TrimSpace(job.Title)
	if title == "" {
		title = strings.TrimSpace(strings.SplitN(strings.TrimSpace(job.Prompt), "\n", 2)[0])
	}
	title = boundedRunes(title, 160)
	var session *agentSession
	switch job.Kind {
	case "start", "continue":
		baseRef := b.publishRef(ctx, repo, "refs/heads/"+base)
		if baseRef == "" {
			return nil, dir, errors.New("the repository has no base branch to start the agent from")
		}
		if job.Kind == "continue" && b.publishRef(ctx, repo, "refs/heads/"+branch) == "" {
			return nil, dir, fmt.Errorf("branch %s is not on the core yet, so the moved session has no history to continue", branch)
		}
		session = &agentSession{
			SessionID: job.SessionID, RepositoryOwner: strings.ToLower(job.RepositoryOwner),
			LocalAgentID: job.JobID, RepositoryName: job.RepositoryName,
			Provider: job.Provider, Model: job.Model, Title: title, Branch: branch,
			BaseBranch: base, BaseRef: baseRef, IssueNumber: job.IssueNumber,
			CreatePr: job.CreatePr, CreatedAt: time.Now().UnixMilli(),
		}
	case "steer":
		if existing != nil && existing.RepositoryName == job.RepositoryName && existing.Branch == branch {
			session = existing
			break
		}
		if validAgentBranch(branch) == "" {
			return nil, dir, errors.New("the scoped agent session is unavailable on this runner")
		}
		baseRef := b.publishRef(ctx, repo, "refs/heads/"+base)
		if baseRef == "" {
			return nil, dir, errors.New("the repository has no base branch to resume the agent on")
		}
		session = &agentSession{
			SessionID: job.SessionID, RepositoryOwner: strings.ToLower(job.RepositoryOwner),
			LocalAgentID: job.JobID, RepositoryName: job.RepositoryName,
			Provider: job.Provider, Model: job.Model, Title: title, Branch: branch,
			BaseBranch: base, BaseRef: baseRef, IssueNumber: job.IssueNumber,
			CreatePr: job.CreatePr, CreatedAt: time.Now().UnixMilli(),
		}
	default:
		return nil, dir, errors.New("unknown job kind")
	}
	start := "origin/" + session.BaseBranch
	if b.publishRef(ctx, repo, "refs/heads/"+branch) != "" {
		start = "origin/" + branch
	}
	// The workdir is still the daemon's and untouched by any job here.
	for _, args := range [][]string{
		{"checkout", "--quiet", "-B", branch, start},
		{"config", "user.name", commitIdentityName(b.node)},
		{"config", "user.email", commitIdentityEmail(b.node)},
		{"remote", "set-url", "origin", cloneURL},
	} {
		if _, err := b.daemonGit(ctx, dir, "", "", false, args...); err != nil {
			return nil, dir, err
		}
	}
	if err := b.box.openSession(b.box.homeFor(dir)); err != nil {
		return nil, dir, err
	}
	if b.box.uid != 0 {
		if err := chownTree(dir, b.box.uid, b.box.gidOrUID()); err != nil {
			return nil, dir, err
		}
	}
	return session, dir, nil
}

func (b *runnerBackend) cacheRepo(cloneURL string) string {
	digest := sha256.Sum256([]byte(cloneURL))
	return filepath.Join(b.cfg.WorkRoot, jobCacheDir, hex.EncodeToString(digest[:12])+".git")
}

func (b *runnerBackend) cacheLock(path string) *sync.Mutex {
	b.cacheMu.Lock()
	defer b.cacheMu.Unlock()
	if b.cacheLocks == nil {
		b.cacheLocks = map[string]*sync.Mutex{}
	}
	lock := b.cacheLocks[path]
	if lock == nil {
		lock = &sync.Mutex{}
		b.cacheLocks[path] = lock
	}
	return lock
}

func (b *runnerBackend) seedPublishRepo(ctx context.Context, job agentJob, cloneURL, repo string) error {
	if cache, ok := b.refreshCache(ctx, job, cloneURL); ok {
		lock := b.cacheLock(cache)
		lock.Lock()
		_, err := b.daemonGit(ctx, "", "", "", true, "clone", "--quiet", "--bare", "--", cache, repo)
		lock.Unlock()
		if err == nil {
			return nil
		}
		log.Printf("runner: copy of the repository cache failed, cloning from the core: %v", err)
		if err := removeUnderRoot(b.cfg.WorkRoot, repo); err != nil {
			return err
		}
	}
	_, err := b.daemonGit(ctx, "", job.ReadGrant, cloneURL, false, "clone", "--quiet", "--bare",
		"--", cloneURL, repo)
	return err
}

func (b *runnerBackend) refreshCache(ctx context.Context, job agentJob, cloneURL string) (string, bool) {
	root := filepath.Join(b.cfg.WorkRoot, jobCacheDir)
	if err := ensureDaemonDir(root, 0700); err != nil {
		log.Printf("runner: repository cache unavailable: %v", err)
		return "", false
	}
	b.sweepCaches(root)
	cache := b.cacheRepo(cloneURL)
	lock := b.cacheLock(cache)
	lock.Lock()
	defer lock.Unlock()
	now := time.Now()
	if info, err := os.Lstat(cache); err == nil && info.IsDir() {
		if _, err := b.daemonGit(ctx, "", job.ReadGrant, cloneURL, false, "--git-dir="+cache, "fetch",
			"--quiet", "--prune", "--no-write-fetch-head", "--", cloneURL,
			"+refs/heads/*:refs/heads/*", "+refs/tags/*:refs/tags/*"); err != nil {
			log.Printf("runner: repository cache fetch failed, cloning from the core: %v", err)
			return "", false
		}
		_ = os.Chtimes(cache, now, now)
		return cache, true
	}
	fresh := cache + ".new"
	if err := removeUnderRoot(root, fresh); err != nil {
		log.Printf("runner: repository cache unavailable: %v", err)
		return "", false
	}
	if _, err := b.daemonGit(ctx, "", job.ReadGrant, cloneURL, false, "clone", "--quiet", "--bare",
		"--", cloneURL, fresh); err != nil {
		_ = removeUnderRoot(root, fresh)
		log.Printf("runner: repository cache clone failed: %v", err)
		return "", false
	}
	if err := os.Rename(fresh, cache); err != nil {
		_ = removeUnderRoot(root, fresh)
		log.Printf("runner: repository cache unavailable: %v", err)
		return "", false
	}
	return cache, true
}

func (b *runnerBackend) sweepCaches(root string) {
	b.cacheMu.Lock()
	if time.Since(b.cacheSwept) < time.Hour {
		b.cacheMu.Unlock()
		return
	}
	b.cacheSwept = time.Now()
	b.cacheMu.Unlock()
	entries, err := os.ReadDir(root)
	if err != nil {
		return
	}
	for _, entry := range entries {
		info, err := entry.Info()
		if err != nil || time.Since(info.ModTime()) < jobCacheTTL {
			continue
		}
		path := filepath.Join(root, entry.Name())
		lock := b.cacheLock(path)
		if !lock.TryLock() {
			continue
		}
		if err := removeUnderRoot(root, path); err != nil {
			log.Printf("runner: %v", err)
		}
		lock.Unlock()
	}
}

func chownTree(root string, uid, gid int) error {
	return filepath.Walk(root, func(path string, _ os.FileInfo, err error) error {
		if err != nil {
			return err
		}
		return os.Lchown(path, uid, gid)
	})
}

// publish commits what the agent left and pushes only the leased ref.
//
// The job's repository is job-controlled: its config can name an fsmonitor,
// hooks, filters, or a different remote. So the daemon never runs git in
// it. The commit and a bundle of the new commits are made inside the job
// sandbox, as the job user. The daemon copies the bundle (a plain file,
// read with O_NOFOLLOW and an owner check), fetches it into its own bare
// repository with hardened git, and pushes from there to the lease's clone
// URL and ref, whatever the workdir's remote says.
func (b *runnerBackend) publish(ctx context.Context, work agentWork, session *agentSession, workdir string) (bool, error) {
	status, err := b.jobGit(ctx, workdir, "status", "--porcelain")
	if err != nil {
		return false, err
	}
	if status != "" {
		if _, err := b.jobGit(ctx, workdir, "add", "-A"); err != nil {
			return false, err
		}
		message := session.Title
		if message == "" {
			message = "Agent changes"
		}
		if _, err := b.jobGit(ctx, workdir, "commit", "--quiet", "--no-verify", "-m", message); err != nil {
			return false, err
		}
	}
	claimed, err := b.jobGit(ctx, workdir, "rev-parse", "--verify", "HEAD^{commit}")
	if err != nil {
		return false, err
	}
	if !runnerObjectID.MatchString(claimed) {
		return false, errors.New("the job's HEAD is not a commit id")
	}
	repo := b.publishRepo(workdir)
	current := b.publishRef(ctx, repo, "refs/heads/"+session.Branch)
	base := b.publishRef(ctx, repo, "refs/heads/"+session.BaseBranch)
	tip := claimed
	if b.publishRef(ctx, repo, claimed) == "" {
		// New commits: the job bundles exactly those (everything the core
		// already has is excluded) into its own TMPDIR.
		home := b.box.homeFor(workdir)
		if home == "" {
			return false, errors.New("the job has no session home to hand over its commits")
		}
		bundle := filepath.Join(home, "tmp", "publish.bundle")
		args := []string{"bundle", "create", "--quiet", bundle, "HEAD"}
		for _, known := range []string{current, base} {
			if known != "" {
				args = append(args, "^"+known)
			}
		}
		if _, err := b.jobGit(ctx, workdir, args...); err != nil {
			return false, err
		}
		data, err := readOwnedFile(bundle, b.box.uid, runnerBundleLimit)
		if err != nil {
			return false, fmt.Errorf("job bundle: %v", err)
		}
		copied := repo + ".bundle"
		if err := os.WriteFile(copied, data, 0600); err != nil {
			return false, err
		}
		if _, err := b.daemonGit(ctx, "", "", "", true, "--git-dir="+repo, "-c", "transfer.fsckObjects=true", "fetch", "--quiet", "--no-tags",
			"--no-write-fetch-head", copied, "+HEAD:refs/fm/result"); err != nil {
			return false, err
		}
		tip = b.publishRef(ctx, repo, "refs/fm/result")
		if tip == "" {
			return false, errors.New("the job's bundle held no commit")
		}
	} else if _, err := b.daemonGit(ctx, "", "", "", false, "--git-dir="+repo, "update-ref",
		"refs/fm/result", claimed); err != nil {
		return false, err
	}
	b.recordDiffStats(ctx, repo, session, base, tip)
	if tip == current {
		return false, nil
	}
	if current == "" && tip == base {
		return false, nil
	}
	cloneURL, err := runnerCloneURL(work.job)
	if err != nil {
		return false, err
	}
	if grant, ok := b.modelGrantFor(work, session); ok {
		if err := b.attestCommits(ctx, repo, tip, []string{base, current}, work.job.ModelGrant, grant); err != nil {
			log.Printf("runner: commits published without a model attestation: %v", err)
		}
	}
	if _, err := b.daemonGit(ctx, "", work.job.PushGrant, cloneURL, false, "--git-dir="+repo, "push",
		"--quiet", "--no-verify", "--", cloneURL, "refs/fm/result:"+work.job.PushRef); err != nil {
		return false, err
	}
	return true, nil
}

func (b *runnerBackend) recordDiffStats(ctx context.Context, repo string, session *agentSession, base, tip string) {
	if base == "" || tip == "" {
		return
	}
	numstat, err := b.daemonGit(ctx, "", "", "", false, "--git-dir="+repo, "diff", "--numstat",
		"--no-renames", base+"..."+tip)
	if err != nil {
		return
	}
	stats := parseNumstat(numstat)
	if ahead, err := b.daemonGit(ctx, "", "", "", false, "--git-dir="+repo, "rev-list", "--count",
		base+".."+tip); err == nil {
		stats.Ahead, _ = strconv.Atoi(strings.TrimSpace(ahead))
	}
	session.DiffStats = &stats
}

// openPull is the core's job: a runner holds no key that may write the pull
// ledger. The published branch is reported, and the app opens the pull.
func (b *runnerBackend) openPull(context.Context, agentWork, *agentSession, string) (int, error) {
	return 0, nil
}

func (b *runnerBackend) release(work agentWork, workdir string) {
	if workdir == "" {
		workdir = b.workdir(work.job.SessionID)
	}
	// The workdir first: its files are the job user's, and nothing of the
	// job may run while the session home is locked and cleaned.
	if err := removeUnderRoot(b.cfg.WorkRoot, workdir); err != nil {
		log.Printf("runner: %v", err)
	}
	for _, path := range []string{b.publishRepo(workdir), b.publishRepo(workdir) + ".bundle"} {
		if err := removeUnderRoot(b.cfg.WorkRoot, path); err != nil {
			log.Printf("runner: %v", err)
		}
	}
	b.box.closeSession(b.box.homeFor(workdir))
}

func (b *runnerBackend) applyAccounts(ctx context.Context, requests []runnerAccountRequest) {
	if b.box.accounts.apply(requests) && b.wake != nil {
		b.wake()
	}
	b.box.accounts.refreshUsage(ctx)
}

// availableProviders names the CLIs this runner can run right now.
func (r *agentJobRunner) availableProviders() []string {
	var providers []string
	for _, provider := range []string{"claude-code", "codex"} {
		availability := r.availability(provider)
		if availability["binaryFound"] == true && availability["loginState"] == "available" {
			providers = append(providers, provider)
		}
	}
	return providers
}

// runnerDrain reports what is pending, then claims as many jobs as there
// are free slots.
func (r *agentJobRunner) runnerDrain(ctx context.Context) error {
	backend, ok := r.backend.(*runnerBackend)
	if !ok {
		return errors.New("not a runner")
	}
	failures := r.reportPending(ctx)
	polled := false
	backend.claimHeld.Store(false)
	if slots := min(r.freeSlots(), runnerMaxClaim); slots > 0 && (backend.actions == nil || backend.actions.free()) {
		providers := r.availableProviders()
		actionsMax := 0
		if backend.actions != nil {
			actionsMax = 1
		}
		if len(providers) != 0 || actionsMax != 0 {
			polled = true
			if len(providers) == 0 {
				providers = []string{"actions"}
			}
			inbox, err := backend.claim(ctx, slots, providers, actionsMax)
			if err != nil {
				failures = append(failures, err.Error())
			}
			backend.claimHeld.Store(err == nil && inbox.WaitMs > 0)
			backend.fastPollMs.Store(inbox.FastPollMs)
			backend.logins.begin(ctx, inbox.Logins)
			backend.applyAccounts(ctx, inbox.Accounts)
			for _, raw := range inbox.Jobs {
				if isRunnerActionsJob(raw) {
					if backend.actions != nil {
						backend.actions.accept(ctx, raw)
					}
					continue
				}
				var target struct {
					Owner string `json:"repositoryOwner"`
					Name  string `json:"repositoryName"`
				}
				_ = json.Unmarshal(raw, &target)
				r.accept(ctx, Repository{Owner: strings.ToLower(target.Owner), Name: target.Name}, raw)
			}
		}
	}
	if !polled {
		// No claim this round, but the app still needs this runner's
		// availability and may hold a sign-in for it.
		inbox, err := backend.poll(ctx)
		if err != nil {
			failures = append(failures, err.Error())
		}
		backend.fastPollMs.Store(inbox.FastPollMs)
		backend.logins.begin(ctx, inbox.Logins)
		backend.applyAccounts(ctx, inbox.Accounts)
	}
	if len(failures) != 0 {
		return errors.New(strings.Join(failures, "; "))
	}
	return nil
}

// runRunner is Daemon.Run for role runner: a loopback status server and the
// claim loop. No gateway, tunnel, catalog, endpoint lease, fork jobs, intake
// or node event channel: a runner has no node account and talks to the app
// only through /api/runners/*.
func (d *Daemon) runRunner(ctx context.Context) error {
	mux := http.NewServeMux()
	mux.HandleFunc("/healthz", d.handleHealth)
	mux.HandleFunc("/readyz", d.handleReady)
	mux.HandleFunc("/v1/status", d.handleStatus)
	mux.HandleFunc("/v1/control/sync", d.handleSync)
	d.server = &http.Server{Addr: d.config.Listen, Handler: localRequestOnly(mux),
		ReadHeaderTimeout: 5 * time.Second, IdleTimeout: 30 * time.Second}
	listener, err := net.Listen("tcp", d.config.Listen)
	if err != nil {
		return err
	}
	serverErr := make(chan error, 1)
	go func() { serverErr <- d.server.Serve(listener) }()
	d.agents.runCtx = ctx
	go d.runnerLoop(ctx)
	select {
	case <-ctx.Done():
		if d.terminals != nil {
			d.terminals.Stop("")
		}
		shutdownCtx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		_ = d.server.Shutdown(shutdownCtx)
		d.agents.wait()
		if backend, ok := d.agents.backend.(*runnerBackend); ok {
			backend.logins.wait()
			backend.actions.wait()
		}
		return nil
	case err := <-serverErr:
		if errors.Is(err, http.ErrServerClosed) {
			return nil
		}
		return err
	}
}

// runnerPollDelay is the wait before the next claim poll after `failures`
// consecutive failed drains; jitter in [0,1) spreads a fleet of runners
// across ±runnerPollJitter of the delay.
func runnerPollDelay(failures int, jitter float64) time.Duration {
	delay := runnerIdlePoll
	if failures > 0 {
		delay = runnerErrorPoll
		for i := 1; i < failures && delay < runnerErrorCeil; i++ {
			delay *= 2
		}
		delay = min(delay, runnerErrorCeil)
	}
	return time.Duration(float64(delay) * (1 - runnerPollJitter + 2*runnerPollJitter*jitter))
}

// runnerLoop polls /api/runners/claim (and reports pending results). A job
// that finishes or starts wakes it through syncRequests for an immediate
// re-poll; otherwise it waits runnerPollDelay.
func (d *Daemon) runnerLoop(ctx context.Context) {
	wait := d.runnerWait
	if wait == nil {
		wait = func(failures int) time.Duration { return runnerPollDelay(failures, mathrand.Float64()) }
	}
	failures := 0
	for ctx.Err() == nil {
		started := time.Now()
		err := d.agents.runnerDrain(ctx)
		d.mu.Lock()
		d.lastSyncAt = time.Now()
		d.lastSyncOK = err == nil
		d.lastSyncErr = ""
		if err != nil {
			d.lastSyncErr = boundedText([]byte(err.Error()), 400)
		}
		d.syncCount++
		d.mu.Unlock()
		if err != nil {
			failures++
			log.Printf("runner: %v", err)
		} else {
			failures = 0
		}
		delay := wait(failures)
		// While a web sign-in is open the app asks for fast polls, so a
		// pending session starts within about a second.
		if backend, ok := d.agents.backend.(*runnerBackend); ok && failures == 0 {
			if fast := backend.fastPollDelay(); fast > 0 && fast < delay {
				delay = fast
			} else if fast == 0 && backend.claimHeld.Load() {
				delay = max(0, runnerClaimGap-time.Since(started))
			}
		}
		timer := time.NewTimer(delay)
		select {
		case <-ctx.Done():
			timer.Stop()
			return
		case <-d.syncRequests:
			timer.Stop()
		case <-timer.C:
		}
	}
}

func (b *runnerBackend) modelGrantFor(work agentWork, session *agentSession) (agentModelGrant, bool) {
	if work.job.ModelGrant == "" || b.identity == nil {
		return agentModelGrant{}, false
	}
	grant, ok := parseAgentModelGrantToken(work.job.ModelGrant)
	model := session.Model
	if model == "" && session.Provider == "claude-code" {
		model = defaultClaudeModel
	}
	if !ok || grant.Runner != b.node || grant.RunnerKey != b.identity.PublicKey() ||
		grant.Ref != work.job.PushRef || grant.Provider != session.Provider || grant.Model != model {
		log.Printf("runner: the lease's model grant does not match this job; commits stay unattested")
		return agentModelGrant{}, false
	}
	return grant, true
}

func (b *runnerBackend) attestCommits(ctx context.Context, repo, tip string, exclude []string, grantToken string, grant agentModelGrant) error {
	args := []string{"--git-dir=" + repo, "rev-list", "--reverse", "--topo-order", "--parents",
		fmt.Sprintf("--max-count=%d", maxPullRuleCommits+1), tip}
	for _, known := range exclude {
		if known != "" {
			args = append(args, "^"+known)
		}
	}
	listing, err := b.daemonGit(ctx, "", "", "", false, args...)
	if err != nil {
		return err
	}
	lines := strings.Split(strings.TrimSpace(listing), "\n")
	if listing == "" {
		return nil
	}
	if len(lines) > maxPullRuleCommits {
		return errors.New("too many commits to attest")
	}
	scratch := repo + ".commit"
	defer os.Remove(scratch)
	rewritten := map[string]string{}
	for _, line := range lines {
		fields := strings.Fields(line)
		if len(fields) == 0 || !runnerObjectID.MatchString(fields[0]) {
			return errors.New("rev-list returned an invalid commit")
		}
		parents := make([]string, 0, len(fields)-1)
		for _, parent := range fields[1:] {
			if mapped, ok := rewritten[parent]; ok {
				parent = mapped
			}
			parents = append(parents, parent)
		}
		raw, err := b.daemonGit(ctx, "", "", "", false, "--git-dir="+repo, "cat-file", "commit", fields[0])
		if err != nil {
			return err
		}
		attested, err := attestAgentCommit([]byte(raw+"\n"), parents, grantToken, grant, b.identity)
		if err != nil {
			return err
		}
		if err := os.WriteFile(scratch, attested, 0600); err != nil {
			return err
		}
		oid, err := b.daemonGit(ctx, "", "", "", false, "--git-dir="+repo, "hash-object", "-t", "commit", "-w", "--", scratch)
		if err != nil {
			return err
		}
		if !runnerObjectID.MatchString(oid) {
			return errors.New("hash-object returned an invalid commit")
		}
		rewritten[fields[0]] = oid
	}
	attestedTip, ok := rewritten[tip]
	if !ok {
		return nil
	}
	_, err = b.daemonGit(ctx, "", "", "", false, "--git-dir="+repo, "update-ref", "refs/fm/result", attestedTip, tip)
	return err
}
