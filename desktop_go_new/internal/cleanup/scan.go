package cleanup

import (
	"context"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"sync/atomic"
	"time"

	"forkmesh.com/desktop-core/internal/git"
)

type Category int

const (
	CategoryClaude Category = iota
	CategoryCodex
	CategoryWorktrees
	CategoryBuildTemp
	CategoryOtherCaches
)

type Status int

const (
	StatusInUse Status = iota
	StatusAttached
	StatusOrphaned
	StatusProtected
)

type Kind int

const (
	KindFiles Kind = iota
	KindWorktree
	KindActionSandbox
	KindReport
)

const (
	minuteMs = int64(60 * 1000)
	dayMs    = 24 * 60 * minuteMs
)

func CategoryName(category Category) string {
	switch category {
	case CategoryClaude:
		return "Claude Code"
	case CategoryCodex:
		return "Codex"
	case CategoryWorktrees:
		return "Agent worktrees"
	case CategoryBuildTemp:
		return "Builds and temp files"
	case CategoryOtherCaches:
		return "Other caches"
	}
	return ""
}

func StatusName(status Status) string {
	switch status {
	case StatusInUse:
		return "In use"
	case StatusAttached:
		return "Attached"
	case StatusOrphaned:
		return "Orphaned"
	case StatusProtected:
		return "Protected"
	}
	return ""
}

func Categories() []Category {
	return []Category{CategoryClaude, CategoryCodex, CategoryWorktrees, CategoryBuildTemp, CategoryOtherCaches}
}

type Member struct {
	Path  string `json:"path"`
	Bytes int64  `json:"bytes"`
}

type Item struct {
	ID          string   `json:"id"`
	Category    Category `json:"category"`
	Kind        Kind     `json:"kind"`
	Title       string   `json:"title"`
	Path        string   `json:"path"`
	Parent      string   `json:"parent"`
	Members     []Member `json:"members"`
	Bytes       int64    `json:"bytes"`
	KeptBytes   int64    `json:"keptBytes"`
	Files       int64    `json:"files"`
	ModifiedMs  int64    `json:"modifiedMs"`
	Status      Status   `json:"status"`
	Reasons     []string `json:"reasons"`
	ConfirmEach bool     `json:"confirmEach"`
	RepoPath    string   `json:"repoPath"`
	Branch      string   `json:"branch"`
	BaseBranch  string   `json:"baseBranch"`
	Ahead       int      `json:"ahead"`
	Behind      int      `json:"behind"`
	DirtyFiles  int      `json:"dirtyFiles"`
	Locked      bool     `json:"locked"`
	Agents      []string `json:"agents"`
	Tasks       []string `json:"tasks"`
}

func (item Item) ReasonText() string { return strings.Join(item.Reasons, " · ") }

func (item Item) Deletable() bool {
	if item.Status != StatusOrphaned && item.Status != StatusAttached {
		return false
	}
	if item.Kind == KindReport {
		return false
	}
	return item.Kind == KindWorktree || len(item.Members) > 0
}

func (item Item) NeedsConfirm() bool { return item.Status == StatusAttached || item.ConfirmEach }

type Process struct {
	PID       int64    `json:"pid"`
	Comm      string   `json:"comm"`
	Cwd       string   `json:"cwd"`
	Exe       string   `json:"exe"`
	Cmdline   []string `json:"cmdline"`
	OpenPaths []string `json:"openPaths"`
}

func fileName(path string) string {
	if index := strings.LastIndexAny(path, `/`+string(filepath.Separator)); index >= 0 {
		return path[index+1:]
	}
	return path
}

func (p Process) Label() string {
	name := p.Comm
	if name == "" {
		name = fileName(p.Exe)
	}
	if name == "" && len(p.Cmdline) > 0 {
		name = fileName(p.Cmdline[0])
	}
	if name == "" {
		name = "process"
	}
	return fmt.Sprintf("%s (PID %d)", name, p.PID)
}

type ProcessSnapshot struct {
	Processes []Process `json:"processes"`
	Available bool      `json:"available"`
}

func (s ProcessSnapshot) UsersOf(path string) []string {
	users := []string{}
	target := cleanPath(path)
	resolved := canonicalPath(path)
	hit := func(candidate string) bool {
		return candidate != "" && (PathWithin(candidate, target) || PathWithin(candidate, resolved))
	}
	for _, process := range s.Processes {
		uses := hit(process.Cwd) || hit(process.Exe)
		for i := 0; !uses && i < len(process.OpenPaths); i++ {
			uses = hit(process.OpenPaths[i])
		}
		if uses {
			users = append(users, process.Label())
		}
	}
	return users
}

func (s ProcessSnapshot) Named(names []string) []Process {
	if len(names) == 0 {
		return s.Processes
	}
	var found []Process
	for _, process := range s.Processes {
		if contains(names, process.Comm) || contains(names, fileName(process.Exe)) {
			found = append(found, process)
		}
	}
	return found
}

func contains(list []string, value string) bool {
	for _, entry := range list {
		if entry == value {
			return true
		}
	}
	return false
}

func readLink(path string) string {
	target, err := os.Readlink(path)
	if err != nil || target == "" {
		return ""
	}
	return strings.TrimSuffix(target, " (deleted)")
}

func readHead(path string, limit int64) []byte {
	file, err := os.Open(path)
	if err != nil {
		return nil
	}
	defer file.Close()
	buffer := make([]byte, limit)
	total := 0
	for int64(total) < limit {
		n, err := file.Read(buffer[total:])
		total += n
		if err != nil || n == 0 {
			break
		}
	}
	return buffer[:total]
}

func ReadProcesses(procRoot string) ProcessSnapshot {
	if procRoot == "" {
		procRoot = "/proc"
	}
	snapshot := ProcessSnapshot{Processes: []Process{}}
	entries, err := os.ReadDir(procRoot)
	if err != nil {
		return snapshot
	}
	for _, entry := range entries {
		if !isDirEntryDir(procRoot, entry) {
			continue
		}
		pid, err := strconv.ParseInt(entry.Name(), 10, 64)
		if err != nil {
			continue
		}
		base := filepath.Join(procRoot, entry.Name())
		process := Process{PID: pid}
		process.Cwd = readLink(base + "/cwd")
		process.Exe = readLink(base + "/exe")
		process.Comm = strings.TrimSpace(string(readHead(base+"/comm", 256)))
		for _, part := range strings.Split(string(readHead(base+"/cmdline", 64*1024)), "\x00") {
			if part != "" {
				process.Cmdline = append(process.Cmdline, part)
			}
		}
		if fds, err := os.ReadDir(base + "/fd"); err == nil {
			for _, fd := range fds {
				target := readLink(filepath.Join(base, "fd", fd.Name()))
				if strings.HasPrefix(target, "/") {
					process.OpenPaths = append(process.OpenPaths, target)
				}
			}
		}
		if process.Cwd == "" && process.Exe == "" && len(process.OpenPaths) == 0 && len(process.Cmdline) == 0 {
			continue
		}
		snapshot.Processes = append(snapshot.Processes, process)
	}
	snapshot.Available = len(snapshot.Processes) > 0
	return snapshot
}

