package mirrornode

import (
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"
)

var poolT0 = time.Date(2026, 9, 15, 12, 0, 0, 0, time.UTC)

func testPolicy() PoolPolicy {
	return PoolPolicy{MaxBurstRunners: 2, ScaleUpAfter: 2 * time.Minute, IdleTimeout: 10 * time.Minute,
		MonthlySpendCeilingCents: 5000, HourlyPriceCents: 10}
}

func burst(label string, created time.Time) PoolInstance {
	return PoolInstance{ID: "id-" + label, Label: label, Tags: []string{poolTag}, CreatedAt: created}
}

func kinds(actions []PoolAction) string {
	var out []string
	for _, a := range actions {
		out = append(out, a.Kind+":"+a.Label)
	}
	return strings.Join(out, ",")
}

func TestDecidePoolScaleUpNeedsSustainedQueueAndBusyAlwaysOn(t *testing.T) {
	policy := testPolicy()
	state := PoolState{}
	obs := PoolObservation{Now: poolT0, QueueDepth: 1, AlwaysOnBusy: true}
	state, actions := decidePool(policy, state, obs)
	if len(actions) != 0 || !state.QueueSince.Equal(poolT0) {
		t.Fatalf("first sighting created: %s %+v", kinds(actions), state)
	}
	obs.Now = poolT0.Add(90 * time.Second)
	state, actions = decidePool(policy, state, obs)
	if len(actions) != 0 {
		t.Fatalf("created before scaleUpAfter: %s", kinds(actions))
	}
	obs.Now = poolT0.Add(2 * time.Minute)
	_, actions = decidePool(policy, state, obs)
	if kinds(actions) != "create:fmnext-burst-1" {
		t.Fatalf("sustained queue did not create: %s", kinds(actions))
	}
	// The always-on runner has room: queued work waits for it.
	free := obs
	free.AlwaysOnBusy = false
	if _, actions := decidePool(policy, state, free); len(actions) != 0 {
		t.Fatalf("created while the always-on runner was free: %s", kinds(actions))
	}
	// The queue drained in between: the timer restarts.
	empty := PoolObservation{Now: poolT0.Add(time.Minute), AlwaysOnBusy: true}
	reset, _ := decidePool(policy, state, empty)
	if !reset.QueueSince.IsZero() {
		t.Fatal("an empty queue kept the scale-up timer")
	}
}

func TestDecidePoolCountsBootingAndIdleCapacity(t *testing.T) {
	policy := testPolicy()
	state := PoolState{QueueSince: poolT0.Add(-time.Hour), LastAccrual: poolT0, Month: "2026-09"}
	booting := burst("fmnext-burst-1", poolT0.Add(-time.Minute))
	obs := PoolObservation{Now: poolT0, QueueDepth: 1, AlwaysOnBusy: true, Instances: []PoolInstance{booting}}
	if _, actions := decidePool(policy, state, obs); len(actions) != 0 {
		t.Fatalf("created a second runner for one job while the first boots: %s", kinds(actions))
	}
	obs.QueueDepth = 2
	if _, actions := decidePool(policy, state, obs); kinds(actions) != "create:fmnext-burst-2" {
		t.Fatalf("two queued with one booting: %s", kinds(actions))
	}
	// Busy runners do not absorb queued work.
	busy := burst("fmnext-burst-1", poolT0.Add(-time.Hour))
	obs = PoolObservation{Now: poolT0, QueueDepth: 1, AlwaysOnBusy: true,
		Instances: []PoolInstance{busy}, Busy: map[string]bool{"fmnext-burst-1": true}}
	if _, actions := decidePool(policy, state, obs); kinds(actions) != "create:fmnext-burst-2" {
		t.Fatalf("busy burst runner absorbed the queue: %s", kinds(actions))
	}
}

func TestDecidePoolRespectsMaxBurstRunners(t *testing.T) {
	policy := testPolicy()
	state := PoolState{QueueSince: poolT0.Add(-time.Hour), LastAccrual: poolT0, Month: "2026-09"}
	obs := PoolObservation{Now: poolT0, QueueDepth: 9, AlwaysOnBusy: true,
		Instances: []PoolInstance{burst("fmnext-burst-1", poolT0.Add(-time.Hour)), burst("fmnext-burst-2", poolT0.Add(-time.Hour))},
		Busy:      map[string]bool{"fmnext-burst-1": true, "fmnext-burst-2": true}}
	if _, actions := decidePool(policy, state, obs); len(actions) != 0 {
		t.Fatalf("exceeded maxBurstRunners: %s", kinds(actions))
	}
	// Only one creation per round, even with a deep queue.
	obs.Instances, obs.Busy = nil, nil
	if _, actions := decidePool(policy, state, obs); kinds(actions) != "create:fmnext-burst-1" {
		t.Fatalf("deep queue round = %s", kinds(actions))
	}
	// Labels fill the lowest free number.
	obs.Instances = []PoolInstance{burst("fmnext-burst-2", poolT0.Add(-time.Hour))}
	obs.Busy = map[string]bool{"fmnext-burst-2": true}
	if _, actions := decidePool(policy, state, obs); kinds(actions) != "create:fmnext-burst-1" {
		t.Fatalf("label choice = %s", kinds(actions))
	}
}

