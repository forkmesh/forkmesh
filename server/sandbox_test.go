package mirrornode

import (
	"context"
	"errors"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
)

func TestDetectJobSandboxPicksSystemdOrFallsBack(t *testing.T) {
	cfg := &RunnerConfig{JobMemoryMax: "700M", JobCPUQuota: "90%"}
	found := func(string) (string, error) { return "/usr/bin/systemd-run", nil }
	missing := func(string) (string, error) { return "", errors.New("no") }
	env := func(name string) string {
		if name == "XDG_RUNTIME_DIR" {
			return "/run/user/1000"
		}
		return ""
	}
	noEnv := func(string) string { return "" }
	yes := func(string) bool { return true }
	no := func(string) bool { return false }

	if _, err := detectJobSandbox(cfg, found, env, 0, yes); err == nil {
		t.Fatal("a root daemon without a job user was accepted")
	}
	rootCfg := &RunnerConfig{JobMemoryMax: "700M", JobCPUQuota: "90%", JobUID: 2001}
	box, err := detectJobSandbox(rootCfg, found, env, 0, yes)
	if err != nil || box.systemdRun == "" || box.user || box.systemctl == "" {
		t.Fatalf("root with systemd: %+v %v", box, err)
	}
	box, err = detectJobSandbox(cfg, found, env, 1000, yes)
	if err != nil || box.systemdRun == "" || !box.user {
		t.Fatalf("user manager: %+v %v", box, err)
	}
	box, err = detectJobSandbox(cfg, found, noEnv, 1000, yes)
	if err != nil || box.sandboxed() {
		t.Fatalf("no user manager should fall back: %+v %v", box, err)
	}
	box, err = detectJobSandbox(cfg, missing, env, 1000, yes)
	if err != nil || box.sandboxed() {
		t.Fatalf("no systemd-run should fall back: %+v %v", box, err)
	}
	if _, err = detectJobSandbox(rootCfg, found, env, 0, no); err == nil {
		t.Fatal("a job user without a running systemd was accepted")
	}
	withUID := &RunnerConfig{JobMemoryMax: "1G", JobCPUQuota: "50%", JobUID: 2001}
	if _, err := detectJobSandbox(withUID, found, env, 1000, yes); err == nil {
		t.Fatal("jobUid accepted without root")
	}
	if _, err := detectJobSandbox(withUID, missing, env, 0, yes); err == nil {
		t.Fatal("jobUid accepted without systemd-run")
	}
	if box, err := detectJobSandbox(withUID, found, env, 0, yes); err != nil || box.uid != 2001 {
		t.Fatalf("root jobUid: %+v %v", box, err)
	}
}

func TestJobSandboxArgv(t *testing.T) {
	box := jobSandbox{memoryMax: "700M", cpuQuota: "90%", systemdRun: "/usr/bin/systemd-run", user: true}
	program, args := box.argv("claude", []string{"-p", "x"})
	got := program + " " + strings.Join(args, " ")
	want := "/usr/bin/systemd-run --scope --quiet --collect --user -p MemoryMax=700M -p MemorySwapMax=0 -p CPUQuota=90% -p TasksMax=512 -- claude -p x"
	if got != want {
		t.Fatalf("user scope argv = %q", got)
	}
	box.user, box.uid, box.dnsAllow = false, 2001, []string{"127.0.0.53"}
	_, args = box.argv("codex", nil)
	joined := strings.Join(args, " ")
	if !strings.Contains(joined, "--uid=2001 --gid=2001 -- codex") || strings.Contains(joined, "--user") ||
		!strings.Contains(joined, "IPAddressDeny=127.0.0.0/8 ::1/128 169.254.0.0/16") ||
		!strings.Contains(joined, "10.0.0.0/8 172.16.0.0/12 192.168.0.0/16 100.64.0.0/10 fc00::/7") ||
		!strings.Contains(joined, "-p IPAddressAllow=127.0.0.53") {
		t.Fatalf("root scope argv = %q", joined)
	}
	box.swapMax = "1G"
	if _, args = box.argv("claude", nil); !strings.Contains(strings.Join(args, " "), "-p MemorySwapMax=1G -p CPUQuota=90%") {
		t.Fatalf("swap argv = %q", args)
	}
	fallback := jobSandbox{memoryMax: "1G"}
	program, args = fallback.argv("claude", []string{"-p"})
	wantKiB := (4*(uint64(1)<<30) + sandboxAddressHeadroom) >> 10
	if program != "/bin/sh" || args[0] != "-c" || !strings.Contains(args[1], "ulimit -v "+strconv.FormatUint(wantKiB, 10)) ||
		strings.Join(args[2:], " ") != "forkmesh-job claude -p" {
		t.Fatalf("fallback argv = %s %q", program, args)
	}
}

