package hosts

import (
	"context"
	"os"
	"os/exec"
	"strings"
	"sync"
	"time"
)

const (
	mirrorStdoutCap = 1 << 20
	mirrorStderrCap = 256 << 10
)

type headBuffer struct {
	mu    sync.Mutex
	data  []byte
	limit int
}

func (b *headBuffer) Write(p []byte) (int, error) {
	b.mu.Lock()
	defer b.mu.Unlock()
	if room := b.limit - len(b.data); room > 0 {
		if len(p) > room {
			b.data = append(b.data, p[:room]...)
		} else {
			b.data = append(b.data, p...)
		}
	}
	return len(p), nil
}

func (b *headBuffer) String() string {
	b.mu.Lock()
	defer b.mu.Unlock()
	return strings.ToValidUTF8(string(b.data), "�")
}

type mirrorProc struct {
	program   string
	args      []string
	env       []string
	dir       string
	stdinPath string
	timeout   time.Duration
	stdout    bool
}

type mirrorProcResult struct {
	started    bool
	normalExit bool
	exitCode   int
	stdout     string
	stderr     string
}

func (r mirrorProcResult) ok() bool { return r.started && r.normalExit && r.exitCode == 0 }

func runMirrorProc(ctx context.Context, spec mirrorProc) mirrorProcResult {
	runCtx, cancel := context.WithCancel(ctx)
	if spec.timeout > 0 {
		runCtx, cancel = context.WithTimeout(ctx, spec.timeout)
	}
	defer cancel()
	cmd := exec.CommandContext(runCtx, spec.program, spec.args...)
	cmd.Env = spec.env
	cmd.Dir = spec.dir
	cmd.WaitDelay = time.Second
	if spec.stdinPath != "" {
		file, err := os.Open(spec.stdinPath)
		if err != nil {
			return mirrorProcResult{exitCode: -1}
		}
		defer file.Close()
		cmd.Stdin = file
	}
	stdout := &headBuffer{limit: mirrorStdoutCap}
	if spec.stdout {
		cmd.Stdout = stdout
	}
	stderr := &headBuffer{limit: mirrorStderrCap}
	cmd.Stderr = stderr
	if err := cmd.Start(); err != nil {
		return mirrorProcResult{exitCode: -1}
	}
	_ = cmd.Wait()
	result := mirrorProcResult{started: true, exitCode: -1, stdout: stdout.String(), stderr: stderr.String()}
	if state := cmd.ProcessState; state != nil {
		if code := state.ExitCode(); code >= 0 {
			result.normalExit = true
			result.exitCode = code
		}
	}
	if runCtx.Err() != nil {
		result.normalExit = false
	}
	return result
}
