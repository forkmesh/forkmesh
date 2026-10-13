package mirrornode

import (
	"context"
	"encoding/base64"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func writeTestFile(t *testing.T, path, body string) {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(path), 0o700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, []byte(body), 0o600); err != nil {
		t.Fatal(err)
	}
}

func readTestFile(t *testing.T, path string) string {
	t.Helper()
	raw, err := os.ReadFile(path)
	if err != nil {
		return ""
	}
	return string(raw)
}

func claudeLogin(t *testing.T, home, token, uuid, email string) {
	t.Helper()
	writeTestFile(t, filepath.Join(home, ".claude", ".credentials.json"),
		`{"claudeAiOauth":{"accessToken":"`+token+`","refreshToken":"r-`+token+`","subscriptionType":"max"}}`)
	writeTestFile(t, filepath.Join(home, ".claude.json"),
		`{"oauthAccount":{"accountUuid":"`+uuid+`","emailAddress":"`+email+`","organizationUuid":"org"}}`)
}

func accountIDs(t *testing.T, a *runnerAccounts, provider string) (string, []string, []string) {
	t.Helper()
	state := a.load(provider)
	ids, labels := []string{}, []string{}
	for _, account := range state.Accounts {
		ids = append(ids, account.ID)
		labels = append(labels, account.Label)
	}
	return state.Active, ids, labels
}

func TestRunnerAccountsSignInSwitchAndRemove(t *testing.T) {
	home, work := t.TempDir(), t.TempDir()
	accounts := newRunnerAccounts(filepath.Join(work, runnerAccountsDir), home, 0, 0)
	box := jobSandbox{workRoot: work, home: home, accounts: accounts}
	active := filepath.Join(home, ".claude", ".credentials.json")

	claudeLogin(t, home, "one", "u1", "one@example.com")
	accounts.sync()
	first, ids, labels := accountIDs(t, accounts, "claude-code")
	if len(ids) != 1 || first != ids[0] || labels[0] != "one@example.com" {
		t.Fatalf("the existing login was not imported: %v %v %q", ids, labels, first)
	}
	if info, err := os.Stat(accounts.root); err != nil || info.Mode().Perm() != 0o700 {
		t.Fatalf("account store is not private: %v %v", info, err)
	}

	accounts.beginLogin("claude-code")
	claudeLogin(t, home, "two", "u2", "two@example.com")
	accounts.sync()
	if _, ids, _ := accountIDs(t, accounts, "claude-code"); len(ids) != 1 {
		t.Fatalf("sync adopted a login that was still running: %v", ids)
	}
	accounts.endLogin("claude-code", true)
	second, ids, labels := accountIDs(t, accounts, "claude-code")
	if len(ids) != 2 || second == first || labels[1] != "two@example.com" {
		t.Fatalf("a second sign-in did not add an account: %v %v", ids, labels)
	}
	if !strings.Contains(readTestFile(t, filepath.Join(accounts.root, "claude-code", first+".json")), `"one"`) {
		t.Fatal("the first account's login was lost by the second sign-in")
	}

	accounts.beginLogin("claude-code")
	claudeLogin(t, home, "one-again", "u1", "one@example.com")
	accounts.endLogin("claude-code", true)
	now, ids, _ := accountIDs(t, accounts, "claude-code")
	if len(ids) != 2 || now != first {
		t.Fatalf("signing the same account in again did not reuse its slot: %v active=%q", ids, now)
	}

	if err := accounts.activate("claude-code", second); err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(readTestFile(t, active), `"two"`) {
		t.Fatalf("switching did not install the account: %q", readTestFile(t, active))
	}

	session := box.homeFor(filepath.Join(work, "sess"))
	if err := box.openSession(session); err != nil {
		t.Fatal(err)
	}
	copied := filepath.Join(session, ".claude", ".credentials.json")
	if !strings.Contains(readTestFile(t, copied), `"two"`) {
		t.Fatal("the job did not get the active account")
	}
	if err := accounts.activate("claude-code", first); err != nil {
		t.Fatal(err)
	}
	writeTestFile(t, copied, `{"claudeAiOauth":{"accessToken":"two-refreshed"}}`)
	box.closeSession(session)
	if !strings.Contains(readTestFile(t, active), `"one-again"`) {
		t.Fatalf("a job of the old account overwrote the switched login: %q", readTestFile(t, active))
	}
	if !strings.Contains(readTestFile(t, filepath.Join(accounts.root, "claude-code", second+".json")), "two-refreshed") {
		t.Fatal("the refreshed login of the inactive account was not kept")
	}

	if err := accounts.remove("claude-code", first); err != nil {
		t.Fatal(err)
	}
	now, ids, _ = accountIDs(t, accounts, "claude-code")
	if len(ids) != 1 || now != second || !strings.Contains(readTestFile(t, active), "two-refreshed") {
		t.Fatalf("removing the active account did not fall back: %v %q", ids, now)
	}
	if err := accounts.remove("claude-code", second); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(active); !os.IsNotExist(err) {
		t.Fatal("removing the last account left its login in place")
	}
	if err := accounts.activate("claude-code", second); err == nil {
		t.Fatal("a removed account was activated")
	}
}

