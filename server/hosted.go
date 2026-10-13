package mirrornode

// ForkMesh-hosted repositories on the core nodes (docs/HOSTED_REPOS.md).
//
// A repository created on the website with no desktop node and no external
// provider lives on every core node. The app (rqlite, replicated by Raft)
// holds the desired set; each core's daemon reconciles its own copy against
// it, so there is no leader and no hand-edited gateway configuration:
//
//   - POST /api/hosted-repositories/replicas, node-signed like a runner claim,
//     reports this node's copies ({id: pushedAt, digest, size}) and answers the
//     desired set. For each repository the app says whether this node is its
//     primary (rendezvous-first live core, the one that receives pushes), and,
//     when this copy is behind the last recorded push or still empty, the peer
//     gateways to fetch from with a router-signed read grant each
//     (/v1/runner/<owner>/<repo>.git, the runner route).
//   - A new repository becomes a bare repository under hosted.root plus a
//     gateway entry: "public", or "restricted" for a private one (served only
//     to router capabilities, never on public routes). The primary of an empty
//     repository initializes it: a deterministic first commit (README,
//     .gitignore, LICENSE as the app spelled them), or a bounded fetch of a
//     public repository on an allowed forge (importUrl).
//   - Replicas converge through the same fast-forward-only reconciliation the
//     upstream sync uses (reconcileUpstreamRefs), so a stale source can never
//     rewind a branch.
//   - A repository the website deleted is dropped from the gateway and its
//     directory moved to <root>/.trash (kept; an operator purges it).
//
// The payload-free "hosted" node event (sent after a create or a push) wakes
// this at once; otherwise it runs every hostedSafetyInterval.

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io/fs"
	"log"
	"net/http"
	"net/url"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"
)

const (
	hostedReplicasPath    = "/api/hosted-repositories/replicas"
	hostedSafetyInterval  = 2 * time.Minute
	hostedMaxRepositories = 5000
	hostedMaxInitFiles    = 8
	hostedMaxInitBytes    = 256 << 10
	// A fetch (import or replica) is stopped once the repository grows past
	// this many bytes; the app refuses pushes past its own quota.
	hostedDefaultMaxBytes = 256 << 20
	hostedFetchTimeout    = 10 * time.Minute
)

var (
	hostedIDPattern     = regexp.MustCompile(`^hr[0-9a-f]{24}$`)
	hostedBranchPattern = regexp.MustCompile(`^[A-Za-z0-9][A-Za-z0-9._/-]{0,99}$`)
	hostedFilePattern   = regexp.MustCompile(`^(README\.md|\.gitignore|LICENSE)$`)
	hostedGrantPattern  = regexp.MustCompile(`^[A-Za-z0-9_-]{16,1000}\.[A-Za-z0-9_-]{86}$`)
	hostedDefaultImport = []string{"github.com", "gitlab.com", "codeberg.org"}
)

// HostedConfig turns the replicator on (core role only).
type HostedConfig struct {
	Enabled bool `json:"enabled,omitempty"`
	// Absolute, owner-only directory holding <id>.git and .trash/.
	Root string `json:"root,omitempty"`
	// Hosts an importUrl may name (https only). Default github.com,
	// gitlab.com, codeberg.org.
	ImportHosts []string `json:"importHosts,omitempty"`
	PushHosts   []string `json:"pushHosts,omitempty"`
	// Largest repository a fetch may produce (bytes); default 256 MiB.
	MaxRepositoryBytes int64 `json:"maxRepositoryBytes,omitempty"`
}

func (c *Config) validateHosted() error {
	h := c.Hosted
	if h == nil || !h.Enabled {
		return nil
	}
	if c.Role != RoleCore && c.Role != RoleMirror {
		return errors.New("hosted repositories need role core or mirror")
	}
	if !filepath.IsAbs(h.Root) || filepath.Clean(h.Root) != h.Root || h.Root == "/" {
		return errors.New("hosted.root must be a clean absolute directory")
	}
	if c.CatalogURL == "" {
		return errors.New("hosted repositories need catalogUrl")
	}
	for _, host := range h.ImportHosts {
		if host == "" || strings.ContainsAny(host, "/:@ ") || strings.ToLower(host) != host {
			return errors.New("hosted.importHosts must be lower-case host names")
		}
	}
	if err := validHostedPushHosts(h.PushHosts); err != nil {
		return err
	}
	if h.MaxRepositoryBytes < 0 {
		return errors.New("hosted.maxRepositoryBytes must not be negative")
	}
	return nil
}

