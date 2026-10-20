package mirrornode

// Native owner-authorized pull mutations for managed mirror nodes.
//
// The routing Worker authenticates an organization owner, signs the exact
// request, and hands it to the Python gateway, which calls this executor with
// one typed JSON object on stdin (action "status", "execute" or "register").
// The mirror therefore performs the merge the owner reviewed on the web even
// while the repository's source-of-truth desktop is offline: the result moves
// the base branch, the compatibility index, and that PR's isolated metadata
// branch in one atomic ref transaction,
// records a durable idempotency result, refreshes the gateway's expected refs
// pin, and returns a fresh node-signed attestation of the refs the mirror now
// serves so the relay can admit and prefer that state for the whole fleet.
//
// Every body field is treated as hostile. Repository paths come only from the
// owner-controlled gateway configuration, branch names from the committed pull
// metadata, and Git runs with fixed argument vectors, hooks disabled, and no
// ambient credentials.

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
	"regexp"
	"sort"
	"strconv"
	"strings"
	"syscall"
	"time"
)

const (
	pullMergeExecutorType  = "forkmesh.pull-merge-executor-v1"
	pullUpdateExecutorType = "forkmesh.pull-update-executor-v1"
	pullMergeJobType       = "forkmesh.pull-merge-job-v1"
	pullUpdateJobType      = "forkmesh.pull-update-job-v1"
	pullJobsDirectoryName  = "pull-jobs"
	gatewayGenerationFile  = "gateway-generation"
	pullMetadataBranch     = "forkmesh/pulls"
	maxPullMutationRequest = 8 << 10
	maxPullMetadataBytes   = 256 << 10
	maxPullPayloadBytes    = 32 << 20
	maxPullJobRecords      = 5000
	pullJobRetention       = 30 * 24 * time.Hour
	pullGitTimeout         = 5 * time.Minute
)

var (
	gitObjectIDPattern = regexp.MustCompile(`^(?:[0-9a-f]{40}|[0-9a-f]{64})$`)
	pullBranchPattern  = regexp.MustCompile(`^[A-Za-z0-9][A-Za-z0-9._/-]{0,199}$`)
	frontMatterKey     = regexp.MustCompile(`^[A-Za-z][A-Za-z0-9]*$`)
	reviewFilePattern  = regexp.MustCompile(`^[0-9]{4,}-review\.md$`)
	coAuthorTrailer    = regexp.MustCompile("(?im)^[ \t]*Co-authored-by:[^<\n]*<([^<>\\s@,]+@[^<>\\s@,]+)>[ \t]*$")
)

// Gateway operations every managed public repository advertises.
var (
	defaultGatewayOperations = []string{
		"blob", "blobs", "branches", "commit", "compare", "git-info-refs",
		"git-upload-pack", "history", "merge-preview", "raw", "release-blob",
		"search", "sizes", "stats", "tree",
	}
	// Owner-authorized mutations the Worker dispatches through this executor.
	writeGatewayOperations = []string{"merge-pull", "update-pull-branch"}
)

type pullMutationProfile struct {
	executorType  string
	jobType       string
	terminal      string
	completedRefs string
	afterFields   []string
	failures      map[string]bool
}

var pullMergeProfile = pullMutationProfile{
	executorType:  pullMergeExecutorType,
	jobType:       pullMergeJobType,
	terminal:      "merged",
	completedRefs: "refs/forkmesh/merge-completed/",
	afterFields:   []string{"baseAfter", "pullsAfter"},
	failures: map[string]bool{
		"checks_required": true, "merge_conflict": true, "pull_not_found": true,
		"pull_not_open": true, "review_required": true, "rules_required": true,
		"stale_base": true, "stale_head": true, "stale_pull_metadata": true, "unsupported_pull": true,
	},
}

var pullUpdateProfile = pullMutationProfile{
	executorType:  pullUpdateExecutorType,
	jobType:       pullUpdateJobType,
	terminal:      "updated",
	completedRefs: "refs/forkmesh/update-completed/",
	afterFields:   []string{"headAfter"},
	failures: map[string]bool{
		"already_current": true, "merge_conflict": true, "pull_not_found": true,
		"pull_not_open": true, "stale_base": true, "stale_head": true,
		"stale_pull_metadata": true, "unsupported_pull": true,
	},
}

type pullMutationRequest struct {
	SchemaVersion    int         `json:"schemaVersion"`
	Type             string      `json:"type"`
	Action           string      `json:"action"`
	Owner            string      `json:"owner"`
	Repository       string      `json:"repository"`
	PullNumber       json.Number `json:"pullNumber"`
	RequestID        string      `json:"requestId"`
	ExpectedBaseOid  string      `json:"expectedBaseOid"`
	ExpectedHeadOid  string      `json:"expectedHeadOid"`
	ExpectedPullsOid string      `json:"expectedPullsOid"`
	// A repository owner may merge without the independent-review gate. The
	// relay sets this only on a merge it has already authorized as an owner
	// action and audits it there; it is absent on every other request, so an
	// older mirror keeps parsing the requests it has always seen.
	OverrideReview bool   `json:"overrideReview,omitempty"`
	Branch         string `json:"branch,omitempty"`
	Base           string `json:"base,omitempty"`
	DeleteBranch   bool   `json:"deleteBranch,omitempty"`
	Discard        bool   `json:"discard,omitempty"`

	number  int
	digest  string
	profile pullMutationProfile
}

// pullJobRecord is the durable, owner-only idempotency result of one request.
type pullJobRecord struct {
	SchemaVersion int    `json:"schemaVersion"`
	Type          string `json:"type"`
	RequestDigest string `json:"requestDigest"`
	RequestID     string `json:"requestId"`
	Status        string `json:"status"`
	Error         string `json:"error"`
	BaseBefore    string `json:"baseBefore"`
	Head          string `json:"head"`
	PullsBefore   string `json:"pullsBefore"`
	BaseAfter     string `json:"baseAfter,omitempty"`
	PullsAfter    string `json:"pullsAfter,omitempty"`
	HeadAfter     string `json:"headAfter,omitempty"`
	Branch        string `json:"branch,omitempty"`
	Base          string `json:"base,omitempty"`
	BranchDeleted bool   `json:"branchDeleted,omitempty"`
	TrashRef      string `json:"trashRef,omitempty"`
	// The refs digest the mirror served right after the transaction landed.
	State       string `json:"state,omitempty"`
	CompletedAt int64  `json:"completedAt"`
}

// PullMutationExecutor runs the bounded merge-pull / update-pull-branch
// protocol for the gateway. It is constructed once per helper invocation.
type PullMutationExecutor struct {
	Config        Config
	Gateway       GatewayConfig
	Identity      *Identity
	Now           func() time.Time
	DaemonBaseURL string // loopback control API; empty disables the wake-up
}

// RunPullMutationExecutor reads one request from input and writes one JSON
// result to output. Invalid requests return an error (non-zero exit); policy
// outcomes such as a merge conflict are ordinary JSON results.
func RunPullMutationExecutor(input io.Reader, output io.Writer, cfg Config, gateway GatewayConfig, identity *Identity) error {
	executor := &PullMutationExecutor{Config: cfg, Gateway: gateway, Identity: identity}
	result, err := executor.Handle(input)
	if err != nil {
		return err
	}
	return json.NewEncoder(output).Encode(result)
}

