package mirrornode

import (
	"context"
	"encoding/base64"
	"errors"
	"log"
	"os/exec"
	"regexp"
	"strings"
	"time"
	"unicode"
)

const (
	hostedPushTimeout      = 10 * time.Minute
	hostedPushRetryAfter   = 10 * time.Minute
	hostedPushRefreshAfter = 6 * time.Hour
	hostedPushMaxToken     = 512
	hostedPushMaxError     = 300
	hostedPushInvalid      = "invalid push mirror"
	hostedPushMaxDeletes = 1000

	hostedPushRefsAll     = "all"
	hostedPushRefsDefault = "default"
)

var (
	hostedDefaultPushHosts = []string{"github.com", "gitlab.com"}
	hostedPushURLPattern   = regexp.MustCompile(
		`^https://([a-z0-9][a-z0-9.-]{0,252})/[A-Za-z0-9][A-Za-z0-9._-]{0,99}/[A-Za-z0-9._-]{1,100}\.git$`)
)

type hostedPushMirror struct {
	Revision string `json:"revision"`
	Provider string `json:"provider"`
	URL      string `json:"url"`
	Username string `json:"username"`
	Token    string `json:"token"`
	Prune    bool   `json:"prune"`
	Refs string `json:"refs,omitempty"`
}

type hostedPushReport struct {
	Revision string `json:"revision"`
	Digest   string `json:"digest"`
	OK       bool   `json:"ok"`
	At       int64  `json:"at"`
	Error    string `json:"error"`
	Refs     int    `json:"refs"`
}

type hostedPushState struct {
	report         hostedPushReport
	attempted      time.Time
	succeeded      time.Time
	successDigest  string
	successAllRefs string
	refs           string
}

type hostedPushRunner func(ctx context.Context, gitDir string, args, env []string) (string, string, error)

func runHostedPush(ctx context.Context, gitDir string, args, env []string) (string, string, error) {
	ctx, cancel := context.WithTimeout(ctx, hostedPushTimeout)
	defer cancel()
	command := exec.CommandContext(ctx, "git", append([]string{"-C", gitDir}, args...)...)
	command.Env = env
	var stdout, stderr strings.Builder
	command.Stdout = &stdout
	command.Stderr = &stderr
	err := command.Run()
	return stdout.String(), stderr.String(), err
}

func hostedPushRefsMode(mirror hostedPushMirror) string {
	if mirror.Refs == "" {
		return hostedPushRefsAll
	}
	return mirror.Refs
}

func validHostedPushMirror(mirror hostedPushMirror, hosts []string) bool {
	if mode := hostedPushRefsMode(mirror); mode != hostedPushRefsAll && mode != hostedPushRefsDefault {
		return false
	}
	if mirror.Provider != "github" && mirror.Provider != "gitlab" {
		return false
	}
	if mirror.Username != "x-access-token" && mirror.Username != "oauth2" {
		return false
	}
	if mirror.Token == "" || len(mirror.Token) > hostedPushMaxToken {
		return false
	}
	for _, char := range mirror.Token {
		if char <= ' ' || char == 0x7f || unicode.IsSpace(char) || unicode.IsControl(char) {
			return false
		}
	}
	match := hostedPushURLPattern.FindStringSubmatch(mirror.URL)
	if match == nil || strings.Contains(mirror.URL, "..") {
		return false
	}
	for _, host := range hosts {
		if match[1] == host {
			return true
		}
	}
	return false
}

func hostedPushAuthorization(mirror hostedPushMirror) string {
	return base64.StdEncoding.EncodeToString([]byte(mirror.Username + ":" + mirror.Token))
}

func redactHostedPushError(text string, mirror hostedPushMirror) string {
	secrets := []string{hostedPushAuthorization(mirror), mirror.Token}
	for _, secret := range secrets {
		if secret != "" {
			text = strings.ReplaceAll(text, secret, "***")
		}
	}
	return boundedText([]byte(text), hostedPushMaxError)
}

func hostedPushRefCount(porcelain string) int {
	count := 0
	for _, line := range strings.Split(porcelain, "\n") {
		if len(line) < 2 || line[1] != '\t' {
			continue
		}
		switch line[0] {
		case ' ', '+', '-', '*':
			count++
		}
	}
	return count
}