type hostedReplicator struct {
	d          *Daemon
	cfg        HostedConfig
	client     *http.Client
	mu         sync.Mutex
	pending    bool
	lastCheck  time.Time
	fetchedAt  map[string]int64 // id -> the pushedAt this copy has fetched
	lastReport map[string]hostedCopyState
	pushState  map[string]hostedPushState
	runPush    hostedPushRunner
	clock      func() time.Time
}

type hostedCopyState struct {
	PushedAt  int64  `json:"pushedAt"`
	Empty     bool   `json:"empty"`
	Digest    string `json:"routingDigest,omitempty"`
	SizeBytes int64  `json:"sizeBytes"`
}

type hostedReplicaRequest struct {
	Repositories map[string]hostedCopyState  `json:"repositories"`
	PushMirrors  map[string]hostedPushReport `json:"pushMirrors,omitempty"`
}

type hostedInitFile struct {
	Path    string `json:"path"`
	Content string `json:"content"`
}

type hostedInit struct {
	DefaultBranch string           `json:"defaultBranch"`
	Files         []hostedInitFile `json:"files"`
	Message       string           `json:"message"`
	AuthorName    string           `json:"authorName"`
	AuthorEmail   string           `json:"authorEmail"`
	Time          int64            `json:"time"`
	ImportURL     string           `json:"importUrl"`
}

type hostedSource struct {
	Node  string `json:"node"`
	URL   string `json:"url"`
	Grant string `json:"grant"`
}

type hostedDesired struct {
	ID         string         `json:"id"`
	Owner      string         `json:"owner"`
	Name       string         `json:"name"`
	Visibility string         `json:"visibility"`
	PushedAt   int64          `json:"pushedAt"`
	Primary    bool           `json:"primary"`
	Init       *hostedInit    `json:"init,omitempty"`
	Sources    []hostedSource `json:"sources,omitempty"`
	// Organization names the repository is also served under (org_repos).
	Aliases    []string          `json:"aliases,omitempty"`
	PushMirror *hostedPushMirror `json:"pushMirror,omitempty"`
}

type hostedRemoved struct {
	ID    string `json:"id"`
	Owner string `json:"owner"`
	Name  string `json:"name"`
}

type hostedReplicaResponse struct {
	OK           bool            `json:"ok"`
	Repositories []hostedDesired `json:"repositories"`
	Removed      []hostedRemoved `json:"removed"`
}

func newHostedReplicator(d *Daemon) (*hostedReplicator, error) {
	h := d.config.Hosted
	if h == nil || !h.Enabled {
		return nil, nil
	}
	cfg := *h
	if len(cfg.ImportHosts) == 0 {
		cfg.ImportHosts = append([]string(nil), hostedDefaultImport...)
	}
	if len(cfg.PushHosts) == 0 {
		cfg.PushHosts = append([]string(nil), hostedDefaultPushHosts...)
	}
	if cfg.MaxRepositoryBytes == 0 {
		cfg.MaxRepositoryBytes = hostedDefaultMaxBytes
	}
	if err := os.MkdirAll(cfg.Root, 0o700); err != nil {
		return nil, fmt.Errorf("hosted root: %w", err)
	}
	return &hostedReplicator{
		d: d, cfg: cfg, client: &http.Client{Timeout: 30 * time.Second},
		pending: true, fetchedAt: map[string]int64{},
		lastReport: map[string]hostedCopyState{}, pushState: map[string]hostedPushState{},
	}, nil
}

// request marks the desired set as changed (the "hosted" node event).
func (r *hostedReplicator) request() {
	r.mu.Lock()
	r.pending = true
	r.mu.Unlock()
}

func (r *hostedReplicator) due(now time.Time) bool {
	r.mu.Lock()
	defer r.mu.Unlock()
	if !r.pending && !r.lastCheck.IsZero() && now.Sub(r.lastCheck) < hostedSafetyInterval {
		return false
	}
	r.pending = false
	r.lastCheck = now
	return true
}

