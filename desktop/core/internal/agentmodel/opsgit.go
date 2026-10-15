package agentmodel

import (
	"context"
	"io"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"time"

	"forkmesh.com/desktop-core/internal/git"
	"forkmesh.com/desktop-core/internal/qtjson"
)

type gitRun struct {
	ok       bool
	exitCode int
	out      string
	raw      []byte
}

func opsGit(dir string, args []string, timeout time.Duration) gitRun {
	if timeout <= 0 {
		timeout = 15 * time.Second
	}
	ctx, cancel := context.WithTimeout(context.Background(), timeout)
	defer cancel()
	result := git.Run(ctx, dir, args, git.Options{Timeout: timeout, NoLane: true})
	if result.TimedOut {
		return gitRun{exitCode: -1}
	}
	return gitRun{ok: result.ExitCode == 0 && result.OK, exitCode: result.ExitCode,
		out: fromU16(qtDecodeUTF8(result.Output)), raw: result.Output}
}

func agentDiffBase(localPath, base string) string {
	if opsGit(localPath, []string{"rev-parse", "--verify", "--quiet", base}, 0).ok {
		return base
	}
	return trimmed(opsGit(localPath, []string{"symbolic-ref", "--short", "HEAD"}, 0).out)
}

func agentBranchWorktree(localPath, branch string) string {
	list := opsGit(localPath, []string{"worktree", "list", "--porcelain"}, 0)
	if !list.ok {
		return ""
	}
	entries := parseWorktreePorcelain(list.out, maxWorktrees)
	for i := 1; i < len(entries); i++ {
		if entries[i].Branch == branch && entries[i].Path != "" {
			return entries[i].Path
		}
	}
	return ""
}

type diffRead struct {
	err         string
	base        string
	committed   []uint16
	uncommitted []uint16
	files       qtjson.Value
	untracked   []string
	worktree    string
	truncated   bool
}

func readAgentDiff(localPath, base, branch string) diffRead {
	read := diffRead{files: qtjson.NewArray()}
	into := agentDiffBase(localPath, base)
	read.base = into
	branchKnown := opsGit(localPath, []string{"rev-parse", "--verify", "--quiet", "refs/heads/" + branch}, 0).ok
	if into != "" && branchKnown {
		span := into + "..." + branch
		if numstat := opsGit(localPath, []string{"diff", "--numstat", span}, 20*time.Second); numstat.ok {
			read.files = numstatRows(numstat.out, maxDiffFiles)
		}
		if patch := opsGit(localPath, []string{"diff", "--no-color", "--no-ext-diff", span}, 30*time.Second); patch.ok {
			read.committed = capDiff(toU16(patch.out), maxDiffChars, &read.truncated)
		}
	}
	read.worktree = agentBranchWorktree(localPath, branch)
	if read.worktree != "" {
		if pending := opsGit(read.worktree, []string{"diff", "--no-color", "--no-ext-diff", "HEAD"}, 30*time.Second); pending.ok {
			read.uncommitted = capDiff(toU16(pending.out), maxDiffChars-len(read.committed), &read.truncated)
		}
		if status := opsGit(read.worktree, []string{"status", "--porcelain", "--untracked-files=all"}, 0); status.ok {
			read.untracked = untrackedPaths(status.out, maxUntracked)
		}
	}
	if !branchKnown && read.worktree == "" {
		read.err = "branch_gone"
	}
	return read
}

type imageRead struct {
	err  string
	data []byte
	size int64
}

func readGitImage(dir, spec string) imageRead {
	size := opsGit(dir, []string{"cat-file", "-s", spec}, 0)
	if !size.ok {
		return imageRead{err: "file_missing"}
	}
	read := imageRead{}
	read.size, _ = strconv.ParseInt(trimmed(size.out), 10, 64)
	if read.size > maxDiffImageBytes {
		return imageRead{err: "too_large", size: read.size}
	}
	blob := opsGit(dir, []string{"cat-file", "blob", spec}, 20*time.Second)
	if !blob.ok {
		return imageRead{err: "file_missing", size: read.size}
	}
	read.data = blob.raw
	return read
}

func canonicalPath(path string) string {
	resolved, err := filepath.EvalSymlinks(path)
	if err != nil {
		return ""
	}
	abs, err := filepath.Abs(resolved)
	if err != nil {
		return ""
	}
	return abs
}

