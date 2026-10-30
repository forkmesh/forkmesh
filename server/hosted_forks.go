package mirrornode

import (
	"context"
	"errors"
	"fmt"
	"net"
	"net/url"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"syscall"
)

const hostedPoolDirectory = ".pools"

func (r *hostedReplicator) forkFromLocal(ctx context.Context, item hostedDesired, gitDir string, parentPublic bool) (bool, error) {
	if !hostedIDPattern.MatchString(item.ForkOf) || item.ForkOf == item.ID {
		return false, errors.New("invalid fork parent")
	}
	network := item.Network
	if network == "" {
		network = item.ForkOf
	}
	if !hostedIDPattern.MatchString(network) {
		return false, errors.New("invalid fork network")
	}
	if !parentPublic {
		return false, nil
	}
	parent := r.gitDir(item.ForkOf)
	if !validBareRepository(parent) ||
		len(gitLines(ctx, parent, "for-each-ref", "--count=1", "--format=%(refname)",
			"refs/heads")) == 0 {
		return false, nil
	}
	unlock, locked, err := lockHostedParentShared(parent)
	if err != nil || !locked {
		return false, err
	}
	defer unlock()
	pool, err := r.ensurePool(ctx, network, parent)
	if err != nil {
		return false, err
	}
	if err := refreshPool(ctx, pool, parent, item.ForkOf); err != nil {
		return false, err
	}
	poolObjects := filepath.Join(pool, "objects")
	info := filepath.Join(gitDir, "objects", "info")
	if err := os.MkdirAll(info, 0o700); err != nil {
		return false, err
	}
	if err := os.WriteFile(filepath.Join(info, "alternates"), []byte(poolObjects+"\n"), 0o600); err != nil {
		return false, err
	}
	if !item.Primary {
		return false, nil
	}
	if output, err := exec.CommandContext(ctx, "git", "--git-dir="+gitDir, "fetch", "--quiet",
		"--no-tags", "--", parent, "+refs/heads/*:refs/heads/*", "+refs/tags/*:refs/tags/*",
	).CombinedOutput(); err != nil {
		return false, fmt.Errorf("fork refs: %s", boundedText(output, 200))
	}
	if head := gitValue(ctx, parent, "symbolic-ref", "--quiet", "HEAD"); strings.HasPrefix(head, "refs/heads/") &&
		gitRefValue(ctx, gitDir, head) != "" {
		_ = exec.CommandContext(ctx, "git", "--git-dir="+gitDir, "symbolic-ref", "HEAD", head).Run()
	}
	return true, nil
}

func (r *hostedReplicator) forkFromUpstream(ctx context.Context, item hostedDesired, gitDir string) (bool, error) {
	if item.ForkUpstream == nil || !validHostedForkUpstream(*item.ForkUpstream) ||
		!hostedIDPattern.MatchString(item.Network) || item.Network == item.ID {
		return false, errors.New("invalid fork parent")
	}
	parent := r.upstreamCopy(ctx, *item.ForkUpstream)
	pool := filepath.Join(r.cfg.Root, hostedPoolDirectory, item.Network+".git")
	prefix := "refs/pools/" + item.Network + "/"
	switch {
	case parent != "":
		unlock, locked, err := lockUpstreamCopyShared(parent)
		if err != nil || !locked {
			return false, err
		}
		defer unlock()
		if pool, err = r.ensurePool(ctx, item.Network, parent); err != nil {
			return false, err
		}
		if err := refreshPool(ctx, pool, parent, item.Network); err != nil {
			return false, err
		}
	case item.Primary:
		var err error
		if pool, err = r.ensureEmptyPool(ctx, item.Network); err != nil {
			return false, err
		}
		if err := r.fetchForkUpstream(ctx, pool, item.ForkUpstream.CloneURL, prefix); err != nil {
			return false, err
		}
	case !validBareRepository(pool):
		return false, nil
	}
	poolObjects := filepath.Join(pool, "objects")
	info := filepath.Join(gitDir, "objects", "info")
	if err := os.MkdirAll(info, 0o700); err != nil {
		return false, err
	}
	if err := os.WriteFile(filepath.Join(info, "alternates"), []byte(poolObjects+"\n"), 0o600); err != nil {
		return false, err
	}
	if !item.Primary {
		return false, nil
	}
	if output, err := exec.CommandContext(ctx, "git", "--git-dir="+gitDir, "fetch", "--quiet",
		"--no-tags", "--", pool, "+"+prefix+"heads/*:refs/heads/*", "+"+prefix+"tags/*:refs/tags/*",
	).CombinedOutput(); err != nil {
		return false, fmt.Errorf("fork refs: %s", boundedText(output, 200))
	}
	if len(gitLines(ctx, gitDir, "for-each-ref", "--count=1", "--format=%(refname)", "refs/heads")) == 0 {
		return false, nil
	}
	head := ""
	if parent != "" {
		head = gitValue(ctx, parent, "symbolic-ref", "--quiet", "HEAD")
	}
	for _, candidate := range []string{head, "refs/heads/main", "refs/heads/master"} {
		if strings.HasPrefix(candidate, "refs/heads/") && gitRefValue(ctx, gitDir, candidate) != "" {
			_ = exec.CommandContext(ctx, "git", "--git-dir="+gitDir, "symbolic-ref", "HEAD", candidate).Run()
			break
		}
	}
	return true, nil
}

