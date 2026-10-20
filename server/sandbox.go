package mirrornode

// Per-job resource containment for runner agent jobs (docs/RUNNERS.md).
//
// When systemd-run is usable, each provider CLI runs in its own transient
// scope with MemoryMax and CPUQuota, and as runner.jobUid when the daemon is
// root. Without systemd the CLI runs directly under a Go-side timeout (the
// job context) and an RLIMIT_AS set through the shell's ulimit, which is
// coarse: V8 reserves a large address space up front, so the limit carries
// generous headroom and only stops a runaway, not a merely greedy, job.

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"fmt"
	"io"
	"log"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"
)

// Headroom added to 4 x MemoryMax for the fallback address-space limit.
const sandboxAddressHeadroom = 8 << 30

const jobTmpDir = ".tmp"

type jobActivity struct {
	mu      sync.Mutex
	running int
}

type jobSandbox struct {
	memoryMax string
	swapMax   string
	cpuQuota  string
	uid       int
	gid       int
	userName  string // the job user's login name, for USER/LOGNAME
	home      string // the job user's persistent HOME (provider logins)
	// workRoot holds the per-job workdirs; a workdir's job gets the
	// per-session HOME <workRoot>/.homes/<workdir name> (jobhome.go).
	workRoot string
	// systemdRun is the systemd-run path, or "" for the rlimit fallback.
	systemdRun string
	systemctl  string // stops a job's scope once its main process exits
	user       bool   // --user manager (the daemon is not root)
	direct     bool
	// dnsAllow are loopback resolvers (e.g. systemd-resolved's 127.0.0.53)
	// that stay reachable although the scope denies loopback addresses.
	dnsAllow  []string
	unit      func() string
	sharedTmp []string
	active    *jobActivity
	accounts  *runnerAccounts
	// homeGate opens the persistent home to the job user only while a
	// sign-in or terminal session runs there (jobhome.go).
	homeGate *persistentHomeGate

	noLimits       bool
	privateXDG     bool
	extraEnv       map[string]bool
	credentialDirs map[string]string
}

// jobDeniedAddresses is what a job scope may never reach (system manager
// only: a user manager cannot attach the BPF filter). The runner's nftables
// rules for the job user are the primary control; this is defence in depth.
// Loopback is included, so a job cannot talk to the daemon's local status
// port or other local services; a job's own dev server on 127.0.0.1 is
// unreachable too, which is the accepted cost.
var jobDeniedAddresses = []string{
	"127.0.0.0/8", "::1/128", "169.254.0.0/16", "fe80::/10",
	"10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16", "100.64.0.0/10", "fc00::/7",
}

// detectJobSandbox picks systemd-run when a manager is reachable: the system
// manager for root, the user manager (XDG_RUNTIME_DIR) otherwise.
func detectJobSandbox(cfg *RunnerConfig, lookPath func(string) (string, error),
	getenv func(string) string, euid int, exists func(string) bool) (jobSandbox, error) {
	box := jobSandbox{memoryMax: cfg.JobMemoryMax, swapMax: cfg.JobMemorySwapMax, cpuQuota: cfg.JobCPUQuota,
		workRoot: cfg.WorkRoot, active: &jobActivity{}}
	if cfg.JobUID != 0 {
		if euid != 0 {
			return box, fmt.Errorf("runner.jobUid needs the daemon to run as root")
		}
		box.uid, box.gid = cfg.JobUID, cfg.JobUID
		box.sharedTmp = jobSharedTmpDirs
	} else if euid == 0 {
		// Jobs as root would own the node key and the daemon: never.
		return box, fmt.Errorf("a runner daemon running as root needs runner.jobUid or runner.jobUser (a dedicated job user such as forkmesh-job)")
	}
	if directJobUID {
		box.direct = box.uid != 0
		return box, nil
	}
	path, err := lookPath("systemd-run")
	if err != nil {
		if box.uid != 0 {
			return box, fmt.Errorf("runner.jobUid needs systemd-run")
		}
		return box, nil
	}
	if euid == 0 {
		if exists("/run/systemd/system") {
			box.systemdRun = path
		}
	} else if runtime := getenv("XDG_RUNTIME_DIR"); runtime != "" &&
		exists(filepath.Join(runtime, "systemd", "private")) {
		box.systemdRun, box.user = path, true
	}
	if box.systemdRun != "" {
		if systemctl, err := lookPath("systemctl"); err == nil {
			box.systemctl = systemctl
		}
	}
	if box.uid != 0 && box.systemdRun == "" {
		return box, fmt.Errorf("runner.jobUid needs a running systemd")
	}
	return box, nil
}

