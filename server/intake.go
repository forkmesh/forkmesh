package mirrornode

// Native collaboration intake for managed mirror nodes.
//
// The desktop used to be launched offscreen as a compatibility materializer.
// That made a GUI application part of the server runtime and broke delivery as
// soon as the desktop was not installed.  The mirror daemon now consumes the
// same signed relay leases itself, writes the repository-native issue, pull and
// discussion formats, commits them, and acknowledges only the rows it wrote.

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
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
	"time"
)

const collaborationInboxLimit = 8 << 20

// A read returns one bounded page, not the whole queue: the relay caps rows
// and bytes so a large backlog can neither exceed collaborationInboxLimit nor
// exhaust the Worker isolate building the response. Follow "more" so a backlog
// still clears in a single cycle, bounded so a page that never acknowledges
// anything cannot spin.
const collaborationDrainMaxPages = 64

var collaborationHTTPClient = &http.Client{Timeout: 20 * time.Second}

type collaborationInbox struct {
	Pending []json.RawMessage `json:"pending"`
	More    bool              `json:"more"`
}

// The direct intake decodes with DisallowUnknownFields, and that applies to
// this nested struct too: every key tools/mirror_gateway.py puts on an item
// (signedNumber, submittedAt) must be declared here, or each web submission a
// mirror accepts is refused as invalid_json (issue #700).
type collaborationItem struct {
	ID             int64            `json:"id"`
	Number         int              `json:"number"`
	Branch         string           `json:"branch"`
	SignedNumber   int              `json:"signedNumber"`
	TitleIfNew     string           `json:"titleIfNew"`
	Event          map[string]any   `json:"event"`
	Pull           map[string]any   `json:"pull"`
	Meta           map[string]any   `json:"meta"`
	AttachmentData []map[string]any `json:"attachmentData"`
	FediverseID    string           `json:"fediverseMentionId"`
	SubmittedAt    int64            `json:"submittedAt"`
}

type directCollaborationRequest struct {
	Owner string            `json:"owner"`
	Repo  string            `json:"repo"`
	Kind  string            `json:"kind"`
	Item  collaborationItem `json:"item"`
}

func isCollaborationKind(kind string) bool {
	return kind == "issues" || kind == "pulls" || kind == "discussions"
}

func allCollaborationKinds() map[string]bool {
	return map[string]bool{"issues": true, "pulls": true, "discussions": true}
}

func collaborationKinds(set map[string]bool) []string {
	ordered := make([]string, 0, 3)
	for _, kind := range []string{"issues", "pulls", "discussions"} {
		if set[kind] {
			ordered = append(ordered, kind)
		}
	}
	return ordered
}

// handleCollaboration is reachable only through the daemon's loopback-only
// control server. The public Python gateway verifies the Ed25519 event and
// portable manager grant before handing the normalized repository-native item
// here. Materialization is synchronous: a 201 means the Git commit is durable
// and immediately readable from this independent node.
func (d *Daemon) handleCollaboration(w http.ResponseWriter, request *http.Request) {
	if request.Method != http.MethodPost {
		w.Header().Set("Allow", http.MethodPost)
		writeJSON(w, http.StatusMethodNotAllowed,
			map[string]any{"ok": false, "error": "POST required"})
		return
	}
	input, err := decodeDirectCollaborationRequest(request.Body)
	if err != nil {
		writeJSON(w, http.StatusBadRequest,
			map[string]any{"ok": false, "error": "invalid_json"})
		return
	}
	if !nodePattern.MatchString(input.Owner) ||
		!repositoryPattern.MatchString(input.Repo) || !isCollaborationKind(input.Kind) {
		writeJSON(w, http.StatusNotFound,
			map[string]any{"ok": false, "error": "not_found"})
		return
	}
	repositoryIndex := -1
	for i := range d.gateway.Repositories {
		candidate := d.gateway.Repositories[i]
		if strings.EqualFold(candidate.Owner, input.Owner) &&
			strings.EqualFold(candidate.Name, input.Repo) {
			repositoryIndex = i
			break
		}
	}
	if repositoryIndex < 0 {
		writeJSON(w, http.StatusNotFound,
			map[string]any{"ok": false, "error": "not_found"})
		return
	}
	input.Item.ID = 1
	ctx, cancel := context.WithTimeout(request.Context(), 2*time.Minute)
	defer cancel()
	d.collaborationWriteMu.Lock()
	number, err := materializeDirectCollaboration(
		ctx, d.gateway.Repositories[repositoryIndex], input.Kind, input.Item,
		d.gateway.Node.Name)
	if err == nil {
		var digest string
		digest, err = refsSHA256(ctx, d.gateway.Repositories[repositoryIndex].GitDir)
		if err == nil {
			d.gateway.Repositories[repositoryIndex].Integrity.ExpectedRefsSHA256 = digest
			_, err = writeGatewayConfig(d.config.GatewayConfig, d.gateway)
		}
		if err == nil && !d.reloadGatewayGeneration(ctx) {
			err = errors.New("gateway did not adopt collaboration generation")
		}
	}
	d.collaborationWriteMu.Unlock()
	if errors.Is(err, errIssueUnknown) {
		w.Header().Set("Retry-After", "30")
		writeJSON(w, http.StatusServiceUnavailable, map[string]any{
			"ok": false, "error": "issue_not_synced", "retryable": true,
			"number": number, "servedBy": d.gateway.Node.Name,
		})
		return
	}
	if err != nil {
		writeJSON(w, http.StatusServiceUnavailable,
			map[string]any{"ok": false, "error": "materialization_failed"})
		return
	}
	d.markEndpointStateDirty()
	d.requestPublish()
	writeJSON(w, http.StatusCreated, map[string]any{
		"ok": true, "number": number, "durable": true,
		"controllerIndependent": true, "servedBy": d.gateway.Node.Name,
	})
}

func decodeDirectCollaborationRequest(body io.Reader) (directCollaborationRequest, error) {
	decoder := json.NewDecoder(io.LimitReader(body, collaborationInboxLimit+1))
	decoder.DisallowUnknownFields()
	var input directCollaborationRequest
	if err := decoder.Decode(&input); err != nil {
		return directCollaborationRequest{}, err
	}
	if decoder.Decode(&struct{}{}) != io.EOF {
		return directCollaborationRequest{}, errors.New("trailing data after collaboration request")
	}
	return input, nil
}

