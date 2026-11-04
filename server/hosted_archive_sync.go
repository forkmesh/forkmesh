package mirrornode

import (
	"bufio"
	"context"
	"crypto/sha1"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"log"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"time"
)

const (
	hostedArchiveTimeout     = 2 * time.Hour
	hostedArchiveBundleLimit = 30 * time.Minute
	hostedArchiveRetryAfter  = time.Hour
	hostedArchiveMinInterval = time.Hour
	hostedArchiveMaxInterval = 400 * 24 * time.Hour
	hostedArchiveMaxError    = 300
	hostedArchiveMaxText     = 300
	hostedArchiveInvalid     = "invalid archive sync"
	hostedArchiveDir         = ".archive"
	hostedArchiveMaxRefs     = 100
)

var (
	hostedArchiveURLPattern = regexp.MustCompile(
		`^https://s3\.us\.archive\.org/[A-Za-z0-9][A-Za-z0-9_.-]{4,99}/[A-Za-z0-9_.-]{1,100}\.bundle$`)
	hostedArchiveKeyPattern        = regexp.MustCompile(`^[A-Za-z0-9]{8,64}$`)
	hostedArchiveCollectionPattern = regexp.MustCompile(`^[a-z0-9_]{1,64}$`)
	hostedArchiveMessagePattern    = regexp.MustCompile(`(?s)<Message>(.*?)</Message>`)
	hostedArchiveObjectPattern     = regexp.MustCompile(`^[0-9a-f]{40}([0-9a-f]{24})?$`)
)

type hostedArchiveUploaded struct {
	Revision string `json:"revision"`
	Digest   string `json:"digest"`
	At       int64  `json:"at"`
}

type hostedArchiveSync struct {
	Revision        string                 `json:"revision"`
	URL             string                 `json:"url"`
	AccessKey       string                 `json:"accessKey"`
	Secret          string                 `json:"secret"`
	IntervalSeconds int64                  `json:"intervalSeconds"`
	Collection      string                 `json:"collection"`
	Title           string                 `json:"title"`
	Source          string                 `json:"source"`
	Uploaded        *hostedArchiveUploaded `json:"uploaded,omitempty"`
}

type hostedArchiveRef struct {
	Name   string `json:"name"`
	Commit string `json:"commit"`
}

type hostedArchiveBundle struct {
	Bytes         int64
	SHA1          string
	RoutingDigest string
	Head          *hostedArchiveRef
	Refs          []hostedArchiveRef
	RefCount      int
}

type hostedArchiveReport struct {
	Revision      string             `json:"revision"`
	Digest        string             `json:"digest"`
	OK            bool               `json:"ok"`
	At            int64              `json:"at"`
	Error         string             `json:"error"`
	Bytes         int64              `json:"bytes"`
	SHA1          string             `json:"sha1,omitempty"`
	RoutingDigest string             `json:"routingDigest,omitempty"`
	Head          *hostedArchiveRef  `json:"head,omitempty"`
	Refs          []hostedArchiveRef `json:"refs,omitempty"`
	RefCount      int                `json:"refCount,omitempty"`
}

type hostedArchiveState struct {
	report    hostedArchiveReport
	attempted time.Time
	running   bool
	success   *hostedArchiveUploaded
}

type hostedArchiveUploader func(ctx context.Context, sync hostedArchiveSync, path string, size int64) error

func hostedArchiveText(value string) bool {
	if value == "" || len(value) > hostedArchiveMaxText {
		return false
	}
	for _, char := range value {
		if char < ' ' || char > '~' {
			return false
		}
	}
	return true
}

func validHostedArchiveSync(sync hostedArchiveSync) bool {
	if sync.Revision == "" || len(sync.Revision) > 80 {
		return false
	}
	if !hostedArchiveURLPattern.MatchString(sync.URL) || strings.Contains(sync.URL, "..") {
		return false
	}
	if !hostedArchiveKeyPattern.MatchString(sync.AccessKey) || !hostedArchiveKeyPattern.MatchString(sync.Secret) {
		return false
	}
	interval := time.Duration(sync.IntervalSeconds) * time.Second
	if interval < hostedArchiveMinInterval || interval > hostedArchiveMaxInterval {
		return false
	}
	return hostedArchiveCollectionPattern.MatchString(sync.Collection) &&
		hostedArchiveText(sync.Title) && hostedArchiveText(sync.Source)
}

