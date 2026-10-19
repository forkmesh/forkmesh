package mirrornode

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"math/rand/v2"
	"net"
	"net/http"
	"net/url"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"
)

type Daemon struct {
	config         Config
	gateway        GatewayConfig
	identity       *Identity
	supervisor     *Supervisor
	events         *EventSocket
	watches        []*EventSocket
	watchUp        map[string]bool
	upstreamSynced map[string]time.Time
	terminals      *TerminalRelay
	startedAt      time.Time
	mu             sync.RWMutex
	lastSyncAt     time.Time
	lastSyncOK     bool
	lastSyncErr    string
	lastPublishAt  time.Time
	lastEndpointAt time.Time
	endpointDirty  bool
	endpointRetry  bool
	// Current endpoint-state retry backoff; reset once the relay accepts.
	endpointRetryDelay time.Duration
	syncCount          int64
	repoStates         map[string]string
	// Collaboration inboxes are push-driven. The key is the lower-case
	// owner/repository named by the relay event and the value is the set of
	// inbox kinds that actually changed. Catch-up is separate because a
	// reconnect must reconcile all three kinds once.
	collaborationPending map[string]map[string]bool
	collaborationCatchUp bool
	// Controller inbox reads are event-driven. A failed catch-up stays pending,
	// but exponential retry state prevents an unrelated Git safety ticker from
	// turning a controller outage into three HTTP requests every few seconds.
	collaborationRetryAt    time.Time
	collaborationRetryDelay time.Duration
	// Website-created fork jobs use the same payload-free node event channel,
	// but their repository/source data is read from a separate signed HTTPS
	// drain. A bounded safety check makes an offline/lost event durable.
	forkJobsEnabled   bool
	forkJobsPending   bool
	forkJobsLastCheck time.Time
	// Website agent jobs queued for this node (agents.go); nil when disabled.
	agents *agentJobRunner
	// Workflow runner for the repositories this node serves; nil unless the
	// operator opted in with enableActions. See actions.go.
	actions *actionsRunner
	// Burst runner autoscaler; nil unless a core enabled runnerPool.
	pool *runnerPoolLoop
	// ForkMesh-hosted repositories (hosted.go); nil unless hosted.enabled.
	hosted         *hostedReplicator
	agentBranchOps bool
	// Public projections are much slower-moving than repository refs. Bound
	// controller requests even when the repository sync timer is aggressive.
	// The payload-free node WebSocket sets pending when public state changes;
	// lastAttempt owns only the slow dropped-event safety reconciliation.
	dashboardReplicaPending     bool
	dashboardReplicaLastAttempt time.Time
	// Per-repo state of the last successful catalog publish, so an unchanged
	// repository skips its redundant POST until the republish lease expires.
	catalogPublished map[string]catalogPublishState
	// Catalog publication has the same failure isolation as collaboration
	// intake. Local ref changes and a live node-event connection bypass it;
	// otherwise an unavailable controller is retried on a bounded backoff.
	catalogRetryAt    time.Time
	catalogRetryDelay time.Duration
	// A commit event carries the source's signed refs digest. Keep it until the
	// local mirror reaches that exact generation so a successful fetch from a
	// stale round-robin peer cannot be mistaken for convergence.
	desiredStates      map[string]string
	desiredGenerations map[string]int64
	syncRequests       chan struct{}
	// Endpoint lease retries never need repository bytes. Keeping their wake-up
	// separate prevents a pending/down tunnel from turning a seven-second health
	// retry into a full fetch of every upstream.
	publishRequests chan struct{}
	// Two direct dashboard writes must observe each other's committed number.
	collaborationWriteMu sync.Mutex
	server               *http.Server
	// One gateway generation adoption runs at a time; a change that arrives
	// mid-reload waits for it and then re-checks the acknowledgement, so it
	// never stacks a second signal or restart on top of the first.
	gatewayReloadInit sync.Once
	gatewayReloadSlot chan struct{}
	// The gateway instance last signalled and the configuration digest it was
	// signalled for, so an adoption that follows an interrupted wait does not
	// queue a redundant second reload. Only read or written while holding
	// gatewayReloadSlot.
	gatewaySignalledInstance uint64
	gatewaySignalledDigest   string
	// Spawn number of the gateway process a fail-safe restart is replacing.
	// The old process keeps listening until the supervisor delivers SIGTERM,
	// so the endpoint lease waits for a newer instance to listen.
	gatewayReplacing uint64
	// Tests shorten the acknowledgement deadline and remove restart jitter.
	gatewayReloadDeadline time.Duration
	gatewayRestartDelay   func() time.Duration
	// runnerWait overrides runnerPollDelay in tests (role runner only).
	runnerWait func(failures int) time.Duration
}

type catalogPublishState struct {
	fingerprint string
	at          time.Time
}

func (d *Daemon) catalogAttemptDue(now time.Time) bool {
	d.mu.RLock()
	defer d.mu.RUnlock()
	return d.catalogRetryAt.IsZero() || !now.Before(d.catalogRetryAt)
}

func (d *Daemon) finishCatalogAttempt(failed bool, now time.Time) {
	d.mu.Lock()
	defer d.mu.Unlock()
	if !failed {
		d.catalogRetryAt = time.Time{}
		d.catalogRetryDelay = 0
		return
	}
	if d.catalogRetryDelay <= 0 {
		d.catalogRetryDelay = controllerRetryFloor
	} else {
		d.catalogRetryDelay = min(d.catalogRetryDelay*2, controllerRetryCeil)
	}
	d.catalogRetryAt = now.Add(d.catalogRetryDelay)
}

type RuntimeStats struct {
	Goroutines int    `json:"goroutines"`
	HeapBytes  uint64 `json:"heapBytes"`
	TotalAlloc uint64 `json:"totalAllocBytes"`
}

type Status struct {
	OK             bool              `json:"ok"`
	Ready          bool              `json:"ready"`
	Node           string            `json:"node"`
	Version        string            `json:"version"`
	UptimeSeconds  int64             `json:"uptimeSeconds"`
	Processes      map[string]bool   `json:"processes"`
	Restarts       map[string]int64  `json:"restarts"`
	Repositories   map[string]string `json:"repositories"`
	LastSyncAt     string            `json:"lastSyncAt,omitempty"`
	LastSyncOK     bool              `json:"lastSyncOk"`
	LastSyncError  string            `json:"lastSyncError,omitempty"`
	LastPublishAt  string            `json:"lastPublishAt,omitempty"`
	LastEndpointAt string            `json:"lastEndpointAt,omitempty"`
	SyncCount      int64             `json:"syncCount"`
	// SyncStatus explains this node's copy in the shared
	// forkmesh-sync-status-v1 model (www/docs/sync-status.md).
	SyncStatus SyncStatus   `json:"syncStatus"`
	Runtime    RuntimeStats `json:"runtime"`
	// Recovery exposes the last recorded backup and restore drill so the
	// recovery point and recovery time objectives are observable.
	Recovery        RecoveryStatus  `json:"recovery"`
	UpstreamWatches map[string]bool `json:"upstreamWatches,omitempty"`
}

const endpointRenewInterval = 4 * time.Minute

