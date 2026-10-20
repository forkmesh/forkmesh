package mirrornode

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"slices"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"
	"unicode"
)

type CatalogPublisher struct {
	URL     string
	Owner   string
	Node    string
	Version string
	// CloneURL identifies the canonical source used to group its mirrors.
	CloneURL string
	// MirrorURL is a replaceable public Git origin, never repository identity.
	MirrorURL string
	Identity  *Identity
	Client    *http.Client
	// LookPath is injectable for deterministic capability tests. Production
	// uses exec.LookPath against the service account's PATH; agent installers
	// place both supported CLIs in /usr/local/bin for that account.
	LookPath func(string) (string, error)
	// DisableAgents withholds agentProviders when this node does not claim
	// website agent jobs.
	DisableAgents bool
	MirrorOf      string
	LastSyncedAt  time.Time
}

// EndpointPublisher renews the account-bound HTTPS routing lease.
type EndpointPublisher struct {
	CatalogURL string
	Node       string
	BaseURL    string
	MirrorURL  string
	Version    string
	Identity   *Identity
	Client     *http.Client
}

func (p *EndpointPublisher) Publish(ctx context.Context) (bool, error) {
	endpointURL, err := mirrorEndpointURL(p.CatalogURL)
	if err != nil {
		return false, err
	}
	payload, err := p.Payload()
	if err != nil {
		return false, err
	}
	body, err := json.Marshal(payload)
	if err != nil {
		return false, err
	}
	req, err := http.NewRequestWithContext(
		ctx, http.MethodPost, endpointURL, bytes.NewReader(body))
	if err != nil {
		return false, err
	}
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("Accept", "application/json")
	req.Header.Set("User-Agent", "ForkMesh-Mirror-Node/"+p.Version)
	client := p.Client
	if client == nil {
		client = &http.Client{Timeout: 30 * time.Second}
	}
	response, err := client.Do(req)
	if err != nil {
		return false, err
	}
	defer response.Body.Close()
	raw, err := io.ReadAll(io.LimitReader(response.Body, 64<<10))
	if err != nil {
		return false, err
	}
	return p.ValidateResult(response.StatusCode, raw)
}

// Payload builds the independently signed endpoint lease body. The same body
// can travel over HTTPS or the already-authenticated node event WebSocket;
// the signature, not the transport, remains the authority.
func (p *EndpointPublisher) Payload() (map[string]any, error) {
	issuedAt := time.Now().UnixMilli()
	messageFields := []string{
		"forkmesh-https-endpoint-v1", p.Node, p.BaseURL,
		p.Identity.PublicKey(), strconv.FormatInt(issuedAt, 10),
	}
	if p.MirrorURL != "" {
		messageFields = []string{
			"forkmesh-https-endpoint-v2", p.Node, p.BaseURL, p.MirrorURL,
			p.Identity.PublicKey(), strconv.FormatInt(issuedAt, 10),
		}
	}
	message := strings.Join(messageFields, "\n")
	payload := map[string]any{
		"node": p.Node, "baseUrl": p.BaseURL,
		"publicKey": p.Identity.PublicKey(), "issuedAt": issuedAt,
		"signature": p.Identity.Sign([]byte(message)),
	}
	if p.MirrorURL != "" {
		payload["mirrorUrl"] = p.MirrorURL
	}
	return payload, nil
}

// ValidateResult applies the identical lease-result checks to the HTTPS and
// WebSocket transports.
func (p *EndpointPublisher) ValidateResult(status int, raw []byte) (bool, error) {
	if status < 200 || status >= 300 {
		return false, fmt.Errorf("endpoint registration returned HTTP %d: %s",
			status, boundedText(raw, 240))
	}
	var result struct {
		OK        bool   `json:"ok"`
		Node      string `json:"node"`
		BaseURL   string `json:"baseUrl"`
		MirrorURL string `json:"mirrorUrl"`
		Health    string `json:"health"`
	}
	if err := json.Unmarshal(raw, &result); err != nil {
		return false, errors.New("endpoint registration returned invalid JSON")
	}
	if !result.OK || result.Node != p.Node || result.BaseURL != p.BaseURL ||
		result.MirrorURL != p.MirrorURL ||
		(result.Health != "active" && result.Health != "pending") {
		return false, errors.New("endpoint registration returned invalid lease state")
	}
	return result.Health == "active", nil
}

