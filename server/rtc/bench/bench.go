package bench

import (
	"context"
	"embed"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"net/netip"
	"net/url"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"

	"forkmesh.com/mirror-node/rtc/netsim"
)

//go:embed web
var webFiles embed.FS

type Engine struct {
	Name    string
	Adapter string
	Key     func(packet []byte) (string, bool)
	Start   func(t testing.TB, routes *http.ServeMux, relay func(media netip.AddrPort) netip.AddrPort)
}

type Options struct {
	Chromium string
	Public   string
	Home     string
	Peers    []string
	Duration time.Duration
	Warmup   time.Duration
	Stagger  time.Duration
	Seed     uint64
	RelayIP  netip.Addr
}

func (o Options) withDefaults() Options {
	if len(o.Peers) == 0 {
		o.Peers = []string{"ana", "ben", "cai"}
	}
	if o.Duration <= 0 {
		o.Duration = 30 * time.Second
	}
	if o.Warmup <= 0 {
		o.Warmup = 5 * time.Second
	}
	if o.Stagger <= 0 {
		o.Stagger = time.Second
	}
	if !o.RelayIP.IsValid() {
		o.RelayIP = netip.MustParseAddr("127.0.0.2")
	}
	return o
}

func HeadlessChromium() string {
	if path := os.Getenv("FORKMESH_TEST_CHROMIUM"); path != "" {
		return path
	}
	home, _ := os.UserHomeDir()
	matches, _ := filepath.Glob(filepath.Join(home, ".cache", "ms-playwright", "chromium_headless_shell-*", "chrome-headless-shell-linux64", "chrome-headless-shell"))
	sort.Strings(matches)
	if len(matches) == 0 {
		return ""
	}
	return matches[len(matches)-1]
}

func OutputDir() (string, error) {
	if dir := os.Getenv("FORKMESH_CALL_BENCH_OUT"); dir != "" {
		return dir, os.MkdirAll(dir, 0o755)
	}
	stamp := time.Now().Format("20060102-150405")
	if scratch := os.Getenv("FORKMESH_AGENT_SCRATCH"); scratch != "" {
		dir := filepath.Join(scratch, "call-bench", stamp)
		return dir, os.MkdirAll(dir, 0o755)
	}
	return os.MkdirTemp("", "call-bench-"+stamp+"-")
}

func browserEnv(home string) []string {
	var env []string
	for _, entry := range os.Environ() {
		key, _, _ := strings.Cut(entry, "=")
		switch key {
		case "HOME", "XDG_DATA_HOME", "XDG_CONFIG_HOME", "XDG_CACHE_HOME", "FORKMESH_HOME", "FORKMESH_PROJECTS_DIR":
			continue
		}
		env = append(env, entry)
	}
	return append(env,
		"HOME="+home,
		"XDG_DATA_HOME="+filepath.Join(home, ".local", "share"),
		"XDG_CONFIG_HOME="+filepath.Join(home, ".config"),
		"XDG_CACHE_HOME="+filepath.Join(home, ".cache"),
	)
}

type browser struct {
	name    string
	command *exec.Cmd
	started time.Time
}

type run struct {
	t        testing.TB
	opts     Options
	profile  Profile
	relay    *netsim.Relay
	impaired string
	mu       sync.Mutex
	start    time.Time
	keys     map[string][]string
	reports  map[string]*Report
	raw      map[string]json.RawMessage
	logs     []string
}

func (r *run) identify(w http.ResponseWriter, req *http.Request) {
	name := req.URL.Query().Get("name")
	body, _ := io.ReadAll(io.LimitReader(req.Body, 256))
	key := strings.TrimSpace(string(body))
	if key == "" {
		http.Error(w, "no key", http.StatusBadRequest)
		return
	}
	r.mu.Lock()
	r.keys[name] = append(r.keys[name], key)
	start := r.start
	r.mu.Unlock()
	if name == r.impaired {
		r.relay.Shape(key, start, r.profile.Up, r.profile.Down)
	} else {
		r.relay.Shape(key, start, nil, nil)
	}
}

func (r *run) report(w http.ResponseWriter, req *http.Request) {
	body, _ := io.ReadAll(io.LimitReader(req.Body, 64<<20))
	var report Report
	if err := json.Unmarshal(body, &report); err != nil {
		r.t.Logf("%s: unreadable report: %v", req.URL.Query().Get("name"), err)
		report.Error = "unreadable report: " + err.Error()
	}
	name := req.URL.Query().Get("name")
	report.Name = name
	r.mu.Lock()
	r.reports[name] = &report
	r.raw[name] = body
	r.mu.Unlock()
}

func (r *run) log(w http.ResponseWriter, req *http.Request) {
	body, _ := io.ReadAll(io.LimitReader(req.Body, 16<<10))
	line := req.URL.Query().Get("name") + ": " + string(body)
	r.t.Log(line)
	r.mu.Lock()
	r.logs = append(r.logs, line)
	r.mu.Unlock()
}