func TestFallbackSandboxAppliesTheAddressLimitAndDropsNodeSecrets(t *testing.T) {
	t.Setenv("FORKMESH_MIRROR_NODE_CONFIG", "/secret/node.json")
	t.Setenv("VULTR_API_KEY", "k")
	box := jobSandbox{memoryMax: "700M", cpuQuota: "90%"}
	out := &cappedBuffer{limit: 4096}
	stderr, err := box.runner()(context.Background(), t.TempDir(), "", out, "sh", "-c",
		`ulimit -v; echo "cfg=${FORKMESH_MIRROR_NODE_CONFIG:-none} vultr=${VULTR_API_KEY:-none}"`)
	if err != nil {
		t.Fatalf("%v: %s", err, stderr)
	}
	lines := strings.Split(strings.TrimSpace(out.String()), "\n")
	if len(lines) != 2 || lines[0] != strconv.FormatUint(box.addressLimit()>>10, 10) || lines[1] != "cfg=none vultr=none" {
		t.Fatalf("sandboxed output = %q", out.String())
	}
}

func TestParseSystemdSize(t *testing.T) {
	for value, want := range map[string]uint64{"700M": 700 << 20, "2G": 2 << 30, "512K": 512 << 10, "1048576": 1 << 20} {
		if got, ok := parseSystemdSize(value); !ok || got != want {
			t.Fatalf("%s = %d %v", value, got, ok)
		}
	}
	for _, bad := range []string{"", "0", "7P", "-1M", "1.5G"} {
		if _, ok := parseSystemdSize(bad); ok {
			t.Fatalf("%q parsed", bad)
		}
	}
}

func TestLoopbackResolvers(t *testing.T) {
	got := loopbackResolvers("# x\nnameserver 127.0.0.53\nnameserver 108.61.10.10\noptions edns0\nnameserver ::1\n")
	if strings.Join(got, ",") != "127.0.0.53,::1" {
		t.Fatalf("resolvers = %v", got)
	}
}

func TestJobEnvironmentIsAnAllowlist(t *testing.T) {
	t.Setenv("FORKMESH_SOMETHING_NEW", "secret")
	t.Setenv("CLAUDE_CONFIG_DIR", "/root/.claude")
	t.Setenv("RQLITE_PASSWORD", "pw")
	t.Setenv("LC_ALL", "C.UTF-8")
	t.Setenv("ANTHROPIC_API_KEY", "k")
	work := t.TempDir()
	box := jobSandbox{workRoot: work, uid: 2001, userName: "forkmesh-job", home: "/var/lib/forkmesh-job"}
	home := box.homeFor(filepath.Join(work, "abc"))
	if home != filepath.Join(work, jobHomesDir, "abc") || box.homeFor(filepath.Join(work, ".publish")) != "" ||
		box.homeFor("/elsewhere/abc") != "" {
		t.Fatalf("homeFor = %q", home)
	}
	env := strings.Join(box.environmentFor(home), "\n")
	for _, bad := range []string{"FORKMESH_SOMETHING_NEW", "CLAUDE_CONFIG_DIR", "RQLITE_PASSWORD"} {
		if strings.Contains(env, bad) {
			t.Fatalf("%s leaked into the job environment:\n%s", bad, env)
		}
	}
	for _, want := range []string{"HOME=" + home, "TMPDIR=" + filepath.Join(home, "tmp"), "USER=forkmesh-job",
		"LC_ALL=C.UTF-8", "ANTHROPIC_API_KEY=k", "GIT_TERMINAL_PROMPT=0"} {
		if !strings.Contains(env, want) {
			t.Fatalf("job environment lacks %s:\n%s", want, env)
		}
	}
	if login := strings.Join(box.environment(), "\n"); !strings.Contains(login, "HOME=/var/lib/forkmesh-job") ||
		strings.Contains(login, "TMPDIR=") {
		t.Fatalf("sign-in environment = %s", login)
	}
}

