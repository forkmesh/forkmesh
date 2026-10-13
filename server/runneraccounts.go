package mirrornode

import (
	"bytes"
	"context"
	"crypto/rand"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"sync"
	"sync/atomic"
	"time"
)

const (
	runnerAccountsDir        = ".accounts"
	runnerAccountMax         = 12
	runnerAccountStateLimit  = 64 << 10
	runnerClaudeConfigLimit  = 4 << 20
	runnerUsageMaxAge        = 5 * time.Minute
	runnerUsageMinGap        = time.Minute
	runnerUsageTimeout       = 15 * time.Second
	runnerUsageResponseLimit = 256 << 10
	runnerAccountResultKeep  = 2 * time.Minute
	runnerAccountAppliedKeep = 15 * time.Minute
)

var (
	claudeUsageURL = "https://api.anthropic.com/api/oauth/usage"
	codexUsageURL  = "https://chatgpt.com/backend-api/wham/usage"
)

var runnerAccountProviders = []string{"claude-code", "codex"}

func runnerAccountCredential(provider string) string {
	switch provider {
	case "claude-code":
		return jobCredentialFiles[0]
	case "codex":
		return jobCredentialFiles[1]
	}
	return ""
}

func runnerAccountProvider(rel string) string {
	for _, provider := range runnerAccountProviders {
		if runnerAccountCredential(provider) == rel {
			return provider
		}
	}
	return ""
}

type runnerAccount struct {
	ID      string `json:"id"`
	Label   string `json:"label"`
	Email   string `json:"email,omitempty"`
	Subject string `json:"subject,omitempty"`
	Plan    string `json:"plan,omitempty"`
	AddedAt int64  `json:"addedAt"`
}

type runnerAccountState struct {
	Active       string          `json:"active"`
	Accounts     []runnerAccount `json:"accounts"`
	StaleSubject string          `json:"staleSubject,omitempty"`
}

func (s *runnerAccountState) find(id string) int {
	for i, account := range s.Accounts {
		if account.ID == id {
			return i
		}
	}
	return -1
}

type runnerUsageWindow struct {
	ID            string  `json:"id"`
	Label         string  `json:"label"`
	UsedPercent   float64 `json:"usedPercent"`
	ResetsAt      int64   `json:"resetsAt,omitempty"`
	WindowMinutes int64   `json:"windowMinutes,omitempty"`
}

type runnerUsage struct {
	Windows   []runnerUsageWindow `json:"windows"`
	Plan      string              `json:"plan,omitempty"`
	FetchedAt int64               `json:"fetchedAt,omitempty"`
	Error     string              `json:"error,omitempty"`
	attempted time.Time
	stale     bool
}

type runnerAccountRequest struct {
	ID       string `json:"id"`
	Provider string `json:"provider"`
	Action   string `json:"action"`
	Account  string `json:"account"`
}

type runnerAccountResult struct {
	ID      string `json:"id"`
	OK      bool   `json:"ok"`
	Message string `json:"message,omitempty"`
	at      time.Time
}

type runnerAccounts struct {
	root   string
	home   string
	uid    int
	gid    int
	now    func() time.Time
	client *http.Client

	signingIn map[string][]byte
	opened    map[string]map[string]string

	usageMu    sync.Mutex
	usage      map[string]*runnerUsage
	refreshing atomic.Bool

	resultsMu sync.Mutex
	results   map[string]runnerAccountResult
	applied   map[string]time.Time
}

func newRunnerAccounts(root, home string, uid, gid int) *runnerAccounts {
	return &runnerAccounts{root: root, home: home, uid: uid, gid: gid, now: time.Now,
		client:    &http.Client{Timeout: runnerUsageTimeout},
		signingIn: map[string][]byte{}, opened: map[string]map[string]string{},
		usage: map[string]*runnerUsage{}, results: map[string]runnerAccountResult{},
		applied: map[string]time.Time{}}
}

func (a *runnerAccounts) ownerGID() int {
	if a.gid != 0 {
		return a.gid
	}
	return a.uid
}

