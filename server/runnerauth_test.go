package mirrornode

import (
	"encoding/base64"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"slices"
	"strings"
	"syscall"
	"testing"
	"time"
)

func claudeCredential(access, refresh string, accessExpiry, refreshExpiry time.Time) string {
	oauth := map[string]any{"accessToken": access}
	if refresh != "" {
		oauth["refreshToken"] = refresh
	}
	if !accessExpiry.IsZero() {
		oauth["expiresAt"] = accessExpiry.UnixMilli()
	}
	if !refreshExpiry.IsZero() {
		oauth["refreshTokenExpiresAt"] = refreshExpiry.UnixMilli()
	}
	raw, _ := json.Marshal(map[string]any{"claudeAiOauth": oauth})
	return string(raw)
}

func fakeJWT(exp time.Time) string {
	claims, _ := json.Marshal(map[string]any{"exp": exp.Unix()})
	return "eyJhbGciOiJub25lIn0." + base64.RawURLEncoding.EncodeToString(claims) + ".sig"
}

func authTestRunner(t *testing.T, home, dir string, now time.Time) *agentJobRunner {
	t.Helper()
	return &agentJobRunner{
		dir:      dir,
		lookPath: gemmaLookPath("claude"),
		now:      func() time.Time { return now },
		getenv: func(name string) string {
			if name == "HOME" {
				return home
			}
			return ""
		},
	}
}

func writeClaudeCredential(t *testing.T, home, body string) string {
	t.Helper()
	path := filepath.Join(home, ".claude", ".credentials.json")
	if err := os.MkdirAll(filepath.Dir(path), 0o700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, []byte(body), 0o600); err != nil {
		t.Fatal(err)
	}
	return path
}

func TestProviderAuthFailureClassification(t *testing.T) {
	for _, text := range []string{
		"Claude Code exited with status 1: Failed to authenticate: OAuth session expired and could not be refreshed",
		"Claude Code exited with status 1: API Error: 401 {\"type\":\"error\",\"error\":{\"type\":\"authentication_error\"}}",
		"Claude Code exited with status 1: Invalid API key · Please run /login",
		"Codex exited with status 1: Your access token could not be refreshed because your refresh token was already used. Please log out and sign in again.",
		"Codex exited with status 1: refresh_token_reused",
		"Codex exited with status 1: Provided authentication token is expired. Please try signing in again.",
	} {
		if !providerAuthFailed(text) {
			t.Errorf("not classified as an expired login: %q", text)
		}
	}
	for _, text := range []string{
		"Claude Code exited with status 1: API Error: 529 overloaded",
		"Claude Code ran past the session time limit",
		"Codex exited with status 2: error: unexpected argument",
		"publish branch: git push: remote rejected (HTTP 403)",
		"The agent changed 401 lines in three files.",
	} {
		if providerAuthFailed(text) {
			t.Errorf("an ordinary failure was classified as an expired login: %q", text)
		}
	}
}

