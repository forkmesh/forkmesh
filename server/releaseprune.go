package mirrornode

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"log"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"slices"
	"strconv"
	"strings"
	"time"
)

const (
	defaultReleasePruneInterval = 6 * time.Hour
	defaultReleasePruneMinAge   = 7 * 24 * time.Hour
	releasePruneFirstDelay      = 5 * time.Minute
	releasePruneLimit           = 200
)

var releaseShardPattern = regexp.MustCompile(`^[0-9a-f]{2}$`)

type releasePruneResult struct {
	Referenced int
	Deleted    int
	Bytes      int64
}

func (c Config) releasePruneInterval() time.Duration {
	if c.ReleasePruneInterval != nil && c.ReleasePruneInterval.Duration > 0 {
		return c.ReleasePruneInterval.Duration
	}
	return defaultReleasePruneInterval
}

func (c Config) releasePruneMinAge() time.Duration {
	if c.ReleasePruneMinAge != nil && c.ReleasePruneMinAge.Duration > 0 {
		return c.ReleasePruneMinAge.Duration
	}
	return defaultReleasePruneMinAge
}

func (c Config) validateReleasePrune() error {
	if c.ReleasePruneInterval != nil {
		value := c.ReleasePruneInterval.Duration
		if value < 10*time.Minute || value > 7*24*time.Hour {
			return errors.New("releasePruneInterval must be between 10m and 168h")
		}
	}
	if c.ReleasePruneMinAge != nil && c.ReleasePruneMinAge.Duration < 24*time.Hour {
		return errors.New("releasePruneMinAge must be at least 24h")
	}
	return nil
}

func (d *Daemon) pruneReleaseStores(parent context.Context) {
	ctx, cancel := context.WithTimeout(parent, d.config.SyncTimeout.Duration)
	defer cancel()
	gitDirs := map[string][]string{}
	labels := map[string]string{}
	var stores []string
	for _, repo := range append([]Repository(nil), d.gateway.Repositories...) {
		if strings.TrimSpace(repo.ReleaseStore) == "" || strings.TrimSpace(repo.GitDir) == "" {
			continue
		}
		store := filepath.Clean(repo.ReleaseStore)
		if _, seen := gitDirs[store]; !seen {
			stores = append(stores, store)
			labels[store] = repositoryKey(repo)
		}
		if !slices.Contains(gitDirs[store], repo.GitDir) {
			gitDirs[store] = append(gitDirs[store], repo.GitDir)
		}
	}
	for _, store := range stores {
		if ctx.Err() != nil {
			return
		}
		result, err := pruneReleaseStore(ctx, store, gitDirs[store],
			d.config.releasePruneMinAge(), releasePruneLimit, time.Now())
		if err != nil {
			log.Printf("release store prune %s: deleted %d blobs (%d bytes), kept the rest: %v",
				labels[store], result.Deleted, result.Bytes, err)
			continue
		}
		log.Printf("release store prune %s: deleted %d unreferenced blobs (%d bytes), %d referenced kept",
			labels[store], result.Deleted, result.Bytes, result.Referenced)
	}
}

func releaseReferencedBlobs(ctx context.Context, gitDir string, cutoff time.Time) (map[string]bool, error) {
	commit := gitValue(ctx, gitDir, "rev-parse", "--verify", "-q", "HEAD^{commit}")
	if !gitObjectIDPattern.MatchString(commit) {
		return nil, errors.New("HEAD does not resolve to a commit")
	}
	committed, err := strconv.ParseInt(gitValue(ctx, gitDir, "show", "-s", "--format=%ct", commit), 10, 64)
	if err != nil || committed <= 0 {
		return nil, errors.New("HEAD has no commit time")
	}
	if time.Unix(committed, 0).Before(cutoff) {
		return nil, errors.New("HEAD is older than the minimum blob age; the mirror may be behind")
	}
	out, err := exec.CommandContext(ctx, "git", "--git-dir="+gitDir, "ls-tree", "-r", "-z",
		"--name-only", commit, "--", ".forkmesh/releases").Output()
	if err != nil {
		return nil, fmt.Errorf("list release manifests: %w", err)
	}
	var paths, names []string
	for _, path := range strings.Split(string(out), "\x00") {
		parts := strings.Split(path, "/")
		if len(parts) == 4 && parts[0] == ".forkmesh" && parts[1] == "releases" &&
			parts[2] != "" && parts[3] == "release.json" {
			paths = append(paths, path)
			names = append(names, commit+":"+path)
		}
	}
	if len(names) == 0 {
		return nil, errors.New("no release manifests at HEAD")
	}
	blobs, err := gitBatchBlobs(ctx, gitDir, names)
	if err != nil {
		return nil, fmt.Errorf("read release manifests: %w", err)
	}
	referenced := map[string]bool{}
	for i, raw := range blobs {
		if raw == nil {
			return nil, fmt.Errorf("%s is unreadable", paths[i])
		}
		var manifest struct {
			Assets []struct {
				BlobSHA256 string `json:"blob_sha256"`
			} `json:"assets"`
		}
		if err := json.Unmarshal(raw, &manifest); err != nil {
			return nil, fmt.Errorf("%s does not parse: %w", paths[i], err)
		}
		for _, asset := range manifest.Assets {
			digest := strings.ToLower(strings.TrimSpace(asset.BlobSHA256))
			if !digestPattern.MatchString(digest) {
				return nil, fmt.Errorf("%s names a malformed blob_sha256", paths[i])
			}
			referenced[digest] = true
		}
	}
	return referenced, nil
}

