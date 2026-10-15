package git

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"log"
	"os"
	"os/exec"
	"strings"
	"sync"
	"time"
)

const (
	DefaultMaxOutput = 32 << 20
	maxStderr        = 256 << 10
	maxTimeoutDetail = 4000
)

var scrubbedVariables = []string{
	"GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_COMMON_DIR", "GIT_OBJECT_DIRECTORY",
	"GIT_NAMESPACE", "GIT_PREFIX", "GIT_ALTERNATE_OBJECT_DIRECTORIES",
	"GIT_TERMINAL_PROMPT", "GIT_OPTIONAL_LOCKS", "GIT_PAGER", "LC_ALL", "LANGUAGE",
}

var fixedVariables = []string{
	"GIT_TERMINAL_PROMPT=0",
	"GIT_OPTIONAL_LOCKS=0",
	"GIT_PAGER=cat",
	"LC_ALL=C",
}

func Environ(base []string, extra ...string) []string {
	drop := map[string]bool{}
	for _, name := range scrubbedVariables {
		drop[name] = true
	}
	overrides := map[string]bool{}
	for _, entry := range extra {
		if eq := strings.IndexByte(entry, '='); eq > 0 {
			overrides[entry[:eq]] = true
		}
	}
	out := make([]string, 0, len(base)+len(fixedVariables)+len(extra))
	for _, entry := range base {
		name := entry
		if eq := strings.IndexByte(entry, '='); eq >= 0 {
			name = entry[:eq]
		}
		if drop[name] || overrides[name] {
			continue
		}
		out = append(out, entry)
	}
	for _, entry := range fixedVariables {
		name := entry[:strings.IndexByte(entry, '=')]
		if !overrides[name] {
			out = append(out, entry)
		}
	}
	return append(out, extra...)
}

type Options struct {
	Timeout   time.Duration
	Input     []byte
	Env       []string
	MaxOutput int
	NoLane    bool
}

type Result struct {
	Output    []byte
	Stderr    string
	Error     string
	ExitCode  int
	OK        bool
	TimedOut  bool
	Truncated bool
	Bypassed  bool
	Attempts  int
}

type Runner struct {
	Binary    string
	BaseEnv   []string
	Logger    *log.Logger
	QueueWait time.Duration
	MaxOutput int

	lanesMu sync.Mutex
	lanes   map[string]*lane
}

var Default = &Runner{}

const SharedRunnerKey = "git.runner"

func Run(ctx context.Context, dir string, args []string, options Options) Result {
	return Default.Run(ctx, dir, args, options)
}

func (r *Runner) logf(format string, args ...any) {
	if r.Logger != nil {
		r.Logger.Printf(format, args...)
	}
}

func (r *Runner) binary() string {
	if r.Binary != "" {
		return r.Binary
	}
	return "git"
}

func (r *Runner) Run(ctx context.Context, dir string, args []string, options Options) Result {
	if ctx == nil {
		ctx = context.Background()
	}
	var lease *Lease
	if !options.NoLane && WritesIndex(args) {
		held, l, err := r.hold(ctx, dir, Subcommand(args))
		if err != nil {
			return Result{Error: err.Error(), ExitCode: -1, TimedOut: true}
		}
		ctx = held
		lease = l
		defer lease.Release()
	}
	policy := RetryPolicy{Dir: dir}
	attempts := 0
	for {
		attempts++
		result := r.attempt(ctx, dir, args, options)
		result.Attempts = attempts
		result.Bypassed = lease.Bypassed()
		if result.OK || result.TimedOut {
			return result
		}
		retry := policy.Next(result.Error)
		if !retry.Retry {
			return result
		}
		if retry.RemovedLock != "" {
			r.logf("git: removed orphaned %s (no git process held it) and retrying %s", retry.RemovedLock, Subcommand(args))
		}
		if retry.Wait > 0 {
			timer := time.NewTimer(retry.Wait)
			select {
			case <-ctx.Done():
				timer.Stop()
				result.TimedOut = true
				result.Error = ctx.Err().Error()
				return result
			case <-timer.C:
			}
		}
	}
}

