package repos

import (
	"encoding/json"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
	"time"

	"forkmesh.com/desktop-core/internal/coretest"
	"forkmesh.com/desktop-core/internal/git"
	"forkmesh.com/desktop-core/internal/rpc"
)

func writeFile(t *testing.T, path, text string) {
	t.Helper()
	if err := os.WriteFile(path, []byte(text), 0o644); err != nil {
		t.Fatal(err)
	}
}

func commitAll(t *testing.T, r *git.Runner, dir, message string) {
	t.Helper()
	must(t, r, dir, nil, "add", "-A")
	must(t, r, dir, nil, "commit", "-q", "-m", message)
}

type agentFixture struct {
	repo     string
	worktree string
	patch    string
}

func newAgentFixture(t *testing.T, r *git.Runner) agentFixture {
	t.Helper()
	repo := t.TempDir()
	must(t, r, repo, nil, "init", "-q", "-b", "main")
	writeFile(t, filepath.Join(repo, "a.txt"), "1\n2\n3\n")
	writeFile(t, filepath.Join(repo, "b.txt"), "b\n")
	commitAll(t, r, repo, "initial")
	must(t, r, repo, nil, "branch", "agent/1")
	must(t, r, repo, nil, "branch", "agent/3")
	must(t, r, repo, nil, "branch", "agent/4")
	worktree := filepath.Join(t.TempDir(), "wt1")
	must(t, r, repo, nil, "worktree", "add", "-q", worktree, "agent/1")
	writeFile(t, filepath.Join(worktree, "a.txt"), "1\nTWO\n3\n4\n")
	writeFile(t, filepath.Join(worktree, "c.txt"), "c1\nc2\nc3\n")
	commitAll(t, r, worktree, "agent one")
	must(t, r, repo, nil, "checkout", "-q", "agent/4")
	writeFile(t, filepath.Join(repo, "b.txt"), "b\nagent\n")
	commitAll(t, r, repo, "agent four")
	must(t, r, repo, nil, "checkout", "-q", "agent/3")
	writeFile(t, filepath.Join(repo, "d.txt"), "d\n")
	commitAll(t, r, repo, "agent three")
	must(t, r, repo, nil, "checkout", "-q", "main")
	writeFile(t, filepath.Join(repo, "b.txt"), "b\nmain\n")
	commitAll(t, r, repo, "main moves")
	writeFile(t, filepath.Join(worktree, "a.txt"), "1\nTWO\n3\n4\n5\n")
	writeFile(t, filepath.Join(worktree, "u.txt"), "x\ny")
	patch := filepath.Join(t.TempDir(), "changes.patch")
	writeFile(t, patch, "diff --git a/x b/x\r\n--- a/x\n+++ b/x\n+new\n-old\n+more\n")
	return agentFixture{repo: repo, worktree: worktree, patch: patch}
}

type agentDiffReply struct {
	Base       string               `json:"base"`
	Token      string               `json:"token"`
	Stats      []agentDiffEntry     `json:"stats"`
	Signatures []agentDiffSignature `json:"signatures"`
	LiveIDs    []int                `json:"liveIds"`
}

func sameDir(a, b string) bool {
	ra, errA := filepath.EvalSymlinks(a)
	rb, errB := filepath.EvalSymlinks(b)
	return errA == nil && errB == nil && ra == rb
}