func (b jobSandbox) sandboxed() bool { return b.systemdRun != "" }

// argv returns the program and arguments that run name args... in the box.
func (b jobSandbox) argv(name string, args []string) (string, []string) {
	unit := ""
	if b.unit != nil {
		unit = b.unit()
	}
	return b.argvUnit(unit, name, args)
}

func (b jobSandbox) argvUnit(unit, name string, args []string) (string, []string) {
	if b.systemdRun != "" {
		wrapped := []string{"--scope", "--quiet", "--collect"}
		if b.user {
			wrapped = append(wrapped, "--user")
		}
		if unit != "" {
			wrapped = append(wrapped, "--unit="+unit)
		}
		swapMax := b.swapMax
		if swapMax == "" {
			swapMax = "0"
		}
		if !b.noLimits {
			wrapped = append(wrapped, "-p", "MemoryMax="+b.memoryMax,
				"-p", "MemorySwapMax="+swapMax, "-p", "CPUQuota="+b.cpuQuota, "-p", "TasksMax=512")
		}
		if !b.user {
			wrapped = append(wrapped, "-p", "IPAddressDeny="+strings.Join(jobDeniedAddresses, " "))
			if len(b.dnsAllow) != 0 {
				wrapped = append(wrapped, "-p", "IPAddressAllow="+strings.Join(b.dnsAllow, " "))
			}
		}
		if b.uid != 0 {
			gid := b.gid
			if gid == 0 {
				gid = b.uid
			}
			wrapped = append(wrapped, "--uid="+strconv.Itoa(b.uid), "--gid="+strconv.Itoa(gid))
		}
		wrapped = append(wrapped, "--", name)
		return b.systemdRun, append(wrapped, args...)
	}
	if b.noLimits || !addressLimitSupported {
		return name, args
	}
	limitKiB := b.addressLimit() >> 10
	script := "ulimit -v " + strconv.FormatUint(limitKiB, 10) + ` && exec "$@"`
	return "/bin/sh", append([]string{"-c", script, "forkmesh-job", name}, args...)
}

// sessionCommand restores a session HOME after the uid switch.
// `systemd-run --scope --uid=` resets HOME (with USER, LOGNAME and SHELL)
// from the job user's passwd entry, which is the persistent home holding
// the provider sign-ins; without this a job ran there instead of in its
// per-session home (jobhome.go), and that home is locked away from the job
// user between sign-ins.
func (b jobSandbox) sessionCommand(home, name string, args []string) (string, []string) {
	if b.systemdRun == "" || b.user || b.uid == 0 || !b.isSessionHome(home) {
		return name, args
	}
	return "/usr/bin/env", append([]string{"--", "HOME=" + home, name}, args...)
}

// loopbackResolvers returns the loopback nameservers of a resolv.conf.
func loopbackResolvers(resolvConf string) []string {
	var out []string
	for _, line := range strings.Split(resolvConf, "\n") {
		fields := strings.Fields(line)
		if len(fields) < 2 || fields[0] != "nameserver" {
			continue
		}
		if ip := net.ParseIP(fields[1]); ip != nil && ip.IsLoopback() {
			out = append(out, ip.String())
		}
	}
	return out
}

// addressLimit is the fallback RLIMIT_AS in bytes.
func (b jobSandbox) addressLimit() uint64 {
	bytes, ok := parseSystemdSize(b.memoryMax)
	if !ok {
		bytes = 700 << 20
	}
	return 4*bytes + sandboxAddressHeadroom
}