func mirrorEndpointURL(catalogURL string) (string, error) {
	parsed, err := url.Parse(catalogURL)
	if err != nil || parsed.Scheme == "" || parsed.Host == "" ||
		(parsed.Scheme != "https" && parsed.Scheme != "http") {
		return "", errors.New("catalogUrl is not an absolute HTTP URL")
	}
	parsed.Path = "/api/mirrors/https"
	parsed.RawPath = ""
	parsed.RawQuery = ""
	parsed.Fragment = ""
	return parsed.String(), nil
}

func (p *CatalogPublisher) Publish(ctx context.Context, repo Repository, stateHash string) error {
	record, err := p.record(ctx, repo, stateHash)
	if err != nil {
		return err
	}
	body, err := json.Marshal(record)
	if err != nil {
		return err
	}
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, p.URL, bytes.NewReader(body))
	if err != nil {
		return err
	}
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("Accept", "application/json")
	req.Header.Set("User-Agent", "ForkMesh-Mirror-Node/"+p.Version)
	client := p.Client
	if client == nil {
		client = &http.Client{Timeout: 30 * time.Second}
	}
	response, err := client.Do(req)
	if err != nil {
		return err
	}
	defer response.Body.Close()
	if response.StatusCode < 200 || response.StatusCode >= 300 {
		body, _ := io.ReadAll(io.LimitReader(response.Body, 1024))
		return fmt.Errorf("catalog returned HTTP %d: %s", response.StatusCode,
			boundedText(body, 240))
	}
	return nil
}