func redactHostedArchiveError(text string, sync hostedArchiveSync) string {
	for _, secret := range []string{sync.Secret, sync.AccessKey} {
		if secret != "" {
			text = strings.ReplaceAll(text, secret, "***")
		}
	}
	return boundedText([]byte(text), hostedArchiveMaxError)
}

func hostedArchiveFile(sync hostedArchiveSync) string {
	return sync.URL[strings.LastIndex(sync.URL, "/")+1:]
}

func putHostedArchive(ctx context.Context, sync hostedArchiveSync, path string, size int64) error {
	return putHostedArchiveTo(ctx, &http.Client{CheckRedirect: func(*http.Request, []*http.Request) error {
		return http.ErrUseLastResponse
	}}, sync.URL, sync, path, size)
}

func putHostedArchiveTo(ctx context.Context, client *http.Client, target string, sync hostedArchiveSync,
	path string, size int64) error {
	file, err := os.Open(path)
	if err != nil {
		return err
	}
	defer file.Close()
	request, err := http.NewRequestWithContext(ctx, http.MethodPut, target, file)
	if err != nil {
		return err
	}
	request.ContentLength = size
	description := fmt.Sprintf("Git bundle of %s, archived from %s. Restore it with: git clone %s",
		sync.Title, sync.Source, hostedArchiveFile(sync))
	for name, value := range map[string]string{
		"Authorization":              "LOW " + sync.AccessKey + ":" + sync.Secret,
		"Content-Type":               "application/octet-stream",
		"x-amz-auto-make-bucket":     "1",
		"x-archive-queue-derive":     "0",
		"x-archive-size-hint":        strconv.FormatInt(size, 10),
		"x-archive-meta-mediatype":   "software",
		"x-archive-meta-collection":  sync.Collection,
		"x-archive-meta-title":       sync.Title,
		"x-archive-meta-description": description,
		"x-archive-meta-source":      sync.Source,
		"x-archive-meta-subject":     "git; source code",
	} {
		request.Header.Set(name, value)
	}
	response, err := client.Do(request)
	if err != nil {
		return err
	}
	defer response.Body.Close()
	if response.StatusCode >= 200 && response.StatusCode < 300 {
		_, _ = io.Copy(io.Discard, io.LimitReader(response.Body, 64<<10))
		return nil
	}
	body, _ := io.ReadAll(io.LimitReader(response.Body, 8<<10))
	message := strings.TrimSpace(string(body))
	if match := hostedArchiveMessagePattern.FindStringSubmatch(message); match != nil {
		message = strings.TrimSpace(match[1])
	}
	if message == "" {
		return fmt.Errorf("the Internet Archive answered HTTP %d", response.StatusCode)
	}
	return fmt.Errorf("the Internet Archive answered HTTP %d: %s", response.StatusCode, message)
}

func (r *hostedReplicator) archiveSyncReports() map[string]hostedArchiveReport {
	r.mu.Lock()
	defer r.mu.Unlock()
	reports := map[string]hostedArchiveReport{}
	for id, state := range r.archiveState {
		if state.report.At != 0 {
			reports[id] = state.report
		}
	}
	if len(reports) == 0 {
		return nil
	}
	return reports
}

func (r *hostedReplicator) forgetArchiveSync(id string) {
	r.mu.Lock()
	defer r.mu.Unlock()
	if state, known := r.archiveState[id]; known && !state.running {
		delete(r.archiveState, id)
	}
}

func (r *hostedReplicator) archiveSyncs(ctx context.Context, response hostedReplicaResponse) {
	for _, removed := range response.Removed {
		r.forgetArchiveSync(removed.ID)
	}
	for _, item := range response.Repositories {
		if !validHostedDesired(item) {
			continue
		}
		if item.ArchiveSync == nil {
			r.forgetArchiveSync(item.ID)
			continue
		}
		if !item.Primary || item.Visibility != "public" {
			continue
		}
		r.archiveSync(ctx, item)
	}
}

