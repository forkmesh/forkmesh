//go:build unix && !linux

package git

import (
	"bytes"
	"context"
	"os/exec"
	"time"
)

func lockHeld(lockPath string) bool {
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	out, _ := exec.CommandContext(ctx, "lsof", "-t", "--", lockPath).Output()
	if ctx.Err() != nil {
		return true
	}
	return len(bytes.TrimSpace(out)) > 0
}