func (p *CatalogPublisher) record(ctx context.Context, repo Repository, stateHash string) (map[string]any, error) {
	branch := gitValue(ctx, repo.GitDir, "symbolic-ref", "--short", "HEAD")
	commit := gitValue(ctx, repo.GitDir, "rev-parse", "HEAD")
	if branch == "" || commit == "" || !digestPattern.MatchString(stateHash) {
		return nil, fmt.Errorf("repository head or state hash unavailable")
	}
	now := strconv.FormatInt(time.Now().UnixMilli(), 10)
	rootCommit := firstLine(gitValue(ctx, repo.GitDir, "rev-list", "--max-parents=0", "HEAD"))
	commitCount := gitValue(ctx, repo.GitDir, "rev-list", "--count", "HEAD")
	mergeCount := gitValue(ctx, repo.GitDir, "rev-list", "--count", "--merges", "HEAD")
	activityWeeks, mergeActivityWeeks := catalogActivityWeeks(ctx, repo.GitDir)
	branchCount := 0
	for _, ref := range gitLines(ctx, repo.GitDir, "for-each-ref", "--format=%(refname)", "refs/heads") {
		if ref == "refs/heads/forkmesh/pulls" || strings.HasPrefix(ref, "refs/heads/forkmesh/pull/") {
			continue
		}
		branchCount++
	}
	metadataPaths := gitLines(ctx, repo.GitDir, "ls-tree", "-r", "--name-only", "HEAD", "--", ".forkmesh")
	paths := append([]string(nil), metadataPaths...)
	if gitValue(ctx, repo.GitDir, "rev-parse", "--verify", "-q", "refs/heads/forkmesh/pulls^{commit}") != "" {
		// The collaboration ledger may contain current records while main
		// retains older directory-form records. Count their unique pull numbers
		// across both refs so desktop and headless mirrors publish the same total.
		paths = append(paths, gitLines(ctx, repo.GitDir, "ls-tree", "-r", "--name-only", "refs/heads/forkmesh/pulls", "--", ".forkmesh/pulls")...)
	}
	// Independent per-PR branches are authoritative. Union them with the
	// compatibility index so a missing or lagging aggregate ref cannot make
	// valid pull requests disappear from the published catalog.
	for _, ref := range gitLines(ctx, repo.GitDir, "for-each-ref", "--format=%(refname)", "refs/heads/forkmesh/pull/") {
		paths = append(paths, gitLines(ctx, repo.GitDir, "ls-tree", "-r", "--name-only", ref, "--", ".forkmesh/pulls")...)
	}
	issueCount, issueMax, pullCount, discussionCount, _ := catalogPathCounts(paths)
	issueCount = catalogOpenIssueCount(ctx, repo.GitDir, "HEAD", paths)
	artifacts := releaseArtifactCount(repo.ReleaseStore)
	size := directorySize(repo.GitDir)
	memUsed, memTotal := memoryUsage()
	diskUsed, diskTotal := diskUsage(repo.GitDir)
	record := map[string]any{
		"owner": p.Owner, "name": repo.Name, "visibility": "public",
		"mirrorEncryption": "", "opaqueRepoId": "", "keyEpoch": 0,
		"encryptedManifestHash": "", "encryptedManifestSig": "",
		"sizeBytes": size, "description": catalogDescription(ctx, repo.GitDir),
		"logoMetadata": map[string]any{"description": "", "languages": map[string]int64{}, "topics": []string{}, "fileStructure": []string{}, "frameworks": []string{}, "projectCategory": ""},
		"cloneUrl":     p.CloneURL, "solana": "", "channel": "#" + p.Owner + "-" + repo.Name,
		"hostedSince": "", "lastSync": now, "updatedAt": now,
		"rootCommit": rootCommit, "source": "remote-clone", "commit": commit, "branch": branch,
		"issueCount": strconv.Itoa(issueCount), "issueMaxNumber": strconv.Itoa(issueMax),
		"commitCount": commitCount, "mergeCount": mergeCount,
		"branchCount": strconv.Itoa(branchCount),
		"pullCount":   strconv.Itoa(pullCount), "discussionCount": strconv.Itoa(discussionCount),
		"activityWeeks": activityWeeks, "mergeActivityWeeks": mergeActivityWeeks,
		"worktreeCount": "0", "artifactCount": strconv.Itoa(artifacts),
		"platform": runtime.GOOS, "version": p.Version, "nodeId": "",
		"clonesServed": "0", "websiteServed": "0", "maintainer": p.Identity.PublicKey(),
		"signature": "", "stateHash": stateHash, "stateSig": "",
		"cpuPercent": cpuPercent(), "memUsedBytes": memUsed, "memTotalBytes": memTotal,
		"diskUsedBytes": diskUsed, "diskTotalBytes": diskTotal,
		"machineName": p.Node, "runtimeMode": "headless",
	}
	if p.MirrorURL != "" {
		record["mirrorUrl"] = strings.TrimRight(p.MirrorURL, "/") + "/" +
			url.PathEscape(repo.Owner) + "/" + url.PathEscape(repo.Name) + ".git"
	}
	if providers := p.agentProviders(); len(providers) > 0 {
		record["agentProviders"] = providers
	}
	if p.MirrorOf != "" {
		record["mirrorOf"] = p.MirrorOf
		record["upstreamCommit"] = commit
		if !p.LastSyncedAt.IsZero() {
			record["lastSyncedAt"] = strconv.FormatInt(p.LastSyncedAt.UnixMilli(), 10)
		}
	}
	if forkParentPattern.MatchString(repo.ForkedFrom) &&
		!strings.EqualFold(repo.ForkedFrom, p.Owner+"/"+repo.Name) {
		record["forkedFrom"] = strings.ToLower(repo.ForkedFrom)
	}
	statePayload := "forkmesh-repostate-v1\n" + p.Owner + "\n" + repo.Name + "\n" + stateHash + "\n" + now
	record["stateSig"] = p.Identity.Sign([]byte(statePayload))
	signed := make(map[string]any, len(record)-1)
	for key, value := range record {
		if key != "signature" {
			signed[key] = value
		}
	}
	canonical, err := json.Marshal(signed)
	if err != nil {
		return nil, err
	}
	digest := sha256.Sum256(canonical)
	record["catalogSigVersion"] = 2
	record["catalogSig"] = p.Identity.Sign([]byte("forkmesh-catalog-v2\n" + hex.EncodeToString(digest[:])))
	return record, nil
}

