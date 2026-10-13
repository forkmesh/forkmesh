package actions

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"time"
)

var gitBinary = "git"

func gitCommand(ctx context.Context, dir string, args ...string) *exec.Cmd {
	full := args
	if dir != "" {
		full = append([]string{"-C", dir}, args...)
	}
	cmd := exec.CommandContext(ctx, gitBinary, full...)
	cmd.Env = os.Environ()
	configureCapture(cmd)
	return cmd
}

func gitCapture(dir string, timeout time.Duration, args ...string) ([]byte, bool) {
	ctx, cancel := context.WithTimeout(context.Background(), timeout)
	defer cancel()
	cmd := gitCommand(ctx, dir, args...)
	var stdout bytes.Buffer
	cmd.Stdout = &stdout
	cmd.Stderr = io.Discard
	if err := cmd.Run(); err != nil {
		return nil, false
	}
	return stdout.Bytes(), true
}

func gitCaptureMerged(dir string, timeout time.Duration, args ...string) ([]byte, int) {
	ctx, cancel := context.WithTimeout(context.Background(), timeout)
	defer cancel()
	cmd := gitCommand(ctx, dir, args...)
	var output bytes.Buffer
	cmd.Stdout = &output
	cmd.Stderr = &output
	err := cmd.Run()
	if err == nil {
		return output.Bytes(), 0
	}
	if exit, ok := err.(*exec.ExitError); ok && exit.ExitCode() >= 0 {
		return output.Bytes(), exit.ExitCode()
	}
	return output.Bytes(), -1
}

func WorkflowContentAt(mirror, commit, workflowPath string) (string, bool) {
	out, ok := gitCapture(mirror, 30*time.Second, "show", commit+":"+workflowPath)
	if !ok {
		return "", false
	}
	return qtFromUtf8(out), true
}

type digestMemo struct {
	mu      sync.Mutex
	entries map[string][2]string
	order   []string
}

var stateMemo = &digestMemo{entries: map[string][2]string{}}

func memoKey(repository, commit string) string {
	absolute, err := filepath.Abs(repository)
	if err != nil {
		absolute = repository
	}
	return absolute + "\x00" + strings.ToLower(qtTrim(commit))
}

func (m *digestMemo) get(key string) ([2]string, bool) {
	m.mu.Lock()
	defer m.mu.Unlock()
	value, ok := m.entries[key]
	return value, ok
}

func (m *digestMemo) remember(key, tree, digest string) {
	m.mu.Lock()
	defer m.mu.Unlock()
	if _, ok := m.entries[key]; ok {
		return
	}
	m.entries[key] = [2]string{tree, digest}
	m.order = append(m.order, key)
	for len(m.order) > 64 {
		delete(m.entries, m.order[0])
		m.order = m.order[1:]
	}
}

var digestArchiveTimeout = 5 * time.Minute

func RepositoryStateDigest(repository, commit string) (string, string, string) {
	if qtTrim(repository) == "" || qtTrim(commit) == "" {
		return "", "", "repository and commit are required"
	}
	key := memoKey(repository, commit)
	if hit, ok := stateMemo.get(key); ok {
		return hit[0], hit[1], ""
	}
	treeBytes, ok := gitCapture(repository, 10*time.Second, "rev-parse", "--verify", commit+"^{tree}")
	if !ok {
		return "", "", "could not resolve the repository tree"
	}
	tree := strings.ToLower(qtTrim(string(treeBytes)))
	if !objectIDPattern.MatchString(tree) {
		return "", "", "repository returned an invalid tree id"
	}
	manifest, ok := gitCapture(repository, 120*time.Second, "ls-tree", "-r", "-z", "--full-tree", commit)
	if !ok {
		return "", "", "could not read the recursive repository tree"
	}
	hash := sha256.New()
	hash.Write([]byte("forkmesh-action-snapshot-v2\x00"))
	hash.Write([]byte(tree))
	hash.Write([]byte("\x00tree-manifest\x00"))
	hash.Write(manifest)
	hash.Write([]byte("\x00archive\x00"))
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	cmd := gitCommand(ctx, repository, "archive", "--format=tar", commit)
	cmd.Stdout = hash
	cmd.Stderr = io.Discard
	if err := cmd.Start(); err != nil {
		return "", "", "could not start repository snapshot hashing"
	}
	done := make(chan error, 1)
	go func() { done <- cmd.Wait() }()
	timer := time.NewTimer(digestArchiveTimeout)
	defer timer.Stop()
	select {
	case err := <-done:
		if err != nil {
			return "", "", "could not archive the repository snapshot"
		}
	case <-timer.C:
		cancel()
		<-done
		return "", "", "repository snapshot hashing timed out"
	}
	execution := hex.EncodeToString(hash.Sum(nil))
	stateMemo.remember(key, tree, execution)
	return tree, execution, ""
}

