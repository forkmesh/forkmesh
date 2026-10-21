package mirrornode

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io/fs"
	"log"
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

type GatewayConfig struct {
	SchemaVersion int `json:"schemaVersion"`
	Node          struct {
		Name      string `json:"name"`
		PublicKey string `json:"publicKey"`
	} `json:"node"`
	RouterPublicKey        string          `json:"routerPublicKey"`
	PublicOrigin           string          `json:"publicOrigin"`
	Listen                 json.RawMessage `json:"listen"`
	ManifestPath           string          `json:"manifestPath"`
	RequestVerifierCommand []string        `json:"requestVerifierCommand"`
	HealthSignerCommand    []string        `json:"healthSignerCommand"`
	// Loopback-only daemon intake used after the public gateway verifies one
	// signed collaboration event. It contains no credential and is never
	// exposed by the direct HTTPS reverse proxy.
	CollaborationControlURL string `json:"collaborationControlUrl,omitempty"`
	// Fixed operator-owned executor the gateway calls for the owner-authorized
	// merge-pull / update-pull-branch operations; see merge.go.
	MergeExecutorCommand []string `json:"mergeExecutorCommand,omitempty"`
	// Where the Actions runner publishes its redacted summary and where the
	// gateway couriers owner-authorized control requests in; see actions.go.
	// Both are set only while the runner has a working sandbox.
	ActionsSummaryPath  string          `json:"actionsSummaryPath,omitempty"`
	ActionsControlPath  string          `json:"actionsControlPath,omitempty"`
	Repositories        []Repository    `json:"repositories"`
	Limits              json.RawMessage `json:"limits"`
	PrivateReplicaStore string          `json:"privateReplicaStore,omitempty"`
	// Operator-authored optional features preserved verbatim across the
	// daemon's load/patch/rewrite cycle. The Python gateway owns their schema;
	// the daemon only patches signer/verifier commands and repository refs, so
	// these pass through untouched (DisallowUnknownFields would otherwise
	// reject them). Dashboard enables the same-codebase mirror website;
	// DynamicRepositoriesPath points at the node-owned fork registry;
	// WritableRepositoryRoot plus WritableOwners let an authorized first push
	// create its bare repository here, which is what carries a repository
	// created from the website with no node of its own (issue #676).
	Dashboard               json.RawMessage `json:"dashboard,omitempty"`
	DynamicRepositoriesPath string          `json:"dynamicRepositoriesPath,omitempty"`
	WritableRepositoryRoot  string          `json:"writableRepositoryRoot,omitempty"`
	WritableOwners          []string        `json:"writableOwners,omitempty"`
}

type Repository struct {
	Owner        string   `json:"owner"`
	Name         string   `json:"name"`
	ForkedFrom   string   `json:"forkedFrom,omitempty"`
	Visibility   string   `json:"visibility"`
	Enabled      bool     `json:"enabled"`
	GitDir       string   `json:"gitDir"`
	ReleaseStore string   `json:"releaseStore,omitempty"`
	Operations   []string `json:"operations"`
	Integrity    struct {
		ExpectedRefsSHA256 string `json:"expectedRefsSha256"`
	} `json:"integrity"`
}

func loadGatewayConfig(path string) (GatewayConfig, []byte, error) {
	var cfg GatewayConfig
	info, err := os.Lstat(path)
	if err != nil || !info.Mode().IsRegular() || info.Mode()&os.ModeSymlink != 0 || info.Size() > 4<<20 {
		return cfg, nil, errors.New("gateway config must be a small regular file")
	}
	raw, err := os.ReadFile(path)
	if err != nil {
		return cfg, nil, err
	}
	decoder := json.NewDecoder(bytes.NewReader(raw))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&cfg); err != nil {
		return cfg, nil, err
	}
	// A runner's document names its identity and no repositories; every
	// serving role still needs one, which NewDaemon checks.
	if cfg.SchemaVersion != 1 || cfg.Node.Name == "" || cfg.Node.PublicKey == "" {
		return cfg, nil, errors.New("gateway config is incomplete")
	}
	for i := range cfg.Repositories {
		if cfg.Repositories[i].ForkedFrom != "" &&
			!forkParentPattern.MatchString(cfg.Repositories[i].ForkedFrom) {
			return cfg, nil, errors.New("gateway repository forkedFrom is invalid")
		}
		if cfg.Repositories[i].ReleaseStore != "" {
			continue
		}
		legacyStore := filepath.Join(
			cfg.Repositories[i].GitDir, "forkmesh-releases")
		if info, statErr := os.Stat(legacyStore); statErr == nil && info.IsDir() {
			cfg.Repositories[i].ReleaseStore = legacyStore
		}
	}
	return cfg, raw, nil
}

