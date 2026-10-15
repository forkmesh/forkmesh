package hosts

import (
	"context"
	"errors"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"runtime"
	"strings"
	"sync"
	"syscall"
	"time"
)

type sshCommand struct {
	Program string
	Args    []string
	Env     []string
}

var (
	sshHostPattern = regexp.MustCompile(`^[A-Za-z0-9](?:[A-Za-z0-9.:-]{0,251}[A-Za-z0-9])?$`)
	sshUserPattern = regexp.MustCompile(`^[A-Za-z_][A-Za-z0-9_.-]{0,63}$`)
)

func joinPath(dir, name string) string { return filepath.Join(dir, name) }

func isSafeSSHEndpoint(host, user string) bool {
	clean := strings.TrimSpace(host)
	return len(clean) <= 253 && !strings.Contains(clean, "..") && sshHostPattern.MatchString(clean) &&
		sshUserPattern.MatchString(strings.TrimSpace(user))
}

func ensureSSHDir(sshDir string) string {
	if sshDir == "" {
		return ""
	}
	if err := os.MkdirAll(sshDir, 0o700); err != nil {
		return ""
	}
	_ = os.Chmod(sshDir, 0o700)
	return sshDir
}

func environmentWithout(names ...string) []string {
	out := []string{}
	for _, entry := range os.Environ() {
		drop := false
		for _, name := range names {
			if strings.HasPrefix(entry, name+"=") {
				drop = true
				break
			}
		}
		if !drop {
			out = append(out, entry)
		}
	}
	return out
}

func buildHostSSHCommand(sshDir, host, user, password, remote, identityFile string) (sshCommand, error) {
	if !isSafeSSHEndpoint(host, user) {
		return sshCommand{}, errors.New("The saved SSH host or username is invalid.")
	}
	if strings.TrimSpace(remote) == "" || strings.Contains(remote, "\x00") {
		return sshCommand{}, errors.New("The remote SSH command is invalid.")
	}
	dir := ensureSSHDir(sshDir)
	if dir == "" {
		return sshCommand{}, errors.New("ForkMesh could not create its persistent SSH trust store.")
	}
	knownHosts := filepath.Join(dir, knownHostsName)
	if _, err := os.Stat(knownHosts); errors.Is(err, os.ErrNotExist) {
		file, err := os.OpenFile(knownHosts, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0o600)
		if err != nil && !errors.Is(err, os.ErrExist) {
			return sshCommand{}, errors.New("ForkMesh could not initialize its persistent SSH trust store.")
		}
		if file != nil {
			_ = file.Close()
		}
	}
	_ = os.Chmod(knownHosts, 0o600)
	args := []string{
		"-o", "StrictHostKeyChecking=accept-new",
		"-o", "UserKnownHostsFile=" + knownHosts,
		"-o", "HashKnownHosts=yes",
		"-o", "UpdateHostKeys=yes",
		"-o", "ConnectTimeout=30",
	}
	if identity := strings.TrimSpace(identityFile); identity != "" {
		if strings.Contains(identity, "\x00") || !isFile(identity) {
			return sshCommand{}, errors.New("The managed SSH key for this host is missing.")
		}
		args = append(args, "-i", identity, "-o", "IdentitiesOnly=yes")
	}
	command := sshCommand{Env: environmentWithout("SSHPASS")}
	if password == "" {
		command.Program = "ssh"
		args = append(args, "-o", "BatchMode=yes", "-o", "PreferredAuthentications=publickey")
	} else {
		command.Program = "sshpass"
		command.Env = append(command.Env, "SSHPASS="+password)
		args = append([]string{"-e", "ssh"}, args...)
		args = append(args, "-o", "PreferredAuthentications=publickey,password")
	}
	args = append(args, strings.TrimSpace(user)+"@"+strings.TrimSpace(host), remote)
	command.Args = args
	return command, nil
}

type runResult struct {
	Started  bool
	OK       bool
	TimedOut bool
	ExitCode int
	Output   []byte
}

type cappedBuffer struct {
	mu    sync.Mutex
	data  []byte
	limit int
}

func (b *cappedBuffer) Write(p []byte) (int, error) {
	b.mu.Lock()
	defer b.mu.Unlock()
	b.data = append(b.data, p...)
	if b.limit > 0 && len(b.data) > b.limit {
		b.data = append([]byte(nil), b.data[len(b.data)-b.limit:]...)
	}
	return len(p), nil
}

func (b *cappedBuffer) Bytes() []byte {
	b.mu.Lock()
	defer b.mu.Unlock()
	return append([]byte(nil), b.data...)
}

func runSSH(ctx context.Context, command sshCommand, timeout time.Duration, maxOutput int, graceful bool) runResult {
	ctx, cancel := context.WithTimeout(ctx, timeout)
	defer cancel()
	cmd := exec.CommandContext(ctx, command.Program, command.Args...)
	cmd.Env = command.Env
	output := &cappedBuffer{limit: maxOutput}
	cmd.Stdout = output
	cmd.Stderr = output
	cmd.Stdin = nil
	if graceful && runtime.GOOS != "windows" {
		cmd.Cancel = func() error { return cmd.Process.Signal(syscall.SIGTERM) }
		cmd.WaitDelay = 3 * time.Second
	} else {
		cmd.WaitDelay = time.Second
	}
	if err := cmd.Start(); err != nil {
		return runResult{Started: false, ExitCode: -1}
	}
	err := cmd.Wait()
	result := runResult{Started: true, Output: output.Bytes(), ExitCode: cmd.ProcessState.ExitCode()}
	if ctx.Err() == context.DeadlineExceeded {
		result.TimedOut = true
		return result
	}
	result.OK = err == nil && result.ExitCode == 0
	return result
}

var (
	ansiPattern            = regexp.MustCompile("\x1B(?:\\[[0-?]*[ -/]*[@-~]|\\][^\x07]*(?:\x07|\x1B\\\\))")
	bearerPattern          = regexp.MustCompile(`(?i)\bBearer\s+[A-Za-z0-9._~+/-]{8,}`)
	namedCredentialPattern = regexp.MustCompile(`(?i)\b(CLOUDFLARE_API_TOKEN|CF_API_TOKEN|CLOUDFLARE_TOKEN|CF_TOKEN|authorization|api[_ -]?token|password|secret)(\s*(?::|=)\s*|\s+Bearer\s+)[^\s,;]+`)
)

func redactProcessOutput(text string) string {
	safe := strings.ReplaceAll(text, "\x00", "")
	safe = ansiPattern.ReplaceAllString(safe, "")
	safe = bearerPattern.ReplaceAllString(safe, "Bearer <redacted>")
	safe = namedCredentialPattern.ReplaceAllString(safe, "${1}=<redacted>")
	return safe
}

func reportOutput(output []byte) string {
	tail := output
	if len(tail) > 2048 {
		tail = tail[len(tail)-2048:]
	}
	return strings.TrimSpace(redactProcessOutput(strings.ToValidUTF8(string(tail), "�")))
}

func containsFold(haystack, needle string) bool {
	return strings.Contains(strings.ToLower(haystack), strings.ToLower(needle))
}