type capped struct {
	buf       bytes.Buffer
	limit     int
	truncated bool
}

func (c *capped) Write(p []byte) (int, error) {
	room := c.limit - c.buf.Len()
	if room <= 0 {
		if len(p) > 0 {
			c.truncated = true
		}
		return len(p), nil
	}
	if len(p) > room {
		c.buf.Write(p[:room])
		c.truncated = true
		return len(p), nil
	}
	c.buf.Write(p)
	return len(p), nil
}

func CommandLine(dir string, args []string) string {
	shown := make([]string, 0, len(args))
	for i := 0; i < len(args); i++ {
		arg := args[i]
		if arg == "-c" && i+1 < len(args) {
			shown = append(shown, arg, redactConfig(args[i+1]))
			i++
			continue
		}
		shown = append(shown, arg)
	}
	line := strings.Join(strings.Fields("git "+strings.Join(shown, " ")), " ")
	if dir != "" {
		line += " (" + dir + ")"
	}
	return line
}

func redactConfig(value string) string {
	eq := strings.IndexByte(value, '=')
	if eq < 0 {
		return value
	}
	key := strings.ToLower(value[:eq])
	if strings.Contains(key, "header") || strings.Contains(key, "auth") || strings.Contains(key, "token") ||
		strings.Contains(key, "password") || strings.Contains(key, "secret") {
		return value[:eq] + "=<redacted>"
	}
	return value
}

func (r *Runner) attempt(ctx context.Context, dir string, args []string, options Options) Result {
	timeout := TimeoutFor(args, options.Timeout)
	limit := options.MaxOutput
	if limit <= 0 {
		limit = r.MaxOutput
	}
	if limit <= 0 {
		limit = DefaultMaxOutput
	}
	runCtx, cancel := context.WithTimeout(ctx, timeout)
	defer cancel()
	full := append([]string{"-C", dir}, args...)
	cmd := exec.CommandContext(runCtx, r.binary(), full...)
	prepare(cmd)
	cmd.WaitDelay = StopGrace
	base := r.BaseEnv
	if base == nil {
		base = os.Environ()
	}
	cmd.Env = Environ(base, options.Env...)
	if options.Input != nil {
		cmd.Stdin = bytes.NewReader(options.Input)
	}
	stdout := &capped{limit: limit}
	stderr := &capped{limit: maxStderr}
	cmd.Stdout = stdout
	cmd.Stderr = stderr
	err := cmd.Run()
	result := Result{Output: stdout.buf.Bytes(), Stderr: strings.TrimSpace(stderr.buf.String()), Truncated: stdout.truncated}
	if cmd.ProcessState != nil {
		result.ExitCode = cmd.ProcessState.ExitCode()
	} else {
		result.ExitCode = -1
	}
	if runCtx.Err() != nil {
		result.TimedOut = true
		if ctx.Err() != nil {
			result.Error = ctx.Err().Error()
			return result
		}
		detail := result.Stderr
		if out := strings.TrimSpace(string(result.Output)); out != "" {
			if detail != "" {
				detail += "\n"
			}
			detail += out
		}
		if len(detail) > maxTimeoutDetail {
			detail = detail[:maxTimeoutDetail] + "\n… (output truncated)"
		}
		result.Error = fmt.Sprintf("git timed out after %ds: %s", int(timeout/time.Second), CommandLine(dir, args))
		if detail != "" {
			result.Error += "\n" + detail
		}
		return result
	}
	if err != nil {
		var exit *exec.ExitError
		if !errors.As(err, &exit) {
			result.Error = err.Error()
			return result
		}
		result.Error = result.Stderr
		return result
	}
	result.OK = true
	return result
}