// LoadGatewayConfigForCommand exposes the public, secret-free gateway identity
// needed by the two bounded helper modes.
func LoadGatewayConfigForCommand(path string) (GatewayConfig, []byte, error) {
	return loadGatewayConfig(path)
}

func refsSHA256(ctx context.Context, gitDir string) (string, error) {
	cmd := exec.CommandContext(ctx, "git", "--git-dir="+gitDir, "for-each-ref", "--format=%(objectname) %(refname)", "refs/heads", "refs/tags")
	out, err := cmd.Output()
	if err != nil {
		return "", fmt.Errorf("read stable refs: %w", err)
	}
	var lines []string
	for _, line := range strings.Split(string(out), "\n") {
		line = strings.TrimSpace(line)
		parts := strings.SplitN(line, " ", 2)
		if len(parts) != 2 || strings.HasSuffix(line, "^{}") ||
			(!strings.HasPrefix(parts[1], "refs/heads/") && !strings.HasPrefix(parts[1], "refs/tags/")) {
			continue
		}
		lines = append(lines, line)
	}
	sort.Slice(lines, func(i, j int) bool {
		return strings.SplitN(lines[i], " ", 2)[1] < strings.SplitN(lines[j], " ", 2)[1]
	})
	digest := sha256.Sum256([]byte(strings.Join(lines, "\n")))
	return hex.EncodeToString(digest[:]), nil
}

func syncRepository(ctx context.Context, repo Repository, upstreams []string, avoidNode string, desired ...string) error {
	if len(upstreams) == 0 {
		return errors.New("no upstream configured")
	}
	var failures []string
	for _, upstream := range upstreams {
		if strings.TrimSpace(upstream) == "" {
			continue
		}
		if _, err := os.Stat(repo.GitDir); errors.Is(err, os.ErrNotExist) {
			if err := cloneMirrorRepository(ctx, upstream, repo.GitDir, avoidNode); err == nil {
				if err := makePublicRepositoryTraversable(repo.GitDir); err != nil {
					failures = append(failures, fmt.Sprintf(
						"%s: publish repository permissions: %s", upstream, err))
					continue
				}
				return nil
			} else {
				failures = append(failures, fmt.Sprintf("%s: %s", upstream, err))
				continue
			}
		} else if err != nil {
			failures = append(failures, fmt.Sprintf("%s: inspect mirror: %s", upstream, err))
			continue
		} else if !isBareRepository(ctx, repo.GitDir) {
			// The release CAS may be populated before the first repository
			// clone. In that state GitDir exists with only forkmesh-releases/
			// beneath it; treating existence as a completed clone wedges the
			// node forever. Safely turn that owned container into a bare repo
			// in place, preserving every content-addressed release blob.
			if err := initializeReleaseStoreContainer(ctx, repo.GitDir); err != nil {
				failures = append(failures, fmt.Sprintf("%s: initialize mirror: %s", upstream, err))
				continue
			}
		}
		if err := makePublicRepositoryTraversable(repo.GitDir); err != nil {
			failures = append(failures, fmt.Sprintf(
				"%s: publish repository permissions: %s", upstream, err))
			continue
		}
		// A public upstream is round-robin: the selected peer can briefly be
		// behind this node. Fetch into an isolated namespace first so one stale
		// response cannot rewind main, delete a new tag, or make the whole cycle
		// fail with a non-fast-forward rejection.
		dropBrokenRefs(ctx, repo.GitDir)
		if output, err := fetchUpstreamAvoidingBrokenPeers(ctx, repo.GitDir, upstream, avoidNode); err == nil {
			if err := reconcileUpstreamRefs(ctx, repo.GitDir, desired...); err != nil {
				failures = append(failures, fmt.Sprintf("%s: %s", upstream, err))
				continue
			}
			ensureHeadResolves(ctx, repo.GitDir)
			convergePeerMains(ctx, repo.GitDir, laterUpstreams(upstreams, upstream), avoidNode)
			return nil
		} else {
			failures = append(failures, fmt.Sprintf("%s: %s", upstream, boundedText(output, 240)))
		}
	}
	return fmt.Errorf("all upstreams failed: %s", strings.Join(failures, "; "))
}

func isBareRepository(ctx context.Context, path string) bool {
	cmd := exec.CommandContext(ctx, "git", "--git-dir="+path,
		"rev-parse", "--is-bare-repository")
	out, err := cmd.Output()
	return err == nil && strings.TrimSpace(string(out)) == "true"
}

