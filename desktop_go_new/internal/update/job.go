package update

import (
	"context"
	"fmt"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
	"time"

	"forkmesh.com/desktop-core/internal/safedelete"
)

type Request struct {
	Kind       string   `json:"kind"`
	ClientDir  string   `json:"clientDir"`
	BuildType  string   `json:"buildType"`
	AppPath    string   `json:"appPath"`
	Args       []string `json:"args"`
	Cwd        string   `json:"cwd"`
	Unit       string   `json:"unit"`
	SelfPath   string   `json:"selfPath"`
	InstallURL string   `json:"installUrl"`
	RepoDir    string   `json:"repoDir"`
	Label      string   `json:"label"`
	AsUser     string   `json:"asUser"`
	Clean      bool     `json:"clean"`
	CatalogURL string   `json:"catalogUrl"`
	RepoURL    string   `json:"repoUrl"`
}

type Env struct {
	LookPath   func(string) (string, error)
	FreeBytes  func(string) int64
	Memory     func() (int64, int64)
	CPUs       func() int
	Darwin     bool
	BrewPrefix func(string) string
	StartCheck time.Duration
	Now        func() time.Time
	MaxPasses  int
	KillGrace  time.Duration
	HTTP       *http.Client
	Lookup     time.Duration
	AutoFirst  func() time.Duration
	AutoEvery  func() time.Duration
	AutoFetch  time.Duration
}

func defaultEnv() *Env {
	return &Env{
		LookPath:   exec.LookPath,
		FreeBytes:  volumeFreeBytes,
		Memory:     memoryBytes,
		CPUs:       runtime.NumCPU,
		Darwin:     runtime.GOOS == "darwin",
		BrewPrefix: brewPrefix,
		StartCheck: 15 * time.Second,
		Now:        time.Now,
		MaxPasses:  3,
		KillGrace:  1500 * time.Millisecond,
		HTTP:       &http.Client{},
		Lookup:     30 * time.Second,
		AutoFirst:  autoFirstDelay,
		AutoEvery:  autoInterval,
		AutoFetch:  45 * time.Second,
	}
}

func brewPrefix(formula string) string {
	brew, err := exec.LookPath("brew")
	if err != nil {
		return ""
	}
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	out, err := exec.CommandContext(ctx, brew, "--prefix", formula).Output()
	if err != nil {
		return ""
	}
	return strings.TrimSpace(string(out))
}

type sink interface {
	appendLog(text string)
	status(text string, isError bool)
	phase(name string, pass int)
	percent(value int)
	restartLog(text string)
	systemLog(text string)
}

type outcome int

const (
	outcomeFailed outcome = iota
	outcomeCancelled
	outcomeInstalled
	outcomeFallback
)

type job struct {
	req  Request
	env  *Env
	out  sink
	ctx  context.Context
	stop context.CancelFunc

	mu       sync.Mutex
	current  *exec.Cmd
	built    string
	flash    string
	prebuilt *prebuiltPlan
}

func newJob(req Request, env *Env, out sink) *job {
	ctx, stop := context.WithCancel(context.Background())
	return &job{req: req, env: env, out: out, ctx: ctx, stop: stop}
}

func (j *job) cancelled() bool { return j.ctx.Err() != nil }

func (j *job) cancel() {
	j.stop()
	j.mu.Lock()
	cmd := j.current
	j.mu.Unlock()
	if cmd == nil {
		return
	}
	signalGroup(cmd, false)
	grace := j.env.KillGrace
	go func() {
		time.Sleep(grace)
		signalGroup(cmd, true)
	}()
}

type stepResult struct {
	ok      bool
	started bool
	output  string
}

type stepWriter struct {
	out    sink
	mu     sync.Mutex
	buffer []byte
	job    *job
}

func (w *stepWriter) Write(p []byte) (int, error) {
	if w.job.cancelled() {
		return len(p), nil
	}
	w.mu.Lock()
	w.buffer = append(w.buffer, p...)
	if len(w.buffer) > 400000 {
		w.buffer = append([]byte(nil), w.buffer[len(w.buffer)-200000:]...)
	}
	w.mu.Unlock()
	w.out.appendLog(string(p))
	return len(p), nil
}