func parsePullMutationRequest(input io.Reader) (pullMutationRequest, error) {
	var request pullMutationRequest
	raw, err := io.ReadAll(io.LimitReader(input, maxPullMutationRequest+1))
	if err != nil || len(raw) == 0 || len(raw) > maxPullMutationRequest {
		return request, errors.New("invalid request size")
	}
	decoder := json.NewDecoder(bytes.NewReader(raw))
	decoder.DisallowUnknownFields()
	decoder.UseNumber()
	if err := decoder.Decode(&request); err != nil {
		return request, errors.New("invalid request JSON")
	}
	if decoder.Decode(&struct{}{}) != io.EOF {
		return request, errors.New("trailing request JSON")
	}
	switch request.Type {
	case pullMergeExecutorType:
		request.profile = pullMergeProfile
	case pullUpdateExecutorType:
		request.profile = pullUpdateProfile
	case branchMergeExecutorType:
		request.profile = branchMergeProfile
	case branchDeleteExecutorType:
		request.profile = branchDeleteProfile
	default:
		return request, errors.New("unsupported executor request type")
	}
	if request.SchemaVersion != 1 ||
		(request.Action != "status" && request.Action != "execute" && request.Action != "register") {
		return request, errors.New("unsupported executor request protocol")
	}
	if isBranchProfile(request.profile) {
		return request, parseBranchMutation(&request)
	}
	if request.Branch != "" || request.Base != "" || request.DeleteBranch || request.Discard {
		return request, errors.New("unsupported executor request protocol")
	}
	// Only merging has a review gate, so only merging may waive one.
	if request.OverrideReview && request.profile.executorType != pullMergeExecutorType {
		return request, errors.New("unsupported executor request protocol")
	}
	number, err := strconv.Atoi(request.PullNumber.String())
	if err != nil || number < 1 || number > 999999999 {
		return request, errors.New("invalid pull number")
	}
	request.number = number
	request.Owner = strings.ToLower(strings.TrimSpace(request.Owner))
	request.Repository = strings.TrimSpace(request.Repository)
	request.RequestID = strings.TrimSpace(request.RequestID)
	request.ExpectedBaseOid = strings.ToLower(strings.TrimSpace(request.ExpectedBaseOid))
	request.ExpectedHeadOid = strings.ToLower(strings.TrimSpace(request.ExpectedHeadOid))
	request.ExpectedPullsOid = strings.ToLower(strings.TrimSpace(request.ExpectedPullsOid))
	if !nodePattern.MatchString(request.Owner) || !repositoryPattern.MatchString(request.Repository) ||
		!requestIDPattern.MatchString(request.RequestID) ||
		!gitObjectIDPattern.MatchString(request.ExpectedBaseOid) ||
		!gitObjectIDPattern.MatchString(request.ExpectedHeadOid) ||
		!gitObjectIDPattern.MatchString(request.ExpectedPullsOid) ||
		len(request.ExpectedBaseOid) != len(request.ExpectedHeadOid) ||
		len(request.ExpectedBaseOid) != len(request.ExpectedPullsOid) {
		return request, errors.New("invalid executor request")
	}
	canonicalValues := map[string]any{
		"owner": request.Owner, "repository": request.Repository,
		"pullNumber": number, "requestId": request.RequestID,
		"expectedBaseOid":  request.ExpectedBaseOid,
		"expectedHeadOid":  request.ExpectedHeadOid,
		"expectedPullsOid": request.ExpectedPullsOid,
		"type":             request.Type,
	}
	// Only added when set: a request that waives the gate must not share a
	// digest — and so an idempotency record — with the reviewed merge of the
	// same objects, while every ordinary request keeps the digest it has had.
	if request.OverrideReview {
		canonicalValues["overrideReview"] = true
	}
	canonical, _ := json.Marshal(canonicalValues)
	digest := sha256.Sum256(canonical)
	request.digest = hex.EncodeToString(digest[:])
	return request, nil
}

func (r pullMutationRequest) token() string {
	digest := sha256.Sum256([]byte(r.RequestID))
	return hex.EncodeToString(digest[:])
}

func (e *PullMutationExecutor) now() time.Time {
	if e.Now != nil {
		return e.Now()
	}
	return time.Now()
}

// repositoryFor resolves the request's public route (any configured owner
// alias) to one enabled public bare repository.
func (e *PullMutationExecutor) repositoryFor(request pullMutationRequest) (Repository, error) {
	for _, repo := range e.Gateway.Repositories {
		if repo.Enabled && repo.Visibility == "public" &&
			strings.EqualFold(repo.Owner, request.Owner) &&
			strings.EqualFold(repo.Name, request.Repository) && filepath.IsAbs(repo.GitDir) {
			return repo, nil
		}
	}
	return Repository{}, errors.New("repository route was not found")
}

func (e *PullMutationExecutor) jobsDirectory() (string, error) {
	if !filepath.IsAbs(e.Config.GatewayConfig) {
		return "", errors.New("gateway configuration path must be absolute")
	}
	dir := filepath.Join(filepath.Dir(e.Config.GatewayConfig), pullJobsDirectoryName)
	if err := os.MkdirAll(dir, 0700); err != nil {
		return "", err
	}
	info, err := os.Lstat(dir)
	if err != nil || !info.IsDir() || info.Mode()&os.ModeSymlink != 0 {
		return "", errors.New("pull job directory is unavailable")
	}
	return dir, nil
}

func (e *PullMutationExecutor) lockJobs(dir string, exclusive bool) (func(), error) {
	lock, err := os.OpenFile(filepath.Join(dir, "lock"), os.O_CREATE|os.O_RDWR, 0600)
	if err != nil {
		return nil, err
	}
	how := syscall.LOCK_SH
	if exclusive {
		how = syscall.LOCK_EX
	}
	if err := syscall.Flock(int(lock.Fd()), how); err != nil {
		lock.Close()
		return nil, err
	}
	return func() {
		_ = syscall.Flock(int(lock.Fd()), syscall.LOCK_UN)
		lock.Close()
	}, nil
}

func (e *PullMutationExecutor) recordPath(dir string, request pullMutationRequest) string {
	return filepath.Join(dir, request.token()+".json")
}

func (e *PullMutationExecutor) loadRecord(dir string, request pullMutationRequest) (*pullJobRecord, error) {
	path := e.recordPath(dir, request)
	info, err := os.Lstat(path)
	if errors.Is(err, os.ErrNotExist) {
		return nil, nil
	}
	if err != nil || !info.Mode().IsRegular() || info.Size() > maxPullMutationRequest {
		return nil, errors.New("pull job record is unreadable")
	}
	raw, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	var record pullJobRecord
	if err := json.Unmarshal(raw, &record); err != nil {
		return nil, errors.New("pull job record is invalid")
	}
	if record.SchemaVersion != 1 || record.Type != request.profile.jobType ||
		record.RequestID != request.RequestID {
		return nil, errors.New("pull job record is invalid")
	}
	if record.RequestDigest != request.digest {
		return nil, errors.New("merge request id was already used")
	}
	if record.Status != request.profile.terminal &&
		(record.Status != "failed" || !request.profile.failures[record.Error]) {
		return nil, errors.New("pull job record is invalid")
	}
	return &record, nil
}

func (e *PullMutationExecutor) storeRecord(dir string, request pullMutationRequest, record pullJobRecord) (*pullJobRecord, error) {
	if existing, err := e.loadRecord(dir, request); err != nil {
		return nil, err
	} else if existing != nil {
		return existing, nil
	}
	e.pruneRecords(dir)
	raw, err := json.Marshal(record)
	if err != nil {
		return nil, err
	}
	final := e.recordPath(dir, request)
	temporary, err := os.CreateTemp(dir, ".job-*")
	if err != nil {
		return nil, err
	}
	temporaryPath := temporary.Name()
	defer os.Remove(temporaryPath)
	if err := temporary.Chmod(0600); err != nil {
		temporary.Close()
		return nil, err
	}
	if _, err := temporary.Write(raw); err != nil {
		temporary.Close()
		return nil, err
	}
	if err := temporary.Sync(); err != nil {
		temporary.Close()
		return nil, err
	}
	if err := temporary.Close(); err != nil {
		return nil, err
	}
	// Link instead of rename: an identical concurrent writer keeps the first
	// record rather than replacing it.
	if err := os.Link(temporaryPath, final); err != nil && !errors.Is(err, os.ErrExist) {
		return nil, err
	}
	return e.loadRecord(dir, request)
}

