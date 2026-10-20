package mirrornode

import (
	"context"
	"errors"
	"fmt"
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
	for _, setting := range [][2]string{
		{"gc.auto", "0"}, {"gc.pruneExpire", "never"}, {"maintenance.auto", "false"},
		{"receive.autogc", "false"}, {"core.hooksPath", os.DevNull},
	} {
		if output, err := exec.CommandContext(ctx, "git", "--git-dir="+target, "config", "--local",
			setting[0], setting[1]).CombinedOutput(); err != nil {
			return "", fmt.Errorf("fork pool config: %s", boundedText(output, 160))
		}
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
