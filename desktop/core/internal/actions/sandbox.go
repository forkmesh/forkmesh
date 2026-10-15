package actions

import (
	"bufio"
	"context"
	"fmt"
	"io"
	"io/fs"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"

	"forkmesh.com/desktop-core/internal/safedelete"
)

type Limits struct {
	MaxMemoryBytes      int64 `json:"maxMemoryBytes"`
	MaxScopeMemoryBytes int64 `json:"maxScopeMemoryBytes"`
	MaxFileBytes        int64 `json:"maxFileBytes"`
	MaxWorkspaceBytes   int64 `json:"maxWorkspaceBytes"`
	MaxCacheBytes       int64 `json:"maxCacheBytes"`
	MaxProcesses        int   `json:"maxProcesses"`
	MaxOpenFiles        int   `json:"maxOpenFiles"`
	MaxCPUSeconds       int   `json:"maxCpuSeconds"`
	StepTimeoutMs       int   `json:"stepTimeoutMs"`
	JobTimeoutMs        int   `json:"jobTimeoutMs"`
}

func DefaultLimits() Limits {
	return Limits{
		MaxMemoryBytes:    4 << 30,
		MaxFileBytes:      1 << 30,
		MaxWorkspaceBytes: 2 << 30,
		MaxCacheBytes:     8 << 30,
		MaxOpenFiles:      65536,
		MaxCPUSeconds:     20 * 60,
		StepTimeoutMs:     90 * 60 * 1000,
		JobTimeoutMs:      4 * 60 * 60 * 1000,
	}
}

var cpuCount = runtime.NumCPU

func clampInt(value, low, high int) int {
	if value < low {
		return low
	}
	if value > high {
		return high
	}
	return value
}

func maxInt64(a, b int64) int64 {
	if a > b {
		return a
	}
	return b
}

func CPUQuotaPercent() int {
	cores := cpuCount()
	if cores < 1 {
		cores = 1
	}
	return clampInt((cores-1)*100, 200, 800)
}

var hostMemoryBytes = func() int64 {
	file, err := os.Open("/proc/meminfo")
	if err != nil {
		return 0
	}
	defer file.Close()
	scanner := bufio.NewScanner(file)
	for scanner.Scan() {
		fields := strings.Fields(scanner.Text())
		if len(fields) >= 2 && fields[0] == "MemTotal:" {
			kb, err := strconv.ParseInt(fields[1], 10, 64)
			if err == nil {
				return kb * 1024
			}
		}
	}
	return 0
}

func ScopeMemoryBytes(limits Limits) int64 {
	floor := maxInt64(64<<20, limits.MaxMemoryBytes)
	if limits.MaxScopeMemoryBytes > 0 {
		return maxInt64(floor, limits.MaxScopeMemoryBytes)
	}
	perCPU := int64(3) << 30 / 2
	cpus := CPUQuotaPercent() / 100
	if cpus < 1 {
		cpus = 1
	}
	bytes := int64(1<<30) + int64(cpus)*perCPU
	if host := hostMemoryBytes(); host > 0 && host/2 < bytes {
		bytes = host / 2
	}
	return maxInt64(bytes, floor)
}

func AddressSpaceBytes(limits Limits) int64 {
	return maxInt64(64<<30, ScopeMemoryBytes(limits))
}

func ScopeTasksMax(limits Limits) int {
	if limits.MaxProcesses > 0 {
		if limits.MaxProcesses < 8 {
			return 8
		}
		return limits.MaxProcesses
	}
	cpus := CPUQuotaPercent() / 100
	if cpus < 1 {
		cpus = 1
	}
	threads := cpuCount()
	if threads < 1 {
		threads = 1
	}
	return clampInt(64+cpus*threads, 128, 4096)
}

func humanBytes(bytes int64) string {
	if bytes >= 1<<30 {
		return fmt.Sprintf("%.1f GiB", float64(bytes)/(1024.0*1024.0*1024.0))
	}
	return fmt.Sprintf("%d MiB", bytes/(1024*1024))
}

const (
	minimumRedactableSecretChars = 6
	sandboxReclaimGrace          = 5 * time.Minute
	sandboxProbeCache            = time.Minute
	runPrefix                    = "forkmesh-run-"
	discardPrefix                = "forkmesh-discard-"
	claimFileName                = ".forkmesh-claim"
)