func initializeReleaseStoreContainer(ctx context.Context, path string) error {
	info, err := os.Lstat(path)
	if err != nil {
		return err
	}
	if !info.IsDir() || info.Mode()&os.ModeSymlink != 0 {
		return errors.New("mirror path is not an owned directory")
	}
	entries, err := os.ReadDir(path)
	if err != nil {
		return err
	}
	for _, entry := range entries {
		if entry.Name() != "forkmesh-releases" || !entry.IsDir() || entry.Type()&os.ModeSymlink != 0 {
			return fmt.Errorf("non-repository mirror path contains unexpected entry %q", entry.Name())
		}
	}
	// --initial-branch is explicit on purpose: a stock VPS leaves
	// init.defaultBranch unset, so the seeded mirror would be born on master
	// while every upstream carries main. ensureHeadResolves still repairs
	// mirrors seeded by older builds.
	cmd := exec.CommandContext(ctx, "git", "init", "--bare",
		"--initial-branch=main", "--", path)
	if output, err := cmd.CombinedOutput(); err != nil {
		return fmt.Errorf("git init: %s", boundedText(output, 160))
	}
	return nil
}

const (
	maxPeerFetchRetries = 2
	maxAvoidedNodes     = 4
	staleRefLockAge     = 10 * time.Minute
)

var redirectedPeerPattern = regexp.MustCompile(`redirecting to https?://([a-z](?:[a-z0-9-]{0,61}[a-z0-9])?)\.`)

func gitRoutingArgs(avoid ...string) []string {
	var nodes []string
	seen := map[string]bool{}
	for _, node := range avoid {
		node = strings.TrimSpace(strings.ToLower(node))
		if !nodePattern.MatchString(node) || seen[node] {
			continue
		}
		seen[node] = true
		nodes = append(nodes, node)
		if len(nodes) == maxAvoidedNodes {
			break
		}
	}
	if len(nodes) == 0 {
		return nil
	}
	return []string{"-c", "http.extraHeader=X-ForkMesh-Avoid-Node: " + strings.Join(nodes, ",")}
}

func redirectedPeer(output []byte) string {
	matches := redirectedPeerPattern.FindAllSubmatch(output, -1)
	if len(matches) == 0 {
		return ""
	}
	return strings.ToLower(string(matches[len(matches)-1][1]))
}

func fetchUpstreamAvoidingBrokenPeers(ctx context.Context, gitDir, upstream, avoidNode string) ([]byte, error) {
	avoid := []string{avoidNode}
	for attempt := 0; ; attempt++ {
		args := gitRoutingArgs(avoid...)
		args = append(args, "--git-dir="+gitDir, "fetch", "--prune", "--", upstream,
			"+refs/heads/*:refs/forkmesh/upstream/heads/*",
			"+refs/tags/*:refs/forkmesh/upstream/tags/*")
		cmd := exec.CommandContext(ctx, "git", args...)
		cmd.Env = gitNetworkEnvironment()
		output, err := cmd.CombinedOutput()
		if err == nil {
			return output, nil
		}
		clearRefNamespace(context.Background(), gitDir, "refs/forkmesh/upstream")
		dropBrokenRefs(ctx, gitDir)
		peer := redirectedPeer(output)
		if attempt >= maxPeerFetchRetries || peer == "" || !nodePattern.MatchString(peer) {
			return output, err
		}
		for _, avoided := range avoid {
			if strings.EqualFold(avoided, peer) {
				return output, err
			}
		}
		avoid = append(avoid, peer)
	}
}

func dropBrokenRefs(ctx context.Context, gitDir string) []string {
	type looseRef struct {
		path string
		ref  string
		oid  string
	}
	var candidates []looseRef
	var dropped []string
	now := time.Now()
	_ = filepath.WalkDir(filepath.Join(gitDir, "refs"), func(path string, entry fs.DirEntry, err error) error {
		if err != nil || entry.IsDir() || !entry.Type().IsRegular() {
			return nil
		}
		rel, relErr := filepath.Rel(gitDir, path)
		if relErr != nil {
			return nil
		}
		ref := filepath.ToSlash(rel)
		if strings.HasSuffix(ref, ".lock") {
			info, infoErr := entry.Info()
			if infoErr == nil && now.Sub(info.ModTime()) > staleRefLockAge && os.Remove(path) == nil {
				dropped = append(dropped, ref)
			}
			return nil
		}
		raw, readErr := os.ReadFile(path)
		if readErr != nil {
			return nil
		}
		value := strings.TrimSpace(string(raw))
		if strings.HasPrefix(value, "ref: ") {
			return nil
		}
		if !gitObjectIDPattern.MatchString(value) || strings.Trim(value, "0") == "" {
			if os.Remove(path) == nil {
				dropped = append(dropped, ref)
			}
			return nil
		}
		candidates = append(candidates, looseRef{path: path, ref: ref, oid: value})
		return nil
	})
	if len(candidates) > 0 {
		var input strings.Builder
		for _, candidate := range candidates {
			input.WriteString(candidate.oid)
			input.WriteByte('\n')
		}
		cmd := exec.CommandContext(ctx, "git", "--git-dir="+gitDir,
			"cat-file", "--batch-check=%(objectname)")
		cmd.Stdin = strings.NewReader(input.String())
		if out, err := cmd.Output(); err == nil {
			lines := strings.Split(strings.TrimRight(string(out), "\n"), "\n")
			if len(lines) == len(candidates) {
				for i, line := range lines {
					if strings.HasSuffix(strings.TrimSpace(line), " missing") && os.Remove(candidates[i].path) == nil {
						dropped = append(dropped, candidates[i].ref)
					}
				}
			}
		}
	}
	if len(dropped) > 0 {
		log.Printf("mirror %s: dropped %d broken refs: %s", gitDir, len(dropped), strings.Join(dropped, ", "))
	}
	return dropped
}

