package hosts

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"sync"
	"time"

	"forkmesh.com/desktop-core/internal/daemon"
)

const (
	pendingReleasePublicationsSetting = "releases/pendingMirrorPublications"
	mirrorSyncIntervalSetting         = "repos/mirrorSyncIntervalMinutes"
	mirrorReposFile                   = "mirror-repos.json"
	managedRepoPath                   = "/var/lib/forkmesh/repositories/forkmesh-forkmesh.git"
	managedReceivePack                = "--receive-pack=runuser -u forkmesh-node -- git-receive-pack"
	managedWakeCommand                = "/usr/bin/curl -fsS --connect-timeout 2 --max-time 5 -X POST http://127.0.0.1:8791/v1/control/sync?local=1"
	defaultGitSSHCommand              = "ssh -oBatchMode=yes -oConnectTimeout=15 -oServerAliveInterval=15 -oServerAliveCountMax=4"
	managedPushCap                    = 75 * time.Second
	managedWakeCap                    = 45 * time.Second
	artifactTransferCap               = 5 * time.Minute
	releaseGateRetry                  = 250 * time.Millisecond
	mirrorRemotesTimeout              = 30 * time.Second
	maxMirrorRepos                    = 2000
)

var lineBreaks = regexp.MustCompile(`[\r\n]+`)

type mirrorRepo struct {
	Owner       string `json:"owner"`
	Name        string `json:"name"`
	LocalPath   string `json:"localPath"`
	MirrorPath  string `json:"mirrorPath"`
	PreviewOnly bool   `json:"previewOnly"`
	Private     bool   `json:"private"`
}

func (r mirrorRepo) key() string      { return r.Owner + "/" + r.Name }
func (r mirrorRepo) lowerKey() string { return strings.ToLower(r.key()) }
func (r mirrorRepo) managedProduct() bool {
	return strings.EqualFold(strings.TrimSpace(r.Owner), "forkmesh") && strings.EqualFold(strings.TrimSpace(r.Name), "forkmesh")
}

type releaseBlob struct {
	hash string
	path string
	size int64
}

type mirrorRetry struct {
	timer *time.Timer
	due   int64
}

type managedTarget struct {
	node      string
	host      string
	url       string
	env       []string
	wake      sshCommand
	inventory sshCommand
	artifacts []sshCommand
}

type mirrorEngine struct {
	m *Module

	mu           sync.Mutex
	repos        map[string]mirrorRepo
	reposLoaded  bool
	pushing      map[string]bool
	pending      map[string]bool
	retries      map[string]*mirrorRetry
	gate         refPushGate
	reconciling  map[string]bool
	health       gatewayHealth
	healthLoaded bool
	heldBack     map[string]string
}

func newMirrorEngine(m *Module) *mirrorEngine {
	return &mirrorEngine{m: m, repos: map[string]mirrorRepo{}, pushing: map[string]bool{}, pending: map[string]bool{},
		retries: map[string]*mirrorRetry{}, reconciling: map[string]bool{}, heldBack: map[string]string{}}
}

func (m *Module) mirror() *mirrorEngine {
	m.mu.Lock()
	defer m.mu.Unlock()
	if m.mirrors == nil {
		m.mirrors = newMirrorEngine(m)
	}
	return m.mirrors
}

func plural(n int) string {
	if n == 1 {
		return ""
	}
	return "s"
}

func (e *mirrorEngine) log(text string) { e.m.logUI("%s", text) }

func (e *mirrorEngine) runCtx() context.Context {
	if e.m.ctx != nil {
		return e.m.ctx
	}
	return context.Background()
}

func (e *mirrorEngine) reposPath() string {
	if e.m.core == nil || e.m.core.Layout.State == "" {
		return ""
	}
	return filepath.Join(e.m.core.Layout.CoreState(), mirrorReposFile)
}

func (e *mirrorEngine) loadReposLocked() {
	if e.reposLoaded {
		return
	}
	e.reposLoaded = true
	path := e.reposPath()
	if path == "" {
		return
	}
	raw, err := os.ReadFile(path)
	if err != nil {
		return
	}
	var list []mirrorRepo
	if json.Unmarshal(raw, &list) != nil {
		return
	}
	for _, repo := range list {
		if repo.Owner != "" && repo.Name != "" {
			e.repos[repo.lowerKey()] = repo
		}
	}
}

func (e *mirrorEngine) saveReposLocked() {
	path := e.reposPath()
	if path == "" {
		return
	}
	list := make([]mirrorRepo, 0, len(e.repos))
	for _, repo := range e.repos {
		list = append(list, repo)
	}
	sort.Slice(list, func(i, j int) bool { return list[i].lowerKey() < list[j].lowerKey() })
	raw, err := json.Marshal(list)
	if err != nil {
		return
	}
	if err := os.MkdirAll(filepath.Dir(path), 0o700); err != nil {
		return
	}
	temp := path + ".tmp"
	if err := os.WriteFile(temp, raw, 0o600); err != nil {
		return
	}
	if os.Rename(temp, path) != nil {
		_ = os.Remove(temp)
	}
}

func cleanMirrorRepo(repo mirrorRepo) (mirrorRepo, bool) {
	repo.Owner = strings.TrimSpace(repo.Owner)
	repo.Name = strings.TrimSpace(repo.Name)
	return repo, repo.Owner != "" && repo.Name != "" && !strings.Contains(repo.Owner, "/") && !strings.Contains(repo.Name, "/")
}