func valueIsWorthRedacting(value string) bool {
	return utf16Len(qtTrim(value)) >= minimumRedactableSecretChars
}

func truthy(value string) bool {
	switch strings.ToLower(qtTrim(value)) {
	case "1", "true", "yes", "on":
		return true
	}
	return false
}

func DangerousEnvironmentName(name string) bool {
	if !validEnvName(name) {
		return true
	}
	upper := strings.ToUpper(name)
	switch upper {
	case "BASH_ENV", "ENV", "HOME", "IFS", "LD_LIBRARY_PATH", "LD_PRELOAD", "PATH", "PYTHONHOME", "PYTHONPATH",
		"SHELLOPTS", "BASHOPTS", "TMPDIR", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_STATE_HOME", "GIT_CONFIG",
		"GIT_CONFIG_GLOBAL", "GIT_CONFIG_SYSTEM", "GIT_CONFIG_COUNT":
		return true
	}
	return strings.HasPrefix(upper, "GIT_CONFIG_KEY_") || strings.HasPrefix(upper, "GIT_CONFIG_VALUE_")
}

func validEnvName(name string) bool {
	if len(name) == 0 || len(name) > 128 {
		return false
	}
	for index, r := range name {
		letter := (r >= 'A' && r <= 'Z') || (r >= 'a' && r <= 'z') || r == '_'
		digit := r >= '0' && r <= '9'
		if index == 0 && !letter {
			return false
		}
		if !letter && !digit {
			return false
		}
	}
	return true
}

type claimSet struct {
	mu    sync.Mutex
	names map[string]bool
	locks map[string]*fileLock
}

var claims = &claimSet{names: map[string]bool{}, locks: map[string]*fileLock{}}

func (c *claimSet) claim(root string) {
	if root == "" {
		return
	}
	c.mu.Lock()
	c.names[filepath.Base(root)] = true
	c.mu.Unlock()
}

func (c *claimSet) lockFile(root string) {
	if root == "" {
		return
	}
	lock, ok := tryLock(filepath.Join(root, claimFileName))
	if !ok {
		return
	}
	c.mu.Lock()
	c.locks[filepath.Base(root)] = lock
	c.mu.Unlock()
}

func (c *claimSet) release(root string) {
	if root == "" {
		return
	}
	name := filepath.Base(root)
	c.mu.Lock()
	delete(c.names, name)
	lock := c.locks[name]
	delete(c.locks, name)
	c.mu.Unlock()
	lock.release()
}

func (c *claimSet) claimed(name string) bool {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.names[name]
}

func (c *claimSet) list() []string {
	c.mu.Lock()
	defer c.mu.Unlock()
	out := make([]string, 0, len(c.names))
	for name := range c.names {
		out = append(out, name)
	}
	sort.Strings(out)
	return out
}

func SandboxClaimedElsewhere(path string) bool {
	return sandboxClaimedElsewhere(path)
}

func ClaimedSandboxNames() []string { return claims.list() }

func sandboxClaimedElsewhere(path string) bool {
	return lockHeldElsewhere(filepath.Join(path, claimFileName))
}

type Sweep struct {
	Removed    int   `json:"removed"`
	FreedBytes int64 `json:"freedBytes"`
	FreeBytes  int64 `json:"freeBytes"`
}

func ReclaimSandboxes(base string, grace time.Duration) Sweep {
	sweep := Sweep{FreeBytes: -1}
	if base == "" {
		return sweep
	}
	if info, err := os.Stat(base); err != nil || !info.IsDir() {
		return sweep
	}
	if grace < 0 {
		grace = sandboxReclaimGrace
	}
	now := time.Now().UnixMilli()
	freeBefore := availableBytes(base)
	entries, _ := os.ReadDir(base)
	for _, prefix := range []string{runPrefix, discardPrefix} {
		for _, entry := range entries {
			name := entry.Name()
			if !strings.HasPrefix(strings.ToLower(name), prefix) {
				continue
			}
			path := filepath.Join(base, name)
			info, err := os.Stat(path)
			if err != nil || !info.IsDir() {
				continue
			}
			if claims.claimed(name) || sandboxClaimedElsewhere(path) {
				continue
			}
			started := int64(0)
			parsed := false
			if dash := strings.LastIndexByte(name, '-'); dash >= 0 {
				if value, err := strconv.ParseInt(name[dash+1:], 10, 64); err == nil {
					started, parsed = value, true
				}
			}
			if !parsed {
				started = info.ModTime().UnixMilli()
			}
			if time.Duration(now-started)*time.Millisecond < grace {
				continue
			}
			if safedelete.RemoveOwnWorktreeUnder(base, path) == nil {
				sweep.Removed++
			}
		}
	}
	sweep.FreeBytes = availableBytes(base)
	if sweep.Removed > 0 && freeBefore >= 0 && sweep.FreeBytes > freeBefore {
		sweep.FreedBytes = sweep.FreeBytes - freeBefore
	}
	return sweep
}

