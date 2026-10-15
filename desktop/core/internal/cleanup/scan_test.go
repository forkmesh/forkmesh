package cleanup

import (
	"bytes"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"sort"
	"strings"
	"testing"
	"time"
)

const (
	kLive   = "11111111-2222-3333-4444-555555555555"
	kOld    = "aaaaaaaa-0000-0000-0000-000000000001"
	kRecent = "aaaaaaaa-0000-0000-0000-000000000002"
	kGone   = "aaaaaaaa-0000-0000-0000-000000000003"
)

func sandbox(t *testing.T) string {
	t.Helper()
	root, err := filepath.EvalSymlinks(t.TempDir())
	if err != nil {
		t.Fatal(err)
	}
	home := filepath.Join(root, "home")
	for _, dir := range []string{home, filepath.Join(root, "tmp")} {
		if err := os.MkdirAll(dir, 0o755); err != nil {
			t.Fatal(err)
		}
	}
	t.Setenv("HOME", home)
	t.Setenv("XDG_DATA_HOME", filepath.Join(home, ".local/share"))
	t.Setenv("XDG_CONFIG_HOME", filepath.Join(home, ".config"))
	t.Setenv("XDG_CACHE_HOME", filepath.Join(home, ".cache"))
	t.Setenv("GIT_CONFIG_GLOBAL", os.DevNull)
	t.Setenv("GIT_CONFIG_SYSTEM", os.DevNull)
	for _, name := range []string{"FORKMESH_HOME", "FORKMESH_PROJECTS_DIR", "FORKMESH_PROJECT", "FORKMESH_PROJECT_DIR",
		"FORKMESH_DATA_DIR", "FORKMESH_CONFIG_DIR", "CCACHE_DIR"} {
		t.Setenv(name, "")
	}
	return root
}

func touch(t *testing.T, path string, body ...[]byte) {
	t.Helper()
	data := bytes.Repeat([]byte("x"), 4096)
	if len(body) > 0 {
		data = body[0]
	}
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, data, 0o644); err != nil {
		t.Fatal(err)
	}
}

func age(t *testing.T, path string, days int) {
	t.Helper()
	when := time.Now().Add(-time.Duration(days) * 24 * time.Hour)
	var paths []string
	if info, err := os.Lstat(path); err == nil && info.IsDir() {
		_ = filepath.Walk(path, func(p string, info os.FileInfo, err error) error {
			if err == nil && p != path && info.Mode()&os.ModeSymlink == 0 {
				paths = append(paths, p)
			}
			return nil
		})
	}
	sort.SliceStable(paths, func(i, j int) bool {
		return strings.Count(paths[i], "/") > strings.Count(paths[j], "/")
	})
	paths = append(paths, path)
	for _, p := range paths {
		_ = os.Chtimes(p, when, when)
	}
}

func baseContext(root string) Context {
	home := filepath.Join(root, "home")
	off := false
	project := filepath.Join(home, "projects/forkmesh")
	return Context{
		Home:         home,
		TmpRoots:     []string{filepath.Join(root, "tmp")},
		UID:          currentUID(),
		ProjectHome:  project,
		DataDir:      filepath.Join(project, "data"),
		BuildsDir:    filepath.Join(project, "builds"),
		WorktreesDir: filepath.Join(project, "worktrees"),
		SandboxDir:   filepath.Join(project, "data/actions/sandbox"),
		RunCcache:    &off,
		NowMs:        time.Now().UnixMilli(),
	}
}

func find(items []Item, category Category, title string) *Item {
	for i := range items {
		if items[i].Category == category && items[i].Title == title {
			return &items[i]
		}
	}
	return nil
}

func hasMember(item *Item, path string) bool {
	if item == nil {
		return false
	}
	for _, member := range item.Members {
		if member.Path == filepath.Clean(path) {
			return true
		}
	}
	return false
}

func scan(c Context) ScanResult { return Scan(nil, c, nil, nil) }

func unixOnly(t *testing.T) {
	if runtime.GOOS == "windows" {
		t.Skip("the scanner's sizes and links are POSIX here")
	}
}