func validHostedForkUpstream(upstream hostedForkUpstream) bool {
	return nodePattern.MatchString(upstream.Owner) && repositoryPattern.MatchString(upstream.Name) &&
		!strings.HasPrefix(upstream.Name, ".") && hostedForkCloneURLAllowed(upstream.CloneURL)
}

func hostedForkCloneURLAllowed(raw string) bool {
	parsed, err := url.Parse(raw)
	if err != nil || len(raw) > 500 || parsed.User != nil || parsed.Opaque != "" || parsed.RawQuery != "" ||
		parsed.ForceQuery || parsed.Fragment != "" || strings.ContainsAny(raw, "?#") {
		return false
	}
	host := strings.ToLower(parsed.Hostname())
	switch {
	case parsed.Scheme == "https":
		if host == "" || host == "localhost" || strings.HasSuffix(host, ".localhost") || net.ParseIP(host) != nil {
			return false
		}
	case parsed.Scheme == "http" && isLoopbackHost(host):
	default:
		return false
	}
	path := strings.Trim(parsed.Path, "/")
	if path == "" {
		return false
	}
	for _, part := range strings.Split(path, "/") {
		if part == "" || part == "." || part == ".." {
			return false
		}
	}
	return true
}

func (r *hostedReplicator) upstreamCopy(ctx context.Context, upstream hostedForkUpstream) string {
	key := strings.ToLower(upstream.Owner + "/" + upstream.Name)
	for _, repo := range r.d.gateway.Repositories {
		if repositoryKey(repo) != key || !repo.Enabled || repo.Visibility != "public" ||
			hostedIDFromGitDir(r.cfg.Root, repo.GitDir) != "" || !validBareRepository(repo.GitDir) {
			continue
		}
		if len(gitLines(ctx, repo.GitDir, "for-each-ref", "--count=1", "--format=%(refname)",
			"refs/heads")) > 0 {
			return repo.GitDir
		}
	}
	return ""
}

func (r *hostedReplicator) forkFetchHostAllowed(raw string) bool {
	parsed, err := url.Parse(raw)
	if err != nil {
		return false
	}
	host := strings.ToLower(parsed.Hostname())
	if parsed.Scheme == "http" {
		return isLoopbackHost(host)
	}
	if parsed.Scheme != "https" || parsed.Port() != "" {
		return false
	}
	if r.importAllowed(raw) {
		return true
	}
	if catalog, err := url.Parse(r.d.config.CatalogURL); err == nil &&
		catalog.Scheme == "https" && catalog.Port() == "" {
		return host == strings.ToLower(catalog.Hostname())
	}
	return false
}

func (r *hostedReplicator) fetchForkUpstream(ctx context.Context, pool, cloneURL, prefix string) error {
	if !hostedForkCloneURLAllowed(cloneURL) {
		return errors.New("fork parent URL is not a public https URL")
	}
	if !r.forkFetchHostAllowed(cloneURL) {
		return errors.New("fork parent URL is not on this network's git origin or an allowed forge")
	}
	args := []string{"--git-dir=" + pool, "fetch", "--quiet", "--no-tags", "--", cloneURL,
		"+refs/heads/*:" + prefix + "heads/*", "+refs/tags/*:" + prefix + "tags/*"}
	env := append(gitNetworkEnvironment(),
		"GIT_ALLOW_PROTOCOL=https:http",
		"GIT_CONFIG_COUNT=2",
		"GIT_CONFIG_KEY_0=credential.helper",
		"GIT_CONFIG_VALUE_0=",
		"GIT_CONFIG_KEY_1=http.followRedirects",
		"GIT_CONFIG_VALUE_1=false")
	return r.boundedGit(ctx, pool, args, env)
}

func lockUpstreamCopyShared(gitDir string) (func(), bool, error) {
	if _, err := os.Lstat(filepath.Join(gitDir, hostedMaintenanceLock)); errors.Is(err, os.ErrNotExist) {
		return func() {}, true, nil
	}
	return lockHostedParentShared(gitDir)
}