func (p *CatalogPublisher) agentProviders() []string {
	// A node that does not claim website agent jobs must not look like an
	// agent target, or the website queues work nothing will ever run.
	if p.DisableAgents {
		return nil
	}
	lookPath := p.LookPath
	if lookPath == nil {
		lookPath = exec.LookPath
	}
	providers := make([]string, 0, 3)
	for _, candidate := range []struct {
		binary   string
		provider string
	}{
		{binary: "claude", provider: "claude-code"},
		{binary: "codex", provider: "codex"},
	} {
		if _, err := lookPath(candidate.binary); err == nil {
			providers = append(providers, candidate.provider)
		}
	}
	if _, err := lookPath("ollama"); err == nil && slices.Contains(providers, "codex") {
		providers = append(providers, "local-llama")
	}
	return providers
}

func gitValue(ctx context.Context, gitDir string, args ...string) string {
	base := append([]string{"--git-dir=" + gitDir}, args...)
	out, err := exec.CommandContext(ctx, "git", base...).Output()
	if err != nil {
		return ""
	}
	return strings.TrimSpace(string(out))
}

func gitLines(ctx context.Context, gitDir string, args ...string) []string {
	value := gitValue(ctx, gitDir, args...)
	if value == "" {
		return nil
	}
	return strings.Split(value, "\n")
}

func countLines(value string) int {
	if strings.TrimSpace(value) == "" {
		return 0
	}
	return len(strings.Split(strings.TrimSpace(value), "\n"))
}

func catalogActivityWeeks(ctx context.Context, gitDir string) ([]int, []int) {
	const weeks = 52
	const weekSeconds = int64(7 * 24 * 60 * 60)
	commits := make([]int, weeks)
	merges := make([]int, weeks)
	start := time.Now().Unix() - int64(weeks)*weekSeconds
	lines := gitLines(ctx, gitDir, "rev-list", "--since=52 weeks ago", "--timestamp", "--parents", "HEAD")
	for _, raw := range lines {
		fields := strings.Fields(raw)
		if len(fields) < 2 {
			continue
		}
		timestamp, err := strconv.ParseInt(fields[0], 10, 64)
		if err != nil {
			continue
		}
		index := int((timestamp - start) / weekSeconds)
		if index < 0 {
			index = 0
		} else if index >= weeks {
			index = weeks - 1
		}
		commits[index]++
		if len(fields) >= 4 {
			merges[index]++
		}
	}
	return commits, merges
}

const (
	catalogAboutPath      = "HEAD:.forkmesh/info.json"
	catalogAboutMaxBytes  = 64 << 10
	catalogDescriptionMax = 240
)

func catalogDescription(ctx context.Context, gitDir string) string {
	size, err := strconv.Atoi(gitValue(ctx, gitDir, "cat-file", "-s", catalogAboutPath))
	if err != nil || size <= 0 || size > catalogAboutMaxBytes {
		return ""
	}
	var about map[string]any
	if json.Unmarshal([]byte(gitValue(ctx, gitDir, "cat-file", "blob", catalogAboutPath)), &about) != nil {
		return ""
	}
	for _, key := range []string{"about", "description"} {
		text, _ := about[key].(string)
		text = strings.Join(strings.FieldsFunc(text, func(r rune) bool {
			return unicode.IsSpace(r) || unicode.IsControl(r) || r == '\u2028' || r == '\u2029'
		}), " ")
		if text == "" {
			continue
		}
		if runes := []rune(text); len(runes) > catalogDescriptionMax {
			text = strings.TrimSpace(string(runes[:catalogDescriptionMax]))
		}
		return text
	}
	return ""
}

func firstLine(value string) string {
	if index := strings.IndexByte(value, '\n'); index >= 0 {
		value = value[:index]
	}
	if len(value) > 64 {
		value = value[:64]
	}
	return strings.TrimSpace(value)
}