func cloneMirrorRepository(ctx context.Context, upstream, destination, avoidNode string) error {
	parent := filepath.Dir(destination)
	if err := os.MkdirAll(parent, 0700); err != nil {
		return err
	}
	temporary, err := os.MkdirTemp(parent, ".clone-*")
	if err != nil {
		return err
	}
	os.Remove(temporary)
	defer removeTreeUnder(parent, temporary)
	args := gitRoutingArgs(avoidNode)
	args = append(args, "clone", "--mirror", "--", upstream, temporary)
	command := exec.CommandContext(ctx, "git", args...)
	command.Env = gitNetworkEnvironment()
	if output, err := command.CombinedOutput(); err != nil {
		return fmt.Errorf("clone failed: %s", boundedText(output, 240))
	}
	if err := os.Rename(temporary, destination); err != nil {
		return err
	}
	return nil
}

func makePublicRepositoryTraversable(gitDir string) error {
	info, err := os.Lstat(gitDir)
	if err != nil {
		return err
	}
	if !info.IsDir() || info.Mode()&os.ModeSymlink != 0 {
		return errors.New("public repository is not a directory")
	}
	// cloneMirrorRepository intentionally starts in a private temporary
	// directory so partial clones are never exposed. Once the atomic rename has
	// completed, the SSH gateway needs to traverse the bind-mounted repository.
	// Files and child directories are created readably by the service's 0022
	// umask; preserve all existing mode bits and add only public traversal here.
	return os.Chmod(gitDir, info.Mode().Perm()|0005)
}

func gitNetworkEnvironment() []string {
	// Never let an upstream select Git's ext:: transport, which executes an
	// arbitrary local command. Public mirrors need only ordinary network
	// transports plus file:// paths for operator-managed local mirrors/tests.
	return append(os.Environ(),
		"GIT_TERMINAL_PROMPT=0",
		"GIT_ALLOW_PROTOCOL=https:ssh:file")
}

type refUpdater func(context.Context, string, string, string, string) ([]byte, error)

func reconcileUpstreamRefs(ctx context.Context, gitDir string, desired ...string) error {
	return reconcileUpstreamRefsWithUpdater(ctx, gitDir, updateRef, desired...)
}

func reconcileUpstreamRefsWithUpdater(ctx context.Context, gitDir string, updater refUpdater, desired ...string) error {
	defer clearRefNamespace(context.Background(), gitDir, "refs/forkmesh/upstream")
	cmd := exec.CommandContext(ctx, "git", "--git-dir="+gitDir, "for-each-ref",
		"--format=%(refname) %(objectname)", "refs/forkmesh/upstream/heads",
		"refs/forkmesh/upstream/tags")
	raw, err := cmd.Output()
	if err != nil {
		return fmt.Errorf("list staged refs: %w", err)
	}
	localRefs, err := gitRefValues(ctx, gitDir, "refs/heads", "refs/tags")
	if err != nil {
		return fmt.Errorf("list local refs: %w", err)
	}
	movedTags := map[string]string{}
	upstreamRefs := map[string]string{}
	head := headBranchRef(ctx, gitDir)
	pullArrivals := map[string]string{}
	for _, line := range strings.Split(strings.TrimSpace(string(raw)), "\n") {
		fields := strings.Fields(line)
		if len(fields) != 2 {
			continue
		}
		local := "refs/heads/" + strings.TrimPrefix(fields[0], "refs/forkmesh/upstream/heads/")
		if strings.HasPrefix(fields[0], "refs/forkmesh/upstream/heads/") &&
			pullMetadataBranchNumber(local) != 0 && localRefs[local] == "" {
			pullArrivals[local] = fields[1]
		}
	}
	retiredPulls := retiredPullBranchArrivals(ctx, gitDir, pullArrivals)
	superseded := supersededBranchTips(ctx, gitDir)
	for _, line := range strings.Split(strings.TrimSpace(string(raw)), "\n") {
		fields := strings.Fields(line)
		if len(fields) != 2 {
			continue
		}
		staged, incoming := fields[0], fields[1]
		local := ""
		switch {
		case strings.HasPrefix(staged, "refs/forkmesh/upstream/heads/"):
			local = "refs/heads/" + strings.TrimPrefix(staged, "refs/forkmesh/upstream/heads/")
		case strings.HasPrefix(staged, "refs/forkmesh/upstream/tags/"):
			local = "refs/tags/" + strings.TrimPrefix(staged, "refs/forkmesh/upstream/tags/")
		default:
			continue
		}
		upstreamRefs[local] = incoming
		current := localRefs[local]
		if current == incoming {
			continue
		}
		if strings.HasPrefix(local, "refs/tags/") && current != "" {
			movedTags[local] = incoming
			continue
		}
		if current == "" && strings.HasPrefix(local, "refs/heads/") &&
			agentBranchTombstoned(ctx, gitDir, strings.TrimPrefix(local, "refs/heads/"), incoming) {
			continue
		}
		if current == "" && retiredPulls[local] {
			continue
		}
		if current != "" && superseded[local][incoming] {
			continue
		}
		if err := settleRef(ctx, gitDir, local, current, incoming, local == head, updater); err != nil {
			return err
		}
	}
	return adoptAdmittedTagMoves(ctx, gitDir, movedTags, upstreamRefs, desired)
}

