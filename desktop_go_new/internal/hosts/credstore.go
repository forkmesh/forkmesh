package hosts

import (
	"context"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"

	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/vault"
)

const (
	envFileCap          = 512 * 1024
	deployEnvMissing    = "app/.env.production was not found next to this build's Worker bundle, so no file was rewritten."
	cloudflareTokenName = "CLOUDFLARE_API_TOKEN"
)

var (
	envNamePattern       = regexp.MustCompile(`^[A-Za-z_][A-Za-z0-9_]{0,127}$`)
	errVaultUnavailable  = errors.New("vault_unavailable")
	workerBundleFiles    = []string{"wrangler.toml", "pywrangler.sh", "pyproject.toml", "tools/build_dashboard_assets.py", "src/entry.py", "src/public_snapshots.py"}
	workerBundleFolders  = []string{"public", "migrations"}
	bootstrapScriptName  = "cloudflare_bootstrap.py"
	deployEnvFileName    = ".env.production"
	vaultVariableSummary = "this device's %s variable"
)

func updatedEnvAssignment(contents, name, value string) string {
	if !envNamePattern.MatchString(name) || strings.ContainsAny(value, "\n\r") {
		return contents
	}
	assignment := name + "=" + value
	linePattern := regexp.MustCompile(`^[ \t]*` + regexp.QuoteMeta(name) + `[ \t]*=`)
	lines := strings.Split(contents, "\n")
	replaced := false
	for index := 0; index < len(lines); {
		line := lines[index]
		carriage := strings.HasSuffix(line, "\r")
		if carriage {
			line = line[:len(line)-1]
		}
		if !linePattern.MatchString(line) {
			index++
			continue
		}
		if !replaced {
			if carriage {
				lines[index] = assignment + "\r"
			} else {
				lines[index] = assignment
			}
			replaced = true
			index++
			continue
		}
		lines = append(lines[:index], lines[index+1:]...)
	}
	if !replaced {
		for len(lines) > 0 && strings.TrimSpace(lines[len(lines)-1]) == "" {
			lines = lines[:len(lines)-1]
		}
		lines = append(lines, assignment)
	}
	result := strings.Join(lines, "\n")
	if !strings.HasSuffix(result, "\n") {
		result += "\n"
	}
	return result
}

func writeEnvAssignments(path string, values map[string]string) (bool, string) {
	info, err := os.Lstat(path)
	exists := err == nil
	contents := ""
	if exists {
		if info.Mode()&os.ModeSymlink != 0 || !info.Mode().IsRegular() {
			return false, fmt.Sprintf("%s is not a regular file.", path)
		}
		if info.Size() > envFileCap {
			return false, fmt.Sprintf("%s is unexpectedly large.", path)
		}
		raw, err := os.ReadFile(path)
		if err != nil {
			return false, fmt.Sprintf("Could not read %s.", path)
		}
		contents = strings.ToValidUTF8(string(raw), "�")
		wipeBytes(raw)
	}
	before := contents
	names := make([]string, 0, len(values))
	for name := range values {
		names = append(names, name)
	}
	sort.Strings(names)
	for _, name := range names {
		contents = updatedEnvAssignment(contents, name, values[name])
	}
	if exists && contents == before {
		return false, ""
	}
	temp, err := os.CreateTemp(filepath.Dir(path), "."+filepath.Base(path)+".*")
	if err != nil {
		return true, fmt.Sprintf("Could not write %s.", path)
	}
	tempPath := temp.Name()
	failed := func(message string) (bool, string) {
		_ = temp.Close()
		_ = os.Remove(tempPath)
		return true, message
	}
	if temp.Chmod(0o600) != nil {
		return failed(fmt.Sprintf("Could not write %s.", path))
	}
	if _, err := temp.WriteString(contents); err != nil || temp.Sync() != nil {
		return failed(fmt.Sprintf("Could not commit %s.", path))
	}
	if temp.Close() != nil || os.Rename(tempPath, path) != nil {
		_ = os.Remove(tempPath)
		return true, fmt.Sprintf("Could not commit %s.", path)
	}
	return true, ""
}

