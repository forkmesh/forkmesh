package mirrornode

import (
	"context"
	"errors"
	"fmt"
	"log"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"time"
)

const (
	agentTrashRefRoot = "refs/forkmesh/trash/agent/"
	agentTrashKeep    = 20
)

func prepareAgentWorkspace(ctx context.Context, gitDir, dir, branch, base, node string) error {
	if err := os.MkdirAll(filepath.Dir(dir), 0700); err != nil {
		return err
	}
	fresh, err := agentWorkspaceNeedsClone(ctx, dir)
	if err != nil {
		return err
	}
	start := "origin/" + base
	if gitRefValue(ctx, gitDir, "refs/heads/"+branch) != "" {
		start = "origin/" + branch
	}
	if fresh {
		if _, err := agentGit(ctx, "", "clone", "--quiet", "--shared", "--no-checkout",
			"--", gitDir, dir); err != nil {
			return err
		}
		if _, err := agentGit(ctx, dir, "checkout", "--quiet", "-B", branch, start); err != nil {
			return err
		}
	} else if err := reuseAgentWorkspace(ctx, dir, branch, start, node); err != nil {
		return err
	}
	for _, args := range [][]string{
		{"config", "user.name", commitIdentityName(node)},
		{"config", "user.email", commitIdentityEmail(node)},
	} {
		if _, err := agentGit(ctx, dir, args...); err != nil {
			return err
		}
	}
	return nil
}

func agentWorkspaceNeedsClone(ctx context.Context, dir string) (bool, error) {
	entries, err := os.ReadDir(dir)
	if errors.Is(err, os.ErrNotExist) {
		return true, nil
	}
	if err == nil && len(entries) == 0 {
		return true, nil
	}
	if err == nil && agentCloneUsable(ctx, dir) {
		return false, nil
	}
	moved, moveErr := moveAside(dir, "replaced")
	if moveErr != nil {
		return false, fmt.Errorf("the agent workspace %s is not a usable clone and could not be moved aside, so it was left as it was: %w", dir, moveErr)
	}
	log.Printf("agent: workspace %s was not a usable clone; moved it to %s and cloning fresh", dir, moved)
	return true, nil
}

func agentCloneUsable(ctx context.Context, dir string) bool {
	if info, err := os.Lstat(filepath.Join(dir, ".git")); err != nil || !info.IsDir() {
		return false
	}
	top, err := agentGit(ctx, dir, "rev-parse", "--show-toplevel")
	if err != nil {
		return false
	}
	want, wantErr := filepath.EvalSymlinks(dir)
	got, gotErr := filepath.EvalSymlinks(top)
	return wantErr == nil && gotErr == nil && want == got
}

func reuseAgentWorkspace(ctx context.Context, dir, branch, start, node string) error {
	if _, err := agentGit(ctx, dir, "fetch", "--quiet", "--prune", "origin"); err != nil {
		return fmt.Errorf("could not fetch into the agent workspace %s, so it was left as it was: %w", dir, err)
	}
	saved, err := saveAgentWorkspace(ctx, dir, branch, node)
	if err != nil {
		return fmt.Errorf("could not save the unpushed work in agent workspace %s, so it was left as it was and the job did not run: %w", dir, err)
	}
	for _, ref := range saved {
		log.Printf("agent: saved unpushed work in %s as %s", dir, ref)
	}
	pruneAgentTrash(ctx, dir, saved)
	if _, err := agentGit(ctx, dir, "checkout", "--force", "--quiet", "-B", branch, start); err != nil {
		return err
	}
	_, err = agentGit(ctx, dir, "clean", "-fdq")
	return err
}

func agentTrashPrefix(dir string) string {
	return agentTrashRefRoot + filepath.Base(dir) + "/"
}

func agentCommit(ctx context.Context, dir, ref string) string {
	value, err := agentGit(ctx, dir, "rev-parse", "--verify", "-q", ref+"^{commit}")
	if err != nil {
		return ""
	}
	return value
}

func agentUnpushed(ctx context.Context, dir, commit string) (bool, error) {
	count, err := agentGit(ctx, dir, "rev-list", "--count", commit, "--not", "--remotes")
	if err != nil {
		return false, err
	}
	return count != "0", nil
}

func agentTrashRefs(ctx context.Context, dir string) ([]string, error) {
	output, err := agentGit(ctx, dir, "for-each-ref", "--format=%(refname)", agentTrashPrefix(dir))
	if err != nil {
		return nil, err
	}
	if output == "" {
		return nil, nil
	}
	return strings.Split(output, "\n"), nil
}

func agentTrashStamp(existing []string, prefix string, now time.Time) string {
	taken := map[string]bool{}
	for _, ref := range existing {
		taken[ref] = true
	}
	base := strconv.FormatInt(now.Unix(), 10)
	stamp := base
	for counter := 2; taken[prefix+stamp] || taken[prefix+stamp+"-wip"] || taken[prefix+stamp+"-branch"]; counter++ {
		stamp = base + "-" + strconv.Itoa(counter)
	}
	return stamp
}