func RemoveUnclaimedSandbox(base, path string) string {
	name := filepath.Base(path)
	parent, err1 := filepath.EvalSymlinks(filepath.Dir(path))
	root, err2 := filepath.EvalSymlinks(base)
	if err1 != nil || err2 != nil || root == "" || parent != root {
		return fmt.Sprintf("%s is not directly inside the Actions sandbox folder", path)
	}
	if info, err := os.Lstat(path); err == nil && info.Mode()&os.ModeSymlink != 0 {
		return fmt.Sprintf("%s is a link, not a sandbox tree", path)
	}
	if !strings.HasPrefix(name, runPrefix) && !strings.HasPrefix(name, discardPrefix) {
		return fmt.Sprintf("%s is not an Actions sandbox tree", path)
	}
	if claims.claimed(name) || sandboxClaimedElsewhere(path) {
		return fmt.Sprintf("a running Action still uses %s", name)
	}
	if err := safedelete.RemoveOwnWorktreeUnder(root, path); err != nil {
		return err.Error()
	}
	return ""
}

type sandboxProbe struct {
	mu        sync.Mutex
	at        time.Time
	available bool
	reason    string
}

var probeCache = &sandboxProbe{}

func SandboxAvailable() (bool, string) {
	probeCache.mu.Lock()
	if !probeCache.at.IsZero() && time.Since(probeCache.at) < sandboxProbeCache {
		available, reason := probeCache.available, probeCache.reason
		probeCache.mu.Unlock()
		return available, reason
	}
	probeCache.mu.Unlock()
	available, reason := ProbeSandbox()
	probeCache.mu.Lock()
	probeCache.at, probeCache.available, probeCache.reason = time.Now(), available, reason
	probeCache.mu.Unlock()
	return available, reason
}

func resetSandboxProbe() {
	probeCache.mu.Lock()
	probeCache.at = time.Time{}
	probeCache.mu.Unlock()
}

func ProbeSandbox() (bool, string) {
	if runtime.GOOS != "linux" {
		return false, "this platform has no configured ForkMesh Actions sandbox"
	}
	if isRoot() {
		return false, "Actions refuse to run while the ForkMesh node is root"
	}
	if _, err := exec.LookPath("bwrap"); err != nil {
		return false, "bubblewrap (bwrap) is required for Actions isolation"
	}
	_, runErr := exec.LookPath("systemd-run")
	systemctl, ctlErr := exec.LookPath("systemctl")
	if runErr != nil || ctlErr != nil {
		return false, "systemd-run and systemctl are required for Actions cgroup limits"
	}
	ctx, cancel := context.WithTimeout(context.Background(), 4*time.Second)
	defer cancel()
	cmd := exec.CommandContext(ctx, systemctl, "--user", "show-environment")
	cmd.Stdout = io.Discard
	cmd.Stderr = io.Discard
	if err := cmd.Run(); err != nil {
		return false, "the unprivileged node user has no active systemd user manager"
	}
	return true, ""
}

func walkFiles(root string, visit func(path string, info fs.FileInfo) bool) {
	_ = filepath.WalkDir(root, func(path string, entry fs.DirEntry, err error) error {
		if err != nil {
			return nil
		}
		if entry.Type()&fs.ModeSymlink != 0 {
			return nil
		}
		if entry.IsDir() || !entry.Type().IsRegular() {
			return nil
		}
		info, err := entry.Info()
		if err != nil {
			return nil
		}
		if !visit(path, info) {
			return filepath.SkipAll
		}
		return nil
	})
}

func WorkspaceWithinQuota(sandboxRoot string, maxWorkspaceBytes int64) (bool, string) {
	var bytes int64
	ok := true
	walkFiles(sandboxRoot, func(path string, info fs.FileInfo) bool {
		size := info.Size()
		if size < 0 || bytes > maxWorkspaceBytes-size {
			ok = false
			return false
		}
		bytes += size
		return true
	})
	if !ok {
		return false, "the Actions writable workspace exceeded its disk quota"
	}
	return true, ""
}