// pruneRecords applies a hard local bound without rewriting any survivor.
func (e *PullMutationExecutor) pruneRecords(dir string) {
	entries, err := os.ReadDir(dir)
	if err != nil {
		return
	}
	type aged struct {
		path     string
		modified time.Time
	}
	var records []aged
	for _, entry := range entries {
		name := entry.Name()
		if !strings.HasSuffix(name, ".json") || len(name) != 64+len(".json") {
			continue
		}
		info, err := entry.Info()
		if err != nil {
			continue
		}
		records = append(records, aged{filepath.Join(dir, name), info.ModTime()})
	}
	sort.Slice(records, func(i, j int) bool { return records[i].modified.Before(records[j].modified) })
	cutoff := e.now().Add(-pullJobRetention)
	excess := len(records) - maxPullJobRecords + 1
	for i, record := range records {
		if record.modified.Before(cutoff) || i < excess {
			_ = os.Remove(record.path)
		}
	}
}

func (e *PullMutationExecutor) git(ctx context.Context, gitDir string, args ...string) (string, int, error) {
	return e.gitWithInput(ctx, gitDir, nil, nil, args...)
}

func pullGitEnvironment(extra []string) []string {
	env := []string{
		"PATH=" + os.Getenv("PATH"),
		"LANG=C", "LC_ALL=C",
		"GIT_TERMINAL_PROMPT=0",
		"GIT_CONFIG_NOSYSTEM=1",
		"GIT_CONFIG_GLOBAL=/dev/null",
		"GIT_ALLOW_PROTOCOL=file",
		"GIT_OPTIONAL_LOCKS=0",
	}
	if tmp := os.Getenv("TMPDIR"); tmp != "" {
		env = append(env, "TMPDIR="+tmp)
	}
	if home := os.Getenv("HOME"); home != "" {
		env = append(env, "HOME="+home)
	}
	return append(env, extra...)
}

func (e *PullMutationExecutor) commitIdentityEnvironment() []string {
	node := e.Gateway.Node.Name
	return []string{
		"GIT_AUTHOR_NAME=" + commitIdentityName(node), "GIT_AUTHOR_EMAIL=" + commitIdentityEmail(node),
		"GIT_COMMITTER_NAME=" + commitIdentityName(node), "GIT_COMMITTER_EMAIL=" + commitIdentityEmail(node),
	}
}

// gitWithInput runs one fixed Git argument vector against the bare repository
// with hooks disabled. It returns trimmed stdout, the exit code, and an error
// only when Git could not run at all or exceeded its output bound.
func (e *PullMutationExecutor) gitWithInput(ctx context.Context, gitDir string, stdin []byte, extraEnv []string, args ...string) (string, int, error) {
	noHooks, err := e.noHooksDirectory()
	if err != nil {
		return "", -1, err
	}
	base := []string{"--git-dir=" + gitDir, "-c", "core.hooksPath=" + noHooks}
	cmd := exec.CommandContext(ctx, "git", append(base, args...)...)
	cmd.Env = pullGitEnvironment(extraEnv)
	if stdin != nil {
		cmd.Stdin = bytes.NewReader(stdin)
	}
	var stdout bytes.Buffer
	cmd.Stdout = &stdout
	cmd.Stderr = io.Discard
	err = cmd.Run()
	if stdout.Len() > maxPullPayloadBytes {
		return "", -1, errors.New("git returned too much data")
	}
	code := 0
	if err != nil {
		var exit *exec.ExitError
		if errors.As(err, &exit) {
			code = exit.ExitCode()
		} else {
			return "", -1, err
		}
	}
	return strings.TrimSpace(stdout.String()), code, nil
}

func (e *PullMutationExecutor) noHooksDirectory() (string, error) {
	dir, err := e.jobsDirectory()
	if err != nil {
		return "", err
	}
	path := filepath.Join(dir, "no-hooks")
	if err := os.MkdirAll(path, 0700); err != nil {
		return "", err
	}
	return path, nil
}

func (e *PullMutationExecutor) refValue(ctx context.Context, gitDir, ref string) (string, error) {
	value, code, err := e.git(ctx, gitDir, "rev-parse", "--verify", "--quiet", "--end-of-options", ref+"^{commit}")
	if err != nil {
		return "", err
	}
	if code != 0 || !gitObjectIDPattern.MatchString(value) {
		return "", nil
	}
	return value, nil
}

func validPullBranch(value string) string {
	branch := strings.TrimSpace(value)
	if !pullBranchPattern.MatchString(branch) || strings.HasPrefix(branch, "-") ||
		strings.HasPrefix(branch, ".") || strings.HasPrefix(branch, "/") ||
		strings.HasSuffix(branch, ".") || strings.HasSuffix(branch, "/") ||
		strings.HasSuffix(branch, ".lock") || strings.Contains(branch, "..") ||
		strings.Contains(branch, "//") || strings.Contains(branch, "@{") ||
		strings.Contains(branch, "\\") {
		return ""
	}
	return branch
}

func coAuthorEmails(messages string) string {
	seen := map[string]bool{}
	var emails []string
	for _, match := range coAuthorTrailer.FindAllStringSubmatch(messages, -1) {
		email := strings.ToLower(match[1])
		if len(email) > 254 || seen[email] {
			continue
		}
		seen[email] = true
		emails = append(emails, email)
		if len(emails) == 16 {
			break
		}
	}
	sort.Strings(emails)
	return strings.Join(emails, ",")
}

// frontMatter is the ordered key/value header of one committed pull record.
type frontMatter struct {
	fields [][2]string
	values map[string]string
	body   []string // lines after the closing delimiter, verbatim
}

func parseFrontMatter(raw []byte) (*frontMatter, bool) {
	if len(raw) == 0 || len(raw) > maxPullMetadataBytes || bytes.IndexByte(raw, 0) >= 0 {
		return nil, false
	}
	lines := strings.Split(string(raw), "\n")
	if len(lines) == 0 || lines[0] != "---" {
		return nil, false
	}
	close := -1
	for i := 1; i < len(lines); i++ {
		if lines[i] == "---" {
			close = i
			break
		}
	}
	if close < 0 {
		return nil, false
	}
	matter := &frontMatter{values: map[string]string{}}
	for _, line := range lines[1:close] {
		key, value, ok := strings.Cut(line, ": ")
		if !ok {
			if strings.HasSuffix(line, ":") && frontMatterKey.MatchString(strings.TrimSuffix(line, ":")) {
				key, value = strings.TrimSuffix(line, ":"), ""
			} else {
				return nil, false
			}
		}
		if !frontMatterKey.MatchString(key) {
			return nil, false
		}
		if _, seen := matter.values[key]; seen {
			return nil, false
		}
		matter.fields = append(matter.fields, [2]string{key, value})
		matter.values[key] = value
	}
	matter.body = lines[close+1:]
	return matter, true
}