func readWorktreeImage(worktree, path string) imageRead {
	root := canonicalPath(worktree)
	full := filepath.Join(worktree, path)
	target := canonicalPath(full)
	info, err := os.Stat(full)
	if root == "" || target == "" || err != nil || !info.Mode().IsRegular() ||
		!strings.HasPrefix(target, root+string(filepath.Separator)) {
		return imageRead{err: "file_missing"}
	}
	read := imageRead{size: info.Size()}
	if read.size > maxDiffImageBytes {
		read.err = "too_large"
		return read
	}
	file, err := os.Open(target)
	if err != nil {
		read.err = "file_missing"
		return read
	}
	defer file.Close()
	data, _ := io.ReadAll(io.LimitReader(file, maxDiffImageBytes+1))
	if len(data) > maxDiffImageBytes {
		read.err = "too_large"
		return read
	}
	read.data = data
	return read
}

func readAgentDiffImage(localPath, base, branch, path string, uncommitted, after bool) imageRead {
	head := "refs/heads/" + branch
	if !uncommitted {
		if after {
			return readGitImage(localPath, head+":"+path)
		}
		fork := opsGit(localPath, []string{"merge-base", agentDiffBase(localPath, base), head}, 0)
		if !fork.ok || trimmed(fork.out) == "" {
			return imageRead{err: "file_missing"}
		}
		return readGitImage(localPath, trimmed(fork.out)+":"+path)
	}
	worktree := agentBranchWorktree(localPath, branch)
	if worktree == "" {
		return imageRead{err: "file_missing"}
	}
	if after {
		return readWorktreeImage(worktree, path)
	}
	return readGitImage(worktree, "HEAD:"+path)
}

func readLandedMergeStats(localPath, into, branch string, newest bool, nowSecs int64) (mergeStats, string) {
	stats := mergeStats{Base: into, Commits: -1}
	landed := ""
	if localPath == "" || into == "" {
		return stats, landed
	}
	log := opsGit(localPath, []string{"log", "--merges", "--first-parent", "--diff-merges=first-parent", "--shortstat",
		"-n", "20", "--pretty=format:%x1e%H%x1f%s", into}, 0)
	if log.ok {
		for _, merge := range parseMergeShortstatLog(log.out) {
			merged := mergedBranchFromMergeSubject(merge.Subject)
			if !newest && merged != branch {
				continue
			}
			landed = merged
			stats.Commit = merge.SHA
			stats.Files = merge.Files
			stats.Insertions = merge.Insertions
			stats.Deletions = merge.Deletions
			if count := opsGit(localPath, []string{"rev-list", "--count", merge.SHA + "^1.." + merge.SHA}, 0); count.ok {
				stats.Commits = qtToInt(count.out)
			}
			break
		}
	}
	times := opsGit(localPath, []string{"log", "--merges", "--first-parent", "--pretty=format:%ct", into}, 0)
	if !times.ok {
		return stats, landed
	}
	timestamps := []int64{}
	for _, line := range splitSkipEmpty(times.out, "\n") {
		if value, err := strconv.ParseInt(trimmed(line), 10, 64); err == nil {
			timestamps = append(timestamps, value)
		}
	}
	stats.Velocity = velocityWindows(mergeVelocityForTimestamps(timestamps, nowSecs))
	return stats, landed
}

func mergeInto(localPath, base string) string {
	into := base
	if into == "" || !opsGit(localPath, []string{"rev-parse", "--verify", "--quiet", into}, 0).ok {
		into = trimmed(opsGit(localPath, []string{"symbolic-ref", "--short", "HEAD"}, 0).out)
	}
	return into
}

func worktreeScan(localPath string, sessions map[string]int) (string, qtjson.Value) {
	rows := qtjson.NewArray()
	list := opsGit(localPath, []string{"worktree", "list", "--porcelain"}, 0)
	if !list.ok {
		return "", rows
	}
	entries := parseWorktreePorcelain(list.out, maxWorktrees)
	base := ""
	if len(entries) > 0 {
		base = entries[0].Branch
	}
	for i, entry := range entries {
		if entry.Bare {
			continue
		}
		ahead, behind := 0, 0
		if i > 0 && base != "" && entry.Branch != "" {
			counts := opsGit(localPath, []string{"rev-list", "--left-right", "--count", base + "..." + entry.Branch},
				8*time.Second)
			if counts.ok {
				if left, right, ok := parseLeftRight(counts.out); ok {
					behind, ahead = left, right
				}
			}
		}
		rows.Append(worktreeRow(entry, i == 0, ahead, behind, sessions[entry.Branch]))
	}
	return base, rows
}