func materializeDirectCollaboration(ctx context.Context, repo Repository,
	kind string, item collaborationItem, node string) (int, error) {
	switch kind {
	case "issues":
		if len(item.Event) == 0 {
			return 0, errors.New("issue event is required")
		}
		branch := gitValue(ctx, repo.GitDir, "symbolic-ref", "--short", "HEAD")
		if branch == "" {
			branch = "HEAD"
		}
		dir, cleanup, err := collaborationWorktree(ctx, repo, branch, false, node)
		if err != nil {
			return 0, err
		}
		defer cleanup()
		wrote, number, err := writeIssueItem(dir, item)
		if err != nil || !wrote {
			if err == nil {
				err = errors.New("issue event could not be materialized")
			}
			return number, err
		}
		return number, commitCollaboration(ctx, dir, ".forkmesh/issues",
			fmt.Sprintf("issue #%d: direct mirror intake", number))
	case "pulls":
		if len(item.Event) == 0 && len(item.Pull) == 0 {
			return 0, errors.New("pull or pull event is required")
		}
		branch := "forkmesh/pulls"
		create := gitRefValue(ctx, repo.GitDir, "refs/heads/"+branch) == ""
		dir, cleanup, err := collaborationWorktree(ctx, repo, branch, create, node)
		if err != nil {
			return 0, err
		}
		defer cleanup()
		if item.Branch != "" {
			wrote, err := writeBranchReviewItem(dir, item)
			if err == nil && !wrote {
				err = errors.New("branch review event could not be materialized")
			}
			if err != nil {
				return 0, err
			}
			return 0, commitCollaboration(ctx, dir, branchReviewsRoot,
				"branch "+safeHeader(item.Branch)+": direct mirror intake")
		}
		wrote, number, err := writePullItem(dir, item)
		if err != nil || !wrote {
			if err == nil {
				err = errors.New("pull event could not be materialized")
			}
			return number, err
		}
		if err := commitCollaboration(ctx, dir, ".forkmesh/pulls",
			fmt.Sprintf("pull #%d: direct mirror intake", number)); err != nil {
			return number, err
		}
		tip := gitValue(ctx, repo.GitDir, "rev-parse", "refs/heads/forkmesh/pulls")
		if tip != "" {
			name := firstNonEmpty(node, "ForkMesh mirror")
			isolated, old, err := materializePullMetadataCommit(
				ctx, repo.GitDir, number, tip, name,
				name+"@users.noreply.forkmesh.com")
			if err != nil {
				return number, err
			}
			if err := publishPullMetadataRefs(ctx, repo.GitDir, number, isolated, old); err != nil {
				return number, err
			}
		}
		return number, nil
	case "discussions":
		if len(item.Event) == 0 {
			return 0, errors.New("discussion event is required")
		}
		branch := gitValue(ctx, repo.GitDir, "symbolic-ref", "--short", "HEAD")
		if branch == "" {
			branch = "HEAD"
		}
		dir, cleanup, err := collaborationWorktree(ctx, repo, branch, false, node)
		if err != nil {
			return 0, err
		}
		defer cleanup()
		wrote, number, err := writeDiscussionItem(dir, item)
		if err != nil || !wrote {
			if err == nil {
				err = errors.New("discussion event could not be materialized")
			}
			return number, err
		}
		return number, commitCollaboration(ctx, dir, ".forkmesh/discussions",
			fmt.Sprintf("discussion #%d: direct mirror intake", number))
	default:
		return 0, errors.New("unsupported collaboration kind")
	}
}

func (d *Daemon) takeCollaborationRequests() (bool, map[string]map[string]bool) {
	return d.takeCollaborationRequestsAt(time.Now())
}

func (d *Daemon) takeCollaborationRequestsAt(now time.Time) (bool, map[string]map[string]bool) {
	d.mu.Lock()
	defer d.mu.Unlock()
	if !d.collaborationRetryAt.IsZero() && now.Before(d.collaborationRetryAt) {
		return false, map[string]map[string]bool{}
	}
	catchUp := d.collaborationCatchUp
	pending := d.collaborationPending
	d.collaborationCatchUp = false
	d.collaborationPending = map[string]map[string]bool{}
	return catchUp, pending
}

// finishCollaborationAttempt retains failed inboxes through requeueCollaboration
// while spacing their controller reads exponentially. A node-event frame clears
// this state in handleNodeEvent, so a recovered WebSocket remains the immediate
// path and the timer is only an outage/dropped-frame safety rail.
func (d *Daemon) finishCollaborationAttempt(failed bool, now time.Time) {
	d.mu.Lock()
	defer d.mu.Unlock()
	if !failed {
		d.collaborationRetryAt = time.Time{}
		d.collaborationRetryDelay = 0
		return
	}
	if d.collaborationRetryDelay <= 0 {
		d.collaborationRetryDelay = controllerRetryFloor
	} else {
		d.collaborationRetryDelay = min(
			d.collaborationRetryDelay*2, controllerRetryCeil)
	}
	d.collaborationRetryAt = now.Add(d.collaborationRetryDelay)
}

func (d *Daemon) requeueCollaboration(repo, kind string) {
	if !isCollaborationKind(kind) || strings.TrimSpace(repo) == "" {
		return
	}
	d.mu.Lock()
	defer d.mu.Unlock()
	if d.collaborationPending == nil {
		d.collaborationPending = map[string]map[string]bool{}
	}
	key := strings.ToLower(strings.TrimSpace(repo))
	if d.collaborationPending[key] == nil {
		d.collaborationPending[key] = map[string]bool{}
	}
	d.collaborationPending[key][kind] = true
}

// The gateway exposes the same physical bare repository through both the
// source owner and the node owner. Catch-up must drain it once, using the
// source alias when one exists, so rows land at the repository URL users see.
func (d *Daemon) preferredCollaborationAlias(repo Repository) bool {
	if !strings.EqualFold(repo.Owner, d.config.PublishOwner) {
		return true
	}
	for _, candidate := range d.gateway.Repositories {
		if candidate.GitDir == repo.GitDir &&
			!strings.EqualFold(candidate.Owner, d.config.PublishOwner) {
			return false
		}
	}
	return true
}

func (d *Daemon) drainCollaboration(ctx context.Context, repo Repository) error {
	if d.config.DisableCatalog || d.config.CatalogURL == "" {
		return nil
	}
	var failures []string
	for _, kind := range []string{"issues", "pulls", "discussions"} {
		if err := d.drainCollaborationKind(ctx, repo, kind); err != nil {
			failures = append(failures, kind+": "+err.Error())
		}
	}
	if len(failures) != 0 {
		return errors.New(strings.Join(failures, "; "))
	}
	return nil
}

func (d *Daemon) collaborationURL(repo Repository, kind string) (*url.URL, error) {
	base, err := url.Parse(d.config.CatalogURL)
	if err != nil || base.Host == "" || (base.Scheme != "https" && base.Scheme != "http") {
		return nil, errors.New("catalogUrl is not an absolute HTTP URL")
	}
	base.Path = "/api/repo/" + url.PathEscape(repo.Owner) + "/" +
		url.PathEscape(repo.Name) + "/" + kind
	base.RawPath, base.RawQuery, base.Fragment = "", "", ""
	return base, nil
}

func (d *Daemon) signedCollaborationQuery() url.Values {
	ts := strconv.FormatInt(time.Now().UnixMilli(), 10)
	canonical := "forkmesh-issues-pull-v1\n" + d.gateway.Node.Name + "\n" + ts
	query := url.Values{}
	query.Set("owner", d.gateway.Node.Name)
	query.Set("ts", ts)
	query.Set("sig", d.identity.Sign([]byte(canonical)))
	query.Set("mirror", "1")
	return query
}

func (d *Daemon) drainCollaborationKind(ctx context.Context, repo Repository, kind string) error {
	for page := 0; page < collaborationDrainMaxPages; page++ {
		more, acked, err := d.drainCollaborationPage(ctx, repo, kind)
		// Stopping on !acked is what keeps this from looping: rows we could
		// not materialize stay leased to this node and would be handed back
		// unchanged on every following read.
		if err != nil || !more || !acked {
			return err
		}
	}
	return nil
}