// isHosted reports whether a gateway entry is one of this replicator's copies.
func (r *hostedReplicator) isHosted(repo Repository) bool {
	if r == nil {
		return false
	}
	return hostedIDFromGitDir(r.cfg.Root, repo.GitDir) != ""
}

func hostedIDFromGitDir(root, gitDir string) string {
	if filepath.Dir(filepath.Clean(gitDir)) != root {
		return ""
	}
	id := strings.TrimSuffix(filepath.Base(gitDir), ".git")
	if !hostedIDPattern.MatchString(id) || filepath.Base(gitDir) != id+".git" {
		return ""
	}
	return id
}

func (r *hostedReplicator) gitDir(id string) string {
	return filepath.Join(r.cfg.Root, id+".git")
}

// reconcile runs one or two report/apply rounds: the second one reports what
// the first changed, so the app can pin a freshly initialized repository and
// wake the other cores without waiting for their safety interval.
func (r *hostedReplicator) reconcile(ctx context.Context) error {
	var failures []string
	var response hostedReplicaResponse
	for round := 0; round < 2; round++ {
		var err error
		response, err = r.exchange(ctx)
		if err != nil {
			return err
		}
		changed, errs := r.apply(ctx, response)
		failures = append(failures, errs...)
		if !changed {
			break
		}
	}
	r.pushMirrors(ctx, response)
	if len(failures) > 0 {
		return errors.New(strings.Join(failures, "; "))
	}
	return nil
}

func (r *hostedReplicator) localStates(ctx context.Context) map[string]hostedCopyState {
	states := map[string]hostedCopyState{}
	for _, repo := range r.d.gateway.Repositories {
		id := hostedIDFromGitDir(r.cfg.Root, repo.GitDir)
		if id == "" {
			continue
		}
		states[id] = r.copyState(ctx, id)
	}
	return states
}

func (r *hostedReplicator) copyState(ctx context.Context, id string) hostedCopyState {
	gitDir := r.gitDir(id)
	state := hostedCopyState{PushedAt: r.fetchedAt[id]}
	state.Empty = len(gitLines(ctx, gitDir, "for-each-ref", "--count=1",
		"--format=%(refname)", "refs/heads", "refs/tags")) == 0
	if !state.Empty {
		state.Digest, _ = routingRefsSHA256(ctx, gitDir)
	}
	state.SizeBytes = hostedDirectoryBytes(gitDir)
	return state
}

func (r *hostedReplicator) exchange(ctx context.Context) (hostedReplicaResponse, error) {
	var response hostedReplicaResponse
	base, err := url.Parse(r.d.config.CatalogURL)
	if err != nil || base.Host == "" ||
		(base.Scheme != "https" && !(base.Scheme == "http" && isLoopbackHost(base.Hostname()))) {
		return response, errors.New("catalogUrl is not an absolute HTTPS URL")
	}
	origin := base.Scheme + "://" + base.Host
	body, err := json.Marshal(hostedReplicaRequest{Repositories: r.localStates(ctx),
		PushMirrors: r.pushMirrorReports()})
	if err != nil {
		return response, err
	}
	status, reply, err := signedNodePost(ctx, r.client, origin, r.d.gateway.Node.Name,
		r.d.identity, "ForkMesh-Mirror-Node/"+r.d.config.Version, time.Now(),
		hostedReplicasPath, body)
	if err != nil {
		return response, fmt.Errorf("hosted replicas: %w", err)
	}
	if status < 200 || status >= 300 {
		return response, fmt.Errorf("hosted replicas: HTTP %d: %s", status, boundedText(reply, 200))
	}
	if err := json.Unmarshal(reply, &response); err != nil || !response.OK {
		return response, errors.New("hosted replicas: invalid response")
	}
	if len(response.Repositories) > hostedMaxRepositories {
		return response, errors.New("hosted replicas: too many repositories")
	}
	return response, nil
}

func validHostedDesired(item hostedDesired) bool {
	return hostedIDPattern.MatchString(item.ID) && nodePattern.MatchString(item.Owner) &&
		repositoryPattern.MatchString(item.Name) && !strings.HasPrefix(item.Name, ".") &&
		(item.Visibility == "public" || item.Visibility == "private") && item.PushedAt >= 0
}