func (w *stepWriter) text() string {
	w.mu.Lock()
	defer w.mu.Unlock()
	text := string(w.buffer)
	if len(text) > 200000 {
		text = text[len(text)-200000:]
	}
	return text
}

func (j *job) step(program string, args []string, dir string, extraEnv []string) stepResult {
	if j.cancelled() {
		return stepResult{}
	}
	commandLine := strings.Join(append(append(append([]string{}, extraEnv...), program), args...), " ")
	j.out.restartLog("run: " + commandLine)
	j.out.appendLog(fmt.Sprintf("\n$ %s\n  (in %s)\n", commandLine, dir))
	started := j.env.Now()
	cmd := exec.Command(program, args...)
	cmd.Dir = dir
	if len(extraEnv) > 0 {
		cmd.Env = append(os.Environ(), extraEnv...)
	}
	prepareStep(cmd)
	writer := &stepWriter{out: j.out, job: j}
	cmd.Stdout = writer
	cmd.Stderr = writer
	cmd.WaitDelay = 5 * time.Second
	if err := cmd.Start(); err != nil {
		elapsed := j.env.Now().Sub(started).Milliseconds()
		if j.cancelled() {
			return stepResult{}
		}
		j.out.restartLog(fmt.Sprintf("failed to start after %dms: %s", elapsed, commandLine))
		j.out.status("Update failed: could not run "+program, true)
		return stepResult{}
	}
	j.mu.Lock()
	j.current = cmd
	j.mu.Unlock()
	if j.cancelled() {
		signalGroup(cmd, false)
	}
	err := cmd.Wait()
	j.mu.Lock()
	j.current = nil
	j.mu.Unlock()
	if j.cancelled() {
		return stepResult{}
	}
	code := 0
	if err != nil {
		code = -1
		if exitErr, ok := err.(*exec.ExitError); ok && exitErr.ExitCode() >= 0 {
			code = exitErr.ExitCode()
		}
		if code == 0 {
			code = -1
		}
	}
	elapsed := j.env.Now().Sub(started).Milliseconds()
	j.out.restartLog(fmt.Sprintf("done in %dms (exit %d): %s", elapsed, code, commandLine))
	j.out.appendLog(fmt.Sprintf("— finished in %dms (exit %d)\n", elapsed, code))
	return stepResult{ok: code == 0, started: true, output: writer.text()}
}

func (j *job) failWith(result stepResult) outcome {
	if j.cancelled() {
		return outcomeCancelled
	}
	if result.started {
		j.out.status("Update failed: "+updateFailureSummary(result.output), true)
	}
	return outcomeFailed
}

func (j *job) userStep(program string, args []string, dir string, extraEnv []string) stepResult {
	if j.req.AsUser == "" {
		return j.step(program, args, dir, extraEnv)
	}
	wrapped := []string{"-u", j.req.AsUser, "-H"}
	if len(extraEnv) > 0 {
		wrapped = append(append(wrapped, "env"), extraEnv...)
	}
	wrapped = append(append(wrapped, program), args...)
	return j.step("sudo", wrapped, dir, nil)
}

func (j *job) quiet(dir string, args ...string) (string, bool) {
	ctx, cancel := context.WithTimeout(j.ctx, 3*time.Second)
	defer cancel()
	cmd := exec.CommandContext(ctx, "git", args...)
	cmd.Dir = dir
	out, err := cmd.Output()
	if err != nil {
		return "", false
	}
	return strings.TrimSpace(string(out)), true
}

func (j *job) run() outcome {
	if j.prebuilt != nil {
		return j.runPrebuilt()
	}
	if j.req.Kind == "update" {
		if result := j.pull(); result != outcomeInstalled {
			return result
		}
	}
	if j.req.Kind == "quick" {
		if result := j.quickPull(); result != outcomeInstalled {
			return result
		}
	}
	if _, err := os.Stat(filepath.Join(j.req.ClientDir, "CMakeLists.txt")); err != nil {
		j.out.status("No local source checkout to rebuild from. Use Quick update on the start screen instead.", true)
		return outcomeFailed
	}
	if j.req.Clean {
		if result := j.clean(); result != outcomeInstalled {
			return result
		}
	}
	if result := j.build(); result != outcomeInstalled {
		return result
	}
	return j.install()
}