func (a *runnerAccounts) providerDir(provider string) (string, error) {
	if a.root == "" || runnerAccountCredential(provider) == "" {
		return "", errors.New("unknown provider")
	}
	if err := os.MkdirAll(filepath.Dir(a.root), 0o700); err != nil {
		return "", err
	}
	if err := ensureDaemonDir(a.root, 0o700); err != nil {
		return "", err
	}
	dir := filepath.Join(a.root, provider)
	if err := ensureDaemonDir(dir, 0o700); err != nil {
		return "", err
	}
	return dir, nil
}

func (a *runnerAccounts) load(provider string) runnerAccountState {
	var state runnerAccountState
	dir, err := a.providerDir(provider)
	if err != nil {
		return state
	}
	raw, err := readOwnedFile(filepath.Join(dir, "state.json"), 0, runnerAccountStateLimit)
	if err == nil {
		_ = json.Unmarshal(raw, &state)
	}
	kept := state.Accounts[:0]
	for _, account := range state.Accounts {
		if validLoginID(account.ID) {
			kept = append(kept, account)
		}
	}
	state.Accounts = kept
	if state.find(state.Active) < 0 {
		state.Active = ""
	}
	return state
}

func (a *runnerAccounts) save(provider string, state runnerAccountState) error {
	dir, err := a.providerDir(provider)
	if err != nil {
		return err
	}
	raw, err := json.Marshal(state)
	if err != nil {
		return err
	}
	return writeOwnedFile(dir, "state.json", raw, 0, 0)
}

func (a *runnerAccounts) readSlot(provider, id string) []byte {
	dir, err := a.providerDir(provider)
	if err != nil || !validLoginID(id) {
		return nil
	}
	raw, err := readOwnedFile(filepath.Join(dir, id+".json"), 0, jobCredentialLimit)
	if err != nil || !usableCredential(provider, raw) {
		return nil
	}
	return raw
}

func usableCredential(provider string, data []byte) bool {
	return validCredential(data) && credentialFingerprint(provider == "codex", data) != ""
}

func (a *runnerAccounts) installedLocked(provider string, state *runnerAccountState, id string) {
	state.StaleSubject = ""
	index := state.find(id)
	if index < 0 {
		return
	}
	if config := a.identity(provider, nil, true).subject; config != "" && config != state.Accounts[index].Subject {
		state.StaleSubject = config
	}
}

func (a *runnerAccounts) writeSlot(provider, id string, data []byte) error {
	dir, err := a.providerDir(provider)
	if err != nil {
		return err
	}
	if !validLoginID(id) {
		return errors.New("invalid account id")
	}
	return writeOwnedFile(dir, id+".json", data, 0, 0)
}

func (a *runnerAccounts) readActive(provider string) []byte {
	rel := runnerAccountCredential(provider)
	if a.home == "" || rel == "" || !safeDirChain(a.home, rel) {
		return nil
	}
	raw, err := readOwnedFile(filepath.Join(a.home, rel), a.uid, jobCredentialLimit)
	if err != nil || !validCredential(raw) {
		return nil
	}
	return raw
}

func (a *runnerAccounts) writeActive(provider string, data []byte) error {
	rel := runnerAccountCredential(provider)
	if a.home == "" || rel == "" {
		return errors.New("this runner has no job user home")
	}
	parent := filepath.Join(a.home, filepath.Dir(rel))
	if info, err := os.Lstat(parent); errors.Is(err, os.ErrNotExist) {
		if !safeDirChain(a.home, filepath.Dir(rel)) {
			return errors.New("the job user home is not a plain directory")
		}
		if err := os.Mkdir(parent, 0o700); err != nil {
			return err
		}
		if a.uid != 0 {
			if err := os.Lchown(parent, a.uid, a.ownerGID()); err != nil {
				return err
			}
		}
	} else if err != nil {
		return err
	} else if !info.IsDir() || info.Mode()&os.ModeSymlink != 0 {
		return fmt.Errorf("%s is not a directory", parent)
	}
	if !safeDirChain(a.home, rel) {
		return errors.New("the credential directory is not a plain directory")
	}
	return writeOwnedFile(parent, filepath.Base(rel), data, a.uid, a.ownerGID())
}

func (a *runnerAccounts) removeActive(provider string) error {
	rel := runnerAccountCredential(provider)
	if a.home == "" || rel == "" || !safeDirChain(a.home, rel) {
		return nil
	}
	err := os.Remove(filepath.Join(a.home, rel))
	if errors.Is(err, os.ErrNotExist) {
		return nil
	}
	return err
}