// apply makes local state match one response. It reports whether anything a
// second report should mention changed. Hosted entries are rebuilt per id
// (the canonical owner plus any organization aliases, all on one gitDir);
// every other gateway entry is left exactly as it was.
func (r *hostedReplicator) apply(ctx context.Context, response hostedReplicaResponse) (bool, []string) {
	var failures []string
	changed := false
	var configured []Repository
	hosted := map[string][]Repository{}
	for _, repo := range r.d.gateway.Repositories {
		if id := hostedIDFromGitDir(r.cfg.Root, repo.GitDir); id != "" {
			hosted[id] = append(hosted[id], repo)
		} else {
			configured = append(configured, repo)
		}
	}
	taken := map[string]bool{}
	for _, repo := range configured {
		taken[repositoryKey(repo)] = true
	}
	for _, removed := range response.Removed {
		if !hostedIDPattern.MatchString(removed.ID) {
			continue
		}
		delete(hosted, removed.ID)
		if err := r.trash(removed.ID); err != nil {
			failures = append(failures, "hosted "+removed.ID+": "+err.Error())
		}
		delete(r.fetchedAt, removed.ID)
	}
	for _, item := range response.Repositories {
		if !validHostedDesired(item) {
			failures = append(failures, "hosted: invalid repository entry")
			continue
		}
		key := strings.ToLower(item.Owner + "/" + item.Name)
		owners := []string{item.Owner}
		for _, alias := range item.Aliases {
			if nodePattern.MatchString(alias) && alias != item.Owner && len(owners) < 5 {
				owners = append(owners, alias)
			}
		}
		collision := false
		for _, owner := range owners {
			if taken[strings.ToLower(owner+"/"+item.Name)] {
				collision = true
			}
		}
		if collision {
			failures = append(failures, "hosted "+key+": name is served by a configured repository")
			continue
		}
		gitDir := r.gitDir(item.ID)
		created, err := r.ensureBare(ctx, item, gitDir)
		if err != nil {
			failures = append(failures, "hosted "+key+": "+err.Error())
			continue
		}
		changed = changed || created
		empty := len(gitLines(ctx, gitDir, "for-each-ref", "--count=1",
			"--format=%(refname)", "refs/heads", "refs/tags")) == 0
		if empty && item.Primary && item.Init != nil {
			if err := r.initialize(ctx, gitDir, *item.Init); err != nil {
				failures = append(failures, "hosted "+key+" init: "+err.Error())
			} else {
				changed = true
			}
		}
		if len(item.Sources) > 0 {
			if err := r.fetchFromPeers(ctx, gitDir, item.Sources); err != nil {
				failures = append(failures, "hosted "+key+" replicate: "+err.Error())
			} else {
				r.fetchedAt[item.ID] = item.PushedAt
				changed = true
			}
		} else if !empty || item.Primary {
			// Nothing to fetch: this copy already reflects every recorded push.
			r.fetchedAt[item.ID] = item.PushedAt
		}
		ensureHeadResolves(ctx, gitDir)
		digest, err := refsSHA256(ctx, gitDir)
		if err != nil {
			failures = append(failures, "hosted "+key+": "+err.Error())
			continue
		}
		visibility := "public"
		if item.Visibility == "private" {
			visibility = "restricted"
		}
		var entries []Repository
		for _, owner := range owners {
			entry := Repository{
				Owner: owner, Name: item.Name, Visibility: visibility, Enabled: true,
				GitDir: gitDir, Operations: hostedGatewayOperations(visibility == "public",
					r.d.gateway.MergeExecutorCommand != nil, r.d.agentBranchOps),
			}
			entry.Integrity.ExpectedRefsSHA256 = digest
			entries = append(entries, entry)
			taken[strings.ToLower(owner+"/"+item.Name)] = true
		}
		hosted[item.ID] = entries
	}
	ids := make([]string, 0, len(hosted))
	for id := range hosted {
		ids = append(ids, id)
	}
	sort.Strings(ids)
	next := append([]Repository(nil), configured...)
	for _, id := range ids {
		next = append(next, hosted[id]...)
	}
	before, _ := json.Marshal(r.d.gateway.Repositories)
	after, _ := json.Marshal(next)
	if string(before) != string(after) {
		r.d.gateway.Repositories = next
		if _, err := writeGatewayConfig(r.d.config.GatewayConfig, r.d.gateway); err != nil {
			failures = append(failures, "hosted: write gateway config: "+err.Error())
		} else {
			r.d.markEndpointStateDirty()
		}
	}
	return changed, failures
}