func catalogPathCounts(paths []string) (issues, maxIssue, pulls, discussions, artifacts int) {
	pullNumbers := make(map[string]struct{})
	for _, path := range paths {
		path = strings.TrimSpace(path)
		if strings.HasPrefix(path, ".forkmesh/issues/open/") && strings.HasSuffix(path, ".json") {
			issues++
		}
		if strings.HasPrefix(path, ".forkmesh/issues/") {
			for _, part := range strings.Split(path, "/") {
				if value, err := strconv.Atoi(strings.TrimPrefix(strings.TrimSuffix(part, ".json"), "issue-")); err == nil && value > maxIssue {
					maxIssue = value
				}
			}
		}
		parts := strings.Split(path, "/")
		if len(parts) >= 4 && parts[0] == ".forkmesh" && parts[1] == "pulls" {
			number := ""
			if _, err := strconv.Atoi(parts[2]); err == nil {
				number = parts[2]
			} else if parts[2] == "open" && len(parts) >= 4 {
				candidate := strings.TrimSuffix(
					strings.TrimPrefix(parts[3], "pull-"), ".json")
				if _, err := strconv.Atoi(candidate); err == nil {
					number = candidate
				}
			}
			if number != "" {
				pullNumbers[number] = struct{}{}
			}
		}
		if strings.HasPrefix(path, ".forkmesh/discussions/") &&
			(strings.HasSuffix(path, "/discussion.md") ||
				strings.HasSuffix(path, "/discussion.json")) {
			discussions++
		}
		if strings.HasPrefix(path, ".forkmesh/artifacts/") {
			artifacts++
		}
	}
	pulls = len(pullNumbers)
	return
}

// releaseArtifactCount reports the physical content-addressed release blobs a
// node can actually serve. Repository metadata may contain historical artifact
// records without their payloads (or payloads without Git metadata), so the CAS
// is the one authoritative source shared with the desktop mirror view.
func releaseArtifactCount(releaseStore string) int {
	if strings.TrimSpace(releaseStore) == "" {
		return 0
	}
	shards, err := os.ReadDir(filepath.Join(releaseStore, "sha256"))
	if err != nil {
		return 0
	}
	count := 0
	for _, shard := range shards {
		if len(shard.Name()) != 2 || shard.Type()&os.ModeSymlink != 0 || !shard.IsDir() {
			continue
		}
		blobs, err := os.ReadDir(filepath.Join(releaseStore, "sha256", shard.Name()))
		if err != nil {
			continue
		}
		for _, blob := range blobs {
			hash := blob.Name()
			if blob.Type()&os.ModeSymlink != 0 || !blob.IsDir() ||
				!digestPattern.MatchString(hash) || !strings.HasPrefix(hash, shard.Name()) {
				continue
			}
			info, err := os.Lstat(filepath.Join(
				releaseStore, "sha256", shard.Name(), hash, "data"))
			if err == nil && info.Mode().IsRegular() && info.Mode()&os.ModeSymlink == 0 {
				count++
			}
		}
	}
	return count
}

// catalogOpenIssueCount matches the desktop's visible-open count. Deleted
// issue records deliberately remain in Git as signed audit history; a creator's
// delete-self event hides the issue without erasing that history. Unreadable or
// malformed records remain counted so a damaged record cannot silently vanish.
func catalogOpenIssueCount(ctx context.Context, gitDir, ref string, paths []string) int {
	issuePaths := make([]string, 0)
	for _, path := range paths {
		parts := strings.Split(strings.TrimSpace(path), "/")
		if len(parts) != 5 || parts[0] != ".forkmesh" || parts[1] != "issues" ||
			parts[2] != "open" || parts[4] != "issue-"+parts[3]+".json" {
			continue
		}
		if _, err := strconv.Atoi(parts[3]); err == nil {
			issuePaths = append(issuePaths, path)
		}
	}
	count := len(issuePaths)
	if count == 0 {
		return 0
	}
	var input bytes.Buffer
	for _, path := range issuePaths {
		fmt.Fprintf(&input, "%s:%s\n", ref, path)
	}
	command := exec.CommandContext(ctx, "git", "--git-dir="+gitDir, "cat-file", "--batch")
	command.Stdin = &input
	output, err := command.Output()
	if err != nil {
		return count
	}
	position := 0
	for range issuePaths {
		lineEnd := bytes.IndexByte(output[position:], '\n')
		if lineEnd < 0 {
			break
		}
		lineEnd += position
		header := strings.Fields(string(output[position:lineEnd]))
		position = lineEnd + 1
		if len(header) < 3 {
			continue
		}
		size, sizeErr := strconv.Atoi(header[2])
		if sizeErr != nil || size < 0 || position+size > len(output) {
			continue
		}
		blob := output[position : position+size]
		position += size
		if position < len(output) && output[position] == '\n' {
			position++
		}
		var record struct {
			Events []struct {
				Type   string `json:"type"`
				Target string `json:"target"`
				Author string `json:"author"`
			} `json:"events"`
		}
		if json.Unmarshal(blob, &record) != nil {
			continue
		}
		creator := ""
		for _, event := range record.Events {
			if event.Type == "open" {
				creator = event.Author
				break
			}
		}
		if creator == "" {
			continue
		}
		for _, event := range record.Events {
			if event.Type == "delete" && event.Target == "self" && event.Author == creator {
				count--
				break
			}
		}
	}
	return count
}

