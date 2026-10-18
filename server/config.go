package mirrornode

import (
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"net/url"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"time"
)

const configSchemaVersion = 1

type Config struct {
	SchemaVersion  int                 `json:"schemaVersion"`
	Listen         string              `json:"listen"`
	GatewayConfig  string              `json:"gatewayConfig"`
	GatewayScript  string              `json:"gatewayScript"`
	Python         string              `json:"python"`
	Cloudflared    string              `json:"cloudflared"`
	ConnectorToken string              `json:"connectorToken"`
	IdentityKey    string              `json:"identityKey"`
	CatalogURL     string              `json:"catalogUrl"`
	MirrorURL      string              `json:"mirrorUrl,omitempty"`
	SyncInterval   Duration            `json:"syncInterval"`
	SyncTimeout    Duration            `json:"syncTimeout"`
	GatewayProbe   string              `json:"gatewayProbe"`
	Upstreams      map[string][]string `json:"upstreams"`
	// EventDrivenUpstreams lists exact configured upstream URLs whose changes
	// are announced on a relay WebSocket. One served by this node's own relay
	// uses the bounded safety fetch cadence instead of the operator polling
	// interval; one served by another relay gets a public upstream watch
	// socket on that relay and keeps the operator polling interval as its
	// fallback (docs/INSTANCES.md).
	EventDrivenUpstreams []string          `json:"eventDrivenUpstreams,omitempty"`
	MirrorOf             map[string]string `json:"mirrorOf,omitempty"`
	PublishOwner         string            `json:"publishOwner"`
	CanonicalOwner       string            `json:"canonicalOwner,omitempty"`
	Version              string            `json:"version"`
	DisableCatalog       bool              `json:"disableCatalog"`
	DisableCloudflared   bool              `json:"disableCloudflared"`
	// Keeps the node strictly read-only: the gateway then advertises no
	// merge-pull / update-pull-branch capability and the Worker never selects
	// this mirror for an owner-authorized web merge.
	DisableMergeExecutor bool `json:"disableMergeExecutor"`
	// Refuses the streamed provider sign-in terminal (terminal.go). It is
	// accepted by default because a node that advertises an agent CLI has to
	// have that CLI signed in to be useful, and the only alternative is an SSH
	// session from a desktop the operator may not run. Setting this leaves the
	// SSH path as the only way in.
	DisableAgentLogin bool `json:"disableAgentLogin,omitempty"`
	// Refuses website-queued agent jobs (agents.go) and stops advertising the
	// node's provider CLIs, so the website never offers it as an agent target.
	DisableAgentJobs bool `json:"disableAgentJobs,omitempty"`
	// Opt-in, unlike the switches above: this node then runs the
	// .forkmesh/*.yml workflows of the repositories it serves, which is shell
	// a repository supplies. Steps run in a bubblewrap sandbox with no
	// network and no view of this node's state; without a working sandbox
	// nothing runs. See actions.go.
	EnableActions bool `json:"enableActions,omitempty"`
	// The account that owns this node. Website agent jobs no longer run a
	// model safety preflight, so nothing reads this any more; it stays so
	// existing configurations keep parsing under DisallowUnknownFields.
	AgentOwner string `json:"agentOwner,omitempty"`
	// Role splits trusted core from untrusted agent compute (docs/RUNNERS.md).
	// "mirror" (the default) keeps today's behaviour. "core" serves
	// repositories but never runs agent jobs or Actions. "runner" serves
	// nothing: no gateway, tunnel, catalog, merge executor or repositories on
	// disk; it only claims agent leases from the app and runs them.
	Role string `json:"role,omitempty"`
	// Runner settings; only meaningful (and only accepted) with role runner.
	Runner *RunnerConfig `json:"runner,omitempty"`
	// Burst runner autoscaler (runnerpool.go); core role only, off by default.
	RunnerPool *RunnerPoolConfig `json:"runnerPool,omitempty"`
	// ForkMesh-hosted repositories replicated across the core (hosted.go,
	// docs/HOSTED_REPOS.md); off by default.
	Hosted *HostedConfig `json:"hosted,omitempty"`

	ReleasePruneInterval *Duration `json:"releasePruneInterval,omitempty"`
	ReleasePruneMinAge   *Duration `json:"releasePruneMinAge,omitempty"`

	// roleOverrides names switches the role forced off, for one startup log.
	roleOverrides []string
}