func hostedGatewayOperations(public, writes, agentBranches bool) []string {
	operations := append(append([]string(nil), defaultGatewayOperations...), "git-receive-pack")
	if public && writes {
		operations = append(operations, writeGatewayOperations...)
	}
	sort.Strings(operations)
	if public && writes && agentBranches {
		operations = append(operations, agentBranchGatewayOperations...)
	}
	return operations
}

func (r *hostedReplicator) ensureBare(ctx context.Context, item hostedDesired, gitDir string) (bool, error) {
	if info, err := os.Lstat(gitDir); err == nil {
		if !info.IsDir() || info.Mode()&os.ModeSymlink != 0 || !validBareRepository(gitDir) {
			return false, errors.New("hosted path is not a bare repository")
		}
		return false, nil
	} else if !errors.Is(err, os.ErrNotExist) {
		return false, err
	}
	branch := "main"
	if item.Init != nil && hostedBranchPattern.MatchString(item.Init.DefaultBranch) {
		branch = item.Init.DefaultBranch
	}
	temporary, err := os.MkdirTemp(r.cfg.Root, ".new-*")
	if err != nil {
		return false, err
	}
	defer removeTreeUnder(r.cfg.Root, temporary)
	target := filepath.Join(temporary, "repo.git")
	if output, err := exec.CommandContext(ctx, "git", "init", "--quiet", "--bare",
		"--initial-branch="+branch, target).CombinedOutput(); err != nil {
		return false, fmt.Errorf("git init: %s", boundedText(output, 160))
	}
	if err := os.Chmod(target, 0o700); err != nil {
		return false, err
	}
	if err := os.Rename(target, gitDir); err != nil {
		return false, err
	}
	return true, nil
}

// initialize writes the first commit exactly as the app described it. The
// same input gives the same commit id on every core, so two nodes that both
// believed they were primary still agree.
func (r *hostedReplicator) initialize(ctx context.Context, gitDir string, init hostedInit) error {
	if init.ImportURL != "" {
		return r.importRepository(ctx, gitDir, init.ImportURL)
	}
	if len(init.Files) == 0 {
		return nil
	}
	if len(init.Files) > hostedMaxInitFiles || !hostedBranchPattern.MatchString(init.DefaultBranch) ||
		init.Time <= 0 || strings.ContainsAny(init.AuthorName+init.AuthorEmail, "<>\n") ||
		init.AuthorName == "" || init.AuthorEmail == "" || strings.TrimSpace(init.Message) == "" {
		return errors.New("invalid init description")
	}
	total := 0
	seen := map[string]bool{}
	var tree strings.Builder
	files := append([]hostedInitFile(nil), init.Files...)
	sort.Slice(files, func(i, j int) bool { return files[i].Path < files[j].Path })
	for _, file := range files {
		total += len(file.Content)
		if !hostedFilePattern.MatchString(file.Path) || seen[file.Path] || total > hostedMaxInitBytes {
			return errors.New("invalid init file")
		}
		seen[file.Path] = true
		command := exec.CommandContext(ctx, "git", "--git-dir="+gitDir, "hash-object", "-w", "--stdin")
		command.Stdin = strings.NewReader(file.Content)
		output, err := command.Output()
		if err != nil {
			return fmt.Errorf("hash %s: %w", file.Path, err)
		}
		fmt.Fprintf(&tree, "100644 blob %s\t%s\n", strings.TrimSpace(string(output)), file.Path)
	}
	mktree := exec.CommandContext(ctx, "git", "--git-dir="+gitDir, "mktree")
	mktree.Stdin = strings.NewReader(tree.String())
	treeID, err := mktree.Output()
	if err != nil {
		return fmt.Errorf("mktree: %w", err)
	}
	date := strconv.FormatInt(init.Time, 10) + " +0000"
	commitTree := exec.CommandContext(ctx, "git", "--git-dir="+gitDir, "commit-tree",
		strings.TrimSpace(string(treeID)))
	commitTree.Stdin = strings.NewReader(strings.TrimSpace(init.Message) + "\n")
	commitTree.Env = append(os.Environ(),
		"GIT_AUTHOR_NAME="+init.AuthorName, "GIT_AUTHOR_EMAIL="+init.AuthorEmail,
		"GIT_AUTHOR_DATE="+date, "GIT_COMMITTER_NAME="+init.AuthorName,
		"GIT_COMMITTER_EMAIL="+init.AuthorEmail, "GIT_COMMITTER_DATE="+date)
	commit, err := commitTree.Output()
	if err != nil {
		return fmt.Errorf("commit-tree: %w", err)
	}
	ref := "refs/heads/" + init.DefaultBranch
	if output, err := exec.CommandContext(ctx, "git", "--git-dir="+gitDir, "update-ref", ref,
		strings.TrimSpace(string(commit)), strings.Repeat("0", 40)).CombinedOutput(); err != nil {
		return fmt.Errorf("update-ref: %s", boundedText(output, 160))
	}
	return exec.CommandContext(ctx, "git", "--git-dir="+gitDir, "symbolic-ref", "HEAD", ref).Run()
}