const (
	pushHistoryRoot = "refs/forkmesh/history/"
	pushHistoryKeep = 20
)

var pushHistorySequence = regexp.MustCompile(`^[0-9]{10}$`)

func supersededBranchTips(ctx context.Context, gitDir string) map[string]map[string]bool {
	values, err := gitRefValues(ctx, gitDir, strings.TrimSuffix(pushHistoryRoot, "/"))
	if err != nil {
		return nil
	}
	tips := map[string]map[string]bool{}
	for ref, oid := range values {
		branch, sequence, ok := cutLast(strings.TrimPrefix(ref, pushHistoryRoot), "/")
		if !ok || !pushHistorySequence.MatchString(sequence) {
			continue
		}
		local := "refs/heads/" + branch
		if tips[local] == nil {
			tips[local] = map[string]bool{}
		}
		tips[local][oid] = true
	}
	return tips
}

func cutLast(text, separator string) (string, string, bool) {
	index := strings.LastIndex(text, separator)
	if index <= 0 {
		return "", "", false
	}
	return text[:index], text[index+len(separator):], true
}

func keepSupersededTip(ctx context.Context, gitDir, ref, oid string) error {
	prefix := pushHistoryRoot + strings.TrimPrefix(ref, "refs/heads/") + "/"
	values, err := gitRefValues(ctx, gitDir, strings.TrimSuffix(prefix, "/"))
	if err != nil {
		return err
	}
	var names []string
	for name := range values {
		if pushHistorySequence.MatchString(strings.TrimPrefix(name, prefix)) {
			names = append(names, name)
		}
	}
	sort.Strings(names)
	sequence := 1
	if len(names) > 0 {
		last, _ := strconv.Atoi(strings.TrimPrefix(names[len(names)-1], prefix))
		sequence = last + 1
	}
	name := fmt.Sprintf("%s%010d", prefix, sequence)
	if output, err := updateRef(ctx, gitDir, name, oid, ""); err != nil {
		return fmt.Errorf("keep %s: %s", ref, boundedText(output, 160))
	}
	names = append(names, name)
	for len(names) > pushHistoryKeep {
		_ = exec.CommandContext(ctx, "git", "--git-dir="+gitDir, "update-ref", "-d", names[0]).Run()
		names = names[1:]
	}
	return nil
}

func adoptForcedBranches(ctx context.Context, gitDir string, forced map[string]string) {
	if len(forced) == 0 {
		return
	}
	head := headBranchRef(ctx, gitDir)
	staged, err := gitRefValues(ctx, gitDir, "refs/forkmesh/upstream/heads")
	if err != nil {
		return
	}
	refs := make([]string, 0, len(forced))
	for ref := range forced {
		refs = append(refs, ref)
	}
	sort.Strings(refs)
	for _, ref := range refs {
		tip := forced[ref]
		if !strings.HasPrefix(ref, "refs/heads/") || strings.HasPrefix(ref, "refs/heads/forkmesh/") ||
			ref == head || !gitObjectIDPattern.MatchString(tip) {
			continue
		}
		if staged["refs/forkmesh/upstream/heads/"+strings.TrimPrefix(ref, "refs/heads/")] != tip {
			continue
		}
		current := gitRefValue(ctx, gitDir, ref)
		if current == "" || current == tip || gitIsAncestor(ctx, gitDir, current, tip) {
			continue
		}
		if err := keepSupersededTip(ctx, gitDir, ref, current); err != nil {
			log.Printf("mirror %s: %v", gitDir, err)
			continue
		}
		if output, err := updateRef(ctx, gitDir, ref, tip, current); err != nil {
			log.Printf("mirror %s: force %s: %s", gitDir, ref, boundedText(output, 160))
			continue
		}
		log.Printf("mirror %s: %s force-updated %s -> %s as admitted", gitDir, ref, current[:12], tip[:12])
	}
}