func (r *hostedReplicator) ensureEmptyPool(ctx context.Context, network string) (string, error) {
	pools := filepath.Join(r.cfg.Root, hostedPoolDirectory)
	pool := filepath.Join(pools, network+".git")
	if validBareRepository(pool) {
		return pool, nil
	}
	if _, err := os.Lstat(pool); err == nil {
		return "", errors.New("fork pool is not a bare repository")
	}
	if err := os.MkdirAll(pools, 0o700); err != nil {
		return "", err
	}
	temporary, err := os.MkdirTemp(pools, ".new-*")
	if err != nil {
		return "", err
	}
	defer removeTreeUnder(pools, temporary)
	target := filepath.Join(temporary, "pool.git")
	if output, err := exec.CommandContext(ctx, "git", "init", "--quiet", "--bare", "--", target).CombinedOutput(); err != nil {
		return "", fmt.Errorf("fork pool: %s", boundedText(output, 200))
	}
	if err := configurePool(ctx, target); err != nil {
		return "", err
	}
	if err := os.Rename(target, pool); err != nil {
		return "", err
	}
	return pool, nil
}

func configurePool(ctx context.Context, pool string) error {
	for _, setting := range [][2]string{
		{"gc.auto", "0"}, {"gc.pruneExpire", "never"}, {"maintenance.auto", "false"},
		{"receive.autogc", "false"}, {"core.hooksPath", os.DevNull},
	} {
		if output, err := exec.CommandContext(ctx, "git", "--git-dir="+pool, "config", "--local",
			setting[0], setting[1]).CombinedOutput(); err != nil {
			return fmt.Errorf("fork pool config: %s", boundedText(output, 160))
		}
	}
	return nil
}

func (r *hostedReplicator) ensurePool(ctx context.Context, network, parent string) (string, error) {
	pools := filepath.Join(r.cfg.Root, hostedPoolDirectory)
	pool := filepath.Join(pools, network+".git")
	if validBareRepository(pool) {
		return pool, nil
	}
	if _, err := os.Lstat(pool); err == nil {
		return "", errors.New("fork pool is not a bare repository")
	}
	if _, err := os.Lstat(filepath.Join(parent, "objects", "info", "alternates")); err == nil {
		return "", errors.New("fork pool is missing for a borrowed parent")
	}
	if err := os.MkdirAll(pools, 0o700); err != nil {
		return "", err
	}
	temporary, err := os.MkdirTemp(pools, ".new-*")
	if err != nil {
		return "", err
	}
	defer removeTreeUnder(pools, temporary)
	target := filepath.Join(temporary, "pool.git")
	if output, err := exec.CommandContext(ctx, "git", "clone", "--quiet", "--bare", "--local",
		"--no-tags", "--", parent, target).CombinedOutput(); err != nil {
		return "", fmt.Errorf("fork pool: %s", boundedText(output, 200))
	}
	if err := configurePool(ctx, target); err != nil {
		return "", err
	}
	if output, err := exec.CommandContext(ctx, "git", "--git-dir="+target, "fsck", "--no-progress",
		"--connectivity-only", "--no-dangling").CombinedOutput(); err != nil {
		return "", fmt.Errorf("fork pool check: %s", boundedText(output, 200))
	}
	if err := os.Rename(target, pool); err != nil {
		return "", err
	}
	return pool, nil
}

func refreshPool(ctx context.Context, pool, parent, parentID string) error {
	prefix := "refs/pools/" + parentID + "/"
	if output, err := exec.CommandContext(ctx, "git", "--git-dir="+pool, "fetch", "--quiet",
		"--no-tags", "--", parent, "+refs/heads/*:"+prefix+"heads/*",
		"+refs/tags/*:"+prefix+"tags/*",
	).CombinedOutput(); err != nil {
		return fmt.Errorf("fork pool refresh: %s", boundedText(output, 200))
	}
	return nil
}

func lockHostedParentShared(gitDir string) (func(), bool, error) {
	lock, err := os.OpenFile(filepath.Join(gitDir, hostedMaintenanceLock),
		os.O_CREATE|os.O_RDWR|syscall.O_NOFOLLOW, 0o600)
	if err != nil {
		return nil, false, err
	}
	if err := syscall.Flock(int(lock.Fd()), syscall.LOCK_SH|syscall.LOCK_NB); err != nil {
		lock.Close()
		if errors.Is(err, syscall.EWOULDBLOCK) {
			return nil, false, nil
		}
		return nil, false, err
	}
	return func() {
		_ = syscall.Flock(int(lock.Fd()), syscall.LOCK_UN)
		lock.Close()
	}, true, nil
}
