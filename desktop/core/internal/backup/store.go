package backup

import (
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"time"
)

const (
	IntervalMs   = int64(60 * 60 * 1000)
	KeepDefault  = 24
	KeepMin      = 1
	KeepMax      = 240
	MaxSnapshots = 240

	filePrefix  = "forkmesh-backup-"
	fileSuffix  = ".tar.gz"
	stampLayout = "20060102-150405"
)

type Snapshot struct {
	Path     string
	FileName string
	Taken    time.Time
	Bytes    int64
}

func FileName(when time.Time) string {
	return filePrefix + when.Format(stampLayout) + fileSuffix
}

func TimestampFromName(name string) (time.Time, bool) {
	if !strings.HasPrefix(name, filePrefix) || !strings.HasSuffix(name, fileSuffix) {
		return time.Time{}, false
	}
	stamp := name[len(filePrefix) : len(name)-len(fileSuffix)]
	if len(stamp) != len(stampLayout) {
		return time.Time{}, false
	}
	taken, err := time.ParseInLocation(stampLayout, stamp, time.Local)
	if err != nil {
		return time.Time{}, false
	}
	return taken, true
}

func List(root string) []Snapshot {
	entries, err := os.ReadDir(root)
	if err != nil {
		return nil
	}
	absolute, _ := filepath.Abs(root)
	var snapshots []Snapshot
	for _, entry := range entries {
		if !entry.Type().IsRegular() {
			continue
		}
		taken, ok := TimestampFromName(entry.Name())
		if !ok {
			continue
		}
		info, err := entry.Info()
		if err != nil {
			continue
		}
		snapshots = append(snapshots, Snapshot{Path: filepath.Join(absolute, entry.Name()), FileName: entry.Name(),
			Taken: taken, Bytes: info.Size()})
	}
	sort.SliceStable(snapshots, func(i, j int) bool {
		if snapshots[i].Taken.Equal(snapshots[j].Taken) {
			return snapshots[i].FileName > snapshots[j].FileName
		}
		return snapshots[i].Taken.After(snapshots[j].Taken)
	})
	return snapshots
}

func ToPrune(snapshots []Snapshot, keep int) []Snapshot {
	floor := keep
	if floor < KeepMin {
		floor = KeepMin
	}
	if len(snapshots) <= floor {
		return nil
	}
	return snapshots[floor:]
}

func IsDue(last time.Time, now time.Time, intervalMs int64) bool {
	if last.IsZero() {
		return true
	}
	elapsed := now.Sub(last).Milliseconds()
	if elapsed < 0 {
		return false
	}
	return elapsed >= intervalMs
}

func ClampKeep(keep int) int {
	if keep < KeepMin {
		return KeepMin
	}
	if keep > KeepMax {
		return KeepMax
	}
	return keep
}

func exists(path string) bool {
	_, err := os.Stat(path)
	return err == nil
}

func ConfigArchiveTarArgs(archivePath, appDataDir, configFile string, excludeRoots []string) []string {
	appData, _ := filepath.Abs(appDataDir)
	appParent := filepath.Dir(appData)
	haveApp := exists(appData)
	haveConfig := configFile != "" && exists(configFile)
	if !haveApp && !haveConfig {
		return nil
	}
	args := []string{"-czf", archivePath}
	for _, root := range excludeRoots {
		if root == "" {
			continue
		}
		absolute, err := filepath.Abs(root)
		if err != nil {
			continue
		}
		rel, err := filepath.Rel(appParent, absolute)
		if err != nil || rel == "" || strings.HasPrefix(rel, "..") {
			continue
		}
		args = append(args, "--exclude="+filepath.ToSlash(rel))
	}
	if haveApp {
		args = append(args, "-C", appParent, filepath.Base(appData))
	}
	if haveConfig {
		absolute, _ := filepath.Abs(configFile)
		args = append(args, "-C", filepath.Dir(absolute), filepath.Base(absolute))
	}
	return args
}

func LocationLabel(root, dataRoot string) string {
	cleanRoot := filepath.ToSlash(filepath.Clean(root))
	cleanData := filepath.ToSlash(filepath.Clean(dataRoot))
	if cleanData != "" && cleanData != "." && strings.HasPrefix(cleanRoot, cleanData+"/") {
		relative := cleanRoot[len(cleanData)+1:]
		if relative != "" && !strings.Contains(relative, "/") {
			return "ForkMesh data › " + relative
		}
		return "ForkMesh data › " + filepath.Base(cleanRoot)
	}
	name := filepath.Base(cleanRoot)
	if name == "" || name == "." || name == "/" {
		return "Backups"
	}
	return name
}

func FailureCode(failure string) string {
	switch failure {
	case "":
		return ""
	case "tar_missing", "tar_failed", "nothing_to_back_up", "rename_failed":
		return failure
	}
	return "failed"
}

type StatusInput struct {
	Enabled     bool
	Keep        int
	Running     bool
	Location    string
	Snapshots   []Snapshot
	Failure     string
	FailureAtMs int64
	IntervalMs  int64
	NowMs       int64
}

func utf16Left(text string, units int) string {
	count := 0
	for index, r := range text {
		size := 1
		if r >= 0x10000 {
			size = 2
		}
		if count+size > units {
			return text[:index]
		}
		count += size
	}
	return text
}

func Status(input StatusInput) map[string]any {
	var lastAt, lastBytes, totalBytes int64
	count := 0
	for _, snapshot := range input.Snapshots {
		if count >= MaxSnapshots {
			break
		}
		count++
		bytes := snapshot.Bytes
		if bytes < 0 {
			bytes = 0
		}
		totalBytes += bytes
		taken := snapshot.Taken.UnixMilli()
		if taken > lastAt {
			lastAt = taken
			lastBytes = bytes
		}
	}
	failure := FailureCode(input.Failure)
	failed := failure != "" && input.FailureAtMs >= lastAt
	var result string
	switch {
	case input.Running:
		result = "running"
	case failed:
		result = "failed"
	case count == 0 && input.Enabled:
		result = "none"
	case count == 0:
		result = "off"
	default:
		result = "ok"
	}
	var nextDue int64
	if input.Enabled && input.IntervalMs > 0 {
		if lastAt > 0 {
			nextDue = lastAt + input.IntervalMs
			if input.NowMs > nextDue {
				nextDue = input.NowMs
			}
		} else {
			nextDue = input.NowMs
		}
	}
	out := map[string]any{
		"enabled":         input.Enabled,
		"keep":            input.Keep,
		"running":         input.Running,
		"result":          result,
		"location":        utf16Left(input.Location, 120),
		"count":           count,
		"totalBytes":      totalBytes,
		"lastBackupAtMs":  lastAt,
		"lastBackupBytes": lastBytes,
		"nextDueAtMs":     nextDue,
	}
	if failed {
		out["error"] = failure
		out["failedAtMs"] = input.FailureAtMs
	}
	return out
}

func FormattedDataSize(bytes int64) string {
	units := []string{"bytes", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB"}
	if bytes < 1024 {
		return fmt.Sprintf("%d bytes", bytes)
	}
	size := float64(bytes)
	unit := 0
	for size >= 1024 && unit < len(units)-1 {
		size /= 1024
		unit++
	}
	return fmt.Sprintf("%.2f %s", size, units[unit])
}
