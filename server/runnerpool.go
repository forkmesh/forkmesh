package mirrornode

// Burst runner autoscaler (PLAN.md Phase 5, docs/RUNNERS.md).
//
// decidePool is a pure function: given the policy, the remembered state and
// one observation (queue depth, the always-on runners' load, the burst
// instances the provider lists and whether each is busy), it returns the new
// state and the create/destroy actions to take. runnerPoolLoop gathers the
// observation, applies the actions through a PoolProvider and persists the
// state. Only instances labelled fmnext-burst-* and tagged forkmesh-next are
// ever considered, by the decision and again by the provider.

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"log"
	"math"
	"net/url"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"time"
)

const (
	poolLabelPrefix = "fmnext-burst-"
	poolTag         = "forkmesh-next"
	// A new instance is booting, not idle, until it has had this long to
	// start the daemon and claim.
	poolBootGrace = 5 * time.Minute
	poolInterval  = 30 * time.Second
	poolMaxRetry  = 15 * time.Minute
)

func poolRetryDelay(failures int) time.Duration {
	delay := poolInterval
	for i := 0; i < failures && delay < poolMaxRetry; i++ {
		delay *= 2
	}
	return min(delay, poolMaxRetry)
}

// PoolInstance is one burst instance as the provider reports it.
type PoolInstance struct {
	ID        string    `json:"id"`
	Label     string    `json:"label"`
	Tags      []string  `json:"tags"`
	CreatedAt time.Time `json:"createdAt"`
}

// PoolProvider is the cloud side. Implementations must refuse to list,
// create or destroy anything that is not a managed burst instance.
type PoolProvider interface {
	List(ctx context.Context) ([]PoolInstance, error)
	Create(ctx context.Context, label string) (PoolInstance, error)
	Destroy(ctx context.Context, instance PoolInstance) error
}

// managedPoolInstance is the one ownership rule, shared by every layer.
func managedPoolInstance(instance PoolInstance) bool {
	if !strings.HasPrefix(instance.Label, poolLabelPrefix) {
		return false
	}
	number := instance.Label[len(poolLabelPrefix):]
	if !numberPattern.MatchString(number) || len(number) > 4 {
		return false
	}
	for _, tag := range instance.Tags {
		if tag == poolTag {
			return true
		}
	}
	return false
}

type PoolPolicy struct {
	MaxBurstRunners          int
	ScaleUpAfter             time.Duration
	IdleTimeout              time.Duration
	MonthlySpendCeilingCents int64
	HourlyPriceCents         float64
}

// PoolObservation is what the loop saw this round.
type PoolObservation struct {
	Now        time.Time
	QueueDepth int
	// AlwaysOnBusy is true when no always-on runner has a free slot (or
	// there is none): only then does queued work justify a burst runner.
	AlwaysOnBusy bool
	Instances    []PoolInstance
	// Busy names the burst instances (by label) holding at least one lease.
	Busy map[string]bool
}

// PoolState is remembered between rounds (and across restarts).
type PoolState struct {
	Month       string               `json:"month"`
	SpentCents  float64              `json:"spentCents"`
	LastAccrual time.Time            `json:"lastAccrual"`
	QueueSince  time.Time            `json:"queueSince"`
	IdleSince   map[string]time.Time `json:"idleSince"`
}

type PoolAction struct {
	Kind     string // "create" or "destroy"
	Label    string
	Instance PoolInstance
	Reason   string
}

