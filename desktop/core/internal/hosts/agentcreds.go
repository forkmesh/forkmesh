package hosts

import (
	"encoding/base64"
	"encoding/json"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"unicode/utf16"
)

const (
	agentCredentialSectionCap = 256 * 1024
	agentCredentialFileCap    = 4 << 20
	claudeAPIKeySetting       = "agents/claudeApiKey"
	codexAPIKeySetting        = "agents/codexApiKey"
)

var agentEnvNamePattern = regexp.MustCompile(`^[A-Z][A-Z0-9_]{0,63}$`)

type agentLogins struct {
	claude []byte
	codex  []byte
	env    map[string]string
}

func (l *agentLogins) wipe() {
	wipeBytes(l.claude)
	wipeBytes(l.codex)
	l.claude, l.codex, l.env = nil, nil, nil
}

func agentProviderKey(provider string) string {
	if provider == "codex" {
		return "codex"
	}
	return "claude"
}

func agentDefaultConfigDir(provider string) string {
	codex := agentProviderKey(provider) == "codex"
	variable, folder := "CLAUDE_CONFIG_DIR", ".claude"
	if codex {
		variable, folder = "CODEX_HOME", ".codex"
	}
	if configured := strings.TrimSpace(os.Getenv(variable)); configured != "" {
		return filepath.Clean(configured)
	}
	home, _ := os.UserHomeDir()
	return home + "/" + folder
}

func (m *Module) activeAgentConfigDir(provider string) string {
	key := agentProviderKey(provider)
	settings := m.settings()
	selected := settings.String("agents/activeAccount/"+key, "default")
	if selected == "default" {
		return agentDefaultConfigDir(provider)
	}
	prefix := "agents/accountProfiles/" + key + "/"
	dir := settings.String(prefix+selected+"/configDir", "")
	if strings.TrimSpace(dir) == "" {
		return agentDefaultConfigDir(provider)
	}
	return dir
}

func agentCredentialPath(provider, configDir string) string {
	if agentProviderKey(provider) == "codex" {
		return filepath.Join(configDir, "auth.json")
	}
	return filepath.Join(configDir, ".credentials.json")
}

func readCredentialFile(path string) []byte {
	file, err := os.Open(path)
	if err != nil {
		return nil
	}
	defer file.Close()
	data := make([]byte, 0, 4096)
	buffer := make([]byte, 32*1024)
	for len(data) <= agentCredentialFileCap {
		n, err := file.Read(buffer)
		data = append(data, buffer[:n]...)
		if err != nil {
			break
		}
	}
	wipeBytes(buffer)
	return data
}

func qtTrimmedBytes(data []byte) []byte {
	isSpace := func(c byte) bool { return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r' }
	start, end := 0, len(data)
	for start < end && isSpace(data[start]) {
		start++
	}
	for end > start && isSpace(data[end-1]) {
		end--
	}
	return data[start:end]
}

func (m *Module) localAgentLogins() agentLogins {
	logins := agentLogins{env: map[string]string{}}
	logins.claude = readCredentialFile(agentCredentialPath("claude-code", m.activeAgentConfigDir("claude-code")))
	logins.codex = readCredentialFile(agentCredentialPath("codex", m.activeAgentConfigDir("codex")))
	settings := m.settings()
	if len(qtTrimmedBytes(logins.claude)) == 0 {
		if key := strings.TrimSpace(settings.String(claudeAPIKeySetting, "")); key != "" {
			logins.env["ANTHROPIC_API_KEY"] = key
		}
	}
	if len(qtTrimmedBytes(logins.codex)) == 0 {
		if key := strings.TrimSpace(settings.String(codexAPIKeySetting, "")); key != "" {
			logins.env["OPENAI_API_KEY"] = key
		}
	}
	return logins
}

func agentEnvEntryUsable(name, value string) bool {
	if !agentEnvNamePattern.MatchString(name) || value == "" || len(utf16.Encode([]rune(value))) > 4096 {
		return false
	}
	for _, r := range value {
		if r < 0x20 || r == 0x7f {
			return false
		}
	}
	return true
}

func agentEnvFileContents(env map[string]string) []byte {
	names := make([]string, 0, len(env))
	for name := range env {
		names = append(names, name)
	}
	sort.Strings(names)
	var out []byte
	for _, name := range names {
		if !agentEnvEntryUsable(name, env[name]) {
			continue
		}
		out = append(out, "export "+name+"="+shq(env[name])+"\n"...)
	}
	return out
}

func agentLoginsEmpty(logins agentLogins) bool {
	return len(qtTrimmedBytes(logins.claude)) == 0 && len(qtTrimmedBytes(logins.codex)) == 0 && len(agentEnvFileContents(logins.env)) == 0
}

func describeAgentLogins(logins agentLogins) string {
	parts := []string{}
	if len(qtTrimmedBytes(logins.claude)) > 0 {
		parts = append(parts, "Claude Code login")
	}
	if len(qtTrimmedBytes(logins.codex)) > 0 {
		parts = append(parts, "Codex login")
	}
	names := []string{}
	for name, value := range logins.env {
		if agentEnvEntryUsable(name, value) {
			names = append(names, name)
		}
	}
	sort.Strings(names)
	return strings.Join(append(parts, names...), ", ")
}

func credentialDocument(data []byte) bool {
	var object map[string]any
	return json.Unmarshal(data, &object) == nil && object != nil
}

func appendSection(payload []byte, name string, data []byte) []byte {
	encoded := make([]byte, base64.StdEncoding.EncodedLen(len(data)))
	base64.StdEncoding.Encode(encoded, data)
	payload = append(payload, name...)
	payload = append(payload, ' ')
	payload = append(payload, encoded...)
	payload = append(payload, '\n')
	wipeBytes(encoded)
	return payload
}

func buildAgentCliBootstrapPayload(logins agentLogins) ([]byte, string) {
	var payload []byte
	fail := func(message string) ([]byte, string) {
		wipeBytes(payload)
		return nil, message
	}
	if claude := qtTrimmedBytes(logins.claude); len(claude) > 0 {
		if len(claude) > agentCredentialSectionCap || !credentialDocument(claude) {
			return fail("This device's Claude Code login file is not readable as a credential document.")
		}
		payload = appendSection(payload, "claude", claude)
	}
	if codex := qtTrimmedBytes(logins.codex); len(codex) > 0 {
		if len(codex) > agentCredentialSectionCap || !credentialDocument(codex) {
			return fail("This device's Codex login file is not readable as a credential document.")
		}
		payload = appendSection(payload, "codex", codex)
	}
	if env := agentEnvFileContents(logins.env); len(env) > 0 {
		payload = appendSection(payload, "env", env)
		wipeBytes(env)
	}
	if len(payload) == 0 {
		return fail("This device has no Claude Code or Codex login to copy.")
	}
	return payload, ""
}

func (m *Module) agentCredentials() agentCredentials {
	logins := m.localAgentLogins()
	defer logins.wipe()
	if agentLoginsEmpty(logins) {
		return agentCredentials{Empty: true}
	}
	payload, failure := buildAgentCliBootstrapPayload(logins)
	return agentCredentials{Payload: payload, Summary: describeAgentLogins(logins), Error: failure}
}

func (m *Module) agentAccess() (bool, string) {
	logins := m.localAgentLogins()
	defer logins.wipe()
	return !agentLoginsEmpty(logins), describeAgentLogins(logins)
}