const (
	RoleMirror = "mirror"
	RoleCore   = "core"
	RoleRunner = "runner"
)

// RunnerConfig is the runner role's job policy. Every field is optional.
type RunnerConfig struct {
	MaxConcurrentRuns int    `json:"maxConcurrentRuns,omitempty"`
	WorkRoot          string `json:"workRoot,omitempty"`
	JobMemoryMax      string `json:"jobMemoryMax,omitempty"`
	JobMemorySwapMax  string `json:"jobMemorySwapMax,omitempty"`
	JobCPUQuota       string `json:"jobCPUQuota,omitempty"`
	AppURL            string `json:"appUrl,omitempty"`
	// When set, only jobs of these repository owners are claimed (the app
	// enforces its own per-runner list as well).
	AllowedOwners []string `json:"allowedOwners,omitempty"`
	// A burst runner is a throwaway cloud instance started by the core's
	// autoscaler; it exits its loop once idle so the pool can reclaim it.
	Burst bool `json:"burst,omitempty"`
	// The uid the job scope runs as; required when the daemon is root
	// (docs/RUNNERS.md: a dedicated user such as forkmesh-job, never the
	// daemon's own). JobUser names it instead and is resolved at startup.
	JobUID  int    `json:"jobUid,omitempty"`
	JobUser string `json:"jobUser,omitempty"`
}

// RunnerPoolConfig configures burst runners. Disabled unless enabled=true.
type RunnerPoolConfig struct {
	Enabled                  bool     `json:"enabled,omitempty"`
	Provider                 string   `json:"provider,omitempty"`
	MaxBurstRunners          int      `json:"maxBurstRunners,omitempty"`
	MonthlySpendCeilingCents int64    `json:"monthlySpendCeilingCents,omitempty"`
	HourlyPriceCents         float64  `json:"hourlyPriceCents,omitempty"`
	ScaleUpAfter             Duration `json:"scaleUpAfter,omitempty"`
	IdleTimeout              Duration `json:"idleTimeout,omitempty"`
	Region                   string   `json:"region,omitempty"`
	Plan                     string   `json:"plan,omitempty"`
	SnapshotID               string   `json:"snapshotId,omitempty"`
	APIKeyFile               string   `json:"apiKeyFile,omitempty"`
	StatePath                string   `json:"statePath,omitempty"`
}

type Duration struct{ time.Duration }

func (d Duration) MarshalJSON() ([]byte, error) {
	return json.Marshal(d.Duration.String())
}

func (d *Duration) UnmarshalJSON(raw []byte) error {
	var value string
	if err := json.Unmarshal(raw, &value); err != nil {
		return errors.New("duration must be a string")
	}
	parsed, err := time.ParseDuration(value)
	if err != nil || parsed <= 0 {
		return errors.New("duration must be positive")
	}
	d.Duration = parsed
	return nil
}

func LoadConfig(path string) (Config, error) {
	var cfg Config
	if !filepath.IsAbs(path) {
		return cfg, errors.New("config path must be absolute")
	}
	info, err := os.Lstat(path)
	if err != nil {
		return cfg, fmt.Errorf("stat config: %w", err)
	}
	if !info.Mode().IsRegular() || info.Mode()&os.ModeSymlink != 0 || info.Size() > 256<<10 {
		return cfg, errors.New("config must be a small regular file")
	}
	raw, err := os.ReadFile(path)
	if err != nil {
		return cfg, fmt.Errorf("read config: %w", err)
	}
	decoder := json.NewDecoder(strings.NewReader(string(raw)))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&cfg); err != nil {
		return cfg, fmt.Errorf("decode config: %w", err)
	}
	if err := decoder.Decode(&struct{}{}); err != io.EOF {
		return cfg, errors.New("config contains trailing JSON")
	}
	if err := cfg.validate(); err != nil {
		return cfg, err
	}
	return cfg, nil
}

var mirrorPagePattern = regexp.MustCompile(`^https://[A-Za-z0-9.-]+(?::[0-9]{1,5})?(?:/[A-Za-z0-9._~-]+){1,8}$`)