func TestDecidePoolDestroysAfterIdleTimeoutOnly(t *testing.T) {
	policy := testPolicy()
	inst := burst("fmnext-burst-1", poolT0.Add(-time.Hour))
	state := PoolState{LastAccrual: poolT0, Month: "2026-09"}
	obs := PoolObservation{Now: poolT0, Instances: []PoolInstance{inst}, AlwaysOnBusy: true}
	state, actions := decidePool(policy, state, obs)
	if len(actions) != 0 || !state.IdleSince["fmnext-burst-1"].Equal(poolT0) {
		t.Fatalf("idle start: %s %+v", kinds(actions), state.IdleSince)
	}
	obs.Now = poolT0.Add(9 * time.Minute)
	state, actions = decidePool(policy, state, obs)
	if len(actions) != 0 {
		t.Fatalf("destroyed before idleTimeout: %s", kinds(actions))
	}
	// Work in between resets the idle clock.
	busy := obs
	busy.Now = poolT0.Add(9*time.Minute + 30*time.Second)
	busy.Busy = map[string]bool{"fmnext-burst-1": true}
	state, _ = decidePool(policy, state, busy)
	if _, ok := state.IdleSince["fmnext-burst-1"]; ok {
		t.Fatal("a busy runner kept its idle clock")
	}
	obs.Now = poolT0.Add(19 * time.Minute)
	state, actions = decidePool(policy, state, obs)
	if len(actions) != 0 {
		t.Fatalf("idle clock was not reset by work: %s", kinds(actions))
	}
	obs.Now = poolT0.Add(29*time.Minute + 30*time.Second)
	_, actions = decidePool(policy, state, obs)
	if kinds(actions) != "destroy:fmnext-burst-1" || actions[0].Instance.ID != "id-fmnext-burst-1" {
		t.Fatalf("idle runner not destroyed: %s", kinds(actions))
	}
	// A booting instance is never idle.
	young := burst("fmnext-burst-2", poolT0.Add(-time.Minute))
	_, actions = decidePool(policy, PoolState{IdleSince: map[string]time.Time{"fmnext-burst-2": poolT0.Add(-time.Hour)}},
		PoolObservation{Now: poolT0, Instances: []PoolInstance{young}})
	if len(actions) != 0 {
		t.Fatalf("destroyed a booting runner: %s", kinds(actions))
	}
}

func TestDecidePoolNeverTouchesUnmanagedInstances(t *testing.T) {
	policy := testPolicy()
	old := poolT0.Add(-48 * time.Hour)
	foreign := []PoolInstance{
		{ID: "a", Label: "fmnext-core-1", Tags: []string{poolTag}, CreatedAt: old},
		{ID: "b", Label: "fmnext-burst-1", Tags: []string{"production"}, CreatedAt: old},
		{ID: "c", Label: "mirror35", Tags: []string{poolTag}, CreatedAt: old},
		{ID: "d", Label: "fmnext-burst-x", Tags: []string{poolTag}, CreatedAt: old},
		{ID: "e", Label: "fmnext-burst-", Tags: []string{poolTag}, CreatedAt: old},
	}
	state := PoolState{LastAccrual: poolT0.Add(-time.Hour), Month: "2026-09",
		IdleSince: map[string]time.Time{"fmnext-burst-1": old}}
	next, actions := decidePool(policy, state, PoolObservation{Now: poolT0, Instances: foreign})
	if len(actions) != 0 || next.SpentCents != 0 {
		t.Fatalf("acted on or billed unmanaged instances: %s %v", kinds(actions), next.SpentCents)
	}
	// Even over the ceiling, only managed instances are destroyed.
	over := PoolState{SpentCents: 6000, LastAccrual: poolT0, Month: "2026-09"}
	all := append(append([]PoolInstance{}, foreign...), burst("fmnext-burst-2", old))
	_, actions = decidePool(policy, over, PoolObservation{Now: poolT0, Instances: all})
	if kinds(actions) != "destroy:fmnext-burst-2" {
		t.Fatalf("ceiling teardown = %s", kinds(actions))
	}
}