func newRunnerAccountID() string {
	raw := make([]byte, 16)
	_, _ = rand.Read(raw)
	return hex.EncodeToString(raw)
}

func jwtClaims(token string) map[string]any {
	parts := strings.Split(token, ".")
	if len(parts) != 3 {
		return nil
	}
	raw, err := base64.RawURLEncoding.DecodeString(strings.TrimRight(parts[1], "="))
	if err != nil {
		return nil
	}
	var claims map[string]any
	if json.Unmarshal(raw, &claims) != nil {
		return nil
	}
	return claims
}

type runnerAccountIdentity struct {
	subject, email, plan string
}

func (a *runnerAccounts) identity(provider string, credential []byte, fresh bool) runnerAccountIdentity {
	var id runnerAccountIdentity
	switch provider {
	case "codex":
		var auth struct {
			APIKey string `json:"OPENAI_API_KEY"`
			Tokens struct {
				IDToken   string `json:"id_token"`
				AccountID string `json:"account_id"`
			} `json:"tokens"`
		}
		if json.Unmarshal(credential, &auth) != nil {
			return id
		}
		claims := jwtClaims(auth.Tokens.IDToken)
		id.email = stringValue(claims["email"])
		if info, ok := claims["https://api.openai.com/auth"].(map[string]any); ok {
			id.plan = stringValue(info["chatgpt_plan_type"])
			if auth.Tokens.AccountID == "" {
				auth.Tokens.AccountID = stringValue(info["chatgpt_account_id"])
			}
		}
		switch {
		case auth.Tokens.AccountID != "" || id.email != "":
			id.subject = auth.Tokens.AccountID + "/" + strings.ToLower(id.email)
		case strings.TrimSpace(auth.APIKey) != "":
			id.plan = "API key"
		}
	case "claude-code":
		var credentials struct {
			OAuth struct {
				SubscriptionType string `json:"subscriptionType"`
			} `json:"claudeAiOauth"`
		}
		if json.Unmarshal(credential, &credentials) == nil {
			id.plan = credentials.OAuth.SubscriptionType
		}
		if !fresh || a.home == "" || !safeDirChain(a.home, ".claude.json") {
			return id
		}
		raw, err := readOwnedFile(filepath.Join(a.home, ".claude.json"), a.uid, runnerClaudeConfigLimit)
		if err != nil {
			return id
		}
		var config struct {
			Account struct {
				UUID  string `json:"accountUuid"`
				Email string `json:"emailAddress"`
				Org   string `json:"organizationUuid"`
			} `json:"oauthAccount"`
		}
		if json.Unmarshal(raw, &config) == nil && (config.Account.UUID != "" || config.Account.Email != "") {
			id.email = config.Account.Email
			id.subject = config.Account.UUID + "/" + config.Account.Org + "/" + strings.ToLower(config.Account.Email)
		}
	}
	id.email = boundedText([]byte(id.email), 120)
	id.plan = boundedText([]byte(id.plan), 40)
	return id
}

func (a *runnerAccounts) adoptLocked(provider string, credential []byte, newSlot bool) error {
	state := a.load(provider)
	who := a.identity(provider, credential, true)
	if who.subject != "" && who.subject == state.StaleSubject {
		who.subject, who.email = "", ""
	}
	index := -1
	if who.subject != "" {
		for i, account := range state.Accounts {
			if account.Subject == who.subject {
				index = i
				break
			}
		}
	}
	if index < 0 && (!newSlot || len(state.Accounts) >= runnerAccountMax) {
		index = state.find(state.Active)
	}
	if index < 0 {
		label := who.email
		if label == "" {
			label = fmt.Sprintf("Account %d", len(state.Accounts)+1)
		}
		state.Accounts = append(state.Accounts, runnerAccount{ID: newRunnerAccountID(), Label: label,
			AddedAt: a.now().UnixMilli()})
		index = len(state.Accounts) - 1
	}
	account := &state.Accounts[index]
	if who.subject != "" {
		account.Subject = who.subject
	}
	if who.email != "" {
		account.Email = who.email
		if account.Label == "" || strings.HasPrefix(account.Label, "Account ") {
			account.Label = who.email
		}
	}
	if who.plan != "" {
		account.Plan = who.plan
	}
	if err := a.writeSlot(provider, account.ID, credential); err != nil {
		return err
	}
	state.Active = account.ID
	a.markStale(provider, account.ID)
	return a.save(provider, state)
}

