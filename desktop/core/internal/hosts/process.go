package hosts

import (
	"bytes"
	"context"
	"errors"
	"io"
	"os/exec"
	"runtime"
	"sync"
	"syscall"
	"time"
)

type procSpec struct {
	Program  string
	Args     []string
	Env      []string
	Dir      string
	Stdin    []byte
	Timeout  time.Duration
	Graceful bool
	OnOutput func(chunk []byte)
	Stderr   func(chunk []byte)
}

type procResult struct {
	Started  bool
	Crashed  bool
	TimedOut bool
	ExitCode int
}

func (r procResult) OK() bool { return r.Started && !r.Crashed && !r.TimedOut && r.ExitCode == 0 }

type chunkWriter struct {
	mu   *sync.Mutex
	emit func([]byte)
}

func (w *chunkWriter) Write(p []byte) (int, error) {
	if w.emit != nil && len(p) > 0 {
		w.mu.Lock()
		w.emit(append([]byte(nil), p...))
		w.mu.Unlock()
	}
	return len(p), nil
}

func runProcess(ctx context.Context, spec procSpec) procResult {
	runCtx := ctx
	var cancel context.CancelFunc
	if spec.Timeout > 0 {
		runCtx, cancel = context.WithTimeout(ctx, spec.Timeout)
	} else {
		runCtx, cancel = context.WithCancel(ctx)
	}
	defer cancel()
	cmd := exec.CommandContext(runCtx, spec.Program, spec.Args...)
	cmd.Env = spec.Env
	cmd.Dir = spec.Dir
	if spec.Stdin != nil {
		cmd.Stdin = bytes.NewReader(spec.Stdin)
	} else {
		cmd.Stdin = nil
	}
	serial := &sync.Mutex{}
	out := &chunkWriter{mu: serial, emit: spec.OnOutput}
	cmd.Stdout = out
	if spec.Stderr != nil {
		cmd.Stderr = &chunkWriter{mu: serial, emit: spec.Stderr}
	} else {
		cmd.Stderr = out
	}
	if spec.Graceful && runtime.GOOS != "windows" {
		cmd.Cancel = func() error { return cmd.Process.Signal(syscall.SIGTERM) }
		cmd.WaitDelay = 3 * time.Second
	} else {
		cmd.WaitDelay = time.Second
	}
	if err := cmd.Start(); err != nil {
		return procResult{ExitCode: -1}
	}
	err := cmd.Wait()
	result := procResult{Started: true}
	if runCtx.Err() == context.DeadlineExceeded && ctx.Err() == nil {
		result.TimedOut = true
	}
	state := cmd.ProcessState
	if state == nil {
		result.Crashed = true
		result.ExitCode = -1
		return result
	}
	result.ExitCode = state.ExitCode()
	if result.ExitCode < 0 {
		result.Crashed = true
		result.ExitCode = signalNumber(state)
	}
	var exitErr *exec.ExitError
	if err != nil && !errors.As(err, &exitErr) && !errors.Is(err, io.ErrClosedPipe) && result.ExitCode == 0 && !result.Crashed {
		result.Crashed = true
	}
	return result
}