func TestHelpers(t *testing.T) {
	unixOnly(t)
	root := sandbox(t)
	if got := EncodeClaudeProject("/home/tester/projects/forkmesh/src/.worktrees/x_y"); got != "-home-tester-projects-forkmesh-src--worktrees-x-y" {
		t.Errorf("encode %q", got)
	}
	if !UUIDsIn("--resume " + kLive + " x")[kLive] {
		t.Error("session ids are read out of command lines")
	}
	if !PathWithin("/a/b/c", "/a/b") || PathWithin("/a/bc", "/a/b") {
		t.Error("path containment respects folder boundaries")
	}
	dir := filepath.Join(root, "measure")
	touch(t, dir+"/a.bin", bytes.Repeat([]byte("a"), 64*1024))
	touch(t, dir+"/sub/b.bin", bytes.Repeat([]byte("b"), 64*1024))
	outside := filepath.Join(root, "outside")
	touch(t, outside+"/big.bin", bytes.Repeat([]byte("c"), 512*1024))
	_ = os.Symlink(outside, dir+"/link")
	usage := Measure(dir, nil)
	if usage.Files != 3 || usage.Bytes < 128*1024 || usage.Bytes >= 512*1024 {
		t.Errorf("measure %+v", usage)
	}
	if SizeText(0) != "0 bytes" || SizeText(1023) != "1023 bytes" || SizeText(1536) != "1.5 KiB" || SizeText(3*1024*1024) != "3.0 MiB" {
		t.Errorf("sizes %q %q %q %q", SizeText(0), SizeText(1023), SizeText(1536), SizeText(3*1024*1024))
	}
	now := int64(1_000_000_000_000)
	if AgeText(0, now) != "-" || AgeText(now-5*minuteMs, now) != "5 min" || AgeText(now-3*60*minuteMs, now) != "3 h" || AgeText(now-5*dayMs, now) != "5 d" {
		t.Error("age texts")
	}
	if CategoryName(CategoryWorktrees) != "Agent worktrees" || StatusName(StatusInUse) != "In use" || len(Categories()) != 5 {
		t.Error("names")
	}
}

func TestProcesses(t *testing.T) {
	unixOnly(t)
	root := sandbox(t)
	proc := filepath.Join(root, "proc")
	target := filepath.Join(root, "home/.claude/projects/p")
	_ = os.MkdirAll(target, 0o755)
	_ = os.MkdirAll(proc+"/4242/fd", 0o755)
	_ = os.Symlink(target, proc+"/4242/cwd")
	_ = os.Symlink(filepath.Join(root, "home/.local/share/claude/versions/2.0.0"), proc+"/4242/exe")
	_ = os.Symlink(filepath.Join(root, "tmp/open.log")+" (deleted)", proc+"/4242/fd/3")
	touch(t, proc+"/4242/comm", []byte("claude\n"))
	touch(t, proc+"/4242/cmdline", []byte("claude\x00--resume\x00"+kLive+"\x00"))
	_ = os.MkdirAll(proc+"/self", 0o755)
	snapshot := ReadProcesses(proc)
	if len(snapshot.Processes) != 1 || snapshot.Processes[0].PID != 4242 || !snapshot.Available {
		t.Fatalf("only numeric process folders are read: %+v", snapshot)
	}
	if len(snapshot.UsersOf(target)) == 0 {
		t.Error("a process whose cwd is inside a path uses it")
	}
	if users := snapshot.UsersOf(filepath.Join(root, "tmp")); len(users) != 1 || users[0] != "claude (PID 4242)" {
		t.Errorf("an open file descriptor marks its folder in use: %v", users)
	}
	if len(snapshot.UsersOf(filepath.Join(root, "home/.codex"))) != 0 {
		t.Error("unrelated folders are not in use")
	}
	if !IsClaudeProcess(snapshot.Processes[0]) || IsCodexProcess(snapshot.Processes[0]) {
		t.Error("claude processes are recognised")
	}
	if !IsCodexProcess(Process{Cmdline: []string{"node", "/usr/lib/codex.js"}}) {
		t.Error("codex under node is recognised")
	}
	if ReadProcesses(filepath.Join(root, "missing")).Available {
		t.Error("no /proc means no snapshot")
	}
}

func claudeSnapshot(cwd string) ProcessSnapshot {
	return ProcessSnapshot{Available: true, Processes: []Process{{PID: 77, Comm: "claude", Cwd: cwd,
		Cmdline: []string{"claude", "--resume", kLive}}}}
}

