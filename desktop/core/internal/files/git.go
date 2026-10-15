package files

import (
	"context"
	"os"
	"os/exec"
	"strings"
	"sync"
	"time"

	"forkmesh.com/desktop-core/internal/git"
	"forkmesh.com/desktop-core/internal/rpc"
)

type limitedBuffer struct {
	mu    sync.Mutex
	limit int
	data  []byte
}

func (b *limitedBuffer) Write(p []byte) (int, error) {
	b.mu.Lock()
	defer b.mu.Unlock()
	if room := b.limit - len(b.data); room > 0 {
		if len(p) < room {
			room = len(p)
		}
		b.data = append(b.data, p[:room]...)
	}
	return len(p), nil
}

func (b *limitedBuffer) bytes() []byte {
	b.mu.Lock()
	defer b.mu.Unlock()
	return append([]byte(nil), b.data...)
}

func gitEnvironment() []string {
	return append(os.Environ(), "GIT_TERMINAL_PROMPT=0", "GIT_OPTIONAL_LOCKS=0", "LC_ALL=C")
}

func (m *Module) gitCommand(dir string, args []string) *exec.Cmd {
	cmd := exec.Command(m.Git, args...)
	cmd.Dir = dir
	cmd.Env = gitEnvironment()
	cmd.WaitDelay = 2 * time.Second
	return cmd
}

func waitTimeout(cmd *exec.Cmd, timeout time.Duration, cancel <-chan struct{}) (error, bool) {
	done := make(chan error, 1)
	go func() { done <- cmd.Wait() }()
	timer := time.NewTimer(timeout)
	defer timer.Stop()
	select {
	case err := <-done:
		return err, false
	case <-timer.C:
	case <-cancel:
	}
	if cmd.Process != nil {
		_ = cmd.Process.Kill()
	}
	<-done
	return nil, true
}

func (m *Module) gitCapture(ctx context.Context, dir string, args []string) ([]byte, string) {
	cmd := m.gitCommand(dir, args)
	output := &limitedBuffer{limit: maxGitOutputBytes}
	cmd.Stdout = output
	if err := cmd.Start(); err != nil {
		return nil, "git_unavailable"
	}
	err, timedOut := waitTimeout(cmd, m.StatusTimeout, ctx.Done())
	if timedOut {
		return nil, "timeout"
	}
	if err != nil {
		return nil, "git_status_failed"
	}
	return output.bytes(), ""
}

type gitRun struct {
	mu     sync.Mutex
	cmd    *exec.Cmd
	cancel context.CancelFunc
	killed bool
}

func (r *gitRun) kill() {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.killed = true
	if r.cancel != nil {
		r.cancel()
	}
	if r.cmd != nil && r.cmd.Process != nil {
		_ = r.cmd.Process.Kill()
	}
}

func (m *Module) laneRunner() *git.Runner {
	if m.Lanes != nil {
		return m.Lanes
	}
	if m.core != nil {
		if runner, ok := m.core.Get(git.SharedRunnerKey).(*git.Runner); ok && runner != nil {
			return runner
		}
	}
	return git.Default
}

func decodeUTF8(data []byte) string {
	return string([]rune(string(data)))
}

func (m *Module) runGit(peer rpc.Peer, action, rootID string) (any, error) {
	rootPath, label := m.fileRootPath(rootID)
	args := gitArguments(action)
	if rootPath == "" || len(args) == 0 {
		return nil, rpc.Fail("unknown_root")
	}
	key := action + ":" + strings.ToLower(rootID)
	run := &gitRun{}
	m.mu.Lock()
	if _, busy := m.running[key]; busy {
		m.mu.Unlock()
		return nil, rpc.Fail("already_running")
	}
	m.running[key] = run
	m.mu.Unlock()
	m.logf("git %s of %s requested by %s.", action, label, deviceLabel(peer))

	finish := func(ok bool, output string) {
		m.mu.Lock()
		current, present := m.running[key]
		if !present || current != run {
			m.mu.Unlock()
			return
		}
		delete(m.running, key)
		m.mu.Unlock()
		message := strings.TrimSpace(output)
		if message == "" {
			if ok {
				message = "Done."
			} else {
				message = "git " + action + " failed."
			}
		} else {
			message = rightUTF16(message, 1500)
		}
		state := "failed"
		if ok {
			state = "finished"
		}
		m.logf("git %s of %s from the phone %s.", action, label, state)
		phones := func(p rpc.Peer) bool { return rpc.Remote(p) }
		m.core.Hub.Broadcast("files.browse.action", map[string]any{"action": action, "root": rootID, "ok": ok,
			"message": message}, phones)
		m.core.Hub.Broadcast("files.browse.changed", map[string]any{"root": rootID}, phones)
	}

	ctx, cancel := context.WithCancel(context.Background())
	run.mu.Lock()
	run.cancel = cancel
	run.mu.Unlock()
	started := time.Now()
	go func() {
		defer cancel()
		settle := func() {
			if wait := m.StartDelay - time.Since(started); wait > 0 {
				time.Sleep(wait)
			}
		}
		if git.WritesIndex(args) {
			_, lease, err := m.laneRunner().Hold(ctx, rootPath)
			if err != nil {
				settle()
				finish(false, "git timed out.")
				return
			}
			defer lease.Release()
		}
		cmd := m.gitCommand(rootPath, args)
		output := &limitedBuffer{limit: maxGitOutputBytes}
		cmd.Stdout = output
		cmd.Stderr = output
		run.mu.Lock()
		if run.killed {
			run.mu.Unlock()
			settle()
			finish(false, "git timed out.")
			return
		}
		run.cmd = cmd
		startErr := cmd.Start()
		run.mu.Unlock()
		if startErr != nil {
			settle()
			finish(false, "git is not available on the desktop.")
			return
		}
		err, timedOut := waitTimeout(cmd, m.GitTimeout, nil)
		settle()
		if timedOut {
			finish(false, "git timed out.")
			return
		}
		finish(err == nil, decodeUTF8(output.bytes()))
	}()
	message := "Fetching " + label + "."
	if action == "pull" {
		message = "Pulling " + label + "."
	}
	return map[string]any{"started": true, "action": action, "subject": rootID, "message": message}, nil
}