func TestInspectCredentialStates(t *testing.T) {
	now := time.Date(2026, 9, 29, 21, 40, 0, 0, time.UTC)
	cases := []struct {
		name, body, state, reason string
		codex                     bool
	}{
		{"valid with refresh token", claudeCredential("a", "r", now.Add(-time.Hour), now.Add(20*24*time.Hour)), "ok", "", false},
		{"expired access token is refreshed by the CLI", claudeCredential("a", "r", now.Add(-48*time.Hour), time.Time{}), "ok", "", false},
		{"refresh token expired", claudeCredential("a", "r", now.Add(-time.Hour), now.Add(-time.Minute)), "expired", "refresh_expired", false},
		{"refresh token near expiry", claudeCredential("a", "r", now.Add(time.Hour), now.Add(6*time.Hour)), "expiring", "refresh_expiring", false},
		{"no refresh token, access expired", claudeCredential("a", "", now.Add(-time.Minute), time.Time{}), "expired", "no_refresh_token", false},
		{"no refresh token, access near expiry", claudeCredential("a", "", now.Add(3*time.Hour), time.Time{}), "expiring", "no_refresh_token", false},
		{"no refresh token, long-lived access", claudeCredential("a", "", now.Add(300*24*time.Hour), time.Time{}), "ok", "", false},
		{"empty login", `{"claudeAiOauth":{}}`, "missing", "", false},
		{"tokens cleared by the CLI after invalid_grant", `{"claudeAiOauth":{"accessToken":"","refreshToken":"","expiresAt":0,"subscriptionType":"max"}}`, "expired", "refresh_revoked", false},
		{"not json", `nope`, "missing", "", false},
		{"codex device login", fmt.Sprintf(`{"tokens":{"access_token":%q,"refresh_token":"r"}}`, fakeJWT(now.Add(-time.Hour))), "ok", "", true},
		{"codex without refresh token", fmt.Sprintf(`{"tokens":{"access_token":%q}}`, fakeJWT(now.Add(-time.Hour))), "expired", "no_refresh_token", true},
		{"codex without refresh token, near expiry", fmt.Sprintf(`{"tokens":{"access_token":%q}}`, fakeJWT(now.Add(2*time.Hour))), "expiring", "no_refresh_token", true},
		{"codex api key", `{"OPENAI_API_KEY":"sk-test"}`, "ok", "", true},
	}
	for _, c := range cases {
		got := inspectCredential(c.codex, []byte(c.body), now)
		if got.State != c.state || got.Reason != c.reason {
			t.Errorf("%s: got %s/%s, want %s/%s", c.name, got.State, got.Reason, c.state, c.reason)
		}
	}
	near := inspectCredential(false, []byte(claudeCredential("a", "r", now, now.Add(6*time.Hour))), now)
	if near.ExpiresAt != now.Add(6*time.Hour).UnixMilli() || !near.HasRefresh {
		t.Fatalf("near-expiry details lost: %+v", near)
	}
	if message := authMessage("Claude Code", near); !strings.Contains(message, "2026-09-30 03:40 UTC") {
		t.Fatalf("the warning does not name the expiry: %q", message)
	}
}

func TestAuthStatusProactiveCheck(t *testing.T) {
	now := time.Date(2026, 9, 29, 21, 40, 0, 0, time.UTC)
	home := t.TempDir()
	runner := authTestRunner(t, home, "", now)
	if got := runner.authStatus("claude-code"); got.State != "missing" {
		t.Fatalf("no credential file reads %s", got.State)
	}
	writeClaudeCredential(t, home, claudeCredential("a", "r", now.Add(time.Hour), now.Add(12*time.Hour)))
	if got := runner.authStatus("claude-code"); got.State != "expiring" || got.CheckedAt != now.UnixMilli() {
		t.Fatalf("a login that expires in 12 hours reads %+v", got)
	}
	if got := runner.availableProviders(); !slices.Equal(got, []string{"claude-code"}) {
		t.Fatalf("an expiring login must still take jobs: %v", got)
	}
	writeClaudeCredential(t, home, claudeCredential("a", "r", now.Add(-time.Hour), now.Add(-time.Hour)))
	status := runner.authSnapshot()["claude-code"]
	if status.State != "expired" || status.Reason != "refresh_expired" {
		t.Fatalf("an expired refresh token reads %+v", status)
	}
	if runner.availability("claude-code")["loginState"] != "expired" || len(runner.availableProviders()) != 0 {
		t.Fatal("a runner with an expired login still offers Claude Code")
	}
	if got := runner.authSnapshot()["codex"]; got.State != "missing" {
		t.Fatalf("codex without a login reads %+v", got)
	}
	writeClaudeCredential(t, home, `{"claudeAiOauth":{"accessToken":"","refreshToken":"","expiresAt":0}}`)
	status = runner.authStatus("claude-code")
	if status.State != "expired" || status.Reason != "refresh_revoked" || !strings.Contains(status.Message, "Reconnect") {
		t.Fatalf("a login the CLI cleared reads %+v", status)
	}
	if runner.availability("claude-code")["loginState"] != "expired" || len(runner.availableProviders()) != 0 {
		t.Fatal("a cleared login must read expired, not missing, and take no jobs")
	}
}