func (e *mirrorEngine) setRepos(list []mirrorRepo) int {
	next := map[string]mirrorRepo{}
	for _, raw := range list {
		if len(next) >= maxMirrorRepos {
			break
		}
		if repo, ok := cleanMirrorRepo(raw); ok {
			next[repo.lowerKey()] = repo
		}
	}
	e.mu.Lock()
	defer e.mu.Unlock()
	e.reposLoaded = true
	e.repos = next
	e.saveReposLocked()
	return len(next)
}

func (e *mirrorEngine) noteRepo(raw mirrorRepo) (mirrorRepo, bool) {
	repo, ok := cleanMirrorRepo(raw)
	if !ok {
		return repo, false
	}
	e.mu.Lock()
	defer e.mu.Unlock()
	e.loadReposLocked()
	if current, known := e.repos[repo.lowerKey()]; !known || current != repo {
		if known || len(e.repos) < maxMirrorRepos {
			e.repos[repo.lowerKey()] = repo
			e.saveReposLocked()
		}
	}
	return repo, true
}

func (e *mirrorEngine) repo(key string) (mirrorRepo, bool) {
	e.mu.Lock()
	defer e.mu.Unlock()
	e.loadReposLocked()
	repo, ok := e.repos[strings.ToLower(strings.TrimSpace(key))]
	return repo, ok
}

func (e *mirrorEngine) repoList() []mirrorRepo {
	e.mu.Lock()
	defer e.mu.Unlock()
	e.loadReposLocked()
	out := make([]mirrorRepo, 0, len(e.repos))
	for _, repo := range e.repos {
		out = append(out, repo)
	}
	sort.Slice(out, func(i, j int) bool { return out[i].lowerKey() < out[j].lowerKey() })
	return out
}

func (m *Module) settingStrings(key string) []string {
	if write, ok := m.pendingSettingValue(key); ok {
		if write.Remove {
			return nil
		}
		if list, ok := write.Value.([]string); ok {
			return append([]string(nil), list...)
		}
	}
	return m.settings().Strings(key)
}

func (e *mirrorEngine) loadHealthLocked() {
	if e.healthLoaded {
		return
	}
	e.healthLoaded = true
	e.health.restoreState(e.m.settingStrings(gatewayHealthSetting), e.m.nowMs())
}

func (e *mirrorEngine) healthWriteLocked() []daemon.SettingWrite {
	state := e.health.persistedState()
	if len(state) == 0 {
		return []daemon.SettingWrite{{Key: gatewayHealthSetting, Remove: true}}
	}
	return []daemon.SettingWrite{{Key: gatewayHealthSetting, Type: "stringList", Value: state}}
}

func (e *mirrorEngine) gatewayRecords() []string {
	e.mu.Lock()
	defer e.mu.Unlock()
	e.loadHealthLocked()
	return e.health.persistedState()
}

func (e *mirrorEngine) counts() (int, int) {
	e.mu.Lock()
	defer e.mu.Unlock()
	return len(e.pushing), len(e.pending)
}

func releaseRolloutToken(owner, name, tag string) string {
	var out bytes.Buffer
	out.WriteByte('[')
	qtJSONString(&out, strings.ToLower(strings.TrimSpace(owner)))
	out.WriteByte(',')
	qtJSONString(&out, strings.ToLower(strings.TrimSpace(name)))
	out.WriteByte(',')
	qtJSONString(&out, strings.TrimSpace(tag))
	out.WriteByte(']')
	return out.String()
}

func (e *mirrorEngine) releasePublicationPending(owner, name, tag string) bool {
	token := releaseRolloutToken(owner, name, tag)
	for _, entry := range e.m.settings().Strings(pendingReleasePublicationsSetting) {
		if entry == token {
			return true
		}
	}
	return false
}

func (e *mirrorEngine) releasePublicationPendingForRepository(owner, name string) bool {
	wantedOwner := strings.ToLower(strings.TrimSpace(owner))
	wantedName := strings.ToLower(strings.TrimSpace(name))
	if wantedOwner == "" || wantedName == "" {
		return false
	}
	for _, token := range e.m.settings().Strings(pendingReleasePublicationsSetting) {
		var parts []any
		if json.Unmarshal([]byte(token), &parts) != nil || len(parts) < 3 {
			continue
		}
		if jsonString(parts[0]) == wantedOwner && jsonString(parts[1]) == wantedName && jsonString(parts[2]) != "" {
			return true
		}
	}
	return false
}

func (e *mirrorEngine) syncIntervalMs() int64 {
	minutes := e.m.settings().Int(mirrorSyncIntervalSetting, 1)
	minutes = min(max(minutes, 1), 24*60)
	return minutes * 60 * 1000
}

func dirExists(path string) bool {
	info, err := os.Stat(path)
	return err == nil && info.IsDir()
}

func gitRemotes(ctx context.Context, dir string) (string, bool) {
	result := runMirrorProc(ctx, mirrorProc{program: "git", args: []string{"remote", "-v"}, dir: dir, env: os.Environ(),
		timeout: mirrorRemotesTimeout, stdout: true})
	return result.stdout, result.ok()
}

type sshRemote struct {
	name string
	url  string
	host string
}

func (e *mirrorEngine) pushAll(repo mirrorRepo, wakeManagedIfUnchanged, pruneRefs bool) {
	e.pushRemotes(repo, false, "", pruneRefs)
	e.pushManaged(repo, wakeManagedIfUnchanged, "", nil, nil, true, pruneRefs)
}