func TestAgentDiffStatsParity(t *testing.T) {
	r := runner(t)
	fx := newAgentFixture(t, r)
	module := &Module{Git: r}
	core := start(t, module)
	ui := coretest.NewPeer("ui", rpc.UI)
	tips := module.AgentBranchTips(t.Context(), fx.repo)
	skipped := AgentDiffSession{ID: 3, Status: "success", Branch: "agent/3", Known: true}
	skipped.Signature = AgentDiffSignature(skipped, "main", tips)
	params := map[string]any{
		"path": fx.repo, "selected": 1, "token": "gen-7",
		"sessions": []AgentDiffSession{
			{ID: 1, Status: "running", Branch: "agent/1", PatchPath: fx.patch, Known: true},
			{ID: 2, Status: "success", Branch: "forkmesh/pull/5"},
			skipped,
			{ID: 4, Status: "success", Branch: "agent/4", BaseBranch: "nope", FinishedAtMs: 42, PRNumber: 9},
			{ID: 5, Status: "failed", Branch: "agent/gone", PatchPath: fx.patch, Merged: true},
		},
	}
	var reply agentDiffReply
	coretest.MustCall(t, core, ui, "core.git.agentDiffStats", params, &reply)
	if reply.Base != "main" || reply.Token != "gen-7" || len(reply.LiveIDs) != 5 || len(reply.Signatures) != 5 {
		t.Fatalf("reply: %+v", reply)
	}
	stats := map[int]AgentDiffStat{}
	for _, entry := range reply.Stats {
		stats[entry.ID] = entry.Stat
	}
	if _, ok := stats[3]; ok || len(stats) != 4 {
		t.Fatalf("session 3 is unchanged and should not be re-read: %+v", reply.Stats)
	}
	one := stats[1]
	if !sameDir(one.Worktree, fx.worktree) {
		t.Fatalf("worktree %q, want %q", one.Worktree, fx.worktree)
	}
	one.Worktree = ""
	if want := (AgentDiffStat{Files: 3, Ahead: 1, Behind: 1, Base: "main", Dirty: 2, Added: 8, Removed: 1}); one != want {
		t.Errorf("session 1 = %+v, want %+v", one, want)
	}
	if want := newAgentDiffStat(); stats[2] != want {
		t.Errorf("pull branch = %+v", stats[2])
	}
	if want := (AgentDiffStat{Files: 1, Ahead: 1, Behind: 1, Base: "main", Dirty: -1, Added: 1, Removed: 1, Conflicted: true}); stats[4] != want {
		t.Errorf("session 4 = %+v, want %+v", stats[4], want)
	}
	if want := (AgentDiffStat{Files: 1, Ahead: -1, Behind: -1, Dirty: -1, Added: 2, Removed: 1}); stats[5] != want {
		t.Errorf("session 5 = %+v, want %+v", stats[5], want)
	}
	for _, sig := range reply.Signatures {
		if sig.ID == 4 && sig.Signature != "success|agent/4|main|"+tips["main"]+"|0|42|9|"+tips["agent/4"] {
			t.Errorf("signature %q", sig.Signature)
		}
	}
	seen := map[int]bool{}
	for len(seen) < 4 {
		var event struct {
			Token string        `json:"token"`
			ID    int           `json:"id"`
			Stat  AgentDiffStat `json:"stat"`
		}
		if err := json.Unmarshal(ui.Next(t, AgentDiffStatEvent, 5*time.Second), &event); err != nil {
			t.Fatal(err)
		}
		if event.Token != "gen-7" {
			t.Fatalf("event token %q", event.Token)
		}
		seen[event.ID] = true
	}
	if seen[3] {
		t.Fatal("event for a skipped session")
	}
	phone := coretest.NewPeer("p", rpc.LAN)
	if _, err := coretest.Call(t, core, phone, "core.git.agentDiffStats", params); rpc.Code(err) != "forbidden" {
		t.Fatalf("phone: %v", err)
	}
	if _, err := coretest.Call(t, core, ui, "core.git.agentDiffStats", map[string]any{"path": ""}); rpc.Code(err) != "bad_request" {
		t.Fatalf("empty path: %v", err)
	}
}

func TestAgentDiffStatsCacheByTips(t *testing.T) {
	if runtime.GOOS == "windows" {
		t.Skip("shell wrapper")
	}
	r := runner(t)
	fx := newAgentFixture(t, r)
	real, err := exec.LookPath("git")
	if err != nil {
		t.Skip("git not installed")
	}
	calls := filepath.Join(t.TempDir(), "calls.txt")
	wrapper := filepath.Join(t.TempDir(), "git-wrapper")
	writeFile(t, wrapper, "#!/bin/sh\nprintf '%s\\n' \"$*\" >> '"+calls+"'\nexec '"+real+"' \"$@\"\n")
	if err := os.Chmod(wrapper, 0o755); err != nil {
		t.Fatal(err)
	}
	counted := &git.Runner{Binary: wrapper, BaseEnv: r.BaseEnv}
	core := start(t, &Module{Git: counted})
	ui := coretest.NewPeer("ui", rpc.UI)
	params := map[string]any{
		"path": fx.repo, "selected": 4,
		"sessions": []AgentDiffSession{
			{ID: 1, Status: "running", Branch: "agent/1"},
			{ID: 4, Status: "success", Branch: "agent/4"},
		},
	}
	count := func(prefix string) int {
		data, _ := os.ReadFile(calls)
		n := 0
		for _, line := range strings.Split(string(data), "\n") {
			if strings.Contains(line, prefix) {
				n++
			}
		}
		return n
	}
	var first, second, third agentDiffReply
	coretest.MustCall(t, core, ui, "core.git.agentDiffStats", params, &first)
	logs, revs, merges := count(" log --no-merges"), count(" rev-list"), count(" merge-tree")
	if logs != 2 || revs != 2 || merges != 1 {
		t.Fatalf("first pass ran log=%d rev-list=%d merge-tree=%d", logs, revs, merges)
	}
	coretest.MustCall(t, core, ui, "core.git.agentDiffStats", params, &second)
	if count(" log --no-merges") != logs || count(" rev-list") != revs || count(" merge-tree") != merges {
		t.Fatal("unchanged tips should be served from the cache")
	}
	if count(" status --porcelain") != 2 {
		t.Fatal("the worktree status must be read on every pass")
	}
	firstJSON, _ := json.Marshal(first.Stats)
	secondJSON, _ := json.Marshal(second.Stats)
	if string(firstJSON) != string(secondJSON) {
		t.Fatalf("cached result differs:\n%s\n%s", firstJSON, secondJSON)
	}
	writeFile(t, filepath.Join(fx.repo, "e.txt"), "e\n")
	commitAll(t, r, fx.repo, "main moves again")
	coretest.MustCall(t, core, ui, "core.git.agentDiffStats", params, &third)
	if count(" log --no-merges") != logs+2 {
		t.Fatal("a moved base must be read again")
	}
	for _, entry := range third.Stats {
		if entry.Stat.Behind != 2 {
			t.Fatalf("after the base moved: %+v", entry)
		}
	}
}