func TestClaude(t *testing.T) {
	unixOnly(t)
	root := sandbox(t)
	c := baseContext(root)
	folder := c.Home + "/code/app"
	_ = os.MkdirAll(folder, 0o755)
	projects := c.Home + "/.claude/projects"
	kept := projects + "/" + EncodeClaudeProject(folder)
	line := []byte(`{"cwd":"` + folder + `","type":"user"}` + "\n")
	touch(t, kept+"/"+kOld+".jsonl", line)
	touch(t, kept+"/"+kOld+"/subagents/a.jsonl", line)
	touch(t, kept+"/"+kRecent+".jsonl", line)
	touch(t, kept+"/"+kLive+".jsonl", line)
	touch(t, kept+"/memory/MEMORY.md", []byte("notes"))
	goneFolder := c.Home + "/code/removed"
	gone := projects + "/" + EncodeClaudeProject(goneFolder)
	touch(t, gone+"/"+kGone+".jsonl", []byte(`{"cwd":"`+goneFolder+`"}`+"\n"))
	age(t, kept+"/"+kOld+".jsonl", 60)
	age(t, kept+"/"+kOld, 60)
	age(t, kept+"/"+kRecent+".jsonl", 2)
	age(t, kept+"/"+kLive+".jsonl", 3)
	age(t, kept+"/memory", 90)
	age(t, gone+"/"+kGone+".jsonl", 5)

	scratch := c.TmpRoots[0] + "/claude-" + c.UID + "/" + EncodeClaudeProject(folder)
	touch(t, scratch+"/"+kLive+"/scratchpad/notes.txt")
	touch(t, scratch+"/"+kOld+"/tasks/out.txt")
	age(t, scratch+"/"+kLive, 1)
	age(t, scratch+"/"+kOld, 1)

	versions := c.Home + "/.local/share/claude/versions"
	touch(t, versions+"/2.0.0")
	touch(t, versions+"/1.9.0")
	age(t, versions+"/1.9.0", 10)
	_ = os.MkdirAll(c.Home+"/.local/bin", 0o755)
	_ = os.Symlink(versions+"/2.0.0", c.Home+"/.local/bin/claude")
	c.ClaudeBinary = c.Home + "/.local/bin/claude"

	account := c.DataDir + "/agent-accounts/claude/abcdef12-0000/projects/p"
	touch(t, account+"/s.jsonl")
	age(t, account, 40)

	c.Processes = claudeSnapshot(c.Home)
	result := scan(c)
	old := find(result.Items, CategoryClaude, "Old transcripts · "+folder)
	current := find(result.Items, CategoryClaude, "Transcripts · "+folder)
	if old == nil || old.Status != StatusOrphaned || !hasMember(old, kept+"/"+kOld+".jsonl") || !hasMember(old, kept+"/"+kOld) {
		t.Errorf("sessions older than the cut-off are orphaned even while their folder exists: %+v", old)
	}
	if old != nil && old.ID != "0:Old transcripts · "+folder+":"+kept {
		t.Errorf("item id %q", old.ID)
	}
	if current == nil || current.Status != StatusAttached || !hasMember(current, kept+"/"+kRecent+".jsonl") ||
		!strings.Contains(current.ReasonText(), "still exists") {
		t.Errorf("recent sessions of an existing folder are attached to it: %+v", current)
	}
	if current == nil || hasMember(current, kept+"/"+kLive+".jsonl") || !strings.Contains(current.ReasonText(), "live session") {
		t.Errorf("the transcript of a running session is never a member: %+v", current)
	}
	if current != nil && current.ReasonText() != "folder "+folder+" still exists · 1 live session kept · memory/ kept" {
		t.Errorf("reason text %q", current.ReasonText())
	}
	for _, item := range result.Items {
		for _, member := range item.Members {
			if strings.Contains(member.Path, "/memory") {
				t.Errorf("a project's memory folder is never offered for deletion: %s", member.Path)
			}
		}
	}
	goneItem := find(result.Items, CategoryClaude, "Transcripts · "+goneFolder)
	if goneItem == nil || goneItem.Status != StatusOrphaned || !strings.Contains(goneItem.ReasonText(), "is gone") {
		t.Errorf("transcripts of a folder that is gone are orphaned: %+v", goneItem)
	}
	if goneItem != nil && !strings.HasPrefix(goneItem.ID, "0:Old transcripts · ") {
		t.Errorf("the gone item keeps the id it was made with: %q", goneItem.ID)
	}
	scratchItem := find(result.Items, CategoryClaude, "Scratchpads · "+EncodeClaudeProject(folder))
	if scratchItem == nil || !hasMember(scratchItem, scratch+"/"+kOld) || hasMember(scratchItem, scratch+"/"+kLive) {
		t.Errorf("the scratchpad of a live session is kept, finished ones can go: %+v", scratchItem)
	}
	if scratchItem != nil && scratchItem.ReasonText() != "scratchpads and task output of finished sessions · 1 kept: live session" {
		t.Errorf("scratch reasons %q", scratchItem.ReasonText())
	}
	v2 := find(result.Items, CategoryClaude, "Claude Code 2.0.0")
	v1 := find(result.Items, CategoryClaude, "Claude Code 1.9.0")
	if v2 == nil || v2.Status != StatusProtected || v2.Deletable() {
		t.Errorf("the Claude Code version `claude` runs is protected: %+v", v2)
	}
	if v1 == nil || v1.Status != StatusOrphaned || !v1.Deletable() {
		t.Errorf("older Claude Code versions are orphaned: %+v", v1)
	}
	accountProtected := false
	for _, item := range result.Items {
		if strings.HasPrefix(item.Title, "Desktop agent transcripts") {
			accountProtected = item.Status == StatusProtected && !item.Deletable() && len(item.Members) == 0 && item.KeptBytes > 0 &&
				item.Title == "Desktop agent transcripts · claude abcdef12"
		}
	}
	if !accountProtected {
		t.Error("desktop agent account transcripts are shown but protected")
	}
}