func (e *mirrorEngine) pushRemotes(repo mirrorRepo, userInitiated bool, releaseTag string, pruneRefs bool) (int, string) {
	e.mu.Lock()
	e.loadHealthLocked()
	e.mu.Unlock()
	managedProduct := repo.managedProduct()
	tag := strings.TrimSpace(releaseTag)
	if repo.PreviewOnly || repo.Private || strings.TrimSpace(repo.LocalPath) == "" {
		return 0, "this repository has no source working copy to push from"
	}
	if tag == "" && e.releasePublicationPendingForRepository(repo.Owner, repo.Name) {
		return 0, "release refs are being held until every signed artifact is present on the managed mirrors"
	}
	if managedProduct && tag != "" && !e.releasePublicationPending(repo.Owner, repo.Name, releaseTag) {
		return 0, "a newer release publication superseded this ref push"
	}
	pushSource := repo.LocalPath
	if !dirExists(pushSource) {
		return 0, fmt.Sprintf("the working copy %s no longer exists", pushSource)
	}
	repoKey := repo.key()
	listing, ok := gitRemotes(e.runCtx(), repo.LocalPath)
	if !ok {
		return 0, fmt.Sprintf("the git remotes of %s could not be read", pushSource)
	}
	remotes := []sshRemote{}
	seenURLs := map[string]bool{}
	for _, line := range strings.Split(listing, "\n") {
		clean := simplified(line)
		if !strings.HasSuffix(clean, "(push)") {
			continue
		}
		parts := strings.Split(clean, " ")
		if len(parts) < 2 {
			continue
		}
		address := parts[1]
		if !strings.HasPrefix(address, "ssh://") || seenURLs[address] {
			continue
		}
		seenURLs[address] = true
		remotes = append(remotes, sshRemote{name: parts[0], url: address, host: urlHost(address)})
	}
	if len(remotes) == 0 {
		return 0, fmt.Sprintf("%s has no ssh:// push remote, so the SSH-fed mirrors receive nothing. Add the gateway's push remote under Remotes on this repository", pushSource)
	}
	nowMs := e.m.nowMs()
	var targets []sshRemote
	var cooling []string
	e.mu.Lock()
	for _, remote := range remotes {
		if userInitiated || e.health.readyToPush(remote.host, nowMs) {
			targets = append(targets, remote)
		} else {
			cooling = append(cooling, remote.host)
		}
	}
	e.scheduleRetryLocked(repoKey, cooling, nowMs)
	if len(targets) == 0 {
		e.mu.Unlock()
		return 0, fmt.Sprintf("all %d mirror gateways are cooling down after a failed push", len(remotes))
	}
	pushKey := func(address string) string { return repoKey + "\x1f" + address }
	var ready []sshRemote
	for _, remote := range targets {
		if !e.pushing[pushKey(remote.url)] {
			ready = append(ready, remote)
		}
	}
	queued := false
	if len(ready) < len(targets) {
		e.pending[repoKey] = true
		queued = true
	}
	e.mu.Unlock()
	if queued && userInitiated {
		e.m.flash(fmt.Sprintf("A mirror push is already running for %s; the newest release state is queued next.", repoKey), false)
	}
	if len(ready) == 0 {
		return len(targets), ""
	}
	targets = ready
	if managedProduct && tag != "" && !e.releasePublicationPending(repo.Owner, repo.Name, releaseTag) {
		return 0, "a newer release publication superseded this ref push"
	}
	e.mu.Lock()
	for _, remote := range targets {
		e.pushing[pushKey(remote.url)] = true
	}
	e.mu.Unlock()
	remaining, failures, divergent := len(targets), 0, 0
	remoteCount := len(targets)
	subject := repoKey
	if tag != "" {
		subject = releaseTag
	}
	finishPush := func(key string, ok, diverged bool) {
		e.mu.Lock()
		delete(e.pushing, key)
		if !ok {
			failures++
		} else if diverged {
			divergent++
		}
		remaining--
		if remaining > 0 {
			e.mu.Unlock()
			return
		}
		failed, held := failures, divergent
		replay := e.pending[repoKey]
		delete(e.pending, repoKey)
		e.mu.Unlock()
		if userInitiated {
			if failed == 0 {
				message := fmt.Sprintf("Pushed %s to %d SSH mirror%s.", subject, remoteCount, plural(remoteCount))
				if held > 0 {
					message += fmt.Sprintf(" %d mirror%s kept a branch that is ahead of this checkout; it syncs once the source catches up.", held, plural(held))
				}
				e.m.flash(message, false)
			} else {
				e.m.flash(fmt.Sprintf("Pushed %s to %d of %d SSH mirrors; %d failed. Check the network log for details.", subject, remoteCount-failed, remoteCount, failed), true)
			}
		}
		if !replay {
			return
		}
		go func() {
			if current, ok := e.repo(repoKey); ok {
				e.pushAll(current, true, false)
			}
		}()
	}
	if userInitiated {
		e.m.flash(fmt.Sprintf("Pushing %s to %d SSH mirror%s…", subject, len(targets), plural(len(targets))), false)
	}
	prRefspec := "refs/pr/*:refs/pr/*"
	pruneFlag := "--no-prune"
	if pruneRefs {
		prRefspec = "+" + prRefspec
		pruneFlag = "--prune"
	}
	env := environmentWithout("GIT_TERMINAL_PROMPT")
	env = append(env, "GIT_TERMINAL_PROMPT=0")
	if _, set := os.LookupEnv("GIT_SSH_COMMAND"); !set {
		env = append(env, "GIT_SSH_COMMAND="+defaultGitSSHCommand)
	}
	for _, remote := range targets {
		remote := remote
		key := pushKey(remote.url)
		args := []string{"-C", pushSource, "push", "--porcelain", "--no-force", pruneFlag, remote.url,
			"refs/heads/*:refs/heads/*", "refs/tags/*:refs/tags/*", prRefspec}
		done := e.m.track()
		go func() {
			defer done()
			result := runMirrorProc(e.runCtx(), mirrorProc{program: "git", args: args, env: env, stdout: true})
			e.concludeRemotePush(repoKey, remote, key, result, finishPush)
		}()
	}
	return len(targets), ""
}

