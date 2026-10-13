package mirrornode

// Durable website-created fork intake for managed mirrors.
//
// The relay stores only a control-plane job. Repository bytes still travel
// directly through Git from the mesh, and the target node acknowledges the
// job only after its bare repository and gateway configuration are durable.

import (
	"context"
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
	"sort"
	"strings"
	"time"
)

const (
	forkJobResponseLimit = 512 << 10
	forkJobBatchLimit    = 20
	forkJobSafetyCheck   = 5 * time.Minute
)

var forkJobHTTPClient = &http.Client{Timeout: 30 * time.Second}

type forkJobInbox struct {
	Pending []forkJob `json:"pending"`
}

type forkJob struct {
	RequestID   string `json:"requestId"`
	SourceOwner string `json:"sourceOwner"`
	SourceName  string `json:"sourceName"`
	SourceURL   string `json:"sourceUrl"`
	Owner       string `json:"owner"`
	Name        string `json:"name"`
	ForkedFrom  string `json:"forkedFrom"`
	Status      string `json:"status"`
	CreatedAt   int64  `json:"createdAt"`
}

func (d *Daemon) forkJobsURL() (*url.URL, error) {
	base, err := url.Parse(d.config.CatalogURL)
	if err != nil || base.Host == "" ||
		(base.Scheme != "https" && !(base.Scheme == "http" && isLoopbackHost(base.Hostname()))) {
		return nil, errors.New("catalogUrl is not an absolute HTTP URL")
	}
	base.Path = "/api/repository-forks"
	base.RawPath, base.RawQuery, base.Fragment = "", "", ""
	return base, nil
}

// shouldCheckForkJobs keeps live pushes as the fast path while preserving a
// bounded safety read for a frame lost while the node was offline. A failed
// clone is retried on the same cadence, not on every repository sync cycle.
func (d *Daemon) shouldCheckForkJobs() bool {
	d.mu.Lock()
	defer d.mu.Unlock()
	if !d.forkJobsEnabled {
		return false
	}
	now := time.Now()
	if !d.forkJobsPending && !d.forkJobsLastCheck.IsZero() &&
		now.Sub(d.forkJobsLastCheck) < forkJobSafetyCheck {
		return false
	}
	d.forkJobsPending = false
	d.forkJobsLastCheck = now
	return true
}

func (d *Daemon) requestForkJobCheck() {
	d.mu.Lock()
	if d.forkJobsEnabled {
		d.forkJobsPending = true
	}
	d.mu.Unlock()
}

func (d *Daemon) drainForkJobs(ctx context.Context) error {
	endpoint, err := d.forkJobsURL()
	if err != nil {
		return err
	}
	endpoint.RawQuery = d.signedCollaborationQuery().Encode()
	request, err := http.NewRequestWithContext(
		ctx, http.MethodGet, endpoint.String(), nil)
	if err != nil {
		return err
	}
	request.Header.Set("Accept", "application/json")
	request.Header.Set("User-Agent", "ForkMesh-Mirror-Node/"+d.config.Version)
	response, err := forkJobHTTPClient.Do(request)
	if err != nil {
		return err
	}
	defer response.Body.Close()
	body, err := io.ReadAll(io.LimitReader(response.Body, forkJobResponseLimit+1))
	if err != nil {
		return err
	}
	if len(body) > forkJobResponseLimit {
		return errors.New("fork job response is too large")
	}
	if response.StatusCode < 200 || response.StatusCode >= 300 {
		return fmt.Errorf("fork job relay returned HTTP %d: %s",
			response.StatusCode, boundedText(body, 240))
	}
	var inbox forkJobInbox
	if err := json.Unmarshal(body, &inbox); err != nil {
		return errors.New("fork job relay returned invalid JSON")
	}
	if len(inbox.Pending) > forkJobBatchLimit {
		return errors.New("fork job relay returned too many jobs")
	}

	var acknowledged []string
	var failures []string
	for _, job := range inbox.Pending {
		if err := d.materializeFork(ctx, job); err != nil {
			failures = append(failures, job.RequestID+": "+err.Error())
			continue
		}
		acknowledged = append(acknowledged, job.RequestID)
	}
	if len(acknowledged) > 0 {
		if err := d.ackForkJobs(ctx, acknowledged); err != nil {
			failures = append(failures, "ack: "+err.Error())
		}
	}
	if len(failures) > 0 {
		return errors.New(strings.Join(failures, "; "))
	}
	return nil
}

func (d *Daemon) ackForkJobs(ctx context.Context, ids []string) error {
	endpoint, err := d.forkJobsURL()
	if err != nil {
		return err
	}
	query := d.signedCollaborationQuery()
	clean := make([]string, 0, len(ids))
	seen := map[string]bool{}
	for _, id := range ids {
		if requestIDPattern.MatchString(id) && !seen[id] {
			clean = append(clean, id)
			seen[id] = true
		}
	}
	if len(clean) == 0 {
		return nil
	}
	sort.Strings(clean)
	query.Set("ids", strings.Join(clean, ","))
	endpoint.RawQuery = query.Encode()
	request, err := http.NewRequestWithContext(
		ctx, http.MethodDelete, endpoint.String(), nil)
	if err != nil {
		return err
	}
	request.Header.Set("Accept", "application/json")
	request.Header.Set("User-Agent", "ForkMesh-Mirror-Node/"+d.config.Version)
	response, err := forkJobHTTPClient.Do(request)
	if err != nil {
		return err
	}
	defer response.Body.Close()
	body, _ := io.ReadAll(io.LimitReader(response.Body, 64<<10))
	if response.StatusCode < 200 || response.StatusCode >= 300 {
		return fmt.Errorf("fork job ack returned HTTP %d: %s",
			response.StatusCode, boundedText(body, 240))
	}
	return nil
}