func TestRunnerAccountsFailedSignInRestoresTheActiveLogin(t *testing.T) {
	home, work := t.TempDir(), t.TempDir()
	accounts := newRunnerAccounts(filepath.Join(work, runnerAccountsDir), home, 0, 0)
	claudeLogin(t, home, "one", "u1", "one@example.com")
	accounts.beginLogin("claude-code")
	_ = os.Remove(filepath.Join(home, ".claude", ".credentials.json"))
	accounts.endLogin("claude-code", false)
	if !strings.Contains(readTestFile(t, filepath.Join(home, ".claude", ".credentials.json")), `"one"`) {
		t.Fatal("a failed sign-in left the runner signed out")
	}
	if _, ids, _ := accountIDs(t, accounts, "claude-code"); len(ids) != 1 {
		t.Fatalf("a failed sign-in added an account: %v", ids)
	}
}

func TestRunnerAccountsRefreshAfterSwitchStaysWithItsAccount(t *testing.T) {
	home, work := t.TempDir(), t.TempDir()
	accounts := newRunnerAccounts(filepath.Join(work, runnerAccountsDir), home, 0, 0)
	active := filepath.Join(home, ".claude", ".credentials.json")
	claudeLogin(t, home, "one", "u1", "one@example.com")
	accounts.sync()
	accounts.beginLogin("claude-code")
	claudeLogin(t, home, "two", "u2", "two@example.com")
	accounts.endLogin("claude-code", true)
	_, ids, _ := accountIDs(t, accounts, "claude-code")
	first, second := ids[0], ids[1]
	if err := accounts.activate("claude-code", first); err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(readTestFile(t, filepath.Join(home, ".claude.json")), "u2") {
		t.Fatal("the test needs .claude.json to still name the second account")
	}

	writeTestFile(t, active, `{"claudeAiOauth":{"accessToken":"one-refreshed","refreshToken":"r-one-refreshed"}}`)
	accounts.sync()
	if now, _, _ := accountIDs(t, accounts, "claude-code"); now != first {
		t.Fatalf("a refresh of the switched-to account moved the runner back to %q", now)
	}
	if !strings.Contains(readTestFile(t, filepath.Join(accounts.root, "claude-code", first+".json")), "one-refreshed") {
		t.Fatal("the refreshed login was not saved to the account that refreshed it")
	}
	if !strings.Contains(readTestFile(t, filepath.Join(accounts.root, "claude-code", second+".json")), `"two"`) {
		t.Fatal("the other account's login was overwritten by a refresh it did not make")
	}

	if err := accounts.activate("claude-code", second); err != nil {
		t.Fatal(err)
	}
	if err := accounts.activate("claude-code", first); err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(readTestFile(t, active), "one-refreshed") {
		t.Fatalf("switching back installed a used refresh token: %q", readTestFile(t, active))
	}

	cleared := `{"claudeAiOauth":{"accessToken":"","refreshToken":"","expiresAt":0}}`
	writeTestFile(t, active, cleared)
	accounts.sync()
	if !strings.Contains(readTestFile(t, filepath.Join(accounts.root, "claude-code", first+".json")), "one-refreshed") {
		t.Fatal("a login the CLI cleared replaced the saved account")
	}
	if now, ids, _ := accountIDs(t, accounts, "claude-code"); now != first || len(ids) != 2 {
		t.Fatalf("a cleared login changed the account list: %v active=%q", ids, now)
	}

	box := jobSandbox{workRoot: work, home: home, accounts: accounts}
	writeTestFile(t, active, `{"claudeAiOauth":{"accessToken":"one-refreshed","refreshToken":"r-one-refreshed"}}`)
	session := box.homeFor(filepath.Join(work, "sess"))
	if err := box.openSession(session); err != nil {
		t.Fatal(err)
	}
	writeTestFile(t, filepath.Join(session, ".claude", ".credentials.json"), cleared)
	box.closeSession(session)
	if !strings.Contains(readTestFile(t, active), "one-refreshed") ||
		!strings.Contains(readTestFile(t, filepath.Join(accounts.root, "claude-code", first+".json")), "one-refreshed") {
		t.Fatal("a job whose CLI cleared its copy wiped the runner's login")
	}

	accounts.beginLogin("claude-code")
	claudeLogin(t, home, "two-again", "u2", "two@example.com")
	accounts.endLogin("claude-code", true)
	if now, ids, _ := accountIDs(t, accounts, "claude-code"); now != second || len(ids) != 2 {
		t.Fatalf("signing the second account in again did not renew its slot: %v active=%q", ids, now)
	}
}