func (e *mirrorEngine) concludeRemotePush(repoKey string, remote sshRemote, key string, result mirrorProcResult, finishPush func(string, bool, bool)) {
	if !result.started {
		e.log(fmt.Sprintf("Mirror: could not run git to push %s to SSH mirror %s.", repoKey, remote.host))
		finishPush(key, false, false)
		return
	}
	errorsText := strings.TrimSpace(result.stderr)
	outcome := classifyMirrorPush(result.stdout)
	if result.exitCode != 0 && !outcome.onlyHeldBack {
		detail := []string{}
		for _, line := range strings.Split(errorsText, "\n") {
			if !strings.HasPrefix(strings.TrimSpace(line), "hint:") {
				detail = append(detail, strings.TrimSpace(line))
			}
		}
		reason := leftUTF16(strings.TrimSpace(strings.Join(detail, " ")), 300)
		if mirrorGatewayUnreachable(errorsText) {
			nowMs := e.m.nowMs()
			e.mu.Lock()
			notice := e.health.noteUnreachable(remote.host, nowMs)
			var writes []daemon.SettingWrite
			if notice.persist {
				writes = e.healthWriteLocked()
			}
			e.scheduleRetryLocked(repoKey, []string{remote.host}, nowMs)
			e.mu.Unlock()
			e.m.writeSettings(writes...)
			if notice.announce {
				minutes := max(int64(1), notice.cooldownMs/60000)
				suffix := "s"
				if notice.cooldownMs < 120000 {
					suffix = ""
				}
				e.log(fmt.Sprintf("Mirror: SSH gateway %s did not answer for %s: %s. Skipping it for about %d minute%s while the rest of the fleet keeps syncing.",
					remote.host, repoKey, reason, minutes, suffix))
			}
			if notice.retired {
				e.log(fmt.Sprintf("Mirror: SSH gateway %s has not answered for over a day, so it is now probed every %d hours instead of every pass and this is the last notice about it. If that node was retired for good, drop it from the fan-out — delete the \"%s\" remote on the repository's Remotes tab, or run \"git remote remove %s\" in the working copy.",
					remote.host, gatewayRetiredCooldownMs/(60*60*1000), remote.name, remote.name))
			}
			finishPush(key, false, false)
			return
		}
		e.log(fmt.Sprintf("Mirror: SSH mirror push of %s to %s failed: %s", repoKey, remote.host, reason))
		finishPush(key, false, false)
		return
	}
	e.mu.Lock()
	answering := e.health.noteReachable(remote.host)
	var writes []daemon.SettingWrite
	if answering {
		writes = e.healthWriteLocked()
	}
	heldKey := repoKey + " " + remote.host
	held := strings.Join(outcome.heldBack, ", ")
	report := false
	if held == "" {
		delete(e.heldBack, heldKey)
	} else if e.heldBack[heldKey] != held {
		e.heldBack[heldKey] = held
		report = true
	}
	e.mu.Unlock()
	e.m.writeSettings(writes...)
	if answering {
		e.log(fmt.Sprintf("Mirror: SSH gateway %s is answering again.", remote.host))
	}
	if len(outcome.updated) > 0 {
		e.log(fmt.Sprintf("Mirror: pushed %s to SSH mirror %s.", repoKey, remote.host))
	}
	if report {
		e.log(fmt.Sprintf("Mirror: SSH mirror %s is ahead of this node on %s for %s, so those branches were left alone (a mirror push never rewinds one). Every other branch synced; they retry once this node catches up.",
			remote.host, held, repoKey))
	}
	finishPush(key, true, len(outcome.heldBack) > 0)
}

func (e *mirrorEngine) scheduleRetryLocked(repoKey string, skipped []string, nowMs int64) {
	if len(skipped) == 0 || strings.TrimSpace(repoKey) == "" {
		return
	}
	delay := mirrorPushRetryDelayMs(&e.health, skipped, nowMs, e.syncIntervalMs())
	if delay <= 0 {
		return
	}
	if existing := e.retries[repoKey]; existing != nil {
		remainingMs := existing.due - nowMs
		if remainingMs >= 0 && remainingMs <= delay {
			return
		}
		existing.timer.Stop()
		delete(e.retries, repoKey)
	}
	wait := min(max(delay, 1000), 60*60*1000)
	retry := &mirrorRetry{due: nowMs + wait}
	retry.timer = time.AfterFunc(time.Duration(wait)*time.Millisecond, func() {
		e.mu.Lock()
		if e.retries[repoKey] == retry {
			delete(e.retries, repoKey)
		}
		e.mu.Unlock()
		if e.runCtx().Err() != nil {
			return
		}
		if repo, ok := e.repo(repoKey); ok {
			e.pushAll(repo, true, false)
		}
	})
	e.retries[repoKey] = retry
}

func mirrorReleaseBlobPath(mirrorPath, hash string) string {
	if len(hash) < 2 {
		return filepath.Join(mirrorPath, "forkmesh-releases", "sha256", hash, hash, "data")
	}
	return filepath.Join(mirrorPath, "forkmesh-releases", "sha256", hash[:2], hash, "data")
}

