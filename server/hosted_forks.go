package mirrornode

// Website forks of hosted repositories (docs/HOSTED_REPOS.md, "Forks").
//
// A fork is an ordinary hosted repository whose desired entry names its
// parent (forkOf) and its fork network (network, the root repository's id).
// Every core already holds the parent, so each core builds the fork locally
// instead of copying it over the network:
//
//   - The network's object pool, <root>/.pools/<network>.git, is created once
//     by a local clone of the parent. A local clone hard-links the parent's
//     object files, so the pool costs no disk until the parent repacks; from
//     then on the pool keeps the objects the forks borrow. The pool is never
//     served, never pushed to and never garbage collected (hosted
//     maintenance only walks <root>/hr*.git), so a later force-push, delete
//     or repack of the parent can never remove an object a fork needs.
//   - Before each new fork the pool fetches the parent's current heads and
//     tags into refs/pools/<parent>/..., so a fork of a fork borrows from the
//     same pool and only the objects new since the last fork are copied.
//   - The fork's objects/info/alternates points at the pool, and its heads,
//     tags and HEAD are copied from the parent. Git sees every object already
//     present, so nothing is transferred and the fork costs a few KB. Its own
//     later pushes stay in its own object directory (hosted maintenance
//     repacks with -l, which never copies borrowed objects in).

import (
	"context"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
)

const hostedPoolDirectory = ".pools"

// forkFromLocal fills an empty fork from this core's copy of its parent.
// It reports false (and no error) when the parent is not here yet; the
// replicator then falls back to fetching from peers.
func (r *hostedReplicator) forkFromLocal(ctx context.Context, item hostedDesired, gitDir string) (bool, error) {
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
	parent := r.gitDir(item.ForkOf)
	if !validBareRepository(parent) ||
		len(gitLines(ctx, parent, "for-each-ref", "--count=1", "--format=%(refname)",
			"refs/heads")) == 0 {
		return false, nil
	}
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
	// Every object is reachable through the pool, so this fetch transfers
	// nothing: it only writes the refs.
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

// ensurePool returns the network pool, creating it from the parent.
func (r *hostedReplicator) ensurePool(ctx context.Context, network, parent string) (string, error) {
	pools := filepath.Join(r.cfg.Root, hostedPoolDirectory)
	pool := filepath.Join(pools, network+".git")
	if validBareRepository(pool) {
		return pool, nil
	}
	if _, err := os.Lstat(pool); err == nil {
		return "", errors.New("fork pool is not a bare repository")
	}
	// A pool is cut from the network's root. A parent that itself borrows
	// objects would hand the pool a borrowed alternates path, so refuse.
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
	// --local hard-links the parent's object files (same filesystem).
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
	if err := os.Rename(target, pool); err != nil {
		return "", err
	}
	return pool, nil
}

// refreshPool keeps the parent's current tips in the pool under a namespace
// per member, so the objects they reach stay there for good.
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
