package stats

import (
	"encoding/json"
	"net/url"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"

	"forkmesh.com/desktop-core/internal/qsettings"
)

const (
	accountNameSetting      = "account/nodeName"
	handleSetting           = "profile/handle"
	displayNameSetting      = "profile/displayName"
	repositoriesArray       = "repositories/items"
	relayListsSetting       = "repositories/relayLists"
	serversSetting          = "servers/items"
	activeServerSetting     = "servers/active"
	serverURLSetting        = "server/url"
	actionsVariablesSetting = "actions/variables"
	defaultHost             = "app.forkmesh.com"
	roomPath                = "/api/repo/mainnode/forkmesh/rooms/world-general/ws"
	defaultServerURL        = "wss://" + defaultHost + roomPath
	localServerURL          = "ws://127.0.0.1:8787" + roomPath
)

var opaqueIDPattern = regexp.MustCompile(`^[0-9a-f]{64}$`)

type repoRecord struct {
	Owner     string
	Name      string
	CloneURL  string
	RelayURL  string
	LocalPath string
}

func loadRepositories(s *qsettings.Settings) []repoRecord {
	count := int(s.Int(repositoriesArray+"/size", 0))
	out := []repoRecord{}
	for i := 1; i <= count; i++ {
		prefix := repositoriesArray + "/" + strconv.Itoa(i) + "/"
		get := func(key string) string { return s.String(prefix+key, "") }
		repo := repoRecord{
			Owner:     get("owner"),
			Name:      get("name"),
			CloneURL:  get("cloneUrl"),
			RelayURL:  strings.TrimSpace(get("relayUrl")),
			LocalPath: get("localPath"),
		}
		replica := strings.TrimSpace(get("privateReplicaId"))
		archive := strings.TrimSpace(get("publicArchiveId"))
		if repo.Name == "" {
			continue
		}
		if repositorySource(repo) == "" && !opaqueIDPattern.MatchString(archive) && !opaqueIDPattern.MatchString(replica) {
			continue
		}
		out = append(out, repo)
	}
	return out
}

func repositorySource(repo repoRecord) string {
	if local := strings.TrimSpace(repo.LocalPath); local != "" {
		return local
	}
	return strings.TrimSpace(repo.CloneURL)
}

func accountNameFromInput(value, fallback string) string {
	value = strings.ToLower(strings.TrimSpace(value))
	var b strings.Builder
	for _, r := range value {
		if r >= 128 {
			continue
		}
		if (r >= 'a' && r <= 'z') || (r >= '0' && r <= '9') || r == '-' {
			b.WriteRune(r)
		}
	}
	out := b.String()
	for out != "" && !(out[0] >= 'a' && out[0] <= 'z') {
		out = out[1:]
	}
	if len(out) > 63 {
		out = out[:63]
	}
	out = strings.TrimRight(out, "-")
	if out == "" {
		return fallback
	}
	return out
}

func savedProfileName(s *qsettings.Settings) string {
	for _, key := range []string{accountNameSetting, handleSetting, displayNameSetting} {
		if name := strings.TrimSpace(s.String(key, "")); name != "" {
			return name
		}
	}
	return ""
}

func accountOwner(s *qsettings.Settings) string {
	if stored := s.String(accountNameSetting, ""); stored != "" {
		return stored
	}
	return accountNameFromInput(strings.ToLower(savedProfileName(s)), "owner")
}

func canonicalServerURL(input string) string {
	s := strings.TrimSpace(input)
	if s == "" {
		return defaultServerURL
	}
	if strings.Contains(s, "://") {
		return s
	}
	if slash := strings.IndexByte(s, '/'); slash >= 0 {
		s = s[:slash]
	}
	scheme := "wss"
	if strings.HasPrefix(s, "127.0.0.1") || strings.HasPrefix(s, "localhost") {
		scheme = "ws"
	}
	return scheme + "://" + s + roomPath
}

func urlHost(u *url.URL) string {
	host := strings.ToLower(u.Hostname())
	if strings.Contains(host, ":") {
		return "[" + host + "]"
	}
	return host
}

func serverHostDisplay(full string) string {
	u, err := url.Parse(strings.TrimSpace(full))
	if err != nil || u.Hostname() == "" {
		return strings.TrimSpace(full)
	}
	host := strings.ToLower(u.Hostname())
	if port, err := strconv.Atoi(u.Port()); err == nil && port > 0 {
		host += ":" + strconv.Itoa(port)
	}
	return host
}

func normalizedOrigin(raw string) string {
	u, err := url.Parse(raw)
	if err != nil || u.Hostname() == "" {
		return ""
	}
	scheme := strings.ToLower(u.Scheme)
	switch scheme {
	case "ws":
		scheme = "http"
	case "wss":
		scheme = "https"
	}
	port := u.Port()
	if (scheme == "http" && port == "80") || (scheme == "https" && port == "443") {
		port = ""
	}
	out := scheme + "://" + urlHost(u)
	if port != "" {
		if n, err := strconv.Atoi(port); err == nil {
			out += ":" + strconv.Itoa(n)
		}
	}
	return strings.ToLower(out)
}

type relayScope struct {
	configured []string
	fallback   string
	lists      map[string]map[string]bool
	active     string
}

func serverURLs(s *qsettings.Settings) []string {
	var rows []map[string]any
	_ = json.Unmarshal([]byte(s.String(serversSetting, "")), &rows)
	urls := []string{}
	for _, row := range rows {
		text, _ := row["url"].(string)
		text = strings.TrimSpace(text)
		if text == "" {
			continue
		}
		urls = append(urls, canonicalServerURL(text))
	}
	if len(urls) == 0 {
		saved := strings.TrimSpace(s.String(serverURLSetting, ""))
		if saved == "" || saved == localServerURL {
			saved = defaultServerURL
		}
		urls = append(urls, saved)
	}
	return urls
}