// Gateway generation adoption. A reload builds and integrity-checks a complete
// replacement application before the gateway acknowledges it, which takes
// several seconds on a 1 vCPU node serving a multi-gigabyte repository. The
// former 3-second deadline expired on every refs change, and because a fanned
// out push reaches every mirror at once, the fail-safe hard restart took the
// whole fleet's public gateway down together for 10-15 s (2026-09-12).
const (
	// How long a live gateway may take to acknowledge a requested generation
	// before the daemon replaces the process.
	gatewayReloadTimeout = 90 * time.Second
	gatewayReloadPoll    = 50 * time.Millisecond
	// How often a not-yet-listening gateway is probed before it is signalled.
	gatewayReloadProbe = 500 * time.Millisecond
	// A genuine fail-safe restart waits a random delay below this bound so
	// mirrors that stalled on the same fanned-out change do not all drop their
	// listeners in the same instant.
	gatewayRestartJitter = 20 * time.Second
)

// How long an unchanged repository may go without re-POSTing its catalog
// record. Every timer cycle used to publish every repo unconditionally, and
// because the record embeds fresh timestamps and cpu/mem/disk samples each
// POST looked like a change to the relay. The refs digest (plus clone URL,
// version, and signing key) is what actually distinguishes two publications;
// when none of those moved, one lease renewal per interval is enough. The
// relay's own unchanged-write window sits just below this, so each renewal
// still lands as a durable lastSync refresh. Event-driven syncs still publish
// immediately: any pushed head or tag changes the refs digest and with it the
// fingerprint.
const catalogRepublishInterval = 10 * time.Minute

const (
	controllerRetryFloor = time.Minute
	controllerRetryCeil  = 30 * time.Minute
)

// Relay events are the fast path, but their notification fan-out is
// deliberately best-effort: a failed Durable Object subrequest must never
// roll back the source's already-accepted catalog publication. A connected
// mirror therefore cannot treat silence as proof that its refs are current,
// so relay-only nodes keep one fixed reconciliation cadence. It is fixed in
// both directions on purpose: every safety fetch is answered by the relay's
// ingress Durable Object, whose free-plan duration allowance is shared with
// the whole site, and a fleet of nodes polling every 15 seconds (two
// configured aliases of one repository each) kept that object busy around
// the clock until the daily allowance ran out and took the site down
// (2026-08-19). A state-bearing commit event still fetches immediately and
// the convergence retry below keeps chasing its generation; this interval
// only covers a frame that was never delivered. Third-party upstreams retain
// their configured interval because the relay cannot describe their
// generation.
var relaySafetySyncInterval = 30 * time.Minute

// A refs change rewrites the gateway configuration and restarts the gateway.
// The catalog publication can complete before that restart is serving the new
// signed digest, so its immediate endpoint-health refresh may legitimately
// observe the previous state. Retry on a bounded cadence until the serving
// state is accepted; otherwise one unlucky health check can leave the endpoint
// rejected until the normal four-minute lease renewal even though its
// repository is already current. The retry backs off toward the lease
// heartbeat: a node whose state the relay keeps reporting as pending (its refs
// lag the accepted pin, and nothing here can change that) used to re-run the
// whole mirror cycle -- two upstream fetches plus a registration the relay
// re-verifies end to end -- every seven seconds for hours.
var (
	endpointStateRetryDelay = 7 * time.Second
	endpointStateRetryCeil  = endpointRenewInterval
)

// A repository whose local mirror has never been cloned cannot answer refs, so
// the gateway refuses to serve it ("enabled public repository requires
// integrity.expectedRefsSha256") and the node never publishes to the catalog.
// Push-driven sync alone cannot recover from that: the relay only pushes when a
// source moves a head, which can be hours on a quiet repository, so a brand-new
// mirror whose first clone dropped its connection mid-pack would stay dark
// indefinitely — installed, running, and invisible. Retrying a *failed*
// operation on bounded backoff is error recovery, not a "did anything change?"
// poll, so it stays inside the no-fallback-polls rule; the retry stops the
// moment every repository has a readable clone.
var (
	bootstrapRetryFloor = 15 * time.Second
	bootstrapRetryCeil  = 5 * time.Minute
	// A state-bearing commit event is stronger than a generic wake-up: retry on
	// a short bounded backoff until the advertised generation is local. This
	// covers the gateway/cache swap where the first fetch legitimately reaches
	// a still-stale peer and exits successfully without moving a ref.
	convergenceRetryFloor = 2 * time.Second
	convergenceRetryCeil  = 15 * time.Second
)

func NewDaemon(cfg Config) (*Daemon, error) {
	gateway, _, err := loadGatewayConfig(cfg.GatewayConfig)
	if err != nil {
		return nil, err
	}
	identity, err := LoadIdentity(cfg.IdentityKey)
	if err != nil {
		return nil, err
	}
	if identity.PublicKey() != gateway.Node.PublicKey {
		return nil, errors.New("gateway identity does not match identityKey")
	}
	if !nodePattern.MatchString(gateway.Node.Name) {
		return nil, errors.New("gateway node name is invalid")
	}
	if cfg.Role == RoleRunner && len(gateway.Repositories) != 0 {
		return nil, errors.New("a runner keeps no repositories; its gateway document must list none")
	}
	if cfg.Role != RoleRunner && len(gateway.Repositories) == 0 {
		return nil, errors.New("gateway config is incomplete")
	}
	for _, repo := range gateway.Repositories {
		// "restricted" is a hosted private repository (hosted.go): served to
		// router capabilities only, never on a public route.
		if (repo.Visibility != "public" && repo.Visibility != "restricted") ||
			!repo.Enabled || !filepath.IsAbs(repo.GitDir) {
			return nil, errors.New("gateway contains an unsupported repository")
		}
	}
	var events *EventSocket
	// A pool runner has no node account: it talks to the app only through the
	// runner-key-signed /api/runners/* routes, so the node event channel would
	// answer 401 forever. It learns about work by polling claims (runner.go).
	if cfg.CatalogURL != "" && cfg.Role != RoleRunner {
		events, err = NewEventSocket(cfg.CatalogURL, gateway.Node.Name, identity)
		if err != nil {
			return nil, err
		}
	}
	daemon := &Daemon{
		config:                  cfg,
		gateway:                 gateway,
		identity:                identity,
		supervisor:              NewSupervisor(),
		events:                  events,
		startedAt:               time.Now(),
		repoStates:              map[string]string{},
		collaborationPending:    map[string]map[string]bool{},
		collaborationCatchUp:    !cfg.DisableCatalog && cfg.CatalogURL != "",
		forkJobsEnabled:         !cfg.DisableCatalog && cfg.CatalogURL != "",
		forkJobsPending:         !cfg.DisableCatalog && cfg.CatalogURL != "",
		desiredStates:           map[string]string{},
		desiredGenerations:      map[string]int64{},
		dashboardReplicaPending: true,
		syncRequests:            make(chan struct{}, 1),
		publishRequests:         make(chan struct{}, 1),
	}
	if cfg.Role == RoleCore {
		// Also when the config did not come through LoadConfig.
		cfg.DisableAgentJobs, cfg.EnableActions = true, false
		daemon.config = cfg
	}
	for _, switched := range cfg.roleOverrides {
		log.Printf("role %s: %s forced off (trusted core runs no untrusted code)", cfg.Role, switched)
	}
	switch {
	case cfg.Role == RoleRunner:
		daemon.agents, err = newRunnerAgentJobs(daemon)
		if err != nil {
			return nil, err
		}
		daemon.collaborationCatchUp = false
		daemon.forkJobsEnabled, daemon.forkJobsPending = false, false
		daemon.dashboardReplicaPending = false
	case !cfg.DisableCatalog && cfg.CatalogURL != "" && !cfg.DisableAgentJobs:
		daemon.agents = newMirrorAgentJobs(daemon)
	}
	if cfg.EnableActions {
		if cfg.Role != RoleRunner {
			daemon.actions = newActionsRunner(daemon)
		}
	}
	if daemon.hosted, err = newHostedReplicator(daemon); err != nil {
		return nil, err
	}
	if pool := cfg.RunnerPool; pool != nil && pool.Enabled && cfg.Role == RoleCore {
		daemon.pool, err = newRunnerPoolLoop(daemon)
		if err != nil {
			return nil, err
		}
	}
	if err := daemon.setUpstreamWatches(); err != nil {
		return nil, err
	}
	if events != nil {
		events.Handler = daemon.handleNodeEvent
		// The owner's browser signs this node's agent CLI in over the same
		// socket. The relay only forwards; this node decides what runs, and
		// the one thing it will run is the provider's own login command.
		daemon.terminals = NewTerminalRelay(
			gateway.Node.Name, cfg.DisableAgentLogin, events.SendTerminal)
		daemon.terminals.logf = func(format string, args ...any) {
			log.Printf(format, args...)
		}
		events.Terminal = daemon.terminals.Handle
	}
	return daemon, nil
}

