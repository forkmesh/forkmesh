package actions

import (
	"context"
	"io"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"time"

	"forkmesh.com/desktop-core/internal/git"
)

var versionLine = regexp.MustCompile(`(?m)^(project\(ForkMesh VERSION )([0-9]+\.[0-9]+\.[0-9]+)`)

func visibleEntries(dir string) []os.DirEntry {
	entries, err := os.ReadDir(dir)
	if err != nil {
		return nil
	}
	var out []os.DirEntry
	for _, entry := range entries {
		if strings.HasPrefix(entry.Name(), ".") {
			continue
		}
		out = append(out, entry)
	}
	sort.Slice(out, func(i, j int) bool { return qtKeyLess(out[i].Name(), out[j].Name()) })
	return out
}

func copyFilePreservingMode(source, destination string) bool {
	input, err := os.Open(source)
	if err != nil {
		return false
	}
	defer input.Close()
	info, err := input.Stat()
	if err != nil {
		return false
	}
	output, err := os.OpenFile(destination, os.O_WRONLY|os.O_CREATE|os.O_EXCL, info.Mode().Perm())
	if err != nil {
		return false
	}
	if _, err := io.Copy(output, input); err != nil {
		_ = output.Close()
		_ = os.Remove(destination)
		return false
	}
	return output.Close() == nil
}

func copyReleaseTree(sourceDir, destinationDir string, copied *bool) {
	_ = os.MkdirAll(destinationDir, 0o777)
	for _, entry := range visibleEntries(sourceDir) {
		from := filepath.Join(sourceDir, entry.Name())
		to := destinationDir + "/" + entry.Name()
		if dirExists(from) {
			copyReleaseTree(from, to, copied)
			continue
		}
		_ = os.Remove(to)
		if copyFilePreservingMode(from, to) {
			*copied = true
		}
	}
}

func (r *runner) gitInWorkTree(args ...string) int {
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	result := git.Default.Run(ctx, r.repoWorkTree, args, git.Options{Timeout: 30 * time.Second})
	if result.TimedOut {
		return -1
	}
	return result.ExitCode
}

func (r *runner) landReleaseMetadata() bool {
	if r.repoWorkTree == "" || !dirExists(r.repoWorkTree) {
		return false
	}
	produced := r.workspace + "/.forkmesh/releases"
	if !dirExists(produced) {
		return false
	}
	destRoot := absolutePath(r.repoWorkTree + "/.forkmesh/releases")
	_ = os.MkdirAll(destRoot, 0o777)
	copied := false
	copyReleaseTree(produced, destRoot, &copied)
	if !copied {
		return false
	}
	paths := []string{".forkmesh/releases"}
	if r.landVersionHeader() {
		paths = append(paths, "desktop/CMakeLists.txt")
	}
	r.gitInWorkTree(append([]string{"add", "--"}, paths...)...)
	if r.gitInWorkTree(append([]string{"diff", "--cached", "--quiet", "--"}, paths...)...) == 0 {
		r.emitLog("==> ℹ️  Release metadata unchanged; continuing mirror publication.")
		return true
	}
	tag := ""
	if len(r.run.Ref) > len("refs/tags/") {
		tag = r.run.Ref[len("refs/tags/"):]
	}
	message := "release: publish " + tag + " artifacts"
	if containsString(paths, "desktop/CMakeLists.txt") {
		message = "release: publish " + tag + " artifacts, bump version header"
	}
	args := append([]string{"-c", "user.email=actions@forkmesh.local", "-c", "user.name=ForkMesh Actions", "commit", "-m", message, "--"}, paths...)
	if r.gitInWorkTree(args...) != 0 {
		r.emitLog("!! Could not commit release metadata into the working copy.")
		return false
	}
	r.emitLog("==> \U0001F4E6 Attached release metadata for " + tag + "; publishing…")
	return true
}

func (r *runner) landVersionHeader() bool {
	if r.repoWorkTree == "" || r.worktree == "" {
		return false
	}
	const rel = "desktop/CMakeLists.txt"
	source, err := os.ReadFile(r.workspace + "/" + rel)
	if err != nil {
		return false
	}
	want := versionLine.FindStringSubmatch(qtFromUtf8(stripCarriageReturns(source)))
	if want == nil {
		return false
	}
	version := want[2]
	destination := r.repoWorkTree + "/" + rel
	current, err := os.ReadFile(destination)
	if err != nil {
		return false
	}
	text := qtFromUtf8(stripCarriageReturns(current))
	have := versionLine.FindStringSubmatchIndex(text)
	if have == nil || text[have[4]:have[5]] == version {
		return false
	}
	text = text[:have[4]] + version + text[have[5]:]
	file, err := os.OpenFile(destination, os.O_WRONLY|os.O_TRUNC, 0)
	if err != nil {
		return false
	}
	_, werr := file.WriteString(text)
	cerr := file.Close()
	return werr == nil && cerr == nil
}