var releaseVersionPattern = regexp.MustCompile(`^[0-9]+\.[0-9]+\.[0-9]+$`)

// UpdateConfigVersion changes only the node's public release identity while
// retaining its keys, upstreams, tunnel, and repository state. Release
// installation runs this as the locked service user before restarting the
// newly installed binary, so catalog and User-Agent data cannot stay pinned to
// the version from the machine's first bootstrap.
func UpdateConfigVersion(path, version string) error {
	if !releaseVersionPattern.MatchString(version) {
		return errors.New("version must be an exact release version")
	}
	cfg, err := LoadConfig(path)
	if err != nil {
		return err
	}
	cfg.Version = version
	if err := writePrivateJSON(path, cfg); err != nil {
		return fmt.Errorf("write config version: %w", err)
	}
	return nil
}

func (c *Config) validate() error {
	if c.SchemaVersion != configSchemaVersion {
		return errors.New("unsupported config schemaVersion")
	}
	if err := c.applyRole(); err != nil {
		return err
	}
	if c.Listen == "" {
		c.Listen = "127.0.0.1:8791"
	}
	host, _, err := net.SplitHostPort(c.Listen)
	if err != nil || (host != "127.0.0.1" && host != "::1" && host != "localhost") {
		return errors.New("listen must be a loopback host and port")
	}
	if c.GatewayProbe == "" {
		c.GatewayProbe = "127.0.0.1:8790"
	}
	probeHost, _, err := net.SplitHostPort(c.GatewayProbe)
	if err != nil || (probeHost != "127.0.0.1" && probeHost != "::1" && probeHost != "localhost") {
		return errors.New("gatewayProbe must be a loopback host and port")
	}
	required := map[string]string{
		"gatewayConfig": c.GatewayConfig, "identityKey": c.IdentityKey,
	}
	if c.Role != RoleRunner {
		// A runner starts no gateway, so it needs neither script nor Python;
		// its gatewayConfig is only the node identity document.
		required["gatewayScript"] = c.GatewayScript
		required["python"] = c.Python
	}
	for label, value := range required {
		if !filepath.IsAbs(value) {
			return fmt.Errorf("%s must be absolute", label)
		}
	}
	if !c.DisableCloudflared {
		if !filepath.IsAbs(c.Cloudflared) || !filepath.IsAbs(c.ConnectorToken) {
			return errors.New("cloudflared and connectorToken must be absolute")
		}
	}
	if c.SyncInterval.Duration == 0 {
		c.SyncInterval.Duration = 15 * time.Second
	}
	if c.SyncInterval.Duration < 5*time.Second || c.SyncInterval.Duration > 24*time.Hour {
		return errors.New("syncInterval must be between 5s and 24h")
	}
	if c.SyncTimeout.Duration == 0 {
		c.SyncTimeout.Duration = 5 * time.Minute
	}
	if c.SyncTimeout.Duration < 10*time.Second || c.SyncTimeout.Duration > 30*time.Minute {
		return errors.New("syncTimeout must be between 10s and 30m")
	}
	if !c.DisableCatalog && (c.CatalogURL == "" || c.PublishOwner == "") {
		return errors.New("catalogUrl and publishOwner are required")
	}
	if c.MirrorURL != "" {
		origin, err := normalizeHTTPSOrigin(c.MirrorURL)
		if err != nil {
			return errors.New("mirrorUrl must be a credential-free HTTPS origin")
		}
		c.MirrorURL = origin
	}
	configuredUpstreams := make(map[string]struct{})
	for _, upstreams := range c.Upstreams {
		for _, upstream := range upstreams {
			configuredUpstreams[upstream] = struct{}{}
		}
	}
	seenEventUpstreams := make(map[string]struct{})
	for _, upstream := range c.EventDrivenUpstreams {
		if _, ok := configuredUpstreams[upstream]; !ok {
			return errors.New("eventDrivenUpstreams must name configured upstreams")
		}
		if _, duplicate := seenEventUpstreams[upstream]; duplicate {
			return errors.New("eventDrivenUpstreams must not contain duplicates")
		}
		seenEventUpstreams[upstream] = struct{}{}
	}
	for key, page := range c.MirrorOf {
		if len(c.Upstreams[key]) == 0 {
			return errors.New("mirrorOf must name repositories with configured upstreams")
		}
		if !mirrorPagePattern.MatchString(page) {
			return errors.New("mirrorOf must map to a credential-free HTTPS repository page")
		}
	}
	c.AgentOwner = strings.ToLower(strings.TrimSpace(c.AgentOwner))
	if c.AgentOwner != "" && !nodePattern.MatchString(c.AgentOwner) {
		return errors.New("agentOwner must be a valid account name")
	}
	if c.Version == "" {
		c.Version = "dev"
	}
	if err := c.validateHosted(); err != nil {
		return err
	}
	if err := c.validateReleasePrune(); err != nil {
		return err
	}
	if err := c.validateRunner(); err != nil {
		return err
	}
	return c.validateRunnerPool()
}