func (d *Daemon) handleNodeEvent(event Event) {
	d.mu.Lock()
	forkEvent := event.Topic == "forks" && d.forkJobsEnabled
	// Only the explicit projection doorbell (and the older fork event during a
	// mixed-version rollout) invalidates the controller-backed bundle. Commits
	// and collaboration events reconcile their Git-authoritative local stores;
	// fetching the unrelated public bundle for those events recreates polling
	// pressure during active desktop work.
	dashboardEvent := event.Topic == "replica" || forkEvent
	agentEvent := event.Topic == "org-agents" && d.agents != nil
	hostedEvent := event.Topic == "hosted" && d.hosted != nil
	syncEvent := dashboardEvent || agentEvent || hostedEvent ||
		event.Topic == "commits" || isCollaborationKind(event.Topic)
	if event.CatchUp {
		d.collaborationCatchUp = true
		d.collaborationRetryAt = time.Time{}
		d.collaborationRetryDelay = 0
		d.catalogRetryAt = time.Time{}
		d.catalogRetryDelay = 0
		d.dashboardReplicaPending = true
		if d.forkJobsEnabled {
			d.forkJobsPending = true
		}
	}
	if dashboardEvent {
		d.dashboardReplicaPending = true
	}
	if event.Topic == "commits" && digestPattern.MatchString(event.State) {
		key := strings.ToLower(strings.TrimSpace(event.Repo))
		if key != "" {
			if d.desiredStates == nil {
				d.desiredStates = map[string]string{}
			}
			if d.desiredGenerations == nil {
				d.desiredGenerations = map[string]int64{}
			}
			priorState, pending := d.desiredStates[key]
			priorGeneration := d.desiredGenerations[key]
			// Durable Objects replay the latest source generation when a node
			// reconnects. Ignore an older replay, or the same generation when its
			// target is already pending/landed, so it cannot replace newer work or
			// trigger a needless Git fetch. Distinct states with the same millisecond
			// generation remain valid: two rapid pushes can share a clock tick.
			sameGenerationState := event.Generation == priorGeneration &&
				((pending && priorState == event.State) ||
					(!pending && d.repoStates[key] == event.State))
			staleReplay := event.Generation > 0 && priorGeneration > 0 &&
				(event.Generation < priorGeneration ||
					sameGenerationState)
			if staleReplay {
				syncEvent = dashboardEvent || isCollaborationKind(event.Topic)
			} else {
				d.desiredStates[key] = event.State
				if event.Generation > 0 {
					d.desiredGenerations[key] = event.Generation
				}
			}
		}
	}
	if isCollaborationKind(event.Topic) {
		// Receiving the doorbell proves the controller socket is live. Let the
		// named inbox bypass any backoff retained from an older outage.
		d.collaborationRetryAt = time.Time{}
		d.collaborationRetryDelay = 0
		key := strings.ToLower(strings.TrimSpace(event.Repo))
		if key != "" {
			if d.collaborationPending == nil {
				d.collaborationPending = map[string]map[string]bool{}
			}
			if d.collaborationPending[key] == nil {
				d.collaborationPending[key] = map[string]bool{}
			}
			d.collaborationPending[key][event.Topic] = true
		}
	}
	if event.Topic == "commits" {
		d.catalogRetryAt = time.Time{}
		d.catalogRetryDelay = 0
	}
	if forkEvent {
		d.forkJobsPending = true
	}
	d.mu.Unlock()
	if d.hosted != nil && (event.CatchUp || hostedEvent) {
		d.hosted.request()
	}
	if d.agents != nil && (event.CatchUp || agentEvent) {
		d.agents.request()
	}
	if event.CatchUp || syncEvent {
		d.requestSync()
	}
}

func (d *Daemon) desiredStatesFor(repo Repository) []string {
	d.mu.RLock()
	defer d.mu.RUnlock()
	name := strings.ToLower(strings.TrimSpace(repo.Name))
	var states []string
	for key, state := range d.desiredStates {
		parts := strings.SplitN(key, "/", 2)
		if len(parts) == 2 && parts[1] == name {
			states = append(states, state)
		}
	}
	sort.Strings(states)
	return states
}

func (d *Daemon) requestSync() {
	select {
	case d.syncRequests <- struct{}{}:
	default:
	}
}

func (d *Daemon) requestPublish() {
	select {
	case d.publishRequests <- struct{}{}:
	default:
	}
}

func (d *Daemon) markEndpointStateDirty() {
	d.mu.Lock()
	d.endpointDirty = true
	if d.endpointRetry {
		d.mu.Unlock()
		return
	}
	d.endpointRetry = true
	switch {
	case d.endpointRetryDelay <= 0:
		d.endpointRetryDelay = endpointStateRetryDelay
	case d.endpointRetryDelay < endpointStateRetryCeil:
		d.endpointRetryDelay = min(d.endpointRetryDelay*2, endpointStateRetryCeil)
	}
	delay := d.endpointRetryDelay
	d.mu.Unlock()
	time.AfterFunc(delay, func() {
		d.mu.Lock()
		d.endpointRetry = false
		dirty := d.endpointDirty
		d.mu.Unlock()
		if dirty {
			d.requestPublish()
		}
	})
}

