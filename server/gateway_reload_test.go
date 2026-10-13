package mirrornode

import (
	"context"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"
)

// fakeReloadGateway mimics tools/mirror_gateway.py's generation protocol: it
// listens, installs its SIGHUP handler before listening, acknowledges the
// configuration it loaded at startup, and on SIGHUP snapshots the on-disk
// configuration, spends RELOAD_DELAY building the replacement, then
// acknowledges that snapshot. A SIGHUP that lands mid-reload queues exactly
// one more reload. MODE=hang never acknowledges a reload; MODE=crash exits on
// SIGHUP. Every SIGHUP appends one line to the signal log.
const fakeReloadGateway = `import hashlib,os,pathlib,signal,socket,threading,time
config=pathlib.Path(os.environ["GATEWAY_CONFIG"])
marker=pathlib.Path(os.environ["GENERATION_MARKER"])
signals=pathlib.Path(os.environ["SIGNAL_LOG"])
mode=os.environ.get("MODE","slow")
delay=float(os.environ.get("RELOAD_DELAY","0"))
def acknowledge(raw):
    marker.parent.mkdir(parents=True,exist_ok=True)
    temporary=marker.with_name(".fake-%d"%os.getpid())
    temporary.write_text(hashlib.sha256(raw).hexdigest()+"\n")
    os.replace(temporary,marker)
requested=threading.Event()
def on_hup(*_):
    with signals.open("a") as stream:
        stream.write("%d\n"%os.getpid())
    if mode=="crash":
        os._exit(3)
    requested.set()
signal.signal(signal.SIGHUP,on_hup)
signal.signal(signal.SIGTERM,lambda *_:os._exit(0))
startup=config.read_bytes()
listener=socket.socket()
listener.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
listener.bind(("127.0.0.1",int(os.environ["PORT"])))
listener.listen(64)
def serve():
    while True:
        connection,_=listener.accept()
        connection.close()
threading.Thread(target=serve,daemon=True).start()
acknowledge(startup)
while True:
    if requested.wait(0.05):
        requested.clear()
        if mode=="hang":
            continue
        snapshot=config.read_bytes()
        time.sleep(delay)
        acknowledge(snapshot)
`

type reloadGatewayFixture struct {
	daemon      *Daemon
	supervisor  *Supervisor
	gatewayPath string
	signalLog   string
}

func startReloadGateway(t *testing.T, mode string, reloadDelay time.Duration) *reloadGatewayFixture {
	t.Helper()
	directory := t.TempDir()
	gatewayPath := filepath.Join(directory, "gateway.json")
	signalLog := filepath.Join(directory, "signals")
	writeFakeGeneration(t, gatewayPath, "startup")
	address := freeAddress(t)
	supervisor := NewSupervisor()
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan struct{})
	go func() {
		supervisor.Run(ctx, []ProcessSpec{{
			Name:    "gateway",
			Program: "/usr/bin/python3",
			Args:    []string{"-c", fakeReloadGateway},
			Env: append(os.Environ(),
				"GATEWAY_CONFIG="+gatewayPath,
				"GENERATION_MARKER="+gatewayGenerationMarkerPath(gatewayPath),
				"SIGNAL_LOG="+signalLog,
				"MODE="+mode,
				"RELOAD_DELAY="+strconv.FormatFloat(reloadDelay.Seconds(), 'f', 3, 64),
				"PORT="+address[strings.LastIndex(address, ":")+1:],
			),
		}})
		close(done)
	}()
	t.Cleanup(func() {
		cancel()
		select {
		case <-done:
		case <-time.After(6 * time.Second):
			t.Error("supervisor did not stop the fake gateway")
		}
	})
	fixture := &reloadGatewayFixture{
		daemon: &Daemon{
			config:                Config{GatewayConfig: gatewayPath, GatewayProbe: address},
			supervisor:            supervisor,
			gatewayReloadDeadline: 30 * time.Second,
			gatewayRestartDelay:   func() time.Duration { return 0 },
		},
		supervisor:  supervisor,
		gatewayPath: gatewayPath,
		signalLog:   signalLog,
	}
	waitFor(t, 10*time.Second, "fake gateway startup", func() bool {
		return fixture.daemon.gatewayListening() &&
			gatewayServesCurrentGeneration(gatewayPath)
	})
	return fixture
}

func writeFakeGeneration(t *testing.T, path, generation string) {
	t.Helper()
	temporary := path + ".tmp"
	if err := os.WriteFile(temporary, []byte(generation+"\n"), 0600); err != nil {
		t.Fatal(err)
	}
	if err := os.Rename(temporary, path); err != nil {
		t.Fatal(err)
	}
}

