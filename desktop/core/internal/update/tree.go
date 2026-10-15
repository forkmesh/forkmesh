package update

import (
	"bufio"
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"time"
)

func formatByteSize(bytes int64) string {
	units := []string{"B", "KB", "MB", "GB", "TB"}
	size := float64(bytes)
	unit := 0
	for size >= 1024.0 && unit < 4 {
		size /= 1024.0
		unit++
	}
	if unit == 0 {
		return fmt.Sprintf("%d B", bytes)
	}
	return fmt.Sprintf("%.1f %s", size, units[unit])
}

func buildScratchDir(buildDir string) string {
	return buildDir + "/.forkmesh-tmp"
}

func rebuildFreeBytesRequired(buildDir string) int64 {
	if _, err := os.Stat(buildDir + "/CMakeFiles/forkmesh.dir"); err == nil {
		return 1 << 30
	}
	return 3 << 30
}

func freeBytesForPath(path string, free func(string) int64) int64 {
	probe := filepath.Clean(path)
	for probe != "" {
		if _, err := os.Stat(probe); err == nil {
			break
		}
		parent := filepath.Dir(probe)
		if parent == probe {
			break
		}
		probe = parent
	}
	return free(probe)
}

func updateFailureSummary(output string) string {
	if strings.Contains(output, "No space left on device") {
		return "the disk filled up during the build. Free space on the volume holding the build directory, then start the update again — nothing was replaced."
	}
	for _, line := range strings.Split(output, "\n") {
		trimmed := strings.TrimSpace(line)
		if strings.Contains(trimmed, "error:") || strings.HasPrefix(trimmed, "fatal:") {
			return utf16Left(trimmed, 300)
		}
	}
	tail := strings.TrimSpace(output)
	if units := utf16Len(tail); units > 300 {
		runes := []rune(tail)
		cut := len(runes)
		count := 0
		for cut > 0 && count < 300 {
			cut--
			if runes[cut] >= 0x10000 {
				count += 2
			} else {
				count++
			}
		}
		tail = string(runes[cut:])
	}
	if newline := strings.IndexByte(tail, '\n'); newline >= 0 && newline < len(tail)-1 {
		tail = tail[newline+1:]
	}
	return strings.TrimSpace(tail)
}

func cmakeCacheValue(cacheFile, key string) string {
	file, err := os.Open(cacheFile)
	if err != nil {
		return ""
	}
	defer file.Close()
	prefix := key + ":"
	scanner := bufio.NewScanner(file)
	scanner.Buffer(make([]byte, 64<<10), 4<<20)
	for scanner.Scan() {
		line := strings.TrimSpace(scanner.Text())
		if !strings.HasPrefix(line, prefix) {
			continue
		}
		equals := strings.IndexByte(line, '=')
		if equals < 0 {
			return ""
		}
		return line[equals+1:]
	}
	return ""
}

func canonical(path string) string {
	if path == "" {
		return ""
	}
	absolute, err := filepath.Abs(path)
	if err != nil {
		return ""
	}
	resolved, err := filepath.EvalSymlinks(absolute)
	if err != nil {
		return ""
	}
	return resolved
}

func cmakeCacheMatches(clientDir, buildDir, buildType, generator string) bool {
	cache := buildDir + "/CMakeCache.txt"
	if _, err := os.Stat(cache); err != nil {
		return false
	}
	if generator != "" && cmakeCacheValue(cache, "CMAKE_GENERATOR") != generator {
		return false
	}
	if cmakeCacheValue(cache, "CMAKE_BUILD_TYPE") != buildType {
		return false
	}
	source := cmakeCacheValue(cache, "CMAKE_HOME_DIRECTORY")
	return source != "" && canonical(source) != "" && canonical(source) == canonical(clientDir)
}

func cmakeGeneratorEntriesToDrop(buildDir, generator string) []string {
	cache := buildDir + "/CMakeCache.txt"
	if generator == "" {
		return nil
	}
	if _, err := os.Stat(cache); err != nil || cmakeCacheValue(cache, "CMAKE_GENERATOR") == generator {
		return nil
	}
	var entries []string
	for _, name := range []string{"CMakeCache.txt", "CMakeFiles", "Makefile", "cmake_install.cmake",
		"build.ninja", ".ninja_deps", ".ninja_log"} {
		path := buildDir + "/" + name
		if _, err := os.Lstat(path); err == nil {
			entries = append(entries, path)
		}
	}
	return entries
}

func cmakeConfigureArgs(clientDir, buildDir, buildType, generator string, prefixes []string) []string {
	args := []string{"-S", clientDir, "-B", buildDir, "-DCMAKE_BUILD_TYPE=" + buildType, "-DFORKMESH_BUILD_TESTS=OFF"}
	if generator != "" {
		args = append(args, "-G", generator)
	}
	return append(args, prefixes...)
}