func TestCodex(t *testing.T) {
	unixOnly(t)
	root := sandbox(t)
	c := baseContext(root)
	codex := c.Home + "/.codex"
	touch(t, codex+"/sessions/2026/09/01/rollout-a.jsonl")
	touch(t, codex+"/logs_2.sqlite")
	touch(t, codex+"/auth.json", []byte("{}"))
	touch(t, codex+"/packages/standalone/releases/0.1/codex")
	touch(t, codex+"/packages/standalone/releases/0.2/codex")
	_ = os.Symlink(codex+"/packages/standalone/releases/0.2", codex+"/packages/standalone/current")
	age(t, codex, 20)
	result := scan(c)
	sessions := find(result.Items, CategoryCodex, "Codex sessions")
	if sessions == nil || !hasMember(sessions, codex+"/sessions/2026/09/01") {
		t.Errorf("Codex session days are offered: %+v", sessions)
	}
	logs := find(result.Items, CategoryCodex, "Codex logs")
	if logs == nil || logs.Status != StatusOrphaned || !hasMember(logs, codex+"/logs_2.sqlite") {
		t.Errorf("Codex logs can go while Codex is not running: %+v", logs)
	}
	for _, item := range result.Items {
		if hasMember(&item, codex+"/auth.json") {
			t.Error("Codex sign-in and settings are never listed")
		}
	}
	current := find(result.Items, CategoryCodex, "Codex 0.2")
	old := find(result.Items, CategoryCodex, "Codex 0.1")
	if current == nil || current.Status != StatusProtected || old == nil || !old.Deletable() {
		t.Error("the current Codex release is protected and older ones are not")
	}
	c.Processes.Processes = append(c.Processes.Processes, Process{PID: 9, Comm: "codex"})
	result = scan(c)
	logs = find(result.Items, CategoryCodex, "Codex logs")
	if logs == nil || logs.Status != StatusInUse || logs.Deletable() || logs.ReasonText() != "Codex is running and writes these" {
		t.Errorf("Codex logs are in use while Codex runs: %+v", logs)
	}
}

func gitRun(t *testing.T, dir string, args ...string) {
	t.Helper()
	cmd := exec.Command("git", append([]string{"-c", "user.name=Cleanup Test", "-c", "user.email=cleanup@test.invalid",
		"-c", "commit.gpgsign=false"}, args...)...)
	cmd.Dir = dir
	if out, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("git %v: %v\n%s", args, err, out)
	}
}

func makeRepo(t *testing.T, c Context) (string, string) {
	repo := c.Home + "/projects/app"
	worktrees := repo + "/.worktrees"
	_ = os.MkdirAll(repo, 0o755)
	gitRun(t, repo, "init", "-q", "-b", "main")
	touch(t, repo+"/README", []byte("hello\n"))
	touch(t, repo+"/.gitignore", []byte(".worktrees\nnode_modules\n"))
	gitRun(t, repo, "add", ".")
	gitRun(t, repo, "commit", "-q", "-m", "init")
	for _, name := range []string{"clean", "dirty", "ahead", "agent", "live", "task", "busy"} {
		gitRun(t, repo, "worktree", "add", "-q", "-b", name, worktrees+"/"+name)
	}
	touch(t, worktrees+"/clean/node_modules/pkg/index.js", bytes.Repeat([]byte("n"), 100000))
	touch(t, worktrees+"/dirty/README", []byte("changed\n"))
	touch(t, worktrees+"/ahead/feature.txt", []byte("feature\n"))
	gitRun(t, worktrees+"/ahead", "add", ".")
	gitRun(t, worktrees+"/ahead", "commit", "-q", "-m", "feature")
	touch(t, worktrees+"/leftover/junk.txt")
	age(t, worktrees, 3)
	return repo, worktrees
}