func (m *frontMatter) encode(replacements map[string]string, order []string) []byte {
	var out []string
	out = append(out, "---")
	seen := map[string]bool{}
	for _, field := range m.fields {
		key, value := field[0], field[1]
		if replacement, ok := replacements[key]; ok {
			value = replacement
			seen[key] = true
		}
		out = append(out, key+": "+value)
	}
	for _, key := range order {
		if !seen[key] {
			out = append(out, key+": "+replacements[key])
		}
	}
	out = append(out, "---")
	out = append(out, m.body...)
	return []byte(strings.Join(out, "\n"))
}

type pullMetadata struct {
	raw     []byte
	mode    string
	matter  *frontMatter
	base    string
	head    string
	author  string
	title   string
	derived bool // branch-backed: derive == "branch"
}

func (e *PullMutationExecutor) readPullMetadata(ctx context.Context, gitDir string, request pullMutationRequest) (*pullMetadata, string) {
	path := fmt.Sprintf(".forkmesh/pulls/%d/pull.md", request.number)
	entry, code, err := e.git(ctx, gitDir, "ls-tree", "-z", request.ExpectedPullsOid, "--", path)
	if err != nil || code != 0 || entry == "" {
		return nil, "pull_not_found"
	}
	fields := strings.SplitN(strings.TrimSuffix(entry, "\x00"), "\t", 2)
	meta := strings.Fields(fields[0])
	if len(fields) != 2 || len(meta) != 3 || meta[1] != "blob" ||
		(meta[0] != "100644" && meta[0] != "100755") {
		return nil, "pull_not_found"
	}
	content, ok := e.blobBytes(ctx, gitDir, meta[2], maxPullMetadataBytes)
	if !ok {
		return nil, "pull_not_found"
	}
	matter, ok := parseFrontMatter(content)
	if !ok {
		return nil, "unsupported_pull"
	}
	number, err := strconv.Atoi(matter.values["number"])
	if matter.values["schema"] != "forkmesh-pull-v1" || err != nil || number != request.number {
		return nil, "pull_not_found"
	}
	if matter.values["status"] != "open" {
		return nil, "pull_not_open"
	}
	base := validPullBranch(matter.values["base"])
	if base == "" {
		return nil, "unsupported_pull"
	}
	return &pullMetadata{
		raw:     content,
		mode:    meta[0],
		matter:  matter,
		base:    base,
		head:    strings.TrimSpace(matter.values["head"]),
		author:  strings.TrimSpace(matter.values["author"]),
		title:   strings.TrimSpace(matter.values["title"]),
		derived: matter.values["derive"] == "branch",
	}, ""
}

// reviewGateSatisfied requires one distinct peer approval and no unresolved
// peer request for changes on the pinned pull-metadata ref. Review files there
// already passed the owner node's signature validation when they were
// committed; this executor still parses only a bounded projection and ignores
// anything authored by the pull's own signer.
func (e *PullMutationExecutor) reviewGateSatisfied(ctx context.Context, gitDir string, request pullMutationRequest, meta *pullMetadata) bool {
	if meta.author == "" {
		return false
	}
	prefix := fmt.Sprintf(".forkmesh/pulls/%d/", request.number)
	listing, code, err := e.git(ctx, gitDir, "ls-tree", "-z", "--name-only", request.ExpectedPullsOid, "--", prefix)
	if err != nil || code != 0 {
		return false
	}
	var paths []string
	for _, path := range strings.Split(listing, "\x00") {
		if path == "" || !reviewFilePattern.MatchString(filepath.Base(path)) {
			continue
		}
		paths = append(paths, path)
	}
	sort.Strings(paths)
	if len(paths) > 1000 {
		paths = paths[:1000]
	}
	latest := map[string]string{}
	for _, path := range paths {
		raw, code, err := e.git(ctx, gitDir, "show", request.ExpectedPullsOid+":"+path)
		if err != nil || code != 0 {
			return false
		}
		matter, ok := parseFrontMatter([]byte(raw + "\n"))
		if !ok || matter.values["type"] != "review" {
			continue
		}
		reviewer := strings.TrimSpace(matter.values["author"])
		state := strings.TrimSpace(matter.values["state"])
		if reviewer == "" || reviewer == meta.author || strings.TrimSpace(matter.values["sig"]) == "" {
			continue
		}
		if state == "approved" || state == "changes_requested" {
			latest[reviewer] = state
		} else {
			delete(latest, reviewer)
		}
	}
	approved := false
	for _, state := range latest {
		if state == "changes_requested" {
			return false
		}
		if state == "approved" {
			approved = true
		}
	}
	return approved
}

// requiredChecksSatisfied applies .forkmesh/required-checks.json from the
// reviewed base commit; see checks.go. No policy means no gate. Every other
// doubt — an unreadable or invalid policy, a patch-backed pull whose tested
// commit is not a published head, an unreadable attestation listing — locks
// the merge.
func (e *PullMutationExecutor) requiredChecksSatisfied(ctx context.Context, gitDir string, request pullMutationRequest, meta *pullMetadata) bool {
	entry, code, err := e.git(ctx, gitDir, "ls-tree", "-z", request.ExpectedBaseOid, "--", requiredChecksPolicyPath)
	if err != nil || code != 0 {
		return false
	}
	if entry == "" {
		return true
	}
	fields := strings.SplitN(strings.TrimSuffix(entry, "\x00"), "\t", 2)
	entryInfo := strings.Fields(fields[0])
	if len(fields) != 2 || len(entryInfo) != 3 || entryInfo[1] != "blob" {
		return false
	}
	raw, ok := e.blobBytes(ctx, gitDir, entryInfo[2], maxRequiredChecksPolicy)
	if !ok {
		return false
	}
	policy, err := ParseRequiredChecksPolicy(raw)
	if err != nil {
		return false
	}
	if !policy.appliesTo(meta.base) {
		return true
	}
	if !meta.derived {
		return false
	}
	prefix := fmt.Sprintf(".forkmesh/pulls/%d/checks/", request.number)
	listing, code, err := e.git(ctx, gitDir, "ls-tree", "-z", request.ExpectedPullsOid, "--", prefix)
	if err != nil || code != 0 {
		return false
	}
	var attestations [][]byte
	for _, line := range strings.Split(listing, "\x00") {
		parts := strings.SplitN(line, "\t", 2)
		info := strings.Fields(parts[0])
		if len(parts) != 2 || len(info) != 3 || info[1] != "blob" ||
			!checkFilePattern.MatchString(filepath.Base(parts[1])) {
			continue
		}
		if len(attestations) >= maxCheckAttestations {
			break
		}
		if content, ok := e.blobBytes(ctx, gitDir, info[2], maxCheckAttestationBytes); ok {
			attestations = append(attestations, content)
		}
	}
	satisfied, _ := ChecksSatisfied(policy, request.Owner, request.Repository,
		request.ExpectedHeadOid, e.now(), attestations)
	return satisfied
}

func (e *PullMutationExecutor) isAncestor(ctx context.Context, gitDir, older, newer string) bool {
	_, code, err := e.git(ctx, gitDir, "merge-base", "--is-ancestor", older, newer)
	return err == nil && code == 0
}

func (e *PullMutationExecutor) commitTree(ctx context.Context, gitDir, tree string, parents []string, message string) (string, error) {
	args := []string{"commit-tree", tree}
	for _, parent := range parents {
		args = append(args, "-p", parent)
	}
	oid, code, err := e.gitWithInput(ctx, gitDir, []byte(message+"\n"), e.commitIdentityEnvironment(), args...)
	if err != nil {
		return "", err
	}
	if code != 0 || !gitObjectIDPattern.MatchString(oid) {
		return "", errors.New("commit object is invalid")
	}
	return oid, nil
}