// drainCollaborationPage reads, materializes and acknowledges one page,
// reporting whether the relay still holds rows and whether this page made
// progress.
func (d *Daemon) drainCollaborationPage(ctx context.Context, repo Repository,
	kind string) (bool, bool, error) {
	endpoint, err := d.collaborationURL(repo, kind)
	if err != nil {
		return false, false, err
	}
	endpoint.RawQuery = d.signedCollaborationQuery().Encode()
	request, err := http.NewRequestWithContext(ctx, http.MethodGet, endpoint.String(), nil)
	if err != nil {
		return false, false, err
	}
	request.Header.Set("Accept", "application/json")
	request.Header.Set("User-Agent", "ForkMesh-Mirror-Node/"+d.config.Version)
	response, err := collaborationHTTPClient.Do(request)
	if err != nil {
		return false, false, err
	}
	defer response.Body.Close()
	raw, err := io.ReadAll(io.LimitReader(response.Body, collaborationInboxLimit+1))
	if err != nil {
		return false, false, err
	}
	if len(raw) > collaborationInboxLimit {
		return false, false, errors.New("inbox response is too large")
	}
	if response.StatusCode < 200 || response.StatusCode >= 300 {
		return false, false, fmt.Errorf("relay returned HTTP %d: %s",
			response.StatusCode, boundedText(raw, 240))
	}
	var inbox collaborationInbox
	if err := json.Unmarshal(raw, &inbox); err != nil {
		return false, false, errors.New("relay returned invalid inbox JSON")
	}
	if len(inbox.Pending) == 0 {
		return false, false, nil
	}

	var ids []int64
	switch kind {
	case "issues":
		ids, err = materializeIssueInbox(ctx, repo, inbox.Pending,
			d.gateway.Node.Name)
	case "pulls":
		ids, err = materializePullInbox(ctx, repo, inbox.Pending,
			d.gateway.Node.Name)
	case "discussions":
		ids, err = materializeDiscussionInbox(ctx, repo, inbox.Pending,
			d.gateway.Node.Name)
	default:
		return false, false, errors.New("unsupported collaboration kind")
	}
	if err != nil {
		return false, false, err
	}
	if len(ids) == 0 {
		return inbox.More, false, nil
	}
	if err := d.ackCollaboration(ctx, repo, kind, sortedUniqueIDs(ids)); err != nil {
		return false, false, err
	}
	return inbox.More, true, nil
}

func (d *Daemon) ackCollaboration(ctx context.Context, repo Repository,
	kind string, ids []int64) error {
	endpoint, err := d.collaborationURL(repo, kind)
	if err != nil {
		return err
	}
	query := d.signedCollaborationQuery()
	parts := make([]string, 0, len(ids))
	for _, id := range ids {
		if id > 0 {
			parts = append(parts, strconv.FormatInt(id, 10))
		}
	}
	if len(parts) == 0 {
		return nil
	}
	query.Set("ids", strings.Join(parts, ","))
	state, err := refsSHA256(ctx, repo.GitDir)
	if err != nil {
		return err
	}
	stateTS := strconv.FormatInt(time.Now().UnixMilli(), 10)
	canonical := strings.Join([]string{
		"forkmesh-repostate-v1", d.gateway.Node.Name, repo.Name, state, stateTS,
	}, "\n")
	query.Set("state", state)
	query.Set("stateTs", stateTS)
	query.Set("stateSig", d.identity.Sign([]byte(canonical)))
	endpoint.RawQuery = query.Encode()
	request, err := http.NewRequestWithContext(
		ctx, http.MethodDelete, endpoint.String(), nil)
	if err != nil {
		return err
	}
	request.Header.Set("Accept", "application/json")
	request.Header.Set("User-Agent", "ForkMesh-Mirror-Node/"+d.config.Version)
	response, err := collaborationHTTPClient.Do(request)
	if err != nil {
		return err
	}
	defer response.Body.Close()
	body, _ := io.ReadAll(io.LimitReader(response.Body, 64<<10))
	if response.StatusCode < 200 || response.StatusCode >= 300 {
		return fmt.Errorf("ack returned HTTP %d: %s", response.StatusCode,
			boundedText(body, 240))
	}
	return nil
}

func collaborationWorktree(ctx context.Context, repo Repository, branch string,
	create bool, node string) (string, func(), error) {
	dir, err := os.MkdirTemp("", "forkmesh-mirror-intake-*")
	if err != nil {
		return "", nil, err
	}
	_ = os.Remove(dir)
	args := []string{"--git-dir=" + repo.GitDir, "worktree", "add", "--force", "--no-checkout"}
	if create {
		head := gitValue(ctx, repo.GitDir, "symbolic-ref", "--short", "HEAD")
		if head == "" {
			head = "HEAD"
		}
		args = append(args, "-b", branch, dir, head)
	} else {
		args = append(args, dir, branch)
	}
	if output, err := exec.CommandContext(ctx, "git", args...).CombinedOutput(); err != nil {
		_ = removeOwnWorktreeUnder(filepath.Dir(dir), dir)
		return "", nil, fmt.Errorf("create worktree: %s", boundedText(output, 240))
	}
	cleanup := func() {
		_ = exec.Command("git", "--git-dir="+repo.GitDir, "worktree", "remove",
			"--force", dir).Run()
		_ = exec.Command("git", "--git-dir="+repo.GitDir, "worktree", "prune").Run()
		_ = removeOwnWorktreeUnder(filepath.Dir(dir), dir)
	}
	if err := sparseCollaborationCheckout(ctx, dir); err != nil {
		cleanup()
		return "", nil, err
	}
	_ = exec.CommandContext(ctx, "git", "-C", dir, "config", "user.name", node).Run()
	_ = exec.CommandContext(ctx, "git", "-C", dir, "config", "user.email",
		node+"@users.noreply.forkmesh.com").Run()
	return dir, cleanup, nil
}

func sparseCollaborationCheckout(ctx context.Context, dir string) error {
	output, err := exec.CommandContext(ctx, "git", "-C", dir, "rev-parse",
		"--path-format=absolute", "--git-path", "info/sparse-checkout").Output()
	if err != nil {
		return fmt.Errorf("sparse checkout path: %v", err)
	}
	patterns := strings.TrimSpace(string(output))
	if err := os.MkdirAll(filepath.Dir(patterns), 0700); err != nil {
		return err
	}
	if err := os.WriteFile(patterns, []byte("/.forkmesh/\n"), 0600); err != nil {
		return err
	}
	if output, err := exec.CommandContext(ctx, "git", "-C", dir, "-c", "core.sparseCheckout=true",
		"read-tree", "-mu", "HEAD").CombinedOutput(); err != nil {
		return fmt.Errorf("checkout .forkmesh: %s", boundedText(output, 240))
	}
	return refuseCollaborationLinks(filepath.Join(dir, ".forkmesh"))
}

func refuseCollaborationLinks(root string) error {
	err := filepath.WalkDir(root, func(path string, entry fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if !entry.IsDir() && !entry.Type().IsRegular() {
			return fmt.Errorf("checkout .forkmesh: %s is not a plain file or folder", path)
		}
		return nil
	})
	if errors.Is(err, fs.ErrNotExist) {
		return nil
	}
	return err
}