func headBranchRef(ctx context.Context, gitDir string) string {
	head := gitValue(ctx, gitDir, "symbolic-ref", "--quiet", "HEAD")
	if !strings.HasPrefix(head, "refs/heads/") {
		return "refs/heads/main"
	}
	return head
}

func chooseRefValue(ctx context.Context, gitDir, local, current, incoming string, mergeable bool) (string, error) {
	if current == "" || current == incoming {
		return incoming, nil
	}
	if gitIsAncestor(ctx, gitDir, incoming, current) {
		return current, nil
	}
	if gitIsAncestor(ctx, gitDir, current, incoming) {
		return incoming, nil
	}
	merged, issueDivergence, err := reconcileIssueOnlyDivergence(ctx, gitDir, current, incoming)
	if err != nil {
		return "", fmt.Errorf("reconcile %s: %w", local, err)
	}
	if issueDivergence {
		return merged, nil
	}
	if mergeable {
		if merged := mergeDivergentMain(ctx, gitDir, current, incoming); merged != "" {
			return merged, nil
		}
	}
	currentTime := gitCommitTime(ctx, gitDir, current)
	incomingTime := gitCommitTime(ctx, gitDir, incoming)
	if currentTime > incomingTime || (currentTime == incomingTime && current > incoming) {
		return current, nil
	}
	return incoming, nil
}

func settleRef(ctx context.Context, gitDir, local, current, incoming string, mergeable bool, updater refUpdater) error {
	for attempt := 0; ; attempt++ {
		chosen, err := chooseRefValue(ctx, gitDir, local, current, incoming, mergeable)
		if err != nil {
			return err
		}
		if chosen == current {
			return nil
		}
		output, err := updater(ctx, gitDir, local, chosen, current)
		if err == nil {
			return nil
		}
		if attempt == 2 || !isRefUpdateContention(output) {
			return fmt.Errorf("update %s: %s", local, boundedText(output, 160))
		}
		current = gitRefValue(ctx, gitDir, local)
		if current == incoming || (strings.HasPrefix(local, "refs/tags/") && current != "") {
			return nil
		}
	}
}

func moveRef(ctx context.Context, gitDir, local, chosen, current string) error {
	if chosen == current {
		return nil
	}
	if output, err := updateRef(ctx, gitDir, local, chosen, current); err != nil {
		return fmt.Errorf("update %s: %s", local, boundedText(output, 160))
	}
	return nil
}

func updateRef(ctx context.Context, gitDir, ref, newValue, oldValue string) ([]byte, error) {
	if oldValue == "" {
		oldValue = strings.Repeat("0", len(newValue))
	}
	return exec.CommandContext(ctx, "git", "--git-dir="+gitDir, "update-ref", ref, newValue, oldValue).CombinedOutput()
}

func isRefUpdateContention(output []byte) bool {
	message := strings.ToLower(string(output))
	return strings.Contains(message, "cannot lock ref") &&
		(strings.Contains(message, "expected") ||
			strings.Contains(message, "reference already exists"))
}

const (
	maxMainMergeSideCommits = 400
	peerMainInterval        = 30 * time.Second
	peerMainNamespace       = "refs/forkmesh/peer"
	divergentMainMessage    = "Merge divergent core main\n"
)

var peerMainChecks = struct {
	sync.Mutex
	at map[string]time.Time
}{at: map[string]time.Time{}}

func mergeDivergentMain(ctx context.Context, gitDir, left, right string) string {
	base := gitValue(ctx, gitDir, "merge-base", left, right)
	if !gitObjectIDPattern.MatchString(base) {
		return ""
	}
	for _, side := range []string{left, right} {
		count, err := strconv.Atoi(gitValue(ctx, gitDir, "rev-list", "--count", base+".."+side))
		if err != nil || count > maxMainMergeSideCommits {
			return ""
		}
	}
	tree := gitValue(ctx, gitDir, "merge-tree", "--write-tree", "--no-messages", left, right)
	if !gitObjectIDPattern.MatchString(tree) {
		return ""
	}
	merged, err := writeReconcilerMerge(ctx, gitDir, tree, left, right, divergentMainMessage)
	if err != nil {
		return ""
	}
	return merged
}