func parseSystemdSize(value string) (uint64, bool) {
	if !systemdSizePattern.MatchString(value) {
		return 0, false
	}
	shift := 0
	switch value[len(value)-1] {
	case 'K':
		shift = 10
	case 'M':
		shift = 20
	case 'G':
		shift = 30
	case 'T':
		shift = 40
	}
	digits := strings.TrimRight(value, "KMGT")
	number, err := strconv.ParseUint(digits, 10, 64)
	if err != nil {
		return 0, false
	}
	return number << shift, true
}

// jobEnvironmentAllowed are the daemon variables a job may inherit: an
// allowlist, so nothing the daemon's unit adds later (tokens, config paths)
// reaches agent code by accident. Provider keys the operator set for the
// daemon are passed on purpose (mode A provider credentials).
var jobEnvironmentAllowed = map[string]bool{
	"PATH": true, "LANG": true, "LANGUAGE": true, "TZ": true, "TERM": true,
	"ANTHROPIC_API_KEY": true, "ANTHROPIC_AUTH_TOKEN": true, "ANTHROPIC_BASE_URL": true,
	"CLAUDE_CODE_OAUTH_TOKEN": true, "OPENAI_API_KEY": true, "OPENAI_BASE_URL": true,
	"HTTP_PROXY": true, "HTTPS_PROXY": true, "NO_PROXY": true,
	"http_proxy": true, "https_proxy": true, "no_proxy": true,
}

// environment is the job's environment with the job user's persistent HOME
// (the provider sign-in writes there).
func (b jobSandbox) environment() []string {
	return b.environmentFor(b.home)
}

// environmentFor builds the allowlisted environment with HOME=home, and
// TMPDIR=<home>/tmp when home is a per-session home.
func (b jobSandbox) environmentFor(home string) []string {
	return b.environmentWith(home, "")
}

func (b jobSandbox) environmentWith(home, tmp string) []string {
	env := make([]string, 0, 24)
	for _, value := range os.Environ() {
		name, _, _ := strings.Cut(value, "=")
		switch {
		case jobEnvironmentAllowed[name], b.extraEnv[name], strings.HasPrefix(name, "LC_"):
			env = append(env, value)
		case b.user && (name == "XDG_RUNTIME_DIR" || name == "DBUS_SESSION_BUS_ADDRESS"):
			// systemd-run --user needs them to reach the user manager.
			env = append(env, value)
		}
	}
	if home == "" && b.uid == 0 {
		home = os.Getenv("HOME")
	}
	if home != "" {
		env = append(env, "HOME="+home)
	}
	if tmp == "" && b.isSessionHome(home) {
		tmp = filepath.Join(home, "tmp")
	}
	if tmp != "" {
		env = append(env, "TMPDIR="+tmp, "CLAUDE_CODE_TMPDIR="+tmp)
	}
	if b.privateXDG && b.isSessionHome(home) {
		env = append(env, "XDG_CONFIG_HOME="+filepath.Join(home, ".config"),
			"XDG_CACHE_HOME="+filepath.Join(home, ".cache"),
			"XDG_DATA_HOME="+filepath.Join(home, ".local", "share"),
			"XDG_STATE_HOME="+filepath.Join(home, ".local", "state"))
	}
	name := b.userName
	if name == "" && b.uid == 0 {
		name = os.Getenv("USER")
	}
	if name != "" {
		env = append(env, "USER="+name, "LOGNAME="+name)
	}
	return append(env, "GIT_TERMINAL_PROMPT=0")
}