func TestWorktrees(t *testing.T) {
	unixOnly(t)
	if _, err := exec.LookPath("git"); err != nil {
		t.Skip("git is missing")
	}
	root := sandbox(t)
	c := baseContext(root)
	repo, worktrees := makeRepo(t, c)
	c.Repositories = []RepositoryRef{{Name: "me/app", Path: repo}}
	c.Agents = []AgentRef{
		{ID: 512, Status: "success", Branch: "agent", RepoPath: repo},
		{ID: 513, Status: "running", Worktree: worktrees + "/live", Live: true},
	}
	c.Tasks = []TaskLink{{TaskID: "t-1", Title: "Ship it", Repository: "me/app", Branch: "task"}}
	c.Processes = ProcessSnapshot{Available: true, Processes: []Process{{PID: 4321, Comm: "sleep", Cwd: worktrees + "/busy"}}}
	result := scan(c)
	worktree := func(name string) *Item { return find(result.Items, CategoryWorktrees, name) }

	clean := worktree("clean")
	if clean == nil || clean.Kind != KindWorktree || clean.Status != StatusOrphaned || clean.Ahead != 0 || clean.DirtyFiles != 0 || clean.Bytes < 100000 {
		t.Fatalf("a clean merged worktree nothing uses is orphaned and sized with its node_modules: %+v", clean)
	}
	if clean.ReasonText() != "worktree of branch clean in me/app · merged into main · nothing references it" || clean.BaseBranch != "main" || clean.RepoPath != repo {
		t.Errorf("clean reasons %q base %q repo %q", clean.ReasonText(), clean.BaseBranch, clean.RepoPath)
	}
	dirty := worktree("dirty")
	if dirty == nil || dirty.Status != StatusAttached || dirty.DirtyFiles != 1 || !strings.Contains(dirty.ReasonText(), "1 uncommitted file") {
		t.Errorf("uncommitted changes attach a worktree: %+v", dirty)
	}
	ahead := worktree("ahead")
	if ahead == nil || ahead.Status != StatusAttached || ahead.Ahead != 1 || !strings.Contains(ahead.ReasonText(), "1 commit not in main") {
		t.Errorf("unmerged commits attach a worktree: %+v", ahead)
	}
	agent := worktree("agent")
	if agent == nil || agent.Status != StatusAttached || !strings.Contains(agent.ReasonText(), "used by agent #512 (success)") {
		t.Errorf("a finished agent session's branch attaches its worktree: %+v", agent)
	}
	live := worktree("live")
	if live == nil || live.Status != StatusInUse || live.Deletable() || !strings.HasPrefix(live.ReasonText(), "agent #513 is running · worktree of branch live") {
		t.Errorf("a running agent's worktree is in use: %+v", live)
	}
	task := worktree("task")
	if task == nil || task.Status != StatusAttached || !strings.Contains(task.ReasonText(), "work item of task “Ship it”") {
		t.Errorf("an organization task work item attaches its worktree: %+v", task)
	}
	busy := worktree("busy")
	if busy == nil || busy.Status != StatusInUse || !strings.Contains(busy.ReasonText(), "open in sleep (PID 4321)") {
		t.Errorf("a process working inside a worktree marks it in use: %+v", busy)
	}
	leftover := worktree("leftover")
	if leftover == nil || leftover.Kind != KindFiles || leftover.Status != StatusOrphaned || !hasMember(leftover, worktrees+"/leftover") {
		t.Errorf("a leftover folder without .git is plain files: %+v", leftover)
	}

	chosen := []Item{*clean, *dirty, *ahead, *live}
	choices := map[string]Choice{ahead.ID: {Confirmed: true}}
	plan := BuildPlan(chosen, choices)
	if len(plan.Worktrees) != 2 || len(plan.Skipped) != 2 {
		t.Errorf("the plan removes the orphan and the confirmed attached worktree only: %+v", plan)
	}
	choices[dirty.ID] = Choice{Confirmed: true}
	plan = BuildPlan(chosen, choices)
	dirtyKept := false
	for _, skip := range plan.Skipped {
		dirtyKept = dirtyKept || (skip.Item.Title == "dirty" && strings.Contains(skip.Why, "uncommitted"))
	}
	if !dirtyKept {
		t.Error("a dirty worktree needs Discard ticked even when confirmed")
	}
	choices[dirty.ID] = Choice{Confirmed: true, DiscardChanges: true}
	if plan = BuildPlan(chosen, choices); len(plan.Worktrees) != 3 {
		t.Error("ticking Discard lets the dirty worktree into the plan")
	}
	recheck := Recheck{Processes: ProcessSnapshot{Processes: []Process{{PID: 1, Cwd: worktrees + "/clean/node_modules"}}}}
	if WorktreeRefusal(*clean, recheck) == "" {
		t.Error("a worktree that became busy after the scan is refused at delete time")
	}
	if why := WorktreeRefusal(*clean, Recheck{}); why != "" {
		t.Errorf("an idle worktree passes the delete-time check: %s", why)
	}
	for _, name := range []string{"clean", "dirty", "ahead", "agent", "live", "task", "busy"} {
		if _, err := os.Stat(worktrees + "/" + name); err != nil {
			t.Errorf("scanning touched %s", name)
		}
	}
}