func commitCollaboration(ctx context.Context, dir, path, message string) error {
	if output, err := exec.CommandContext(ctx, "git", "-C", dir, "add", "-A", "--", path).CombinedOutput(); err != nil {
		return fmt.Errorf("git add: %s", boundedText(output, 240))
	}
	command := exec.CommandContext(ctx, "git", "-C", dir, "commit", "-m", message, "--", path)
	if output, err := command.CombinedOutput(); err != nil {
		status := exec.CommandContext(ctx, "git", "-C", dir, "diff", "--cached", "--quiet", "--", path).Run()
		if status == nil {
			return nil
		}
		return fmt.Errorf("git commit: %s", boundedText(output, 240))
	}
	return nil
}

func decodeCollaborationItem(raw json.RawMessage) (collaborationItem, error) {
	var item collaborationItem
	decoder := json.NewDecoder(bytes.NewReader(raw))
	decoder.UseNumber()
	if err := decoder.Decode(&item); err != nil {
		return item, err
	}
	return item, nil
}

func materializeIssueInbox(ctx context.Context, repo Repository,
	rows []json.RawMessage, node string) ([]int64, error) {
	branch := gitValue(ctx, repo.GitDir, "symbolic-ref", "--short", "HEAD")
	if branch == "" {
		branch = "HEAD"
	}
	dir, cleanup, err := collaborationWorktree(ctx, repo, branch, false, node)
	if err != nil {
		return nil, err
	}
	defer cleanup()
	var ids []int64
	for _, raw := range rows {
		item, err := decodeCollaborationItem(raw)
		if err != nil || item.ID <= 0 || len(item.Event) == 0 {
			continue
		}
		wrote, number, err := writeIssueItem(dir, item)
		if errors.Is(err, errIssueUnknown) {
			continue
		}
		if err != nil {
			return ids, err
		}
		if !wrote {
			continue
		}
		message := fmt.Sprintf("issue #%d: mirror intake", number)
		if stringValue(item.Event["type"]) == "delete" &&
			stringValue(item.Event["target"]) == "self" {
			message = fmt.Sprintf("issue #%d: delete (mirror intake)", number)
		}
		if err := commitCollaboration(ctx, dir, ".forkmesh/issues",
			message); err != nil {
			return ids, err
		}
		// Agent requests and fediverse confirmation require owner-only side
		// effects. Materialize them for reads, but leave their row for the owner.
		wantsAgent, _ := item.Meta["wantsAgent"].(bool)
		if !wantsAgent && item.FediverseID == "" {
			ids = append(ids, item.ID)
		}
	}
	return ids, nil
}

func issueFiles(root string) ([]string, error) {
	var files []string
	base := filepath.Join(root, ".forkmesh", "issues")
	err := filepath.WalkDir(base, func(path string, entry os.DirEntry, err error) error {
		if errors.Is(err, os.ErrNotExist) {
			return nil
		}
		if err != nil {
			return err
		}
		if !entry.IsDir() && strings.HasPrefix(entry.Name(), "issue-") &&
			strings.HasSuffix(entry.Name(), ".json") {
			files = append(files, path)
		}
		return nil
	})
	return files, err
}

func issueNumber(value any) int {
	switch number := value.(type) {
	case json.Number:
		parsed, _ := strconv.Atoi(number.String())
		return parsed
	case float64:
		return int(number)
	case int:
		return number
	}
	return 0
}

func stringValue(value any) string {
	text, _ := value.(string)
	return text
}

var issueRewardIDPattern = regexp.MustCompile(`^reward-[a-z0-9-]{8,88}$`)

// copyIssueReportReward carries the reward estimate the relay attached to a
// Discord bug report onto the new issue record. The desktop drain always did
// this (IssueStore); without it a mirror-filed report reached the ledger with
// no reward at all. It is the same narrow shape the relay validates: an
// accepted-report estimate in the published 0.001-0.005 SOL range, still
// awaiting the wallet of the reporter it credits, and nobody else.
func copyIssueReportReward(issue map[string]any, meta map[string]any) {
	rewardID := strings.ToLower(strings.TrimSpace(stringValue(meta["rewardId"])))
	recipient := strings.ToLower(strings.TrimSpace(stringValue(meta["rewardRecipient"])))
	reporter := strings.ToLower(strings.TrimSpace(stringValue(meta["reporter"])))
	amount := issueNumber(meta["rewardEstimateLamports"])
	if !issueRewardIDPattern.MatchString(rewardID) || recipient == "" ||
		recipient != reporter || amount < 1_000_000 || amount > 5_000_000 ||
		stringValue(meta["rewardCondition"]) != "accepted_report" ||
		stringValue(meta["rewardStatus"]) != "pending_wallet" {
		return
	}
	issue["rewardId"] = rewardID
	issue["rewardRecipient"] = recipient
	issue["rewardEstimateLamports"] = amount
	issue["rewardCondition"] = "accepted_report"
	issue["rewardStatus"] = "pending_wallet"
}

var errIssueUnknown = errors.New("issue is not materialized on this node")

var pullEventTypePattern = regexp.MustCompile(`^[A-Za-z0-9][A-Za-z0-9_-]{0,63}$`)

