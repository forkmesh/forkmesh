package repos

import (
	"context"
	"encoding/base64"
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"forkmesh.com/desktop-core/internal/coretest"
	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/rpc"
)

func callRun(t *testing.T, core *daemon.Core, peer rpc.Peer, params map[string]any) runReply {
	t.Helper()
	var reply runReply
	coretest.MustCall(t, core, peer, "core.git.run", params, &reply)
	return reply
}

func TestGitRunValidation(t *testing.T) {
	r := runner(t)
	_, clone := fixture(t, r)
	core := start(t, &Module{Git: r})
	ui := coretest.NewPeer("ui", rpc.UI)
	phone := coretest.NewPeer("p", rpc.LAN)
	if _, err := coretest.Call(t, core, phone, "core.git.run", map[string]any{"path": clone, "args": []string{"status"}}); rpc.Code(err) != "forbidden" {
		t.Fatalf("phone: %v", err)
	}
	bad := []map[string]any{
		{"path": "relative", "args": []string{"status"}},
		{"path": clone},
		{"path": clone, "args": []string{}},
		{"path": clone, "args": []string{"-c", "x=y"}},
		{"path": clone, "args": []string{"status", "a\x00b"}},
		{"path": clone, "args": []string{"status"}, "timeoutMs": -1},
		{"path": clone, "args": []string{"status"}, "inputBase64": "!!"},
		{"path": clone, "args": []string{"status"}, "env": []string{"NOEQUALS"}},
		{"path": clone, "args": []string{"status"}, "env": []string{"=x"}},
		{"path": clone, "args": []string{"status"}, "env": []string{"1BAD=x"}},
		{"path": clone, "args": []string{"status"}, "env": []string{"A-B=x"}},
		{"path": clone, "args": []string{"status"}, "env": []string{"A=x\x00y"}},
	}
	for _, params := range bad {
		if _, err := coretest.Call(t, core, ui, "core.git.run", params); rpc.Code(err) != "bad_request" {
			t.Errorf("%v: %v", params, err)
		}
	}
	if _, err := coretest.Call(t, core, ui, "core.git.run", map[string]any{"path": filepath.Join(clone, "missing"), "args": []string{"status"}}); rpc.Code(err) != "no_directory" {
		t.Errorf("missing dir: %v", err)
	}
	if !core.Registry.Native("core.git.run") || !core.Registry.Native("core.git.agentDiffStats") {
		t.Fatal("not registered")
	}
}