func isDirEntryDir(parent string, entry os.DirEntry) bool {
	if entry.IsDir() {
		return true
	}
	if entry.Type()&os.ModeSymlink == 0 {
		return false
	}
	info, err := os.Stat(filepath.Join(parent, entry.Name()))
	return err == nil && info.IsDir()
}

func IsClaudeProcess(p Process) bool {
	if strings.Contains(p.Exe, "/claude/versions/") || p.Comm == "claude" || fileName(p.Exe) == "claude" {
		return true
	}
	return len(p.Cmdline) > 0 && fileName(p.Cmdline[0]) == "claude"
}

func IsCodexProcess(p Process) bool {
	if p.Comm == "codex" || fileName(p.Exe) == "codex" {
		return true
	}
	for i := 0; i < 2 && i < len(p.Cmdline); i++ {
		name := fileName(p.Cmdline[i])
		if name == "codex" || name == "codex.js" {
			return true
		}
	}
	return false
}

type Usage struct {
	Bytes    int64
	Files    int64
	NewestMs int64
}

func cancelled(cancel *atomic.Bool) bool { return cancel != nil && cancel.Load() }

type RepositoryRef struct {
	Name string `json:"name"`
	Path string `json:"path"`
}

type AgentRef struct {
	ID          int    `json:"id"`
	IssueNumber int    `json:"issueNumber"`
	Title       string `json:"title"`
	Status      string `json:"status"`
	RepoPath    string `json:"repoPath"`
	Branch      string `json:"branch"`
	Worktree    string `json:"worktree"`
	Live        bool   `json:"live"`
}

type TaskLink struct {
	TaskID     string `json:"taskId"`
	Title      string `json:"title"`
	Repository string `json:"repository"`
	Branch     string `json:"branch"`
	Worktree   string `json:"worktree"`
}

type Context struct {
	Home             string          `json:"home"`
	TmpRoots         []string        `json:"tmpRoots"`
	UID              string          `json:"uid"`
	ProjectHome      string          `json:"projectHome"`
	DataDir          string          `json:"dataDir"`
	BuildsDir        string          `json:"buildsDir"`
	WorktreesDir     string          `json:"worktreesDir"`
	SandboxDir       string          `json:"sandboxDir"`
	ClaudeBinary     string          `json:"claudeBinary"`
	CcacheDir        string          `json:"ccacheDir"`
	Repositories     []RepositoryRef `json:"repositories"`
	Agents           []AgentRef      `json:"agents"`
	Tasks            []TaskLink      `json:"tasks"`
	ClaimedSandboxes []string        `json:"claimedSandboxes"`
	NowMs            int64           `json:"nowMs"`
	LiveMinutes      int             `json:"liveMinutes"`
	OldSessionDays   int             `json:"oldSessionDays"`
	RunCcache        *bool           `json:"runCcache"`

	Processes ProcessSnapshot `json:"-"`
	Git       *git.Runner     `json:"-"`
	Ccache    string          `json:"-"`
}

func (c Context) liveMinutes() int {
	if c.LiveMinutes <= 0 {
		return 30
	}
	return c.LiveMinutes
}

func (c Context) oldSessionDays() int {
	if c.OldSessionDays <= 0 {
		return 30
	}
	return c.OldSessionDays
}

func (c Context) runCcache() bool { return c.RunCcache == nil || *c.RunCcache }

func (c Context) claimed(name string) bool { return contains(c.ClaimedSandboxes, name) }

type ScanResult struct {
	Items     []Item   `json:"items"`
	Notes     []string `json:"notes"`
	Cancelled bool     `json:"cancelled"`
}

func cleanPath(path string) string {
	if path == "" {
		return ""
	}
	absolute, err := filepath.Abs(path)
	if err != nil {
		return filepath.Clean(path)
	}
	return absolute
}

func canonicalPath(path string) string {
	if path != "" {
		if resolved, err := filepath.EvalSymlinks(cleanPath(path)); err == nil {
			return resolved
		}
	}
	return cleanPath(path)
}

func pathsMatch(a, b string) bool {
	if strings.TrimSpace(a) == "" || strings.TrimSpace(b) == "" {
		return false
	}
	return cleanPath(a) == cleanPath(b) || canonicalPath(a) == canonicalPath(b)
}

func lstat(path string) (os.FileInfo, bool) {
	info, err := os.Lstat(path)
	return info, err == nil
}

func isSymlink(path string) bool {
	info, ok := lstat(path)
	return ok && info.Mode()&os.ModeSymlink != 0
}

func isDir(path string) bool {
	info, err := os.Stat(path)
	return err == nil && info.IsDir()
}

func exists(path string) bool {
	_, err := os.Stat(path)
	return err == nil
}

type entry struct {
	name string
	path string
	link bool
	dir  bool
	file bool
}

func childEntries(dir string) []entry {
	names, err := readNames(dir)
	if err != nil {
		return nil
	}
	sort.Strings(names)
	out := make([]entry, 0, len(names))
	for _, name := range names {
		path := filepath.Join(dir, name)
		e := entry{name: name, path: path}
		if info, ok := lstat(path); ok {
			e.link = info.Mode()&os.ModeSymlink != 0
		}
		if info, err := os.Stat(path); err == nil {
			e.dir = info.IsDir()
			e.file = info.Mode().IsRegular()
		}
		out = append(out, e)
	}
	return out
}

func readNames(dir string) ([]string, error) {
	handle, err := os.Open(dir)
	if err != nil {
		return nil, err
	}
	defer handle.Close()
	return handle.Readdirnames(-1)
}

func childDirs(dir string) []entry {
	var dirs []entry
	for _, e := range childEntries(dir) {
		if e.dir && !e.link {
			dirs = append(dirs, e)
		}
	}
	return dirs
}

func filesByTime(dir, suffix string) []entry {
	type timed struct {
		entry
		ms int64
	}
	var found []timed
	for _, e := range childEntries(dir) {
		if !e.file || strings.HasPrefix(e.name, ".") || !strings.HasSuffix(strings.ToLower(e.name), suffix) {
			continue
		}
		info, err := os.Stat(e.path)
		if err != nil {
			continue
		}
		found = append(found, timed{e, info.ModTime().UnixMilli()})
	}
	sort.SliceStable(found, func(i, j int) bool { return found[i].ms > found[j].ms })
	out := make([]entry, len(found))
	for i := range found {
		out[i] = found[i].entry
	}
	return out
}

func modifiedMs(path string) int64 {
	info, err := os.Stat(path)
	if err != nil {
		return 0
	}
	return info.ModTime().UnixMilli()
}

func completeBaseName(name string) string {
	if index := strings.LastIndexByte(name, '.'); index >= 0 {
		return name[:index]
	}
	return name
}

type worktreeInfo struct {
	path       string
	head       string
	branch     string
	locked     bool
	lockReason string
	prunable   bool
}