func releaseBlobPresent(store, digest string) bool {
	info, err := os.Lstat(filepath.Join(store, "sha256", digest[:2], digest, "data"))
	return err == nil && info.Mode().IsRegular()
}

func releaseBlobExpired(dir string, cutoff time.Time) (int64, bool) {
	info, err := os.Lstat(dir)
	if err != nil || !info.IsDir() || info.ModTime().After(cutoff) {
		return 0, false
	}
	data, err := os.Lstat(filepath.Join(dir, "data"))
	if err != nil || !data.Mode().IsRegular() {
		return 0, false
	}
	entries, err := os.ReadDir(dir)
	if err != nil {
		return 0, false
	}
	var size int64
	for _, entry := range entries {
		entryInfo, err := os.Lstat(filepath.Join(dir, entry.Name()))
		if err != nil || entryInfo.ModTime().After(cutoff) {
			return 0, false
		}
		if entryInfo.Mode().IsRegular() {
			size += entryInfo.Size()
		}
	}
	return size, true
}

func pruneReleaseStore(ctx context.Context, store string, gitDirs []string,
	minAge time.Duration, limit int, now time.Time) (releasePruneResult, error) {
	var result releasePruneResult
	if strings.TrimSpace(store) == "" || len(gitDirs) == 0 {
		return result, errors.New("no release store or repository")
	}
	if minAge <= 0 {
		return result, errors.New("minimum blob age must be positive")
	}
	cutoff := now.Add(-minAge)
	referenced := map[string]bool{}
	for _, gitDir := range gitDirs {
		blobs, err := releaseReferencedBlobs(ctx, gitDir, cutoff)
		if err != nil {
			return result, err
		}
		for digest := range blobs {
			referenced[digest] = true
		}
	}
	if len(referenced) == 0 {
		return result, errors.New("release manifests reference no blobs")
	}
	result.Referenced = len(referenced)
	for digest := range referenced {
		if !releaseBlobPresent(store, digest) {
			return result, fmt.Errorf("referenced blob %s is missing from the store", digest)
		}
	}
	root := filepath.Join(store, "sha256")
	shards, err := os.ReadDir(root)
	if err != nil {
		return result, fmt.Errorf("read release store: %w", err)
	}
	for _, shard := range shards {
		if !releaseShardPattern.MatchString(shard.Name()) || !shard.IsDir() ||
			shard.Type()&os.ModeSymlink != 0 {
			continue
		}
		shardDir := filepath.Join(root, shard.Name())
		blobs, err := os.ReadDir(shardDir)
		if err != nil {
			continue
		}
		removed := false
		for _, blob := range blobs {
			if result.Deleted >= limit {
				return result, nil
			}
			if ctx.Err() != nil {
				return result, ctx.Err()
			}
			hash := blob.Name()
			if !digestPattern.MatchString(hash) || !strings.HasPrefix(hash, shard.Name()) ||
				!blob.IsDir() || blob.Type()&os.ModeSymlink != 0 || referenced[hash] {
				continue
			}
			blobDir := filepath.Join(shardDir, hash)
			size, expired := releaseBlobExpired(blobDir, cutoff)
			if !expired {
				continue
			}
			if err := removeTreeUnder(shardDir, blobDir); err != nil {
				return result, err
			}
			result.Deleted++
			result.Bytes += size
			removed = true
		}
		if removed {
			if remaining, err := os.ReadDir(shardDir); err == nil && len(remaining) == 0 {
				_ = os.Remove(shardDir)
			}
		}
	}
	return result, nil
}