var (
	systemdSizePattern  = regexp.MustCompile(`^[1-9][0-9]{0,6}[KMGT]?$`)
	systemdQuotaPattern = regexp.MustCompile(`^[1-9][0-9]{0,3}%$`)
	jobUserPattern      = regexp.MustCompile(`^[a-z_][a-z0-9_-]{0,31}$`)
)

// applyRole normalizes the role and forces the switches it implies before
// the rest of validation runs, so a runner is never asked for tunnel or
// catalog settings it will not use.
func (c *Config) applyRole() error {
	c.Role = strings.ToLower(strings.TrimSpace(c.Role))
	switch c.Role {
	case "":
		c.Role = RoleMirror
	case RoleMirror, RoleCore, RoleRunner:
	default:
		return errors.New("role must be mirror, core or runner")
	}
	c.roleOverrides = nil
	if c.Runner != nil && c.Role != RoleRunner {
		return errors.New("runner settings require role runner")
	}
	switch c.Role {
	case RoleCore:
		// Trusted core never executes repository-supplied or agent code.
		if !c.DisableAgentJobs {
			c.roleOverrides = append(c.roleOverrides, "agent jobs")
		}
		if c.EnableActions {
			c.roleOverrides = append(c.roleOverrides, "actions")
		}
		c.DisableAgentJobs = true
		c.EnableActions = false
	case RoleRunner:
		if c.Runner == nil {
			c.Runner = &RunnerConfig{}
		}
		if len(c.Upstreams) != 0 || len(c.EventDrivenUpstreams) != 0 || len(c.MirrorOf) != 0 {
			return errors.New("a runner keeps no repositories, so upstreams must be empty")
		}
		c.DisableCatalog = true
		c.DisableCloudflared = true
		c.DisableMergeExecutor = true
		c.DisableAgentJobs = false
	}
	return nil
}

func (c *Config) validateRunner() error {
	if c.Role != RoleRunner {
		return nil
	}
	r := c.Runner
	if r.MaxConcurrentRuns == 0 {
		r.MaxConcurrentRuns = 1
	}
	if r.MaxConcurrentRuns < 1 || r.MaxConcurrentRuns > 8 {
		return errors.New("runner.maxConcurrentRuns must be between 1 and 8")
	}
	if r.WorkRoot == "" {
		r.WorkRoot = filepath.Join(filepath.Dir(c.IdentityKey), "runner-work")
	}
	if !filepath.IsAbs(r.WorkRoot) || filepath.Clean(r.WorkRoot) != r.WorkRoot || r.WorkRoot == "/" {
		return errors.New("runner.workRoot must be a clean absolute directory")
	}
	if r.JobMemoryMax == "" {
		r.JobMemoryMax = "700M"
	}
	if !systemdSizePattern.MatchString(r.JobMemoryMax) {
		return errors.New("runner.jobMemoryMax must look like 700M")
	}
	if r.JobMemorySwapMax == "" {
		r.JobMemorySwapMax = "0"
	}
	if r.JobMemorySwapMax != "0" && !systemdSizePattern.MatchString(r.JobMemorySwapMax) {
		return errors.New("runner.jobMemorySwapMax must be 0 or look like 1G")
	}
	if r.JobCPUQuota == "" {
		r.JobCPUQuota = "90%"
	}
	if !systemdQuotaPattern.MatchString(r.JobCPUQuota) {
		return errors.New("runner.jobCPUQuota must look like 90%")
	}
	if r.JobUID < 0 {
		return errors.New("runner.jobUid must not be negative")
	}
	if r.JobUser != "" && (r.JobUser == "root" || !jobUserPattern.MatchString(r.JobUser)) {
		return errors.New("runner.jobUser must be a plain user name other than root")
	}
	origin, err := normalizeAppOrigin(r.AppURL)
	if err != nil {
		return errors.New("runner.appUrl must be an HTTPS origin")
	}
	r.AppURL = origin
	seen := map[string]bool{}
	owners := r.AllowedOwners[:0]
	for _, owner := range r.AllowedOwners {
		owner = strings.ToLower(strings.TrimSpace(owner))
		if !nodePattern.MatchString(owner) {
			return errors.New("runner.allowedOwners must be account names")
		}
		if !seen[owner] {
			seen[owner] = true
			owners = append(owners, owner)
		}
	}
	r.AllowedOwners = owners
	return nil
}

