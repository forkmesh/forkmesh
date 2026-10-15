package diag

import (
	"context"
	"encoding/json"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"sync"
	"time"
)

const (
	buildMacStaleLockSeconds = 4 * 60 * 60
	buildMacProbeTimeout     = 12 * time.Second
	buildMacCacheMs          = 20000
	buildMacDefaultHost      = ""
)

var buildMacJobJunk = regexp.MustCompile(`[^A-Za-z0-9 ._/+-]`)

func buildMacProbeScript() string {
	return `if [ -d ci/lock ]; then echo "$(( $(date +%s) - $(stat -f %m ci/lock) ))"; ` +
		`head -c 200 ci/lock/run 2>/dev/null; fi; true`
}

func buildMacStatus(reachable bool, output []byte, checkedAtMs int64) map[string]any {
	out := map[string]any{"configured": true, "checkedAtMs": checkedAtMs}
	if !reachable {
		out["state"] = "offline"
		return out
	}
	lines := strings.Split(fromUTF8(output), "\n")
	age, err := strconv.ParseInt(strings.TrimSpace(lines[0]), 10, 64)
	job := ""
	if len(lines) > 1 {
		job = buildMacJobJunk.ReplaceAllString(strings.TrimSpace(lines[1]), "")
	}
	if err != nil || age < 0 || age >= buildMacStaleLockSeconds || job == "" {
		out["state"] = "idle"
		return out
	}
	out["state"] = "building"
	out["job"] = left(job, 80)
	out["sinceMs"] = checkedAtMs - age*1000
	return out
}

type probeFunc func(ctx context.Context, args []string) ([]byte, bool)

func sshProbe(ctx context.Context, args []string) ([]byte, bool) {
	ctx, cancel := context.WithTimeout(ctx, buildMacProbeTimeout)
	defer cancel()
	cmd := exec.CommandContext(ctx, "ssh", args...)
	cmd.Stdin = nil
	output, err := cmd.Output()
	if err != nil || ctx.Err() != nil {
		return nil, false
	}
	return output, true
}

type buildMac struct {
	mu     sync.Mutex
	last   map[string]any
	lastAt int64
	flight chan struct{}
	probe  probeFunc
	now    func() int64
}

func fileExists(path string) bool {
	_, err := os.Stat(path)
	return err == nil
}

func macBuildHost(variables string) string {
	values := map[string]any{}
	if json.Unmarshal([]byte(variables), &values) == nil {
		if host, ok := values["MAC_BUILD_HOST"].(string); ok {
			if host = strings.TrimSpace(host); host != "" {
				return host
			}
		}
	}
	return buildMacDefaultHost
}

func (b *buildMac) status(ctx context.Context, sshDir string, variables func() string) (map[string]any, error) {
	dir := filepath.Join(sshDir, "mac-build")
	key := filepath.Join(dir, "id_ed25519")
	knownHosts := filepath.Join(dir, "known_hosts")
	if !fileExists(key) || !fileExists(knownHosts) {
		return map[string]any{"configured": false}, nil
	}
	if macBuildHost(variables()) == "" {
		return map[string]any{"configured": false}, nil
	}
	b.mu.Lock()
	if b.last != nil && b.now()-b.lastAt < buildMacCacheMs {
		last := b.last
		b.mu.Unlock()
		return last, nil
	}
	flight := b.flight
	if flight == nil {
		flight = make(chan struct{})
		b.flight = flight
		host := macBuildHost(variables())
		args := []string{"-i", key, "-o", "IdentitiesOnly=yes", "-o", "BatchMode=yes", "-o", "ConnectTimeout=5",
			"-o", "StrictHostKeyChecking=yes", "-o", "UserKnownHostsFile=" + knownHosts, "-o", "HostKeyAlias=forkmesh-mac",
			"build@" + host, buildMacProbeScript()}
		go b.run(args, flight)
	}
	b.mu.Unlock()
	select {
	case <-flight:
	case <-ctx.Done():
		return nil, ctx.Err()
	}
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.last, nil
}

func (b *buildMac) run(args []string, flight chan struct{}) {
	output, reachable := b.probe(context.Background(), args)
	if !reachable {
		output = nil
	}
	now := b.now()
	status := buildMacStatus(reachable, output, now)
	b.mu.Lock()
	b.last = status
	b.lastAt = now
	b.flight = nil
	b.mu.Unlock()
	close(flight)
}