func TestJobGetsItsOwnTmpAndSharedTmpIsSweptWhenIdle(t *testing.T) {
	if os.Getuid() == 0 {
		t.Skip("needs a non-root uid to stand in for the job user")
	}
	work, shared := t.TempDir(), t.TempDir()
	box := jobSandbox{memoryMax: "700M", cpuQuota: "90%", workRoot: work, uid: os.Getuid(), gid: os.Getgid(),
		unit: func() string { return "fmnext-job-test" }, sharedTmp: []string{shared}, active: &jobActivity{}}
	run := func(script string) string {
		out := &cappedBuffer{limit: 4096}
		stderr, err := box.runner()(context.Background(), "", "", out, "sh", "-c", script, "sh", shared)
		if err != nil {
			t.Fatalf("%v: %s", err, stderr)
		}
		return strings.TrimSpace(out.String())
	}
	got := run(`echo "$TMPDIR $CLAUDE_CODE_TMPDIR"; touch "$TMPDIR/x" "$1/left"; mkdir "$1/cc"; touch "$1/cc/big"`)
	want := filepath.Join(work, jobTmpDir, "fmnext-job-test")
	if got != want+" "+want {
		t.Fatalf("job tmp = %q", got)
	}
	if _, err := os.Lstat(want); !os.IsNotExist(err) {
		t.Fatalf("per-job tmp survived the job: %v", err)
	}
	if entries, _ := os.ReadDir(shared); len(entries) != 0 {
		t.Fatalf("shared tmp not swept: %v", entries)
	}
	box.begin()
	run(`touch "$1/left"`)
	if _, err := os.Lstat(filepath.Join(shared, "left")); err != nil {
		t.Fatalf("swept while another job was running: %v", err)
	}
	box.end()
	if _, err := os.Lstat(filepath.Join(shared, "left")); !os.IsNotExist(err) {
		t.Fatalf("shared tmp not swept once idle: %v", err)
	}
	daemon := jobSandbox{sharedTmp: []string{shared}, active: &jobActivity{}}
	if err := os.WriteFile(filepath.Join(shared, "mine"), nil, 0o600); err != nil {
		t.Fatal(err)
	}
	daemon.sweepIdle()
	if _, err := os.Lstat(filepath.Join(shared, "mine")); err != nil {
		t.Fatalf("a daemon without a job user swept its own tmp: %v", err)
	}
}