// mergeTreeCommit merges `theirs` into `ours` in the object database only and
// returns the merge commit, "" on conflict, or the fast-forward target.
func (e *PullMutationExecutor) mergeTreeCommit(ctx context.Context, gitDir, ours, theirs, message string) (string, bool, error) {
	if e.isAncestor(ctx, gitDir, theirs, ours) {
		return ours, true, nil // nothing new to merge
	}
	if e.isAncestor(ctx, gitDir, ours, theirs) {
		return theirs, true, nil // fast-forward
	}
	output, code, err := e.git(ctx, gitDir, "merge-tree", "--write-tree", "--no-messages", ours, theirs)
	if err != nil {
		return "", false, err
	}
	if code == 1 {
		return "", false, nil // conflict
	}
	first := strings.TrimSpace(strings.SplitN(output, "\n", 2)[0])
	if code != 0 || !gitObjectIDPattern.MatchString(first) {
		return "", false, errors.New("merge tree is invalid")
	}
	commit, err := e.commitTree(ctx, gitDir, first, []string{ours, theirs}, message)
	return commit, true, err
}

// blobBytes returns one committed object's exact bytes (no trimming), bounded.
func (e *PullMutationExecutor) blobBytes(ctx context.Context, gitDir, oid string, maximum int) ([]byte, bool) {
	if !gitObjectIDPattern.MatchString(oid) {
		return nil, false
	}
	cmd := exec.CommandContext(ctx, "git", "--git-dir="+gitDir, "cat-file", "blob", oid)
	cmd.Env = pullGitEnvironment(nil)
	raw, err := cmd.Output()
	if err != nil || len(raw) == 0 || len(raw) > maximum {
		return nil, false
	}
	return raw, true
}

func (e *PullMutationExecutor) pullPayloads(ctx context.Context, gitDir string, request pullMutationRequest) ([]byte, []byte) {
	read := func(name string) []byte {
		path := fmt.Sprintf(".forkmesh/pulls/%d/%s", request.number, name)
		entry, code, err := e.git(ctx, gitDir, "ls-tree", "-z", request.ExpectedPullsOid, "--", path)
		if err != nil || code != 0 || entry == "" {
			return nil
		}
		meta := strings.Fields(strings.SplitN(strings.TrimSuffix(entry, "\x00"), "\t", 2)[0])
		if len(meta) != 3 || meta[1] != "blob" {
			return nil
		}
		raw, _ := e.blobBytes(ctx, gitDir, meta[2], maxPullPayloadBytes)
		return raw
	}
	return read("commits.mbox"), read("changes.patch")
}

// metadataCommit writes the post-mutation pull record on top of the exact
// pulls ledger commit. A merged pull keeps its metadata and review history but
// drops its open-PR patch payload in the same tree.
func (e *PullMutationExecutor) metadataCommit(ctx context.Context, gitDir string, request pullMutationRequest, meta *pullMetadata, updated []byte, message string, dropPayload bool) (string, error) {
	if len(updated) > maxPullMetadataBytes {
		return "", errors.New("pull metadata is too large")
	}
	blob, code, err := e.gitWithInput(ctx, gitDir, updated, nil, "hash-object", "-w", "--stdin")
	if err != nil {
		return "", err
	}
	if code != 0 || !gitObjectIDPattern.MatchString(blob) {
		return "", errors.New("pull metadata object is invalid")
	}
	jobs, err := e.jobsDirectory()
	if err != nil {
		return "", err
	}
	indexFile, err := os.CreateTemp(jobs, ".index-*")
	if err != nil {
		return "", err
	}
	indexPath := indexFile.Name()
	indexFile.Close()
	_ = os.Remove(indexPath)
	defer os.Remove(indexPath)
	indexEnv := []string{"GIT_INDEX_FILE=" + indexPath}
	if _, code, err := e.gitWithInput(ctx, gitDir, nil, indexEnv, "read-tree", request.ExpectedPullsOid); err != nil || code != 0 {
		return "", errors.New("pull metadata tree could not be read")
	}
	path := fmt.Sprintf(".forkmesh/pulls/%d/pull.md", request.number)
	if _, code, err := e.gitWithInput(ctx, gitDir, nil, indexEnv, "update-index", "--add", "--cacheinfo", meta.mode+","+blob+","+path); err != nil || code != 0 {
		return "", errors.New("pull metadata index update failed")
	}
	if dropPayload {
		var removals bytes.Buffer
		for _, payload := range []string{"changes.patch", "commits.mbox"} {
			fmt.Fprintf(&removals, "0 %s\t.forkmesh/pulls/%d/%s\n", blob, request.number, payload)
		}
		if _, code, err := e.gitWithInput(ctx, gitDir, removals.Bytes(), indexEnv, "update-index", "--index-info"); err != nil || code != 0 {
			return "", errors.New("pull payload removal failed")
		}
	}
	tree, code, err := e.gitWithInput(ctx, gitDir, nil, indexEnv, "write-tree")
	if err != nil || code != 0 || !gitObjectIDPattern.MatchString(tree) {
		return "", errors.New("pull metadata tree is invalid")
	}
	return e.commitTree(ctx, gitDir, tree, []string{request.ExpectedPullsOid}, message)
}

func (e *PullMutationExecutor) completedRefs(request pullMutationRequest) []string {
	prefix := request.profile.completedRefs + request.token() + "/"
	if request.profile.executorType == pullUpdateExecutorType ||
		request.profile.executorType == branchDeleteExecutorType {
		return []string{prefix + "head"}
	}
	if request.profile.executorType == branchMergeExecutorType {
		return []string{prefix + "base"}
	}
	return []string{prefix + "base", prefix + "pulls"}
}

func (e *PullMutationExecutor) completedValues(ctx context.Context, gitDir string, request pullMutationRequest) ([]string, error) {
	var values []string
	for _, ref := range e.completedRefs(request) {
		value, err := e.refValue(ctx, gitDir, ref)
		if err != nil {
			return nil, err
		}
		if value == "" {
			return nil, nil
		}
		values = append(values, value)
	}
	return values, nil
}

func (e *PullMutationExecutor) failure(request pullMutationRequest, code string) pullJobRecord {
	return pullJobRecord{
		SchemaVersion: 1, Type: request.profile.jobType, RequestDigest: request.digest,
		RequestID: request.RequestID, Status: "failed", Error: code,
		BaseBefore: request.ExpectedBaseOid, Head: request.ExpectedHeadOid,
		PullsBefore: request.ExpectedPullsOid, CompletedAt: e.now().UnixMilli(),
		Branch: request.Branch, Base: request.Base,
	}
}

func (e *PullMutationExecutor) success(ctx context.Context, gitDir string, request pullMutationRequest, after []string) pullJobRecord {
	record := pullJobRecord{
		SchemaVersion: 1, Type: request.profile.jobType, RequestDigest: request.digest,
		RequestID: request.RequestID, Status: request.profile.terminal,
		BaseBefore: request.ExpectedBaseOid, Head: request.ExpectedHeadOid,
		PullsBefore: request.ExpectedPullsOid, CompletedAt: e.now().UnixMilli(),
	}
	switch request.profile.executorType {
	case pullUpdateExecutorType:
		record.HeadAfter = after[0]
	case branchMergeExecutorType:
		record.BaseAfter = after[0]
	case branchDeleteExecutorType:
	default:
		record.BaseAfter, record.PullsAfter = after[0], after[1]
	}
	if isBranchProfile(request.profile) {
		record.Branch, record.Base = request.Branch, request.Base
		if e.branchDeletes(request) {
			record.BranchDeleted = true
			record.TrashRef = e.agentBranchTrashRef(request)
		}
	}
	if digest, err := refsSHA256(ctx, gitDir); err == nil {
		record.State = digest
	}
	return record
}

