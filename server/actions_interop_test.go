package mirrornode

import (
	"context"
	"os"
	"path/filepath"
	"strconv"
	"testing"
	"time"
)

// The Go half of app/tests/test_actions_go_runner_interop.py. That test runs
// the real Python gateway and this real runner against one directory: the
// gateway couriers a router-signed control request in, this consumes it, and
// the gateway then reads the summary this writes. Run on its own it skips.
//
//	FORKMESH_ACTIONS_INTEROP_DIR    the gateway's 0700 state directory
//	FORKMESH_ACTIONS_INTEROP_PHASE  "prepare" | "consume"
//	FORKMESH_ACTIONS_INTEROP_NODE   node name the gateway serves as
//	FORKMESH_ACTIONS_INTEROP_ROUTER router public key, base64url
//	FORKMESH_ACTIONS_INTEROP_BARE   the served bare repository
//	FORKMESH_ACTIONS_INTEROP_NOW    the gateway's clock, in milliseconds
func TestActionsInteropWithThePythonGateway(t *testing.T) {
	directory := os.Getenv("FORKMESH_ACTIONS_INTEROP_DIR")
	if directory == "" {
		t.Skip("driven by app/tests/test_actions_go_runner_interop.py")
	}
	nowMs, err := strconv.ParseInt(os.Getenv("FORKMESH_ACTIONS_INTEROP_NOW"), 10, 64)
	if err != nil {
		t.Fatal(err)
	}
	bare := os.Getenv("FORKMESH_ACTIONS_INTEROP_BARE")
	identity, _ := testIdentity(t)
	var gateway GatewayConfig
	gateway.Node.Name = os.Getenv("FORKMESH_ACTIONS_INTEROP_NODE")
	gateway.Node.PublicKey = identity.PublicKey()
	gateway.RouterPublicKey = os.Getenv("FORKMESH_ACTIONS_INTEROP_ROUTER")
	gateway.Repositories = []Repository{{Owner: "alice", Name: "project",
		Visibility: "public", Enabled: true, GitDir: bare}}
	daemon := &Daemon{
		config:   Config{GatewayConfig: filepath.Join(directory, "config.json"), EnableActions: true},
		gateway:  gateway,
		identity: identity,
	}
	runner := newActionsRunner(daemon)
	runner.now = func() time.Time { return time.UnixMilli(nowMs) }
	runner.probeSandbox = func(context.Context) error { return nil }
	runner.sandbox = func(work string, env []string) (string, []string, error) {
		args := []string{"-i", "-C", filepath.Join(work, "src"), "PATH=/usr/bin:/bin"}
		return "env", append(args, env...), nil
	}
	ctx, cancel := context.WithCancel(context.Background())
	defer func() { cancel(); runner.wait() }()
	runner.runCtx = ctx
	if !runner.available(ctx) {
		t.Fatal("sandbox seam should be available")
	}
	f := &actionsFixture{t: t, daemon: daemon, runner: runner, bare: bare}

	switch os.Getenv("FORKMESH_ACTIONS_INTEROP_PHASE") {
	case "prepare":
		// Commit a workflow to the served repository, then publish the first
		// summary: the gateway refuses control requests until it can read a
		// version 2 document.
		// `git init --bare` may leave HEAD on an unborn branch of another
		// name; the fixture's commits are on main.
		run(t, bare, "git", "symbolic-ref", "HEAD", "refs/heads/main")
		branch := "main"
		f.source = filepath.Join(t.TempDir(), "source")
		run(t, filepath.Dir(f.source), "git", "clone", "-q", bare, f.source)
		run(t, f.source, "git", "config", "user.email", "test@example.test")
		run(t, f.source, "git", "config", "user.name", "Test")
		f.push(branch, map[string]string{".forkmesh/ci.yml": ciWorkflow})
		runner.scanRefs(ctx)
		runner.writeSummary(ctx)
	case "consume":
		runner.scanControl(ctx)
		f.settle()
		runner.writeSummary(ctx)
	default:
		t.Fatal("unknown interop phase")
	}
}
