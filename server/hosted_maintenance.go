package mirrornode

import (
	"bufio"
	"context"
	"errors"
	"fmt"
	"log"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"time"
)

const (
	hostedMaintenanceMarker  = "forkmesh-maintenance-due"
	hostedMaintenanceLock    = "forkmesh-maintenance.lock"
	hostedMaintenanceTick    = time.Minute
	hostedMaintenanceScan    = 15 * time.Minute
	hostedMaintenancePacks   = 4
	hostedMaintenanceLoose   = 256
	hostedMaintenanceTimeout = 30 * time.Minute
)

var hostedMaintenanceGitConfig = []string{
	"-c", "pack.threads=1",
	"-c", "pack.windowMemory=64m",
	"-c", "pack.deltaCacheSize=16m",
	"-c", "core.deltaBaseCacheLimit=16m",
	"-c", "core.packedGitWindowSize=16m",
	"-c", "core.packedGitLimit=256m",
	"-c", "gc.auto=0",
	"-c", "maintenance.auto=false",
}

var hostedRepositorySettings = [][2]string{
	{"receive.autogc", "false"},
	{"maintenance.auto", "false"},
}

var errHostedMaintenanceBusy = errors.New("repository is busy")

func hostedObjectCounts(gitDir string) (packs, loose int) {
	objects := filepath.Join(gitDir, "objects")
	if entries, err := os.ReadDir(filepath.Join(objects, "pack")); err == nil {
		for _, entry := range entries {
			name := entry.Name()
			if entry.Type().IsRegular() && strings.HasPrefix(name, "pack-") && strings.HasSuffix(name, ".pack") {
				packs++
			}
		}
	}
	entries, err := os.ReadDir(objects)
	if err != nil {
		return packs, loose
	}
	for _, entry := range entries {
		name := entry.Name()
		if !entry.IsDir() || len(name) != 2 || !isHexString(name) {
			continue
		}
		files, err := os.ReadDir(filepath.Join(objects, name))
		if err != nil {
			continue
		}
		for _, file := range files {
			if file.Type().IsRegular() && isHexString(file.Name()) {
				loose++
			}
		}
	}
	return packs, loose
}

func isHexString(value string) bool {
	if value == "" {
		return false
	}
	for _, r := range value {
		if !((r >= '0' && r <= '9') || (r >= 'a' && r <= 'f')) {
			return false
		}
	}
	return true
}

func hostedRepositoryBytes(ctx context.Context, gitDir string) int64 {
	output, err := exec.CommandContext(ctx, "git", "--git-dir="+gitDir, "count-objects", "-v").Output()
	if err != nil {
		return hostedDirectoryBytes(gitDir)
	}
	var kib int64
	found := false
	scanner := bufio.NewScanner(strings.NewReader(string(output)))
	for scanner.Scan() {
		key, value, ok := strings.Cut(scanner.Text(), ": ")
		if !ok || (key != "size" && key != "size-pack") {
			continue
		}
		number, err := strconv.ParseInt(strings.TrimSpace(value), 10, 64)
		if err != nil || number < 0 {
			return hostedDirectoryBytes(gitDir)
		}
		kib += number
		found = true
	}
	if !found {
		return hostedDirectoryBytes(gitDir)
	}
	return kib * 1024
}

func hostedMaintenanceDue(gitDir string, scan bool) bool {
	if _, err := os.Lstat(filepath.Join(gitDir, hostedMaintenanceMarker)); err == nil {
		return true
	}
	if !scan {
		return false
	}
	packs, loose := hostedObjectCounts(gitDir)
	return packs > hostedMaintenancePacks || loose > hostedMaintenanceLoose
}

func lowPriorityCommand(ctx context.Context, name string, args ...string) *exec.Cmd {
	var prefix []string
	if nice, err := exec.LookPath("nice"); err == nil {
		prefix = append(prefix, nice, "-n", "19")
	}
	if ionice, err := exec.LookPath("ionice"); err == nil {
		prefix = append(prefix, ionice, "-c", "3")
	}
	if len(prefix) == 0 {
		return exec.CommandContext(ctx, name, args...)
	}
	return exec.CommandContext(ctx, prefix[0], append(append(prefix[1:], name), args...)...)
}