func visibleDirs(path string) []string {
	entries, err := os.ReadDir(path)
	if err != nil {
		return nil
	}
	out := []string{}
	for _, entry := range entries {
		name := entry.Name()
		if strings.HasPrefix(name, ".") {
			continue
		}
		if info, err := os.Stat(filepath.Join(path, name)); err == nil && info.IsDir() {
			out = append(out, name)
		}
	}
	return out
}

func mirrorReleaseBlobs(mirrorPath string) []releaseBlob {
	if strings.TrimSpace(mirrorPath) == "" || !dirExists(mirrorPath) {
		return nil
	}
	cas := filepath.Join(mirrorPath, "forkmesh-releases", "sha256")
	blobs := []releaseBlob{}
	for _, shard := range visibleDirs(cas) {
		for _, hash := range visibleDirs(filepath.Join(cas, shard)) {
			data := filepath.Join(cas, shard, hash, "data")
			info, err := os.Stat(data)
			if err != nil {
				continue
			}
			absolute, _ := filepath.Abs(data)
			blobs = append(blobs, releaseBlob{hash: hash, path: absolute, size: info.Size()})
		}
	}
	sort.SliceStable(blobs, func(i, j int) bool { return blobs[i].size > blobs[j].size })
	return blobs
}

func regularArtifact(path string) (string, int64, bool) {
	info, err := os.Lstat(path)
	if err != nil || info.Mode()&os.ModeSymlink != 0 || !info.Mode().IsRegular() {
		return "", 0, false
	}
	absolute, _ := filepath.Abs(path)
	return absolute, info.Size(), true
}

func (e *mirrorEngine) collectArtifacts(repo mirrorRepo, releaseTag string, hashes []string, releasePush, reconcile bool) ([]releaseBlob, bool) {
	seen := map[string]bool{}
	out := []releaseBlob{}
	for _, raw := range hashes {
		hash := trimmedLower(raw)
		if seen[hash] {
			continue
		}
		if managedReleaseArtifactSeedCommand(hash) == "" {
			e.log(fmt.Sprintf("Release: %s refs were held back because its signed artifact list contains an invalid digest.", releaseTag))
			return nil, false
		}
		path, size, ok := regularArtifact(mirrorReleaseBlobPath(repo.MirrorPath, hash))
		if !ok || size <= 0 {
			e.log(fmt.Sprintf("Release: %s refs were held back because artifact %s is not present in the local served CAS.", releaseTag, hash[:12]))
			return nil, false
		}
		seen[hash] = true
		out = append(out, releaseBlob{hash: hash, path: path, size: size})
	}
	if !releasePush && reconcile {
		for _, blob := range mirrorReleaseBlobs(repo.MirrorPath) {
			hash := trimmedLower(blob.hash)
			if seen[hash] || blob.size <= 0 || managedReleaseArtifactSeedCommand(hash) == "" {
				continue
			}
			path, size, ok := regularArtifact(blob.path)
			if !ok {
				continue
			}
			seen[hash] = true
			out = append(out, releaseBlob{hash: hash, path: path, size: size})
		}
	}
	return out, true
}

func (e *mirrorEngine) managedTargets(artifacts []releaseBlob, reconcile bool) ([]managedTarget, int) {
	m := e.m
	hosts := m.savedHosts()
	sshDir := m.sshDir()
	targets := []managedTarget{}
	unavailable := 0
	seen := map[string]bool{}
	for _, saved := range hosts {
		if !savedHostMayBeActive(saved) {
			continue
		}
		node := strings.TrimSpace(jsonString(saved["name"]))
		host := strings.TrimSpace(jsonString(saved["ip"]))
		user := strings.TrimSpace(jsonString(saved["user"]))
		if node == "" || host == "" || user != "root" || seen[strings.ToLower(host)] {
			continue
		}
		password := m.password(credentialKey(node, host, user))
		identity := savedHostIdentityFile(hosts, sshDir, node, host, user)
		lastErr := ""
		build := func(remote string) sshCommand {
			command, err := buildHostSSHCommand(sshDir, host, user, password, remote, identity)
			if err != nil {
				lastErr = err.Error()
				return sshCommand{}
			}
			lastErr = ""
			return command
		}
		transport := build("true")
		wake := build(managedWakeCommand)
		var inventory sshCommand
		if reconcile {
			list := make([]string, 0, len(artifacts))
			for _, artifact := range artifacts {
				list = append(list, artifact.hash)
			}
			inventory = build(managedReleaseArtifactInventoryCommand(list))
		}
		commands := make([]sshCommand, 0, len(artifacts))
		artifactInvalid := false
		for _, artifact := range artifacts {
			command := build(managedReleaseArtifactSeedCommand(artifact.hash))
			if command.Program == "" {
				artifactInvalid = true
			}
			commands = append(commands, command)
		}
		if transport.Program == "" || len(transport.Args) < 2 || wake.Program == "" || (reconcile && inventory.Program == "") || artifactInvalid {
			unavailable++
			reason := lastErr
			if reason == "" {
				reason = "invalid saved host metadata"
			}
			e.log(fmt.Sprintf("Mirror: cannot open the managed SSH transport to %s (%s): %s", node, host, reason))
			continue
		}
		transport.Args = append(append([]string{}, transport.Args[:len(transport.Args)-2]...),
			"-o", "ServerAliveInterval=15", "-o", "ServerAliveCountMax=2")
		env := []string{}
		for _, entry := range transport.Env {
			if strings.HasPrefix(entry, "GIT_TERMINAL_PROMPT=") || strings.HasPrefix(entry, "GIT_SSH_COMMAND=") {
				continue
			}
			env = append(env, entry)
		}
		env = append(env, "GIT_TERMINAL_PROMPT=0", "GIT_SSH_COMMAND="+hostSSHCommandLine(transport))
		address := managedPushURL(user, host)
		if address == "" {
			unavailable++
			continue
		}
		seen[strings.ToLower(host)] = true
		targets = append(targets, managedTarget{node: node, host: host, url: address, env: env, wake: wake, inventory: inventory, artifacts: commands})
	}
	return targets, unavailable
}