func (r *hostedReplicator) archiveDue(id string, sync hostedArchiveSync, digest string, now time.Time) bool {
	if r.archiveState == nil {
		r.archiveState = map[string]hostedArchiveState{}
	}
	for _, state := range r.archiveState {
		if state.running {
			return false
		}
	}
	state, known := r.archiveState[id]
	if known && !state.report.OK && state.report.Revision == sync.Revision &&
		now.Sub(state.attempted) < hostedArchiveRetryAfter {
		return false
	}
	last := sync.Uploaded
	if known && state.success != nil {
		last = state.success
	}
	if last == nil || last.Revision != sync.Revision {
		return true
	}
	if digest == last.Digest {
		return false
	}
	return now.Sub(time.UnixMilli(last.At)) >= time.Duration(sync.IntervalSeconds)*time.Second
}

func (r *hostedReplicator) archiveSync(ctx context.Context, item hostedDesired) {
	sync := *item.ArchiveSync
	now := r.now()
	if !validHostedArchiveSync(sync) {
		r.mu.Lock()
		if r.archiveState == nil {
			r.archiveState = map[string]hostedArchiveState{}
		}
		state := r.archiveState[item.ID]
		if state.running || (state.report.Error == hostedArchiveInvalid && state.report.Revision == sync.Revision) {
			r.mu.Unlock()
			return
		}
		state.report = hostedArchiveReport{Revision: sync.Revision, At: now.UnixMilli(), Error: hostedArchiveInvalid}
		state.attempted = now
		r.archiveState[item.ID] = state
		r.mu.Unlock()
		log.Printf("hosted %s: archive sync: %s", item.ID, hostedArchiveInvalid)
		return
	}
	gitDir := r.gitDir(item.ID)
	if !validBareRepository(gitDir) || len(gitLines(ctx, gitDir, "for-each-ref", "--count=1",
		"--format=%(refname)", "refs/heads", "refs/tags")) == 0 {
		return
	}
	digest, err := refsSHA256(ctx, gitDir)
	if err != nil {
		return
	}
	r.mu.Lock()
	if !r.archiveDue(item.ID, sync, digest, now) {
		r.mu.Unlock()
		return
	}
	state := r.archiveState[item.ID]
	state.running = true
	state.attempted = now
	r.archiveState[item.ID] = state
	r.mu.Unlock()
	r.archiveWork.Add(1)
	go func() {
		defer r.archiveWork.Done()
		report := r.uploadArchiveBundle(item.ID, gitDir, sync, digest)
		r.mu.Lock()
		state := r.archiveState[item.ID]
		state.running = false
		state.report = report
		if report.OK {
			state.success = &hostedArchiveUploaded{Revision: report.Revision, Digest: report.Digest, At: report.At}
		}
		r.archiveState[item.ID] = state
		r.mu.Unlock()
		r.request()
	}()
}

func (r *hostedReplicator) uploadArchiveBundle(id, gitDir string, sync hostedArchiveSync, digest string) hostedArchiveReport {
	ctx, cancel := context.WithTimeout(context.Background(), hostedArchiveTimeout)
	defer cancel()
	report := hostedArchiveReport{Revision: sync.Revision, Digest: digest}
	bundle, err := r.bundleAndUpload(ctx, id, gitDir, sync)
	report.At = r.now().UnixMilli()
	report.OK = err == nil
	report.Bytes = bundle.Bytes
	if err == nil {
		report.SHA1 = bundle.SHA1
		report.RoutingDigest = bundle.RoutingDigest
		report.Head = bundle.Head
		report.Refs = bundle.Refs
		report.RefCount = bundle.RefCount
	}
	if err != nil {
		report.Error = redactHostedArchiveError(err.Error(), sync)
		if report.Error == "" {
			report.Error = "upload failed"
		}
		log.Printf("hosted %s: archive sync to %s failed: %s", id, sync.URL, report.Error)
	}
	return report
}

