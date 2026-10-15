package repos

import (
	"context"
	"encoding/base64"
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"time"
	"unicode/utf8"

	"forkmesh.com/desktop-core/internal/git"
	"forkmesh.com/desktop-core/internal/rpc"
)

const (
	MaxRunArgs      = 4096
	MaxRunTimeoutMs = 60 * 60 * 1000
	MaxRunInput     = 64 << 20
	MaxRunEnv       = 1024
	MaxRunEnvBytes  = 1 << 20
)

type runParams struct {
	Path        string   `json:"path"`
	Args        []string `json:"args"`
	Write       *bool    `json:"write"`
	TimeoutMs   int64    `json:"timeoutMs"`
	Input       *string  `json:"input"`
	InputBase64 *string  `json:"inputBase64"`
	Env         []string `json:"env"`
}

type runReply struct {
	Path         string `json:"path"`
	Code         int    `json:"code"`
	OK           bool   `json:"ok"`
	TimedOut     bool   `json:"timedOut"`
	Stdout       string `json:"stdout"`
	StdoutBase64 string `json:"stdoutBase64,omitempty"`
	Stderr       string `json:"stderr"`
	Error        string `json:"error"`
	Truncated    bool   `json:"truncated"`
	Attempts     int    `json:"attempts"`
	Write        bool   `json:"write"`
	Bypassed     bool   `json:"bypassed"`
	QueuedMs     int64  `json:"queuedMs"`
}

func runDirectory(path string) (string, error) {
	path = strings.TrimSpace(path)
	if path == "" || !filepath.IsAbs(path) {
		return "", rpc.Fail("bad_request")
	}
	path = filepath.Clean(path)
	info, err := os.Stat(path)
	if err != nil || !info.IsDir() {
		return "", rpc.Fail("no_directory")
	}
	return path, nil
}

func runInput(params runParams) ([]byte, error) {
	switch {
	case params.InputBase64 != nil:
		decoded, err := base64.StdEncoding.DecodeString(*params.InputBase64)
		if err != nil || len(decoded) > MaxRunInput {
			return nil, rpc.Fail("bad_request")
		}
		return decoded, nil
	case params.Input != nil:
		if len(*params.Input) > MaxRunInput {
			return nil, rpc.Fail("bad_request")
		}
		return []byte(*params.Input), nil
	}
	return nil, nil
}

func validEnvName(name string) bool {
	if name == "" {
		return false
	}
	for i, r := range name {
		if r == '_' || (r >= 'A' && r <= 'Z') || (r >= 'a' && r <= 'z') || (i > 0 && r >= '0' && r <= '9') {
			continue
		}
		return false
	}
	return true
}

func validRunEnv(env []string) bool {
	if len(env) > MaxRunEnv {
		return false
	}
	total := 0
	for _, entry := range env {
		total += len(entry)
		eq := strings.IndexByte(entry, '=')
		if eq <= 0 || !validEnvName(entry[:eq]) || strings.IndexByte(entry, 0) >= 0 || total > MaxRunEnvBytes {
			return false
		}
	}
	return true
}

func validRunArgs(args []string) bool {
	if len(args) == 0 || len(args) > MaxRunArgs {
		return false
	}
	for _, arg := range args {
		if strings.IndexByte(arg, 0) >= 0 {
			return false
		}
	}
	return git.Subcommand(args) != ""
}

func (m *Module) gitRun(ctx context.Context, req *rpc.Request) (any, error) {
	if err := local(req); err != nil {
		return nil, err
	}
	var params runParams
	if len(req.Params) > 0 {
		if err := json.Unmarshal(req.Params, &params); err != nil {
			return nil, rpc.Fail("bad_request")
		}
	}
	path, err := runDirectory(params.Path)
	if err != nil {
		return nil, err
	}
	if !validRunArgs(params.Args) {
		return nil, rpc.Fail("bad_request")
	}
	if params.TimeoutMs < 0 || params.TimeoutMs > MaxRunTimeoutMs || !validRunEnv(params.Env) {
		return nil, rpc.Fail("bad_request")
	}
	input, err := runInput(params)
	if err != nil {
		return nil, err
	}
	write := git.WritesIndex(params.Args)
	if params.Write != nil {
		write = *params.Write || write
	}
	options := git.Options{Timeout: time.Duration(params.TimeoutMs) * time.Millisecond, Input: input, Env: params.Env}
	reply := runReply{Path: path, Write: write}
	if write {
		started := time.Now()
		held, lease, err := m.Git.Hold(ctx, path)
		reply.QueuedMs = time.Since(started).Milliseconds()
		if err != nil {
			reply.Code = -1
			reply.TimedOut = true
			reply.Error = err.Error()
			return reply, nil
		}
		defer lease.Release()
		ctx = held
		reply.Bypassed = lease.Bypassed()
	} else {
		options.NoLane = true
	}
	result := m.Git.Run(ctx, path, params.Args, options)
	reply.Code = result.ExitCode
	reply.OK = result.OK
	reply.TimedOut = result.TimedOut
	reply.Stderr = result.Stderr
	reply.Error = result.Error
	reply.Truncated = result.Truncated
	reply.Attempts = result.Attempts
	reply.Bypassed = reply.Bypassed || result.Bypassed
	if utf8.Valid(result.Output) {
		reply.Stdout = string(result.Output)
	} else {
		reply.StdoutBase64 = base64.StdEncoding.EncodeToString(result.Output)
	}
	return reply, nil
}