func (a *runnerAccounts) syncLocked(provider string) {
	if _, busy := a.signingIn[provider]; busy {
		return
	}
	current := a.readActive(provider)
	if current == nil || !usableCredential(provider, current) {
		return
	}
	state := a.load(provider)
	if len(state.Accounts) == 0 {
		_ = a.adoptLocked(provider, current, true)
		return
	}
	if state.Active != "" && bytes.Equal(a.readSlot(provider, state.Active), current) {
		return
	}
	_ = a.adoptLocked(provider, current, false)
}

func (a *runnerAccounts) sync() {
	if a == nil {
		return
	}
	credentialMu.Lock()
	defer credentialMu.Unlock()
	for _, provider := range runnerAccountProviders {
		a.syncLocked(provider)
	}
}

func (a *runnerAccounts) beginLogin(provider string) {
	if a == nil || runnerAccountCredential(provider) == "" {
		return
	}
	credentialMu.Lock()
	defer credentialMu.Unlock()
	a.syncLocked(provider)
	a.signingIn[provider] = a.readActive(provider)
}

func (a *runnerAccounts) endLogin(provider string, succeeded bool) {
	if a == nil || runnerAccountCredential(provider) == "" {
		return
	}
	credentialMu.Lock()
	defer credentialMu.Unlock()
	before, ok := a.signingIn[provider]
	delete(a.signingIn, provider)
	if !ok {
		return
	}
	current := a.readActive(provider)
	if succeeded && current != nil && !bytes.Equal(current, before) && usableCredential(provider, current) {
		if state := a.load(provider); state.StaleSubject != "" {
			state.StaleSubject = ""
			_ = a.save(provider, state)
		}
		_ = a.adoptLocked(provider, current, true)
		return
	}
	state := a.load(provider)
	if current == nil && state.Active != "" {
		if saved := a.readSlot(provider, state.Active); saved != nil && a.writeActive(provider, saved) == nil {
			a.installedLocked(provider, &state, state.Active)
			_ = a.save(provider, state)
		}
		return
	}
	a.syncLocked(provider)
}

func (a *runnerAccounts) activate(provider, id string) error {
	credentialMu.Lock()
	defer credentialMu.Unlock()
	if _, busy := a.signingIn[provider]; busy {
		return errors.New("a sign-in for this provider is running; try again when it ends")
	}
	a.syncLocked(provider)
	state := a.load(provider)
	if state.find(id) < 0 {
		return errors.New("that account is not on this runner")
	}
	saved := a.readSlot(provider, id)
	if saved == nil {
		return errors.New("that account's saved login is missing; sign it in again")
	}
	if err := a.writeActive(provider, saved); err != nil {
		return err
	}
	state.Active = id
	a.installedLocked(provider, &state, id)
	a.markStale(provider, id)
	return a.save(provider, state)
}

func (a *runnerAccounts) remove(provider, id string) error {
	credentialMu.Lock()
	defer credentialMu.Unlock()
	if _, busy := a.signingIn[provider]; busy {
		return errors.New("a sign-in for this provider is running; try again when it ends")
	}
	a.syncLocked(provider)
	state := a.load(provider)
	index := state.find(id)
	if index < 0 {
		return nil
	}
	state.Accounts = append(state.Accounts[:index], state.Accounts[index+1:]...)
	if dir, err := a.providerDir(provider); err == nil {
		_ = os.Remove(filepath.Join(dir, id+".json"))
	}
	a.usageMu.Lock()
	delete(a.usage, provider+"/"+id)
	a.usageMu.Unlock()
	if state.Active == id {
		state.Active = ""
		for _, account := range state.Accounts {
			if saved := a.readSlot(provider, account.ID); saved != nil {
				if err := a.writeActive(provider, saved); err != nil {
					return err
				}
				state.Active = account.ID
				a.installedLocked(provider, &state, account.ID)
				break
			}
		}
		if state.Active == "" {
			if err := a.removeActive(provider); err != nil {
				return err
			}
		}
	}
	return a.save(provider, state)
}