func writeIssueItem(root string, item collaborationItem) (bool, int, error) {
	files, err := issueFiles(root)
	if err != nil {
		return false, 0, err
	}
	eventType := stringValue(item.Event["type"])
	eventID := stringValue(item.Event["id"])
	eventSig := stringValue(item.Event["sig"])
	eventAuthor := stringValue(item.Event["author"])
	eventTitle := stringValue(item.Event["title"])
	eventTS := issueNumber(item.Event["ts"])
	number := item.Number
	maxNumber := 0
	var existingPath string
	var issue map[string]any
	for _, path := range files {
		raw, readErr := os.ReadFile(path)
		if readErr != nil {
			continue
		}
		var candidate map[string]any
		decoder := json.NewDecoder(bytes.NewReader(raw))
		decoder.UseNumber()
		if decoder.Decode(&candidate) != nil {
			continue
		}
		candidateNumber := issueNumber(candidate["number"])
		if candidateNumber > maxNumber {
			maxNumber = candidateNumber
		}
		events, _ := candidate["events"].([]any)
		for _, value := range events {
			existing, _ := value.(map[string]any)
			if eventType == "open" && ((eventSig != "" && stringValue(existing["sig"]) == eventSig) ||
				(eventSig == "" && stringValue(existing["author"]) == eventAuthor &&
					issueNumber(existing["ts"]) == eventTS && stringValue(existing["title"]) == eventTitle)) {
				return true, candidateNumber, nil
			}
			if eventType != "open" && eventID != "" && stringValue(existing["id"]) == eventID {
				return true, candidateNumber, nil
			}
		}
		if candidateNumber == number {
			existingPath, issue = path, candidate
		}
	}
	// The web app's "Delete issue" (issue #582) arrives as a signed delete/self
	// event the relay only accepts from a verified repository manager, so it is
	// the owner's authoritative removal rather than a creator tombstone. Drop
	// the issue folder instead of appending the event: every reader then agrees
	// the issue is gone, and git history keeps the record.
	if eventType == "delete" && stringValue(item.Event["target"]) == "self" {
		if number <= 0 {
			return false, 0, nil
		}
		if existingPath == "" {
			return true, number, nil // already removed; acknowledge the row
		}
		if err := removeTreeUnder(filepath.Join(root, ".forkmesh", "issues"), filepath.Dir(existingPath)); err != nil {
			return false, number, err
		}
		return true, number, nil
	}
	if eventType == "open" {
		var placeholder map[string]any
		if number > 0 && issue != nil && issueOpeningIsPlaceholder(issue) {
			placeholder = issue
		} else {
			if number <= 0 || issue != nil {
				number = maxNumber + 1
			}
			existingPath = ""
		}
		if number <= 0 {
			number = 1
		}
		labels, _ := item.Meta["labels"].([]any)
		assignees, _ := item.Meta["assignees"].([]any)
		reporter := stringValue(item.Meta["reporter"])
		authorName := stringValue(item.Event["authorName"])
		if reporter != "" {
			authorName = reporter
		}
		issue = map[string]any{
			"schema": "forkmesh-issue-v1", "number": number,
			"title": firstNonEmpty(eventTitle, item.TitleIfNew), "status": "open",
			"labels": labels, "milestone": stringValue(item.Meta["milestone"]),
			"startDate": 0, "endDate": 0, "priority": issueNumber(item.Meta["priority"]),
			"progress": 0, "assignees": assignees, "createdAt": eventTS,
			"updatedAt": eventTS, "author": eventAuthor, "authorName": authorName,
			"bountyUsd": 0, "bountyAddress": "", "bountyStatus": "",
			"votes": 0, "events": []any{item.Event},
		}
		copyIssueReportReward(issue, item.Meta)
		if placeholder != nil {
			foldIssueEvents(issue, placeholder)
		}
	} else {
		if number <= 0 {
			return false, 0, nil
		}
		if issue == nil {
			return false, number, errIssueUnknown
		}
		events, _ := issue["events"].([]any)
		issue["events"] = append(events, item.Event)
		applyIssueProjection(issue, item.Event)
	}
	issue["updatedAt"] = eventTS
	status := stringValue(issue["status"])
	if status != "closed" {
		status = "open"
	}
	destination := filepath.Join(root, ".forkmesh", "issues", status,
		strconv.Itoa(number), fmt.Sprintf("issue-%d.json", number))
	if existingPath != "" && existingPath != destination {
		if err := moveIssueDirectory(filepath.Dir(existingPath), filepath.Dir(destination)); err != nil {
			return false, number, err
		}
	}
	if err := os.MkdirAll(filepath.Dir(destination), 0700); err != nil {
		return false, number, err
	}
	if err := writeJSONFile(destination, issue); err != nil {
		return false, number, err
	}
	if err := writeIssueAttachments(filepath.Dir(destination), item); err != nil {
		return false, number, err
	}
	return true, number, nil
}

func moveIssueDirectory(from, to string) error {
	if err := os.MkdirAll(filepath.Dir(to), 0700); err != nil {
		return err
	}
	if _, err := os.Lstat(to); errors.Is(err, os.ErrNotExist) {
		return os.Rename(from, to)
	}
	entries, err := os.ReadDir(from)
	if err != nil {
		return err
	}
	now := time.Now()
	for _, entry := range entries {
		source := filepath.Join(from, entry.Name())
		target := filepath.Join(to, entry.Name())
		if _, err := os.Lstat(target); err == nil {
			target = asideName(to, entry.Name(), "conflict", now)
			log.Printf("issue move: %s already has %s; kept %s as %s", to, entry.Name(), source, target)
		}
		if err := os.Rename(source, target); err != nil {
			return err
		}
	}
	return os.Remove(from)
}

func issueOpeningIsPlaceholder(record map[string]any) bool {
	events, _ := record["events"].([]any)
	if len(events) == 0 {
		return false
	}
	opening, _ := events[0].(map[string]any)
	if opening == nil || stringValue(opening["type"]) != "open" {
		return false
	}
	signature := stringValue(opening["sig"])
	return signature == "relay-placeholder" ||
		(signature == "" && strings.TrimSpace(stringValue(opening["title"])) == "")
}

func issueEventKey(event map[string]any) string {
	if id := stringValue(event["id"]); id != "" {
		return "id:" + id
	}
	if signature := stringValue(event["sig"]); signature != "" {
		return "sig:" + signature
	}
	return ""
}

func foldIssueEvents(target, source map[string]any) bool {
	events, _ := target["events"].([]any)
	seen := map[string]bool{}
	latest := map[string]int64{}
	for _, value := range events {
		event, _ := value.(map[string]any)
		if event == nil {
			continue
		}
		if key := issueEventKey(event); key != "" {
			seen[key] = true
		}
		kind := stringValue(event["type"])
		if at := issueInt64(event["ts"]); at > latest[kind] {
			latest[kind] = at
		}
	}
	var incoming []map[string]any
	sourceEvents, _ := source["events"].([]any)
	for _, value := range sourceEvents {
		event, _ := value.(map[string]any)
		if event == nil || stringValue(event["type"]) == "open" {
			continue
		}
		key := issueEventKey(event)
		if key == "" || seen[key] {
			continue
		}
		seen[key] = true
		incoming = append(incoming, event)
	}
	if len(incoming) == 0 {
		return false
	}
	sort.SliceStable(incoming, func(i, j int) bool {
		return issueInt64(incoming[i]["ts"]) < issueInt64(incoming[j]["ts"])
	})
	updatedAt := issueInt64(target["updatedAt"])
	for _, event := range incoming {
		kind := stringValue(event["type"])
		at := issueInt64(event["ts"])
		if kind == "claim" || kind == "vote" || at >= latest[kind] {
			applyIssueProjection(target, event)
		}
		if at > latest[kind] {
			latest[kind] = at
		}
		if at > updatedAt {
			updatedAt = at
			target["updatedAt"] = event["ts"]
		}
		events = append(events, event)
	}
	target["events"] = events
	return true
}

func applyIssueProjection(issue, event map[string]any) {
	switch stringValue(event["type"]) {
	case "title":
		issue["title"] = stringValue(event["title"])
	case "status":
		issue["status"] = stringValue(event["status"])
	case "labels":
		issue["labels"] = event["labels"]
	case "milestone":
		issue["milestone"] = stringValue(event["milestone"])
	case "dates":
		issue["startDate"], issue["endDate"] = event["startDate"], event["endDate"]
	case "priority":
		issue["priority"] = event["priority"]
	case "progress":
		issue["progress"] = event["progress"]
	case "assignees":
		issue["assignees"] = event["assignees"]
	case "claim":
		issue["assignees"] = applyIssueClaim(issue["assignees"], event)
	case "vote":
		issue["votes"] = issueNumber(issue["votes"]) + 1
	}
}

// A "claim" is the one assignment a public contributor may sign (issue #570),
// so it names a single account and moves only that name. Folding it as a delta
// — rather than replacing the list the way an "assignees" event does — is what
// keeps one visitor's self-assignment from dropping everybody else's, and lets
// two claims arriving out of order converge. The stored spelling is whatever
// was signed; matching is case-insensitive, like the relay's account check.
func applyIssueClaim(current any, event map[string]any) []any {
	name := strings.TrimSpace(stringValue(event["assignee"]))
	existing, _ := current.([]any)
	if name == "" {
		return existing
	}
	kept := make([]any, 0, len(existing)+1)
	found := false
	for _, value := range existing {
		if strings.EqualFold(strings.TrimSpace(stringValue(value)), name) {
			found = true
			if stringValue(event["claimState"]) == "released" {
				continue
			}
		}
		kept = append(kept, value)
	}
	if !found && stringValue(event["claimState"]) == "assigned" {
		kept = append(kept, name)
	}
	return kept
}