func finishLater(onFinished func(int, int), succeeded, failed int) {
	if onFinished != nil {
		go onFinished(succeeded, failed)
	}
}

func (e *mirrorEngine) pushManaged(repo mirrorRepo, wakeIfUnchanged bool, releaseTag string, hashes []string, onFinished func(int, int), reconcile, pruneRefs bool) int {
	if repo.PreviewOnly || repo.Private || strings.TrimSpace(repo.LocalPath) == "" || !dirExists(repo.LocalPath) || !repo.managedProduct() {
		finishLater(onFinished, 0, 0)
		return 0
	}
	releasePush := strings.TrimSpace(releaseTag) != ""
	if releasePush && !e.releasePublicationPending(repo.Owner, repo.Name, releaseTag) {
		e.log(fmt.Sprintf("Release: %s refs were held back because a newer publication superseded this fan-out.", releaseTag))
		finishLater(onFinished, 0, 1)
		return 0
	}
	if !releasePush && e.releasePublicationPendingForRepository(repo.Owner, repo.Name) {
		finishLater(onFinished, 0, 0)
		return 0
	}
	artifacts, ok := e.collectArtifacts(repo, releaseTag, hashes, releasePush, reconcile)
	if !ok {
		finishLater(onFinished, 0, 1)
		return 0
	}
	key := repo.lowerKey()
	e.mu.Lock()
	startReconcile := !releasePush && reconcile && len(artifacts) > 0 && !e.reconciling[key]
	e.mu.Unlock()
	targets, unavailable := e.managedTargets(artifacts, startReconcile)
	if len(targets) == 0 {
		finishLater(onFinished, 0, unavailable)
		return 0
	}
	hosts := make([]string, 0, len(targets))
	for _, target := range targets {
		hosts = append(hosts, target.host)
	}
	if releasePush {
		e.mu.Lock()
		busy := e.gate.busy(key, hosts)
		e.mu.Unlock()
		if busy {
			time.AfterFunc(releaseGateRetry, func() {
				if current, ok := e.repo(key); ok {
					e.pushManaged(current, wakeIfUnchanged, releaseTag, hashes, onFinished, reconcile, false)
					return
				}
				if onFinished != nil {
					onFinished(0, 1)
				}
			})
			return len(targets)
		}
	}
	if startReconcile {
		e.startArtifactReconciliation(key, targets, unavailable, artifacts)
	}
	if releasePush && len(artifacts) > 0 {
		return e.seedReleaseArtifacts(repo, wakeIfUnchanged, releaseTag, onFinished, targets, unavailable, artifacts)
	}
	return e.pushManagedRefs(repo, key, hosts, targets, unavailable, wakeIfUnchanged, pruneRefs, releaseTag, onFinished)
}

func (e *mirrorEngine) runSSHStep(command sshCommand, stdinPath string, timeout time.Duration) mirrorProcResult {
	return runMirrorProc(e.runCtx(), mirrorProc{program: command.Program, args: command.Args, env: command.Env, stdinPath: stdinPath, timeout: timeout})
}

func firstErrors(result mirrorProcResult, fallback string) string {
	text := leftUTF16(strings.TrimSpace(result.stderr), 300)
	if text == "" {
		return fallback
	}
	return text
}

func (e *mirrorEngine) startArtifactReconciliation(key string, targets []managedTarget, unavailable int, artifacts []releaseBlob) {
	e.mu.Lock()
	e.reconciling[key] = true
	e.mu.Unlock()
	var mu sync.Mutex
	remaining, failed, repaired := len(targets), unavailable, 0
	finishTarget := func(ok bool, count int) {
		mu.Lock()
		if !ok {
			failed++
		}
		repaired += count
		remaining--
		last := remaining == 0
		failedNow, repairedNow := failed, repaired
		mu.Unlock()
		if !last {
			return
		}
		e.mu.Lock()
		delete(e.reconciling, key)
		e.mu.Unlock()
		if failedNow > 0 {
			e.log(fmt.Sprintf("Artifacts: managed-host reconciliation failed on %d host%s and will retry; normal ref fan-out continued independently.", failedNow, plural(failedNow)))
			return
		}
		if repairedNow > 0 {
			e.log(fmt.Sprintf("Artifacts: restored %d retained CAS object%s across the managed mirror fleet.", repairedNow, plural(repairedNow)))
		}
	}
	indexByHash := map[string]int{}
	for i, artifact := range artifacts {
		if _, ok := indexByHash[artifact.hash]; !ok {
			indexByHash[artifact.hash] = i
		}
	}
	for _, target := range targets {
		target := target
		done := e.m.track()
		go func() {
			defer done()
			inventory := e.runSSHStepOut(target.inventory, artifactTransferCap)
			if !inventory.ok() {
				e.log(fmt.Sprintf("Artifacts: could not inventory managed host %s (%s): %s", target.node, target.host, firstErrors(inventory, "SSH artifact inventory failed")))
				finishTarget(false, 0)
				return
			}
			missing := []int{}
			reported := map[string]bool{}
			for _, line := range lineBreaks.Split(strings.TrimSpace(inventory.stdout), -1) {
				if line == "" {
					continue
				}
				hash := trimmedLower(line)
				index, known := indexByHash[hash]
				if !known || reported[hash] {
					e.log(fmt.Sprintf("Artifacts: managed host %s (%s) returned an invalid inventory.", target.node, target.host))
					finishTarget(false, 0)
					return
				}
				reported[hash] = true
				missing = append(missing, index)
			}
			if len(missing) == 0 {
				finishTarget(true, 0)
				return
			}
			for _, index := range missing {
				artifact := artifacts[index]
				seed := e.runSSHStep(target.artifacts[index], artifact.path, artifactTransferCap)
				if !seed.ok() {
					e.log(fmt.Sprintf("Artifacts: could not restore %s on managed host %s (%s): %s", artifact.hash[:12], target.node, target.host, firstErrors(seed, "SSH artifact transfer failed")))
					finishTarget(false, 0)
					return
				}
			}
			wake := e.runSSHStep(target.wake, "", 0)
			if !wake.ok() {
				e.log(fmt.Sprintf("Artifacts: restored the CAS on %s (%s) but could not wake its catalog publish: %s", target.node, target.host, firstErrors(wake, "sync wake failed")))
			}
			finishTarget(wake.ok(), len(missing))
		}()
	}
}