func (a *runnerAccounts) noteOpenedLocked(home, rel string) {
	if a == nil {
		return
	}
	provider := runnerAccountProvider(rel)
	if provider == "" {
		return
	}
	a.syncLocked(provider)
	if a.opened[home] == nil {
		a.opened[home] = map[string]string{}
	}
	a.opened[home][provider] = a.load(provider).Active
}

func (a *runnerAccounts) copyBackLocked(home, rel string, data []byte) bool {
	if a == nil {
		return false
	}
	provider := runnerAccountProvider(rel)
	id := a.opened[home][provider]
	if provider == "" || id == "" {
		return false
	}
	a.markStale(provider, id)
	state := a.load(provider)
	if state.find(id) < 0 || !usableCredential(provider, data) {
		return true
	}
	if !bytes.Equal(a.readSlot(provider, id), data) {
		_ = a.writeSlot(provider, id, data)
	}
	return state.Active != id
}

func (a *runnerAccounts) closed(home string) {
	if a == nil {
		return
	}
	credentialMu.Lock()
	delete(a.opened, home)
	credentialMu.Unlock()
}

func (a *runnerAccounts) apply(requests []runnerAccountRequest) bool {
	if a == nil {
		return false
	}
	now := a.now()
	changed := false
	for _, request := range requests {
		if !validLoginID(request.ID) {
			continue
		}
		a.resultsMu.Lock()
		_, done := a.applied[request.ID]
		a.resultsMu.Unlock()
		if done {
			continue
		}
		var err error
		switch {
		case runnerAccountCredential(request.Provider) == "":
			err = errors.New("unknown provider")
		case !validLoginID(request.Account):
			err = errors.New("invalid account id")
		case request.Action == "activate":
			err = a.activate(request.Provider, request.Account)
		case request.Action == "remove":
			err = a.remove(request.Provider, request.Account)
		default:
			err = errors.New("unknown action")
		}
		result := runnerAccountResult{ID: request.ID, OK: err == nil, at: now}
		if err != nil {
			result.Message = boundedText([]byte(err.Error()), 200)
		}
		a.resultsMu.Lock()
		a.applied[request.ID] = now
		a.results[request.ID] = result
		a.resultsMu.Unlock()
		changed = true
	}
	return changed
}

func (a *runnerAccounts) pendingResults() []runnerAccountResult {
	results := []runnerAccountResult{}
	if a == nil {
		return results
	}
	now := a.now()
	a.resultsMu.Lock()
	defer a.resultsMu.Unlock()
	for id, result := range a.results {
		if now.Sub(result.at) > runnerAccountResultKeep {
			delete(a.results, id)
			continue
		}
		results = append(results, result)
	}
	for id, at := range a.applied {
		if now.Sub(at) > runnerAccountAppliedKeep {
			delete(a.applied, id)
		}
	}
	sort.Slice(results, func(i, j int) bool { return results[i].ID < results[j].ID })
	return results
}

func (a *runnerAccounts) snapshot(provider string) []map[string]any {
	accounts := []map[string]any{}
	if a == nil {
		return accounts
	}
	credentialMu.Lock()
	state := a.load(provider)
	credentialMu.Unlock()
	a.usageMu.Lock()
	defer a.usageMu.Unlock()
	for _, account := range state.Accounts {
		entry := map[string]any{"id": account.ID, "label": account.Label, "email": account.Email,
			"plan": account.Plan, "active": account.ID == state.Active, "addedAt": account.AddedAt}
		if usage := a.usage[provider+"/"+account.ID]; usage != nil && (usage.FetchedAt != 0 || usage.Error != "") {
			entry["usage"] = map[string]any{"windows": usage.Windows, "plan": usage.Plan,
				"fetchedAt": usage.FetchedAt, "error": usage.Error}
		}
		accounts = append(accounts, entry)
	}
	return accounts
}

func (a *runnerAccounts) markStale(provider, id string) {
	a.usageMu.Lock()
	defer a.usageMu.Unlock()
	if usage := a.usage[provider+"/"+id]; usage != nil {
		usage.stale = true
	}
}

func (a *runnerAccounts) refreshUsage(ctx context.Context) {
	if a == nil || !a.refreshing.CompareAndSwap(false, true) {
		return
	}
	go func() {
		defer a.refreshing.Store(false)
		a.refreshUsageNow(ctx)
	}()
}