func TestDecidePoolSpendCeilingAndMonthRollover(t *testing.T) {
	policy := testPolicy() // 10 cents/hour, ceiling 5000
	inst := burst("fmnext-burst-1", poolT0.Add(-24*time.Hour))
	state := PoolState{LastAccrual: poolT0.Add(-2 * time.Hour), Month: "2026-09", SpentCents: 100}
	obs := PoolObservation{Now: poolT0, Instances: []PoolInstance{inst}, Busy: map[string]bool{"fmnext-burst-1": true}}
	next, actions := decidePool(policy, state, obs)
	if len(actions) != 0 || next.SpentCents != 120 {
		t.Fatalf("accrual = %v (%s), want 120", next.SpentCents, kinds(actions))
	}
	// A creation must fit its first hour under the ceiling.
	near := PoolState{SpentCents: 4991, LastAccrual: poolT0, Month: "2026-09", QueueSince: poolT0.Add(-time.Hour)}
	queued := PoolObservation{Now: poolT0, QueueDepth: 3, AlwaysOnBusy: true}
	if _, actions := decidePool(policy, near, queued); len(actions) != 0 {
		t.Fatalf("created past the ceiling: %s", kinds(actions))
	}
	near.SpentCents = 4990
	if _, actions := decidePool(policy, near, queued); kinds(actions) != "create:fmnext-burst-1" {
		t.Fatalf("creation that fits was refused: %s", kinds(actions))
	}
	// At the ceiling everything managed goes, busy or not, and nothing starts.
	at := PoolState{SpentCents: 5000, LastAccrual: poolT0, Month: "2026-09", QueueSince: poolT0.Add(-time.Hour)}
	obs = PoolObservation{Now: poolT0, QueueDepth: 5, AlwaysOnBusy: true, Instances: []PoolInstance{inst},
		Busy: map[string]bool{"fmnext-burst-1": true}}
	if _, actions := decidePool(policy, at, obs); kinds(actions) != "destroy:fmnext-burst-1" {
		t.Fatalf("ceiling = %s", kinds(actions))
	}
	// A new month starts from zero, billing only from the month's start.
	october := time.Date(2026, 10, 1, 1, 0, 0, 0, time.UTC)
	rolled := PoolState{SpentCents: 5000, LastAccrual: october.Add(-3 * time.Hour), Month: "2026-09"}
	next, actions = decidePool(policy, rolled, PoolObservation{Now: october, Instances: []PoolInstance{inst},
		Busy: map[string]bool{"fmnext-burst-1": true}})
	if next.Month != "2026-10" || next.SpentCents != 10 || len(actions) != 0 {
		t.Fatalf("rollover = %+v %s", next, kinds(actions))
	}
}

func TestPoolObservationFromAppStatus(t *testing.T) {
	var status poolStatus
	_ = json.Unmarshal([]byte(`{"queued":3,"runners":[
		{"name":"fmnext-runner","scope":"pool","lastSeenAt":`+itoa64(poolT0.Add(-time.Minute).UnixMilli())+`,"activeLeases":1},
		{"name":"fmnext-burst-1","scope":"pool","lastSeenAt":0,"activeLeases":1},
		{"name":"fmnext-burst-2","scope":"pool","lastSeenAt":0,"activeLeases":0}]}`), &status)
	obs := poolObservation(poolT0, status, nil)
	if obs.QueueDepth != 3 || !obs.AlwaysOnBusy || !obs.Busy["fmnext-burst-1"] || obs.Busy["fmnext-burst-2"] {
		t.Fatalf("observation = %+v", obs)
	}
	status.Runners[0].ActiveLeases = 0
	if poolObservation(poolT0, status, nil).AlwaysOnBusy {
		t.Fatal("a free always-on runner counted as busy")
	}
	status.Runners[0].LastSeenAt = poolT0.Add(-time.Hour).UnixMilli()
	if !poolObservation(poolT0, status, nil).AlwaysOnBusy {
		t.Fatal("a vanished always-on runner counted as free capacity")
	}
}

func itoa64(v int64) string { return strconv.FormatInt(v, 10) }

// fakePoolProvider records calls and enforces nothing, so the tests show
// the decision layer alone keeps to managed instances.
type fakePoolProvider struct {
	mu        sync.Mutex
	instances []PoolInstance
	created   []string
	destroyed []string
}

func (f *fakePoolProvider) List(context.Context) ([]PoolInstance, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	return append([]PoolInstance(nil), f.instances...), nil
}