func saveAgentWorkspace(ctx context.Context, dir, branch, node string) ([]string, error) {
	existing, err := agentTrashRefs(ctx, dir)
	if err != nil {
		return nil, err
	}
	prefix := agentTrashPrefix(dir)
	stamp := agentTrashStamp(existing, prefix, time.Now())
	var saved []string
	keep := func(ref, commit string) error {
		if _, err := agentGit(ctx, dir, "update-ref", ref, commit, ""); err != nil {
			return err
		}
		if agentCommit(ctx, dir, ref) != commit {
			return fmt.Errorf("%s did not read back as %s", ref, commit)
		}
		saved = append(saved, ref)
		return nil
	}
	head := agentCommit(ctx, dir, "HEAD")
	if head != "" {
		unpushed, err := agentUnpushed(ctx, dir, head)
		if err != nil {
			return nil, err
		}
		if unpushed {
			if err := keep(prefix+stamp, head); err != nil {
				return nil, err
			}
		}
	}
	if local := agentCommit(ctx, dir, "refs/heads/"+branch); local != "" && local != head {
		unpushed, err := agentUnpushed(ctx, dir, local)
		if err != nil {
			return nil, err
		}
		if unpushed {
			if err := keep(prefix+stamp+"-branch", local); err != nil {
				return nil, err
			}
		}
	}
	status, err := agentGit(ctx, dir, "status", "--porcelain", "--untracked-files=all")
	if err != nil {
		return nil, err
	}
	if status != "" {
		wip, err := agentWipCommit(ctx, dir, head, node)
		if err != nil {
			return nil, err
		}
		if err := keep(prefix+stamp+"-wip", wip); err != nil {
			return nil, err
		}
	}
	return saved, nil
}

func agentWipCommit(ctx context.Context, dir, head, node string) (string, error) {
	file, err := os.CreateTemp(filepath.Join(dir, ".git"), "forkmesh-wip-index-*")
	if err != nil {
		return "", err
	}
	index := file.Name()
	file.Close()
	if err := os.Remove(index); err != nil {
		return "", err
	}
	defer os.Remove(index)
	name := strings.TrimSpace(node)
	if name == "" {
		name = "forkmesh-agent"
	}
	email := commitIdentityEmail(name)
	name = commitIdentityName(name)
	env := []string{"GIT_INDEX_FILE=" + index,
		"GIT_AUTHOR_NAME=" + name, "GIT_AUTHOR_EMAIL=" + email,
		"GIT_COMMITTER_NAME=" + name, "GIT_COMMITTER_EMAIL=" + email}
	if head != "" {
		if _, err := agentGitEnv(ctx, dir, env, "read-tree", head); err != nil {
			return "", err
		}
	}
	if _, err := agentGitEnv(ctx, dir, env, "add", "-A"); err != nil {
		return "", err
	}
	tree, err := agentGitEnv(ctx, dir, env, "write-tree")
	if err != nil {
		return "", err
	}
	args := []string{"commit-tree", tree, "-m", "Agent workspace snapshot before the next job"}
	if head != "" {
		args = append(args, "-p", head)
	}
	return agentGitEnv(ctx, dir, env, args...)
}

type agentTrashEntry struct {
	ref     string
	stamp   int64
	counter int
}

func agentTrashOrder(prefix, ref string) agentTrashEntry {
	entry := agentTrashEntry{ref: ref, counter: 1}
	parts := strings.Split(strings.TrimPrefix(ref, prefix), "-")
	entry.stamp, _ = strconv.ParseInt(parts[0], 10, 64)
	if len(parts) > 1 {
		if counter, err := strconv.Atoi(parts[1]); err == nil {
			entry.counter = counter
		}
	}
	return entry
}

func pruneAgentTrash(ctx context.Context, dir string, fresh []string) {
	refs, err := agentTrashRefs(ctx, dir)
	if err != nil {
		log.Printf("agent: could not list trash refs in %s: %v", dir, err)
		return
	}
	prefix := agentTrashPrefix(dir)
	pinned := map[string]bool{}
	for _, ref := range fresh {
		pinned[ref] = true
	}
	entries := make([]agentTrashEntry, 0, len(refs))
	for _, ref := range refs {
		entries = append(entries, agentTrashOrder(prefix, ref))
	}
	sort.SliceStable(entries, func(i, j int) bool {
		a, b := entries[i], entries[j]
		if pinned[a.ref] != pinned[b.ref] {
			return pinned[a.ref]
		}
		if a.stamp != b.stamp {
			return a.stamp > b.stamp
		}
		if a.counter != b.counter {
			return a.counter > b.counter
		}
		return a.ref > b.ref
	})
	for index := agentTrashKeep; index < len(entries); index++ {
		if _, err := agentGit(ctx, dir, "update-ref", "-d", entries[index].ref); err != nil {
			log.Printf("agent: could not prune trash ref %s in %s: %v", entries[index].ref, dir, err)
		}
	}
}