func (a *runnerAccounts) refreshUsageNow(ctx context.Context) {
	now := a.now()
	for _, provider := range runnerAccountProviders {
		credentialMu.Lock()
		state := a.load(provider)
		credentialMu.Unlock()
		for _, account := range state.Accounts {
			key := provider + "/" + account.ID
			a.usageMu.Lock()
			usage := a.usage[key]
			if usage == nil {
				usage = &runnerUsage{}
				a.usage[key] = usage
			}
			age := now.Sub(usage.attempted)
			due := age >= runnerUsageMaxAge || (usage.stale && age >= runnerUsageMinGap)
			a.usageMu.Unlock()
			if !due || ctx.Err() != nil {
				continue
			}
			credentialMu.Lock()
			credential := a.readSlot(provider, account.ID)
			credentialMu.Unlock()
			windows, plan, err := a.fetchUsage(ctx, provider, credential, now)
			a.usageMu.Lock()
			usage.attempted, usage.stale = now, false
			if err != nil {
				usage.Error = boundedText([]byte(err.Error()), 160)
			} else {
				usage.Windows, usage.Plan, usage.Error = windows, plan, ""
				usage.FetchedAt = now.UnixMilli()
			}
			a.usageMu.Unlock()
		}
	}
}

var errUsageTokenExpired = errors.New("token_expired")

func (a *runnerAccounts) fetchUsage(ctx context.Context, provider string, credential []byte, now time.Time) ([]runnerUsageWindow, string, error) {
	if credential == nil {
		return nil, "", errors.New("missing_login")
	}
	headers := map[string]string{}
	endpoint := ""
	switch provider {
	case "claude-code":
		var credentials struct {
			OAuth struct {
				AccessToken string `json:"accessToken"`
				ExpiresAt   int64  `json:"expiresAt"`
			} `json:"claudeAiOauth"`
		}
		if json.Unmarshal(credential, &credentials) != nil || credentials.OAuth.AccessToken == "" {
			return nil, "", errors.New("no_oauth_login")
		}
		if credentials.OAuth.ExpiresAt != 0 && credentials.OAuth.ExpiresAt <= now.Add(time.Minute).UnixMilli() {
			return nil, "", errUsageTokenExpired
		}
		endpoint = claudeUsageURL
		headers["Authorization"] = "Bearer " + credentials.OAuth.AccessToken
		headers["anthropic-beta"] = "oauth-2025-04-20"
	case "codex":
		var auth struct {
			Tokens struct {
				AccessToken string `json:"access_token"`
				AccountID   string `json:"account_id"`
			} `json:"tokens"`
		}
		if json.Unmarshal(credential, &auth) != nil || auth.Tokens.AccessToken == "" {
			return nil, "", errors.New("no_oauth_login")
		}
		if exp, ok := jwtClaims(auth.Tokens.AccessToken)["exp"].(float64); ok && int64(exp) <= now.Add(time.Minute).Unix() {
			return nil, "", errUsageTokenExpired
		}
		endpoint = codexUsageURL
		headers["Authorization"] = "Bearer " + auth.Tokens.AccessToken
		if auth.Tokens.AccountID != "" {
			headers["ChatGPT-Account-Id"] = auth.Tokens.AccountID
		}
	default:
		return nil, "", errors.New("unknown provider")
	}
	ctx, cancel := context.WithTimeout(ctx, runnerUsageTimeout)
	defer cancel()
	request, err := http.NewRequestWithContext(ctx, http.MethodGet, endpoint, nil)
	if err != nil {
		return nil, "", err
	}
	request.Header.Set("Accept", "application/json")
	request.Header.Set("User-Agent", "forkmesh-runner")
	for name, value := range headers {
		request.Header.Set(name, value)
	}
	response, err := a.client.Do(request)
	if err != nil {
		return nil, "", errors.New("unreachable")
	}
	defer response.Body.Close()
	raw, _ := io.ReadAll(io.LimitReader(response.Body, runnerUsageResponseLimit))
	switch {
	case response.StatusCode == http.StatusUnauthorized || response.StatusCode == http.StatusForbidden:
		return nil, "", errors.New("unauthorized")
	case response.StatusCode < 200 || response.StatusCode >= 300:
		return nil, "", fmt.Errorf("http_%d", response.StatusCode)
	}
	if provider == "codex" {
		return parseCodexUsage(raw, now)
	}
	return parseClaudeUsage(raw)
}