func TestGitRunReadsWritesAndInput(t *testing.T) {
	r := runner(t)
	_, clone := fixture(t, r)
	core := start(t, &Module{Git: r})
	ui := coretest.NewPeer("ui", rpc.UI)

	read := callRun(t, core, ui, map[string]any{"path": clone, "args": []string{"rev-parse", "--abbrev-ref", "HEAD"}})
	if !read.OK || read.Code != 0 || strings.TrimSpace(read.Stdout) != "main" || read.Write {
		t.Fatalf("read: %+v", read)
	}
	failed := callRun(t, core, ui, map[string]any{"path": clone, "args": []string{"rev-parse", "--verify", "--quiet", "refs/heads/nope"}})
	if failed.OK || failed.Code != 1 || failed.TimedOut {
		t.Fatalf("failed read: %+v", failed)
	}

	if err := os.WriteFile(filepath.Join(clone, "new.txt"), []byte("new\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	added := callRun(t, core, ui, map[string]any{"path": clone, "args": []string{"add", "new.txt"}})
	if !added.OK || !added.Write || added.Attempts != 1 {
		t.Fatalf("add: %+v", added)
	}
	committed := callRun(t, core, ui, map[string]any{"path": clone, "args": []string{"commit", "-q", "-m", "via core"}, "write": true})
	if !committed.OK {
		t.Fatalf("commit: %+v", committed)
	}
	subject := callRun(t, core, ui, map[string]any{"path": clone, "args": []string{"log", "-1", "--format=%s"}})
	if strings.TrimSpace(subject.Stdout) != "via core" {
		t.Fatalf("log: %+v", subject)
	}
	nothing := callRun(t, core, ui, map[string]any{"path": clone, "args": []string{"commit", "-q", "-m", "empty"}})
	if nothing.OK || nothing.Code == 0 {
		t.Fatalf("empty commit should fail: %+v", nothing)
	}

	hashed := callRun(t, core, ui, map[string]any{"path": clone, "args": []string{"hash-object", "--stdin"},
		"inputBase64": base64.StdEncoding.EncodeToString([]byte("hello\n"))})
	if strings.TrimSpace(hashed.Stdout) != "ce013625030ba8dba906f756967f9e9ca394464a" {
		t.Fatalf("hash-object: %+v", hashed)
	}
	plain := callRun(t, core, ui, map[string]any{"path": clone, "args": []string{"hash-object", "--stdin"}, "input": "hello\n"})
	if plain.Stdout != hashed.Stdout {
		t.Fatalf("plain input: %+v", plain)
	}

	blob := []byte{0xff, 0xfe, 0x00, 'x', '\n'}
	stored := callRun(t, core, ui, map[string]any{"path": clone, "args": []string{"hash-object", "-w", "--stdin"},
		"inputBase64": base64.StdEncoding.EncodeToString(blob)})
	binary := callRun(t, core, ui, map[string]any{"path": clone, "args": []string{"cat-file", "blob", strings.TrimSpace(stored.Stdout)}})
	decoded, err := base64.StdEncoding.DecodeString(binary.StdoutBase64)
	if err != nil || binary.Stdout != "" || string(decoded) != string(blob) {
		t.Fatalf("binary stdout: %+v", binary)
	}
}

func TestGitRunSharesTheIndexQueue(t *testing.T) {
	r := runner(t)
	_, clone := fixture(t, r)
	module := &Module{Git: r}
	core := start(t, module)
	ui := coretest.NewPeer("ui", rpc.UI)

	_, lease, err := r.Hold(context.Background(), clone)
	if err != nil {
		t.Fatal(err)
	}
	type outcome struct {
		reply runReply
		at    time.Time
	}
	call := func(params map[string]any) chan outcome {
		done := make(chan outcome, 1)
		go func() {
			raw, err := json.Marshal(params)
			if err != nil {
				t.Error(err)
				return
			}
			result, err := core.Registry.Dispatch(context.Background(), &rpc.Request{Peer: ui, ID: 1, Method: "core.git.run", Params: raw})
			if err != nil {
				t.Error(err)
				done <- outcome{}
				return
			}
			done <- outcome{reply: result.(runReply), at: time.Now()}
		}()
		return done
	}
	worktree := filepath.Join(t.TempDir(), "wt")
	writer := call(map[string]any{"path": clone, "args": []string{"worktree", "add", "-q", worktree, "feature"}, "write": true})
	forced := call(map[string]any{"path": clone, "args": []string{"rev-parse", "HEAD"}, "write": true})
	reader := call(map[string]any{"path": clone, "args": []string{"rev-parse", "HEAD"}})
	select {
	case got := <-reader:
		if !got.reply.OK || got.reply.Write {
			t.Fatalf("reader: %+v", got.reply)
		}
	case <-time.After(10 * time.Second):
		t.Fatal("a read waited for the index queue")
	}
	select {
	case got := <-writer:
		t.Fatalf("writer ran while the lane was held: %+v", got.reply)
	case got := <-forced:
		t.Fatalf("write:true ran while the lane was held: %+v", got.reply)
	case <-time.After(300 * time.Millisecond):
	}
	if !module.Busy() {
		t.Fatal("queued writes should keep the module busy")
	}
	released := time.Now()
	lease.Release()
	for _, done := range []chan outcome{writer, forced} {
		select {
		case got := <-done:
			if !got.reply.OK || !got.reply.Write || got.at.Before(released) || got.reply.QueuedMs < 250 {
				t.Fatalf("queued write: %+v", got.reply)
			}
		case <-time.After(20 * time.Second):
			t.Fatal("queued write never ran")
		}
	}
	if _, err := os.Stat(filepath.Join(worktree, "c.txt")); err != nil {
		t.Fatalf("worktree add did not run: %v", err)
	}
}

func TestGitRunEnvironment(t *testing.T) {
	r := runner(t)
	_, clone := fixture(t, r)
	core := start(t, &Module{Git: r})
	ui := coretest.NewPeer("ui", rpc.UI)
	env := []string{"GIT_CONFIG_COUNT=1", "GIT_CONFIG_KEY_0=forkmesh.probe", "GIT_CONFIG_VALUE_0=from-desktop", "LC_ALL="}
	read := callRun(t, core, ui, map[string]any{"path": clone, "args": []string{"config", "--get", "forkmesh.probe"}, "env": env})
	if !read.OK || strings.TrimSpace(read.Stdout) != "from-desktop" {
		t.Fatalf("env read: %+v", read)
	}
	write := callRun(t, core, ui, map[string]any{"path": clone, "args": []string{"config", "--get", "forkmesh.probe"}, "env": env, "write": true})
	if !write.OK || !write.Write || strings.TrimSpace(write.Stdout) != "from-desktop" {
		t.Fatalf("env write: %+v", write)
	}
	plain := callRun(t, core, ui, map[string]any{"path": clone, "args": []string{"config", "--get", "forkmesh.probe"}})
	if plain.OK {
		t.Fatalf("the environment is per call: %+v", plain)
	}
}