func buildJobsForMemory(totalRAM, availableRAM int64, idealThreads int) int {
	const gib = int64(1) << 30
	const perJob = 2 * gib
	jobs := idealThreads
	if jobs < 1 {
		jobs = 1
	}
	if jobs > maxRebuildJobs {
		jobs = maxRebuildJobs
	}
	reserve := 6 * gib
	if totalRAM > 0 && totalRAM/2 < reserve {
		reserve = totalRAM / 2
	}
	atLeastOne := func(n int64) int {
		if n < 1 {
			return 1
		}
		return int(n)
	}
	if totalRAM > 0 {
		if cap := atLeastOne((totalRAM - reserve) / perJob); cap < jobs {
			jobs = cap
		}
	}
	if availableRAM > 0 {
		usable := availableRAM - reserve
		if usable < 0 {
			usable = 0
		}
		if cap := atLeastOne(usable / perJob); cap < jobs {
			jobs = cap
		}
	}
	if jobs < 1 {
		return 1
	}
	return jobs
}

const maxRebuildJobs = 12

func builtExecutablePath(buildDir string, darwin bool) string {
	if darwin {
		app := buildDir + "/ForkMesh.app/Contents/MacOS/ForkMesh"
		if _, err := os.Stat(app); err == nil {
			return app
		}
	}
	return buildDir + "/forkmesh"
}

func skippedSourceName(name string) bool {
	return strings.HasPrefix(name, ".") || name == "build" || strings.HasPrefix(name, "build-") || name == "tests"
}

func sourceChangedSince(clientDir string, since time.Time) string {
	root, err := filepath.Abs(clientDir)
	if err != nil {
		return ""
	}
	scan := func(dir string) string {
		info, err := os.Stat(dir)
		if err != nil || !info.IsDir() {
			return ""
		}
		found := ""
		_ = filepath.WalkDir(dir, func(path string, entry fs.DirEntry, err error) error {
			if err != nil {
				if entry != nil && entry.IsDir() && path != dir {
					return fs.SkipDir
				}
				return nil
			}
			if path == dir {
				return nil
			}
			if skippedSourceName(entry.Name()) {
				if entry.IsDir() {
					return fs.SkipDir
				}
				return nil
			}
			if entry.Type()&fs.ModeSymlink != 0 {
				return nil
			}
			info, err := entry.Info()
			if err != nil {
				return nil
			}
			if info.ModTime().After(since) {
				found = path
				return fs.SkipAll
			}
			return nil
		})
		return found
	}
	if changed := scan(root); changed != "" {
		return changed
	}
	return scan(filepath.Clean(filepath.Join(root, "..", "marketplace")))
}

func builtMirrorNodeCompanionPath(built string, darwin bool) string {
	return builtCompanionPath(built, "forkmesh-mirror-node", darwin)
}

func builtCompanionPath(built, name string, darwin bool) string {
	dir := filepath.Dir(built)
	executable := func(path string) bool {
		info, err := os.Stat(path)
		return err == nil && !info.IsDir() && info.Mode()&0o111 != 0
	}
	if candidate := filepath.Join(dir, name); executable(candidate) {
		return candidate
	}
	if darwin {
		up := filepath.Dir(filepath.Dir(filepath.Dir(dir)))
		if candidate := filepath.Join(up, name); executable(candidate) {
			return candidate
		}
	}
	return ""
}

func copyFile(source, target string) error {
	data, err := os.ReadFile(source)
	if err != nil {
		return err
	}
	file, err := os.OpenFile(target, os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0o755)
	if err != nil {
		return err
	}
	if _, err := file.Write(data); err != nil {
		file.Close()
		_ = os.Remove(target)
		return err
	}
	if err := file.Close(); err != nil {
		_ = os.Remove(target)
		return err
	}
	return os.Chmod(target, 0o755)
}

func installMirrorNodeCompanionFile(source, binDir string) bool {
	return installCompanionFile(source, binDir, "forkmesh-mirror-node")
}

func installCompanionFile(source, binDir, name string) bool {
	target := filepath.Join(binDir, name)
	if c := canonical(source); c != "" && c == canonical(target) {
		return true
	}
	bak := target + ".bak-update"
	_ = os.Remove(bak)
	if _, err := os.Lstat(target); err == nil {
		if os.Rename(target, bak) != nil {
			return false
		}
	}
	if copyFile(source, target) != nil {
		_ = os.Rename(bak, target)
		return false
	}
	return true
}

func coreBinaryName() string {
	if runtime.GOOS == "windows" {
		return "forkmesh-core.exe"
	}
	return "forkmesh-core"
}
