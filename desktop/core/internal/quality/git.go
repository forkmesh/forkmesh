package quality

import (
	"bytes"
	"context"
	"os/exec"
	"time"
)

const (
	scanGitTimeout  = 8 * time.Second
	batchGitTimeout = 60 * time.Second
	issueGitTimeout = 10 * time.Second
)

type gitRunner struct {
	binary string
}

func (g gitRunner) run(ctx context.Context, dir string, timeout time.Duration, stdin []byte, args ...string) ([]byte, bool) {
	ctx, cancel := context.WithTimeout(ctx, timeout)
	defer cancel()
	binary := g.binary
	if binary == "" {
		binary = "git"
	}
	cmd := exec.CommandContext(ctx, binary, append([]string{"-C", dir}, args...)...)
	if stdin != nil {
		cmd.Stdin = bytes.NewReader(stdin)
	}
	var stdout bytes.Buffer
	cmd.Stdout = &stdout
	if err := cmd.Run(); err != nil {
		return nil, false
	}
	return stdout.Bytes(), true
}

func (g gitRunner) text(ctx context.Context, dir string, args ...string) string {
	if qtTrim(dir) == "" {
		return ""
	}
	out, ok := g.run(ctx, dir, scanGitTimeout, nil, args...)
	if !ok {
		return ""
	}
	return qtTrim(string(bytes.ToValidUTF8(out, []byte("�"))))
}