func directorySize(root string) int64 {
	var total int64
	_ = filepathWalk(root, func(info os.FileInfo) {
		if info.Mode().IsRegular() {
			total += info.Size()
		}
	})
	return total
}

var filepathWalk = func(root string, visit func(os.FileInfo)) error {
	return filepath.Walk(root, func(_ string, info os.FileInfo, err error) error {
		if err == nil {
			visit(info)
		}
		return nil
	})
}

func memoryUsage() (uint64, uint64) {
	if raw, err := readMemInfo(); err == nil {
		var total, available uint64
		for _, line := range strings.Split(string(raw), "\n") {
			fields := strings.Fields(line)
			if len(fields) < 2 {
				continue
			}
			value, err := strconv.ParseUint(fields[1], 10, 64)
			if err != nil {
				continue
			}
			switch fields[0] {
			case "MemTotal:":
				total = value * 1024
			case "MemAvailable:":
				available = value * 1024
			}
		}
		if total > 0 && available <= total {
			return total - available, total
		}
	}
	return systemMemoryUsage()
}

var readMemInfo = func() ([]byte, error) { return os.ReadFile("/proc/meminfo") }

func diskUsage(path string) (uint64, uint64) {
	var info syscall.Statfs_t
	if syscall.Statfs(path, &info) != nil {
		return 0, 0
	}
	total := info.Blocks * uint64(info.Bsize)
	free := info.Bavail * uint64(info.Bsize)
	return total - free, total
}

var (
	cpuMu       sync.Mutex
	cpuPrevious cpuTimes
	readCPUStat = func() ([]byte, error) { return os.ReadFile("/proc/stat") }
)

type cpuTimes struct {
	idle  uint64
	total uint64
}

func parseCPUTimes(raw []byte) (cpuTimes, bool) {
	line := strings.SplitN(string(raw), "\n", 2)[0]
	fields := strings.Fields(line)
	if len(fields) < 5 || fields[0] != "cpu" {
		return cpuTimes{}, false
	}
	var sample cpuTimes
	for index, field := range fields[1:] {
		value, err := strconv.ParseUint(field, 10, 64)
		if err != nil {
			return cpuTimes{}, false
		}
		sample.total += value
		if index == 3 || index == 4 { // idle + iowait
			sample.idle += value
		}
	}
	return sample, sample.total > 0
}

func cpuPercent() int {
	raw, err := readCPUStat()
	if err != nil {
		return 0
	}
	current, ok := parseCPUTimes(raw)
	if !ok {
		return 0
	}
	cpuMu.Lock()
	previous := cpuPrevious
	cpuPrevious = current
	cpuMu.Unlock()
	if previous.total == 0 || current.total <= previous.total ||
		current.idle < previous.idle {
		return 0
	}
	totalDelta := current.total - previous.total
	idleDelta := current.idle - previous.idle
	if idleDelta >= totalDelta {
		return 0
	}
	percent := int((100*(totalDelta-idleDelta) + totalDelta/2) / totalDelta)
	if percent > 100 {
		return 100
	}
	return percent
}