func testJWT(claims map[string]any) string {
	raw, _ := json.Marshal(claims)
	return "eyJhbGciOiJub25lIn0." + base64.RawURLEncoding.EncodeToString(raw) + ".c2ln"
}

func TestRunnerAccountsCodexIdentityAndRequests(t *testing.T) {
	home, work := t.TempDir(), t.TempDir()
	accounts := newRunnerAccounts(filepath.Join(work, runnerAccountsDir), home, 0, 0)
	login := func(email, account string) {
		idToken := testJWT(map[string]any{"email": email,
			"https://api.openai.com/auth": map[string]any{"chatgpt_plan_type": "pro"}})
		writeTestFile(t, filepath.Join(home, ".codex", "auth.json"),
			`{"tokens":{"id_token":"`+idToken+`","access_token":"a-`+account+`","account_id":"`+account+`"}}`)
	}
	login("a@example.com", "acct-a")
	accounts.sync()
	accounts.beginLogin("codex")
	login("b@example.com", "acct-b")
	accounts.endLogin("codex", true)
	first, ids, labels := accountIDs(t, accounts, "codex")
	if len(ids) != 2 || labels[0] != "a@example.com" || labels[1] != "b@example.com" || first != ids[1] {
		t.Fatalf("codex accounts: %v %v active=%q", ids, labels, first)
	}
	if plan := accounts.load("codex").Accounts[0].Plan; plan != "pro" {
		t.Fatalf("plan = %q", plan)
	}
	request := runnerAccountRequest{ID: strings.Repeat("a", 32), Provider: "codex", Action: "activate", Account: ids[0]}
	bad := runnerAccountRequest{ID: strings.Repeat("b", 32), Provider: "codex", Action: "activate", Account: strings.Repeat("c", 32)}
	if !accounts.apply([]runnerAccountRequest{request, bad, {ID: "short"}}) {
		t.Fatal("requests were not applied")
	}
	if active, _, _ := accountIDs(t, accounts, "codex"); active != ids[0] {
		t.Fatal("activate request did not switch")
	}
	results := accounts.pendingResults()
	if len(results) != 2 || !results[0].OK || results[1].OK || results[1].Message == "" {
		t.Fatalf("results = %+v", results)
	}
	if err := accounts.activate("codex", ids[1]); err != nil {
		t.Fatal(err)
	}
	if accounts.apply([]runnerAccountRequest{request}) {
		t.Fatal("a request id was applied twice")
	}
	if active, _, _ := accountIDs(t, accounts, "codex"); active != ids[1] {
		t.Fatal("a replayed request switched the account back")
	}
	accounts.now = func() time.Time { return time.Now().Add(runnerAccountResultKeep + time.Second) }
	if len(accounts.pendingResults()) != 0 {
		t.Fatal("old results are still reported")
	}
	snapshot := accounts.snapshot("codex")
	if len(snapshot) != 2 || snapshot[1]["active"] != true || snapshot[0]["email"] != "a@example.com" {
		t.Fatalf("snapshot = %v", snapshot)
	}
	raw, _ := json.Marshal(snapshot)
	if strings.Contains(string(raw), "a-acct") || strings.Contains(string(raw), "subject") {
		t.Fatalf("the snapshot leaks credential data: %s", raw)
	}
}