func (r *hostedReplicator) pushMirrorReports() map[string]hostedPushReport {
	r.mu.Lock()
	defer r.mu.Unlock()
	if len(r.pushState) == 0 {
		return nil
	}
	reports := make(map[string]hostedPushReport, len(r.pushState))
	for id, state := range r.pushState {
		reports[id] = state.report
	}
	return reports
}

func (r *hostedReplicator) forgetPushMirror(id string) {
	r.mu.Lock()
	delete(r.pushState, id)
	r.mu.Unlock()
}

func (r *hostedReplicator) pushMirrorDue(id, revision, refs, digest, allRefs string, now time.Time) bool {
	r.mu.Lock()
	defer r.mu.Unlock()
	state, known := r.pushState[id]
	if !known || state.report.Revision != revision || state.refs != refs {
		return true
	}
	if !state.report.OK {
		return now.Sub(state.attempted) >= hostedPushRetryAfter
	}
	return digest != state.successDigest || allRefs != state.successAllRefs ||
		now.Sub(state.succeeded) >= hostedPushRefreshAfter
}

func (r *hostedReplicator) recordPush(id string, report hostedPushReport, refs, allRefs string, now time.Time) {
	r.mu.Lock()
	defer r.mu.Unlock()
	state := r.pushState[id]
	state.report = report
	state.refs = refs
	state.attempted = now
	if report.OK {
		state.succeeded = now
		state.successDigest = report.Digest
		state.successAllRefs = allRefs
	}
	r.pushState[id] = state
}

func (r *hostedReplicator) recordInvalidPush(id, revision string, now time.Time) {
	r.mu.Lock()
	defer r.mu.Unlock()
	state, known := r.pushState[id]
	if known && !state.report.OK && state.report.Revision == revision &&
		state.report.Error == hostedPushInvalid {
		return
	}
	state.report = hostedPushReport{Revision: revision, OK: false, At: now.UnixMilli(),
		Error: hostedPushInvalid}
	state.refs = ""
	state.attempted = now
	r.pushState[id] = state
}

func (r *hostedReplicator) pushMirrors(ctx context.Context, response hostedReplicaResponse) {
	for _, removed := range response.Removed {
		r.forgetPushMirror(removed.ID)
	}
	pushed := false
	for _, item := range response.Repositories {
		if !validHostedDesired(item) {
			continue
		}
		if item.PushMirror == nil {
			r.forgetPushMirror(item.ID)
			continue
		}
		if !item.Primary {
			continue
		}
		if r.pushMirror(ctx, item) {
			pushed = true
		}
	}
	if pushed {
		r.request()
	}
}

func (r *hostedReplicator) pushMirror(ctx context.Context, item hostedDesired) bool {
	mirror := *item.PushMirror
	now := r.now()
	if !validHostedPushMirror(mirror, r.cfg.PushHosts) {
		r.recordInvalidPush(item.ID, mirror.Revision, now)
		log.Printf("hosted %s: push mirror: %s", item.ID, hostedPushInvalid)
		return false
	}
	gitDir := r.gitDir(item.ID)
	if !validBareRepository(gitDir) || len(gitLines(ctx, gitDir, "for-each-ref", "--count=1",
		"--format=%(refname)", "refs/heads", "refs/tags")) == 0 {
		return false
	}
	digest, err := routingRefsSHA256(ctx, gitDir)
	if err != nil {
		return false
	}
	allRefs, err := refsSHA256(ctx, gitDir)
	if err != nil {
		return false
	}
	mode := hostedPushRefsMode(mirror)
	if mode == hostedPushRefsDefault {
		allRefs = digest
	}
	if !r.pushMirrorDue(item.ID, mirror.Revision, mode, digest, allRefs, now) {
		return false
	}
	env := append(gitNetworkEnvironment(),
		"GIT_ALLOW_PROTOCOL=https",
		"GIT_ASKPASS=",
		"SSH_ASKPASS=",
		"GIT_CONFIG_COUNT=3",
		"GIT_CONFIG_KEY_0=http.extraHeader",
		"GIT_CONFIG_VALUE_0=Authorization: Basic "+hostedPushAuthorization(mirror),
		"GIT_CONFIG_KEY_1=http.followRedirects",
		"GIT_CONFIG_VALUE_1=false",
		"GIT_CONFIG_KEY_2=credential.helper",
		"GIT_CONFIG_VALUE_2=")
	runner := r.runPush
	if runner == nil {
		runner = runHostedPush
	}
	report := hostedPushReport{Revision: mirror.Revision, Digest: digest}
	var stdout, stderr string
	refspecs, err := hostedPushRefspecs(ctx, gitDir, mirror, env, runner)
	if err == nil {
		args := []string{"push", "--porcelain"}
		if mirror.Prune {
			args = append(args, "--prune")
		}
		args = append(append(args, mirror.URL), refspecs...)
		stdout, stderr, err = runner(ctx, gitDir, args, env)
		report.Refs = hostedPushRefCount(stdout)
	} else {
		var listed *hostedPushListError
		if errors.As(err, &listed) {
			stdout, stderr = listed.stdout, listed.stderr
		}
	}
	report.OK = err == nil
	report.At = r.now().UnixMilli()
	if err != nil {
		text := strings.TrimSpace(stderr)
		if text == "" {
			text = strings.TrimSpace(stdout)
		}
		if text == "" {
			text = err.Error()
		}
		report.Error = redactHostedPushError(text, mirror)
		if report.Error == "" {
			report.Error = "push failed"
		}
		log.Printf("hosted %s: push mirror to %s failed: %s", item.ID, mirror.URL, report.Error)
	}
	r.recordPush(item.ID, report, mode, allRefs, now)
	return true
}