func forkMeshServiceHost(host string) string {
	value := strings.ToLower(strings.TrimSpace(host))
	for _, prefix := range []string{"www.", "app.", "api."} {
		value = strings.TrimPrefix(value, prefix)
	}
	return value
}

func (d *Daemon) validateForkJob(job forkJob) error {
	if !requestIDPattern.MatchString(job.RequestID) ||
		!forkParentPattern.MatchString(job.SourceOwner+"/"+job.SourceName) ||
		!repositoryPattern.MatchString(job.Name) ||
		job.Owner != d.gateway.Node.Name || job.Owner != d.config.PublishOwner ||
		job.Name != job.SourceName ||
		job.ForkedFrom != strings.ToLower(job.SourceOwner+"/"+job.SourceName) {
		return errors.New("invalid fork job identity")
	}
	source, err := url.Parse(job.SourceURL)
	catalog, catalogErr := url.Parse(d.config.CatalogURL)
	if err != nil || catalogErr != nil || source.User != nil ||
		source.Host == "" || source.RawQuery != "" || source.Fragment != "" ||
		(source.Scheme != "https" &&
			!(source.Scheme == "http" && isLoopbackHost(source.Hostname()))) ||
		forkMeshServiceHost(source.Hostname()) !=
			forkMeshServiceHost(catalog.Hostname()) ||
		source.Path != "/"+job.SourceOwner+"/"+job.SourceName {
		return errors.New("invalid fork source URL")
	}
	return nil
}

func (d *Daemon) forkRepositoryRoot() (string, error) {
	for _, repository := range d.gateway.Repositories {
		if repository.Owner != d.config.PublishOwner ||
			!filepath.IsAbs(repository.GitDir) {
			continue
		}
		root := filepath.Clean(filepath.Dir(repository.GitDir))
		info, err := os.Lstat(root)
		if err != nil || !info.IsDir() || info.Mode()&os.ModeSymlink != 0 {
			return "", errors.New("repository storage root is unavailable")
		}
		return root, nil
	}
	return "", errors.New("repository storage root is unavailable")
}

func (d *Daemon) localForkReference(job forkJob) string {
	for _, repository := range d.gateway.Repositories {
		if strings.EqualFold(repository.Owner, job.SourceOwner) &&
			strings.EqualFold(repository.Name, job.SourceName) &&
			validBareRepository(repository.GitDir) {
			return repository.GitDir
		}
	}
	return ""
}

func validBareRepository(path string) bool {
	if !filepath.IsAbs(path) {
		return false
	}
	info, err := os.Lstat(path)
	if err != nil || !info.IsDir() || info.Mode()&os.ModeSymlink != 0 {
		return false
	}
	output, err := exec.Command(
		"git", "--git-dir="+path, "rev-parse", "--is-bare-repository").Output()
	return err == nil && strings.TrimSpace(string(output)) == "true"
}

func cloneForkRepository(ctx context.Context, sourceURL, reference, destination string) error {
	parent := filepath.Dir(destination)
	if err := os.MkdirAll(parent, 0700); err != nil {
		return err
	}
	sources := []string{sourceURL}
	if reference != "" && reference != sourceURL {
		// The stable mesh route is preferred for freshness. A local verified
		// mirror is the offline fallback and supplies the shared object store in
		// either case, so a fork normally transfers only refs and missing objects.
		sources = append(sources, reference)
	}
	var failures []string
	for _, source := range sources {
		temporary, err := os.MkdirTemp(parent, ".fork-clone-*")
		if err != nil {
			return err
		}
		if err := os.Remove(temporary); err != nil {
			return err
		}
		args := []string{"clone", "--mirror"}
		if reference != "" {
			args = append(args, "--no-local", "--reference-if-able", reference)
		}
		args = append(args, "--", source, temporary)
		command := exec.CommandContext(ctx, "git", args...)
		command.Env = gitNetworkEnvironment()
		output, cloneErr := command.CombinedOutput()
		if cloneErr == nil {
			check := exec.CommandContext(ctx, "git", "--git-dir="+temporary,
				"fsck", "--connectivity-only", "--no-dangling")
			if checkOutput, checkErr := check.CombinedOutput(); checkErr != nil {
				cloneErr = fmt.Errorf("connectivity check failed: %s",
					boundedText(checkOutput, 200))
			}
		}
		if cloneErr == nil {
			if err := os.Rename(temporary, destination); err == nil {
				return nil
			} else {
				cloneErr = err
			}
		}
		_ = removeTreeUnder(filepath.Dir(temporary), temporary)
		failures = append(failures, boundedText(output, 200)+" "+cloneErr.Error())
	}
	return fmt.Errorf("fork clone failed: %s", strings.Join(failures, "; "))
}