// normalizeAppOrigin accepts an HTTPS origin, or plain HTTP to loopback so a
// local app host can be exercised in tests and development.
func normalizeAppOrigin(value string) (string, error) {
	if origin, err := normalizeHTTPSOrigin(value); err == nil {
		return origin, nil
	}
	parsed, err := url.Parse(strings.TrimSpace(value))
	if err != nil || parsed.Scheme != "http" || parsed.User != nil ||
		parsed.RawQuery != "" || parsed.Fragment != "" ||
		(parsed.Path != "" && parsed.Path != "/") {
		return "", errors.New("invalid app origin")
	}
	host := parsed.Hostname()
	if host != "127.0.0.1" && host != "::1" && host != "localhost" {
		return "", errors.New("invalid app origin")
	}
	return "http://" + parsed.Host, nil
}

func (c *Config) validateRunnerPool() error {
	p := c.RunnerPool
	if p == nil {
		return nil
	}
	if p.Enabled && c.Role != RoleCore {
		return errors.New("runnerPool may only be enabled on a core node")
	}
	if p.Provider == "" {
		p.Provider = "vultr"
	}
	if p.Provider != "vultr" {
		return errors.New("runnerPool.provider must be vultr")
	}
	if p.MaxBurstRunners == 0 {
		p.MaxBurstRunners = 2
	}
	if p.MaxBurstRunners < 1 || p.MaxBurstRunners > 10 {
		return errors.New("runnerPool.maxBurstRunners must be between 1 and 10")
	}
	if p.MonthlySpendCeilingCents < 0 || p.HourlyPriceCents < 0 {
		return errors.New("runnerPool prices must not be negative")
	}
	if p.ScaleUpAfter.Duration == 0 {
		p.ScaleUpAfter.Duration = 2 * time.Minute
	}
	if p.IdleTimeout.Duration == 0 {
		p.IdleTimeout.Duration = 10 * time.Minute
	}
	if !p.Enabled {
		return nil
	}
	if p.MonthlySpendCeilingCents == 0 || p.HourlyPriceCents == 0 {
		return errors.New("an enabled runnerPool needs monthlySpendCeilingCents and hourlyPriceCents")
	}
	if p.Region == "" || p.Plan == "" || p.SnapshotID == "" || !filepath.IsAbs(p.APIKeyFile) {
		return errors.New("an enabled runnerPool needs region, plan, snapshotId and an absolute apiKeyFile")
	}
	if p.StatePath != "" && !filepath.IsAbs(p.StatePath) {
		return errors.New("runnerPool.statePath must be absolute")
	}
	return nil
}

func normalizeHTTPSOrigin(value string) (string, error) {
	parsed, err := url.Parse(strings.TrimSpace(value))
	if err != nil || parsed.Scheme != "https" || parsed.Hostname() == "" ||
		parsed.User != nil || parsed.RawQuery != "" || parsed.Fragment != "" ||
		(parsed.Path != "" && parsed.Path != "/") ||
		(parsed.Port() != "" && parsed.Port() != "443") {
		return "", errors.New("invalid HTTPS origin")
	}
	host := strings.ToLower(parsed.Hostname())
	if strings.Contains(host, ":") {
		host = "[" + host + "]"
	}
	return "https://" + host, nil
}