func TestBuildsAndCaches(t *testing.T) {
	unixOnly(t)
	root := sandbox(t)
	c := baseContext(root)
	_ = os.MkdirAll(c.WorktreesDir+"/5-feature", 0o755)
	touch(t, c.BuildsDir+"/agent-5/build.ninja")
	touch(t, c.BuildsDir+"/agent-9/build.ninja")
	touch(t, c.BuildsDir+"/agent-9-tmp/cc.o")
	touch(t, c.BuildsDir+"/agent-9.build0.log")
	touch(t, c.SandboxDir+"/forkmesh-run-1-100/work/a")
	touch(t, c.SandboxDir+"/forkmesh-run-2-200/work/a")
	touch(t, c.DataDir+"/actions/cache/repo/x")
	touch(t, c.Home+"/.cache/ms-playwright/chromium-1000/chrome")
	touch(t, c.Home+"/.cache/ms-playwright/chromium-1100/chrome")
	touch(t, c.Home+"/.cache/fm-rewrite/state.json")
	touch(t, c.Home+"/.cache/forkmesh-release-host/build.log")
	touch(t, c.Home+"/.ccache/a/b")
	touch(t, c.Home+"/.cache/ccache/a/b")
	touch(t, c.TmpRoots[0]+"/fm123-build/x.o")
	touch(t, c.TmpRoots[0]+"/unrelated-file")
	touch(t, c.Home+"/.qttest/share/ForkMeshTests/WindowResize-qt_temp-1/x")
	age(t, c.Home, 4)
	age(t, c.TmpRoots[0], 4)
	c.CcacheDir = c.Home + "/.ccache"
	c.ClaimedSandboxes = []string{"forkmesh-run-1-100"}
	result := scan(c)

	attached := find(result.Items, CategoryBuildTemp, "Build · agent-5")
	if attached == nil || attached.Status != StatusAttached || !strings.Contains(attached.ReasonText(), "5-feature") {
		t.Errorf("an agent build dir whose worktree still exists is attached: %+v", attached)
	}
	orphan := find(result.Items, CategoryBuildTemp, "Build · agent-9")
	if orphan == nil || orphan.Status != StatusOrphaned || len(orphan.Members) != 3 || orphan.ReasonText() != "agent 9 build; no worktree of that name is left" {
		t.Errorf("an agent build dir, its tmp dir and its logs are one orphaned item: %+v", orphan)
	}
	if orphan != nil && orphan.ID != "3:Build · agent-9:"+c.BuildsDir+"/agent-9" {
		t.Errorf("build id %q", orphan.ID)
	}
	claimed := find(result.Items, CategoryBuildTemp, "Actions sandbox · forkmesh-run-1-100")
	finished := find(result.Items, CategoryBuildTemp, "Actions sandbox · forkmesh-run-2-200")
	if claimed == nil || claimed.Status != StatusInUse || finished == nil || finished.Status != StatusOrphaned || finished.Kind != KindActionSandbox {
		t.Error("only finished Actions sandboxes can go")
	}
	actionsCache := find(result.Items, CategoryBuildTemp, "Actions build cache")
	if actionsCache == nil || actionsCache.Kind != KindReport || actionsCache.Deletable() {
		t.Error("the Actions build cache is report only")
	}
	newest := find(result.Items, CategoryBuildTemp, "Playwright chromium-1100")
	older := find(result.Items, CategoryBuildTemp, "Playwright chromium-1000")
	if newest == nil || newest.Status != StatusAttached || older == nil || older.Status != StatusOrphaned {
		t.Error("only older Playwright browser builds are orphaned")
	}
	operatorCache := find(result.Items, CategoryOtherCaches, "fm-rewrite")
	if operatorCache == nil || !operatorCache.ConfirmEach || !operatorCache.NeedsConfirm() {
		t.Error("operator caches always need an explicit confirm")
	}
	release := find(result.Items, CategoryOtherCaches, "forkmesh-release-host")
	if release == nil || release.Status != StatusProtected {
		t.Error("release build folders are protected")
	}
	activeCache := find(result.Items, CategoryBuildTemp, "ccache · "+filepath.Clean(c.Home+"/.ccache"))
	idleCache := find(result.Items, CategoryBuildTemp, "ccache · "+filepath.Clean(c.Home+"/.cache/ccache"))
	if activeCache == nil || activeCache.Kind != KindReport || activeCache.Deletable() {
		t.Error("the active ccache is report only")
	}
	if idleCache == nil || !idleCache.Deletable() || !idleCache.NeedsConfirm() {
		t.Error("an idle ccache folder can go after a confirm")
	}
	temp := find(result.Items, CategoryBuildTemp, "Agent temp files")
	if temp == nil || !hasMember(temp, c.TmpRoots[0]+"/fm123-build") || hasMember(temp, c.TmpRoots[0]+"/unrelated-file") {
		t.Error("only agent temp files are listed from the temp folder")
	}
	qttest := find(result.Items, CategoryBuildTemp, "Qt test temp folders")
	if qttest == nil || len(qttest.Members) != 1 {
		t.Error("Qt test temp folders are listed")
	}
}

func TestProtectedItemsAreNeverDeletable(t *testing.T) {
	unixOnly(t)
	root := sandbox(t)
	c := baseContext(root)
	touch(t, c.DataDir+"/agent-accounts/codex/acct/projects/p/s.jsonl")
	_ = os.MkdirAll(c.WorktreesDir+"/clone/.git", 0o755)
	touch(t, c.WorktreesDir+"/clone/file")
	touch(t, c.Home+"/.cache/fm-checkout/.git/HEAD")
	age(t, c.Home, 5)
	result := scan(c)
	for _, title := range []string{"Desktop agent transcripts · codex acct", "clone", "fm-checkout"} {
		var item *Item
		for i := range result.Items {
			if result.Items[i].Title == title {
				item = &result.Items[i]
			}
		}
		if item == nil || item.Status != StatusProtected || item.Deletable() || PlanRefusal(*item, Choice{Confirmed: true}) != "protected" {
			t.Errorf("%s: %+v", title, item)
		}
	}
	if item := find(result.Items, CategoryCodex, "Desktop agent transcripts · codex acct"); item == nil {
		t.Error("codex accounts are listed under Codex")
	}
}