func (j *job) pull() outcome {
	repoDir := j.req.RepoDir
	if repoDir == "" {
		repoDir = filepath.Dir(filepath.Clean(j.req.ClientDir))
	}
	url := j.req.InstallURL
	if strings.TrimSpace(url) == "" && strings.TrimSpace(j.req.CatalogURL) != "" {
		url = j.findMirror()
		if j.cancelled() {
			return outcomeCancelled
		}
		j.req.InstallURL = url
	}
	if strings.TrimSpace(url) == "" {
		j.out.status("No online ForkMesh mirror is available right now. Try again shortly.", true)
		j.flash = "No online ForkMesh mirror is available right now."
		return outcomeFailed
	}
	stopUntouched := func(reason string) outcome {
		j.out.restartLog(fmt.Sprintf("update stopped; %s left untouched: %s", repoDir, reason))
		j.out.status(reason+" Nothing in "+repoDir+" was changed or removed.", true)
		j.flash = reason
		return outcomeFailed
	}
	if _, err := os.Stat(filepath.Join(repoDir, ".git")); err == nil {
		j.out.phase("pulling the source", 0)
		j.out.percent(20)
		j.out.status("Pulling a fresh copy from "+url+"...", false)
		if result := j.userStep("git", []string{"-C", repoDir, "remote", "set-url", "origin", url}, repoDir, nil); !result.ok {
			if !result.started || j.cancelled() {
				return j.failWith(result)
			}
			return stopUntouched("Could not point the checkout at the install mirror.")
		}
		if result := j.userStep("git", []string{"-C", repoDir, "pull", "--ff-only"}, repoDir, nil); !result.ok {
			if !result.started || j.cancelled() {
				return j.failWith(result)
			}
			return stopUntouched("Could not fast-forward from " + url + " (the mirror may be busy, or this checkout has its own commits).")
		}
		return outcomeInstalled
	}
	if entries, err := os.ReadDir(repoDir); err == nil && len(entries) > 0 {
		return stopUntouched("The source folder is not empty but has no git checkout.")
	}
	j.out.phase("downloading the source", 0)
	j.out.percent(20)
	j.out.status("Downloading a fresh copy from "+url+"...", false)
	parent := filepath.Dir(repoDir)
	if j.req.AsUser != "" {
		if result := j.userStep("mkdir", []string{"-p", parent}, os.TempDir(), nil); !result.ok {
			return j.failWith(result)
		}
	} else if err := os.MkdirAll(parent, 0o755); err != nil {
		j.out.status("Update failed: could not create "+parent, true)
		return outcomeFailed
	}
	if result := j.userStep("git", []string{"clone", "--depth", "1", url, repoDir}, parent, nil); !result.ok {
		return j.failWith(result)
	}
	return outcomeInstalled
}

func (j *job) gitHead(dir string) string {
	head, _ := j.quiet(dir, "rev-parse", "--short", "HEAD")
	return head
}

