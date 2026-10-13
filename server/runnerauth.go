package mirrornode

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"log"
	"os"
	"path/filepath"
	"strings"
	"time"
)

const (
	runnerAuthWarnBefore = 24 * time.Hour
	runnerAuthFileName   = "auth.json"
	runnerAuthSeedWindow = 2 * time.Minute
	authFailureKind      = "auth_expired"
	loginMissingKind     = "login_missing"
)

var runnerAuthProviders = []string{"claude-code", "codex"}

type runnerAuthFailure struct {
	Fingerprint string `json:"fingerprint"`
	FailedAt    int64  `json:"failedAt"`
	JobID       int64  `json:"jobId,omitempty"`
	Detail      string `json:"detail,omitempty"`
}

type providerAuth struct {
	State           string `json:"state"`
	CheckedAt       int64  `json:"checkedAt"`
	ExpiresAt       int64  `json:"expiresAt,omitempty"`
	AccessExpiresAt int64  `json:"accessExpiresAt,omitempty"`
	HasRefresh      bool   `json:"hasRefresh"`
	Source          string `json:"source,omitempty"`
	Reason          string `json:"reason,omitempty"`
	Message         string `json:"message,omitempty"`
	FailedAt        int64  `json:"failedAt,omitempty"`
	FailedJob       int64  `json:"failedJob,omitempty"`
}

type claudeCredentialFile struct {
	OAuth struct {
		AccessToken           string `json:"accessToken"`
		RefreshToken          string `json:"refreshToken"`
		ExpiresAt             int64  `json:"expiresAt"`
		RefreshTokenExpiresAt int64  `json:"refreshTokenExpiresAt"`
	} `json:"claudeAiOauth"`
}

type codexCredentialFile struct {
	APIKey      string `json:"OPENAI_API_KEY"`
	AccessToken string `json:"access_token"`
	Tokens      struct {
		AccessToken  string `json:"access_token"`
		RefreshToken string `json:"refresh_token"`
		IDToken      string `json:"id_token"`
	} `json:"tokens"`
}

func providerLabel(provider string) string {
	if provider == "codex" {
		return "Codex"
	}
	return "Claude Code"
}

func normalAuthProvider(provider string) string {
	switch provider {
	case "codex":
		return "codex"
	case "local-llama", "":
		return ""
	}
	return "claude-code"
}

func (r *agentJobRunner) clock() time.Time {
	if r.now != nil {
		return r.now()
	}
	return time.Now()
}

func (r *agentJobRunner) credentialPath(codex bool) string {
	home := r.getenv("HOME")
	if home == "" {
		home, _ = os.UserHomeDir()
	}
	if codex {
		dir := r.getenv("CODEX_HOME")
		if dir == "" {
			dir = filepath.Join(home, ".codex")
		}
		return filepath.Join(dir, "auth.json")
	}
	dir := r.getenv("CLAUDE_CONFIG_DIR")
	if dir == "" {
		dir = filepath.Join(home, ".claude")
	}
	return filepath.Join(dir, ".credentials.json")
}

func (r *agentJobRunner) credential(codex bool) (string, string) {
	if codex {
		if strings.TrimSpace(r.getenv("OPENAI_API_KEY")) != "" {
			return "OPENAI_API_KEY", "OPENAI_API_KEY"
		}
	} else {
		for _, name := range []string{"CLAUDE_CODE_OAUTH_TOKEN", "ANTHROPIC_AUTH_TOKEN", "ANTHROPIC_API_KEY"} {
			if strings.TrimSpace(r.getenv(name)) != "" {
				return name, name
			}
		}
	}
	raw, err := r.readCredential(codex)
	if err != nil {
		return "", ""
	}
	if fingerprint := credentialFingerprint(codex, raw); fingerprint != "" {
		return "device login", fingerprint
	}
	return "", ""
}

func (r *agentJobRunner) readCredential(codex bool) ([]byte, error) {
	if r.credentialUID != 0 {
		return readOwnedFile(r.credentialPath(codex), r.credentialUID, jobCredentialLimit)
	}
	return os.ReadFile(r.credentialPath(codex))
}

func credentialFingerprint(codex bool, rawBytes []byte) string {
	var parts []string
	if codex {
		var auth codexCredentialFile
		if json.Unmarshal(rawBytes, &auth) != nil {
			return ""
		}
		var raw struct {
			Tokens map[string]any `json:"tokens"`
		}
		_ = json.Unmarshal(rawBytes, &raw)
		if len(raw.Tokens) == 0 && auth.AccessToken == "" && strings.TrimSpace(auth.APIKey) == "" {
			return ""
		}
		parts = []string{auth.APIKey, auth.AccessToken, auth.Tokens.AccessToken, auth.Tokens.RefreshToken, auth.Tokens.IDToken}
	} else {
		var credentials claudeCredentialFile
		if json.Unmarshal(rawBytes, &credentials) != nil ||
			(credentials.OAuth.AccessToken == "" && credentials.OAuth.RefreshToken == "") {
			return ""
		}
		parts = []string{credentials.OAuth.AccessToken, credentials.OAuth.RefreshToken}
	}
	digest := sha256.Sum256([]byte(strings.Join(parts, "\x00")))
	return "tokens:" + hex.EncodeToString(digest[:16])
}