func TestNeedsLoginSurvivesRestartAndOwnRewrites(t *testing.T) {
	now := time.Date(2026, 9, 29, 21, 40, 0, 0, time.UTC)
	home := t.TempDir()
	dir := t.TempDir()
	stale := claudeCredential("a1", "r1", now.Add(-19*time.Hour), now.Add(26*24*time.Hour))
	creds := writeClaudeCredential(t, home, stale)
	runner := authTestRunner(t, home, dir, now)
	if runner.authStatus("claude-code").State != "ok" {
		t.Fatal("a stored login with a refresh token should read ok before any failure")
	}
	if runner.recordAuthFailure("claude-code", 198, "Claude Code exited with status 1: The agent is done.") {
		t.Fatal("an ordinary failure put the runner into needs_login")
	}
	if !runner.recordAuthFailure("claude-code", 198,
		"Claude Code exited with status 1: Failed to authenticate: OAuth session expired and could not be refreshed") {
		t.Fatal("the expired login was not recorded")
	}
	status := runner.authStatus("claude-code")
	if status.State != "expired" || status.Reason != "job_failed" || status.FailedJob != 198 {
		t.Fatalf("needs_login not reported: %+v", status)
	}
	if !strings.Contains(status.Message, "Reconnect") {
		t.Fatalf("the message does not say what to do: %q", status.Message)
	}

	later := now.Add(time.Minute)
	if err := os.WriteFile(creds, []byte(strings.ReplaceAll(stale, `","`, `", "`)), 0o600); err != nil {
		t.Fatal(err)
	}
	if err := os.Chtimes(creds, later, later); err != nil {
		t.Fatal(err)
	}
	if runner.authStatus("claude-code").State != "expired" {
		t.Fatal("rewriting the same tokens cleared needs_login")
	}

	restarted := authTestRunner(t, home, dir, now.Add(time.Hour))
	if restarted.authStatus("claude-code").State != "expired" || len(restarted.availableProviders()) != 0 {
		t.Fatal("needs_login did not survive a daemon restart")
	}

	writeClaudeCredential(t, home, claudeCredential("a2", "r2", now.Add(8*time.Hour), now.Add(30*24*time.Hour)))
	if got := restarted.authStatus("claude-code"); got.State != "ok" {
		t.Fatalf("a new login did not clear needs_login: %+v", got)
	}
	if !slices.Equal(restarted.availableProviders(), []string{"claude-code"}) {
		t.Fatal("a reconnected runner does not take Claude Code jobs again")
	}
	raw, _ := os.ReadFile(filepath.Join(dir, runnerAuthFileName))
	if strings.Contains(string(raw), "claude-code") {
		t.Fatalf("the cleared failure is still persisted: %s", raw)
	}
}

func TestNeedsLoginSeededFromJobHistory(t *testing.T) {
	now := time.Date(2026, 9, 29, 21, 40, 0, 0, time.UTC)
	home := t.TempDir()
	dir := t.TempDir()
	creds := writeClaudeCredential(t, home, claudeCredential("a", "r", now.Add(-40*time.Hour), now.Add(26*24*time.Hour)))
	failedAt := time.Date(2026, 9, 28, 23, 6, 9, 0, time.UTC)
	if err := os.Chtimes(creds, failedAt.Add(time.Second), failedAt.Add(time.Second)); err != nil {
		t.Fatal(err)
	}
	state := agentJobState{Jobs: map[string]*agentJobRecord{
		"189": {JobID: 189, Provider: "claude-code", Status: "completed", UpdatedAt: failedAt.Add(-20 * time.Hour).UnixMilli()},
		"198": {JobID: 198, Provider: "claude-code", Status: "failed", UpdatedAt: failedAt.UnixMilli(),
			Result: "Failed to authenticate: OAuth session expired and could not be refreshed"},
	}}
	if err := writePrivateJSON(filepath.Join(dir, "state.json"), state); err != nil {
		t.Fatal(err)
	}
	runner := authTestRunner(t, home, dir, now)
	status := runner.authStatus("claude-code")
	if status.State != "expired" || status.FailedJob != 198 {
		t.Fatalf("the last job's expired login was not picked up at startup: %+v", status)
	}

	fresh := t.TempDir()
	if err := writePrivateJSON(filepath.Join(fresh, "state.json"), state); err != nil {
		t.Fatal(err)
	}
	relogged := failedAt.Add(time.Hour)
	if err := os.Chtimes(creds, relogged, relogged); err != nil {
		t.Fatal(err)
	}
	if got := authTestRunner(t, home, fresh, now).authStatus("claude-code"); got.State != "ok" {
		t.Fatalf("a login newer than the failed job was treated as expired: %+v", got)
	}
}

func TestExpiredLoginFailsJobWithAuthKind(t *testing.T) {
	now := time.Date(2026, 9, 29, 21, 40, 0, 0, time.UTC)
	home := t.TempDir()
	writeClaudeCredential(t, home, claudeCredential("a", "r", now.Add(-time.Hour), now.Add(-time.Minute)))
	runner := authTestRunner(t, home, t.TempDir(), now)
	availability := runner.availability("claude-code")
	if availability["loginState"] != "expired" || !strings.Contains(fmt.Sprint(availability["message"]), "Reconnect") {
		t.Fatalf("unexpected availability %v", availability)
	}
}