func (j *job) build() outcome {
	clientDir := j.req.ClientDir
	buildDir := clientDir + "/build"
	buildType := j.req.BuildType
	if buildType == "" {
		buildType = "Release"
	}
	free := freeBytesForPath(buildDir, j.env.FreeBytes)
	required := rebuildFreeBytesRequired(buildDir)
	if free >= 0 && free < required {
		j.out.status(fmt.Sprintf("Update cancelled: only %s free on the volume holding %s, and the rebuild needs at least %s. Free some space and try again — nothing was changed.",
			formatByteSize(free), buildDir, formatByteSize(required)), true)
		return outcomeFailed
	}
	scratch := buildScratchDir(buildDir)
	j.out.phase("configuring", 0)
	j.out.percent(35)
	j.out.status("Configuring...", false)
	generator := ""
	if _, err := j.env.LookPath("ninja"); err == nil {
		generator = "Ninja"
	}
	configured := cmakeCacheMatches(clientDir, buildDir, buildType, generator)
	stale := cmakeGeneratorEntriesToDrop(buildDir, generator)
	for _, entry := range append(append([]string{}, stale...), scratch) {
		if reason := safedelete.Refusal(buildDir, entry); reason != "" {
			j.out.status(fmt.Sprintf("Update cancelled: refusing to delete %s (%s). Nothing was changed.", entry, reason), true)
			return outcomeFailed
		}
	}
	j.out.appendLog(fmt.Sprintf("\n$ clear %s\n  (in %s)\n", scratch, clientDir))
	if err := safedelete.RemoveTreeUnder(buildDir, scratch); err != nil {
		j.out.status("Update failed: could not clear "+scratch+": "+err.Error(), true)
		return outcomeFailed
	}
	if j.req.AsUser != "" {
		if result := j.userStep("mkdir", []string{"-p", scratch}, clientDir, nil); !result.ok {
			return j.failWith(result)
		}
	} else if err := os.MkdirAll(scratch, 0o755); err != nil {
		j.out.status("Update failed: could not create "+scratch, true)
		return outcomeFailed
	}
	if configured {
		j.out.restartLog("build tree already configured; skipping the configure step")
	} else {
		if len(stale) > 0 {
			j.out.restartLog("switching the build tree's generator; dropping " + strings.Join(stale, ", "))
			for _, entry := range stale {
				if err := safedelete.RemoveTreeUnder(buildDir, entry); err != nil {
					j.out.status("Update failed: could not drop "+entry+": "+err.Error(), true)
					return outcomeFailed
				}
			}
		}
		var prefixes []string
		if j.env.Darwin && j.env.BrewPrefix != nil {
			if qt := j.env.BrewPrefix("qt"); qt != "" {
				prefixes = append(prefixes, "-DCMAKE_PREFIX_PATH="+qt)
			}
			if ssl := j.env.BrewPrefix("openssl@3"); ssl != "" {
				prefixes = append(prefixes, "-DOPENSSL_ROOT_DIR="+ssl)
			}
		}
		if result := j.userStep("cmake", cmakeConfigureArgs(clientDir, buildDir, buildType, generator, prefixes), clientDir, nil); !result.ok {
			return j.failWith(result)
		}
	}
	maxPasses := j.env.MaxPasses
	if maxPasses < 1 {
		maxPasses = 3
	}
	for pass := 1; ; pass++ {
		if pass == 1 {
			j.out.phase("compiling", pass)
		} else {
			j.out.phase(fmt.Sprintf("compiling again (pass %d)", pass), pass)
		}
		j.out.percent(60)
		if pass == 1 {
			j.out.status("Rebuilding...", false)
		} else {
			j.out.status(fmt.Sprintf("Rebuilding again (pass %d of %d): the sources changed during the build...", pass, maxPasses), false)
		}
		total, available := j.env.Memory()
		jobs := buildJobsForMemory(total, available, j.env.CPUs())
		headBefore := j.gitHead(clientDir)
		j.out.restartLog(fmt.Sprintf("build pass %d: %d parallel jobs, HEAD %s", pass, jobs, headBefore))
		started := j.env.Now()
		result := j.userStep("cmake", []string{"--build", buildDir, "-j", fmt.Sprint(jobs)}, buildDir, []string{"TMPDIR=" + scratch})
		if j.cancelled() {
			return outcomeCancelled
		}
		if !result.started {
			return outcomeFailed
		}
		changed := sourceChangedSince(clientDir, started)
		relative := changed
		if changed != "" {
			if rel, err := filepath.Rel(clientDir, changed); err == nil {
				relative = rel
			}
			headAfter := j.gitHead(clientDir)
			note := fmt.Sprintf("the sources changed while build pass %d ran (%s; HEAD %s -> %s)", pass, relative, headBefore, headAfter)
			if pass < maxPasses {
				j.out.restartLog(note + "; building again")
				j.out.appendLog(fmt.Sprintf("\n!! %s; building again so the binary comes from one consistent tree.\n", note))
				continue
			}
			if result.ok {
				j.out.restartLog(note + "; out of passes, installing this build as it is")
			} else {
				j.out.restartLog(note + "; out of passes")
			}
		}
		if !result.ok {
			moved := ""
			if changed != "" {
				moved = fmt.Sprintf("the sources changed during all %d build passes (last: %s), so this is most likely a merge landing mid-build — rebuild again. ", maxPasses, relative)
			}
			j.out.status("Update failed: "+moved+updateFailureSummary(result.output), true)
			return outcomeFailed
		}
		break
	}
	j.buildCompanion(clientDir, buildDir)
	if j.cancelled() {
		return outcomeCancelled
	}
	j.built = builtExecutablePath(buildDir, j.env.Darwin)
	return outcomeInstalled
}