func (d *Daemon) Run(ctx context.Context, executable string) error {
	if !filepath.IsAbs(executable) {
		return errors.New("executable path must be absolute")
	}
	if d.config.Role == RoleRunner {
		// No gateway, tunnel, catalog, merge executor or repositories.
		return d.runRunner(ctx)
	}
	if d.pool != nil {
		go d.pool.run(ctx)
	}
	d.gateway.HealthSignerCommand = []string{executable, "--config", d.configPath(), "--sign-mirror-health"}
	d.gateway.RequestVerifierCommand = []string{executable, "--config", d.configPath(), "--verify-mirror-capability"}
	// Upgrade the served capability set in place: owner-authorized web merges
	// reach this node through the gateway's merge-pull route, which hands the
	// typed request to this same binary (merge.go). Operators keep a read-only
	// node with disableMergeExecutor.
	ensureGatewayCapabilities(&d.gateway,
		[]string{executable, "--config", d.configPath(), "--execute-mirror-merge"},
		!d.config.DisableMergeExecutor)
	d.agentBranchOps = !d.config.DisableMergeExecutor &&
		gatewayScriptSupports(d.config.GatewayScript, agentBranchMergeOp)
	ensureGatewayAgentBranchOperations(&d.gateway, d.agentBranchOps)
	ensureReadOnlyMirrorOperations(&d.gateway, d.readOnlyMirrorDirs())
	// The Actions operations are advertised only by a node that was asked to
	// run workflows and has just proved it can sandbox them. Every other node
	// has them stripped, so switching enableActions off withdraws them.
	if err := ensureGatewayActions(&d.gateway, d.config.GatewayConfig,
		d.actions != nil && d.actions.ready(ctx)); err != nil {
		return err
	}
	if d.gateway.ActionsSummaryPath != "" {
		// The gateway is about to start and reads this path; give it a
		// current document rather than a missing file.
		d.actions.writeSummary(ctx)
	}
	d.gateway.CollaborationControlURL = "http://" + d.config.Listen +
		"/v1/control/collaboration"
	if _, err := writeGatewayConfig(d.config.GatewayConfig, d.gateway); err != nil {
		return err
	}
	specs := []ProcessSpec{{Name: "gateway", Program: d.config.Python, Args: []string{d.config.GatewayScript, "--config", d.config.GatewayConfig}}}
	if !d.config.DisableCloudflared {
		environment, err := tokenEnvironment(d.config.ConnectorToken)
		if err != nil {
			return err
		}
		specs = append(specs, ProcessSpec{Name: "cloudflared", Program: d.config.Cloudflared, Args: []string{"tunnel", "--no-autoupdate", "--protocol", "http2", "run"}, Env: environment, Output: io.Discard})
	}
	mux := http.NewServeMux()
	mux.HandleFunc("/healthz", d.handleHealth)
	mux.HandleFunc("/readyz", d.handleReady)
	mux.HandleFunc("/v1/status", d.handleStatus)
	mux.HandleFunc("/v1/control/sync", d.handleSync)
	mux.HandleFunc("/v1/control/collaboration", d.handleCollaboration)
	d.server = &http.Server{Addr: d.config.Listen, Handler: localRequestOnly(mux), ReadHeaderTimeout: 5 * time.Second, IdleTimeout: 30 * time.Second}
	listener, err := net.Listen("tcp", d.config.Listen)
	if err != nil {
		return err
	}
	serverErr := make(chan error, 1)
	go func() { serverErr <- d.server.Serve(listener) }()
	if d.agents != nil {
		d.agents.runCtx = ctx
	}
	d.syncOnce(ctx)
	if d.actions != nil {
		go d.actions.run(ctx)
	}
	if d.hosted != nil {
		go d.hosted.maintainLoop(ctx)
	}
	go d.supervisor.Run(ctx, specs)
	if d.events != nil {
		go d.events.Run(ctx)
	}
	for _, watch := range d.watches {
		go watch.Run(ctx)
	}
	go d.syncLoop(ctx)
	select {
	case <-ctx.Done():
		d.supervisor.Stop()
		d.terminals.Stop("")
		shutdownCtx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		_ = d.server.Shutdown(shutdownCtx)
		return nil
	case err := <-serverErr:
		if errors.Is(err, http.ErrServerClosed) {
			return nil
		}
		return err
	}
}

func localRequestOnly(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, request *http.Request) {
		if !isLoopbackHTTPHost(request.Host) ||
			strings.EqualFold(request.Header.Get("Sec-Fetch-Site"), "cross-site") ||
			!isLocalOrigin(request.Header.Get("Origin")) {
			writeJSON(w, http.StatusForbidden,
				map[string]any{"ok": false, "error": "local request required"})
			return
		}
		next.ServeHTTP(w, request)
	})
}

func isLoopbackHTTPHost(value string) bool {
	host := strings.TrimSpace(value)
	if parsed, _, err := net.SplitHostPort(host); err == nil {
		host = parsed
	}
	host = strings.Trim(host, "[]")
	if strings.EqualFold(host, "localhost") {
		return true
	}
	ip := net.ParseIP(host)
	return ip != nil && ip.IsLoopback()
}

func isLocalOrigin(value string) bool {
	value = strings.TrimSpace(value)
	if value == "" {
		return true // native clients and health probes do not send Origin
	}
	parsed, err := url.Parse(value)
	return err == nil && (parsed.Scheme == "http" || parsed.Scheme == "https") &&
		isLoopbackHTTPHost(parsed.Host)
}

func (d *Daemon) configPath() string { return os.Getenv("FORKMESH_MIRROR_NODE_CONFIG") }

// hasExternalUpstreams reports whether any configured upstream lives outside
// the relay. The relay pushes a "commits" event the moment a source publishes
// changed heads or tags for a relay-hosted upstream, so only third-party
// upstreams — which cannot push — still justify a periodic fetch.
func (d *Daemon) hasExternalUpstreams() bool {
	catalog, err := url.Parse(d.config.CatalogURL)
	if err != nil || catalog.Host == "" {
		return true
	}
	relayHosts := []string{catalog.Host}
	eventDriven := make(map[string]struct{}, len(d.config.EventDrivenUpstreams))
	for _, upstream := range d.config.EventDrivenUpstreams {
		eventDriven[upstream] = struct{}{}
	}
	// The relay answers on its app./api. host and on the site apex it serves,
	// so a catalog at app.<apex> or api.<apex> makes all three internal. The
	// apex is derived from the configured catalog host; no deployment's
	// hostnames are baked in.
	lowerHost := strings.ToLower(catalog.Host)
	for _, prefix := range []string{"app.", "api."} {
		if apex := strings.TrimPrefix(lowerHost, prefix); apex != lowerHost && strings.Contains(apex, ".") {
			relayHosts = append(relayHosts, apex, "app."+apex, "api."+apex)
			break
		}
	}
	for _, upstreams := range d.config.Upstreams {
		for _, upstream := range upstreams {
			if _, ok := eventDriven[upstream]; ok && sameRelay(d.config.CatalogURL, upstream) {
				continue
			}
			parsed, err := url.Parse(upstream)
			if err != nil || parsed.Host == "" {
				return true
			}
			internal := false
			for _, relayHost := range relayHosts {
				if strings.EqualFold(parsed.Host, relayHost) {
					internal = true
					break
				}
			}
			if !internal {
				return true
			}
		}
	}
	return false
}

// fetchInterval keeps push events as the normal low-latency path while
// guaranteeing eventual convergence after a best-effort notification is lost.
// A mixed node uses the configured interval because at least one upstream has
// no event source; an all-relay node always reconciles on the fixed safety
// cadence, whatever its configured interval says.
func (d *Daemon) fetchInterval() time.Duration {
	if d.events != nil && !d.config.DisableCatalog &&
		!d.hasExternalUpstreams() {
		return relaySafetySyncInterval
	}
	return d.config.SyncInterval.Duration
}

// convergencePending clears state targets that have landed and reports
// whether any source generation still needs another fetch. Modern bootstrap
// configs expose both the canonical and node-owner aliases for one Git dir, so
// exact keys normally match. The name fallback keeps upgraded legacy configs
// self-healing without conflating two configured repositories: it is accepted
// only when exactly one local repository with that name has the target state.
func (d *Daemon) convergencePending() bool {
	d.mu.Lock()
	defer d.mu.Unlock()
	for wantedKey, wantedState := range d.desiredStates {
		if d.repoStates[wantedKey] == wantedState {
			delete(d.desiredStates, wantedKey)
			continue
		}
		parts := strings.SplitN(wantedKey, "/", 2)
		if len(parts) != 2 {
			continue
		}
		matches := 0
		for localKey, localState := range d.repoStates {
			localParts := strings.SplitN(localKey, "/", 2)
			if len(localParts) == 2 && localParts[1] == parts[1] &&
				localState == wantedState {
				matches++
			}
		}
		if matches == 1 {
			delete(d.desiredStates, wantedKey)
		}
	}
	return len(d.desiredStates) > 0
}