func decidePool(policy PoolPolicy, state PoolState, obs PoolObservation) (PoolState, []PoolAction) {
	now := obs.Now
	next := PoolState{Month: now.UTC().Format("2006-01"), SpentCents: state.SpentCents,
		LastAccrual: now, QueueSince: state.QueueSince, IdleSince: map[string]time.Time{}}
	var managed []PoolInstance
	for _, instance := range obs.Instances {
		if managedPoolInstance(instance) {
			managed = append(managed, instance)
		}
	}
	sort.Slice(managed, func(i, j int) bool { return managed[i].Label < managed[j].Label })
	// Spend: every managed instance is billed from the last accrual (or its
	// creation, if later) to now. A new month starts from zero.
	if state.Month != next.Month {
		next.SpentCents = 0
	}
	for _, instance := range managed {
		from := state.LastAccrual
		if instance.CreatedAt.After(from) {
			from = instance.CreatedAt
		}
		if state.Month != next.Month {
			monthStart := time.Date(now.UTC().Year(), now.UTC().Month(), 1, 0, 0, 0, 0, time.UTC)
			if from.Before(monthStart) {
				from = monthStart
			}
		}
		if !from.IsZero() && now.After(from) {
			next.SpentCents += now.Sub(from).Hours() * policy.HourlyPriceCents
		}
	}
	if obs.QueueDepth > 0 {
		if next.QueueSince.IsZero() {
			next.QueueSince = now
		}
	} else {
		next.QueueSince = time.Time{}
	}
	var actions []PoolAction
	overCeiling := next.SpentCents >= float64(policy.MonthlySpendCeilingCents)
	live := 0
	idleOrBooting := 0
	for _, instance := range managed {
		switch {
		case overCeiling:
			actions = append(actions, PoolAction{Kind: "destroy", Label: instance.Label,
				Instance: instance, Reason: "monthly spend ceiling reached"})
			continue
		case obs.Busy[instance.Label]:
			live++
			continue
		case now.Sub(instance.CreatedAt) < poolBootGrace:
			live++
			idleOrBooting++
			continue
		}
		idle := state.IdleSince[instance.Label]
		if idle.IsZero() {
			idle = now
		}
		if now.Sub(idle) >= policy.IdleTimeout {
			actions = append(actions, PoolAction{Kind: "destroy", Label: instance.Label,
				Instance: instance, Reason: "idle"})
			continue
		}
		next.IdleSince[instance.Label] = idle
		live++
		idleOrBooting++
	}
	if overCeiling || obs.QueueDepth <= idleOrBooting || !obs.AlwaysOnBusy ||
		next.QueueSince.IsZero() || now.Sub(next.QueueSince) < policy.ScaleUpAfter ||
		live >= policy.MaxBurstRunners {
		return next, actions
	}
	// Starting one more must fit the ceiling for at least its first hour.
	if next.SpentCents+float64(live+1)*policy.HourlyPriceCents > float64(policy.MonthlySpendCeilingCents) {
		return next, actions
	}
	used := map[string]bool{}
	for _, instance := range managed {
		used[instance.Label] = true
	}
	for n := 1; n <= policy.MaxBurstRunners*4; n++ {
		label := poolLabelPrefix + strconv.Itoa(n)
		if !used[label] {
			actions = append(actions, PoolAction{Kind: "create", Label: label,
				Reason: fmt.Sprintf("%d queued for %s with the always-on runners busy",
					obs.QueueDepth, now.Sub(next.QueueSince).Round(time.Second))})
			break
		}
	}
	return next, actions
}

// poolStatus is the app's POST /api/runners/pool-status answer.
type poolStatus struct {
	Queued  int `json:"queued"`
	Runners []struct {
		Name         string `json:"name"`
		Scope        string `json:"scope"`
		LastSeenAt   int64  `json:"lastSeenAt"`
		ActiveLeases int    `json:"activeLeases"`
	} `json:"runners"`
}

func poolObservation(now time.Time, status poolStatus, instances []PoolInstance) PoolObservation {
	obs := PoolObservation{Now: now, QueueDepth: status.Queued, Instances: instances,
		Busy: map[string]bool{}, AlwaysOnBusy: true}
	for _, runner := range status.Runners {
		if strings.HasPrefix(runner.Name, poolLabelPrefix) {
			if runner.ActiveLeases > 0 {
				obs.Busy[runner.Name] = true
			}
			continue
		}
		// An always-on runner seen in the last few minutes with no lease has
		// room, so queued work waits for it rather than for a new machine.
		if runner.ActiveLeases == 0 && now.Sub(time.UnixMilli(runner.LastSeenAt)) < 5*time.Minute {
			obs.AlwaysOnBusy = false
		}
	}
	return obs
}

type runnerPoolLoop struct {
	daemon    *Daemon
	policy    PoolPolicy
	provider  PoolProvider
	statePath string
	origin    string
	state     PoolState
	now       func() time.Time
}