func TestRunnerAccountsUsageLimits(t *testing.T) {
	var seen []string
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		seen = append(seen, r.URL.Path+" "+r.Header.Get("Authorization")+" "+r.Header.Get("ChatGPT-Account-Id"))
		switch r.URL.Path {
		case "/claude":
			if r.Header.Get("anthropic-beta") != "oauth-2025-04-20" {
				w.WriteHeader(400)
				return
			}
			_, _ = w.Write([]byte(`{"five_hour":{"utilization":42.5,"resets_at":"2026-09-27T18:00:00+00:00"},` +
				`"seven_day":{"utilization":10,"resets_at":"2026-10-01T00:00:00Z"},"seven_day_opus":null,` +
				`"seven_day_future":{"utilization":3,"resets_at":null},"extra_usage":{"is_enabled":false}}`))
		case "/codex":
			_, _ = w.Write([]byte(`{"plan_type":"plus","rate_limit":{"primary_window":{"used_percent":7,` +
				`"limit_window_seconds":18000,"reset_after_seconds":60,"reset_at":1790000000},` +
				`"secondary_window":{"used_percent":55,"limit_window_seconds":604800,"reset_after_seconds":120}}}`))
		default:
			w.WriteHeader(401)
		}
	}))
	defer server.Close()
	oldClaude, oldCodex := claudeUsageURL, codexUsageURL
	claudeUsageURL, codexUsageURL = server.URL+"/claude", server.URL+"/codex"
	defer func() { claudeUsageURL, codexUsageURL = oldClaude, oldCodex }()

	home, work := t.TempDir(), t.TempDir()
	accounts := newRunnerAccounts(filepath.Join(work, runnerAccountsDir), home, 0, 0)
	now := time.Date(2026, 9, 27, 12, 0, 0, 0, time.UTC)
	accounts.now = func() time.Time { return now }
	writeTestFile(t, filepath.Join(home, ".claude", ".credentials.json"),
		`{"claudeAiOauth":{"accessToken":"live","expiresAt":`+jsonInt(now.Add(time.Hour).UnixMilli())+`}}`)
	writeTestFile(t, filepath.Join(home, ".codex", "auth.json"),
		`{"tokens":{"access_token":"`+testJWT(map[string]any{"exp": now.Add(time.Hour).Unix()})+`","account_id":"acct"}}`)
	accounts.sync()
	accounts.refreshUsageNow(context.Background())

	claude := accounts.snapshot("claude-code")[0]["usage"].(map[string]any)
	windows := claude["windows"].([]runnerUsageWindow)
	if len(windows) != 3 || windows[0].ID != "five_hour" || windows[0].UsedPercent != 42.5 ||
		windows[0].ResetsAt != time.Date(2026, 9, 27, 18, 0, 0, 0, time.UTC).UnixMilli() ||
		windows[1].Label != "Weekly, all models" || windows[2].ID != "seven_day_future" || windows[2].ResetsAt != 0 {
		t.Fatalf("claude windows = %+v", windows)
	}
	codex := accounts.snapshot("codex")[0]["usage"].(map[string]any)
	windows = codex["windows"].([]runnerUsageWindow)
	if codex["plan"] != "plus" || len(windows) != 2 || windows[0].Label != "5-hour session" ||
		windows[0].ResetsAt != 1790000000*1000 || windows[1].Label != "Weekly" ||
		windows[1].ResetsAt != now.Add(2*time.Minute).UnixMilli() {
		t.Fatalf("codex usage = %+v", codex)
	}
	if len(seen) != 2 || !strings.Contains(seen[1], "acct") {
		t.Fatalf("requests = %v", seen)
	}

	accounts.refreshUsageNow(context.Background())
	if len(seen) != 2 {
		t.Fatal("usage was fetched again before it was due")
	}
	writeTestFile(t, filepath.Join(home, ".claude", ".credentials.json"),
		`{"claudeAiOauth":{"accessToken":"old","expiresAt":`+jsonInt(now.Add(-time.Hour).UnixMilli())+`}}`)
	accounts.sync()
	now = now.Add(runnerUsageMinGap)
	accounts.refreshUsageNow(context.Background())
	claude = accounts.snapshot("claude-code")[0]["usage"].(map[string]any)
	if len(seen) != 2 || claude["error"] != "token_expired" || len(claude["windows"].([]runnerUsageWindow)) != 3 {
		t.Fatalf("an expired token was used or the last reading dropped: %v %v", seen, claude)
	}
}

func jsonInt(value int64) string {
	raw, _ := json.Marshal(value)
	return string(raw)
}