// bootstrapPending reports whether any configured repository still has no
// readable local clone, so the last cycle could not derive its refs digest.
// Only the sync goroutine calls it, which is also the only writer of the
// gateway repositories it walks.
func (d *Daemon) bootstrapPending() bool {
	d.mu.RLock()
	states := d.repoStates
	d.mu.RUnlock()
	for i := range d.gateway.Repositories {
		if states[repositoryKey(d.gateway.Repositories[i])] == "" {
			return true
		}
	}
	return false
}

func (d *Daemon) syncLoop(ctx context.Context) {
	// Every node retains a bounded reconciliation pass. For relay-hosted
	// upstreams this is only the dropped-event safety net; commit events still
	// enter syncRequests below and fetch immediately.
	fetchTicker := time.NewTicker(d.fetchInterval())
	defer fetchTicker.Stop()
	heartbeat := time.NewTicker(endpointRenewInterval)
	defer heartbeat.Stop()
	bootstrap := time.NewTimer(bootstrapRetryFloor)
	defer bootstrap.Stop()
	if !bootstrap.Stop() {
		<-bootstrap.C
	}
	bootstrapDelay := time.Duration(0)
	bootstrapArmed := false
	convergence := time.NewTimer(convergenceRetryFloor)
	defer convergence.Stop()
	if !convergence.Stop() {
		<-convergence.C
	}
	convergenceDelay := time.Duration(0)
	convergenceArmed := false
	releasePrune := time.NewTimer(releasePruneFirstDelay)
	defer releasePrune.Stop()
	// Re-armed after every cycle, but only when it is not already pending: any
	// other cycle — a push, the four-minute heartbeat — must not push the retry
	// back, or a backoff at the ceiling would be reset before it ever fired.
	// Each failed attempt backs off toward the ceiling instead of hammering the
	// upstream that just dropped a multi-hundred-megabyte pack; a clone that
	// lands disarms the retry entirely.
	armBootstrap := func() {
		if !d.bootstrapPending() {
			if bootstrapArmed && !bootstrap.Stop() {
				select {
				case <-bootstrap.C:
				default:
				}
			}
			if bootstrapDelay > 0 {
				log.Printf("initial mirror clone landed; bootstrap retries stopped")
			}
			bootstrapArmed = false
			bootstrapDelay = 0
			return
		}
		if bootstrapArmed {
			return
		}
		switch {
		case bootstrapDelay <= 0:
			bootstrapDelay = bootstrapRetryFloor
		case bootstrapDelay < bootstrapRetryCeil:
			bootstrapDelay = min(bootstrapDelay*2, bootstrapRetryCeil)
		}
		log.Printf("mirror has no local clone yet; retrying the initial clone in %s", bootstrapDelay)
		bootstrap.Reset(bootstrapDelay)
		bootstrapArmed = true
	}
	armConvergence := func() {
		if !d.convergencePending() {
			if convergenceArmed && !convergence.Stop() {
				select {
				case <-convergence.C:
				default:
				}
			}
			convergenceArmed = false
			convergenceDelay = 0
			return
		}
		if convergenceArmed {
			return
		}
		switch {
		case convergenceDelay <= 0:
			convergenceDelay = convergenceRetryFloor
		case convergenceDelay < convergenceRetryCeil:
			convergenceDelay = min(
				convergenceDelay*2, convergenceRetryCeil)
		}
		convergence.Reset(convergenceDelay)
		convergenceArmed = true
	}
	armBootstrap()
	armConvergence()
	for {
		select {
		case <-ctx.Done():
			return
		case <-fetchTicker.C:
			d.syncOnce(ctx)
		case <-heartbeat.C:
			d.publishOnce(ctx)
		case <-d.syncRequests:
			d.syncOnce(ctx)
		case <-d.publishRequests:
			d.publishOnce(ctx)
		case <-bootstrap.C:
			bootstrapArmed = false
			d.syncOnce(ctx)
		case <-convergence.C:
			convergenceArmed = false
			d.syncOnce(ctx)
		case <-releasePrune.C:
			d.pruneReleaseStores(ctx)
			releasePrune.Reset(d.config.releasePruneInterval())
		}
		armBootstrap()
		armConvergence()
	}
}

func (d *Daemon) syncOnce(parent context.Context)    { d.cycle(parent, true) }
func (d *Daemon) publishOnce(parent context.Context) { d.cycle(parent, false) }

func (d *Daemon) publishEndpoint(
	ctx context.Context, publisher *EndpointPublisher,
) (bool, error) {
	if d.events != nil {
		payload, err := publisher.Payload()
		if err != nil {
			return false, err
		}
		requestID := fmt.Sprint(payload["issuedAt"])
		status, raw, delivered := d.events.Request(
			ctx, "endpoint", requestID, payload)
		if delivered {
			return publisher.ValidateResult(status, raw)
		}
	}
	return publisher.Publish(ctx)
}

type gatewayReloadOutcome int

const (
	// The running gateway acknowledged the exact on-disk configuration.
	gatewayReloadServed gatewayReloadOutcome = iota
	// No gateway process is running (not started yet, or exited and awaiting
	// the supervisor's respawn). The next process loads the on-disk
	// configuration at startup and acknowledges it itself.
	gatewayReloadUnavailable
	// The caller's context ended while a live gateway was still reloading. The
	// reload continues; a later cycle re-checks the acknowledgement.
	gatewayReloadInterrupted
	// A live gateway never acknowledged within the deadline.
	gatewayReloadStalled
	// A stalled gateway was handed to the supervisor for replacement.
	gatewayReloadRestarted
)

func (d *Daemon) acquireGatewayReload(ctx context.Context) (func(), bool) {
	d.gatewayReloadInit.Do(func() { d.gatewayReloadSlot = make(chan struct{}, 1) })
	select {
	case d.gatewayReloadSlot <- struct{}{}:
		return func() { <-d.gatewayReloadSlot }, true
	case <-ctx.Done():
		return nil, false
	}
}

func (d *Daemon) reloadDeadline() time.Duration {
	if d.gatewayReloadDeadline > 0 {
		return d.gatewayReloadDeadline
	}
	return gatewayReloadTimeout
}

func (d *Daemon) restartDelay() time.Duration {
	if d.gatewayRestartDelay != nil {
		return d.gatewayRestartDelay()
	}
	return rand.N(gatewayRestartJitter)
}

// reloadGatewayGeneration asks the live gateway to adopt the on-disk
// configuration and reports whether it acknowledged that exact generation. It
// never restarts the gateway.
func (d *Daemon) reloadGatewayGeneration(ctx context.Context) bool {
	release, ok := d.acquireGatewayReload(ctx)
	if !ok {
		return false
	}
	defer release()
	return d.awaitGatewayGeneration(ctx) == gatewayReloadServed
}

// adoptGatewayGeneration is reloadGatewayGeneration plus the bounded fail-safe:
// only a live gateway that never acknowledges within the long deadline is
// replaced, after a per-node random delay.
func (d *Daemon) adoptGatewayGeneration(ctx context.Context) gatewayReloadOutcome {
	release, ok := d.acquireGatewayReload(ctx)
	if !ok {
		return gatewayReloadInterrupted
	}
	defer release()
	outcome := d.awaitGatewayGeneration(ctx)
	if outcome != gatewayReloadStalled {
		return outcome
	}
	delay := d.restartDelay()
	log.Printf("gateway did not acknowledge its configuration generation within %s; restarting it in %s",
		d.reloadDeadline(), delay.Round(time.Millisecond))
	if delay > 0 {
		timer := time.NewTimer(delay)
		select {
		case <-ctx.Done():
			timer.Stop()
			return gatewayReloadInterrupted
		case <-timer.C:
		}
	}
	if gatewayServesCurrentGeneration(d.config.GatewayConfig) {
		return gatewayReloadServed
	}
	if !d.restartGateway() {
		return gatewayReloadUnavailable
	}
	return gatewayReloadRestarted
}