// execute performs the mutation under the exclusive job lock.
func (e *PullMutationExecutor) execute(ctx context.Context, dir, gitDir string, request pullMutationRequest) (*pullJobRecord, error) {
	if existing, err := e.loadRecord(dir, request); err != nil || existing != nil {
		return existing, err
	}
	// A completion marker without its record means the transaction landed and
	// the process died before recording it: finish the record, never redo it.
	if completed, err := e.completedValues(ctx, gitDir, request); err != nil {
		return nil, err
	} else if completed != nil {
		return e.storeRecord(dir, request, e.success(ctx, gitDir, request, completed))
	}
	if isBranchProfile(request.profile) {
		return e.executeBranch(ctx, dir, gitDir, request)
	}
	currentPulls, err := e.refValue(ctx, gitDir, "refs/heads/"+pullMetadataBranch)
	if err != nil {
		return nil, err
	}
	if currentPulls != request.ExpectedPullsOid {
		return e.storeRecord(dir, request, e.failure(request, "stale_pull_metadata"))
	}
	meta, failure := e.readPullMetadata(ctx, gitDir, request)
	if failure != "" {
		return e.storeRecord(dir, request, e.failure(request, failure))
	}
	currentBase, err := e.refValue(ctx, gitDir, "refs/heads/"+meta.base)
	if err != nil {
		return nil, err
	}
	if currentBase != request.ExpectedBaseOid {
		return e.storeRecord(dir, request, e.failure(request, "stale_base"))
	}
	if request.profile.executorType == pullUpdateExecutorType {
		return e.executeUpdate(ctx, dir, gitDir, request, meta)
	}
	return e.executeMerge(ctx, dir, gitDir, request, meta)
}

func (e *PullMutationExecutor) executeMerge(ctx context.Context, dir, gitDir string, request pullMutationRequest, meta *pullMetadata) (*pullJobRecord, error) {
	if !request.OverrideReview && !e.reviewGateSatisfied(ctx, gitDir, request, meta) {
		return e.storeRecord(dir, request, e.failure(request, "review_required"))
	}
	// The owner's review waiver does not reach required checks: the policy on
	// the base branch is the owner's lever, and a check is a machine verdict.
	if !e.requiredChecksSatisfied(ctx, gitDir, request, meta) {
		return e.storeRecord(dir, request, e.failure(request, "checks_required"))
	}
	if !e.pullRulesSatisfied(ctx, gitDir, request, meta) {
		return e.storeRecord(dir, request, e.failure(request, "rules_required"))
	}
	var baseAfter string
	replacements := map[string]string{"status": "merged"}
	// A merge-conditional reward estimate stops waiting the moment the pull
	// lands. Left at pending_merge it holds the contributor's payment out of
	// the payout queue and keeps the public ledger printing "pending merge"
	// for work that is already in. This is the same transition the desktop's
	// PullReward::advanceForMerge applies when the owner merges locally.
	if meta.matter.values["rewardStatus"] == "pending_merge" {
		if condition, ok := meta.matter.values["rewardCondition"]; !ok || condition == "merged" {
			replacements["rewardStatus"] = "pending_wallet"
		}
	}
	order := []string{}
	var transaction []string
	var messages string
	if meta.derived {
		head := validPullBranch(meta.head)
		if head == "" || head == meta.base {
			return e.storeRecord(dir, request, e.failure(request, "unsupported_pull"))
		}
		currentHead, err := e.refValue(ctx, gitDir, "refs/heads/"+head)
		if err != nil {
			return nil, err
		}
		if currentHead != request.ExpectedHeadOid {
			return e.storeRecord(dir, request, e.failure(request, "stale_head"))
		}
		merged, clean, err := e.mergeTreeCommit(ctx, gitDir, request.ExpectedBaseOid, request.ExpectedHeadOid,
			fmt.Sprintf("Merge pull request #%d", request.number))
		if err != nil {
			return nil, err
		}
		if !clean {
			return e.storeRecord(dir, request, e.failure(request, "merge_conflict"))
		}
		baseAfter = merged
		messages, _, _ = e.git(ctx, gitDir, "log", "--format=%B",
			request.ExpectedBaseOid+".."+request.ExpectedHeadOid)
		replacements["mergeBase"] = request.ExpectedBaseOid
		replacements["mergeHead"] = request.ExpectedHeadOid
		order = append(order, "mergeBase", "mergeHead")
		transaction = append(transaction, "verify refs/heads/"+head+" "+request.ExpectedHeadOid)
	} else {
		// A patch-backed pull pins its reviewed content through the ledger
		// commit itself, so the head pin must name that exact commit.
		if request.ExpectedHeadOid != request.ExpectedPullsOid {
			return e.storeRecord(dir, request, e.failure(request, "unsupported_pull"))
		}
		mbox, patch := e.pullPayloads(ctx, gitDir, request)
		if len(bytes.TrimSpace(mbox)) == 0 && len(bytes.TrimSpace(patch)) == 0 {
			return e.storeRecord(dir, request, e.failure(request, "unsupported_pull"))
		}
		jobs, err := e.jobsDirectory()
		if err != nil {
			return nil, err
		}
		noHooks, err := e.noHooksDirectory()
		if err != nil {
			return nil, err
		}
		head, err := materializePullHead(ctx, gitDir, jobs, noHooks, meta.matter.values,
			request.ExpectedBaseOid, mbox, patch)
		if err != nil {
			return nil, err
		}
		if head.head == "" {
			return e.storeRecord(dir, request, e.failure(request, "merge_conflict"))
		}
		merged, clean, err := e.mergeTreeCommit(ctx, gitDir, request.ExpectedBaseOid, head.head,
			fmt.Sprintf("Merge pull request #%d", request.number))
		if err != nil {
			return nil, err
		}
		if !clean {
			return e.storeRecord(dir, request, e.failure(request, "merge_conflict"))
		}
		baseAfter = merged
		messages, _, _ = e.git(ctx, gitDir, "log", "--format=%B", head.base+".."+head.head)
		replacements["mergeBase"] = request.ExpectedBaseOid
		replacements["mergeHead"] = head.head
		order = append(order, "mergeBase", "mergeHead")
	}
	replacements["mergedAt"] = strconv.FormatInt(e.now().UnixMilli(), 10)
	order = append(order, "mergedAt")
	if emails := coAuthorEmails(messages); emails != "" {
		replacements["coAuthors"] = emails
		order = append(order, "coAuthors")
	}
	updated := meta.matter.encode(replacements, order)
	// The waiver is recorded where the merge itself is recorded: the ledger
	// commit that closes the pull says the owner merged it unreviewed.
	message := fmt.Sprintf("pull #%d: merged", request.number)
	if request.OverrideReview {
		message += " (review gate waived by owner)"
	}
	pullsAfter, err := e.metadataCommit(ctx, gitDir, request, meta, updated,
		message, true)
	if err != nil {
		return nil, err
	}
	metadataAfter, metadataBefore, err := materializePullMetadataCommit(
		ctx, gitDir, request.number, pullsAfter, commitIdentityName(e.Gateway.Node.Name),
		commitIdentityEmail(e.Gateway.Node.Name))
	if err != nil {
		return nil, err
	}
	completed := e.completedRefs(request)
	transaction = append(transaction,
		"update refs/heads/"+meta.base+" "+baseAfter+" "+request.ExpectedBaseOid,
		"update refs/heads/"+pullMetadataBranch+" "+pullsAfter+" "+request.ExpectedPullsOid,
	)
	if metadataBefore != "" {
		transaction = append(transaction, "delete "+pullMetadataRef(request.number)+" "+metadataBefore)
	}
	compatibilityRef := pullMetadataCompatibilityRef(request.number)
	compatibilityBefore, err := e.refValue(ctx, gitDir, compatibilityRef)
	if err != nil {
		return nil, err
	}
	if compatibilityBefore == "" {
		transaction = append(transaction, "create "+compatibilityRef+" "+metadataAfter)
	} else {
		transaction = append(transaction, "update "+compatibilityRef+" "+metadataAfter+" "+compatibilityBefore)
	}
	transaction = append(transaction,
		"create "+completed[0]+" "+baseAfter,
		"create "+completed[1]+" "+pullsAfter,
	)
	return e.commitTransaction(ctx, dir, gitDir, request, transaction, []string{baseAfter, pullsAfter})
}