func (r *hostedReplicator) bundleAndUpload(ctx context.Context, id, gitDir string,
	sync hostedArchiveSync) (hostedArchiveBundle, error) {
	var bundle hostedArchiveBundle
	dir := filepath.Join(r.cfg.Root, hostedArchiveDir)
	if err := os.MkdirAll(dir, 0o700); err != nil {
		return bundle, err
	}
	file, err := os.CreateTemp(dir, id+"-*.bundle")
	if err != nil {
		return bundle, err
	}
	path := file.Name()
	file.Close()
	defer os.Remove(path)
	head := gitValue(ctx, gitDir, "symbolic-ref", "--quiet", "HEAD")
	if !strings.HasPrefix(head, "refs/heads/") {
		head = "refs/heads/main"
	}
	args := []string{"-C", gitDir, "bundle", "create", "--quiet", path}
	if gitValue(ctx, gitDir, "rev-parse", "--verify", "--quiet", "HEAD^{commit}") != "" {
		args = append(args, "HEAD")
	}
	args = append(args, "--branches", "--tags")
	bundleCtx, cancel := context.WithTimeout(ctx, hostedArchiveBundleLimit)
	defer cancel()
	command := exec.CommandContext(bundleCtx, "git", args...)
	var stderr strings.Builder
	command.Stderr = &stderr
	if err := command.Run(); err != nil {
		text := strings.TrimSpace(stderr.String())
		if text == "" {
			text = err.Error()
		}
		return bundle, errors.New("git bundle: " + text)
	}
	bundle, err = describeHostedArchiveBundle(bundleCtx, path, head)
	if err != nil {
		return bundle, err
	}
	upload := r.uploadArchive
	if upload == nil {
		upload = putHostedArchive
	}
	if err := upload(ctx, sync, path, bundle.Bytes); err != nil {
		return bundle, err
	}
	return bundle, nil
}

func describeHostedArchiveBundle(ctx context.Context, path, head string) (hostedArchiveBundle, error) {
	var bundle hostedArchiveBundle
	file, err := os.Open(path)
	if err != nil {
		return bundle, err
	}
	defer file.Close()
	hash := sha1.New()
	size, err := io.Copy(hash, file)
	if err != nil {
		return bundle, err
	}
	bundle.Bytes = size
	bundle.SHA1 = hex.EncodeToString(hash.Sum(nil))
	output, err := exec.CommandContext(ctx, "git", "bundle", "list-heads", path).Output()
	if err != nil {
		return bundle, fmt.Errorf("git bundle list-heads: %w", err)
	}
	refs := map[string]string{}
	scanner := bufio.NewScanner(strings.NewReader(string(output)))
	for scanner.Scan() {
		parts := strings.Fields(scanner.Text())
		if len(parts) != 2 || !hostedArchiveObjectPattern.MatchString(parts[0]) ||
			(!strings.HasPrefix(parts[1], "refs/heads/") && !strings.HasPrefix(parts[1], "refs/tags/")) ||
			!hostedArchiveText(parts[1]) {
			continue
		}
		refs[parts[1]] = parts[0]
	}
	names := make([]string, 0, len(refs))
	for name := range refs {
		names = append(names, name)
	}
	sort.Strings(names)
	var routing []string
	for _, name := range names {
		if name == head || strings.HasPrefix(name, "refs/tags/") {
			routing = append(routing, refs[name]+" "+name)
		}
	}
	digest := sha256.Sum256([]byte(strings.Join(routing, "\n")))
	bundle.RoutingDigest = hex.EncodeToString(digest[:])
	if commit, ok := refs[head]; ok {
		bundle.Head = &hostedArchiveRef{Name: head, Commit: commit}
	}
	bundle.RefCount = len(names)
	sort.SliceStable(names, func(i, j int) bool {
		return names[i] == head && names[j] != head
	})
	for _, name := range names {
		if len(bundle.Refs) == hostedArchiveMaxRefs {
			break
		}
		bundle.Refs = append(bundle.Refs, hostedArchiveRef{Name: name, Commit: refs[name]})
	}
	return bundle, nil
}