// awaitGatewayGeneration signals the running gateway once it listens (its
// SIGHUP handler is installed by then) and waits for the acknowledgement. A
// slow reload is not a failure: while the same live process is still working
// the daemon keeps waiting until the long deadline. A process that exits is
// the supervisor's to respawn, and a respawned process acknowledges the
// configuration it loads at startup, so it is not signalled for that
// configuration again.
//
// The gateway snapshots the configuration when its reload starts. When the
// file changes again mid-reload, the acknowledgement will name the older
// snapshot, so the waiter re-signals once for the newer bytes (the supervisor
// and the gateway both coalesce a signal that lands during a reload) and gives
// that generation a fresh deadline.
func (d *Daemon) awaitGatewayGeneration(ctx context.Context) gatewayReloadOutcome {
	path := d.config.GatewayConfig
	if gatewayServesCurrentGeneration(path) {
		return gatewayReloadServed
	}
	instance, running := d.supervisor.Instance("gateway")
	if !running {
		return gatewayReloadUnavailable
	}
	started := time.Now()
	deadline := started.Add(d.reloadDeadline())
	signalled := instance == d.gatewaySignalledInstance && d.gatewaySignalledDigest != ""
	signalledFor := ""
	if signalled {
		signalledFor = d.gatewaySignalledDigest
	}
	defer func() {
		if signalled {
			d.gatewaySignalledInstance, d.gatewaySignalledDigest = instance, signalledFor
		}
	}()
	nextProbe := started
	ticker := time.NewTicker(gatewayReloadPoll)
	defer ticker.Stop()
	for {
		wanted := gatewayConfigDigest(path)
		if wanted != "" && gatewayAcknowledgedGeneration(path) == wanted {
			if elapsed := time.Since(started); elapsed >= 3*time.Second {
				log.Printf("gateway adopted its configuration generation after %s",
					elapsed.Round(10*time.Millisecond))
			}
			return gatewayReloadServed
		}
		current, running := d.supervisor.Instance("gateway")
		if !running {
			return gatewayReloadUnavailable
		}
		now := time.Now()
		if current != instance {
			// A replacement started after this wait began, so it loads at
			// least these bytes during startup and acknowledges them itself.
			instance, signalled, signalledFor = current, true, wanted
		}
		if signalled && wanted != signalledFor {
			signalled = false
			nextProbe = now
			deadline = now.Add(d.reloadDeadline())
		}
		// Probe only until the signal is sent: a gateway busy swapping its
		// generation is not accepting, and repeated probes would fill its
		// listen backlog for real visitors.
		if !signalled && !now.Before(nextProbe) {
			nextProbe = now.Add(gatewayReloadProbe)
			if d.gatewayListening() && d.supervisor.Reload("gateway") {
				signalled, signalledFor = true, wanted
			}
		}
		if !now.Before(deadline) {
			return gatewayReloadStalled
		}
		select {
		case <-ctx.Done():
			return gatewayReloadInterrupted
		case <-ticker.C:
		}
	}
}

// gatewayAcknowledgedGeneration is the configuration digest the running
// gateway last acknowledged, or "" when there is no valid acknowledgement.
func gatewayAcknowledgedGeneration(gatewayConfigPath string) string {
	raw, err := os.ReadFile(gatewayGenerationMarkerPath(gatewayConfigPath))
	if err != nil {
		return ""
	}
	marker := strings.TrimSpace(string(raw))
	if !digestPattern.MatchString(marker) {
		return ""
	}
	return marker
}

// restartGateway hands the running gateway to the supervisor for replacement
// and holds the endpoint lease until a newer process listens.
func (d *Daemon) restartGateway() bool {
	instance, running := d.supervisor.Instance("gateway")
	if !running || !d.supervisor.Restart("gateway") {
		return false
	}
	d.mu.Lock()
	d.gatewayReplacing = instance
	d.mu.Unlock()
	return true
}