func (j *job) buildCompanion(clientDir, buildDir string) {
	mirrorSource := filepath.Join(clientDir, "..", "server")
	goTool, err := j.env.LookPath("go")
	if _, statErr := os.Stat(filepath.Join(mirrorSource, "go.mod")); err != nil || statErr != nil {
		j.out.appendLog("\nSkipping the Go mirror-node companion (Go toolchain or server sources not available); host deploys need forkmesh-mirror-node installed beside the client.\n")
		return
	}
	j.out.phase("building the mirror-node companion", 0)
	j.out.status("Building the Go mirror-node companion...", false)
	result := j.userStep(goTool, []string{"build", "-trimpath", "-ldflags=-s -w", "-o",
		filepath.Join(buildDir, "forkmesh-mirror-node"), "./cmd/forkmesh-mirror-node"}, mirrorSource, nil)
	if !result.ok && !j.cancelled() {
		j.out.appendLog("\nThe Go mirror-node companion failed to build; continuing the client update without refreshing it.\n")
	}
}

func (j *job) startCheck(binary string) bool {
	timeout := j.env.StartCheck
	if timeout <= 0 {
		timeout = 15 * time.Second
	}
	ctx, cancel := context.WithTimeout(j.ctx, timeout)
	defer cancel()
	cmd := exec.CommandContext(ctx, binary, "--version")
	if j.req.AsUser != "" {
		cmd = exec.CommandContext(ctx, "sudo", "-u", j.req.AsUser, "-H", binary, "--version")
	}
	cmd.WaitDelay = 2 * time.Second
	return cmd.Run() == nil
}

func (j *job) install() outcome {
	built := j.built
	appPath := j.req.AppPath
	j.out.phase("installing", 0)
	j.out.percent(80)
	if j.req.AsUser != "" {
		return j.installAsUser()
	}
	if !j.startCheck(built) {
		if j.cancelled() {
			return outcomeCancelled
		}
		j.out.status("Update failed: the new build did not pass its start check; keeping the current version.", true)
		return outcomeFailed
	}
	if appPath == "" {
		appPath = built
		j.req.AppPath = built
	}
	if canonical(built) != canonical(appPath) {
		bak := appPath + ".bak-update"
		_ = os.Remove(bak)
		_, statErr := os.Lstat(appPath)
		hadInstalledCopy := statErr == nil
		dir := filepath.Dir(appPath)
		if err := os.MkdirAll(dir, 0o755); err != nil {
			j.out.status("Update failed: could not create "+dir, true)
			return outcomeFailed
		}
		if hadInstalledCopy && os.Rename(appPath, bak) != nil {
			j.out.status("Update failed: could not move the current binary aside at "+appPath, true)
			return outcomeFailed
		}
		if err := copyFile(built, appPath); err != nil {
			if hadInstalledCopy {
				_ = os.Rename(bak, appPath)
			}
			j.out.status("Update failed: could not replace "+appPath, true)
			return outcomeFailed
		}
	}
	if companion := builtMirrorNodeCompanionPath(built, j.env.Darwin); companion != "" {
		if installMirrorNodeCompanionFile(companion, filepath.Dir(appPath)) {
			j.out.restartLog("installed the Go mirror-node companion beside " + appPath)
		} else {
			j.out.restartLog("could not install the Go mirror-node companion beside " + appPath)
		}
	}
	if j.cancelled() {
		return outcomeCancelled
	}
	j.out.percent(100)
	j.out.status("Relaunching...", false)
	return outcomeInstalled
}