func laterUpstreams(upstreams []string, used string) []string {
	for i, upstream := range upstreams {
		if upstream == used {
			return upstreams[i+1:]
		}
	}
	return nil
}

func peerMainDue(gitDir string, now time.Time) bool {
	peerMainChecks.Lock()
	defer peerMainChecks.Unlock()
	if last, ok := peerMainChecks.at[gitDir]; ok && now.Sub(last) < peerMainInterval {
		return false
	}
	peerMainChecks.at[gitDir] = now
	return true
}

func convergePeerMains(ctx context.Context, gitDir string, peers []string, avoidNode string) []string {
	if len(peers) == 0 || !peerMainDue(gitDir, time.Now()) {
		return nil
	}
	defer clearRefNamespace(context.Background(), gitDir, peerMainNamespace)
	head := headBranchRef(ctx, gitDir)
	staged := peerMainNamespace + "/" + strings.TrimPrefix(head, "refs/heads/")
	var moved []string
	for _, peer := range peers {
		if strings.TrimSpace(peer) == "" {
			continue
		}
		args := append(gitRoutingArgs(avoidNode), "--git-dir="+gitDir, "fetch",
			"--no-tags", "--", peer, "+"+head+":"+staged)
		cmd := exec.CommandContext(ctx, "git", args...)
		cmd.Env = gitNetworkEnvironment()
		if err := cmd.Run(); err != nil {
			continue
		}
		incoming := gitValue(ctx, gitDir, "rev-parse", "--verify", "-q", staged+"^{commit}")
		current := gitValue(ctx, gitDir, "rev-parse", "--verify", "-q", head+"^{commit}")
		if !gitObjectIDPattern.MatchString(incoming) || current == "" || incoming == current {
			continue
		}
		chosen, err := chooseRefValue(ctx, gitDir, head, current, incoming, true)
		if err != nil || chosen == current {
			continue
		}
		if moveRef(ctx, gitDir, head, chosen, current) == nil {
			log.Printf("mirror %s: %s moved %s -> %s from peer %s", gitDir, head,
				current[:12], chosen[:12], peer)
			moved = append(moved, peer)
		}
	}
	return moved
}

func routingDigest(refs map[string]string) string {
	names := make([]string, 0, len(refs))
	for ref := range refs {
		names = append(names, ref)
	}
	sort.Strings(names)
	lines := make([]string, 0, len(names))
	for _, ref := range names {
		lines = append(lines, refs[ref]+" "+ref)
	}
	sum := sha256.Sum256([]byte(strings.Join(lines, "\n")))
	return hex.EncodeToString(sum[:])
}

func adoptAdmittedTagMoves(ctx context.Context, gitDir string, moved, upstream map[string]string, desired []string) error {
	if len(moved) == 0 || len(desired) == 0 {
		return nil
	}
	head := gitValue(ctx, gitDir, "symbolic-ref", "--quiet", "HEAD")
	if !strings.HasPrefix(head, "refs/heads/") {
		head = "refs/heads/main"
	}
	output, err := exec.CommandContext(ctx, "git", "--git-dir="+gitDir, "for-each-ref",
		"--format=%(objectname) %(refname)", head, "refs/tags/").Output()
	if err != nil {
		return fmt.Errorf("list routing refs: %w", err)
	}
	current := map[string]string{}
	local := map[string]string{}
	for _, line := range strings.Split(string(output), "\n") {
		fields := strings.Fields(line)
		if len(fields) != 2 {
			continue
		}
		current[fields[1]] = fields[0]
		local[fields[1]] = fields[0]
	}
	for ref, oid := range moved {
		local[ref] = oid
	}
	served := map[string]string{}
	for ref, oid := range upstream {
		if ref == head || strings.HasPrefix(ref, "refs/tags/") {
			served[ref] = oid
		}
	}
	candidates := []string{routingDigest(local)}
	if _, ok := served[head]; ok {
		candidates = append(candidates, routingDigest(served))
	}
	candidate := ""
	for _, state := range desired {
		for _, digest := range candidates {
			if strings.EqualFold(strings.TrimSpace(state), digest) {
				candidate = digest
			}
		}
	}
	admitted := candidate != ""
	if !admitted {
		return nil
	}
	names := make([]string, 0, len(moved))
	for ref := range moved {
		names = append(names, ref)
	}
	sort.Strings(names)
	for _, ref := range names {
		output, err := exec.CommandContext(ctx, "git", "--git-dir="+gitDir,
			"update-ref", ref, moved[ref], current[ref]).CombinedOutput()
		if err != nil {
			return fmt.Errorf("move %s: %s", ref, boundedText(output, 160))
		}
		log.Printf("tag %s moved to the admitted state %s", ref, candidate[:12])
	}
	return nil
}