func TestDeletion(t *testing.T) {
	unixOnly(t)
	root := sandbox(t)
	parent := root + "/del/project"
	finished := parent + "/" + kOld + ".jsonl"
	finishedDir := parent + "/" + kOld
	live := parent + "/" + kLive + ".jsonl"
	fresh := parent + "/fresh.txt"
	outsideTarget := root + "/del/keep"
	link := parent + "/link"
	touch(t, finished)
	touch(t, finishedDir+"/sub/x")
	touch(t, live)
	touch(t, fresh)
	touch(t, outsideTarget+"/precious.txt")
	_ = os.Symlink(outsideTarget, link)
	age(t, parent, 30)
	touch(t, fresh)
	touch(t, root+"/del/sibling.txt")

	item := Item{ID: "x", Category: CategoryClaude, Kind: KindFiles, Title: "Transcripts", Path: parent, Parent: parent, Status: StatusOrphaned}
	for _, path := range []string{finished, finishedDir, live, fresh, link, root + "/del/sibling.txt"} {
		item.Members = append(item.Members, Member{Path: path, Bytes: 10})
	}
	item.Bytes = 60
	recheck := Recheck{Live: map[string]bool{kLive: true}, NowMs: time.Now().UnixMilli()}
	plan := BuildPlan([]Item{item}, nil)
	if len(plan.Files) != 1 || plan.Bytes != 60 {
		t.Fatal("an orphaned files item needs no confirm")
	}
	outcomes := DeleteFiles(plan.Files, recheck, nil)
	outcomeFor := func(path string) *Outcome {
		for i := range outcomes {
			if outcomes[i].Path == filepath.Clean(path) {
				return &outcomes[i]
			}
		}
		return nil
	}
	if exists(finished) || exists(finishedDir) {
		t.Error("finished transcripts and their folders are removed")
	}
	if o := outcomeFor(live); !exists(live) || o == nil || o.Removed || o.Error != "its session is live" {
		t.Errorf("a transcript whose session went live after the scan is kept: %+v", o)
	}
	if o := outcomeFor(fresh); !exists(fresh) || o == nil || o.Error != "changed in the last 30 minutes" {
		t.Errorf("a file that changed after the scan is kept: %+v", o)
	}
	if isSymlink(link) || !exists(outsideTarget+"/precious.txt") {
		t.Error("a link is removed without touching what it points at")
	}
	if o := outcomeFor(root + "/del/sibling.txt"); !exists(root+"/del/sibling.txt") || o == nil || o.Error != root+"/del/sibling.txt is not inside "+parent {
		t.Errorf("a member outside the item's folder is refused: %+v", o)
	}
	if FreedBytes(outcomes) != 30 {
		t.Errorf("freed bytes count only what was removed: %d", FreedBytes(outcomes))
	}
	log := root + "/logs/cleanup.log"
	if !AppendLog(log, outcomes, recheck.NowMs) {
		t.Fatal("the cleanup log is written")
	}
	text, _ := os.ReadFile(log)
	if !strings.Contains(string(text), "\tremoved\t10\t"+finished) || !strings.Contains(string(text), "\tkept\t") {
		t.Errorf("every deletion and refusal is logged with its path and size:\n%s", text)
	}
	if info, err := os.Stat(log); err != nil || info.Mode().Perm() != 0o600 {
		t.Error("the log is private")
	}
	stamp := time.UnixMilli(recheck.NowMs).Local().Format("2006-01-02T15:04:05")
	if got := LogLine(Outcome{Path: "/a\tb", Bytes: 7, Reason: "why\nnot", Error: "e"}, recheck.NowMs); got != stamp+"\tkept\t7\t/a b\twhy not\te" {
		t.Errorf("log line %q", got)
	}

	sandboxItem := Item{ID: "s", Kind: KindActionSandbox, Path: root + "/sandbox/forkmesh-run-2-200", Parent: root + "/sandbox", Status: StatusOrphaned,
		Members: []Member{{Path: root + "/sandbox/forkmesh-run-2-200", Bytes: 5}}, Bytes: 5}
	var seenBase, seenPath string
	sandboxOutcomes := DeleteSandboxes([]Step{{Item: sandboxItem}}, Recheck{}, func(base, path string) string {
		seenBase, seenPath = base, path
		return ""
	})
	if len(sandboxOutcomes) != 1 || !sandboxOutcomes[0].Removed || seenBase != root+"/sandbox" || seenPath != sandboxItem.Path {
		t.Error("Actions sandboxes go through the guarded sandbox removal")
	}
	if out := DeleteSandboxes([]Step{{Item: sandboxItem}}, Recheck{}, nil); out[0].Error != "the desktop is not available to remove it" {
		t.Errorf("no remover: %+v", out)
	}

	protectedItem, inUse, attached := item, item, item
	protectedItem.Status = StatusProtected
	inUse.Status = StatusInUse
	attached.Status = StatusAttached
	refused := BuildPlan([]Item{protectedItem, inUse, attached}, nil)
	if refused.Count() != 0 || len(refused.Skipped) != 3 || refused.Skipped[0].Why != "protected" || refused.Skipped[1].Why != "in use" || refused.Skipped[2].Why != "not confirmed" {
		t.Errorf("protected, in-use and unconfirmed attached items never reach the plan: %+v", refused.Skipped)
	}
	if text := Describe(BuildPlan([]Item{item}, nil)); !strings.HasPrefix(text, "1 item, 60 bytes reclaimed\n6 entries in "+parent) {
		t.Errorf("the plan summary says how much space comes back: %q", text)
	}
}

