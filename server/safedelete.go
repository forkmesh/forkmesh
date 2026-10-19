package mirrornode

import (
	"fmt"
	"os"
	"os/user"
	"path/filepath"
	"strings"
	"time"
)

type deleteAllow int

const (
	deleteTree deleteAllow = iota
	deleteOwnWorktree
	deleteOwnClone
)

var safeDeleteProjectRoots = []string{"src", "data", "config", "ssh", "state", "logs",
	"data/core", "data/core/config", "data/core/ssh", "data/core/state", "data/core/logs"}

var safeDeleteSharedFolders = map[string]bool{
	"bin": true, "builds": true, "cache": true, "release": true, "tmp": true,
	"toolchain": true, "toolchain-windows": true, "worktrees": true,
}

var safeDeleteDisposable = map[string]bool{
	".cache": true, ".pytest_cache": true, ".tmp": true, ".worktrees": true, "__pycache__": true,
	"build": true, "builds": true, "cache": true, "caches": true, "dist": true,
	"node_modules": true, "out": true, "temp": true, "tmp": true, "worktrees": true,
}

func safeDeleteClean(path string) string {
	if strings.TrimSpace(path) == "" {
		return ""
	}
	absolute, err := filepath.Abs(path)
	if err != nil {
		return ""
	}
	absolute = filepath.Clean(absolute)
	if absolute == string(filepath.Separator) {
		return absolute
	}
	dir, name := filepath.Split(absolute)
	rest := []string{name}
	for dir = filepath.Clean(dir); ; dir = filepath.Dir(dir) {
		if resolved, err := filepath.EvalSymlinks(dir); err == nil {
			return filepath.Join(append([]string{resolved}, rest...)...)
		}
		parent := filepath.Dir(dir)
		if parent == dir {
			return absolute
		}
		rest = append([]string{filepath.Base(dir)}, rest...)
	}
}

func safeDeleteInside(child, parent string) bool {
	if child == parent || parent == "" {
		return false
	}
	return strings.HasPrefix(child, strings.TrimSuffix(parent, string(filepath.Separator))+string(filepath.Separator))
}

func safeDeleteWithin(child, parent string) bool {
	return child == parent || safeDeleteInside(child, parent)
}

func safeDeleteIsDisposable(name string) bool {
	return safeDeleteDisposable[name] || strings.HasPrefix(name, "build-") ||
		strings.HasPrefix(name, "build_") || strings.HasPrefix(name, "tmp-") ||
		strings.HasPrefix(name, "tmp.")
}

func safeDeleteHomes() []string {
	var homes []string
	candidates := []string{os.Getenv("HOME")}
	if current, err := user.Current(); err == nil {
		candidates = append(candidates, current.HomeDir)
	}
	for _, candidate := range candidates {
		cleaned := safeDeleteClean(candidate)
		if cleaned == "" || cleaned == string(filepath.Separator) {
			continue
		}
		seen := false
		for _, home := range homes {
			seen = seen || home == cleaned
		}
		if !seen {
			homes = append(homes, cleaned)
		}
	}
	return homes
}

type safeDeleteRoot struct {
	path string
	kind string
}

func safeDeleteProtectedRoots() []safeDeleteRoot {
	var roots []safeDeleteRoot
	add := func(path, kind string) {
		cleaned := safeDeleteClean(path)
		if cleaned == "" || cleaned == string(filepath.Separator) {
			return
		}
		roots = append(roots, safeDeleteRoot{cleaned, kind})
	}
	addProject := func(project string) {
		if filepath.Base(project) == "forkmesh-backup" {
			add(project, "frozen")
			return
		}
		add(project, "project")
		for _, sub := range safeDeleteProjectRoots {
			kind := "owned"
			if sub == "src" || sub == "ssh" || sub == "data/core/ssh" {
				kind = "strict"
			}
			add(filepath.Join(project, sub), kind)
		}
	}
	var projectsDirs []string
	for _, home := range safeDeleteHomes() {
		projectsDirs = append(projectsDirs, filepath.Join(home, "projects"))
		add(filepath.Join(home, ".ssh"), "strict")
		for _, legacy := range []string{".forkmesh", ".config/ForkMesh", ".local/share/ForkMesh", ".local/share/forkmesh"} {
			add(filepath.Join(home, legacy), "owned")
		}
	}
	if value := strings.TrimSpace(os.Getenv("FORKMESH_PROJECTS_DIR")); value != "" {
		projectsDirs = append(projectsDirs, value)
	}
	for _, variable := range []string{"FORKMESH_HOME", "FORKMESH_PROJECT_DIR"} {
		if value := strings.TrimSpace(os.Getenv(variable)); value != "" {
			addProject(value)
		}
	}
	for _, variable := range []string{"FORKMESH_DATA_DIR", "FORKMESH_CONFIG_DIR"} {
		if value := strings.TrimSpace(os.Getenv(variable)); value != "" {
			add(value, "owned")
		}
	}
	for _, projects := range projectsDirs {
		add(projects, "projects")
		cleaned := safeDeleteClean(projects)
		names := map[string]bool{"forkmesh": true, "forkmesh-backup": true}
		if entries, err := os.ReadDir(cleaned); err == nil {
			for _, entry := range entries {
				if entry.IsDir() {
					names[entry.Name()] = true
				}
			}
		}
		for name := range names {
			addProject(filepath.Join(cleaned, name))
		}
	}
	return roots
}