// Pin the initial tips in the borrowed repository. Git alternates avoid a
// second object database, while these hidden refs prevent a later force-push
// and source-side GC from deleting objects an independent fork still needs.
// refsSHA256 intentionally hashes only heads and tags, so these pins never
// alter the source's advertised repository state.
func pinForkReferenceObjects(ctx context.Context, source, requestID string) error {
	if source == "" || !requestIDPattern.MatchString(requestID) {
		return nil
	}
	output, err := exec.CommandContext(ctx, "git", "--git-dir="+source,
		"for-each-ref", "--format=%(objectname) %(refname)",
		"refs/heads", "refs/tags").Output()
	if err != nil {
		return err
	}
	for _, line := range strings.Split(strings.TrimSpace(string(output)), "\n") {
		fields := strings.Fields(line)
		if len(fields) != 2 ||
			(len(fields[0]) != 40 && len(fields[0]) != 64) {
			continue
		}
		if _, decodeErr := hex.DecodeString(fields[0]); decodeErr != nil {
			continue
		}
		kind, name := "", ""
		switch {
		case strings.HasPrefix(fields[1], "refs/heads/"):
			kind, name = "heads", strings.TrimPrefix(fields[1], "refs/heads/")
		case strings.HasPrefix(fields[1], "refs/tags/"):
			kind, name = "tags", strings.TrimPrefix(fields[1], "refs/tags/")
		}
		if kind == "" || name == "" || strings.Contains(name, "..") {
			continue
		}
		ref := "refs/forkmesh/fork-bases/" + requestID + "/" + kind + "/" + name
		if result, updateErr := exec.CommandContext(ctx, "git", "--git-dir="+source,
			"update-ref", ref, fields[0]).CombinedOutput(); updateErr != nil {
			return fmt.Errorf("pin %s: %s", fields[1], boundedText(result, 160))
		}
	}
	return nil
}

func forkGatewayOperations(gateway GatewayConfig, owner string) []string {
	for _, repository := range gateway.Repositories {
		if repository.Owner == owner && len(repository.Operations) > 0 {
			return append([]string(nil), repository.Operations...)
		}
	}
	return append([]string(nil), defaultGatewayOperations...)
}

func (d *Daemon) materializeFork(ctx context.Context, job forkJob) error {
	if err := d.validateForkJob(job); err != nil {
		return err
	}
	for _, repository := range d.gateway.Repositories {
		if repositoryKey(repository) != strings.ToLower(job.Owner+"/"+job.Name) {
			continue
		}
		if strings.EqualFold(repository.ForkedFrom, job.ForkedFrom) &&
			validBareRepository(repository.GitDir) {
			return nil // retry after a lost acknowledgement
		}
		return errors.New("target repository name is already configured")
	}

	root, err := d.forkRepositoryRoot()
	if err != nil {
		return err
	}
	destination := filepath.Join(root, job.Owner+"-"+job.Name+".git")
	if filepath.Dir(destination) != root {
		return errors.New("invalid fork destination")
	}
	reference := d.localForkReference(job)
	if info, statErr := os.Lstat(destination); statErr == nil {
		if !info.IsDir() || info.Mode()&os.ModeSymlink != 0 ||
			!validBareRepository(destination) {
			return errors.New("fork destination already exists")
		}
	} else if !errors.Is(statErr, os.ErrNotExist) {
		return statErr
	} else if err := cloneForkRepository(
		ctx, job.SourceURL, reference, destination); err != nil {
		return err
	}
	// A fork is independent: origin is provenance, not a force-sync remote.
	// `forkedFrom` carries that provenance in the signed catalog instead.
	_ = exec.CommandContext(ctx, "git", "--git-dir="+destination,
		"remote", "remove", "origin").Run()
	if err := pinForkReferenceObjects(ctx, reference, job.RequestID); err != nil {
		return err
	}
	digest, err := refsSHA256(ctx, destination)
	if err != nil {
		return err
	}
	releaseStore := filepath.Join(destination, "forkmesh-releases")
	if err := os.MkdirAll(releaseStore, 0700); err != nil {
		return err
	}
	repository := Repository{
		Owner: job.Owner, Name: job.Name, ForkedFrom: job.ForkedFrom,
		Visibility: "public", Enabled: true, GitDir: destination,
		ReleaseStore: releaseStore,
		Operations:   forkGatewayOperations(d.gateway, job.Owner),
	}
	repository.Integrity.ExpectedRefsSHA256 = digest
	previousLength := len(d.gateway.Repositories)
	d.gateway.Repositories = append(d.gateway.Repositories, repository)
	if _, err := writeGatewayConfig(d.config.GatewayConfig, d.gateway); err != nil {
		d.gateway.Repositories = d.gateway.Repositories[:previousLength]
		return err
	}
	d.markEndpointStateDirty()
	d.restartGateway()
	return nil
}