func (e *PullMutationExecutor) executeUpdate(ctx context.Context, dir, gitDir string, request pullMutationRequest, meta *pullMetadata) (*pullJobRecord, error) {
	if !meta.derived {
		return e.storeRecord(dir, request, e.failure(request, "unsupported_pull"))
	}
	head := validPullBranch(meta.head)
	if head == "" || head == meta.base {
		return e.storeRecord(dir, request, e.failure(request, "unsupported_pull"))
	}
	currentHead, err := e.refValue(ctx, gitDir, "refs/heads/"+head)
	if err != nil {
		return nil, err
	}
	if currentHead != request.ExpectedHeadOid {
		return e.storeRecord(dir, request, e.failure(request, "stale_head"))
	}
	if e.isAncestor(ctx, gitDir, request.ExpectedBaseOid, request.ExpectedHeadOid) {
		return e.storeRecord(dir, request, e.failure(request, "already_current"))
	}
	// First parent stays the head branch's own history, exactly as a local
	// "git merge <base>" on the checked-out head branch would record it.
	headAfter, clean, err := e.mergeTreeCommit(ctx, gitDir, request.ExpectedHeadOid, request.ExpectedBaseOid,
		fmt.Sprintf("Merge branch '%s' into %s", meta.base, head))
	if err != nil {
		return nil, err
	}
	if !clean {
		return e.storeRecord(dir, request, e.failure(request, "merge_conflict"))
	}
	completed := e.completedRefs(request)
	transaction := []string{
		"verify refs/heads/" + meta.base + " " + request.ExpectedBaseOid,
		"verify refs/heads/" + pullMetadataBranch + " " + request.ExpectedPullsOid,
		"update refs/heads/" + head + " " + headAfter + " " + request.ExpectedHeadOid,
		"create " + completed[0] + " " + headAfter,
	}
	return e.commitTransaction(ctx, dir, gitDir, request, transaction, []string{headAfter})
}

// commitTransaction applies the exact-OID ref transaction atomically. A lost
// compare-and-swap is reported as stale metadata so the owner reloads and
// reviews the moved refs instead of the mirror guessing.
func (e *PullMutationExecutor) commitTransaction(ctx context.Context, dir, gitDir string, request pullMutationRequest, lines []string, after []string) (*pullJobRecord, error) {
	script := "start\n" + strings.Join(lines, "\n") + "\nprepare\ncommit\n"
	_, code, err := e.gitWithInput(ctx, gitDir, []byte(script), nil, "update-ref", "--stdin")
	if err != nil {
		return nil, err
	}
	if code != 0 {
		if completed, err := e.completedValues(ctx, gitDir, request); err != nil {
			return nil, err
		} else if completed != nil {
			return e.storeRecord(dir, request, e.success(ctx, gitDir, request, completed))
		}
		lost := "stale_pull_metadata"
		if isBranchProfile(request.profile) {
			lost = "stale_base"
		}
		return e.storeRecord(dir, request, e.failure(request, lost))
	}
	record, err := e.storeRecord(dir, request, e.success(ctx, gitDir, request, after))
	if err != nil {
		return nil, err
	}
	e.refreshGatewayGeneration(ctx, gitDir)
	return record, nil
}

// refreshGatewayGeneration pins the gateway configuration to the refs the
// repository serves now and wakes the daemon so it republishes the catalog and
// renews the endpoint lease. The daemon also converges on its own next cycle,
// so both steps are best-effort here.
func (e *PullMutationExecutor) refreshGatewayGeneration(ctx context.Context, gitDir string) {
	digest, err := refsSHA256(ctx, gitDir)
	if err != nil {
		return
	}
	gateway, _, err := loadGatewayConfig(e.Config.GatewayConfig)
	if err != nil {
		return
	}
	changed := false
	for i := range gateway.Repositories {
		if gateway.Repositories[i].GitDir == gitDir &&
			gateway.Repositories[i].Integrity.ExpectedRefsSHA256 != digest {
			gateway.Repositories[i].Integrity.ExpectedRefsSHA256 = digest
			changed = true
		}
	}
	if changed {
		_, _ = writeGatewayConfig(e.Config.GatewayConfig, gateway)
	}
	e.wakeDaemon()
}

func (e *PullMutationExecutor) generationReady(ctx context.Context, gitDir string) bool {
	digest, err := refsSHA256(ctx, gitDir)
	if err != nil {
		return false
	}
	gateway, _, err := loadGatewayConfig(e.Config.GatewayConfig)
	if err != nil {
		return false
	}
	ready := false
	for _, repo := range gateway.Repositories {
		if repo.GitDir != gitDir {
			continue
		}
		if repo.Integrity.ExpectedRefsSHA256 != digest {
			return false
		}
		ready = true
	}
	return ready
}

func (e *PullMutationExecutor) daemonBaseURL() string {
	if e.DaemonBaseURL != "" {
		return e.DaemonBaseURL
	}
	listen := strings.TrimSpace(e.Config.Listen)
	if listen == "" {
		return ""
	}
	return "http://" + listen
}

// wakeDaemon asks the supervisor for one immediate cycle. It is a loopback
// control call that never carries repository data or credentials.
func (e *PullMutationExecutor) wakeDaemon() {
	base := e.daemonBaseURL()
	if base == "" {
		return
	}
	endpoint, err := url.Parse(base + "/v1/control/sync")
	if err != nil {
		return
	}
	client := &http.Client{Timeout: 3 * time.Second}
	request, err := http.NewRequest(http.MethodPost, endpoint.String(), nil)
	if err != nil {
		return
	}
	response, err := client.Do(request)
	if err != nil {
		return
	}
	_, _ = io.Copy(io.Discard, io.LimitReader(response.Body, 4096))
	response.Body.Close()
}

// gatewayConfigDigest is the identity of one exact on-disk gateway generation.
func gatewayConfigDigest(path string) string {
	raw, err := os.ReadFile(path)
	if err != nil {
		return ""
	}
	digest := sha256.Sum256(raw)
	return hex.EncodeToString(digest[:])
}

func gatewayGenerationMarkerPath(gatewayConfigPath string) string {
	return filepath.Join(filepath.Dir(gatewayConfigPath), pullJobsDirectoryName, gatewayGenerationFile)
}

// recordGatewayGeneration notes that the running gateway reloaded this exact
// configuration in place, so the daemon's next cycle need not restart it.
func recordGatewayGeneration(gatewayConfigPath string) error {
	marker := gatewayGenerationMarkerPath(gatewayConfigPath)
	if err := os.MkdirAll(filepath.Dir(marker), 0700); err != nil {
		return err
	}
	digest := gatewayConfigDigest(gatewayConfigPath)
	if digest == "" {
		return errors.New("gateway configuration is unreadable")
	}
	temporary, err := os.CreateTemp(filepath.Dir(marker), ".generation-*")
	if err != nil {
		return err
	}
	temporaryPath := temporary.Name()
	defer os.Remove(temporaryPath)
	if _, err := temporary.WriteString(digest + "\n"); err != nil {
		temporary.Close()
		return err
	}
	if err := temporary.Sync(); err != nil {
		temporary.Close()
		return err
	}
	if err := temporary.Close(); err != nil {
		return err
	}
	return os.Rename(temporaryPath, marker)
}

