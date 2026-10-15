package hosts

import (
	"context"
	"encoding/json"
	"time"

	"forkmesh.com/desktop-core/internal/rpc"
	"forkmesh.com/desktop-core/internal/vault"
)

const environmentTimeout = 3 * time.Second

type packageFile struct {
	Name       string `json:"name"`
	Path       string `json:"path"`
	Executable bool   `json:"executable"`
}

type hostEnv struct {
	InstallURL        string        `json:"installUrl"`
	UninstallURL      string        `json:"uninstallUrl"`
	Version           string        `json:"version"`
	BuildCommit       string        `json:"buildCommit"`
	SourceDir         string        `json:"sourceDir"`
	AppBinary         string        `json:"appBinary"`
	KernelType        string        `json:"kernelType"`
	CPUArch           string        `json:"cpuArch"`
	CatalogURL        string        `json:"catalogUrl"`
	AccountsLinkURL   string        `json:"accountsLinkUrl"`
	NodeDeleteURL     string        `json:"nodeDeleteUrl"`
	ProfilePublicKey  string        `json:"profilePublicKey"`
	LinkSigner        string        `json:"linkSigner"`
	LinkUser          string        `json:"linkUser"`
	AccountOwner      string        `json:"accountOwner"`
	CanSign           bool          `json:"canSign"`
	SessionToken      string        `json:"sessionToken"`
	Admin             bool          `json:"admin"`
	LinkedNodes       []string      `json:"linkedNodes"`
	FleetAddresses    []string      `json:"fleetAddresses"`
	MirrorRoot        string        `json:"mirrorRoot"`
	ManifestDigestEnv string        `json:"manifestDigestEnv"`
	MirrorPackage     []packageFile `json:"mirrorPackage"`
	ProvisionLog      string        `json:"provisionLog"`
	CloudflareZone    string        `json:"cloudflareZone"`
	AgentCredentials  bool          `json:"agentCredentials"`
	AgentAccess       string        `json:"agentAccess"`
	FormVultrKey      string        `json:"formVultrKey"`
	UploadChecked     bool          `json:"uploadChecked"`
	DeployEnvPath     string        `json:"deployEnvPath"`

	valid bool
}

func (m *Module) cachedEnv() hostEnv {
	m.mu.Lock()
	defer m.mu.Unlock()
	return m.env
}

func (m *Module) storeEnv(env hostEnv) {
	env.valid = true
	env.AgentCredentials, env.AgentAccess = m.agentAccess()
	m.mu.Lock()
	m.env = env
	m.mu.Unlock()
}

func (m *Module) environment(ctx context.Context) hostEnv {
	if m.uiAttached() {
		callCtx, cancel := context.WithTimeout(ctx, environmentTimeout)
		raw, err := m.callUI(callCtx, "ui.hosts.environment", map[string]any{})
		cancel()
		if err == nil {
			var fresh hostEnv
			if json.Unmarshal(raw, &fresh) == nil {
				m.storeEnv(fresh)
				return m.cachedEnv()
			}
		}
	}
	return m.cachedEnv()
}

func (m *Module) handleEnvironmentPush(ctx context.Context, req *rpc.Request) (any, error) {
	var fresh hostEnv
	if err := req.Bind(&fresh); err != nil {
		return nil, err
	}
	m.storeEnv(fresh)
	return map[string]any{}, nil
}

func (m *Module) cloudflareZone(env hostEnv) string {
	if zone := trimmedSetting(m.settingString("control/cloudflareZone")); zone != "" {
		return zone
	}
	return cloudflareZoneNameFromVariables(m.actionVariables())
}

func trimmedSetting(value string) string {
	out := []rune(value)
	start, end := 0, len(out)
	for start < end && (out[start] == ' ' || out[start] == '\t' || out[start] == '\n' || out[start] == '\r') {
		start++
	}
	for end > start && (out[end-1] == ' ' || out[end-1] == '\t' || out[end-1] == '\n' || out[end-1] == '\r') {
		end--
	}
	return string(out[start:end])
}

func (m *Module) actionVariables() map[string]string {
	if m.Variables != nil {
		return m.Variables()
	}
	if m.core != nil {
		return vault.Variables(m.core)
	}
	return vault.PendingPlaintext(m.settings())
}