func (d *Daemon) cycle(parent context.Context, fetch bool) {
	ctx, cancel := context.WithTimeout(parent, d.config.SyncTimeout.Duration)
	defer cancel()
	var failures []string
	// A failed refresh is operationally useful as a warning, but it must not
	// withdraw a mirror whose verified local generation is still readable. The
	// router readiness bit tracks serving safety; lastSyncErr separately retains
	// upstream, collaboration, and catalog failures for diagnosis and retry.
	servingOK := true
	if fetch && d.shouldCheckForkJobs() {
		if err := d.drainForkJobs(ctx); err != nil {
			failures = append(failures, "website forks: "+err.Error())
		}
	}
	if fetch && d.hosted != nil && d.hosted.due(time.Now()) {
		if err := d.reconcileHosted(ctx); err != nil {
			failures = append(failures, "hosted repositories: "+err.Error())
		}
	}
	if fetch && d.agents != nil && d.agents.due(time.Now()) {
		repositories := append([]Repository(nil), d.gateway.Repositories...)
		if err := d.agents.drain(ctx, repositories); err != nil {
			failures = append(failures, "website agents: "+err.Error())
		}
	}
	catchUp := false
	collaboration := map[string]map[string]bool{}
	refreshDashboard := fetch && d.dashboardReplicaRefreshDue(time.Now())
	if fetch {
		catchUp, collaboration = d.takeCollaborationRequests()
	}
	collaborationAttempted := catchUp || len(collaboration) > 0
	collaborationFailed := false
	changed := false
	states := map[string]string{}
	seenDirs := map[string]bool{}
	retiredDirs := map[string]bool{}
	for i := range d.gateway.Repositories {
		repo := &d.gateway.Repositories[i]
		key := repositoryKey(*repo)
		upstreams := d.upstreamsFor(*repo)
		if fetch && !seenDirs[repo.GitDir] && len(upstreams) > 0 {
			seenDirs[repo.GitDir] = true
			if err := syncRepository(ctx, *repo, upstreams, d.gateway.Node.Name, d.desiredStatesFor(*repo)...); err != nil {
				failures = append(failures, key+": "+err.Error())
			} else {
				d.noteUpstreamSync(repo.GitDir, time.Now())
			}
		}
		kinds := collaboration[strings.ToLower(key)]
		if catchUp && d.preferredCollaborationAlias(*repo) {
			kinds = allCollaborationKinds()
		}
		if fetch && len(kinds) > 0 {
			for _, kind := range collaborationKinds(kinds) {
				if err := d.drainCollaborationKind(ctx, *repo, kind); err != nil {
					failures = append(failures, "collaboration "+key+"/"+kind+": "+err.Error())
					d.requeueCollaboration(key, kind)
					collaborationFailed = true
				}
			}
		}
		if fetch && !retiredDirs[repo.GitDir] {
			retiredDirs[repo.GitDir] = true
			if _, err := retireMergedPullBranches(ctx, repo.GitDir, d.gateway.Node.Name); err != nil {
				failures = append(failures, "merged pull branches "+key+": "+err.Error())
			}
			if _, err := ensurePullHeadRefs(ctx, repo.GitDir); err != nil {
				failures = append(failures, "pull heads "+key+": "+err.Error())
			}
		}
		// Heal an unborn HEAD even when every upstream is unreachable: the
		// catalog record resolves HEAD, and a node that cannot publish is
		// invisible to the relay no matter how healthy its refs are.
		ensureHeadResolves(ctx, repo.GitDir)
		digest, err := refsSHA256(ctx, repo.GitDir)
		if err != nil {
			failures = append(failures, key+": "+err.Error())
			servingOK = false
			continue
		}
		states[key] = digest
		if repo.Integrity.ExpectedRefsSHA256 != digest {
			repo.Integrity.ExpectedRefsSHA256 = digest
			changed = true
		}
	}
	if collaborationAttempted {
		d.finishCollaborationAttempt(collaborationFailed, time.Now())
	}
	// A direct Git receive has already changed the local repository and cannot
	// be discovered from the relay event channel. Keep a service-owned
	// post-receive doorbell on every native repository so the receive queues
	// this cycle immediately instead of waiting for the safety reconciliation.
	if err := d.ensurePushWakeHooks(); err != nil {
		failures = append(failures, err.Error())
	}
	// Every repository was just synchronized: let the workflow runner look
	// for branch heads that moved.
	if d.actions != nil {
		d.actions.requestRefScan()
	}
	if changed {
		if _, err := writeGatewayConfig(d.config.GatewayConfig, d.gateway); err != nil {
			failures = append(failures, "write gateway config: "+err.Error())
			servingOK = false
		} else {
			d.markEndpointStateDirty()
			// Adopt an ordinary fetched generation through the same validated
			// in-process replacement used after an owner-authorized merge. The
			// listener remains up while requests already using the prior
			// generation finish. Only a live gateway that never acknowledges
			// within the long deadline is restarted, after a random delay.
			d.adoptGatewayGeneration(ctx)
		}
	} else if servingOK && !gatewayServesCurrentGeneration(d.config.GatewayConfig) {
		// An earlier adoption was interrupted by its context, or a collaboration
		// write's reload did not finish. Nothing else would ever re-signal the
		// gateway until the next refs change, so converge here.
		d.adoptGatewayGeneration(ctx)
	}
	if !d.config.DisableCatalog {
		catalogAllowed := d.catalogAttemptDue(time.Now()) || changed
		catalogAttempted := false
		catalogFailed := false
		mirrorURL := d.config.MirrorURL
		if mirrorURL == "" && d.config.DisableCloudflared {
			mirrorURL = d.gateway.PublicOrigin
		}
		publisher := &CatalogPublisher{URL: d.config.CatalogURL, Owner: d.config.PublishOwner, Node: d.gateway.Node.Name, Version: d.config.Version, Identity: d.identity, MirrorURL: mirrorURL, DisableAgents: d.agents == nil}
		published := false
		for _, repo := range d.gateway.Repositories {
			catalogRepositoryOwner := d.config.PublishOwner
			if d.config.CanonicalOwner != "" {
				catalogRepositoryOwner = d.config.CanonicalOwner
			}
			if repo.Owner != catalogRepositoryOwner || d.hosted.isHosted(repo) {
				continue
			}
			if upstreams := d.upstreamsFor(repo); len(upstreams) > 0 {
				publisher.CloneURL = upstreams[0]
			} else {
				publisher.CloneURL = ""
			}
			publisher.MirrorOf = d.mirrorOfFor(repo)
			publisher.LastSyncedAt = d.upstreamSyncedAt(repo.GitDir)
			key := repositoryKey(repo)
			fingerprint := strings.Join([]string{
				states[key], publisher.CloneURL, publisher.MirrorURL, publisher.MirrorOf,
				repo.ForkedFrom, d.config.Version, d.identity.PublicKey(),
				strconv.Itoa(releaseArtifactCount(repo.ReleaseStore)),
			}, "\n")
			d.mu.RLock()
			prior, known := d.catalogPublished[key]
			d.mu.RUnlock()
			if known && prior.fingerprint == fingerprint &&
				time.Since(prior.at) < catalogRepublishInterval {
				// The relay already holds this exact publication and its
				// lease is fresh; the row still counts as published for the
				// endpoint renewal below.
				published = true
				continue
			}
			if !catalogAllowed {
				continue
			}
			catalogAttempted = true
			if err := publisher.Publish(ctx, repo, states[key]); err != nil {
				failures = append(failures, "catalog "+key+": "+err.Error())
				catalogFailed = true
			} else {
				published = true
				d.mu.Lock()
				d.lastPublishAt = time.Now()
				if d.catalogPublished == nil {
					d.catalogPublished = map[string]catalogPublishState{}
				}
				d.catalogPublished[key] = catalogPublishState{
					fingerprint: fingerprint, at: time.Now()}
				d.mu.Unlock()
			}
		}
		if catalogAttempted {
			d.finishCatalogAttempt(catalogFailed, time.Now())
		}
		d.mu.RLock()
		lastEndpointAt := d.lastEndpointAt
		endpointDirty := d.endpointDirty
		d.mu.RUnlock()
		// Run() performs its first repository cycle before starting the
		// supervised gateway. Arm the same bounded post-restart retry on a
		// fresh process, or a healthy restart can leave lastEndpointAt empty
		// until the four-minute heartbeat even though publication succeeded.
		if published && lastEndpointAt.IsZero() && !endpointDirty {
			d.markEndpointStateDirty()
			endpointDirty = true
		}
		running, _ := d.supervisor.Snapshot()
		leaseDue := endpointDirty || lastEndpointAt.IsZero() ||
			time.Since(lastEndpointAt) >= endpointRenewInterval
		publicServing := endpointLeaseAllowed(
			running, d.config.DisableCloudflared, d.gatewayServing())
		if published && leaseDue && !publicServing && running["gateway"] {
			// The gateway was just (re)started by the configuration change
			// above and is not listening yet. Registering now makes the relay
			// fetch this node's manifest and health through an origin that
			// answers 502, and the lease is rejected as invalid_manifest or
			// unreachable_mirror_url -- every refs change did this on
			// 2026-09-09/10. Keep the state dirty and retry once it listens.
			d.markEndpointStateDirty()
		}
		if published && publicServing && leaseDue {
			endpoint := &EndpointPublisher{
				CatalogURL: d.config.CatalogURL,
				Node:       d.gateway.Node.Name,
				BaseURL:    d.gateway.PublicOrigin,
				MirrorURL:  mirrorURL,
				Version:    d.config.Version,
				Identity:   d.identity,
			}
			active, err := d.publishEndpoint(ctx, endpoint)
			if err != nil {
				failures = append(failures, "endpoint lease: "+err.Error())
			} else if active {
				d.mu.Lock()
				d.lastEndpointAt = time.Now()
				d.endpointDirty = false
				d.endpointRetryDelay = 0
				d.mu.Unlock()
			}
		}
		// The gateway may still be between generations, or the relay may report
		// a transient pending/rejected health result. Keep retrying while the
		// local state is dirty instead of waiting for the lease heartbeat.
		d.mu.RLock()
		retryEndpoint := d.endpointDirty
		d.mu.RUnlock()
		if published && retryEndpoint {
			d.markEndpointStateDirty()
		}
	}
	if refreshDashboard {
		// Public dashboard documents are a last-good cache, independent of
		// serving safety. A controller outage leaves the validated local bundle
		// in place and is reported only as a cycle warning.
		if err := d.refreshDashboardReplica(ctx); err != nil {
			failures = append(failures, "dashboard replica: "+err.Error())
		}
	}
	d.mu.Lock()
	d.lastSyncAt = time.Now()
	d.syncCount++
	d.lastSyncOK = servingOK
	d.lastSyncErr = boundedText([]byte(strings.Join(failures, "; ")), 1000)
	d.repoStates = states
	d.mu.Unlock()
	if len(failures) > 0 {
		log.Printf("mirror cycle completed with warnings: %s", strings.Join(failures, "; "))
	}
}