func TestMirrorJobSandboxPicksUserScopeOrRunsDirectly(t *testing.T) {
	found := func(name string) (string, error) { return "/usr/bin/" + name, nil }
	missing := func(string) (string, error) { return "", errors.New("no") }
	env := func(name string) string {
		switch name {
		case "XDG_RUNTIME_DIR":
			return "/run/user/1000"
		case "CLAUDE_CONFIG_DIR":
			return "/srv/claude"
		case "CODEX_HOME":
			return "relative"
		}
		return ""
	}
	yes := func(string) bool { return true }
	no := func(string) bool { return false }

	box := detectMirrorJobSandbox("/w", "/home/node", found, env, 1000, yes)
	if !box.sandboxed() || !box.user || box.systemctl == "" || box.uid != 0 {
		t.Fatalf("user manager not used: %+v", box)
	}
	program, args := box.argvUnit("fmnext-job-x", "claude", []string{"-p"})
	got := program + " " + strings.Join(args, " ")
	if got != "/usr/bin/systemd-run --scope --quiet --collect --user --unit=fmnext-job-x -- claude -p" {
		t.Fatalf("mirror scope argv = %q", got)
	}
	if box.credentialDirs[jobCredentialFiles[0]] != "/srv/claude" || box.credentialDirs[jobCredentialFiles[1]] != "" {
		t.Fatalf("credential dirs = %v", box.credentialDirs)
	}
	for name, candidate := range map[string]jobSandbox{
		"no user manager": detectMirrorJobSandbox("/w", "/home/node", found, env, 1000, no),
		"no systemd-run":  detectMirrorJobSandbox("/w", "/home/node", missing, env, 1000, yes),
		"root daemon":     detectMirrorJobSandbox("/w", "/root", found, env, 0, yes),
	} {
		if candidate.sandboxed() {
			t.Fatalf("%s: sandbox chosen: %+v", name, candidate)
		}
		if program, args := candidate.argvUnit("", "claude", []string{"-p"}); program != "claude" ||
			strings.Join(args, " ") != "-p" {
			t.Fatalf("%s: env-scrub fallback argv = %s %q", name, program, args)
		}
	}
}

func TestAgentWorkMustNotShareTheNodeKeyFolder(t *testing.T) {
	state := t.TempDir()
	key := filepath.Join(state, "identity", "ed25519.pem")
	if err := agentWorkOutsideNodeKey(filepath.Join(state, "mirror-gateway", "agent-jobs", "work"), key); err != nil {
		t.Fatalf("bootstrap layout refused: %v", err)
	}
	for _, root := range []string{filepath.Join(state, "identity"), filepath.Join(state, "identity", "agent-jobs", "work"), state} {
		if err := agentWorkOutsideNodeKey(root, key); err == nil {
			t.Fatalf("work root %s next to the node key was accepted", root)
		}
	}
	if err := agentWorkOutsideNodeKey(filepath.Join(state, "work"), ""); err != nil {
		t.Fatalf("no key path: %v", err)
	}
}