func (r *hostedReplicator) importAllowed(raw string) bool {
	parsed, err := url.Parse(raw)
	if err != nil || parsed.Scheme != "https" || parsed.User != nil || parsed.RawQuery != "" ||
		parsed.Fragment != "" || parsed.Port() != "" || parsed.Path == "" || parsed.Path == "/" {
		return false
	}
	host := strings.ToLower(parsed.Hostname())
	for _, allowed := range r.cfg.ImportHosts {
		if host == allowed {
			return true
		}
	}
	return false
}

func (r *hostedReplicator) importRepository(ctx context.Context, gitDir, source string) error {
	if !r.importAllowed(source) {
		return errors.New("import URL is not on an allowed forge")
	}
	args := []string{"--git-dir=" + gitDir, "fetch", "--quiet", "--no-tags", "--", source,
		"+refs/heads/*:refs/heads/*", "+refs/tags/*:refs/tags/*"}
	env := append(gitNetworkEnvironment(), "GIT_ALLOW_PROTOCOL=https")
	if err := r.boundedGit(ctx, gitDir, args, env); err != nil {
		return err
	}
	// Point HEAD at the forge's default branch when it has one of the usual
	// names; ensureHeadResolves covers the rest.
	for _, candidate := range []string{"main", "master", "trunk", "develop"} {
		if gitRefValue(ctx, gitDir, "refs/heads/"+candidate) != "" {
			_ = exec.CommandContext(ctx, "git", "--git-dir="+gitDir, "symbolic-ref", "HEAD",
				"refs/heads/"+candidate).Run()
			break
		}
	}
	return nil
}

// fetchFromPeers pulls every head and tag from the first peer that answers,
// then fast-forwards local refs only (reconcileUpstreamRefs).
func (r *hostedReplicator) fetchFromPeers(ctx context.Context, gitDir string, sources []hostedSource) error {
	var failures []string
	for _, source := range sources {
		if !nodePattern.MatchString(source.Node) || !hostedGrantPattern.MatchString(source.Grant) ||
			!hostedSourceURLAllowed(source.URL) {
			failures = append(failures, "invalid source")
			continue
		}
		args := []string{"--git-dir=" + gitDir, "fetch", "--quiet", "--prune", "--no-tags",
			"--", source.URL,
			"+refs/heads/*:refs/forkmesh/upstream/heads/*",
			"+refs/tags/*:refs/forkmesh/upstream/tags/*"}
		// The grant travels in the environment, never on a command line.
		env := append(gitNetworkEnvironment(),
			"GIT_ALLOW_PROTOCOL=https:http",
			"GIT_CONFIG_COUNT=2",
			"GIT_CONFIG_KEY_0=http.extraHeader",
			"GIT_CONFIG_VALUE_0=X-ForkMesh-Runner-Grant: "+source.Grant,
			"GIT_CONFIG_KEY_1=http.followRedirects",
			"GIT_CONFIG_VALUE_1=false")
		if err := r.boundedGit(ctx, gitDir, args, env); err != nil {
			failures = append(failures, source.Node+": "+err.Error())
			clearRefNamespace(context.Background(), gitDir, "refs/forkmesh/upstream")
			continue
		}
		if err := reconcileUpstreamRefs(ctx, gitDir); err != nil {
			failures = append(failures, source.Node+": "+err.Error())
			continue
		}
		return nil
	}
	return errors.New(strings.Join(failures, "; "))
}