func waitFor(t *testing.T, timeout time.Duration, what string, condition func() bool) {
	t.Helper()
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		if condition() {
			return
		}
		time.Sleep(20 * time.Millisecond)
	}
	t.Fatalf("timed out waiting for %s", what)
}

func (f *reloadGatewayFixture) signals() int {
	raw, err := os.ReadFile(f.signalLog)
	if err != nil {
		return 0
	}
	return strings.Count(string(raw), "\n")
}

// 2026-09-12: a validated reload of a ~2 GB repository takes longer than the
// old 3-second acknowledgement deadline on a 1 vCPU mirror, and every refs
// change hard-restarted every mirror's gateway at once.
func TestSlowGatewayReloadIsAdoptedWithoutRestart(t *testing.T) {
	f := startReloadGateway(t, "slow", 3500*time.Millisecond)
	instance, _ := f.supervisor.Instance("gateway")
	writeFakeGeneration(t, f.gatewayPath, "refs changed")
	started := time.Now()
	if outcome := f.daemon.adoptGatewayGeneration(context.Background()); outcome != gatewayReloadServed {
		t.Fatalf("slow reload outcome = %v, want served", outcome)
	}
	if elapsed := time.Since(started); elapsed < 3*time.Second {
		t.Fatalf("reload finished in %s; the fake should take 3.5 s", elapsed)
	}
	current, running := f.supervisor.Instance("gateway")
	_, restarts := f.supervisor.Snapshot()
	if !running || current != instance || restarts["gateway"] != 0 {
		t.Fatalf("slow reload replaced the gateway: instance %d->%d running=%v restarts=%v",
			instance, current, running, restarts)
	}
	if f.signals() != 1 {
		t.Fatalf("gateway received %d reload signals, want 1", f.signals())
	}
	if !f.daemon.gatewayServing() {
		t.Fatal("hot-reloaded gateway must keep counting as serving")
	}
}

func TestStalledGatewayIsRestartedAfterDeadline(t *testing.T) {
	f := startReloadGateway(t, "hang", 0)
	f.daemon.gatewayReloadDeadline = 1500 * time.Millisecond
	instance, _ := f.supervisor.Instance("gateway")
	writeFakeGeneration(t, f.gatewayPath, "refs changed")
	if outcome := f.daemon.adoptGatewayGeneration(context.Background()); outcome != gatewayReloadRestarted {
		t.Fatalf("stalled reload outcome = %v, want restarted", outcome)
	}
	f.daemon.mu.RLock()
	replacing := f.daemon.gatewayReplacing
	f.daemon.mu.RUnlock()
	if replacing != instance {
		t.Fatalf("restart did not record the replaced instance: %d != %d", replacing, instance)
	}
	// The endpoint lease must not see the doomed process as serving, and the
	// replacement acknowledges the new configuration on its own at startup.
	waitFor(t, 10*time.Second, "replacement gateway", func() bool {
		current, running := f.supervisor.Instance("gateway")
		return running && current != instance &&
			gatewayServesCurrentGeneration(f.gatewayPath) && f.daemon.gatewayServing()
	})
	if _, restarts := f.supervisor.Snapshot(); restarts["gateway"] != 1 {
		t.Fatalf("restarts = %v, want exactly one", restarts)
	}
}

func TestStalledGatewayRestartWaitsForJitterAndHonorsLateAck(t *testing.T) {
	f := startReloadGateway(t, "hang", 0)
	f.daemon.gatewayReloadDeadline = 300 * time.Millisecond
	f.daemon.gatewayRestartDelay = func() time.Duration {
		// The acknowledgement lands during the per-node restart delay.
		go func() {
			time.Sleep(100 * time.Millisecond)
			_ = os.WriteFile(gatewayGenerationMarkerPath(f.gatewayPath),
				[]byte(gatewayConfigDigest(f.gatewayPath)+"\n"), 0600)
		}()
		return 700 * time.Millisecond
	}
	writeFakeGeneration(t, f.gatewayPath, "refs changed")
	if outcome := f.daemon.adoptGatewayGeneration(context.Background()); outcome != gatewayReloadServed {
		t.Fatalf("late acknowledgement outcome = %v, want served", outcome)
	}
	if _, restarts := f.supervisor.Snapshot(); restarts["gateway"] != 0 {
		t.Fatalf("a late acknowledgement still restarted the gateway: %v", restarts)
	}
}