// runner returns an agentCommandRunner that runs every CLI in this box.
// A command whose dir is a job workdir runs with that session's HOME and
// TMPDIR; its scope is stopped when the main process ends, so nothing the
// job started in the background outlives it.
func (b jobSandbox) runner() agentCommandRunner {
	return func(ctx context.Context, dir, stdin string, stdout io.Writer, name string, args ...string) ([]byte, error) {
		unit := ""
		if b.unit != nil {
			unit = b.unit()
		} else if b.systemdRun != "" {
			unit = randomJobUnit()
		}
		b.begin()
		defer b.end()
		home := b.homeFor(dir)
		if home == "" {
			home = b.home
		}
		name, args = b.sessionCommand(home, name, args)
		program, argv := b.argvUnit(unit, name, args)
		command := exec.CommandContext(ctx, program, argv...)
		command.Dir = dir
		tmp := ""
		if !b.isSessionHome(home) {
			var err error
			if tmp, err = b.openJobTmp(unit); err != nil {
				return nil, err
			}
			defer b.closeJobTmp(tmp)
		}
		command.Env = b.environmentWith(home, tmp)
		command.Stdin = strings.NewReader(stdin)
		stderr := &cappedBuffer{limit: 64 << 10}
		command.Stdout, command.Stderr = stdout, stderr
		command.SysProcAttr = &syscall.SysProcAttr{Setpgid: true, Credential: b.credential()}
		command.Cancel = func() error {
			return syscall.Kill(-command.Process.Pid, syscall.SIGKILL)
		}
		command.WaitDelay = 10 * time.Second
		err := command.Run()
		b.stopScope(unit)
		return stderr.Bytes(), err
	}
}

func (b jobSandbox) credential() *syscall.Credential {
	if !b.direct {
		return nil
	}
	return &syscall.Credential{Uid: uint32(b.uid), Gid: uint32(b.gidOrUID()), Groups: []uint32{}}
}

func (b jobSandbox) killJobUserProcesses() {
	if !b.direct {
		return
	}
	command := exec.Command("/bin/kill", "-KILL", "-1")
	command.Env = []string{"PATH=/usr/bin:/bin"}
	command.Dir = "/"
	command.SysProcAttr = &syscall.SysProcAttr{Setpgid: true, Credential: b.credential()}
	_ = command.Run()
}