func enforceCacheBudget(cache string, maxBytes int64) {
	if cache == "" || maxBytes <= 0 {
		return
	}
	if info, err := os.Stat(cache); err != nil || !info.IsDir() {
		return
	}
	lock, ok := tryLock(filepath.Join(cache, ".lock"))
	if !ok {
		return
	}
	defer lock.release()
	var bytes int64
	over := false
	walkFiles(cache, func(path string, info fs.FileInfo) bool {
		if size := info.Size(); size > 0 {
			bytes += size
		}
		if bytes > maxBytes {
			over = true
			return false
		}
		return true
	})
	if over {
		_ = safedelete.RemoveTreeUnder(filepath.Dir(cache), cache)
	}
}

func isSymlink(path string) bool {
	info, err := os.Lstat(path)
	return err == nil && info.Mode()&os.ModeSymlink != 0
}

func copySandboxTree(source, destination string, maxBytes int64) (bool, string) {
	sourceInfo, err := os.Stat(source)
	if err != nil {
		return true, ""
	}
	if !sourceInfo.IsDir() || isSymlink(source) {
		return false, "artifact source is not a real directory"
	}
	if info, err := os.Stat(destination); err == nil && (!info.IsDir() || isSymlink(destination)) {
		return false, "artifact destination is unsafe"
	} else if err != nil && isSymlink(destination) {
		return false, "artifact destination is unsafe"
	}
	if os.MkdirAll(destination, 0o777) != nil {
		return false, "artifact destination could not be created"
	}
	var total int64
	failure := ""
	_ = filepath.WalkDir(source, func(path string, entry fs.DirEntry, err error) error {
		if path == source {
			return nil
		}
		if err != nil {
			failure = "artifact output could not be read"
			return filepath.SkipAll
		}
		if entry.Type()&fs.ModeSymlink != 0 {
			failure = "artifact output contains a symlink"
			return filepath.SkipAll
		}
		relative, rerr := filepath.Rel(source, path)
		relative = filepath.ToSlash(relative)
		if rerr != nil || relative == "" || relative == "." || relative == ".." || strings.HasPrefix(relative, "../") || filepath.IsAbs(relative) {
			failure = "artifact output escaped its directory"
			return filepath.SkipAll
		}
		target := filepath.Join(destination, filepath.FromSlash(relative))
		if entry.IsDir() {
			if isSymlink(target) || os.MkdirAll(target, 0o777) != nil {
				failure = "artifact directory is unsafe"
				return filepath.SkipAll
			}
			return nil
		}
		if !entry.Type().IsRegular() {
			failure = "artifact output contains a special file"
			return filepath.SkipAll
		}
		info, ierr := entry.Info()
		if ierr != nil || info.Size() < 0 || info.Size() > maxBytes-total {
			failure = "artifact output exceeds its size limit"
			return filepath.SkipAll
		}
		total += info.Size()
		if reason := copyArtifactFile(path, target); reason != "" {
			failure = reason
			return filepath.SkipAll
		}
		return nil
	})
	if failure != "" {
		return false, failure
	}
	return true, ""
}

func copyArtifactFile(path, target string) string {
	input, err := os.Open(path)
	if err != nil {
		return "artifact output could not be read"
	}
	defer input.Close()
	if info, err := os.Lstat(target); err == nil && (!info.Mode().IsRegular() || info.Mode()&os.ModeSymlink != 0) {
		return "artifact target is unsafe"
	}
	output, err := os.CreateTemp(filepath.Dir(target), filepath.Base(target)+".*")
	if err != nil {
		return "artifact target could not be written"
	}
	tempName := output.Name()
	buffer := make([]byte, 256*1024)
	for {
		n, rerr := input.Read(buffer)
		if n > 0 {
			if written, werr := output.Write(buffer[:n]); werr != nil || written != n {
				_ = output.Close()
				_ = os.Remove(tempName)
				return "artifact write failed"
			}
		}
		if rerr == io.EOF {
			break
		}
		if rerr != nil {
			_ = output.Close()
			_ = os.Remove(tempName)
			return "artifact read failed"
		}
	}
	_ = output.Chmod(0o600)
	if output.Sync() != nil || output.Close() != nil || os.Rename(tempName, target) != nil {
		_ = os.Remove(tempName)
		return "artifact commit failed"
	}
	return ""
}