type repoGit struct {
	ok         bool
	mainBranch string
	base       string
	entries    []worktreeInfo
}

func parseWorktrees(porcelain string) []worktreeInfo {
	var entries []worktreeInfo
	var current worktreeInfo
	open := false
	flush := func() {
		if open && current.path != "" {
			entries = append(entries, current)
		}
		current = worktreeInfo{}
		open = false
	}
	for _, raw := range strings.Split(porcelain, "\n") {
		line := strings.TrimSpace(raw)
		switch {
		case line == "":
			flush()
		case strings.HasPrefix(line, "worktree "):
			flush()
			current.path = line[9:]
			open = true
		case strings.HasPrefix(line, "HEAD "):
			current.head = line[5:]
		case strings.HasPrefix(line, "branch "):
			current.branch = strings.TrimPrefix(line[7:], "refs/heads/")
		case line == "locked" || strings.HasPrefix(line, "locked "):
			current.locked = true
			current.lockReason = strings.TrimSpace(line[6:])
		case line == "prunable" || strings.HasPrefix(line, "prunable "):
			current.prunable = true
		}
	}
	flush()
	return entries
}

func commonRepoFor(worktree string) string {
	marker := strings.TrimSpace(string(readHead(worktree+"/.git", 4096)))
	if !strings.HasPrefix(marker, "gitdir:") {
		return ""
	}
	gitdir := strings.TrimSpace(marker[7:])
	if !filepath.IsAbs(gitdir) {
		gitdir = filepath.Join(worktree, gitdir)
	}
	gitdir = filepath.Clean(gitdir)
	common := gitdir
	if pointer := strings.TrimSpace(string(readHead(gitdir+"/commondir", 4096))); pointer != "" {
		if filepath.IsAbs(pointer) {
			common = filepath.Clean(pointer)
		} else {
			common = filepath.Clean(filepath.Join(gitdir, pointer))
		}
	}
	if !isDir(common) {
		return ""
	}
	if fileName(common) == ".git" {
		return filepath.Dir(cleanPath(common))
	}
	return common
}

var cwdField = regexp.MustCompile(`"cwd"\s*:\s*"((?:[^"\\]|\\.)*)"`)