func (d *Daemon) upstreamsFor(repo Repository) []string {
	if values := d.config.Upstreams[repositoryKey(repo)]; len(values) > 0 {
		return values
	}
	for _, candidate := range d.gateway.Repositories {
		if candidate.GitDir == repo.GitDir {
			if values := d.config.Upstreams[repositoryKey(candidate)]; len(values) > 0 {
				return values
			}
		}
	}
	return nil
}

// gatewayListening reports whether the supervised gateway accepts connections
// on its loopback probe address right now. A supervised process counts as
// running from the moment it is spawned, several seconds before it listens.
func (d *Daemon) gatewayListening() bool {
	connection, err := net.DialTimeout("tcp", d.config.GatewayProbe, 750*time.Millisecond)
	if err != nil {
		return false
	}
	connection.Close()
	return true
}

// gatewayServing is gatewayListening that does not count a process a requested
// restart is about to terminate. Restart is asynchronous: right after it the
// old process can still accept, and a lease registered then is verified by the
// relay against a gateway that is already gone (unreachable_mirror_url).
func (d *Daemon) gatewayServing() bool {
	d.mu.RLock()
	replacing := d.gatewayReplacing
	d.mu.RUnlock()
	if replacing != 0 && d.supervisor != nil {
		if instance, _ := d.supervisor.Instance("gateway"); instance == replacing {
			return false
		}
	}
	return d.gatewayListening()
}

// endpointLeaseAllowed decides whether this cycle may register the public
// endpoint: the relay verifies a registration by fetching the node's manifest
// and health through its public origin, so the gateway must be listening, not
// merely supervised, and the tunnel must be up unless the origin is direct.
func endpointLeaseAllowed(running map[string]bool, disableCloudflared, listening bool) bool {
	return running["gateway"] && listening &&
		(disableCloudflared || running["cloudflared"])
}

func (d *Daemon) ready() bool {
	if d.config.Role == RoleRunner {
		d.mu.RLock()
		defer d.mu.RUnlock()
		return d.lastSyncOK && !d.lastSyncAt.IsZero() && time.Since(d.lastSyncAt) < runnerStaleAfter
	}
	listening := d.gatewayServing()
	d.mu.RLock()
	syncOK := d.lastSyncOK
	last := d.lastSyncAt
	d.mu.RUnlock()
	running, _ := d.supervisor.Snapshot()
	return listening && running["gateway"] && syncOK && !last.IsZero() && time.Since(last) < 2*d.config.SyncInterval.Duration+d.config.SyncTimeout.Duration
}

func (d *Daemon) status() Status {
	running, restarts := d.supervisor.Snapshot()
	ready := d.ready()
	d.mu.RLock()
	defer d.mu.RUnlock()
	var memory runtime.MemStats
	runtime.ReadMemStats(&memory)
	status := Status{
		OK:            true,
		Ready:         ready,
		Node:          d.gateway.Node.Name,
		Version:       d.config.Version,
		UptimeSeconds: int64(time.Since(d.startedAt).Seconds()),
		Processes:     running,
		Restarts:      restarts,
		Repositories:  copyMap(d.repoStates),
		LastSyncOK:    d.lastSyncOK,
		LastSyncError: d.lastSyncErr,
		SyncCount:     d.syncCount,
		SyncStatus: EvaluateSyncStatus(daemonSyncEvidence(
			time.Now(), d.startedAt, d.lastSyncAt, d.lastSyncOK, d.lastSyncErr)),
		Runtime: RuntimeStats{
			Goroutines: runtime.NumGoroutine(),
			HeapBytes:  memory.HeapAlloc,
			TotalAlloc: memory.TotalAlloc,
		},
		Recovery: ReadRecoveryStatus(d.config.GatewayConfig, time.Now()),
	}
	if !d.lastSyncAt.IsZero() {
		status.LastSyncAt = d.lastSyncAt.UTC().Format(time.RFC3339)
	}
	if !d.lastPublishAt.IsZero() {
		status.LastPublishAt = d.lastPublishAt.UTC().Format(time.RFC3339)
	}
	if !d.lastEndpointAt.IsZero() {
		status.LastEndpointAt = d.lastEndpointAt.UTC().Format(time.RFC3339)
	}
	if len(d.watchUp) > 0 {
		status.UpstreamWatches = make(map[string]bool, len(d.watchUp))
		for upstream, up := range d.watchUp {
			status.UpstreamWatches[upstream] = up
		}
	}
	return status
}

func (d *Daemon) handleHealth(w http.ResponseWriter, _ *http.Request) {
	writeJSON(w, http.StatusOK, map[string]any{"ok": true, "node": d.gateway.Node.Name})
}
func (d *Daemon) handleReady(w http.ResponseWriter, _ *http.Request) {
	status := http.StatusOK
	if !d.ready() {
		status = http.StatusServiceUnavailable
	}
	writeJSON(w, status, d.status())
}
func (d *Daemon) handleStatus(w http.ResponseWriter, _ *http.Request) {
	writeJSON(w, http.StatusOK, d.status())
}

func (d *Daemon) handleSync(w http.ResponseWriter, request *http.Request) {
	if request.Method != http.MethodPost {
		w.Header().Set("Allow", http.MethodPost)
		writeJSON(w, http.StatusMethodNotAllowed,
			map[string]any{"ok": false, "error": "POST required"})
		return
	}
	// A post-receive wake is materially different from an operator asking for
	// a pull. The received refs are already the newest durable local state. A
	// fetch here can immediately select an older round-robin peer and resurrect
	// a branch the source just pruned. Publish/reload the received generation
	// first; the ordinary safety loop still performs upstream reconciliation.
	localReceive := request.URL.Query().Get("local") == "1"
	d.mu.Lock()
	d.collaborationCatchUp = true
	// A local receive/manual wake represents new work, so it gets one immediate
	// controller attempt even if an older outage had reached the retry ceiling.
	// Any continued outage installs a fresh bounded backoff in this cycle.
	d.collaborationRetryAt = time.Time{}
	d.collaborationRetryDelay = 0
	d.catalogRetryAt = time.Time{}
	d.catalogRetryDelay = 0
	if d.forkJobsEnabled {
		d.forkJobsPending = true
	}
	d.mu.Unlock()
	if d.hosted != nil && !localReceive {
		// An operator-requested sync reconciles hosted repositories too.
		d.hosted.request()
	}
	queue := d.syncRequests
	if localReceive {
		queue = d.publishRequests
	}
	select {
	case queue <- struct{}{}:
		writeJSON(w, http.StatusAccepted, map[string]any{"ok": true, "queued": true})
	default:
		writeJSON(w, http.StatusAccepted, map[string]any{
			"ok": true, "queued": false, "message": "sync already requested",
		})
	}
}

func writeJSON(w http.ResponseWriter, status int, value any) {
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Cache-Control", "no-store")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(value)
}
func copyMap(source map[string]string) map[string]string {
	out := make(map[string]string, len(source))
	for k, v := range source {
		out[k] = v
	}
	return out
}

func ValidateRuntime(cfg Config) error {
	paths := map[string]string{"gateway script": cfg.GatewayScript, "python": cfg.Python, "identity": cfg.IdentityKey}
	if cfg.Role == RoleRunner {
		paths = map[string]string{"identity": cfg.IdentityKey}
	}
	for label, path := range paths {
		if info, err := os.Stat(path); err != nil || !info.Mode().IsRegular() {
			return fmt.Errorf("%s is unavailable", label)
		}
	}
	if _, err := exec.LookPath("git"); err != nil {
		return errors.New("git is unavailable")
	}
	return nil
}