func writeIssueAttachments(dir string, item collaborationItem) error {
	for _, attachment := range item.AttachmentData {
		name := filepath.Base(stringValue(attachment["name"]))
		if name == "." || name == "" || name != stringValue(attachment["name"]) {
			continue
		}
		data, err := decodeBase64(stringValue(attachment["data"]))
		if err != nil || len(data) > 4<<20 {
			continue
		}
		if err := os.WriteFile(filepath.Join(dir, name), data, 0600); err != nil {
			return err
		}
	}
	return nil
}

func decodeBase64(value string) ([]byte, error) {
	for _, encoding := range []*base64.Encoding{
		base64.StdEncoding, base64.RawStdEncoding, base64.RawURLEncoding,
	} {
		if data, err := encoding.DecodeString(value); err == nil {
			return data, nil
		}
	}
	return nil, errors.New("invalid base64")
}

func materializePullInbox(ctx context.Context, repo Repository,
	rows []json.RawMessage, node string) ([]int64, error) {
	branch := "forkmesh/pulls"
	create := gitRefValue(ctx, repo.GitDir, "refs/heads/"+branch) == ""
	dir, cleanup, err := collaborationWorktree(ctx, repo, branch, create, node)
	if err != nil {
		return nil, err
	}
	defer cleanup()
	var ids []int64
	reserved := reservedPullNumbers(ctx, repo.GitDir)
	for _, raw := range rows {
		item, err := decodeCollaborationItem(raw)
		if err != nil || item.ID <= 0 {
			continue
		}
		if item.Branch != "" {
			wrote, err := writeBranchReviewItem(dir, item)
			if err != nil {
				return ids, err
			}
			if !wrote {
				continue
			}
			if err := commitCollaboration(ctx, dir, branchReviewsRoot,
				"branch "+safeHeader(item.Branch)+": mirror intake"); err != nil {
				return ids, err
			}
			ids = append(ids, item.ID)
			continue
		}
		if len(item.Pull) != 0 && (item.Number <= 0 || reserved[item.Number]) {
			candidate := 1
			for number := range reserved {
				if number >= candidate {
					candidate = number + 1
				}
			}
			item.Number = candidate
		}
		wrote, number, err := writePullItem(dir, item)
		if err != nil {
			return ids, err
		}
		if !wrote {
			continue
		}
		reserved[number] = true
		if err := commitCollaboration(ctx, dir, ".forkmesh/pulls",
			fmt.Sprintf("pull #%d: mirror intake", number)); err != nil {
			return ids, err
		}
		if tip := gitValue(ctx, repo.GitDir, "rev-parse", "refs/heads/forkmesh/pulls"); tip != "" {
			name := node
			if name == "" {
				name = "ForkMesh mirror"
			}
			isolated, old, materializeErr := materializePullMetadataCommit(
				ctx, repo.GitDir, number, tip, name,
				name+"@users.noreply.forkmesh.com")
			if materializeErr != nil {
				return ids, materializeErr
			}
			if err := publishPullMetadataRefs(ctx, repo.GitDir, number, isolated, old); err != nil {
				return ids, err
			}
		}
		ids = append(ids, item.ID)
	}
	return ids, nil
}

func reservedPullNumbers(ctx context.Context, gitDir string) map[int]bool {
	reserved := map[int]bool{}
	const branchPrefix = "refs/heads/forkmesh/pull/"
	for _, ref := range gitLines(ctx, gitDir, "for-each-ref", "--format=%(refname)", branchPrefix) {
		if number, err := strconv.Atoi(strings.TrimPrefix(ref, branchPrefix)); err == nil && number > 0 {
			reserved[number] = true
		}
	}
	const compatibilityPrefix = "refs/pr/"
	for _, ref := range gitLines(ctx, gitDir, "for-each-ref", "--format=%(refname)", compatibilityPrefix) {
		segment, rest, ok := strings.Cut(strings.TrimPrefix(ref, compatibilityPrefix), "/")
		if number, err := strconv.Atoi(segment); ok && rest == "metadata" && err == nil && number > 0 {
			reserved[number] = true
		}
	}
	for _, ref := range []string{"refs/heads/forkmesh/pulls", "HEAD"} {
		for _, path := range gitLines(ctx, gitDir, "ls-tree", "-r", "--name-only", ref, "--", ".forkmesh/pulls") {
			rest := strings.TrimPrefix(path, ".forkmesh/pulls/")
			segment := strings.SplitN(rest, "/", 2)[0]
			if number, err := strconv.Atoi(segment); err == nil && number > 0 {
				reserved[number] = true
			}
		}
	}
	return reserved
}

const branchReviewsRoot = ".forkmesh/branch-reviews"

var branchReviewEventTypes = map[string]bool{
	"thread-comment": true,
	"thread-reply":   true,
	"thread-state":   true,
}

func branchReviewKey(branch string) string {
	sum := sha256.Sum256([]byte(branch))
	return hex.EncodeToString(sum[:])[:24]
}

func validReviewBranchName(branch string) bool {
	if branch == "" || len(branch) > 255 || strings.HasPrefix(branch, "-") ||
		strings.HasPrefix(branch, "/") || strings.HasSuffix(branch, "/") ||
		strings.HasSuffix(branch, ".lock") || strings.Contains(branch, "..") ||
		strings.Contains(branch, "//") || strings.Contains(branch, "@{") {
		return false
	}
	return !strings.ContainsAny(branch, "\x00\n\r\t ~^:?*[\\\x7f")
}

func writeBranchReviewItem(root string, item collaborationItem) (bool, error) {
	typeName := safeHeader(stringValue(item.Event["type"]))
	if !validReviewBranchName(item.Branch) || !branchReviewEventTypes[typeName] {
		return false, nil
	}
	dir := filepath.Join(root, ".forkmesh", "branch-reviews", branchReviewKey(item.Branch))
	if err := os.MkdirAll(dir, 0700); err != nil {
		return false, err
	}
	marker := filepath.Join(dir, "branch.md")
	if !fileExists(marker) {
		if err := os.WriteFile(marker, []byte("---\nbranch: "+item.Branch+"\n---\n"), 0600); err != nil {
			return false, err
		}
	}
	id := stringValue(item.Event["id"])
	entries, _ := os.ReadDir(dir)
	index := 2
	for _, entry := range entries {
		if entry.IsDir() || entry.Name() == "branch.md" || !strings.HasSuffix(entry.Name(), ".md") {
			continue
		}
		if value, err := strconv.Atoi(strings.SplitN(entry.Name(), "-", 2)[0]); err == nil && value >= index {
			index = value + 1
		}
		body, _ := os.ReadFile(filepath.Join(dir, entry.Name()))
		if id != "" && strings.Contains(string(body), "\nid: "+id+"\n") {
			return true, nil
		}
	}
	path := filepath.Join(dir, fmt.Sprintf("%04d-%s.md", index, typeName))
	return true, os.WriteFile(path, []byte(pullEventMarkdown(item.Event)), 0600)
}