func loadRelayScope(s *qsettings.Settings) relayScope {
	urls := serverURLs(s)
	scope := relayScope{fallback: normalizedOrigin(defaultServerURL), lists: map[string]map[string]bool{}}
	for _, server := range urls {
		origin := normalizedOrigin(canonicalServerURL(server))
		if origin != "" && !contains(scope.configured, origin) {
			scope.configured = append(scope.configured, origin)
		}
	}
	var stored map[string]any
	_ = json.Unmarshal([]byte(s.String(relayListsSetting, "")), &stored)
	keys := make([]string, 0, len(stored))
	for key := range stored {
		keys = append(keys, key)
	}
	sort.Strings(keys)
	for _, key := range keys {
		origin := normalizedOrigin(key)
		if origin == "" {
			continue
		}
		set := map[string]bool{}
		if values, ok := stored[key].([]any); ok {
			for _, value := range values {
				text, _ := value.(string)
				text = strings.ToLower(strings.TrimSpace(text))
				if text != "" {
					set[text] = true
				}
			}
		}
		if contains(scope.configured, origin) || origin == scope.fallback {
			scope.lists[origin] = set
		}
	}
	active := int(s.Int(activeServerSetting, 0))
	if active < 0 || active >= len(urls) {
		active = 0
	}
	catalog := canonicalServerURL(serverHostDisplay(urls[active]))
	scope.active = normalizedOrigin(catalog)
	if scope.active == "" {
		scope.active = normalizedOrigin(defaultServerURL)
	}
	return scope
}

func contains(list []string, value string) bool {
	for _, item := range list {
		if item == value {
			return true
		}
	}
	return false
}

func repositoryKey(owner, name string) string {
	owner = strings.ToLower(strings.TrimSpace(owner))
	name = strings.ToLower(strings.TrimSpace(name))
	if owner == "" || name == "" {
		return ""
	}
	return owner + "/" + name
}

func (scope relayScope) resolveOrigin(stored, cloneURL string, keys []string) string {
	if origin := normalizedOrigin(strings.TrimSpace(stored)); origin != "" {
		return origin
	}
	if origin := normalizedOrigin(strings.TrimSpace(cloneURL)); origin != "" && contains(scope.configured, origin) {
		return origin
	}
	listedBy := ""
	listings := 0
	for origin, list := range scope.lists {
		for _, key := range keys {
			if list[key] {
				listedBy = origin
				listings++
				break
			}
		}
	}
	if listings == 1 {
		return listedBy
	}
	return scope.fallback
}

type repoCatalog struct {
	records []repoRecord
	scope   relayScope
	account string
}

func (m *Module) catalog() repoCatalog {
	s := m.settings()
	return repoCatalog{records: loadRepositories(s), scope: loadRelayScope(s), account: accountOwner(s)}
}

func (c repoCatalog) belongsToActiveRelay(repo repoRecord) bool {
	if c.scope.active == "" {
		return false
	}
	publisher := c.account
	if publisher == "" {
		publisher = repoSegment(repo.Owner, "owner")
	}
	keys := []string{}
	for _, candidate := range []string{repo.Owner, publisher} {
		key := repositoryKey(candidate, repo.Name)
		if key != "" && !contains(keys, key) {
			keys = append(keys, key)
		}
	}
	return strings.EqualFold(c.scope.resolveOrigin(repo.RelayURL, repo.CloneURL, keys), c.scope.active)
}

func (c repoCatalog) indexFor(owner, name string) int {
	for i, repo := range c.records {
		if repo.Owner == owner && repo.Name == name && c.belongsToActiveRelay(repo) {
			return i
		}
	}
	return -1
}

func hasGit(dir string) bool {
	_, err := os.Stat(filepath.Join(dir, ".git"))
	return err == nil
}

func (c repoCatalog) checkoutDir(index int) string {
	if index < 0 || index >= len(c.records) {
		return ""
	}
	repo := c.records[index]
	writable := repo
	if strings.TrimSpace(repo.LocalPath) == "" || !fileExists(repo.LocalPath+"/.git") {
		for i, other := range c.records {
			if i == index {
				continue
			}
			if other.Owner == repo.Owner && other.Name == repo.Name && strings.TrimSpace(other.LocalPath) != "" &&
				fileExists(other.LocalPath+"/.git") {
				writable = other
				break
			}
		}
	}
	dir := writable.LocalPath
	if dir == "" || !hasGit(dir) {
		return ""
	}
	return dir
}

func fileExists(path string) bool {
	_, err := os.Stat(path)
	return err == nil
}

func repoSegment(value, fallback string) string {
	value = strings.ToLower(strings.TrimSpace(value))
	var b strings.Builder
	lastDash := false
	for _, r := range value {
		if isLetterOrNumber(r) || r == '_' || r == '-' {
			b.WriteRune(r)
			lastDash = false
		} else if !lastDash {
			b.WriteByte('-')
			lastDash = true
		}
	}
	out := strings.Trim(b.String(), "-")
	if out == "" {
		out = fallback
	}
	runes := []rune(out)
	if len(runes) > 48 {
		out = string(runes[:48])
	}
	return out
}

func actionVariables(s *qsettings.Settings) map[string]string {
	var raw map[string]any
	_ = json.Unmarshal([]byte(s.String(actionsVariablesSetting, "")), &raw)
	out := map[string]string{}
	for key, value := range raw {
		text, _ := value.(string)
		out[key] = text
	}
	return out
}