func (r *run) launch(ctx context.Context, base, adapter, name string, until time.Time) *browser {
	query := url.Values{}
	query.Set("name", name)
	query.Set("adapter", adapter)
	query.Set("until", strconv.FormatInt(until.UnixMilli(), 10))
	profile, err := os.MkdirTemp(r.opts.Home, "profile-"+name+"-")
	if err != nil {
		r.t.Fatal(err)
	}
	command := exec.CommandContext(ctx, r.opts.Chromium,
		"--headless", "--no-sandbox", "--disable-gpu", "--no-first-run",
		"--user-data-dir="+profile,
		"--use-fake-ui-for-media-stream", "--use-fake-device-for-media-stream",
		"--allow-loopback-in-peer-connection",
		"--disable-features=WebRtcHideLocalIpsWithMdns",
		"--autoplay-policy=no-user-gesture-required",
		"--disable-background-timer-throttling",
		"--disable-renderer-backgrounding",
		"--disable-backgrounding-occluded-windows",
		base+"/web/bench.html?"+query.Encode())
	command.Env = browserEnv(r.opts.Home)
	prepareGroup(command)
	if err := command.Start(); err != nil {
		r.t.Fatal(err)
	}
	return &browser{name: name, command: command, started: time.Now()}
}

func RunProfile(t *testing.T, engine Engine, profile Profile, opts Options) Result {
	opts = opts.withDefaults()
	if opts.Home == "" {
		opts.Home = t.TempDir()
	}
	r := &run{
		t: t, opts: opts, profile: profile, impaired: opts.Peers[0],
		keys: map[string][]string{}, reports: map[string]*Report{}, raw: map[string]json.RawMessage{},
	}
	routes := http.NewServeMux()
	engine.Start(t, routes, func(media netip.AddrPort) netip.AddrPort {
		relay, err := netsim.Listen(netip.AddrPortFrom(opts.RelayIP, media.Port()).String(), media, netsim.Config{Key: engine.Key, Seed: opts.Seed})
		if err != nil {
			t.Fatalf("relay beside %v: %v", media, err)
		}
		t.Cleanup(func() { relay.Close() })
		r.relay = relay
		return relay.Addr()
	})
	if r.relay == nil {
		t.Fatal("the engine never asked for a relay")
	}
	routes.Handle("/web/", http.FileServer(http.FS(webFiles)))
	if opts.Public != "" {
		routes.Handle("/public/", http.StripPrefix("/public/", http.FileServer(http.Dir(opts.Public))))
	}
	routes.HandleFunc("/bench/identify", r.identify)
	routes.HandleFunc("/bench/report", r.report)
	routes.HandleFunc("/bench/log", r.log)
	web := httptest.NewServer(routes)
	t.Cleanup(web.Close)
	ctx, cancel := context.WithCancel(context.Background())
	t.Cleanup(cancel)
	clean := opts.Peers[1:]
	cpuBefore := selfCPUSeconds()
	wallBefore := time.Now()
	start := time.Now().Add(opts.Warmup + time.Duration(len(clean))*opts.Stagger)
	until := start.Add(opts.Duration)
	r.mu.Lock()
	r.start = start
	r.mu.Unlock()
	var browsers []*browser
	t.Cleanup(func() {
		for _, b := range browsers {
			killGroup(b.command)
		}
	})
	for _, name := range clean {
		browsers = append(browsers, r.launch(ctx, web.URL, engine.Adapter, name, until))
		time.Sleep(opts.Stagger)
	}
	time.Sleep(time.Until(start))
	browsers = append(browsers, r.launch(ctx, web.URL, engine.Adapter, r.impaired, until))
	deadline := until.Add(30 * time.Second)
	for time.Now().Before(deadline) {
		r.mu.Lock()
		done := len(r.reports) == len(opts.Peers)
		r.mu.Unlock()
		if done {
			break
		}
		time.Sleep(200 * time.Millisecond)
	}
	ended := time.Now()
	cpu := map[string]float64{}
	for _, b := range browsers {
		seconds := cpuSeconds(b.command.Process.Pid)
		cpu[b.name] = 100 * seconds / ended.Sub(b.started).Seconds()
	}
	serverCPU := 100 * (selfCPUSeconds() - cpuBefore) / ended.Sub(wallBefore).Seconds()
	for _, b := range browsers {
		killGroup(b.command)
	}
	browsers = nil
	r.mu.Lock()
	defer r.mu.Unlock()
	result := Result{
		Engine:    engine.Name,
		Profile:   profile,
		Started:   start,
		Seconds:   opts.Duration.Seconds(),
		Impaired:  r.impaired,
		Peers:     opts.Peers,
		CPU:       cpu,
		ServerCPU: serverCPU,
		Keys:      r.keys,
		Raw:       r.raw,
		Logs:      r.logs,
	}
	for _, key := range r.keys[r.impaired] {
		if stats, ok := r.relay.Client(key); ok {
			result.Relay = append(result.Relay, stats)
		}
	}
	for _, name := range opts.Peers {
		report := r.reports[name]
		switch {
		case report == nil:
			result.Errors = append(result.Errors, name+": no report")
		case report.Error != "":
			result.Errors = append(result.Errors, name+": "+report.Error)
		}
	}
	result.compute(r.reports, profile.Marks())
	return result
}