func writePullItem(root string, item collaborationItem) (bool, int, error) {
	base := filepath.Join(root, ".forkmesh", "pulls")
	if err := os.MkdirAll(base, 0700); err != nil {
		return false, 0, err
	}
	if len(item.Event) != 0 {
		number := item.Number
		pullDir := filepath.Join(base, strconv.Itoa(number))
		if number <= 0 || !fileExists(filepath.Join(pullDir, "pull.md")) {
			return false, number, nil
		}
		id := stringValue(item.Event["id"])
		entries, _ := os.ReadDir(pullDir)
		for _, entry := range entries {
			if entry.IsDir() || !strings.HasSuffix(entry.Name(), ".md") || entry.Name() == "pull.md" {
				continue
			}
			body, _ := os.ReadFile(filepath.Join(pullDir, entry.Name()))
			if id != "" && strings.Contains(string(body), "\nid: "+id+"\n") {
				return true, number, nil
			}
		}
		index := 2
		for _, entry := range entries {
			if value, err := strconv.Atoi(strings.SplitN(entry.Name(), "-", 2)[0]); err == nil && value >= index {
				index = value + 1
			}
		}
		typeName := safeHeader(stringValue(item.Event["type"]))
		if !pullEventTypePattern.MatchString(typeName) {
			return false, number, nil
		}
		path := filepath.Join(pullDir, fmt.Sprintf("%04d-%s.md", index, typeName))
		if err := os.WriteFile(path, []byte(pullEventMarkdown(item.Event)), 0600); err != nil {
			return false, number, err
		}
		return true, number, applyPullLifecycleEvent(pullDir, item.Event)
	}
	if len(item.Pull) == 0 {
		return false, 0, nil
	}
	number := item.Number
	maxNumber := 0
	entries, _ := os.ReadDir(base)
	for _, entry := range entries {
		value, err := strconv.Atoi(entry.Name())
		if err != nil || !entry.IsDir() {
			continue
		}
		if value > maxNumber {
			maxNumber = value
		}
		existing, _ := os.ReadFile(filepath.Join(base, entry.Name(), "pull.md"))
		sig := stringValue(item.Pull["sig"])
		if sig != "" && strings.Contains(string(existing), "\nsig: "+sig+"\n") {
			return true, value, nil
		}
	}
	if number <= 0 || fileExists(filepath.Join(base, strconv.Itoa(number), "pull.md")) {
		number = maxNumber + 1
	}
	pullDir := filepath.Join(base, strconv.Itoa(number))
	if err := os.MkdirAll(pullDir, 0700); err != nil {
		return false, number, err
	}
	markdown := pullMarkdown(number, item.Pull)
	if err := os.WriteFile(filepath.Join(pullDir, "pull.md"), []byte(markdown), 0600); err != nil {
		return false, number, err
	}
	if patch := stringValue(item.Pull["patch"]); patch != "" {
		if err := os.WriteFile(filepath.Join(pullDir, "changes.patch"), []byte(patch), 0600); err != nil {
			return false, number, err
		}
	}
	if commits := stringValue(item.Pull["commits"]); commits != "" {
		if err := os.WriteFile(filepath.Join(pullDir, "commits.mbox"), []byte(commits), 0600); err != nil {
			return false, number, err
		}
	}
	return true, number, nil
}

var pullLifecycleStatus = map[string]string{"status": "closed", "delete": "deleted"}

func applyPullLifecycleEvent(pullDir string, event map[string]any) error {
	typeName := stringValue(event["type"])
	status, ok := pullLifecycleStatus[typeName]
	if !ok || (typeName == "status" && stringValue(event["state"]) != "closed") {
		return nil
	}
	recordPath := filepath.Join(pullDir, "pull.md")
	raw, err := os.ReadFile(recordPath)
	if err != nil {
		return err
	}
	updated, changed := setPullRecordStatus(string(raw), status)
	if !changed {
		return nil
	}
	if err := os.WriteFile(recordPath, []byte(updated), 0600); err != nil {
		return err
	}
	if status != "deleted" {
		return nil
	}
	for _, name := range []string{"changes.patch", "commits.mbox"} {
		if err := os.Remove(filepath.Join(pullDir, name)); err != nil && !errors.Is(err, os.ErrNotExist) {
			return err
		}
	}
	return nil
}

func setPullRecordStatus(record, status string) (string, bool) {
	lines := strings.Split(record, "\n")
	if len(lines) == 0 || strings.TrimSpace(lines[0]) != "---" {
		return record, false
	}
	for i := 1; i < len(lines); i++ {
		if strings.TrimSpace(lines[i]) == "---" {
			break
		}
		key, value, found := strings.Cut(lines[i], ":")
		if !found || strings.TrimSpace(key) != "status" {
			continue
		}
		switch strings.ToLower(strings.TrimSpace(value)) {
		case "merged", "closed-merged", "deleted", status:
			return record, false
		}
		lines[i] = "status: " + status
		return strings.Join(lines, "\n"), true
	}
	return record, false
}

func pullMarkdown(number int, pull map[string]any) string {
	lines := []string{
		"---", "schema: forkmesh-pull-v1", fmt.Sprintf("number: %d", number),
		"title: " + safeHeader(stringValue(pull["title"])),
		"base: " + safeHeader(stringValue(pull["base"])),
		"head: " + safeHeader(stringValue(pull["head"])), "status: open",
		"ts: " + strconv.Itoa(issueNumber(pull["ts"])),
		"author: " + safeHeader(stringValue(pull["author"])),
		"authorName: " + safeHeader(stringValue(pull["authorName"])),
		"sig: " + safeHeader(stringValue(pull["sig"])),
	}
	for _, key := range []string{"creationBaseOid", "creationHeadOid", "client", "clientSig"} {
		if value := safeHeader(stringValue(pull[key])); value != "" {
			lines = append(lines, key+": "+value)
		}
	}
	lines = append(lines, "---", "", stringValue(pull["description"]), "")
	return strings.Join(lines, "\n")
}

func pullEventMarkdown(event map[string]any) string {
	lines := []string{"---"}
	for _, key := range []string{"type", "id", "author", "authorName", "ts", "state", "path", "side", "line", "threadId", "parentId", "lineStart", "lineEnd", "targetPath", "appliedCommit", "patchset", "sig"} {
		if value, ok := event[key]; ok && fmt.Sprint(value) != "" {
			lines = append(lines, key+": "+safeHeader(fmt.Sprint(value)))
		}
	}
	if patch := stringValue(event["suggestionPatch"]); patch != "" {
		lines = append(lines, "suggestionPatchB64: "+base64.RawURLEncoding.EncodeToString([]byte(patch)))
	}
	// The commented text is multi-line; it rides in the same blob encoding
	// the desktop's PullStore::writeEventFile uses for it.
	if anchor := stringValue(event["anchorLines"]); anchor != "" {
		lines = append(lines, "anchorLinesB64: "+base64.RawURLEncoding.EncodeToString([]byte(anchor)))
	}
	lines = append(lines, "---", "", stringValue(event["body"]), "")
	return strings.Join(lines, "\n")
}

