package mirrornode

import (
	"errors"
	"log"
	"net/url"
	"regexp"
	"sort"
	"strings"
	"time"
)

var watchRepositoryPattern = regexp.MustCompile(`^[A-Za-z0-9._-]{1,100}$`)

var readOnlyMirrorBlockedOperations = map[string]bool{
	"git-receive-pack":   true,
	"merge-pull":         true,
	"update-pull-branch": true,
}

func upstreamWatchTarget(upstream string) (string, string, bool) {
	parsed, err := url.Parse(strings.TrimSpace(upstream))
	if err != nil || parsed.Host == "" || parsed.User != nil ||
		parsed.RawQuery != "" || parsed.Fragment != "" {
		return "", "", false
	}
	loopback := parsed.Scheme == "http" && isLoopbackHost(parsed.Hostname())
	if parsed.Scheme != "https" && !loopback {
		return "", "", false
	}
	parts := strings.Split(strings.Trim(parsed.Path, "/"), "/")
	if len(parts) != 2 {
		return "", "", false
	}
	owner := strings.ToLower(parts[0])
	name := strings.ToLower(strings.TrimSuffix(parts[1], ".git"))
	if !nodePattern.MatchString(owner) || !watchRepositoryPattern.MatchString(name) ||
		name == "." || name == ".." {
		return "", "", false
	}
	scheme := "wss"
	if loopback {
		scheme = "ws"
	}
	endpoint := (&url.URL{Scheme: scheme, Host: parsed.Host, Path: "/api/nodes/events"}).String()
	return endpoint, owner + "/" + name, true
}

func NewUpstreamWatch(upstream string) (*EventSocket, error) {
	endpoint, repository, ok := upstreamWatchTarget(upstream)
	if !ok {
		return nil, errors.New("upstream " + upstream + " cannot be watched")
	}
	return &EventSocket{
		endpoint:     endpoint,
		watch:        repository,
		pingInterval: 5 * time.Minute,
		idleTimeout:  9 * time.Minute,
		minBackoff:   time.Second,
		maxBackoff:   5 * time.Minute,
	}, nil
}

func relayApex(host string) string {
	host = strings.ToLower(strings.TrimSpace(host))
	if parsed, err := url.Parse("//" + host); err == nil && parsed.Hostname() != "" {
		host = parsed.Hostname()
	}
	for _, prefix := range []string{"app.", "api.", "www."} {
		if trimmed := strings.TrimPrefix(host, prefix); trimmed != host && strings.Contains(trimmed, ".") {
			return trimmed
		}
	}
	return host
}

func sameRelay(catalogURL, upstream string) bool {
	catalog, err := url.Parse(catalogURL)
	if err != nil || catalog.Host == "" {
		return false
	}
	source, err := url.Parse(upstream)
	if err != nil || source.Host == "" {
		return false
	}
	a, b := relayApex(catalog.Host), relayApex(source.Host)
	return a == b || strings.HasSuffix(a, "."+b) || strings.HasSuffix(b, "."+a)
}

func (d *Daemon) setUpstreamWatches() error {
	if d.config.Role == RoleRunner {
		return nil
	}
	seen := map[string]bool{}
	for _, upstream := range d.config.EventDrivenUpstreams {
		if seen[upstream] || (d.config.CatalogURL != "" && sameRelay(d.config.CatalogURL, upstream)) {
			continue
		}
		seen[upstream] = true
		watch, err := NewUpstreamWatch(upstream)
		if err != nil {
			return err
		}
		label := upstream
		watch.Handler = d.handleUpstreamEvent
		watch.Connected = func(connected bool) { d.setWatchConnected(label, connected) }
		d.watches = append(d.watches, watch)
	}
	return nil
}

func (d *Daemon) setWatchConnected(upstream string, connected bool) {
	d.mu.Lock()
	if d.watchUp == nil {
		d.watchUp = map[string]bool{}
	}
	prior, known := d.watchUp[upstream]
	d.watchUp[upstream] = connected
	d.mu.Unlock()
	if !known || prior != connected {
		state := "disconnected"
		if connected {
			state = "connected"
		}
		log.Printf("upstream watch %s: %s", upstream, state)
	}
}

func (d *Daemon) handleUpstreamEvent(event Event) {
	if event.CatchUp || event.Topic == "commits" {
		d.requestSync()
	}
}

func (d *Daemon) noteUpstreamSync(gitDir string, at time.Time) {
	d.mu.Lock()
	defer d.mu.Unlock()
	if d.upstreamSynced == nil {
		d.upstreamSynced = map[string]time.Time{}
	}
	d.upstreamSynced[gitDir] = at
}

func (d *Daemon) upstreamSyncedAt(gitDir string) time.Time {
	d.mu.RLock()
	defer d.mu.RUnlock()
	return d.upstreamSynced[gitDir]
}

func (d *Daemon) mirrorOfFor(repo Repository) string {
	page := d.configuredMirrorOf(repo)
	if page == "" || (d.config.CatalogURL != "" && sameRelay(d.config.CatalogURL, page)) {
		return ""
	}
	return page
}

func (d *Daemon) configuredMirrorOf(repo Repository) string {
	if page := d.config.MirrorOf[repositoryKey(repo)]; page != "" {
		return page
	}
	keys := make([]string, 0, len(d.gateway.Repositories))
	pages := map[string]string{}
	for _, candidate := range d.gateway.Repositories {
		if candidate.GitDir != repo.GitDir {
			continue
		}
		key := repositoryKey(candidate)
		if page := d.config.MirrorOf[key]; page != "" {
			keys = append(keys, key)
			pages[key] = page
		}
	}
	if len(keys) == 0 {
		return ""
	}
	sort.Strings(keys)
	return pages[keys[0]]
}

func (d *Daemon) readOnlyMirrorDirs() map[string]bool {
	dirs := map[string]bool{}
	for i := range d.gateway.Repositories {
		repo := d.gateway.Repositories[i]
		if d.mirrorOfFor(repo) != "" {
			dirs[repo.GitDir] = true
		}
	}
	return dirs
}

func ensureReadOnlyMirrorOperations(gateway *GatewayConfig, dirs map[string]bool) {
	if len(dirs) == 0 {
		return
	}
	blocked := map[string]bool{}
	for op := range readOnlyMirrorBlockedOperations {
		blocked[op] = true
	}
	for _, op := range agentBranchGatewayOperations {
		blocked[op] = true
	}
	for i := range gateway.Repositories {
		repo := &gateway.Repositories[i]
		if !dirs[repo.GitDir] {
			continue
		}
		ops := make([]string, 0, len(repo.Operations))
		for _, op := range repo.Operations {
			if !blocked[op] {
				ops = append(ops, op)
			}
		}
		repo.Operations = ops
	}
}
