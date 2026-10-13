package git

import (
	"bufio"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"time"
)

func cleanAbsolute(path string) string {
	abs, err := filepath.Abs(path)
	if err != nil {
		return filepath.Clean(path)
	}
	return filepath.Clean(abs)
}

func canonicalOrClean(path string) string {
	if resolved, err := filepath.EvalSymlinks(path); err == nil {
		return cleanAbsolute(resolved)
	}
	return cleanAbsolute(path)
}

func pathInside(path, root string) bool {
	return root != "" && (path == root || strings.HasPrefix(path, root+string(filepath.Separator)))
}

func readFirstLine(path string) string {
	file, err := os.Open(path)
	if err != nil {
		return ""
	}
	defer file.Close()
	reader := bufio.NewReaderSize(file, 4096)
	line, _ := reader.ReadString('\n')
	if len(line) > 4096 {
		line = line[:4096]
	}
	return strings.TrimSpace(line)
}

func isDir(path string) bool {
	info, err := os.Stat(path)
	return err == nil && info.IsDir()
}

func isFile(path string) bool {
	info, err := os.Stat(path)
	return err == nil && info.Mode().IsRegular()
}

func exists(path string) bool {
	_, err := os.Stat(path)
	return err == nil
}

func GitDirForCheckout(dir string) string {
	if strings.TrimSpace(dir) == "" {
		return ""
	}
	at := cleanAbsolute(dir)
	for depth := 0; depth < 128; depth++ {
		dotGit := filepath.Join(at, ".git")
		if info, err := os.Stat(dotGit); err == nil {
			if info.IsDir() {
				return dotGit
			}
			if info.Mode().IsRegular() {
				line := readFirstLine(dotGit)
				if !strings.HasPrefix(line, "gitdir:") {
					return ""
				}
				target := strings.TrimSpace(line[len("gitdir:"):])
				if target == "" {
					return ""
				}
				if filepath.IsAbs(target) {
					return filepath.Clean(target)
				}
				return filepath.Clean(filepath.Join(at, target))
			}
		}
		if isFile(filepath.Join(at, "HEAD")) && (isDir(filepath.Join(at, "objects")) || isFile(filepath.Join(at, "commondir"))) {
			return at
		}
		parent := filepath.Dir(at)
		if parent == at {
			break
		}
		at = parent
	}
	return ""
}

func RepositoryKey(dir string) string {
	gitDir := GitDirForCheckout(dir)
	if gitDir == "" {
		return cleanAbsolute(dir)
	}
	if common := readFirstLine(filepath.Join(gitDir, "commondir")); common != "" {
		if filepath.IsAbs(common) {
			gitDir = common
		} else {
			gitDir = filepath.Join(gitDir, common)
		}
	}
	return canonicalOrClean(gitDir)
}

func LockContention(err string) bool {
	lower := strings.ToLower(err)
	if !strings.Contains(lower, "file exists") {
		return false
	}
	return strings.Contains(lower, ".lock") || strings.Contains(lower, "packed-refs.new")
}

var lockPathPattern = regexp.MustCompile(`(?i)Unable to create '([^'\n]+\.lock)'`)

func LockPathFromError(err string) string {
	match := lockPathPattern.FindStringSubmatch(err)
	if match == nil {
		return ""
	}
	return match[1]
}

func IndexWriteRefused(err string) bool {
	return strings.Contains(err, "Unable to write index.")
}

type LockState int

const (
	LockMissing LockState = iota
	LockHeld
	LockFresh
	LockStaleState
)

func (s LockState) String() string {
	switch s {
	case LockHeld:
		return "held"
	case LockFresh:
		return "fresh"
	case LockStaleState:
		return "stale"
	}
	return "missing"
}

func LockHeldByLiveProcess(lockPath string) bool {
	if lockPath == "" || !exists(lockPath) {
		return false
	}
	return lockHeld(lockPath)
}

func InspectLock(lockPath string, stale time.Duration) LockState {
	if lockPath == "" {
		return LockMissing
	}
	info, err := os.Stat(lockPath)
	if err != nil {
		return LockMissing
	}
	if LockHeldByLiveProcess(lockPath) {
		return LockHeld
	}
	if time.Since(info.ModTime()) >= stale {
		return LockStaleState
	}
	return LockFresh
}

func RemoveLockIfStale(lockPath string, stale time.Duration) bool {
	if filepath.Ext(lockPath) != ".lock" && filepath.Base(lockPath) != "packed-refs.new" {
		return false
	}
	if InspectLock(lockPath, stale) != LockStaleState {
		return false
	}
	info, err := os.Lstat(lockPath)
	if err != nil || !info.Mode().IsRegular() {
		return false
	}
	return os.Remove(lockPath) == nil
}

type Retry struct {
	Retry       bool
	Wait        time.Duration
	RemovedLock string
}

type RetryPolicy struct {
	Dir          string
	Stale        time.Duration
	backoff      int
	heldRetried  bool
	removedStale bool
}

func (p *RetryPolicy) Next(errText string) Retry {
	var decision Retry
	stale := p.Stale
	if stale <= 0 {
		stale = LockStale
	}
	lock := LockPathFromError(errText)
	if lock == "" && IndexWriteRefused(errText) && p.Dir != "" {
		gitDir := GitDirForCheckout(p.Dir)
		if gitDir == "" {
			return decision
		}
		lock = filepath.Join(gitDir, "index.lock")
		if !exists(lock) {
			return decision
		}
	} else if !LockContention(errText) {
		return decision
	}
	if filepath.Base(lock) == "index.lock" {
		switch InspectLock(lock, stale) {
		case LockHeld:
			if p.heldRetried {
				return decision
			}
			p.heldRetried = true
			decision.Retry = true
			decision.Wait = HeldLockWait
			return decision
		case LockStaleState:
			if !p.removedStale && RemoveLockIfStale(lock, stale) {
				p.removedStale = true
				decision.Retry = true
				decision.RemovedLock = lock
				return decision
			}
		}
	}
	if p.backoff+1 >= LockBackoffTries {
		return decision
	}
	decision.Retry = true
	decision.Wait = time.Duration(25<<p.backoff) * time.Millisecond
	p.backoff++
	return decision
}