func (f *fakePoolProvider) Create(_ context.Context, label string) (PoolInstance, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.created = append(f.created, label)
	instance := burst(label, poolT0)
	f.instances = append(f.instances, instance)
	return instance, nil
}

func (f *fakePoolProvider) Destroy(_ context.Context, instance PoolInstance) error {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.destroyed = append(f.destroyed, instance.Label)
	return nil
}

func TestRunnerPoolLoopAppliesAndPersistsDecisions(t *testing.T) {
	provider := &fakePoolProvider{instances: []PoolInstance{
		{ID: "x", Label: "fmnext-core-1", Tags: []string{poolTag}, CreatedAt: poolT0.Add(-time.Hour)}}}
	now := poolT0
	loop := &runnerPoolLoop{policy: testPolicy(), provider: provider,
		statePath: filepath.Join(t.TempDir(), "pool.json"), now: func() time.Time { return now }}
	status := poolStatus{Queued: 1}
	for _, step := range []time.Duration{0, time.Minute, 2 * time.Minute} {
		now = poolT0.Add(step)
		if err := loop.apply(context.Background(), status); err != nil {
			t.Fatal(err)
		}
	}
	if strings.Join(provider.created, ",") != "fmnext-burst-1" {
		t.Fatalf("created = %v", provider.created)
	}
	raw, err := os.ReadFile(loop.statePath)
	if err != nil || !strings.Contains(string(raw), `"month": "2026-09"`) {
		t.Fatalf("state not persisted: %s %v", raw, err)
	}
	// Queue drains; after the boot grace and idle timeout the runner goes.
	status = poolStatus{}
	for _, step := range []time.Duration{10 * time.Minute, 21 * time.Minute} {
		now = poolT0.Add(step)
		if err := loop.apply(context.Background(), status); err != nil {
			t.Fatal(err)
		}
	}
	if strings.Join(provider.destroyed, ",") != "fmnext-burst-1" {
		t.Fatalf("destroyed = %v", provider.destroyed)
	}
}

// fakeVultr is a local stand-in for the Vultr v2 API. No test reaches the
// real API: BaseURL always points here.
type fakeVultr struct {
	mu        sync.Mutex
	instances map[string]vultrInstance
	deleted   []string
	created   []map[string]any
	auth      []string
}

func (f *fakeVultr) ServeHTTP(w http.ResponseWriter, request *http.Request) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.auth = append(f.auth, request.Header.Get("Authorization"))
	switch {
	case request.Method == http.MethodGet && request.URL.Path == "/instances":
		var list []vultrInstance
		for _, inst := range f.instances {
			// Like Vultr, filter by tag server-side.
			for _, tag := range inst.Tags {
				if tag == request.URL.Query().Get("tag") {
					list = append(list, inst)
				}
			}
		}
		_ = json.NewEncoder(w).Encode(map[string]any{"instances": list, "meta": map[string]any{"links": map[string]any{"next": ""}}})
	case request.Method == http.MethodGet && strings.HasPrefix(request.URL.Path, "/instances/"):
		inst, ok := f.instances[strings.TrimPrefix(request.URL.Path, "/instances/")]
		if !ok {
			http.NotFound(w, request)
			return
		}
		_ = json.NewEncoder(w).Encode(map[string]any{"instance": inst})
	case request.Method == http.MethodPost && request.URL.Path == "/instances":
		var body map[string]any
		_ = json.NewDecoder(request.Body).Decode(&body)
		f.created = append(f.created, body)
		inst := vultrInstance{ID: "00000000-0000-4000-8000-00000000000" + string(rune('0'+len(f.created))),
			Label: body["label"].(string), Tags: []string{poolTag}, DateCreated: "2026-09-15T12:00:00+00:00"}
		f.instances[inst.ID] = inst
		w.WriteHeader(http.StatusAccepted)
		_ = json.NewEncoder(w).Encode(map[string]any{"instance": inst})
	case request.Method == http.MethodDelete && strings.HasPrefix(request.URL.Path, "/instances/"):
		id := strings.TrimPrefix(request.URL.Path, "/instances/")
		f.deleted = append(f.deleted, id)
		delete(f.instances, id)
		w.WriteHeader(http.StatusNoContent)
	default:
		http.NotFound(w, request)
	}
}