func hostedSourceURLAllowed(raw string) bool {
	parsed, err := url.Parse(raw)
	if err != nil || parsed.User != nil || parsed.RawQuery != "" || parsed.Fragment != "" {
		return false
	}
	if parsed.Scheme != "https" && !(parsed.Scheme == "http" && isLoopbackHost(parsed.Hostname())) {
		return false
	}
	return strings.HasPrefix(parsed.Path, "/v1/runner/") && strings.HasSuffix(parsed.Path, ".git")
}

// boundedGit runs one git network command and stops it when the repository
// grows past the size limit or the fetch timeout passes.
func (r *hostedReplicator) boundedGit(ctx context.Context, gitDir string, args, env []string) error {
	ctx, cancel := context.WithTimeout(ctx, hostedFetchTimeout)
	defer cancel()
	command := exec.CommandContext(ctx, "git", args...)
	command.Env = env
	var output strings.Builder
	command.Stdout = &output
	command.Stderr = &output
	if err := command.Start(); err != nil {
		return err
	}
	done := make(chan error, 1)
	go func() { done <- command.Wait() }()
	ticker := time.NewTicker(500 * time.Millisecond)
	defer ticker.Stop()
	for {
		select {
		case err := <-done:
			if err != nil {
				return fmt.Errorf("git: %s", boundedText([]byte(output.String()), 200))
			}
			if hostedDirectoryBytes(gitDir) > r.cfg.MaxRepositoryBytes {
				return errors.New("repository is larger than the hosted limit")
			}
			return nil
		case <-ticker.C:
			if hostedDirectoryBytes(gitDir) > r.cfg.MaxRepositoryBytes {
				cancel()
				<-done
				return errors.New("repository is larger than the hosted limit")
			}
		}
	}
}

// trash moves a deleted repository out of the served root. Nothing is
// erased here; an operator empties <root>/.trash.
func (r *hostedReplicator) trash(id string) error {
	gitDir := r.gitDir(id)
	if _, err := os.Lstat(gitDir); errors.Is(err, os.ErrNotExist) {
		return nil
	}
	trash := filepath.Join(r.cfg.Root, ".trash")
	if err := os.MkdirAll(trash, 0o700); err != nil {
		return err
	}
	destination := filepath.Join(trash, id+"-"+strconv.FormatInt(time.Now().Unix(), 10)+".git")
	return os.Rename(gitDir, destination)
}

// routingRefsSHA256 matches tools/mirror_gateway.py routing_refs_sha256: the
// default branch plus tags, the digest the app pins for routing.
func routingRefsSHA256(ctx context.Context, gitDir string) (string, error) {
	head := gitValue(ctx, gitDir, "symbolic-ref", "--quiet", "HEAD")
	if !strings.HasPrefix(head, "refs/heads/") {
		head = "refs/heads/main"
	}
	output, err := exec.CommandContext(ctx, "git", "--git-dir="+gitDir, "for-each-ref",
		"--sort=refname", "--format=%(objectname) %(refname)", head, "refs/tags/").Output()
	if err != nil {
		return "", err
	}
	var lines []string
	for _, line := range strings.Split(string(output), "\n") {
		if line != "" {
			lines = append(lines, line)
		}
	}
	digest := sha256.Sum256([]byte(strings.Join(lines, "\n")))
	return hex.EncodeToString(digest[:]), nil
}

func hostedDirectoryBytes(root string) int64 {
	var total int64
	_ = filepath.WalkDir(root, func(_ string, entry fs.DirEntry, err error) error {
		if err != nil {
			return nil
		}
		if entry.Type().IsRegular() {
			if info, infoErr := entry.Info(); infoErr == nil {
				total += info.Size()
			}
		}
		return nil
	})
	return total
}

func (d *Daemon) reconcileHosted(ctx context.Context) error {
	if d.hosted == nil {
		return nil
	}
	if err := d.hosted.reconcile(ctx); err != nil {
		log.Printf("hosted repositories: %v", err)
		return err
	}
	return nil
}