func TestDeletionGoesThroughTheGuardedHelper(t *testing.T) {
	unixOnly(t)
	root := sandbox(t)
	home := root + "/home"
	checkout := root + "/work/repo"
	_ = os.MkdirAll(checkout+"/.git", 0o755)
	touch(t, checkout+"/file")
	userData := home + "/projects/forkmesh/data/identity"
	touch(t, userData+"/ed25519.pem")
	extraData := root + "/layout/data/mirrors"
	touch(t, extraData+"/x")
	for _, path := range []string{checkout, userData, extraData + "/x"} {
		age(t, path, 5)
	}
	items := []Item{
		{ID: "a", Kind: KindFiles, Path: root + "/work", Parent: root + "/work", Status: StatusOrphaned, Members: []Member{{Path: checkout}}},
		{ID: "b", Kind: KindFiles, Path: home + "/projects/forkmesh/data", Parent: home + "/projects/forkmesh/data", Status: StatusOrphaned, Members: []Member{{Path: userData}}},
		{ID: "c", Kind: KindFiles, Path: root + "/layout/data", Parent: root + "/layout/data", Status: StatusOrphaned, Members: []Member{{Path: extraData + "/x"}}},
	}
	var steps []Step
	for _, item := range items {
		steps = append(steps, Step{Item: item})
	}
	outcomes := DeleteFiles(steps, Recheck{NowMs: time.Now().UnixMilli(), Protected: []string{root + "/layout/data"}}, nil)
	if len(outcomes) != 3 {
		t.Fatalf("outcomes %+v", outcomes)
	}
	if outcomes[0].Removed || !strings.Contains(outcomes[0].Error, "is a git checkout") || !exists(checkout+"/file") {
		t.Errorf("a checkout is refused by safedelete: %+v", outcomes[0])
	}
	if outcomes[1].Removed || !strings.Contains(outcomes[1].Error, "protected ForkMesh folder") || !exists(userData+"/ed25519.pem") {
		t.Errorf("the identity key folder is refused: %+v", outcomes[1])
	}
	if outcomes[2].Removed || !strings.Contains(outcomes[2].Error, "protected ForkMesh folder "+root+"/layout/data") || !exists(extraData+"/x") {
		t.Errorf("the core's own data folder is refused: %+v", outcomes[2])
	}
	for _, file := range []string{"scan.go", "plan.go", "jobs.go", "measure_unix.go", "measure_windows.go"} {
		source, err := os.ReadFile(file)
		if err != nil {
			t.Fatal(err)
		}
		for _, banned := range []string{"os.RemoveAll", "os.Remove(", "syscall.Unlink", "syscall.Rmdir"} {
			if strings.Contains(string(source), banned) {
				t.Errorf("%s calls %s; deletes go through safedelete only", file, banned)
			}
		}
	}
}

func TestMemberRefusals(t *testing.T) {
	unixOnly(t)
	root := sandbox(t)
	real := root + "/real"
	touch(t, real+"/inner/file")
	_ = os.Symlink(real, root+"/linked")
	age(t, real, 3)
	item := Item{Parent: root + "/linked", Status: StatusOrphaned, Kind: KindFiles}
	if got := MemberRefusal(item, Member{Path: root + "/linked/inner"}, Recheck{}); got != "a link sits between "+root+"/linked and "+root+"/linked/inner" {
		t.Errorf("link parent: %q", got)
	}
	item.Parent = root
	if got := MemberRefusal(item, Member{Path: root + "/linked/inner/file"}, Recheck{}); got != "a link sits between "+root+" and "+root+"/linked/inner/file" {
		t.Errorf("link between: %q", got)
	}
	if got := MemberRefusal(item, Member{Path: root + "/missing"}, Recheck{}); got != "already gone" {
		t.Errorf("gone: %q", got)
	}
	if got := MemberRefusal(Item{}, Member{Path: root + "/real"}, Recheck{}); got != "no expected parent folder" {
		t.Errorf("no parent: %q", got)
	}
	if got := MemberRefusal(item, Member{Path: root}, Recheck{}); got != root+" is not inside "+root {
		t.Errorf("parent itself: %q", got)
	}
	busy := Recheck{Processes: ProcessSnapshot{Processes: []Process{{PID: 5, Comm: "vim", OpenPaths: []string{real + "/inner/file"}}}}}
	if got := MemberRefusal(item, Member{Path: real}, busy); got != "now open in vim (PID 5)" {
		t.Errorf("busy: %q", got)
	}
	if got := WorktreeRefusal(Item{Kind: KindFiles}, Recheck{}); got != "not a worktree" {
		t.Errorf("not a worktree: %q", got)
	}
	if got := WorktreeRefusal(Item{Kind: KindWorktree, Path: root + "/missing"}, Recheck{}); got != "already gone" {
		t.Errorf("worktree gone: %q", got)
	}
}