func readableFile(path string) bool {
	info, err := os.Stat(path)
	if err != nil || !info.Mode().IsRegular() {
		return false
	}
	file, err := os.Open(path)
	if err != nil {
		return false
	}
	_ = file.Close()
	return true
}

func readableDir(path string) bool {
	info, err := os.Stat(path)
	if err != nil || !info.IsDir() {
		return false
	}
	file, err := os.Open(path)
	if err != nil {
		return false
	}
	_ = file.Close()
	return true
}

func canonicalPath(path string) string {
	absolute, err := filepath.Abs(path)
	if err != nil {
		return ""
	}
	resolved, err := filepath.EvalSymlinks(absolute)
	if err != nil {
		return ""
	}
	return resolved
}

func completeWorkerBundle(script string) string {
	worker := filepath.Join(filepath.Dir(script), "..", "app")
	for _, name := range workerBundleFiles {
		if !readableFile(filepath.Join(worker, name)) {
			return ""
		}
	}
	for _, name := range workerBundleFolders {
		if !readableDir(filepath.Join(worker, name)) {
			return ""
		}
	}
	return canonicalPath(worker)
}

func installedToolCandidates(name, appDir string) []string {
	candidates := []string{}
	if override := strings.TrimSpace(os.Getenv("FORKMESH_RESOURCE_DIR")); override != "" {
		candidates = append(candidates, filepath.Join(override, "tools", name))
	}
	if appDir == "" {
		return candidates
	}
	return append(candidates,
		filepath.Join(appDir, "..", "share", "forkmesh", "tools", name),
		filepath.Join(appDir, "..", "Resources", "forkmesh", "tools", name),
		filepath.Join(appDir, "resources", "forkmesh", "tools", name))
}

func siteDeployEnvFilePath(sourceDir, appDir string) string {
	candidates := []string{}
	if override := strings.TrimSpace(os.Getenv("FORKMESH_CLOUDFLARE_BOOTSTRAP")); override != "" {
		candidates = append(candidates, override)
	}
	if source := strings.TrimSpace(sourceDir); source != "" {
		candidates = append(candidates, filepath.Join(source, "..", "tools", bootstrapScriptName), filepath.Join(source, "tools", bootstrapScriptName))
	}
	if appDir != "" {
		candidates = append(candidates, filepath.Join(appDir, "..", "tools", bootstrapScriptName), filepath.Join(appDir, "tools", bootstrapScriptName))
	}
	candidates = append(candidates, installedToolCandidates(bootstrapScriptName, appDir)...)
	for _, candidate := range candidates {
		if !readableFile(candidate) {
			continue
		}
		resolved := canonicalPath(candidate)
		if resolved == "" {
			continue
		}
		if worker := completeWorkerBundle(resolved); worker != "" {
			return filepath.Join(worker, deployEnvFileName)
		}
	}
	return ""
}

func (m *Module) deployEnvPath() string {
	env := m.cachedEnv()
	if path := strings.TrimSpace(env.DeployEnvPath); path != "" {
		return path
	}
	appDir := ""
	if binary := strings.TrimSpace(env.AppBinary); binary != "" {
		appDir = filepath.Dir(binary)
	}
	return siteDeployEnvFilePath(env.SourceDir, appDir)
}

func (m *Module) vaultModule() *vault.Module {
	if m.core == nil {
		return nil
	}
	module, _ := m.core.Module("vault").(*vault.Module)
	return module
}

func (m *Module) storeActionVariables(values map[string]string) error {
	if len(values) == 0 {
		return nil
	}
	module := m.vaultModule()
	if module == nil || module.Store() == nil {
		return errVaultUnavailable
	}
	if _, err := module.Apply(vault.ActionsNamespace, values, nil, false); err != nil {
		return err
	}
	markers := map[string]any{}
	for name, value := range vault.PlaintextVariables(m.settings()) {
		markers[name] = value
	}
	changed := false
	for name := range values {
		if markers[name] != vault.Marker {
			markers[name] = vault.Marker
			changed = true
		}
	}
	if changed {
		m.writeSettings(daemon.SettingWrite{Key: vault.VariablesSetting, Type: "json", Value: markers})
	}
	m.broadcastUI("core.hosts.variablesChanged", map[string]any{"names": sortedVariableNames(values)})
	return nil
}