func dirExists(path string) bool {
	info, err := os.Stat(path)
	return err == nil && info.IsDir()
}

func pathExists(path string) bool {
	_, err := os.Stat(path)
	return err == nil
}

func pythonSiteDirs(home string) []string {
	lib := home + "/.local/lib"
	entries, err := os.ReadDir(lib)
	if err != nil {
		return nil
	}
	var names []string
	for _, entry := range entries {
		name := entry.Name()
		if !strings.HasPrefix(strings.ToLower(name), "python") {
			continue
		}
		if !dirExists(filepath.Join(lib, name)) {
			continue
		}
		names = append(names, name)
	}
	sort.Slice(names, func(i, j int) bool { return qtKeyLess(names[i], names[j]) })
	return names
}

type sandboxPaths struct {
	worktree       string
	workspace      string
	home           string
	tmp            string
	releaseStaging string
	buildCache     string
	toolchainBin   string
	hostHome       string
	network        bool
}

func SandboxArguments(paths sandboxPaths, shell string) []string {
	args := []string{"--die-with-parent", "--new-session", "--unshare-user", "--unshare-pid", "--unshare-ipc",
		"--unshare-uts", "--unshare-cgroup-try"}
	if !paths.network {
		args = append(args, "--unshare-net")
	}
	args = append(args, "--uid", "65534", "--gid", "65534", "--cap-drop", "ALL", "--hostname", "forkmesh-action",
		"--proc", "/proc", "--dev", "/dev", "--dir", "/run", "--dir", "/var", "--tmpfs", "/var/tmp", "--dir", "/etc")
	bind := func(path string) {
		if pathExists(path) {
			args = append(args, "--ro-bind", path, path)
		}
	}
	for _, path := range []string{"/usr", "/bin", "/lib", "/lib64", "/etc/alternatives", "/etc/passwd", "/etc/group",
		"/etc/nsswitch.conf", "/etc/ssl", "/etc/ca-certificates"} {
		bind(path)
	}
	if paths.network {
		bind("/etc/resolv.conf")
		bind("/etc/hosts")
	}
	args = append(args, "--ro-bind", paths.worktree, "/source", "--bind", paths.workspace, "/workspace",
		"--dir", "/home", "--bind", paths.home, "/home/forkmesh", "--bind", paths.tmp, "/tmp",
		"--bind", paths.releaseStaging, "/release-cas")
	if paths.buildCache != "" {
		args = append(args, "--bind", paths.buildCache, "/cache")
	}
	if paths.toolchainBin != "" && dirExists(paths.toolchainBin) {
		args = append(args, "--ro-bind", paths.toolchainBin, "/home/forkmesh/.local/bin")
	}
	for _, python := range pythonSiteDirs(paths.hostHome) {
		site := paths.hostHome + "/.local/lib/" + python + "/site-packages"
		if !dirExists(site) {
			continue
		}
		args = append(args, "--ro-bind", site, "/home/forkmesh/.local/lib/"+python+"/site-packages")
	}
	return append(args, "--unsetenv", "XDG_RUNTIME_DIR", "--unsetenv", "DBUS_SESSION_BUS_ADDRESS",
		"--chdir", "/workspace", shell, "-se")
}

func SystemdRunArguments(unit string, limits Limits, slice, bwrap string, sandbox []string) []string {
	runtimeSeconds := (limits.StepTimeoutMs + 999) / 1000
	if runtimeSeconds < 1 {
		runtimeSeconds = 1
	}
	args := []string{"--user", "--scope", "--quiet", "--unit=" + unit,
		"--property=TasksMax=" + strconv.Itoa(ScopeTasksMax(limits)),
		"--property=MemoryMax=" + strconv.FormatInt(ScopeMemoryBytes(limits), 10),
		"--property=MemorySwapMax=0",
		"--property=CPUQuota=" + strconv.Itoa(CPUQuotaPercent()) + "%",
		"--property=CPUWeight=20",
		"--property=RuntimeMaxSec=" + strconv.Itoa(runtimeSeconds+60)}
	if slice != "" {
		args = append(args, "--slice="+slice)
	}
	args = append(args, "--", bwrap)
	return append(args, sandbox...)
}