// stopScope ends a finished job's scope and everything still in it.
func (b jobSandbox) stopScope(unit string) {
	if unit == "" || b.systemctl == "" {
		return
	}
	args := []string{"stop", "--quiet", unit + ".scope"}
	if b.user {
		args = append([]string{"--user"}, args...)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	_ = exec.CommandContext(ctx, b.systemctl, args...).Run()
}

func randomJobUnit() string {
	raw := make([]byte, 8)
	_, _ = rand.Read(raw)
	return "fmnext-job-" + hex.EncodeToString(raw)
}

func (b jobSandbox) openJobTmp(unit string) (string, error) {
	if unit == "" || b.workRoot == "" {
		return "", nil
	}
	if err := os.MkdirAll(b.workRoot, 0o700); err != nil {
		return "", err
	}
	root := filepath.Join(b.workRoot, jobTmpDir)
	if err := ensureDaemonDir(root, 0o711); err != nil {
		return "", err
	}
	tmp := filepath.Join(root, unit)
	if err := removeTreeUnder(root, tmp); err != nil {
		return "", err
	}
	if err := os.Mkdir(tmp, 0o700); err != nil {
		return "", err
	}
	if b.uid != 0 {
		if err := os.Lchown(tmp, b.uid, b.gidOrUID()); err != nil {
			_ = removeTreeUnder(root, tmp)
			return "", err
		}
	}
	return tmp, nil
}

func (b jobSandbox) closeJobTmp(tmp string) {
	if tmp != "" {
		_ = removeTreeUnder(filepath.Dir(tmp), tmp)
	}
}

func (b jobSandbox) begin() {
	if b.active == nil {
		return
	}
	b.active.mu.Lock()
	b.active.running++
	b.active.mu.Unlock()
}

func (b jobSandbox) end() {
	if b.active == nil {
		return
	}
	b.active.mu.Lock()
	defer b.active.mu.Unlock()
	b.active.running--
	if b.active.running == 0 {
		b.sweepSharedTmp()
	}
}

func (b jobSandbox) sweepIdle() {
	if b.active == nil {
		return
	}
	b.active.mu.Lock()
	defer b.active.mu.Unlock()
	if b.active.running == 0 {
		b.sweepSharedTmp()
	}
}

func (b jobSandbox) sweepSharedTmp() {
	b.killJobUserProcesses()
	if b.workRoot != "" {
		root := filepath.Join(b.workRoot, jobTmpDir)
		if entries, err := os.ReadDir(root); err == nil {
			for _, entry := range entries {
				_ = removeTreeUnder(root, filepath.Join(root, entry.Name()))
			}
		}
	}
	if b.uid == 0 {
		return
	}
	for _, dir := range b.sharedTmp {
		entries, err := os.ReadDir(dir)
		if err != nil {
			continue
		}
		for _, entry := range entries {
			path := filepath.Join(dir, entry.Name())
			info, err := os.Lstat(path)
			if err != nil {
				continue
			}
			if stat, ok := info.Sys().(*syscall.Stat_t); ok && int(stat.Uid) == b.uid {
				_ = removeTreeUnder(dir, path)
			}
		}
	}
}

var mirrorJobEnvironmentAllowed = map[string]bool{
	"OLLAMA_HOST": true, "CODEX_OSS_BASE_URL": true, "CODEX_OSS_PORT": true,
	"SSL_CERT_FILE": true, "SSL_CERT_DIR": true, "NODE_EXTRA_CA_CERTS": true,
}

func detectMirrorJobSandbox(workRoot, home string, lookPath func(string) (string, error),
	getenv func(string) string, euid int, exists func(string) bool) jobSandbox {
	box := jobSandbox{workRoot: workRoot, home: home, active: &jobActivity{}, noLimits: true,
		privateXDG: true, extraEnv: mirrorJobEnvironmentAllowed, credentialDirs: map[string]string{}}
	for variable, rel := range map[string]string{
		"CLAUDE_CONFIG_DIR": jobCredentialFiles[0], "CODEX_HOME": jobCredentialFiles[1],
	} {
		if dir := strings.TrimSpace(getenv(variable)); dir != "" && filepath.IsAbs(dir) {
			box.credentialDirs[rel] = filepath.Clean(dir)
		}
	}
	if euid == 0 {
		return box
	}
	runtime := getenv("XDG_RUNTIME_DIR")
	if runtime == "" || !exists(filepath.Join(runtime, "systemd", "private")) {
		return box
	}
	path, err := lookPath("systemd-run")
	if err != nil {
		return box
	}
	box.systemdRun, box.user = path, true
	if systemctl, err := lookPath("systemctl"); err == nil {
		box.systemctl = systemctl
	}
	return box
}

func agentWorkOutsideNodeKey(workRoot, identityKey string) error {
	if strings.TrimSpace(identityKey) == "" || strings.TrimSpace(workRoot) == "" {
		return nil
	}
	root := safeDeleteClean(workRoot)
	keyDir := safeDeleteClean(filepath.Dir(identityKey))
	if safeDeleteWithin(root, keyDir) || safeDeleteWithin(keyDir, root) {
		return fmt.Errorf("the agent workspace %s shares a folder with the node key %s, so the agent did not run; move gatewayConfig out of the key's folder", root, identityKey)
	}
	return nil
}

func newMirrorAgentJobs(d *Daemon) *agentJobRunner {
	home, _ := os.UserHomeDir()
	return newMirrorAgentJobsWith(d, home, exec.LookPath, os.Getenv, os.Geteuid(),
		func(path string) bool { _, err := os.Stat(path); return err == nil })
}

func newMirrorAgentJobsWith(d *Daemon, home string, lookPath func(string) (string, error),
	getenv func(string) string, euid int, exists func(string) bool) *agentJobRunner {
	r := newAgentJobRunner(d)
	box := detectMirrorJobSandbox(filepath.Join(r.dir, "work"), home, lookPath, getenv, euid, exists)
	r.mirrorBox = &box
	r.command = box.runner()
	switch {
	case box.sandboxed():
		log.Printf("agent: mirror jobs run in systemd --user scopes with an allowlisted environment and a private HOME under %s; they still run as this node's user", filepath.Join(box.workRoot, jobHomesDir))
	case euid == 0:
		log.Printf("agent: WARNING this mirror runs as root, so its agent jobs do too (allowlisted environment, private HOME under %s); use role runner with a job user for untrusted owners", filepath.Join(box.workRoot, jobHomesDir))
	default:
		log.Printf("agent: systemd-run --user unavailable; mirror jobs run directly with an allowlisted environment and a private HOME under %s; they still run as this node's user", filepath.Join(box.workRoot, jobHomesDir))
	}
	return r
}
