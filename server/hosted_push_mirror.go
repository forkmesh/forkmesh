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

func validHostedPushMirror(mirror hostedPushMirror, hosts []string) bool {
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

func (r *hostedReplicator) pushMirrorDue(id, revision, digest, allRefs string, now time.Time) bool {
	r.mu.Lock()
	defer r.mu.Unlock()
	state, known := r.pushState[id]
	if !known || state.report.Revision != revision {
		return true
	}
	if !state.report.OK {
		return now.Sub(state.attempted) >= hostedPushRetryAfter
	}
	return digest != state.successDigest || allRefs != state.successAllRefs ||
		now.Sub(state.succeeded) >= hostedPushRefreshAfter
}

func (r *hostedReplicator) recordPush(id string, report hostedPushReport, allRefs string, now time.Time) {
	r.mu.Lock()
	defer r.mu.Unlock()
	state := r.pushState[id]
	state.report = report
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
	if !r.pushMirrorDue(item.ID, mirror.Revision, digest, allRefs, now) {
		return false
	}
	args := []string{"push", "--porcelain"}
	if mirror.Prune {
		args = append(args, "--prune")
	}
	args = append(args, mirror.URL, "+refs/heads/*:refs/heads/*", "+refs/tags/*:refs/tags/*")
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
	stdout, stderr, err := runner(ctx, gitDir, args, env)
	report := hostedPushReport{Revision: mirror.Revision, Digest: digest, OK: err == nil,
		At: r.now().UnixMilli(), Refs: hostedPushRefCount(stdout)}
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
	r.recordPush(item.ID, report, allRefs, now)
	return true
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