var claudeUsageLabels = []struct{ key, label string }{
	{"five_hour", "5-hour session"},
	{"seven_day", "Weekly, all models"},
	{"seven_day_opus", "Weekly, Opus"},
	{"seven_day_sonnet", "Weekly, Sonnet"},
	{"seven_day_oauth_apps", "Weekly, apps"},
}

func parseClaudeUsage(raw []byte) ([]runnerUsageWindow, string, error) {
	var body map[string]json.RawMessage
	if json.Unmarshal(raw, &body) != nil {
		return nil, "", errors.New("invalid_response")
	}
	labels := map[string]string{}
	order := []string{}
	for _, known := range claudeUsageLabels {
		labels[known.key] = known.label
		order = append(order, known.key)
	}
	extra := []string{}
	for key := range body {
		if _, ok := labels[key]; !ok && (strings.HasPrefix(key, "five_hour") || strings.HasPrefix(key, "seven_day")) {
			extra = append(extra, key)
		}
	}
	sort.Strings(extra)
	windows := []runnerUsageWindow{}
	for _, key := range append(order, extra...) {
		var window struct {
			Utilization *float64 `json:"utilization"`
			ResetsAt    string   `json:"resets_at"`
		}
		if json.Unmarshal(body[key], &window) != nil || window.Utilization == nil {
			continue
		}
		label := labels[key]
		if label == "" {
			label = strings.ReplaceAll(key, "_", " ")
		}
		entry := runnerUsageWindow{ID: key, Label: label, UsedPercent: *window.Utilization}
		if at, err := time.Parse(time.RFC3339Nano, window.ResetsAt); err == nil {
			entry.ResetsAt = at.UnixMilli()
		}
		switch {
		case strings.HasPrefix(key, "five_hour"):
			entry.WindowMinutes = 300
		case strings.HasPrefix(key, "seven_day"):
			entry.WindowMinutes = 7 * 24 * 60
		}
		windows = append(windows, entry)
	}
	return windows, "", nil
}

func parseCodexUsage(raw []byte, now time.Time) ([]runnerUsageWindow, string, error) {
	type codexWindow struct {
		UsedPercent        *float64 `json:"used_percent"`
		LimitWindowSeconds int64    `json:"limit_window_seconds"`
		ResetAfterSeconds  int64    `json:"reset_after_seconds"`
		ResetAt            int64    `json:"reset_at"`
	}
	var body struct {
		PlanType  string `json:"plan_type"`
		RateLimit *struct {
			Primary   *codexWindow `json:"primary_window"`
			Secondary *codexWindow `json:"secondary_window"`
		} `json:"rate_limit"`
	}
	if json.Unmarshal(raw, &body) != nil {
		return nil, "", errors.New("invalid_response")
	}
	windows := []runnerUsageWindow{}
	if body.RateLimit != nil {
		for _, item := range []struct {
			id     string
			window *codexWindow
		}{{"primary", body.RateLimit.Primary}, {"secondary", body.RateLimit.Secondary}} {
			if item.window == nil || item.window.UsedPercent == nil {
				continue
			}
			minutes := item.window.LimitWindowSeconds / 60
			entry := runnerUsageWindow{ID: item.id, Label: codexWindowLabel(minutes),
				UsedPercent: *item.window.UsedPercent, WindowMinutes: minutes}
			switch {
			case item.window.ResetAt > 0:
				entry.ResetsAt = item.window.ResetAt * 1000
			case item.window.ResetAfterSeconds > 0:
				entry.ResetsAt = now.Add(time.Duration(item.window.ResetAfterSeconds) * time.Second).UnixMilli()
			}
			windows = append(windows, entry)
		}
	}
	return windows, boundedText([]byte(body.PlanType), 40), nil
}

func codexWindowLabel(minutes int64) string {
	switch {
	case minutes == 300:
		return "5-hour session"
	case minutes == 7*24*60:
		return "Weekly"
	case minutes >= 24*60 && minutes%(24*60) == 0:
		return fmt.Sprintf("%d-day", minutes/(24*60))
	case minutes >= 60 && minutes%60 == 0:
		return fmt.Sprintf("%d-hour", minutes/60)
	case minutes > 0:
		return fmt.Sprintf("%d-minute", minutes)
	}
	return "Usage"
}