func TestMirrorAgentRunsWithAnAllowlistedEnvironmentAndPrivateHome(t *testing.T) {
	bin, out := t.TempDir(), t.TempDir()
	envDump, credDump := filepath.Join(out, "env"), filepath.Join(out, "cred")
	script := `#!/bin/sh
cat >/dev/null
env > "` + envDump + `"
cat "$HOME/.claude/.credentials.json" > "` + credDump + `" 2>/dev/null
echo "agent line" >> README.md
echo '{"type":"result","result":"Added a line.","session_id":"sess-1","is_error":false,"num_turns":1,"usage":{"input_tokens":1,"output_tokens":1}}'
`
	if err := os.WriteFile(filepath.Join(bin, "claude"), []byte(script), 0o700); err != nil {
		t.Fatal(err)
	}
	daemonHome, claudeDir := t.TempDir(), t.TempDir()
	credential := `{"claudeAiOauth":{"accessToken":"node-login","refreshToken":"r","expiresAt":4102444800000}}`
	if err := os.WriteFile(filepath.Join(claudeDir, ".credentials.json"), []byte(credential), 0o600); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(filepath.Join(daemonHome, ".claude"), 0o700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(daemonHome, ".claude", ".credentials.json"), []byte(`{"decoy":true}`), 0o600); err != nil {
		t.Fatal(err)
	}
	t.Setenv("PATH", bin+string(os.PathListSeparator)+os.Getenv("PATH"))
	t.Setenv("HOME", daemonHome)
	t.Setenv("CLAUDE_CONFIG_DIR", claudeDir)
	t.Setenv("ANTHROPIC_API_KEY", "")
	t.Setenv("FORKMESH_MIRROR_NODE_CONFIG", "/nonexistent/forkmesh/config/mirror-node.json")
	t.Setenv("FORKMESH_HOME", "/nonexistent/forkmesh-home")
	t.Setenv("FORKMESH_DATA_DIR", "/nonexistent/forkmesh/data")
	t.Setenv("FORKMESH_CONFIG_DIR", "/nonexistent/forkmesh/config")
	t.Setenv("FORKMESH_IDENTITY_KEY", "/nonexistent/identity/ed25519.pem")
	t.Setenv("VULTR_API_KEY", "vultr-secret")
	t.Setenv("XDG_CONFIG_HOME", "/nonexistent/daemon-xdg")
	t.Setenv("OLLAMA_HOST", "127.0.0.1:11434")

	daemon, relay, _ := newAgentTestDaemon(t, "")
	daemon.config.IdentityKey = filepath.Join(filepath.Dir(filepath.Dir(daemon.config.GatewayConfig)), "identity", "ed25519.pem")
	missing := func(string) (string, error) { return "", errors.New("no") }
	runner := newMirrorAgentJobsWith(daemon, daemonHome, func(name string) (string, error) {
		if name == "systemd-run" || name == "systemctl" {
			return missing(name)
		}
		return exec.LookPath(name)
	}, os.Getenv, 1000, func(string) bool { return false })
	if runner.mirrorBox == nil || runner.mirrorBox.sandboxed() {
		t.Fatalf("mirror box = %+v", runner.mirrorBox)
	}
	daemon.agents = runner
	relay.enqueue(agentTestJob(61, "start"))
	drainAgents(t, daemon)
	reports := relay.snapshot()
	if len(reports) != 2 || reports[1]["status"] != "completed" {
		t.Fatalf("reports = %#v", reports)
	}

	raw, err := os.ReadFile(envDump)
	if err != nil {
		t.Fatal(err)
	}
	env := map[string]string{}
	for _, line := range strings.Split(strings.TrimSpace(string(raw)), "\n") {
		if name, value, ok := strings.Cut(line, "="); ok {
			env[name] = value
		}
	}
	for name := range env {
		if strings.HasPrefix(name, "FORKMESH_") || name == "VULTR_API_KEY" || name == "CLAUDE_CONFIG_DIR" {
			t.Fatalf("%s reached the agent: %v", name, env)
		}
	}
	for name, value := range env {
		if strings.Contains(value, "/nonexistent/") {
			t.Fatalf("%s=%s points at the daemon's folders", name, value)
		}
	}
	home := filepath.Join(runner.dir, "work", jobHomesDir, agentSessionDirectory("session-a"))
	want := map[string]string{
		"HOME": home, "TMPDIR": filepath.Join(home, "tmp"), "CLAUDE_CODE_TMPDIR": filepath.Join(home, "tmp"),
		"XDG_CONFIG_HOME": filepath.Join(home, ".config"), "XDG_CACHE_HOME": filepath.Join(home, ".cache"),
		"XDG_DATA_HOME": filepath.Join(home, ".local", "share"), "XDG_STATE_HOME": filepath.Join(home, ".local", "state"),
		"OLLAMA_HOST": "127.0.0.1:11434", "GIT_TERMINAL_PROMPT": "0",
	}
	for name, value := range want {
		if env[name] != value {
			t.Fatalf("%s = %q, want %q", name, env[name], value)
		}
	}
	if got, _ := os.ReadFile(credDump); string(got) != credential {
		t.Fatalf("agent login = %q, want the CLAUDE_CONFIG_DIR credential", got)
	}
	if _, err := os.Lstat(filepath.Join(home, ".claude", ".credentials.json")); !os.IsNotExist(err) {
		t.Fatalf("credential copy left in the session home: %v", err)
	}
	if _, err := os.Lstat(filepath.Join(home, "tmp")); !os.IsNotExist(err) {
		t.Fatalf("job tmp left behind: %v", err)
	}
}