func inspectCredential(codex bool, raw []byte, now time.Time) providerAuth {
	status := providerAuth{State: "missing", CheckedAt: now.UnixMilli(), Source: "device login"}
	accessExpiry, refreshExpiry := int64(0), int64(0)
	hasAccess, hasRefresh := false, false
	if codex {
		var auth codexCredentialFile
		if json.Unmarshal(raw, &auth) != nil {
			return status
		}
		if strings.TrimSpace(auth.APIKey) != "" && auth.Tokens.AccessToken == "" && auth.Tokens.RefreshToken == "" {
			status.State = "ok"
			return status
		}
		access := firstNonEmpty(auth.Tokens.AccessToken, auth.AccessToken)
		hasAccess, hasRefresh = access != "", auth.Tokens.RefreshToken != ""
		if exp, ok := jwtClaims(access)["exp"].(float64); ok {
			accessExpiry = int64(exp) * 1000
		}
	} else {
		var credentials claudeCredentialFile
		if json.Unmarshal(raw, &credentials) != nil {
			return status
		}
		hasAccess, hasRefresh = credentials.OAuth.AccessToken != "", credentials.OAuth.RefreshToken != ""
		accessExpiry, refreshExpiry = credentials.OAuth.ExpiresAt, credentials.OAuth.RefreshTokenExpiresAt
		if !hasAccess && !hasRefresh && claudeLoginCleared(raw) {
			status.State, status.Reason = "expired", "refresh_revoked"
			return status
		}
	}
	if !hasAccess && !hasRefresh {
		return status
	}
	status.State, status.HasRefresh, status.AccessExpiresAt = "ok", hasRefresh, accessExpiry
	hard := refreshExpiry
	reasonExpired, reasonExpiring := "refresh_expired", "refresh_expiring"
	if !hasRefresh {
		hard = accessExpiry
		reasonExpired, reasonExpiring = "no_refresh_token", "no_refresh_token"
	}
	if hard == 0 {
		return status
	}
	status.ExpiresAt = hard
	switch left := time.UnixMilli(hard).Sub(now); {
	case left <= 0:
		status.State, status.Reason = "expired", reasonExpired
	case left <= runnerAuthWarnBefore:
		status.State, status.Reason = "expiring", reasonExpiring
	}
	return status
}

func claudeLoginCleared(raw []byte) bool {
	var file struct {
		OAuth map[string]json.RawMessage `json:"claudeAiOauth"`
	}
	if json.Unmarshal(raw, &file) != nil || file.OAuth == nil {
		return false
	}
	_, hasRefresh := file.OAuth["refreshToken"]
	return hasRefresh
}

func authMessage(label string, status providerAuth) string {
	at := ""
	if status.ExpiresAt > 0 {
		at = time.UnixMilli(status.ExpiresAt).UTC().Format("2006-01-02 15:04 UTC")
	}
	switch status.State {
	case "missing":
		return label + " is not signed in on this node. Connect it from the Runners page."
	case "expired":
		if status.Reason == "job_failed" {
			return label + " login on this node expired and could not be refreshed. Reconnect it from the Runners page."
		}
		if status.Reason == "refresh_revoked" {
			return label + " login on this node was revoked: its refresh token had already been used. Reconnect it from the Runners page."
		}
		if at != "" {
			return label + " login on this node expired at " + at + ". Reconnect it from the Runners page."
		}
		return label + " login on this node has expired. Reconnect it from the Runners page."
	case "expiring":
		return label + " login on this node expires at " + at + ". Reconnect it from the Runners page before then."
	}
	return label + " login on this node is valid."
}

func (r *agentJobRunner) authPath() string {
	if r.dir == "" {
		return ""
	}
	return filepath.Join(r.dir, runnerAuthFileName)
}

func (r *agentJobRunner) loadAuthLocked() {
	if r.authLoaded {
		return
	}
	r.authLoaded = true
	if r.authFailed == nil {
		r.authFailed = map[string]runnerAuthFailure{}
	}
	path := r.authPath()
	if path == "" {
		return
	}
	stored := map[string]runnerAuthFailure{}
	if raw, err := os.ReadFile(path); err == nil && json.Unmarshal(raw, &stored) == nil {
		for provider, failure := range stored {
			if normalAuthProvider(provider) == provider && failure.Fingerprint != "" {
				r.authFailed[provider] = failure
			}
		}
		return
	}
	if r.seedAuthFailuresLocked() {
		r.saveAuthLocked()
	}
}

func (r *agentJobRunner) saveAuthLocked() {
	path := r.authPath()
	if path == "" {
		return
	}
	if err := os.MkdirAll(r.dir, 0o700); err != nil {
		return
	}
	_ = writePrivateJSON(path, r.authFailed)
}