func ensureHostedMaintenanceFiles(ctx context.Context, gitDir string) error {
	lock, err := os.OpenFile(filepath.Join(gitDir, hostedMaintenanceLock),
		os.O_CREATE|os.O_RDWR|syscall.O_NOFOLLOW, 0o600)
	if err != nil {
		return err
	}
	lock.Close()
	for _, setting := range hostedRepositorySettings {
		if gitValue(ctx, gitDir, "config", "--local", "--get", setting[0]) == setting[1] {
			continue
		}
		if output, err := exec.CommandContext(ctx, "git", "--git-dir="+gitDir, "config", "--local",
			setting[0], setting[1]).CombinedOutput(); err != nil {
			return fmt.Errorf("git config: %s", boundedText(output, 160))
		}
	}
	return nil
}

func maintainHostedRepository(ctx context.Context, gitDir string) error {
	if err := ensureHostedMaintenanceFiles(ctx, gitDir); err != nil {
		return err
	}
	lock, err := os.OpenFile(filepath.Join(gitDir, hostedMaintenanceLock),
		os.O_RDWR|syscall.O_NOFOLLOW, 0o600)
	if err != nil {
		return err
	}
	defer lock.Close()
	if err := syscall.Flock(int(lock.Fd()), syscall.LOCK_EX|syscall.LOCK_NB); err != nil {
		if errors.Is(err, syscall.EWOULDBLOCK) {
			return errHostedMaintenanceBusy
		}
		return err
	}
	defer syscall.Flock(int(lock.Fd()), syscall.LOCK_UN)
	if err := os.Remove(filepath.Join(gitDir, hostedMaintenanceMarker)); err != nil && !errors.Is(err, os.ErrNotExist) {
		return err
	}
	ctx, cancel := context.WithTimeout(ctx, hostedMaintenanceTimeout)
	defer cancel()
	args := append(append([]string(nil), hostedMaintenanceGitConfig...),
		"--git-dir="+gitDir, "repack", "-d", "-l", "-q", "--geometric=2")
	if output, err := lowPriorityCommand(ctx, "git", args...).CombinedOutput(); err != nil {
		_ = os.WriteFile(filepath.Join(gitDir, hostedMaintenanceMarker), nil, 0o600)
		return fmt.Errorf("git repack: %s", boundedText(output, 200))
	}
	return nil
}

func (r *hostedReplicator) hostedGitDirs() []string {
	entries, err := os.ReadDir(r.cfg.Root)
	if err != nil {
		return nil
	}
	var dirs []string
	for _, entry := range entries {
		name := entry.Name()
		if !entry.IsDir() || !strings.HasSuffix(name, ".git") ||
			!hostedIDPattern.MatchString(strings.TrimSuffix(name, ".git")) {
			continue
		}
		dirs = append(dirs, filepath.Join(r.cfg.Root, name))
	}
	return dirs
}

func (r *hostedReplicator) maintain(ctx context.Context, scan bool) int {
	maintained := 0
	for _, gitDir := range r.hostedGitDirs() {
		if ctx.Err() != nil {
			break
		}
		if scan {
			if err := ensureHostedMaintenanceFiles(ctx, gitDir); err != nil {
				log.Printf("hosted maintenance %s: %v", filepath.Base(gitDir), err)
				continue
			}
		}
		if !hostedMaintenanceDue(gitDir, scan) {
			continue
		}
		before := hostedRepositoryBytes(ctx, gitDir)
		started := time.Now()
		err := maintainHostedRepository(ctx, gitDir)
		if errors.Is(err, errHostedMaintenanceBusy) {
			continue
		}
		if err != nil {
			log.Printf("hosted maintenance %s: %v", filepath.Base(gitDir), err)
			continue
		}
		maintained++
		log.Printf("hosted maintenance %s: %d -> %d bytes in %s", filepath.Base(gitDir),
			before, hostedRepositoryBytes(ctx, gitDir), time.Since(started).Round(time.Millisecond))
	}
	return maintained
}

func (r *hostedReplicator) maintainLoop(ctx context.Context) {
	ticker := time.NewTicker(hostedMaintenanceTick)
	defer ticker.Stop()
	lastScan := time.Time{}
	for {
		now := time.Now()
		scan := now.Sub(lastScan) >= hostedMaintenanceScan
		if scan {
			lastScan = now
		}
		r.maintain(ctx, scan)
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
		}
	}
}