func newRunnerPoolLoop(d *Daemon) (*runnerPoolLoop, error) {
	cfg := d.config.RunnerPool
	key, err := readPoolAPIKey(cfg.APIKeyFile)
	if err != nil {
		return nil, err
	}
	catalog, err := url.Parse(d.config.CatalogURL)
	if err != nil || catalog.Host == "" {
		return nil, errors.New("runnerPool needs catalogUrl to reach the app")
	}
	statePath := cfg.StatePath
	if statePath == "" {
		statePath = filepath.Join(filepath.Dir(d.config.GatewayConfig), "runner-pool.json")
	}
	loop := &runnerPoolLoop{daemon: d, statePath: statePath,
		origin: catalog.Scheme + "://" + catalog.Host, now: time.Now,
		policy: PoolPolicy{MaxBurstRunners: cfg.MaxBurstRunners,
			ScaleUpAfter: cfg.ScaleUpAfter.Duration, IdleTimeout: cfg.IdleTimeout.Duration,
			MonthlySpendCeilingCents: cfg.MonthlySpendCeilingCents,
			HourlyPriceCents:         cfg.HourlyPriceCents},
		provider: &VultrPool{APIKey: key, Region: cfg.Region, Plan: cfg.Plan,
			SnapshotID: cfg.SnapshotID}}
	if raw, err := os.ReadFile(statePath); err == nil {
		_ = json.Unmarshal(raw, &loop.state)
	}
	return loop, nil
}

func readPoolAPIKey(path string) (string, error) {
	info, err := os.Lstat(path)
	if err != nil || !info.Mode().IsRegular() || info.Mode().Perm()&0077 != 0 || info.Size() > 4096 {
		return "", errors.New("runnerPool.apiKeyFile must be a small owner-only regular file")
	}
	raw, err := os.ReadFile(path)
	if err != nil {
		return "", err
	}
	key := strings.TrimSpace(string(raw))
	if key == "" || strings.ContainsAny(key, " \r\n") {
		return "", errors.New("runnerPool.apiKeyFile is empty or malformed")
	}
	return key, nil
}

func (p *runnerPoolLoop) run(ctx context.Context) {
	log.Printf("runner pool: enabled (max %d burst runners, ceiling %d cents/month, worst case %d cents/month)",
		p.policy.MaxBurstRunners, p.policy.MonthlySpendCeilingCents,
		poolMonthlyProjection(p.policy.HourlyPriceCents, p.policy.MaxBurstRunners))
	failures := 0
	for {
		if err := p.round(ctx); err != nil && ctx.Err() == nil {
			failures++
			log.Printf("runner pool: %v (next try in %s)", err, poolRetryDelay(failures))
		} else {
			failures = 0
		}
		select {
		case <-ctx.Done():
			return
		case <-time.After(poolRetryDelay(failures)):
		}
	}
}

func (p *runnerPoolLoop) fetchStatus(ctx context.Context) (poolStatus, error) {
	var status poolStatus
	d := p.daemon
	code, reply, err := signedNodePost(ctx, agentJobHTTPClient, p.origin, d.gateway.Node.Name,
		d.identity, "ForkMesh-Mirror-Node/"+d.config.Version, p.now(), "/api/runners/pool-status", []byte("{}"))
	if err != nil {
		return status, err
	}
	if code != 200 {
		return status, fmt.Errorf("pool status returned HTTP %d: %s", code, boundedText(reply, 200))
	}
	if err := json.Unmarshal(reply, &status); err != nil {
		return status, errors.New("pool status returned invalid JSON")
	}
	return status, nil
}

// round is one observe-decide-act cycle. A failed observation acts on
// nothing: the pool never destroys or creates on partial information.
func (p *runnerPoolLoop) round(ctx context.Context) error {
	status, err := p.fetchStatus(ctx)
	if err != nil {
		return err
	}
	return p.apply(ctx, status)
}

func (p *runnerPoolLoop) apply(ctx context.Context, status poolStatus) error {
	instances, err := p.provider.List(ctx)
	if err != nil {
		return err
	}
	next, actions := decidePool(p.policy, p.state, poolObservation(p.now(), status, instances))
	p.state = next
	var failures []string
	for _, action := range actions {
		switch action.Kind {
		case "create":
			log.Printf("runner pool: creating %s (%s)", action.Label, action.Reason)
			if _, err := p.provider.Create(ctx, action.Label); err != nil {
				failures = append(failures, action.Label+": "+err.Error())
			}
		case "destroy":
			log.Printf("runner pool: destroying %s (%s)", action.Label, action.Reason)
			if err := p.provider.Destroy(ctx, action.Instance); err != nil {
				failures = append(failures, action.Label+": "+err.Error())
			}
		}
	}
	if err := os.MkdirAll(filepath.Dir(p.statePath), 0700); err == nil {
		_ = writePrivateJSON(p.statePath, p.state)
	}
	if len(failures) != 0 {
		return errors.New(strings.Join(failures, "; "))
	}
	return nil
}

// poolMonthlyProjection is the worst-case monthly cost of n always-running
// burst instances; used by docs and the status line.
func poolMonthlyProjection(hourlyCents float64, n int) int64 {
	return int64(math.Ceil(hourlyCents * 730 * float64(n)))
}