func TestVultrPoolOnlyEverActsOnManagedInstances(t *testing.T) {
	const (
		managedID = "11111111-1111-4111-8111-111111111111"
		coreID    = "22222222-2222-4222-8222-222222222222"
		prodID    = "33333333-3333-4333-8333-333333333333"
	)
	fake := &fakeVultr{instances: map[string]vultrInstance{
		managedID: {ID: managedID, Label: "fmnext-burst-1", Tags: []string{poolTag}, DateCreated: "2026-09-15T10:00:00+00:00"},
		coreID:    {ID: coreID, Label: "fmnext-core-1", Tags: []string{poolTag}},
		prodID:    {ID: prodID, Label: "fmnext-burst-2", Tags: []string{"production"}},
	}}
	server := httptest.NewServer(fake)
	t.Cleanup(server.Close)
	pool := &VultrPool{APIKey: "sub-user-key", Region: "ewr", Plan: "vc2-2c-4gb", SnapshotID: "snap",
		BaseURL: server.URL, Client: server.Client()}
	ctx := context.Background()
	listed, err := pool.List(ctx)
	if err != nil || len(listed) != 1 || listed[0].ID != managedID || !listed[0].CreatedAt.Equal(poolT0.Add(-2*time.Hour)) {
		t.Fatalf("list = %+v %v", listed, err)
	}
	if _, err := pool.Create(ctx, "fmnext-core-2"); err == nil {
		t.Fatal("created an instance outside the burst label space")
	}
	created, err := pool.Create(ctx, "fmnext-burst-3")
	if err != nil || created.Label != "fmnext-burst-3" {
		t.Fatalf("create = %+v %v", created, err)
	}
	body := fake.created[0]
	if body["label"] != "fmnext-burst-3" || body["hostname"] != "fmnext-burst-3" || body["snapshot_id"] != "snap" ||
		body["region"] != "ewr" || strings.Join(anyStrings(body["tags"]), ",") != poolTag {
		t.Fatalf("create body = %#v", body)
	}
	// Destroy refuses unmanaged targets, including a forged local record
	// whose live instance is something else.
	for _, target := range []PoolInstance{
		{ID: coreID, Label: "fmnext-core-1", Tags: []string{poolTag}},
		{ID: prodID, Label: "fmnext-burst-2", Tags: []string{poolTag}}, // live tags say production
		{ID: coreID, Label: "fmnext-burst-1", Tags: []string{poolTag}}, // label forged
		{ID: "../account", Label: "fmnext-burst-1", Tags: []string{poolTag}},
	} {
		if err := pool.Destroy(ctx, target); err == nil {
			t.Fatalf("destroyed %+v", target)
		}
	}
	if len(fake.deleted) != 0 {
		t.Fatalf("deleted = %v", fake.deleted)
	}
	if err := pool.Destroy(ctx, listed[0]); err != nil {
		t.Fatal(err)
	}
	if strings.Join(fake.deleted, ",") != managedID {
		t.Fatalf("deleted = %v", fake.deleted)
	}
	for _, header := range fake.auth {
		if header != "Bearer sub-user-key" {
			t.Fatalf("auth header = %q", header)
		}
	}
}

func TestPoolAPIKeyFileMustBeOwnerOnly(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "vultr.key")
	if err := os.WriteFile(path, []byte("abc\n"), 0644); err != nil {
		t.Fatal(err)
	}
	if err := os.Chmod(path, 0644); err != nil {
		t.Fatal(err)
	}
	if _, err := readPoolAPIKey(path); err == nil {
		t.Fatal("world-readable key accepted")
	}
	if err := os.Chmod(path, 0600); err != nil {
		t.Fatal(err)
	}
	if key, err := readPoolAPIKey(path); err != nil || key != "abc" {
		t.Fatalf("key = %q %v", key, err)
	}
}

func TestPoolNeverManagesTheAlwaysOnRunner(t *testing.T) {
	for _, label := range []string{"fmnext-run-1", "fmnext-run-2", "fmnext-core-1", "fmnext-app-1"} {
		if managedPoolInstance(PoolInstance{ID: "x", Label: label, Tags: []string{poolTag}}) {
			t.Fatalf("%s must never be a managed burst instance", label)
		}
	}
	if !managedPoolInstance(PoolInstance{ID: "x", Label: "fmnext-burst-1", Tags: []string{poolTag}}) {
		t.Fatal("fmnext-burst-1 is a managed burst instance")
	}
}

func TestPoolRetryDelayBacksOffAndCaps(t *testing.T) {
	if got := poolRetryDelay(0); got != poolInterval {
		t.Fatalf("healthy delay = %s", got)
	}
	if got := poolRetryDelay(1); got != 2*poolInterval {
		t.Fatalf("first failure delay = %s", got)
	}
	if got := poolRetryDelay(3); got != 8*poolInterval {
		t.Fatalf("third failure delay = %s", got)
	}
	if got := poolRetryDelay(50); got != poolMaxRetry {
		t.Fatalf("long outage delay = %s", got)
	}
}