// gatewayServesCurrentGeneration reports whether the gateway already reloaded
// the configuration exactly as it is on disk now.
func gatewayServesCurrentGeneration(gatewayConfigPath string) bool {
	raw, err := os.ReadFile(gatewayGenerationMarkerPath(gatewayConfigPath))
	if err != nil {
		return false
	}
	marker := strings.TrimSpace(string(raw))
	return digestPattern.MatchString(marker) && marker == gatewayConfigDigest(gatewayConfigPath)
}

// attestation signs the refs digest the mirror serves right now so the relay
// can admit (and, for an owner-authorized merge, prefer) that generation.
func (e *PullMutationExecutor) attestation(ctx context.Context, gitDir string, repo Repository) map[string]any {
	if e.Identity == nil {
		return nil
	}
	digest, err := routingRefsSHA256(ctx, gitDir)
	if err != nil || !digestPattern.MatchString(digest) {
		return nil
	}
	ts := strconv.FormatInt(e.now().UnixMilli(), 10)
	canonical := strings.Join([]string{
		"forkmesh-repostate-v1", e.Gateway.Node.Name, repo.Name, digest, ts,
	}, "\n")
	return map[string]any{
		"state":    digest,
		"stateTs":  ts,
		"stateSig": e.Identity.Sign([]byte(canonical)),
	}
}

func (e *PullMutationExecutor) publicResult(ctx context.Context, gitDir string, repo Repository, request pullMutationRequest, record *pullJobRecord) map[string]any {
	if record == nil {
		return map[string]any{
			"ok": true, "status": "missing", "requestId": request.RequestID,
			"generationReady": false,
		}
	}
	if record.Status == "failed" {
		return map[string]any{
			"ok": false, "status": "failed", "requestId": request.RequestID,
			"error": record.Error, "generationReady": false,
		}
	}
	result := map[string]any{
		"ok": true, "status": request.profile.terminal, "requestId": request.RequestID,
		"baseBefore": record.BaseBefore, "head": record.Head, "pullsBefore": record.PullsBefore,
		"generationReady": e.generationReady(ctx, gitDir),
	}
	switch {
	case request.profile.executorType == pullUpdateExecutorType:
		result["headAfter"] = record.HeadAfter
	case isBranchProfile(request.profile):
		delete(result, "pullsBefore")
		result["branch"] = record.Branch
		result["base"] = record.Base
		result["branchDeleted"] = record.BranchDeleted
		if record.TrashRef != "" {
			result["trashRef"] = record.TrashRef
		}
		if request.profile.executorType == branchMergeExecutorType {
			result["baseAfter"] = record.BaseAfter
		}
	default:
		result["baseAfter"] = record.BaseAfter
		result["pullsAfter"] = record.PullsAfter
	}
	for key, value := range e.attestation(ctx, gitDir, repo) {
		result[key] = value
	}
	return result
}

// Handle runs one executor request and returns the JSON-ready result.
func (e *PullMutationExecutor) Handle(input io.Reader) (map[string]any, error) {
	raw, err := io.ReadAll(io.LimitReader(input, maxPullRulesRequest+1))
	if err != nil {
		return nil, errors.New("invalid request size")
	}
	var envelope struct {
		Type string `json:"type"`
	}
	if json.Unmarshal(raw, &envelope) == nil && envelope.Type == pullRulesEvaluateType {
		return e.evaluateRulesRequest(raw)
	}
	request, err := parsePullMutationRequest(bytes.NewReader(raw))
	if err != nil {
		return nil, err
	}
	repo, err := e.repositoryFor(request)
	if err != nil {
		return nil, err
	}
	dir, err := e.jobsDirectory()
	if err != nil {
		return nil, err
	}
	ctx, cancel := context.WithTimeout(context.Background(), pullGitTimeout+time.Minute)
	defer cancel()
	switch request.Action {
	case "register":
		if err := recordGatewayGeneration(e.Config.GatewayConfig); err != nil {
			return nil, err
		}
		e.wakeDaemon()
		return map[string]any{"ok": true, "status": "registered", "requestId": request.RequestID}, nil
	case "status":
		unlock, err := e.lockJobs(dir, false)
		if err != nil {
			return nil, err
		}
		record, err := e.loadRecord(dir, request)
		unlock()
		if err != nil {
			return nil, err
		}
		return e.publicResult(ctx, repo.GitDir, repo, request, record), nil
	}
	unlock, err := e.lockJobs(dir, true)
	if err != nil {
		return nil, err
	}
	record, err := e.execute(ctx, dir, repo.GitDir, request)
	unlock()
	if err != nil {
		return nil, err
	}
	return e.publicResult(ctx, repo.GitDir, repo, request, record), nil
}

// actionsGatewayOperations are served only while the workflow runner is
// enabled and its sandbox works; see actions.go.
var actionsGatewayOperations = []string{"actions-control", "actions-status"}

// ensureGatewayActions advertises, or withdraws, the two Actions operations
// and the paths the Python gateway requires with them. The control directory
// is the gateway's own: mode 0700, the only form it accepts.
func ensureGatewayActions(gateway *GatewayConfig, gatewayConfigPath string, enabled bool) error {
	directory := filepath.Dir(gatewayConfigPath)
	if enabled {
		control := filepath.Join(directory, actionsControlDirectoryName)
		if err := os.MkdirAll(control, 0o700); err != nil {
			return err
		}
		if err := os.Chmod(control, 0o700); err != nil {
			return err
		}
		gateway.ActionsSummaryPath = filepath.Join(directory, actionsSummaryFileName)
		gateway.ActionsControlPath = control
	} else {
		gateway.ActionsSummaryPath = ""
		gateway.ActionsControlPath = ""
	}
	for i := range gateway.Repositories {
		repo := &gateway.Repositories[i]
		ops := repo.Operations[:0:0]
		for _, op := range repo.Operations {
			if op != "actions-control" && op != "actions-status" {
				ops = append(ops, op)
			}
		}
		if enabled && repo.Enabled && repo.Visibility == "public" {
			ops = append(ops, actionsGatewayOperations...)
		}
		sort.Strings(ops)
		repo.Operations = ops
	}
	return nil
}

// ensureGatewayCapabilities upgrades an existing gateway configuration in
// place: every served public repository advertises the read-only preview
// operation, and — unless the operator disabled the executor — the two
// owner-authorized pull mutations the Worker dispatches to this node.
func ensureGatewayCapabilities(gateway *GatewayConfig, executorCommand []string, enableWrites bool) {
	for i := range gateway.Repositories {
		repo := &gateway.Repositories[i]
		if !repo.Enabled {
			continue
		}
		present := map[string]bool{}
		for _, op := range repo.Operations {
			present[op] = true
		}
		var ops []string
		for _, op := range repo.Operations {
			if !enableWrites && (op == "merge-pull" || op == "update-pull-branch") {
				continue
			}
			ops = append(ops, op)
		}
		for _, op := range defaultGatewayOperations {
			if !present[op] {
				ops = append(ops, op)
				present[op] = true
			}
		}
		if enableWrites {
			for _, op := range writeGatewayOperations {
				if !present[op] {
					ops = append(ops, op)
					present[op] = true
				}
			}
		}
		sort.Strings(ops)
		repo.Operations = ops
	}
	if enableWrites {
		gateway.MergeExecutorCommand = append([]string(nil), executorCommand...)
	} else {
		gateway.MergeExecutorCommand = nil
	}
}