type hostedPushListError struct {
	err            error
	stdout, stderr string
}

func (e *hostedPushListError) Error() string { return e.err.Error() }
func (e *hostedPushListError) Unwrap() error { return e.err }

func hostedPushRefspecs(ctx context.Context, gitDir string, mirror hostedPushMirror, env []string,
	runner hostedPushRunner) ([]string, error) {
	if hostedPushRefsMode(mirror) != hostedPushRefsDefault {
		return []string{"+refs/heads/*:refs/heads/*", "+refs/tags/*:refs/tags/*"}, nil
	}
	head := gitValue(ctx, gitDir, "symbolic-ref", "--quiet", "HEAD")
	if !strings.HasPrefix(head, "refs/heads/") || gitValue(ctx, gitDir, "rev-parse", "--verify",
		"--quiet", head+"^{commit}") == "" {
		return nil, errors.New("the default branch has no commits")
	}
	refspecs := []string{"+" + head + ":" + head, "+refs/tags/*:refs/tags/*"}
	if !mirror.Prune {
		return refspecs, nil
	}
	stdout, stderr, err := runner(ctx, gitDir, []string{"ls-remote", "--heads", mirror.URL}, env)
	if err != nil {
		return nil, &hostedPushListError{err: err, stdout: stdout, stderr: stderr}
	}
	return append(refspecs, hostedPushExtraBranchDeletes(stdout, head)...), nil
}

func hostedPushExtraBranchDeletes(lsRemote, keep string) []string {
	var deletes []string
	seen := map[string]bool{}
	for _, line := range strings.Split(lsRemote, "\n") {
		fields := strings.Split(strings.TrimRight(line, "\r"), "\t")
		if len(fields) != 2 {
			continue
		}
		ref := fields[1]
		if ref == keep || seen[ref] || !strings.HasPrefix(ref, "refs/heads/") ||
			len(ref) == len("refs/heads/") || len(ref) > 1024 ||
			strings.ContainsAny(ref, ":*?[\\^~ \t") || strings.Contains(ref, "..") ||
			strings.HasSuffix(ref, ".lock") || strings.HasSuffix(ref, "/") {
			continue
		}
		bad := false
		for _, char := range ref {
			if char < ' ' || char == 0x7f {
				bad = true
				break
			}
		}
		if bad {
			continue
		}
		seen[ref] = true
		deletes = append(deletes, ":"+ref)
		if len(deletes) == hostedPushMaxDeletes {
			break
		}
	}
	return deletes
}

func (r *hostedReplicator) now() time.Time {
	if r.clock != nil {
		return r.clock()
	}
	return time.Now()
}

func validHostedPushHosts(hosts []string) error {
	for _, host := range hosts {
		if host == "" || strings.ContainsAny(host, "/:@ ") || strings.ToLower(host) != host {
			return errors.New("hosted.pushHosts must be lower-case host names")
		}
	}
	return nil
}