func TestCopyBackNeverRevertsARefreshedLogin(t *testing.T) {
	now := time.Now()
	persistent := t.TempDir()
	work := t.TempDir()
	box := jobSandbox{workRoot: work, home: persistent}
	original := claudeCredential("a1", "r1", now.Add(-time.Minute), time.Time{})
	creds := writeClaudeCredential(t, persistent, original)
	home := box.homeFor(filepath.Join(work, "sess"))
	if err := box.openSession(home); err != nil {
		t.Fatal(err)
	}
	refreshed := claudeCredential("a2", "r2", now.Add(8*time.Hour), time.Time{})
	if err := os.WriteFile(creds, []byte(refreshed), 0o600); err != nil {
		t.Fatal(err)
	}
	box.closeSession(home)
	if got := readTestFile(t, creds); got != refreshed {
		t.Fatalf("the untouched snapshot replaced the login the CLI refreshed in place: %s", got)
	}

	if err := box.openSession(home); err != nil {
		t.Fatal(err)
	}
	copied := filepath.Join(home, ".claude", ".credentials.json")
	ownRefresh := claudeCredential("a3", "r3", now.Add(9*time.Hour), time.Time{})
	writeTestFile(t, copied, ownRefresh)
	box.closeSession(home)
	if got := readTestFile(t, creds); got != ownRefresh {
		t.Fatalf("the job's own refresh was not kept: %s", got)
	}

	if err := box.openSession(home); err != nil {
		t.Fatal(err)
	}
	older := claudeCredential("a4", "r4", now.Add(7*time.Hour), time.Time{})
	newer := claudeCredential("a5", "r5", now.Add(10*time.Hour), time.Time{})
	writeTestFile(t, copied, older)
	if err := os.WriteFile(creds, []byte(newer), 0o600); err != nil {
		t.Fatal(err)
	}
	box.closeSession(home)
	if got := readTestFile(t, creds); got != newer {
		t.Fatalf("an older session refresh replaced a newer login: %s", got)
	}

	if err := box.openSession(home); err != nil {
		t.Fatal(err)
	}
	newest := claudeCredential("a6", "r6", now.Add(11*time.Hour), time.Time{})
	concurrent := claudeCredential("a7", "r7", now.Add(8*time.Hour+30*time.Minute), time.Time{})
	writeTestFile(t, copied, newest)
	if err := os.WriteFile(creds, []byte(concurrent), 0o600); err != nil {
		t.Fatal(err)
	}
	box.closeSession(home)
	if got := readTestFile(t, creds); got != newest {
		t.Fatalf("a newer session refresh lost to an older concurrent one: %s", got)
	}
}

func TestRunnerCredentialReadRefusesJobPlantedLinksAndFifos(t *testing.T) {
	uid := os.Getuid()
	if uid == 0 {
		t.Skip("needs a non-root test user")
	}
	now := time.Now()
	home, outside := t.TempDir(), t.TempDir()
	r := authTestRunner(t, home, t.TempDir(), now)
	r.credentialUID = uid
	body := claudeCredential("access", "refresh", now.Add(time.Hour), now.Add(48*time.Hour))
	path := writeClaudeCredential(t, home, body)
	if source, _ := r.credential(false); source != "device login" {
		t.Fatalf("plain credential source = %q", source)
	}
	secret := filepath.Join(outside, "node.key")
	if err := os.WriteFile(secret, []byte(body), 0o600); err != nil {
		t.Fatal(err)
	}
	if err := os.Remove(path); err != nil {
		t.Fatal(err)
	}
	if err := os.Symlink(secret, path); err != nil {
		t.Fatal(err)
	}
	if source, _ := r.credential(false); source != "" {
		t.Fatalf("symlinked credential was followed: source %q", source)
	}
	if state := r.authStatus("claude-code").State; state != "missing" {
		t.Fatalf("symlinked credential inspected: state %q", state)
	}
	if err := os.Remove(path); err != nil {
		t.Fatal(err)
	}
	if err := syscall.Mkfifo(path, 0o600); err != nil {
		t.Fatal(err)
	}
	done := make(chan string, 1)
	go func() {
		source, _ := r.credential(false)
		done <- source
	}()
	select {
	case source := <-done:
		if source != "" {
			t.Fatalf("fifo credential source = %q", source)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("reading a job-planted fifo blocked the runner")
	}
}