func safeDeleteRefusal(parent, target string, allow deleteAllow) string {
	if strings.TrimSpace(target) == "" {
		return "the path is empty"
	}
	path := safeDeleteClean(target)
	if path == string(filepath.Separator) {
		return "the path is /"
	}
	for _, home := range safeDeleteHomes() {
		if safeDeleteWithin(home, path) {
			return fmt.Sprintf("%s is HOME (%s) or contains it", path, home)
		}
	}
	if strings.TrimSpace(parent) == "" {
		return "no expected parent folder was given"
	}
	base := safeDeleteClean(parent)
	if base == string(filepath.Separator) {
		return "the expected parent folder is /"
	}
	if !safeDeleteInside(path, base) {
		return fmt.Sprintf("%s is not inside the expected parent %s", path, base)
	}
	parts := strings.Split(path, string(filepath.Separator))
	for index, part := range parts {
		if part != ".git" {
			continue
		}
		if index == len(parts)-1 {
			return fmt.Sprintf("%s is git history (.git)", path)
		}
		switch parts[index+1] {
		case "objects", "refs", "logs", "HEAD", "packed-refs":
			return fmt.Sprintf("%s is git history (.git)", path)
		}
	}
	if info, err := os.Lstat(filepath.Join(path, ".git")); err == nil {
		if info.IsDir() && allow != deleteOwnClone {
			return fmt.Sprintf("%s is a git checkout (it has a .git folder)", path)
		}
		if !info.IsDir() && allow == deleteTree {
			return fmt.Sprintf("%s is a git worktree; only the caller that created it may remove it", path)
		}
	}
	roots := safeDeleteProtectedRoots()
	for _, root := range roots {
		if safeDeleteWithin(root.path, path) {
			return fmt.Sprintf("%s is or contains protected ForkMesh folder %s", path, root.path)
		}
	}
	var innermost *safeDeleteRoot
	for index := range roots {
		if safeDeleteInside(path, roots[index].path) && (innermost == nil || len(roots[index].path) > len(innermost.path)) {
			innermost = &roots[index]
		}
	}
	if innermost == nil {
		return ""
	}
	rel := strings.Split(strings.TrimPrefix(path, innermost.path+string(filepath.Separator)), string(filepath.Separator))
	disposable := false
	for index, part := range rel {
		disposable = disposable || safeDeleteIsDisposable(part) || (part == ".forkmesh" && index < len(rel)-1)
	}
	switch innermost.kind {
	case "frozen":
		return fmt.Sprintf("%s is inside %s, which is never erased", path, innermost.path)
	case "project":
		if !safeDeleteSharedFolders[rel[0]] && !strings.HasPrefix(rel[0], "toolchain-") && !disposable {
			return fmt.Sprintf("%s is inside project folder %s but not in a cache, build or tmp folder", path, innermost.path)
		}
	case "strict", "projects":
		if !disposable {
			return fmt.Sprintf("%s is inside protected folder %s but not in a cache, build or tmp folder", path, innermost.path)
		}
	case "owned":
		if len(rel) < 2 && !disposable {
			return fmt.Sprintf("%s is a top-level folder of protected ForkMesh folder %s", path, innermost.path)
		}
	}
	return ""
}

func removeTreeUnder(parent, target string) error {
	return removeTreeAllowing(parent, target, deleteTree)
}

func removeOwnWorktreeUnder(parent, target string) error {
	return removeTreeAllowing(parent, target, deleteOwnWorktree)
}

func removeOwnCloneUnder(parent, target string) error {
	return removeTreeAllowing(parent, target, deleteOwnClone)
}

func removeTreeAllowing(parent, target string, allow deleteAllow) error {
	if reason := safeDeleteRefusal(parent, target, allow); reason != "" {
		return fmt.Errorf("refusing to delete: %s", reason)
	}
	return os.RemoveAll(safeDeleteClean(target))
}

func asideName(dir, name, label string, now time.Time) string {
	stamp := now.Format("20060102-150405")
	candidate := filepath.Join(dir, name+"."+label+"-"+stamp)
	for counter := 2; ; counter++ {
		if _, err := os.Lstat(candidate); err != nil {
			return candidate
		}
		candidate = filepath.Join(dir, fmt.Sprintf("%s.%s-%s-%d", name, label, stamp, counter))
	}
}

func moveAside(target, label string) (string, error) {
	path := safeDeleteClean(target)
	if path == "" || path == string(filepath.Separator) {
		return "", fmt.Errorf("refusing to move %q aside", target)
	}
	destination := asideName(filepath.Dir(path), filepath.Base(path), label, time.Now())
	if err := os.Rename(path, destination); err != nil {
		return "", err
	}
	return destination, nil
}