func transcriptCwd(projectDir string) string {
	for _, transcript := range filesByTime(projectDir, ".jsonl") {
		match := cwdField.FindSubmatch(readHead(transcript.path, 256*1024))
		if match == nil {
			continue
		}
		cwd := string(match[1])
		cwd = strings.ReplaceAll(cwd, `\/`, "/")
		cwd = strings.ReplaceAll(cwd, `\\`, `\`)
		return cwd
	}
	return ""
}

func projectMatchesCwd(dirName, cwd string) bool {
	encoded := EncodeClaudeProject(cwd)
	if dirName == encoded {
		return true
	}
	return len(encoded) > 120 && len(dirName) > 120 && dirName[:120] == encoded[:120]
}

func memberKey(name string) string { return strings.TrimSuffix(name, ".jsonl") }

func claudeProjectRoots(c Context) []string {
	roots := []string{c.Home + "/.claude/projects"}
	if c.DataDir != "" {
		for _, provider := range childDirs(c.DataDir + "/agent-accounts") {
			for _, account := range childDirs(provider.path) {
				roots = append(roots, account.path+"/projects")
			}
		}
	}
	return roots
}

func EncodeClaudeProject(cwd string) string {
	var out strings.Builder
	for _, r := range cwd {
		switch {
		case (r >= '0' && r <= '9') || (r >= 'a' && r <= 'z') || (r >= 'A' && r <= 'Z'):
			out.WriteRune(r)
		case r > 0xFFFF:
			out.WriteString("--")
		default:
			out.WriteByte('-')
		}
	}
	return out.String()
}

var uuidPattern = regexp.MustCompile(`[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}`)

func UUIDsIn(text string) map[string]bool {
	found := map[string]bool{}
	for _, match := range uuidPattern.FindAllString(text, -1) {
		found[match] = true
	}
	return found
}

func LiveClaudeSessions(c Context) map[string]bool {
	live := map[string]bool{}
	cwds := map[string]int{}
	var cwdOrder []string
	for _, process := range c.Processes.Processes {
		if !IsClaudeProcess(process) {
			continue
		}
		for uuid := range UUIDsIn(strings.Join(process.Cmdline, " ")) {
			live[uuid] = true
		}
		if process.Cwd != "" {
			if cwds[process.Cwd] == 0 {
				cwdOrder = append(cwdOrder, process.Cwd)
			}
			cwds[process.Cwd]++
		}
	}
	window := int64(c.liveMinutes()) * minuteMs
	for _, root := range claudeProjectRoots(c) {
		for _, project := range childDirs(root) {
			transcripts := filesByTime(project.path, ".jsonl")
			for _, transcript := range transcripts {
				if c.NowMs-modifiedMs(transcript.path) < window {
					live[completeBaseName(transcript.name)] = true
				}
			}
			for _, cwd := range cwdOrder {
				if !projectMatchesCwd(project.name, cwd) {
					continue
				}
				for i := 0; i < cwds[cwd] && i < len(transcripts); i++ {
					live[completeBaseName(transcripts[i].name)] = true
				}
			}
		}
	}
	return live
}

func PathWithin(path, root string) bool {
	if path == "" || root == "" {
		return false
	}
	if path == root {
		return true
	}
	prefix := root
	if !strings.HasSuffix(prefix, "/") {
		prefix += "/"
	}
	return strings.HasPrefix(path, prefix)
}

func SizeText(bytes int64) string {
	if bytes < 0 {
		bytes = 0
	}
	power := 0
	if bytes > 0 {
		bits := 0
		for value := uint64(bytes); value > 1; value >>= 1 {
			bits++
		}
		power = bits / 10
	}
	if power == 0 {
		return fmt.Sprintf("%d bytes", bytes)
	}
	units := []string{"KiB", "MiB", "GiB", "TiB", "PiB", "EiB"}
	value := float64(bytes)
	for i := 0; i < power; i++ {
		value /= 1024
	}
	precision := 1
	if 3*power < precision {
		precision = 3 * power
	}
	return strconv.FormatFloat(value, 'f', precision, 64) + " " + units[power-1]
}

func AgeText(modifiedMs, nowMs int64) string {
	if modifiedMs <= 0 {
		return "-"
	}
	minutes := (nowMs - modifiedMs) / minuteMs
	if minutes < 0 {
		minutes = 0
	}
	if minutes < 60 {
		return fmt.Sprintf("%d min", minutes)
	}
	if minutes < 48*60 {
		return fmt.Sprintf("%d h", minutes/60)
	}
	return fmt.Sprintf("%d d", minutes/(24*60))
}

func firstUsers(users []string) string {
	if len(users) > 3 {
		users = users[:3]
	}
	return strings.Join(users, ", ")
}

func plural(count int, suffix string) string {
	if count == 1 {
		return ""
	}
	return suffix
}

type keepFilter func(e entry, usage Usage) (string, bool)

type scanner struct {
	ctx           context.Context
	c             Context
	cancel        *atomic.Bool
	progress      func(string)
	result        ScanResult
	live          map[string]bool
	repos         map[string]*repoGit
	worktreeNames []string
	claudeRunning bool
	codexRunning  bool
}

func Scan(ctx context.Context, c Context, cancel *atomic.Bool, progress func(string)) ScanResult {
	if ctx == nil {
		ctx = context.Background()
	}
	if c.Git == nil {
		c.Git = &git.Runner{}
	}
	s := &scanner{ctx: ctx, c: c, cancel: cancel, progress: progress, repos: map[string]*repoGit{},
		result: ScanResult{Items: []Item{}, Notes: []string{}}}
	return s.run()
}

func (s *scanner) run() ScanResult {
	s.live = LiveClaudeSessions(s.c)
	for _, process := range s.c.Processes.Processes {
		s.claudeRunning = s.claudeRunning || IsClaudeProcess(process)
		s.codexRunning = s.codexRunning || IsCodexProcess(process)
	}
	steps := []struct {
		stage string
		run   func()
	}{
		{"Agent worktrees", s.scanWorktrees},
		{"Claude Code", s.scanClaude},
		{"Codex", s.scanCodex},
		{"Builds and temp files", s.scanBuilds},
		{"Other caches", s.scanOtherCaches},
	}
	for _, step := range steps {
		if s.stopped() {
			break
		}
		if s.progress != nil {
			s.progress(step.stage)
		}
		step.run()
	}
	s.result.Cancelled = s.stopped()
	return s.result
}

func (s *scanner) stopped() bool { return cancelled(s.cancel) || s.ctx.Err() != nil }

func (s *scanner) measure(path string) Usage { return Measure(path, s.cancel) }

func (s *scanner) isLiveName(name string) bool {
	for uuid := range UUIDsIn(name) {
		if s.live[uuid] {
			return true
		}
	}
	return false
}

func (s *scanner) recent(newestMs int64) bool {
	return newestMs > 0 && s.c.NowMs-newestMs < int64(s.c.liveMinutes())*minuteMs
}

func (s *scanner) recentText() string {
	return fmt.Sprintf("changed in the last %d minutes", s.c.liveMinutes())
}

func addMember(item *Item, path string, usage Usage) {
	item.Members = append(item.Members, Member{Path: path, Bytes: usage.Bytes})
	item.Bytes += usage.Bytes
	item.Files += usage.Files
	if usage.NewestMs > item.ModifiedMs {
		item.ModifiedMs = usage.NewestMs
	}
}

func keep(item *Item, usage Usage) { item.KeptBytes += usage.Bytes }

func makeItem(category Category, kind Kind, title, path, parent string) Item {
	item := Item{Category: category, Kind: kind, Title: title, Path: cleanPath(path), Parent: cleanPath(parent),
		Status: StatusOrphaned, Ahead: -1, Behind: -1, Members: []Member{}, Reasons: []string{},
		Agents: []string{}, Tasks: []string{}}
	item.ID = fmt.Sprintf("%d:%s:%s", int(category), title, item.Path)
	return item
}

func (s *scanner) dropInUseMembers(item *Item) {
	kept := []Member{}
	var users []string
	busy := 0
	for _, member := range item.Members {
		using := s.c.Processes.UsersOf(member.Path)
		if len(using) == 0 {
			kept = append(kept, member)
			continue
		}
		busy++
		item.Bytes -= member.Bytes
		item.KeptBytes += member.Bytes
		for _, user := range using {
			if !contains(users, user) {
				users = append(users, user)
			}
		}
	}
	if busy == 0 {
		return
	}
	item.Members = kept
	if len(kept) == 0 {
		item.Status = StatusInUse
		item.Reasons = append([]string{"open in " + firstUsers(users)}, item.Reasons...)
	} else {
		item.Reasons = append(item.Reasons, fmt.Sprintf("%d kept: open in %s", busy, firstUsers(users)))
	}
}

func (s *scanner) push(item Item, keepEmpty bool) {
	if item.Kind == KindFiles && item.Status != StatusProtected {
		s.dropInUseMembers(&item)
	}
	if !keepEmpty && len(item.Members) == 0 && item.Bytes <= 0 && item.KeptBytes <= 0 && item.Kind == KindFiles {
		return
	}
	if item.Kind == KindFiles && len(item.Members) == 0 && (item.Status == StatusOrphaned || item.Status == StatusAttached) {
		item.Status = StatusInUse
	}
	s.result.Items = append(s.result.Items, item)
}

func (s *scanner) membersOf(category Category, title, dir string, keepIt keepFilter) Item {
	item := makeItem(category, KindFiles, title, dir, dir)
	keptWhy := map[string]int{}
	var order []string
	for _, e := range childEntries(dir) {
		if s.stopped() {
			break
		}
		usage := s.measure(e.path)
		if keepIt != nil {
			if why, kept := keepIt(e, usage); kept {
				keep(&item, usage)
				if keptWhy[why] == 0 {
					order = append(order, why)
				}
				keptWhy[why]++
				continue
			}
		}
		addMember(&item, e.path, usage)
	}
	for _, why := range order {
		item.Reasons = append(item.Reasons, fmt.Sprintf("%d kept: %s", keptWhy[why], why))
	}
	return item
}

func (s *scanner) liveFilter() keepFilter {
	return func(e entry, usage Usage) (string, bool) {
		if s.isLiveName(e.name) {
			return "live session", true
		}
		if s.recent(usage.NewestMs) {
			return s.recentText(), true
		}
		return "", false
	}
}

func prepend(item *Item, reason string) {
	item.Reasons = append([]string{reason}, item.Reasons...)
}

func (s *scanner) scanClaude() {
	claude := s.c.Home + "/.claude"
	for _, project := range childDirs(claude + "/projects") {
		if s.stopped() {
			return
		}
		s.scanClaudeProject(project)
	}
	for _, name := range []string{"shell-snapshots", "todos", "debug", "session-env", "file-history", "paste-cache", "telemetry"} {
		dir := claude + "/" + name
		if !isDir(dir) || isSymlink(dir) {
			continue
		}
		item := s.membersOf(CategoryClaude, "Claude "+name, dir, s.liveFilter())
		prepend(&item, "per-session state of finished sessions")
		s.push(item, false)
	}
	for _, root := range s.c.TmpRoots {
		s.scanClaudeTemp(root + "/claude-" + s.c.UID)
	}
	cache := s.c.Home + "/.cache"
	for _, e := range childEntries(cache) {
		if !strings.HasPrefix(e.name, "claude") || e.link {
			continue
		}
		item := makeItem(CategoryClaude, KindFiles, e.name, e.path, cache)
		addMember(&item, e.path, s.measure(e.path))
		item.Reasons = append(item.Reasons, "Claude Code cache; rebuilt when needed")
		if s.recent(item.ModifiedMs) && s.claudeRunning {
			item.Status = StatusAttached
			item.Reasons = append(item.Reasons, s.recentText())
		}
		s.push(item, false)
	}
	s.scanClaudeVersions()
	s.scanAgentAccounts()
}

func (s *scanner) scanClaudeProject(project entry) {
	cwd := transcriptCwd(project.path)
	folderExists := cwd != "" && isDir(cwd)
	label := cwd
	if label == "" {
		label = project.name
	}
	old := makeItem(CategoryClaude, KindFiles, "Old transcripts · "+label, project.path, project.path)
	current := makeItem(CategoryClaude, KindFiles, "Transcripts · "+label, project.path, project.path)
	liveKept := 0
	memory := int64(0)
	for _, e := range childEntries(project.path) {
		if s.stopped() {
			return
		}
		usage := s.measure(e.path)
		if e.name == "memory" {
			memory += usage.Bytes
			continue
		}
		if s.live[memberKey(e.name)] || s.recent(usage.NewestMs) {
			liveKept++
			keep(&current, usage)
			continue
		}
		isOld := usage.NewestMs > 0 && s.c.NowMs-usage.NewestMs > int64(s.c.oldSessionDays())*dayMs
		if folderExists && !isOld {
			addMember(&current, e.path, usage)
		} else {
			addMember(&old, e.path, usage)
		}
	}
	var parts []string
	if liveKept > 0 {
		parts = append(parts, fmt.Sprintf("%d live session%s kept", liveKept, plural(liveKept, "s")))
	}
	if memory > 0 {
		parts = append(parts, "memory/ kept")
	}
	notes := strings.Join(parts, " · ")
	if folderExists {
		current.Status = StatusAttached
		current.Reasons = append(current.Reasons, fmt.Sprintf("folder %s still exists", cwd))
		old.Reasons = append(old.Reasons, fmt.Sprintf("sessions older than %d days", s.c.oldSessionDays()))
		if notes != "" {
			current.Reasons = append(current.Reasons, notes)
		}
		if len(old.Members) > 0 {
			s.push(old, false)
		}
		if len(current.Members) > 0 || current.KeptBytes > 0 {
			s.push(current, true)
		}
		return
	}
	old.Title = "Transcripts · " + label
	if cwd == "" {
		old.Reasons = append(old.Reasons, "no transcript names its folder")
	} else {
		old.Reasons = append(old.Reasons, fmt.Sprintf("folder %s is gone", cwd))
	}
	old.KeptBytes = current.KeptBytes
	if notes != "" {
		old.Reasons = append(old.Reasons, notes)
	}
	if len(old.Members) > 0 || old.KeptBytes > 0 {
		s.push(old, true)
	}
}

func (s *scanner) scanClaudeTemp(dir string) {
	if !isDir(dir) || isSymlink(dir) {
		return
	}
	misc := makeItem(CategoryClaude, KindFiles, "Claude temp files", dir, dir)
	kept := 0
	for _, e := range childEntries(dir) {
		if s.stopped() {
			return
		}
		if e.dir && !e.link && strings.HasPrefix(e.name, "-") {
			item := s.membersOf(CategoryClaude, "Scratchpads · "+e.name, e.path, s.liveFilter())
			prepend(&item, "scratchpads and task output of finished sessions")
			s.push(item, false)
			continue
		}
		usage := s.measure(e.path)
		if s.isLiveName(e.name) || s.recent(usage.NewestMs) || (s.claudeRunning && e.name == "bundled-skills") {
			kept++
			keep(&misc, usage)
			continue
		}
		addMember(&misc, e.path, usage)
	}
	misc.Reasons = append(misc.Reasons, "diff and cache files from finished sessions")
	if kept > 0 {
		misc.Reasons = append(misc.Reasons, fmt.Sprintf("%d kept for running sessions", kept))
	}
	s.push(misc, false)
}

func (s *scanner) scanClaudeVersions() {
	dir := s.c.Home + "/.local/share/claude/versions"
	current := ""
	if s.c.ClaudeBinary != "" {
		current = canonicalPath(s.c.ClaudeBinary)
	}
	for _, e := range childEntries(dir) {
		if e.link {
			continue
		}
		item := makeItem(CategoryClaude, KindFiles, "Claude Code "+e.name, e.path, dir)
		addMember(&item, e.path, s.measure(e.path))
		if current != "" && PathWithin(current, canonicalPath(e.path)) {
			item.Status = StatusProtected
			item.Reasons = append(item.Reasons, "the version `claude` runs")
			item.Members = []Member{}
		} else {
			item.Reasons = append(item.Reasons, "older Claude Code version")
		}
		s.push(item, true)
	}
}

func left(text string, count int) string {
	runes := []rune(text)
	if len(runes) <= count {
		return text
	}
	return string(runes[:count])
}

func (s *scanner) scanAgentAccounts() {
	if s.c.DataDir == "" {
		return
	}
	root := s.c.DataDir + "/agent-accounts"
	for _, provider := range childDirs(root) {
		for _, account := range childDirs(provider.path) {
			projects := account.path + "/projects"
			if !isDir(projects) {
				continue
			}
			category := CategoryClaude
			if provider.name == "codex" {
				category = CategoryCodex
			}
			item := makeItem(category, KindFiles, fmt.Sprintf("Desktop agent transcripts · %s %s", provider.name, left(account.name, 8)), projects, projects)
			usage := s.measure(projects)
			item.KeptBytes = usage.Bytes
			item.Files = usage.Files
			item.ModifiedMs = usage.NewestMs
			item.Status = StatusProtected
			item.Reasons = append(item.Reasons, "ForkMesh user data (agent history); never deleted here")
			s.push(item, true)
		}
	}
}

func (s *scanner) scanCodex() {
	codex := s.c.Home + "/.codex"
	if !isDir(codex) {
		return
	}
	sessions := codex + "/sessions"
	if isDir(sessions) {
		item := makeItem(CategoryCodex, KindFiles, "Codex sessions", sessions, sessions)
		type day struct {
			path  string
			usage Usage
		}
		var days []day
		for _, year := range childDirs(sessions) {
			for _, month := range childDirs(year.path) {
				for _, d := range childDirs(month.path) {
					days = append(days, day{d.path, s.measure(d.path)})
				}
			}
		}
		for _, d := range days {
			if s.recent(d.usage.NewestMs) {
				keep(&item, d.usage)
				continue
			}
			addMember(&item, d.path, d.usage)
		}
		sort.SliceStable(item.Members, func(i, j int) bool { return item.Members[i].Path < item.Members[j].Path })
		item.Reasons = append(item.Reasons, "rollouts of finished Codex sessions")
		s.push(item, false)
	}
	logs := makeItem(CategoryCodex, KindFiles, "Codex logs", codex, codex)
	for _, e := range childEntries(codex) {
		if e.name == "log" || (strings.HasPrefix(e.name, "logs_") && strings.Contains(e.name, ".sqlite")) {
			addMember(&logs, e.path, s.measure(e.path))
		}
	}
	if s.codexRunning && len(logs.Members) > 0 {
		logs.KeptBytes += logs.Bytes
		logs.Bytes = 0
		logs.Members = []Member{}
		logs.Status = StatusInUse
		logs.Reasons = append(logs.Reasons, "Codex is running and writes these")
	} else {
		logs.Reasons = append(logs.Reasons, "Codex diagnostic logs")
	}
	s.push(logs, logs.KeptBytes > 0)
	for _, name := range []string{"cache", "tmp", "shell_snapshots"} {
		dir := codex + "/" + name
		if !isDir(dir) || isSymlink(dir) {
			continue
		}
		item := s.membersOf(CategoryCodex, "Codex "+name, dir, s.liveFilter())
		prepend(&item, "Codex scratch; rebuilt when needed")
		s.push(item, false)
	}
	images := codex + "/generated_images"
	if isDir(images) && !isSymlink(images) {
		item := s.membersOf(CategoryCodex, "Codex generated images", images, nil)
		item.Status = StatusAttached
		item.ConfirmEach = true
		item.Reasons = append(item.Reasons, "images Codex made for you")
		s.push(item, false)
	}
	releases := codex + "/packages/standalone/releases"
	currentRelease := canonicalPath(codex + "/packages/standalone/current")
	for _, e := range childDirs(releases) {
		item := makeItem(CategoryCodex, KindFiles, "Codex "+e.name, e.path, releases)
		addMember(&item, e.path, s.measure(e.path))
		if pathsMatch(currentRelease, e.path) {
			item.Status = StatusProtected
			item.Reasons = append(item.Reasons, "the release `codex` runs")
			item.Members = []Member{}
		} else {
			item.Reasons = append(item.Reasons, "older Codex release")
		}
		s.push(item, true)
	}
	for _, root := range s.c.TmpRoots {
		temp := makeItem(CategoryCodex, KindFiles, "Codex temp files", root, root)
		for _, e := range childEntries(root) {
			if !strings.HasPrefix(e.name, "codex-") || e.link || !ownedByUs(e.path, s.c.UID) {
				continue
			}
			usage := s.measure(e.path)
			if s.codexRunning || s.recent(usage.NewestMs) {
				keep(&temp, usage)
				continue
			}
			addMember(&temp, e.path, usage)
		}
		temp.Reasons = append(temp.Reasons, "sandbox mount points from earlier Codex runs")
		s.push(temp, false)
	}
}

func (s *scanner) scanWorktrees() {
	var roots []string
	addRoot := func(root string) {
		if root == "" || !isDir(root) || isSymlink(root) {
			return
		}
		resolved := canonicalPath(root)
		if !contains(roots, resolved) {
			roots = append(roots, resolved)
		}
	}
	for _, repo := range s.c.Repositories {
		addRoot(repo.Path + "/.worktrees")
		addRoot(repo.Path + "/.claude/worktrees")
	}
	addRoot(s.c.WorktreesDir)
	for _, tmp := range s.c.TmpRoots {
		addRoot(tmp + "/forkmesh-worktrees")
	}
	for _, root := range roots {
		for _, child := range childDirs(root) {
			if s.stopped() {
				return
			}
			s.scanWorktree(root, child)
		}
	}
}

func (s *scanner) git(dir string, args ...string) (string, bool) {
	result := s.c.Git.Run(s.ctx, dir, args, git.Options{Timeout: 30 * time.Second, NoLane: true,
		Env: []string{"GIT_OPTIONAL_LOCKS=0", "GIT_TERMINAL_PROMPT=0", "LC_ALL=C"}})
	if !result.OK {
		return "", false
	}
	return strings.TrimSpace(string(result.Output)), true
}

func (s *scanner) repoGit(repoPath string) *repoGit {
	if info, ok := s.repos[repoPath]; ok {
		return info
	}
	info := &repoGit{}
	listing, ok := s.git(repoPath, "worktree", "list", "--porcelain")
	info.entries = parseWorktrees(listing)
	info.ok = ok
	if len(info.entries) > 0 {
		info.mainBranch = info.entries[0].branch
	}
	for _, candidate := range []string{"main", "master"} {
		if _, ok := s.git(repoPath, "rev-parse", "--verify", "-q", "refs/heads/"+candidate); ok {
			info.base = candidate
			break
		}
	}
	if info.base == "" {
		info.base = info.mainBranch
	}
	s.repos[repoPath] = info
	return info
}

func (s *scanner) repoName(repoPath string) string {
	for _, repo := range s.c.Repositories {
		if pathsMatch(repo.Path, repoPath) {
			return repo.Name
		}
	}
	return fileName(repoPath)
}

func lastSection(text string) string {
	if index := strings.LastIndexByte(text, '/'); index >= 0 {
		return text[index+1:]
	}
	return text
}

func (s *scanner) sameRepository(repository, repoPath string) bool {
	if repository == "" {
		return true
	}
	name := s.repoName(repoPath)
	return strings.EqualFold(repository, name) || strings.EqualFold(lastSection(repository), lastSection(name))
}

func (s *scanner) scanWorktree(root string, child entry) {
	path := child.path
	s.worktreeNames = append(s.worktreeNames, child.name)
	marker := path + "/.git"
	usage := s.measure(path)
	if isDir(marker) {
		item := makeItem(CategoryWorktrees, KindFiles, child.name, path, root)
		item.KeptBytes = usage.Bytes
		item.Files = usage.Files
		item.ModifiedMs = usage.NewestMs
		item.Status = StatusProtected
		item.Reasons = append(item.Reasons, "a full git clone with its own .git folder; never removed here")
		s.push(item, true)
		return
	}
	if !exists(marker) {
		item := makeItem(CategoryWorktrees, KindFiles, child.name, path, root)
		addMember(&item, path, usage)
		item.Reasons = append(item.Reasons, "leftover folder; not a git worktree")
		if s.recent(usage.NewestMs) {
			item.Status = StatusAttached
			item.Reasons = append(item.Reasons, s.recentText())
		}
		s.push(item, false)
		return
	}
	repoPath := commonRepoFor(path)
	item := makeItem(CategoryWorktrees, KindWorktree, child.name, path, root)
	item.Bytes = usage.Bytes
	item.Files = usage.Files
	item.ModifiedMs = usage.NewestMs
	item.RepoPath = repoPath
	var found *worktreeInfo
	if repoPath != "" {
		info := s.repoGit(repoPath)
		for index := range info.entries {
			if pathsMatch(info.entries[index].path, path) {
				found = &info.entries[index]
				break
			}
		}
		item.BaseBranch = info.base
	}
	if found == nil {
		item.Kind = KindFiles
		item.KeptBytes = item.Bytes
		item.Bytes = 0
		item.Status = StatusProtected
		if repoPath == "" {
			item.Reasons = append(item.Reasons, "its .git file points at a repository that is gone; remove it by hand")
		} else {
			item.Reasons = append(item.Reasons, fmt.Sprintf("has a .git file but %s does not list it; run git worktree prune", repoPath))
		}
		s.push(item, true)
		return
	}
	item.Branch = found.branch
	item.Locked = found.locked
	var attached, inUse []string
	if item.Branch == "" {
		item.Reasons = append(item.Reasons, "detached HEAD in "+s.repoName(repoPath))
	} else {
		item.Reasons = append(item.Reasons, fmt.Sprintf("worktree of branch %s in %s", item.Branch, s.repoName(repoPath)))
	}
	if status, ok := s.git(path, "status", "--porcelain"); ok {
		for _, line := range strings.Split(status, "\n") {
			if line != "" {
				item.DirtyFiles++
			}
		}
	}
	if item.DirtyFiles > 0 {
		attached = append(attached, fmt.Sprintf("%d uncommitted file%s", item.DirtyFiles, plural(item.DirtyFiles, "s")))
	}
	if item.BaseBranch != "" && found.head != "" {
		counts, ok := s.git(repoPath, "rev-list", "--left-right", "--count", fmt.Sprintf("refs/heads/%s...%s", item.BaseBranch, found.head))
		fields := strings.Fields(counts)
		if ok && len(fields) == 2 {
			item.Behind = qtInt(fields[0])
			item.Ahead = qtInt(fields[1])
		}
	}
	if item.Ahead > 0 {
		attached = append(attached, fmt.Sprintf("%d commit%s not in %s", item.Ahead, plural(item.Ahead, "s"), item.BaseBranch))
	} else if item.Ahead == 0 {
		item.Reasons = append(item.Reasons, "merged into "+item.BaseBranch)
	}
	if item.Behind > 0 {
		item.Reasons = append(item.Reasons, fmt.Sprintf("%d behind", item.Behind))
	}
	if item.Locked {
		if found.lockReason == "" {
			attached = append(attached, "locked")
		} else {
			attached = append(attached, "locked: "+found.lockReason)
		}
	}
	for _, agent := range s.c.Agents {
		matches := pathsMatch(agent.Worktree, path) ||
			(agent.Branch != "" && agent.Branch == item.Branch && (agent.RepoPath == "" || pathsMatch(agent.RepoPath, repoPath)))
		if !matches {
			continue
		}
		text := fmt.Sprintf("agent #%d (%s)", agent.ID, agent.Status)
		item.Agents = append(item.Agents, text)
		if agent.Live {
			inUse = append(inUse, fmt.Sprintf("agent #%d is running", agent.ID))
		} else {
			attached = append(attached, "used by "+text)
		}
	}
	for _, task := range s.c.Tasks {
		matches := pathsMatch(task.Worktree, path) ||
			(task.Branch != "" && task.Branch == item.Branch && s.sameRepository(task.Repository, repoPath))
		if !matches {
			continue
		}
		text := "task " + task.TaskID
		if task.Title != "" {
			text = "task “" + task.Title + "”"
		}
		item.Tasks = append(item.Tasks, text)
		attached = append(attached, "work item of "+text)
	}
	if users := s.c.Processes.UsersOf(path); len(users) > 0 {
		inUse = append(inUse, "open in "+firstUsers(users))
	}
	switch {
	case len(inUse) > 0:
		item.Status = StatusInUse
		reasons := append([]string{}, inUse...)
		reasons = append(reasons, item.Reasons...)
		item.Reasons = append(reasons, attached...)
	case len(attached) > 0:
		item.Status = StatusAttached
		item.Reasons = append(item.Reasons, attached...)
	default:
		item.Status = StatusOrphaned
		item.Reasons = append(item.Reasons, "nothing references it")
	}
	s.push(item, true)
}

func qtInt(text string) int {
	value, err := strconv.ParseInt(strings.TrimSpace(text), 10, 32)
	if err != nil {
		return 0
	}
	return int(value)
}

func (s *scanner) scanBuilds() {
	s.scanAgentBuilds()
	for _, root := range s.c.TmpRoots {
		s.scanTmp(root)
	}
	qttest := s.c.Home + "/.qttest/share/ForkMeshTests"
	if isDir(qttest) {
		item := s.membersOf(CategoryBuildTemp, "Qt test temp folders", qttest, func(e entry, usage Usage) (string, bool) {
			if !strings.Contains(e.name, "qt_temp") {
				return "not a test temp folder", true
			}
			if s.recent(usage.NewestMs) {
				return s.recentText(), true
			}
			return "", false
		})
		prepend(&item, "left by killed window-test runs")
		s.push(item, false)
	}
	s.scanSandboxes()
	s.scanPlaywright()
	s.scanCcache()
}

var agentBuildName = regexp.MustCompile(`^agent-([A-Za-z0-9][A-Za-z0-9_-]*?)(?:-tmp)?(?:\.[^/]*)?$`)

func (s *scanner) scanAgentBuilds() {
	builds := s.c.BuildsDir
	if builds == "" || !isDir(builds) {
		return
	}
	groups := map[string]*Item{}
	for _, e := range childEntries(builds) {
		if s.stopped() {
			return
		}
		if e.link {
			continue
		}
		match := agentBuildName.FindStringSubmatch(e.name)
		key := e.name
		if match != nil {
			key = "agent-" + match[1]
		}
		if groups[key] == nil {
			item := makeItem(CategoryBuildTemp, KindFiles, "Build · "+key, builds+"/"+key, builds)
			if match != nil {
				s.classifyAgentBuild(&item, match[1])
			} else {
				item.Status = StatusAttached
				item.Reasons = append(item.Reasons, "build tree that is not an agent's")
			}
			groups[key] = &item
		}
		addMember(groups[key], e.path, s.measure(e.path))
	}
	keys := make([]string, 0, len(groups))
	for key := range groups {
		keys = append(keys, key)
	}
	sort.Strings(keys)
	for _, key := range keys {
		item := *groups[key]
		if item.Status == StatusOrphaned && s.recent(item.ModifiedMs) {
			item.Status = StatusAttached
			item.Reasons = append(item.Reasons, s.recentText())
		}
		s.push(item, false)
	}
}

func (s *scanner) classifyAgentBuild(item *Item, slug string) {
	number, err := strconv.ParseInt(slug, 10, 32)
	numeric := err == nil
	for _, agent := range s.c.Agents {
		if !numeric || (int64(agent.ID) != number && int64(agent.IssueNumber) != number) {
			continue
		}
		if agent.Live {
			item.Status = StatusAttached
			item.Reasons = append(item.Reasons, fmt.Sprintf("agent #%d is running", agent.ID))
			return
		}
	}
	flat := strings.ReplaceAll(slug, "-", "")
	for _, name := range s.worktreeNames {
		flatName := strings.ReplaceAll(name, "-", "")
		matches := name == slug || strings.HasPrefix(name, slug+"-") || name == "agent-"+slug ||
			strings.HasPrefix(name, "agent-"+slug+"-") ||
			(!numeric && len([]rune(flat)) >= 4 && strings.HasPrefix(flatName, flat))
		if matches {
			item.Status = StatusAttached
			item.Reasons = append(item.Reasons, fmt.Sprintf("worktree %s still exists", name))
			return
		}
	}
	item.Reasons = append(item.Reasons, fmt.Sprintf("agent %s build; no worktree of that name is left", slug))
}

var tmpPattern = regexp.MustCompile(`^(fm\d+[-_.].*|fm-.*|fmmcp-.*|claude-[0-9a-f]+-cwd|ActionSecurity-.*|.*qt_temp.*|ForkMesh-.*|forkmesh-build.*|chat-history-.*)$`)

func (s *scanner) scanTmp(root string) {
	item := makeItem(CategoryBuildTemp, KindFiles, "Agent temp files", root, root)
	for _, e := range childEntries(root) {
		if s.stopped() {
			return
		}
		if e.link || !tmpPattern.MatchString(e.name) || !ownedByUs(e.path, s.c.UID) || (!e.dir && !e.file) {
			continue
		}
		usage := s.measure(e.path)
		if s.recent(usage.NewestMs) {
			keep(&item, usage)
			continue
		}
		addMember(&item, e.path, usage)
	}
	item.Reasons = append(item.Reasons, "scratch files agents left in "+root)
	if item.KeptBytes > 0 {
		item.Reasons = append(item.Reasons, "recent ones kept")
	}
	s.push(item, false)
}

func (s *scanner) scanSandboxes() {
	base := s.c.SandboxDir
	if base == "" || !isDir(base) {
		return
	}
	for _, e := range childDirs(base) {
		if s.stopped() {
			return
		}
		if !strings.HasPrefix(e.name, sandboxRunPrefix) && !strings.HasPrefix(e.name, sandboxDiscardPrefix) {
			continue
		}
		item := makeItem(CategoryBuildTemp, KindActionSandbox, "Actions sandbox · "+e.name, e.path, base)
		addMember(&item, e.path, s.measure(e.path))
		users := s.c.Processes.UsersOf(e.path)
		switch {
		case s.c.claimed(e.name):
			item.Status = StatusInUse
			item.Reasons = append(item.Reasons, "a running Action owns it")
		case len(users) > 0:
			item.Status = StatusInUse
			item.Reasons = append(item.Reasons, "open in "+firstUsers(users))
		default:
			item.Reasons = append(item.Reasons, "finished run's disposable tree")
		}
		s.push(item, true)
	}
	cache := filepath.Dir(cleanPath(base)) + "/cache"
	if isDir(cache) {
		item := makeItem(CategoryBuildTemp, KindReport, "Actions build cache", cache, filepath.Dir(cache))
		usage := s.measure(cache)
		item.KeptBytes = usage.Bytes
		item.Files = usage.Files
		item.ModifiedMs = usage.NewestMs
		item.Status = StatusProtected
		item.Reasons = append(item.Reasons, "persistent per-repository build cache; Actions caps it itself")
		s.push(item, true)
	}
}

var playwrightBuild = regexp.MustCompile(`^(.*)-(\d+)$`)

func (s *scanner) scanPlaywright() {
	root := s.c.Home + "/.cache/ms-playwright"
	dirs := childDirs(root)
	newest := map[string]entry{}
	for _, e := range dirs {
		match := playwrightBuild.FindStringSubmatch(e.name)
		if match == nil {
			continue
		}
		family := match[1]
		current, ok := newest[family]
		if !ok || qtInt(playwrightBuild.FindStringSubmatch(current.name)[2]) < qtInt(match[2]) {
			newest[family] = e
		}
	}
	for _, e := range dirs {
		match := playwrightBuild.FindStringSubmatch(e.name)
		if match == nil {
			continue
		}
		item := makeItem(CategoryBuildTemp, KindFiles, "Playwright "+e.name, e.path, root)
		addMember(&item, e.path, s.measure(e.path))
		if newest[match[1]].path == e.path {
			item.Status = StatusAttached
			item.Reasons = append(item.Reasons, fmt.Sprintf("newest %s build; browser tests use it", match[1]))
		} else {
			item.Reasons = append(item.Reasons, fmt.Sprintf("older %s build", match[1]))
		}
		s.push(item, false)
	}
}

func (s *scanner) activeCcache() string {
	if s.c.CcacheDir != "" || !s.c.runCcache() {
		return s.c.CcacheDir
	}
	binary := s.c.Ccache
	if binary == "" {
		binary = "ccache"
	}
	ctx, cancel := context.WithTimeout(s.ctx, 5*time.Second)
	defer cancel()
	out, err := exec.CommandContext(ctx, binary, "-k", "cache_dir").Output()
	if err != nil {
		return ""
	}
	return strings.TrimSpace(string(out))
}

func (s *scanner) scanCcache() {
	active := s.activeCcache()
	var candidates []string
	if active != "" {
		candidates = append(candidates, cleanPath(active))
	}
	for _, path := range []string{s.c.Home + "/.ccache", s.c.Home + "/.cache/ccache"} {
		if !contains(candidates, cleanPath(path)) {
			candidates = append(candidates, cleanPath(path))
		}
	}
	for _, path := range candidates {
		if !isDir(path) || isSymlink(path) {
			continue
		}
		isActive := active != "" && pathsMatch(path, active)
		kind := KindFiles
		if isActive {
			kind = KindReport
		}
		item := makeItem(CategoryBuildTemp, kind, "ccache · "+path, path, filepath.Dir(path))
		usage := s.measure(path)
		if isActive {
			item.KeptBytes = usage.Bytes
			item.Files = usage.Files
			item.ModifiedMs = usage.NewestMs
			item.Status = StatusProtected
			item.Reasons = append(item.Reasons, "the active compiler cache; report only, use Trim ccache")
		} else {
			addMember(&item, path, usage)
			item.Status = StatusAttached
			item.ConfirmEach = true
			item.Reasons = append(item.Reasons, "not the active ccache folder")
		}
		s.push(item, true)
	}
}

var otherCachePattern = regexp.MustCompile(`^(fm-.*|fm\d.*|forkmesh-.*)$`)

func (s *scanner) scanOtherCaches() {
	cache := s.c.Home + "/.cache"
	for _, e := range childEntries(cache) {
		if s.stopped() {
			return
		}
		if e.link || !otherCachePattern.MatchString(e.name) {
			continue
		}
		item := makeItem(CategoryOtherCaches, KindFiles, e.name, e.path, cache)
		usage := s.measure(e.path)
		release := strings.HasPrefix(e.name, "forkmesh-release-")
		if release || exists(e.path+"/.git") {
			item.KeptBytes = usage.Bytes
			item.Files = usage.Files
			item.ModifiedMs = usage.NewestMs
			item.Status = StatusProtected
			if release {
				item.Reasons = append(item.Reasons, "release build folder; a release timer uses it")
			} else {
				item.Reasons = append(item.Reasons, "a git checkout or worktree")
			}
			s.push(item, true)
			continue
		}
		addMember(&item, e.path, usage)
		item.ConfirmEach = true
		item.Reasons = append(item.Reasons, "operator cache from an agent run; check before deleting")
		if s.recent(usage.NewestMs) {
			item.Status = StatusAttached
			item.Reasons = append(item.Reasons, s.recentText())
		}
		s.push(item, false)
	}
}