func materializeDiscussionInbox(ctx context.Context, repo Repository,
	rows []json.RawMessage, node string) ([]int64, error) {
	branch := gitValue(ctx, repo.GitDir, "symbolic-ref", "--short", "HEAD")
	if branch == "" {
		branch = "HEAD"
	}
	dir, cleanup, err := collaborationWorktree(ctx, repo, branch, false, node)
	if err != nil {
		return nil, err
	}
	defer cleanup()
	var ids []int64
	for _, raw := range rows {
		item, err := decodeCollaborationItem(raw)
		if err != nil || item.ID <= 0 || len(item.Event) == 0 {
			continue
		}
		wrote, number, err := writeDiscussionItem(dir, item)
		if err != nil {
			return ids, err
		}
		if !wrote {
			continue
		}
		if err := commitCollaboration(ctx, dir, ".forkmesh/discussions",
			fmt.Sprintf("discussion #%d: mirror intake", number)); err != nil {
			return ids, err
		}
		ids = append(ids, item.ID)
	}
	return ids, nil
}

func writeDiscussionItem(root string, item collaborationItem) (bool, int, error) {
	base := filepath.Join(root, ".forkmesh", "discussions")
	if err := os.MkdirAll(base, 0700); err != nil {
		return false, 0, err
	}
	number := item.Number
	typeName := stringValue(item.Event["type"])
	maxNumber := 0
	entries, _ := os.ReadDir(base)
	for _, entry := range entries {
		value, err := strconv.Atoi(entry.Name())
		if err == nil && entry.IsDir() && value > maxNumber {
			maxNumber = value
		}
	}
	if typeName == "open" {
		if number <= 0 || fileExists(filepath.Join(base, strconv.Itoa(number), "discussion.md")) {
			number = maxNumber + 1
		}
		dir := filepath.Join(base, strconv.Itoa(number))
		if err := os.MkdirAll(dir, 0700); err != nil {
			return false, number, err
		}
		lines := []string{
			"---", "schema: forkmesh-discussion-v1", fmt.Sprintf("number: %d", number),
			"title: " + safeHeader(firstNonEmpty(stringValue(item.Event["title"]), item.TitleIfNew)),
			"category: " + safeHeader(firstNonEmpty(stringValue(item.Event["category"]), "Ideas")),
			"status: open", "createdAt: " + strconv.Itoa(issueNumber(item.Event["ts"])),
			"author: " + safeHeader(stringValue(item.Event["author"])),
			"authorName: " + safeHeader(stringValue(item.Event["authorName"])),
			"type: open", "id: " + safeHeader(stringValue(item.Event["id"])),
			"ts: " + strconv.Itoa(issueNumber(item.Event["ts"])),
			"sig: " + safeHeader(stringValue(item.Event["sig"])), "---", "",
			stringValue(item.Event["body"]), "",
		}
		return true, number, os.WriteFile(filepath.Join(dir, "discussion.md"),
			[]byte(strings.Join(lines, "\n")), 0600)
	}
	if typeName == "answer" && number > 0 {
		return writeDiscussionAnswer(filepath.Join(base, strconv.Itoa(number)), item)
	}
	if typeName != "comment" || number <= 0 {
		return false, number, nil
	}
	dir := filepath.Join(base, strconv.Itoa(number))
	if !fileExists(filepath.Join(dir, "discussion.md")) {
		return false, number, nil
	}
	entries, _ = os.ReadDir(dir)
	index := 2
	for _, entry := range entries {
		if value, err := strconv.Atoi(strings.SplitN(entry.Name(), "-", 2)[0]); err == nil && value >= index {
			index = value + 1
		}
	}
	lines := []string{
		"---", "type: comment", "id: " + safeHeader(stringValue(item.Event["id"])),
		"author: " + safeHeader(stringValue(item.Event["author"])),
		"authorName: " + safeHeader(stringValue(item.Event["authorName"])),
		"ts: " + strconv.Itoa(issueNumber(item.Event["ts"])),
		"sig: " + safeHeader(stringValue(item.Event["sig"])), "---", "",
		stringValue(item.Event["body"]), "",
	}
	path := filepath.Join(dir, fmt.Sprintf("%04d-comment.md", index))
	return true, number, os.WriteFile(path, []byte(strings.Join(lines, "\n")), 0600)
}

func discussionTimestampMs(value int) int {
	if value > 0 && value < 10000000000 {
		return value * 1000
	}
	return value
}

func writeDiscussionAnswer(dir string, item collaborationItem) (bool, int, error) {
	number := item.Number
	raw, err := os.ReadFile(filepath.Join(dir, "discussion.md"))
	if err != nil {
		return false, number, nil
	}
	opened, ok := parseFrontMatter(raw)
	if !ok {
		return false, number, nil
	}
	switch strings.ToLower(strings.TrimSpace(opened.values["category"])) {
	case "q&a", "qa", "q and a", "q-and-a":
	default:
		return false, number, nil
	}
	author := safeHeader(stringValue(item.Event["author"]))
	approved, _ := item.Meta["ownerApproved"].(bool)
	if author == "" || (!approved && author != opened.values["author"]) {
		return false, number, nil
	}
	ts := issueNumber(item.Event["ts"])
	if current, err := os.ReadFile(filepath.Join(dir, "answer.md")); err == nil {
		if previous, ok := parseFrontMatter(current); ok {
			before, _ := strconv.Atoi(previous.values["ts"])
			if discussionTimestampMs(before) > discussionTimestampMs(ts) {
				return true, number, nil
			}
		}
	}
	target := safeHeader(stringValue(item.Event["target"]))
	targetAuthor := ""
	if target != "" {
		entries, _ := os.ReadDir(dir)
		for _, entry := range entries {
			if !strings.HasSuffix(entry.Name(), "-comment.md") {
				continue
			}
			body, err := os.ReadFile(filepath.Join(dir, entry.Name()))
			if err != nil {
				continue
			}
			if comment, ok := parseFrontMatter(body); ok && comment.values["id"] == target {
				targetAuthor = comment.values["author"]
				break
			}
		}
		if targetAuthor == "" {
			return false, number, nil
		}
	}
	lines := []string{
		"---", "type: answer", "id: " + safeHeader(stringValue(item.Event["id"])),
		"author: " + author,
		"authorName: " + safeHeader(stringValue(item.Event["authorName"])),
		"ts: " + strconv.Itoa(ts),
		"target: " + target,
		"targetAuthor: " + safeHeader(targetAuthor),
		"sig: " + safeHeader(stringValue(item.Event["sig"])), "---", "",
	}
	return true, number, os.WriteFile(filepath.Join(dir, "answer.md"),
		[]byte(strings.Join(lines, "\n")), 0600)
}

func safeHeader(value string) string {
	return strings.TrimSpace(strings.NewReplacer("\r", " ", "\n", " ").Replace(value))
}

func firstNonEmpty(values ...string) string {
	for _, value := range values {
		if value != "" {
			return value
		}
	}
	return ""
}

func writeJSONFile(path string, value any) error {
	raw, err := json.MarshalIndent(value, "", "    ")
	if err != nil {
		return err
	}
	raw = append(raw, '\n')
	return os.WriteFile(path, raw, 0600)
}

func fileExists(path string) bool {
	info, err := os.Stat(path)
	return err == nil && info.Mode().IsRegular()
}

func sortedUniqueIDs(ids []int64) []int64 {
	set := map[int64]bool{}
	for _, id := range ids {
		if id > 0 {
			set[id] = true
		}
	}
	ids = ids[:0]
	for id := range set {
		ids = append(ids, id)
	}
	sort.Slice(ids, func(i, j int) bool { return ids[i] < ids[j] })
	return ids
}
