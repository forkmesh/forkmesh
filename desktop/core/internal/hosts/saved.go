package hosts

import (
	"bytes"
	"encoding/json"
	"os"
	"sort"
	"strings"

	"forkmesh.com/desktop-core/internal/qsettings"
)

const (
	hostsSetting         = "hosts/list"
	actionsVariablesKey  = "actions/variables"
	gatewayHealthSetting = "mirrors/sshGatewayHealth"
	sharedKeyName        = "forkmesh_shared_ed25519"
	legacySharedKeyName  = "vultr_mirror_ed25519"
	knownHostsName       = "known_hosts"
	accountNameSetting   = "account/nodeName"
)

var secretHostFields = []string{"pass", "password", "sshPassword", "adminPassword"}

func credentialKey(name, host, user string) string {
	var buffer bytes.Buffer
	encoder := json.NewEncoder(&buffer)
	encoder.SetEscapeHTML(false)
	_ = encoder.Encode([]string{strings.TrimSpace(name), strings.ToLower(strings.TrimSpace(host)), strings.TrimSpace(user)})
	return strings.TrimRight(buffer.String(), "\n")
}

func parseSavedHosts(text string) ([]map[string]any, map[string]string) {
	var stored []any
	if err := json.Unmarshal([]byte(text), &stored); err != nil {
		return nil, nil
	}
	migrated := map[string]string{}
	out := make([]map[string]any, 0, len(stored))
	for _, value := range stored {
		host, ok := value.(map[string]any)
		if !ok {
			out = append(out, map[string]any{})
			continue
		}
		password := ""
		for _, field := range secretHostFields {
			if secret, present := host[field]; present {
				if password == "" {
					password = jsonString(secret)
				}
				delete(host, field)
			}
		}
		if password != "" {
			migrated[credentialKey(jsonString(host["name"]), jsonString(host["ip"]), jsonString(host["user"]))] = password
		}
		out = append(out, host)
	}
	return out, migrated
}

func savedHostsFrom(settings *qsettings.Settings) ([]map[string]any, map[string]string) {
	if settings == nil {
		return nil, nil
	}
	return parseSavedHosts(settings.String(hostsSetting, ""))
}

func findSavedHost(hosts []map[string]any, name string) (map[string]any, int) {
	wanted := strings.ToLower(strings.TrimSpace(name))
	for index, host := range hosts {
		if strings.ToLower(strings.TrimSpace(jsonString(host["name"]))) == wanted {
			return host, index
		}
	}
	return nil, -1
}

func savedHostAddressNamed(hosts []map[string]any, node string) string {
	for _, host := range hosts {
		if strings.EqualFold(strings.TrimSpace(jsonString(host["name"])), node) {
			return strings.TrimSpace(jsonString(host["ip"]))
		}
	}
	return ""
}

func isFile(path string) bool {
	info, err := os.Stat(path)
	return err == nil && info.Mode().IsRegular()
}

func sharedHostKeyPath(sshDir string) string {
	if sshDir == "" {
		return ""
	}
	shared := joinPath(sshDir, sharedKeyName)
	if isFile(shared) {
		return shared
	}
	legacy := joinPath(sshDir, legacySharedKeyName)
	if isFile(legacy) {
		return legacy
	}
	return shared
}

func existingSharedHostIdentityFile(sshDir string) string {
	path := sharedHostKeyPath(sshDir)
	if path != "" && isFile(path) {
		return path
	}
	return ""
}

func savedHostIdentityFile(hosts []map[string]any, sshDir, name, ip, user string) string {
	for _, host := range hosts {
		if jsonString(host["name"]) != name || jsonString(host["ip"]) != ip || jsonString(host["user"]) != user {
			continue
		}
		if identity := strings.TrimSpace(jsonString(host["identityFile"])); identity != "" {
			return identity
		}
		break
	}
	return existingSharedHostIdentityFile(sshDir)
}

func vultrKeyFromSettings(settings *qsettings.Settings) string {
	if settings == nil {
		return ""
	}
	value, ok := settings.Value(actionsVariablesKey)
	if !ok {
		return ""
	}
	raw := value.Bytes
	if value.Kind != "bytes" {
		raw = []byte(value.String())
	}
	var variables map[string]any
	if json.Unmarshal(raw, &variables) != nil {
		return ""
	}
	return vultrKeyFromVariables(variables)
}

func vultrKeyFromVaultVariables(variables map[string]string) string {
	values := make(map[string]any, len(variables))
	for key, value := range variables {
		values[key] = value
	}
	return vultrKeyFromVariables(values)
}

func vultrKeyFromVariables(variables map[string]any) string {
	keys := make([]string, 0, len(variables))
	for key := range variables {
		keys = append(keys, key)
	}
	sort.Strings(keys)
	for _, name := range []string{"VULTR_API_KEY", "VULTR_API_TOKEN", "VULTR_TOKEN", "VULTR_KEY"} {
		for _, key := range keys {
			value := variables[key]
			if !strings.EqualFold(strings.TrimSpace(key), name) {
				continue
			}
			text := strings.TrimSpace(jsonString(value))
			if text == "" || strings.ContainsAny(text, "\x00\n\r") {
				continue
			}
			return text
		}
	}
	return ""
}