func (e *mirrorEngine) runSSHStepOut(command sshCommand, timeout time.Duration) mirrorProcResult {
	return runMirrorProc(e.runCtx(), mirrorProc{program: command.Program, args: command.Args, env: command.Env, timeout: timeout, stdout: true})
}

func (e *mirrorEngine) seedReleaseArtifacts(repo mirrorRepo, wakeIfUnchanged bool, releaseTag string, onFinished func(int, int), targets []managedTarget, unavailable int, artifacts []releaseBlob) int {
	var mu sync.Mutex
	remaining, failed := len(targets), unavailable
	finishTarget := func(ok bool) {
		mu.Lock()
		if !ok {
			failed++
		}
		remaining--
		last := remaining == 0
		failedNow := failed
		mu.Unlock()
		if !last {
			return
		}
		if failedNow > 0 {
			e.log(fmt.Sprintf("Release: %s artifact seeding failed on %d managed host%s; refs and deployment remain pending.", releaseTag, failedNow, plural(failedNow)))
			if onFinished != nil {
				onFinished(0, failedNow)
			}
			return
		}
		e.log(fmt.Sprintf("Release: seeded %d signed artifact%s on every managed host before advancing refs.", len(artifacts), plural(len(artifacts))))
		current, ok := e.repo(repo.lowerKey())
		if !ok {
			current = repo
		}
		e.pushManaged(current, wakeIfUnchanged, releaseTag, nil, onFinished, false, false)
	}
	e.m.flash(fmt.Sprintf("Seeding %d signed release artifact%s to %d installed mirror host%s before publishing refs…",
		len(artifacts), plural(len(artifacts)), len(targets), plural(len(targets))), false)
	for _, target := range targets {
		target := target
		done := e.m.track()
		go func() {
			defer done()
			for index, artifact := range artifacts {
				seed := e.runSSHStep(target.artifacts[index], artifact.path, artifactTransferCap)
				if !seed.ok() {
					e.log(fmt.Sprintf("Release: could not seed artifact %s on managed host %s (%s): %s", artifact.hash[:12], target.node, target.host, firstErrors(seed, "SSH artifact transfer failed")))
					finishTarget(false)
					return
				}
			}
			finishTarget(true)
		}()
	}
	return len(targets)
}