func (r *agentJobRunner) seedAuthFailuresLocked() bool {
	raw, err := os.ReadFile(r.statePath())
	if err != nil {
		return false
	}
	var state agentJobState
	if json.Unmarshal(raw, &state) != nil {
		return false
	}
	latest := map[string]*agentJobRecord{}
	for _, record := range state.Jobs {
		provider := normalAuthProvider(record.Provider)
		if provider == "" || (record.Status != "failed" && record.Status != "completed") {
			continue
		}
		if current := latest[provider]; current == nil || record.UpdatedAt > current.UpdatedAt {
			latest[provider] = record
		}
	}
	seeded := false
	for provider, record := range latest {
		if record.Status != "failed" || !providerAuthFailed(record.Result, record.Reason) {
			continue
		}
		codex := provider == "codex"
		source, stamp := r.credential(codex)
		if source != "device login" {
			continue
		}
		info, err := os.Stat(r.credentialPath(codex))
		if err != nil || info.ModTime().After(time.UnixMilli(record.UpdatedAt).Add(runnerAuthSeedWindow)) {
			continue
		}
		r.authFailed[provider] = runnerAuthFailure{Fingerprint: stamp, FailedAt: record.UpdatedAt,
			JobID: record.JobID, Detail: boundedRunes(firstNonEmpty(record.Result, record.Reason), 240)}
		seeded = true
	}
	return seeded
}

func (r *agentJobRunner) authFailure(provider, stamp string) (runnerAuthFailure, bool) {
	r.authMu.Lock()
	defer r.authMu.Unlock()
	r.loadAuthLocked()
	failure, ok := r.authFailed[provider]
	if ok && failure.Fingerprint != stamp {
		delete(r.authFailed, provider)
		r.saveAuthLocked()
		return runnerAuthFailure{}, false
	}
	return failure, ok
}

func (r *agentJobRunner) authExpired(provider, stamp string) bool {
	_, failed := r.authFailure(provider, stamp)
	return failed
}

func (r *agentJobRunner) noteAuthFailure(provider string, texts ...string) bool {
	return r.recordAuthFailure(provider, 0, texts...)
}

func (r *agentJobRunner) recordAuthFailure(provider string, jobID int64, texts ...string) bool {
	if !providerAuthFailed(texts...) {
		return false
	}
	provider = normalAuthProvider(provider)
	if provider == "" {
		return false
	}
	source, stamp := r.credential(provider == "codex")
	if source == "" {
		return false
	}
	r.authMu.Lock()
	r.loadAuthLocked()
	r.authFailed[provider] = runnerAuthFailure{Fingerprint: stamp, FailedAt: r.clock().UnixMilli(),
		JobID: jobID, Detail: boundedRunes(strings.Join(texts, " | "), 240)}
	r.saveAuthLocked()
	r.authMu.Unlock()
	return true
}

func (r *agentJobRunner) authStatus(provider string) providerAuth {
	provider = normalAuthProvider(provider)
	codex := provider == "codex"
	now := r.clock()
	status := providerAuth{State: "missing", CheckedAt: now.UnixMilli()}
	source, stamp := r.credential(codex)
	switch source {
	case "", "device login":
		if raw, err := r.readCredential(codex); err == nil {
			status = inspectCredential(codex, raw, now)
		}
	default:
		status = providerAuth{State: "ok", CheckedAt: now.UnixMilli(), Source: source}
	}
	if source != "" {
		if failure, failed := r.authFailure(provider, stamp); failed {
			status.State, status.Reason = "expired", "job_failed"
			status.FailedAt, status.FailedJob = failure.FailedAt, failure.JobID
		}
	}
	status.Message = authMessage(providerLabel(provider), status)
	return status
}

func (r *agentJobRunner) authSnapshot() map[string]providerAuth {
	snapshot := map[string]providerAuth{}
	for _, provider := range runnerAuthProviders {
		status := r.authStatus(provider)
		snapshot[provider] = status
		r.authMu.Lock()
		if r.authSeen == nil {
			r.authSeen = map[string]string{}
		}
		previous, seen := r.authSeen[provider]
		r.authSeen[provider] = status.State
		r.authMu.Unlock()
		if (seen && previous != status.State) || (!seen && (status.State == "expired" || status.State == "expiring")) {
			log.Printf("runner: %s login is %s: %s", provider, status.State, status.Message)
		}
	}
	return snapshot
}

var providerAuthFailures = []string{
	"failed to authenticate",
	"oauth session expired",
	"oauth token has expired",
	"oauth token revoked",
	"could not be refreshed",
	"invalid api key",
	"invalid bearer token",
	"invalid x-api-key",
	"please run /login",
	"authentication_error",
	"api error: 401",
	"401 unauthorized",
	"status 401",
	"invalid_grant",
	"invalid credentials",
	"not logged in",
	"refresh_token_reused",
	"refresh token was already used",
	"access token could not be refreshed",
	"provided authentication token is expired",
	"please log out and sign in again",
	"run `codex login`",
	"run codex login",
	"token_expired",
}

func providerAuthFailed(texts ...string) bool {
	joined := strings.ToLower(strings.Join(texts, "\n"))
	for _, marker := range providerAuthFailures {
		if strings.Contains(joined, marker) {
			return true
		}
	}
	return false
}