// ensureHeadResolves repairs a mirror whose HEAD names an unborn branch.
// initializeReleaseStoreContainer now seeds main explicitly, but a mirror any
// older build seeded with `git init --bare` has HEAD on the host's default
// initial branch (usually master) while the fetched history only carries the
// upstream's branches. Catalog
// publication resolves HEAD, so an unborn HEAD keeps the node out of the
// catalog forever — installed, serving, and invisible to the relay.
func ensureHeadResolves(ctx context.Context, gitDir string) {
	if gitRefValue(ctx, gitDir, "HEAD^{commit}") != "" {
		return
	}
	target := ""
	for _, ref := range gitLines(ctx, gitDir, "for-each-ref",
		"--format=%(refname)", "refs/heads") {
		if ref == "refs/heads/main" {
			target = ref
			break
		}
		if target == "" {
			target = ref
		}
	}
	if target == "" {
		return
	}
	_ = exec.CommandContext(ctx, "git", "--git-dir="+gitDir,
		"symbolic-ref", "HEAD", target).Run()
}

func gitRefValues(ctx context.Context, gitDir string, namespaces ...string) (map[string]string, error) {
	args := append([]string{"--git-dir=" + gitDir, "for-each-ref",
		"--format=%(refname) %(objectname)"}, namespaces...)
	out, err := exec.CommandContext(ctx, "git", args...).Output()
	if err != nil {
		return nil, err
	}
	values := map[string]string{}
	for _, line := range strings.Split(string(out), "\n") {
		fields := strings.Fields(line)
		if len(fields) == 2 {
			values[fields[0]] = fields[1]
		}
	}
	return values, nil
}

func gitRefValue(ctx context.Context, gitDir, ref string) string {
	out, err := exec.CommandContext(ctx, "git", "--git-dir="+gitDir,
		"rev-parse", "--verify", "-q", ref).Output()
	if err != nil {
		return ""
	}
	return strings.TrimSpace(string(out))
}

func gitIsAncestor(ctx context.Context, gitDir, older, newer string) bool {
	return exec.CommandContext(ctx, "git", "--git-dir="+gitDir,
		"merge-base", "--is-ancestor", older, newer).Run() == nil
}

func gitCommitTime(ctx context.Context, gitDir, oid string) int64 {
	out, err := exec.CommandContext(ctx, "git", "--git-dir="+gitDir,
		"show", "-s", "--format=%ct", oid).Output()
	if err != nil {
		return 0
	}
	value, _ := strconv.ParseInt(strings.TrimSpace(string(out)), 10, 64)
	return value
}

func clearRefNamespace(ctx context.Context, gitDir, namespace string) {
	out, _ := exec.CommandContext(ctx, "git", "--git-dir="+gitDir,
		"for-each-ref", "--format=%(refname)", namespace).Output()
	refs := strings.Fields(string(out))
	if len(refs) == 0 {
		return
	}
	var batch strings.Builder
	for _, ref := range refs {
		batch.WriteString("delete " + ref + "\n")
	}
	cmd := exec.CommandContext(ctx, "git", "--git-dir="+gitDir, "update-ref", "--stdin")
	cmd.Stdin = strings.NewReader(batch.String())
	if cmd.Run() == nil {
		return
	}
	for _, ref := range refs {
		_ = exec.CommandContext(ctx, "git", "--git-dir="+gitDir,
			"update-ref", "-d", ref).Run()
	}
}

func writeGatewayConfig(path string, cfg GatewayConfig) (bool, error) {
	raw, err := json.MarshalIndent(cfg, "", "  ")
	if err != nil {
		return false, err
	}
	raw = append(raw, '\n')
	previous, _ := os.ReadFile(path)
	if bytes.Equal(previous, raw) {
		return false, nil
	}
	dir := filepath.Dir(path)
	tmp, err := os.CreateTemp(dir, ".gateway-config-*")
	if err != nil {
		return false, err
	}
	tmpName := tmp.Name()
	defer os.Remove(tmpName)
	if err := tmp.Chmod(0600); err != nil {
		tmp.Close()
		return false, err
	}
	if _, err := tmp.Write(raw); err != nil {
		tmp.Close()
		return false, err
	}
	if err := tmp.Sync(); err != nil {
		tmp.Close()
		return false, err
	}
	if err := tmp.Close(); err != nil {
		return false, err
	}
	if err := os.Rename(tmpName, path); err != nil {
		return false, err
	}
	return true, nil
}

func repositoryKey(repo Repository) string { return strings.ToLower(repo.Owner + "/" + repo.Name) }

func boundedText(raw []byte, maximum int) string {
	value := strings.Join(strings.Fields(string(raw)), " ")
	if len(value) > maximum {
		value = value[:maximum]
	}
	return value
}