func (e *mirrorEngine) pushManagedRefs(repo mirrorRepo, key string, hosts []string, targets []managedTarget, unavailable int, wakeIfUnchanged, pruneRefs bool, releaseTag string, onFinished func(int, int)) int {
	releasePush := strings.TrimSpace(releaseTag) != ""
	e.mu.Lock()
	reservation := e.gate.reserve(key, hosts)
	e.mu.Unlock()
	ready := []managedTarget{}
	for _, target := range targets {
		if reservation.readyHosts[trimmedLower(target.host)] {
			ready = append(ready, target)
		}
	}
	if len(ready) == 0 {
		finishLater(onFinished, 0, 0)
		return len(targets)
	}
	targets = ready
	replay := func() {
		go func() {
			if current, ok := e.repo(key); ok {
				e.pushManaged(current, wakeIfUnchanged, "", nil, nil, true, pruneRefs)
			}
		}()
	}
	var mu sync.Mutex
	remaining, succeeded, failed, updated := len(targets), 0, unavailable, 0
	subject := repo.key()
	if releasePush {
		subject = strings.TrimSpace(releaseTag)
	}
	complete := func(ok, changed bool) {
		mu.Lock()
		if ok {
			succeeded++
		} else {
			failed++
		}
		if changed {
			updated++
		}
		remaining--
		last := remaining == 0
		s, f, u := succeeded, failed, updated
		mu.Unlock()
		if !last {
			return
		}
		if releasePush || u > 0 || f > 0 {
			if releasePush {
				e.log(fmt.Sprintf("Release: managed-host ref fan-out for %s finished (%d succeeded, %d failed).", subject, s, f))
			} else {
				e.log(fmt.Sprintf("Mirror: managed-host ref fan-out for %s finished (%d updated, %d failed).", subject, u, f))
			}
		}
		if onFinished != nil {
			onFinished(s, f)
		}
	}
	if releasePush {
		e.m.flash(fmt.Sprintf("Pushing %s directly to %d installed mirror host%s…", subject, len(targets), plural(len(targets))), false)
	}
	prRefspec := "refs/pr/*:refs/pr/*"
	pruneFlag := "--no-prune"
	if pruneRefs {
		prRefspec = "+" + prRefspec
		pruneFlag = "--prune"
	}
	for _, target := range targets {
		target := target
		args := []string{"-C", repo.LocalPath, "push", "--porcelain", "--no-force", pruneFlag, managedReceivePack, target.url,
			"refs/heads/*:refs/heads/*", "refs/tags/*:refs/tags/*", prRefspec}
		done := e.m.track()
		go func() {
			defer done()
			result := runMirrorProc(e.runCtx(), mirrorProc{program: "git", args: args, env: target.env, timeout: managedPushCap, stdout: true})
			e.mu.Lock()
			again := e.gate.finish(key, target.host)
			e.mu.Unlock()
			if again {
				replay()
			}
			if !result.started {
				e.log(fmt.Sprintf("Mirror: could not start the direct ref push of %s/%s to managed host %s (%s).", repo.Owner, repo.Name, target.node, target.host))
				complete(false, false)
				return
			}
			outcome := classifyMirrorPush(result.stdout)
			gitOK := result.normalExit && (result.exitCode == 0 || (!releasePush && outcome.onlyHeldBack))
			changed := len(outcome.updated) > 0
			gitErrors := leftUTF16(strings.TrimSpace(result.stderr), 300)
			if !changed && (!gitOK || !wakeIfUnchanged) {
				if !gitOK {
					reason := gitErrors
					if reason == "" {
						reason = "git push failed"
					}
					e.log(fmt.Sprintf("Mirror: direct ref update of %s/%s on managed host %s (%s) failed: %s", repo.Owner, repo.Name, target.node, target.host, reason))
				}
				complete(gitOK, false)
				return
			}
			wake := e.runSSHStep(target.wake, "", managedWakeCap)
			wakeOK := wake.ok()
			if gitOK && wakeOK {
				if changed {
					e.log(fmt.Sprintf("Mirror: pushed %s/%s to managed host %s (%s) and woke its daemon.", repo.Owner, repo.Name, target.node, target.host))
				} else {
					e.log(fmt.Sprintf("Mirror: refs for %s/%s were already current on managed host %s (%s); woke its daemon.", repo.Owner, repo.Name, target.node, target.host))
				}
				complete(true, changed)
				return
			}
			reason := ""
			if !gitOK {
				reason = gitErrors
				if reason == "" {
					reason = "git push failed"
				}
			}
			if !wakeOK {
				if reason != "" {
					reason += "; "
				}
				wakeErrors := leftUTF16(strings.TrimSpace(wake.stderr), 300)
				if wakeErrors == "" {
					reason += "mirror daemon wake failed"
				} else {
					reason += "mirror daemon wake failed: " + wakeErrors
				}
			}
			e.log(fmt.Sprintf("Mirror: direct ref update of %s/%s on managed host %s (%s) was incomplete: %s", repo.Owner, repo.Name, target.node, target.host, reason))
			complete(false, changed)
		}()
	}
	return len(targets)
}

func (e *mirrorEngine) unwire(ctx context.Context, node string) int {
	target := strings.TrimSpace(node)
	if target == "" {
		return 0
	}
	e.mu.Lock()
	e.loadHealthLocked()
	e.mu.Unlock()
	address := ""
	for _, host := range e.m.savedHosts() {
		if strings.EqualFold(strings.TrimSpace(jsonString(host["name"])), target) {
			address = strings.TrimSpace(jsonString(host["ip"]))
			break
		}
	}
	paths := []string{}
	seenPaths := map[string]bool{}
	for _, repo := range e.repoList() {
		for _, path := range []string{repo.LocalPath, repo.MirrorPath} {
			clean := strings.TrimSpace(path)
			if clean != "" && !seenPaths[clean] && dirExists(clean) {
				seenPaths[clean] = true
				paths = append(paths, clean)
			}
		}
	}
	removed := 0
	retired := map[string]bool{}
	for _, path := range paths {
		listing, ok := gitRemotes(ctx, path)
		if !ok {
			continue
		}
		doomed := map[string]bool{}
		for _, line := range strings.Split(listing, "\n") {
			parts := strings.Split(simplified(line), " ")
			if len(parts) < 2 || !strings.HasPrefix(parts[1], "ssh://") {
				continue
			}
			host := urlHost(parts[1])
			byAddress := address != "" && strings.EqualFold(host, address)
			if !byAddress && !mirrorGatewayHostBelongsToNode(host, target) {
				continue
			}
			doomed[parts[0]] = true
			retired[host] = true
		}
		names := make([]string, 0, len(doomed))
		for name := range doomed {
			names = append(names, name)
		}
		sort.Strings(names)
		for _, name := range names {
			result := runMirrorProc(ctx, mirrorProc{program: "git", args: []string{"remote", "remove", name}, dir: path, env: os.Environ(), timeout: mirrorRemotesTimeout})
			if result.ok() {
				removed++
				e.log(fmt.Sprintf("Mirror: removed git remote %s from %s — node %s was deleted.", name, path, target))
			}
		}
	}
	e.mu.Lock()
	forgot := false
	for host := range retired {
		forgot = e.health.forget(host) || forgot
	}
	var writes []daemon.SettingWrite
	if forgot {
		writes = e.healthWriteLocked()
	}
	e.mu.Unlock()
	e.m.writeSettings(writes...)
	if removed > 0 {
		e.m.broadcastUI("core.mirror.unwired", map[string]any{"node": target, "removed": removed})
	}
	return removed
}