func vaultFailure(err error) string {
	code := "vault_io"
	for _, known := range []error{errVaultUnavailable, vault.ErrFormat, vault.ErrTampered, vault.ErrNoIdentity, vault.ErrInvalidName, vault.ErrTooLarge} {
		if errors.Is(err, known) {
			code = known.Error()
		}
	}
	if errors.Is(err, vault.ErrIdentityMismatch) {
		code = "vault_locked"
	}
	return "The credential was not saved: the encrypted vault is unavailable (" + code + "), and ForkMesh does not write it to ForkMesh.conf in plaintext."
}

func (m *Module) rememberVultrKey(ctx context.Context, apiKey string) {
	stores, failure := m.storeVultrKey(apiKey)
	if len(stores) == 0 {
		return
	}
	m.appendOutput(installChannel, "Saved your Vultr API key to "+strings.Join(stores, " and ")+".\n")
	if failure != "" {
		m.appendOutput(installChannel, failure+"\n")
	}
}

func (m *Module) storeVultrKey(apiKey string) ([]string, string) {
	key := strings.TrimSpace(apiKey)
	m.mu.Lock()
	remembered := m.vultrKept
	m.mu.Unlock()
	if key == "" || key == remembered || strings.ContainsAny(key, "\n\r") {
		return nil, ""
	}
	canonical := vultrKeyVariableNames[0]
	variables := m.actionVariables()
	updates := map[string]string{}
	haveCanonical := false
	for name, value := range variables {
		trimmed := strings.TrimSpace(name)
		known := false
		for _, candidate := range vultrKeyVariableNames {
			if strings.EqualFold(trimmed, candidate) {
				known = true
				break
			}
		}
		if !known {
			continue
		}
		if strings.EqualFold(trimmed, canonical) {
			haveCanonical = true
		}
		if strings.TrimSpace(value) != key {
			updates[name] = key
		}
	}
	if !haveCanonical {
		updates[canonical] = key
	}
	applied := []string{}
	retryable := false
	failure := ""
	if len(updates) > 0 {
		if err := m.storeActionVariables(updates); err != nil {
			retryable = true
			failure = vaultFailure(err)
		} else {
			applied = append(applied, fmt.Sprintf(vaultVariableSummary, canonical))
		}
	}
	envError := ""
	if path := m.deployEnvPath(); path == "" {
		envError = deployEnvMissing
	} else if changed, message := writeEnvAssignments(path, map[string]string{canonical: key}); message != "" {
		envError = message
		retryable = true
	} else if changed {
		applied = append(applied, path)
	}
	if !retryable {
		m.mu.Lock()
		m.vultrKept = key
		m.mu.Unlock()
	}
	return applied, strings.TrimSpace(strings.Join([]string{failure, envError}, " "))
}

func (m *Module) rememberCloudflareToken(ctx context.Context, token string) ([]string, string) {
	key := strings.TrimSpace(token)
	if key == "" || strings.ContainsAny(key, "\n\r") {
		return nil, ""
	}
	applied := []string{}
	failure := ""
	if m.actionVariables()[cloudflareTokenName] != key {
		if err := m.storeActionVariables(map[string]string{cloudflareTokenName: key}); err != nil {
			failure = vaultFailure(err)
		} else {
			applied = append(applied, fmt.Sprintf(vaultVariableSummary, cloudflareTokenName))
		}
	}
	envError := ""
	if path := m.deployEnvPath(); path == "" {
		envError = deployEnvMissing
	} else if changed, message := writeEnvAssignments(path, map[string]string{cloudflareTokenName: key}); message != "" {
		envError = message
	} else if changed {
		applied = append(applied, path)
	}
	return applied, strings.TrimSpace(strings.Join([]string{failure, envError}, " "))
}