func TestDeadGatewayIsRespawnedAndAcknowledgesAtStartup(t *testing.T) {
	f := startReloadGateway(t, "crash", 0)
	instance, _ := f.supervisor.Instance("gateway")
	writeFakeGeneration(t, f.gatewayPath, "refs changed")
	if outcome := f.daemon.adoptGatewayGeneration(context.Background()); outcome != gatewayReloadUnavailable {
		t.Fatalf("dead gateway outcome = %v, want unavailable", outcome)
	}
	waitFor(t, 15*time.Second, "respawned gateway", func() bool {
		current, running := f.supervisor.Instance("gateway")
		return running && current != instance &&
			gatewayServesCurrentGeneration(f.gatewayPath) && f.daemon.gatewayListening()
	})
	if outcome := f.daemon.adoptGatewayGeneration(context.Background()); outcome != gatewayReloadServed {
		t.Fatalf("respawned gateway outcome = %v, want served", outcome)
	}
	if f.signals() != 1 {
		t.Fatalf("respawned gateway was signalled again: %d signals", f.signals())
	}
}

func TestGatewayGenerationChangesDuringReloadCoalesce(t *testing.T) {
	f := startReloadGateway(t, "slow", 1500*time.Millisecond)
	writeFakeGeneration(t, f.gatewayPath, "generation A")
	var outcomes sync.Map
	var callers sync.WaitGroup
	adopt := func(name string) {
		defer callers.Done()
		outcomes.Store(name, f.daemon.adoptGatewayGeneration(context.Background()))
	}
	callers.Add(1)
	go adopt("first")
	waitFor(t, 5*time.Second, "first reload signal", func() bool { return f.signals() == 1 })
	// Generation B lands while the gateway is still building A. Two more
	// changes queue behind the in-flight adoption.
	time.Sleep(300 * time.Millisecond)
	writeFakeGeneration(t, f.gatewayPath, "generation B")
	callers.Add(2)
	go adopt("second")
	go adopt("third")
	callers.Wait()
	outcomes.Range(func(name, outcome any) bool {
		if outcome.(gatewayReloadOutcome) != gatewayReloadServed {
			t.Errorf("%s adoption outcome = %v, want served", name, outcome)
		}
		return true
	})
	if !gatewayServesCurrentGeneration(f.gatewayPath) {
		t.Fatal("gateway does not serve the newest generation")
	}
	if got := f.signals(); got != 2 {
		t.Fatalf("gateway received %d reload signals, want 2 (A, then one coalesced B)", got)
	}
	if _, restarts := f.supervisor.Snapshot(); restarts["gateway"] != 0 {
		t.Fatalf("coalesced reloads restarted the gateway: %v", restarts)
	}
}

func TestInterruptedReloadDoesNotRestartLiveGateway(t *testing.T) {
	f := startReloadGateway(t, "slow", 2*time.Second)
	writeFakeGeneration(t, f.gatewayPath, "refs changed")
	ctx, cancel := context.WithTimeout(context.Background(), 300*time.Millisecond)
	defer cancel()
	if outcome := f.daemon.adoptGatewayGeneration(ctx); outcome != gatewayReloadInterrupted {
		t.Fatalf("interrupted reload outcome = %v, want interrupted", outcome)
	}
	if _, restarts := f.supervisor.Snapshot(); restarts["gateway"] != 0 {
		t.Fatalf("an interrupted reload restarted the gateway: %v", restarts)
	}
	// The in-flight reload still completes; a later adoption sees it without
	// signalling again.
	if outcome := f.daemon.adoptGatewayGeneration(context.Background()); outcome != gatewayReloadServed {
		t.Fatalf("follow-up outcome = %v, want served", outcome)
	}
	if f.signals() != 1 {
		t.Fatalf("follow-up re-signalled a reload already in flight: %d signals", f.signals())
	}
}

func TestSupervisorReloadCoalescesWithQueuedSignal(t *testing.T) {
	supervisor := NewSupervisor()
	if supervisor.Reload("gateway") {
		t.Fatal("a reload without a process must be refused")
	}
	queue := make(chan struct{}, 1)
	supervisor.reload["gateway"] = queue
	if !supervisor.Reload("gateway") || !supervisor.Reload("gateway") {
		t.Fatal("a reload already queued must coalesce, not report failure")
	}
	if len(queue) != 1 {
		t.Fatalf("coalesced reloads queued %d signals, want 1", len(queue))
	}
}