func IsKnownPushBase(before string) bool {
	value := strings.ToLower(qtTrim(before))
	return objectIDPattern.MatchString(value) && strings.Count(value, "0") != len(value)
}

func IsMetadataPath(p string) bool {
	return strings.HasPrefix(p, ".forkmesh/issues/") || strings.HasPrefix(p, ".forkmesh/pulls/") || strings.HasPrefix(p, ".forkmesh/commits/")
}

func IsMetadataOnlyChange(paths []string) bool {
	if len(paths) == 0 {
		return false
	}
	for _, p := range paths {
		if !IsMetadataPath(p) {
			return false
		}
	}
	return true
}

type PushRangeGit func(args []string) (string, bool)

func splitNonEmpty(text, sep string) []string {
	var out []string
	for _, part := range strings.Split(text, sep) {
		if part != "" {
			out = append(out, part)
		}
	}
	return out
}

func appendUnique(into []string, seen map[string]bool, paths []string) []string {
	for _, p := range paths {
		if !seen[p] {
			seen[p] = true
			into = append(into, p)
		}
	}
	return into
}

func tipParentsUnion(git PushRangeGit, after string) []string {
	parentsLine, ok := git([]string{"rev-list", "--parents", "-n", "1", after, "--"})
	if !ok {
		return nil
	}
	parents := splitNonEmpty(qtTrim(parentsLine), " ")
	if len(parents) > 0 {
		parents = parents[1:]
	}
	var changed []string
	seen := map[string]bool{}
	if len(parents) == 0 {
		if files, ok := git([]string{"diff-tree", "--no-commit-id", "--name-only", "-r", "--root", after}); ok {
			changed = appendUnique(changed, seen, splitNonEmpty(files, "\n"))
		}
		return changed
	}
	for _, parent := range parents {
		if files, ok := git([]string{"diff-tree", "--no-commit-id", "--name-only", "-r", parent, after}); ok {
			changed = appendUnique(changed, seen, splitNonEmpty(files, "\n"))
		}
	}
	return changed
}

func PushRangeChangedPathsWith(git PushRangeGit, before, after string) []string {
	tip := qtTrim(after)
	if tip == "" || strings.HasPrefix(tip, "-") {
		return nil
	}
	base := strings.ToLower(qtTrim(before))
	if IsKnownPushBase(base) && base != strings.ToLower(tip) {
		if _, ok := git([]string{"merge-base", "--is-ancestor", base, tip}); ok {
			if files, ok := git([]string{"diff", "--no-ext-diff", "--no-renames", "--name-only", base, tip, "--"}); ok {
				return splitNonEmpty(files, "\n")
			}
		}
	}
	return tipParentsUnion(git, tip)
}

func PushRangeChangedPaths(repoPath, before, after string) []string {
	return PushRangeChangedPathsWith(func(args []string) (string, bool) {
		out, ok := gitCapture(repoPath, 30*time.Second, args...)
		if !ok {
			return "", false
		}
		return qtFromUtf8(out), true
	}, before, after)
}
