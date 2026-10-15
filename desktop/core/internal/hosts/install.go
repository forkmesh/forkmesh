package hosts

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"net/url"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"unicode/utf8"

	"forkmesh.com/desktop-core/internal/rpc"
)

const (
	hostUploadMarker        = "__FORKMESH_UPLOAD__"
	vultrMirrorUploadMarker = "__FORKMESH_GO_MIRROR_V1__"
	installRawTailChars     = 4000
	installLinkTailChars    = 512
)

var (
	linkCodePattern   = regexp.MustCompile(`FORKMESH LINK CODE:\s*([0-9]{6})`)
	runnerTokenRegexp = regexp.MustCompile(`fmrt_[0-9a-f]{8,}`)
	exactCommit       = regexp.MustCompile(`^(?:[0-9a-f]{40}|[0-9a-f]{64})$`)
	exactDigest       = regexp.MustCompile(`^[0-9a-f]{64}$`)
	exactVersion      = regexp.MustCompile(`^[0-9]+\.[0-9]+\.[0-9]+$`)
	sixDigits         = regexp.MustCompile(`^[0-9]{6}$`)
)

func qtArg(template string, args ...string) string {
	var out strings.Builder
	for i := 0; i < len(template); i++ {
		c := template[i]
		if c != '%' || i+1 >= len(template) || template[i+1] < '0' || template[i+1] > '9' {
			out.WriteByte(c)
			continue
		}
		digits := 1
		if i+2 < len(template) && template[i+2] >= '0' && template[i+2] <= '9' {
			digits = 2
		}
		number, _ := strconv.Atoi(template[i+1 : i+1+digits])
		if number >= 1 && number <= len(args) {
			out.WriteString(args[number-1])
			i += digits
			continue
		}
		out.WriteByte(c)
	}
	return out.String()
}

func shq(value string) string { return "'" + strings.ReplaceAll(value, "'", `'\''`) + "'" }

func redactRunnerToken(text string) string {
	return runnerTokenRegexp.ReplaceAllString(text, "fmrt_[hidden]")
}

func withAgentCliServiceRefresh(pipeline string) string {
	return qtArg(withAgentCliServiceRefreshTemplate, pipeline, agentCliServiceRefreshScript)
}

func rightRunes(text string, limit int) string {
	if utf16Len(text) <= limit {
		return text
	}
	runes := []rune(text)
	units := 0
	for i := len(runes) - 1; i >= 0; i-- {
		width := 1
		if runes[i] >= 0x10000 {
			width = 2
		}
		if units+width > limit {
			return string(runes[i+1:])
		}
		units += width
	}
	return text
}

type publishedRelease struct {
	Tag               string   `json:"tag"`
	Version           string   `json:"version"`
	BuildCommit       string   `json:"buildCommit"`
	ManifestSha256    string   `json:"manifestSha256"`
	LinuxX86_64Sha256 string   `json:"linuxX86_64Sha256"`
	LinuxArm64Sha256  string   `json:"linuxArm64Sha256"`
	ArtifactSha256s   []string `json:"artifactSha256s"`
	Owner             string   `json:"owner"`
	Name              string   `json:"name"`
	RefsReady         *bool    `json:"refsReady"`
}

func (r publishedRelease) valid() bool {
	return r.Tag != "" && r.Version != "" && r.BuildCommit != "" && r.ManifestSha256 != "" &&
		len(r.ArtifactSha256s) > 0 && (r.LinuxX86_64Sha256 != "" || r.LinuxArm64Sha256 != "")
}

type installOptions struct {
	UploadBinary   bool
	Reinstall      bool
	FromSource     bool
	RequirePublish bool
	Release        publishedRelease
	Vultr          *vultrInstallTarget
}

type vultrInstallTarget struct {
	DNSHostname string
	TunnelToken string
}

func (e hostEnv) relayHost() string {
	parsed, err := url.Parse(e.CatalogURL)
	if err != nil {
		return ""
	}
	return parsed.Hostname()
}

func controllerReleaseManifestDigest(env hostEnv, expectedBuildCommit string) (string, error) {
	configured := strings.ToLower(strings.TrimSpace(env.ManifestDigestEnv))
	if configured != "" {
		if exactDigest.MatchString(configured) {
			return configured, nil
		}
		return "", errors.New("FORKMESH_EXPECTED_RELEASE_MANIFEST_SHA256 must be an exact 64-hex SHA-256 digest.")
	}
	if env.SourceDir != "" {
		path := filepath.Join(env.SourceDir, ".forkmesh/releases/latest/release.json")
		if info, err := os.Stat(path); err == nil && info.Size() > 0 && info.Size() <= 1024*1024 {
			if raw, err := os.ReadFile(path); err == nil {
				var manifest map[string]any
				if json.Unmarshal(raw, &manifest) == nil {
					tag := jsonString(manifest["tag"])
					checksums := strings.ToLower(strings.TrimSpace(jsonString(manifest["checksums_sha256"])))
					if jsonString(manifest["schema"]) == "forkmesh-release-v2" &&
						strings.ToLower(strings.TrimSpace(jsonString(manifest["build_commit"]))) == expectedBuildCommit &&
						(tag == env.Version || tag == "v"+env.Version) && exactDigest.MatchString(checksums) {
						sum := sha256.Sum256(raw)
						return hex.EncodeToString(sum[:]), nil
					}
				}
			}
		}
	}
	return "", errors.New("No controller-trusted release manifest matches this build. Refresh the checked-in signed release metadata or set FORKMESH_EXPECTED_RELEASE_MANIFEST_SHA256 from an authenticated control channel before deploying a fleet binary.")
}

func uploadOS(kernel string) string {
	switch kernel {
	case "darwin":
		return "macos"
	case "winnt":
		return "windows"
	}
	return kernel
}

func (m *Module) buildVultrMirrorNodeInstallCommand(env hostEnv, node, publicAddress, dnsHostname string) (string, []byte, error) {
	type payloadFile struct {
		name string
		data []byte
	}
	wanted := []string{"forkmesh-mirror-node", "mirror_gateway.py", "mirror_replica.py", "public_snapshots.py", "portable_sessions.py",
		"direct_https_bootstrap.py", "node_operator_policy.py", "secret_gate.py", "forkmesh-mirror-node.service"}
	paths := map[string]string{}
	for _, file := range env.MirrorPackage {
		paths[file.Name] = file.Path
	}
	files := make([]payloadFile, 0, len(wanted))
	for _, name := range wanted {
		path := paths[name]
		if path == "" {
			return "", nil, fmt.Errorf("The Go mirror-node package is incomplete (%s is missing). Reinstall this ForkMesh release and retry.", name)
		}
		data, err := os.ReadFile(path)
		if err != nil || len(data) == 0 {
			return "", nil, fmt.Errorf("Could not read packaged %s.", name)
		}
		files = append(files, payloadFile{name: name, data: data})
	}
	payload := []byte(vultrMirrorUploadMarker + "\n" + strconv.Itoa(len(files)) + "\n")
	for _, file := range files {
		digest := sha256.Sum256(file.data)
		payload = append(payload, []byte(file.name+"\n"+strconv.Itoa(len(file.data))+"\n"+hex.EncodeToString(digest[:])+"\n")...)
		payload = append(payload, file.data...)
	}
	hostname := strings.ToLower(strings.TrimSpace(dnsHostname))
	zone := ""
	if dot := strings.IndexByte(hostname, '.'); dot >= 0 {
		zone = hostname[dot+1:]
	}
	catalog, _ := url.Parse(env.CatalogURL)
	accounts := ""
	relayHost := ""
	if catalog != nil {
		relayHost = catalog.Hostname()
		copyURL := *catalog
		copyURL.Path = "/api/accounts"
		copyURL.RawPath = ""
		copyURL.RawQuery = ""
		copyURL.ForceQuery = false
		accounts = copyURL.String()
	}
	if hostname == "" || zone == "" || relayHost == "" || strings.TrimSpace(publicAddress) == "" {
		return "", nil, errors.New("The Vultr mirror hostname, public address, or relay endpoint is unavailable.")
	}
	pipeline := qtArg(vultrMirrorPipelineTemplate,
		shq(vultrMirrorUploadMarker),
		shq("https://"+relayHost+"/api/mirrors/https"),
		shq(node),
		shq("https://"+relayHost+"/forkmesh/forkmesh"),
		shq(env.CatalogURL),
		shq("https://"+hostname),
		shq(env.Version),
		shq(hostname),
		shq(zone),
		shq(accounts),
		shq(strings.TrimSpace(publicAddress)),
		shq(env.ProfilePublicKey))
	return pipeline, payload, nil
}

func (m *Module) buildHostInstallCommand(env hostEnv, ip, user, node string, opts installOptions) (string, []byte, error) {
	if opts.Vultr != nil {
		return m.buildVultrMirrorNodeInstallCommand(env, node, ip, opts.Vultr.DNSHostname)
	}
	installURL := env.InstallURL
	if installURL == "" {
		return "", nil, errors.New("Could not resolve the installer URL.")
	}
	doUpload := opts.UploadBinary && !opts.FromSource && !opts.RequirePublish
	var bytes []byte
	if doUpload {
		data, err := os.ReadFile(env.AppBinary)
		if err != nil || len(data) == 0 {
			return "", nil, fmt.Errorf("Could not read this app's binary (%s) to upload it.", env.AppBinary)
		}
		bytes = data
	}
	expectedBuildCommit := ""
	expectedDigest := ""
	expectedVersion := env.Version
	if opts.RequirePublish {
		if opts.Release.valid() {
			expectedBuildCommit = strings.ToLower(strings.TrimSpace(opts.Release.BuildCommit))
			expectedVersion = strings.TrimSpace(opts.Release.Version)
			expectedDigest = strings.ToLower(strings.TrimSpace(opts.Release.ManifestSha256))
		} else {
			expectedBuildCommit = strings.ToLower(strings.TrimSpace(env.BuildCommit))
		}
		if !exactCommit.MatchString(expectedBuildCommit) {
			return "", nil, errors.New("This ForkMesh build has no exact source revision, so it cannot prove that a published same-version binary is current. Rebuild from a Git checkout (or configure a release archive with FORKMESH_BUILD_COMMIT_OVERRIDE), then retry.")
		}
		if !opts.Release.valid() {
			digest, err := controllerReleaseManifestDigest(env, expectedBuildCommit)
			if err != nil {
				return "", nil, err
			}
			expectedDigest = digest
		}
		if !exactDigest.MatchString(expectedDigest) || !exactVersion.MatchString(expectedVersion) {
			return "", nil, errors.New("The selected published release has invalid version or manifest provenance.")
		}
	}
	if opts.RequirePublish && opts.Release.valid() {
		blobBase, err := url.Parse(installURL)
		if err != nil || blobBase.Host == "" {
			return "", nil, errors.New("Could not resolve the native release artifact endpoint.")
		}
		blobBase.Path = "/api/repo/forkmesh/forkmesh/releases/blob/sha256/"
		blobBase.RawPath = ""
		blobBase.RawQuery = ""
		blobBase.ForceQuery = false
		blobBase.Fragment = ""
		blobPrefix := blobBase.String()
		pipeline := qtArg(nativeReleasePipelineTemplate, shq(opts.Release.LinuxX86_64Sha256), shq(opts.Release.LinuxArm64Sha256),
			shq(blobPrefix), shq(expectedVersion))
		refreshed := withAgentCliServiceRefresh(pipeline)
		if user == "root" {
			return refreshed, nil, nil
		}
		return "sudo -S -p '' -- bash -c " + shq(refreshed), nil, nil
	}
	envPrefix := "FORKMESH_NODE_NAME=" + shq(node)
	if env.LinkUser != "" {
		envPrefix += " FORKMESH_OWNER=" + shq(env.LinkUser)
	}
	if opts.Reinstall {
		envPrefix += " FORKMESH_REINSTALL=1"
	}
	if opts.FromSource {
		envPrefix += " FORKMESH_FROM_SOURCE=1 FORKMESH_RESTART=1"
	}
	if opts.RequirePublish {
		envPrefix += " FORKMESH_RELEASE=latest FORKMESH_NO_SOURCE_FALLBACK=1 FORKMESH_EXPECTED_BUILD_COMMIT=" + shq(expectedBuildCommit) +
			" FORKMESH_EXPECTED_RELEASE_VERSION=" + shq(expectedVersion) +
			" FORKMESH_EXPECTED_RELEASE_MANIFEST_SHA256=" + shq(expectedDigest)
		if !opts.Reinstall {
			envPrefix += " FORKMESH_RESTART=1"
		}
	}
	var pipeline string
	if opts.RequirePublish {
		pipeline = qtArg(verifiedInstallPipelineTemplate, shq(installURL), envPrefix, shq("ForkMesh "+expectedVersion), shq(expectedBuildCommit))
	} else {
		pipeline = "curl -fsSL " + shq(installURL) + " | " + envPrefix + " bash"
	}
	if doUpload {
		digest := sha256.Sum256(bytes)
		pipeline = qtArg(uploadPipelineTemplate, shq(hostUploadMarker), shq(installURL), envPrefix, shq(uploadOS(env.KernelType)),
			shq(env.CPUArch), shq(hex.EncodeToString(digest[:])))
	}
	pipeline = withAgentCliServiceRefresh(pipeline)
	if user != "root" {
		return "sudo -S -p '' -- bash -c " + shq(pipeline), bytes, nil
	}
	return pipeline, bytes, nil
}

type utf8Carry struct{ pending []byte }

func (c *utf8Carry) decode(chunk []byte) string {
	data := append(c.pending, chunk...)
	c.pending = nil
	cut := len(data)
	for back := 1; back <= 3 && back <= len(data); back++ {
		b := data[len(data)-back]
		if b&0xC0 == 0x80 {
			continue
		}
		if b >= 0xC0 {
			need := 2
			if b >= 0xF0 {
				need = 4
			} else if b >= 0xE0 {
				need = 3
			}
			if back < need {
				cut = len(data) - back
			}
		}
		break
	}
	text := data[:cut]
	c.pending = append([]byte(nil), data[cut:]...)
	if utf8.Valid(text) {
		return string(text)
	}
	return strings.ToValidUTF8(string(text), "�")
}

type installRequest struct {
	Node                  string `json:"node"`
	IP                    string `json:"ip"`
	User                  string `json:"user"`
	Password              string `json:"password"`
	ForceUpload           bool   `json:"forceUpload"`
	UploadChecked         bool   `json:"uploadChecked"`
	Reinstall             bool   `json:"reinstall"`
	FromSource            bool   `json:"fromSource"`
	SuppressFailureStatus bool   `json:"suppressFailureStatus"`
	AttemptBanner         string `json:"attemptBanner"`
	Origin                string `json:"-"`
	Vultr                 *vultrInstallTarget
}

func (m *Module) installBusyLocal() bool {
	return m.jobRunning("install", "uninstall") || m.deployRemaining() > 0
}

func (m *Module) installBusy() bool {
	return m.provisionActive() || m.installBusyLocal()
}

func (m *Module) failInstallStart(job *Job, message string) {
	if message != "" {
		m.setStatus("install", message)
	}
	if job != nil {
		m.finishJob(job, false, message, map[string]any{"started": false})
	}
}

func (m *Module) runHostInstall(ctx context.Context, req installRequest, done func(ok bool, lastFailure string)) (*Job, string) {
	finish := func(ok bool, lastFailure string) {
		if done != nil {
			done(ok, lastFailure)
		}
	}
	banner := req.AttemptBanner
	if m.installBusyLocal() {
		m.setStatus("install", "An install is already running.")
		finish(false, "")
		return nil, "An install is already running."
	}
	ip, user, node, pass := strings.TrimSpace(req.IP), strings.TrimSpace(req.User), strings.TrimSpace(req.Node), req.Password
	if ip == "" || user == "" || node == "" {
		message := "Enter the host IP, SSH username and a node name first. Leave the password blank to use the shared ForkMesh SSH key."
		m.setStatus("install", message)
		finish(false, "")
		return nil, message
	}
	env := m.environment(ctx)
	if env.InstallURL == "" {
		m.setStatus("install", "Could not resolve the installer URL.")
		finish(false, "")
		return nil, "Could not resolve the installer URL."
	}
	uploadBinary := !req.FromSource && (req.ForceUpload || req.UploadChecked)
	needSudo := user != "root"
	remoteCmd, uploadBytes, err := m.buildHostInstallCommand(env, ip, user, node, installOptions{UploadBinary: uploadBinary,
		Reinstall: req.Reinstall, FromSource: req.FromSource, Vultr: req.Vultr})
	if err != nil {
		m.setLastInstallFailure(err.Error())
		m.setStatus("install", err.Error())
		finish(false, err.Error())
		return nil, err.Error()
	}
	hasUpload := len(uploadBytes) > 0
	command, err := buildHostSSHCommand(m.sshDir(), ip, user, pass, remoteCmd, savedHostIdentityFile(m.savedHosts(), m.sshDir(), node, ip, user))
	if err != nil {
		m.setStatus("install", err.Error())
		finish(false, "")
		return nil, err.Error()
	}
	m.rememberHost(node, ip, user, pass, "installing", "", nil)
	job := m.startJob("install", node, "", req.Origin, installChannel)
	if banner == "" {
		m.clearOutput(installChannel)
	} else {
		m.appendOutput(installChannel, "\n──── "+banner+" ────\n")
	}
	m.setLastInstallFailure("")
	m.appendOutput(installChannel, "$ ssh "+user+"@"+ip+" "+redactRunnerToken(remoteCmd)+"\n")
	if req.Vultr != nil {
		m.appendOutput(installChannel, m.timestamped("Connecting to "+ip+" as "+user+" and starting the native mirror bootstrap ...\n\n"))
	} else {
		m.appendOutput(installChannel, m.timestamped("Connecting to "+ip+" as "+user+" and running "+env.InstallURL+" ...\n\n"))
	}
	if hasUpload {
		m.appendOutput(installChannel, m.timestamped(fmt.Sprintf("Uploading the native mirror package (%.1f MB) over the SSH session…\n", float64(len(uploadBytes))/(1024.0*1024.0))))
		m.setStatus("install", "Uploading the release and installing on "+ip+"…")
	} else {
		m.setStatus("install", "Installing on "+ip+"…")
	}
	stdin := []byte{}
	if needSudo {
		stdin = append(stdin, []byte(pass+"\n")...)
	}
	if req.Vultr != nil && req.Vultr.DNSHostname != "" && req.Vultr.TunnelToken != "" {
		stdin = append(stdin, []byte(req.Vultr.TunnelToken+"\n")...)
	}
	if hasUpload {
		if req.Vultr == nil {
			stdin = append(stdin, []byte(hostUploadMarker+"\n")...)
		}
		stdin = append(stdin, uploadBytes...)
	}
	go func() {
		rawTail := ""
		linkTail := ""
		linkPrompted := false
		carry := &utf8Carry{}
		result := runProcess(job.Context(), procSpec{Program: command.Program, Args: command.Args, Env: command.Env, Stdin: stdin,
			OnOutput: func(chunk []byte) {
				text := redactRunnerToken(carry.decode(chunk))
				if text == "" {
					return
				}
				m.appendOutput(installChannel, text)
				rawTail = rightRunes(rawTail+text, installRawTailChars)
				if linkPrompted {
					return
				}
				linkTail = rightRunes(linkTail+text, installLinkTailChars)
				if match := linkCodePattern.FindStringSubmatch(linkTail); match != nil {
					linkPrompted = true
					m.handleInstallLinkCode(match[1], installChannel, true)
				}
			}})
		if !result.Started {
			m.appendOutput(installChannel, "\n[error] Could not start SSH. Install OpenSSH (and sshpass only when using password login) and try again.\n")
		}
		ok := result.OK()
		verb := "Install"
		if req.Reinstall {
			verb = "Reinstall"
		}
		lastFailure := ""
		code := result.ExitCode
		if ok {
			m.appendOutput(installChannel, "\n"+m.timestamped(fmt.Sprintf("✔ %s finished. Node \"%s\" will join the network and appear on Network > Nodes shortly.\n", verb, node)))
			m.setStatus("install", fmt.Sprintf("✔ %sed on %s as node \"%s\".", verb, ip, node))
			m.rememberHost(node, ip, user, pass, "installed", "", nil)
		} else {
			m.appendOutput(installChannel, "\n"+m.timestamped(fmt.Sprintf("✘ %s failed (exit %d).\n", verb, code)))
			lastFailure = sshFailureSummary(code, rawTail)
			m.setLastInstallFailure(lastFailure)
			if hint := sshConnectionFailureHint(code, rawTail, ip); hint != "" {
				m.appendOutput(installChannel, "\n"+hint+"\n")
			}
			if !req.SuppressFailureStatus {
				m.setStatus("install", "✘ Install failed — see the output above.")
				m.rememberHost(node, ip, user, pass, "install failed", "", nil)
			}
		}
		m.finishJob(job, ok, m.status("install"), map[string]any{"lastFailure": lastFailure, "exitCode": code})
		finish(ok, lastFailure)
	}()
	return job, ""
}

func (m *Module) setLastInstallFailure(text string) {
	m.mu.Lock()
	m.lastInstallFailure = text
	m.mu.Unlock()
}

func (m *Module) lastFailure() string {
	m.mu.Lock()
	defer m.mu.Unlock()
	return m.lastInstallFailure
}

func (m *Module) handleInstallLinkCode(code, channel string, prompt bool) {
	env := m.cachedEnv()
	signer, linkUser := m.linkSigner(env)
	if signer != "" {
		m.appendOutput(channel, "\nLinking node to your user account ("+linkUser+")…\n")
		go m.submitHostLinkCode(m.ctx, code)
		return
	}
	if channel == installChannel {
		m.appendOutput(channel, "\n[warning] Not auto-linking this host: this node is not linked to a user account. Link this node to your user account, then reinstall the host to attach it.\n")
		if prompt {
			m.broadcastUI("core.hosts.linkPrompt", map[string]any{"code": code})
		}
		return
	}
	m.appendOutput(channel, "\n[warning] Not auto-linking: this app is not linked to a user account.\n")
}

func (m *Module) linkSigner(env hostEnv) (string, string) {
	if env.LinkSigner == "" || env.LinkUser == "" {
		return "", ""
	}
	if m.core == nil {
		return "", ""
	}
	if _, err := m.core.RequireIdentity(); err != nil {
		return "", ""
	}
	return env.LinkSigner, env.LinkUser
}

func (m *Module) submitHostLinkCode(ctx context.Context, code string) (bool, string) {
	env := m.environment(ctx)
	signer, linkUser := m.linkSigner(env)
	if signer == "" {
		return false, ""
	}
	id, err := m.core.RequireIdentity()
	if err != nil {
		return false, ""
	}
	ts := strconv.FormatInt(m.nowMs(), 10)
	canonical := "forkmesh-link-v1\n" + signer + "\n" + code + "\n" + ts
	body := map[string]any{"nodeName": signer, "code": code, "ts": ts, "sig": id.Sign([]byte(canonical))}
	_, response, transport := m.postJSON(ctx, env.AccountsLinkURL, "", body)
	_ = transport
	var message string
	switch {
	case jsonBool(response["linked"]):
		user := jsonString(response["user"])
		if _, ok := response["user"].(string); !ok {
			user = linkUser
		}
		message = fmt.Sprintf("\n✔ Node \"%s\" is now linked to user \"%s\".\n", jsonString(response["node"]), user)
	case jsonBool(response["pending"]):
		user := jsonString(response["user"])
		if _, ok := response["user"].(string); !ok {
			user = linkUser
		}
		message = fmt.Sprintf("\n✔ Link code accepted; the new node will be linked to user \"%s\" as soon as it registers.\n", user)
	default:
		reason := "network error"
		if text, ok := response["error"].(string); ok {
			reason = text
		}
		message = fmt.Sprintf("\n✘ Could not link the new node (%s).\n", reason)
	}
	m.appendOutput(installChannel, message)
	m.setStatus("install", strings.TrimSpace(message))
	return !strings.Contains(message, "Could not"), strings.TrimSpace(message)
}

func jsonBool(value any) bool {
	b, _ := value.(bool)
	return b
}

func (m *Module) runHostUninstall(ctx context.Context, node, ip, user, pass, origin string) (*Job, string) {
	if m.jobRunning("install", "uninstall") {
		m.setStatus("install", "A host session is already running.")
		return nil, "A host session is already running."
	}
	ip, user, node = strings.TrimSpace(ip), strings.TrimSpace(user), strings.TrimSpace(node)
	if ip == "" || user == "" || node == "" {
		message := "Enter the host IP, SSH username and a node name first. Leave the password blank to use the shared ForkMesh SSH key."
		m.setStatus("install", message)
		return nil, message
	}
	env := m.environment(ctx)
	if env.UninstallURL == "" {
		m.setStatus("install", "Could not resolve the uninstaller URL.")
		return nil, "Could not resolve the uninstaller URL."
	}
	pipeline := qtArg(uninstallPipelineTemplate, shq(env.UninstallURL))
	needSudo := user != "root"
	remoteCmd := pipeline
	if needSudo {
		remoteCmd = "sudo -S -p '' -- bash -c " + shq(pipeline)
	}
	command, err := buildHostSSHCommand(m.sshDir(), ip, user, pass, remoteCmd, savedHostIdentityFile(m.savedHosts(), m.sshDir(), node, ip, user))
	if err != nil {
		m.setStatus("install", err.Error())
		return nil, err.Error()
	}
	m.rememberHost(node, ip, user, pass, "uninstalling", "", nil)
	job := m.startJob("uninstall", node, "", origin, installChannel)
	m.clearOutput(installChannel)
	m.appendOutput(installChannel, "$ ssh "+user+"@"+ip+" "+remoteCmd+"\n")
	m.appendOutput(installChannel, "Connecting to "+ip+" as "+user+" and running "+env.UninstallURL+" ...\n\n")
	m.setStatus("install", "Uninstalling from "+ip+"…")
	var stdin []byte
	if needSudo {
		stdin = []byte(pass + "\n")
	}
	go func() {
		carry := &utf8Carry{}
		result := runProcess(job.Context(), procSpec{Program: command.Program, Args: command.Args, Env: command.Env, Stdin: stdin,
			OnOutput: func(chunk []byte) { m.appendOutput(installChannel, carry.decode(chunk)) }})
		if !result.Started {
			m.appendOutput(installChannel, "\n[error] Could not start SSH. Install OpenSSH (and sshpass only when using password login) and try again.\n")
		}
		ok := result.OK()
		if ok {
			m.appendOutput(installChannel, fmt.Sprintf("\n✔ Uninstall finished. ForkMesh has been removed from \"%s\".\n", node))
			m.setStatus("install", "✔ Uninstalled from "+ip+".")
			m.rememberHost(node, ip, user, pass, "uninstalled", "", nil)
		} else {
			m.appendOutput(installChannel, fmt.Sprintf("\n✘ Uninstall failed (exit %d).\n", result.ExitCode))
			m.setStatus("install", "✘ Uninstall failed — see the output above.")
			m.rememberHost(node, ip, user, pass, "uninstall failed", "", nil)
		}
		m.finishJob(job, ok, m.status("install"), map[string]any{"exitCode": result.ExitCode})
	}()
	return job, ""
}

func (m *Module) registerInstallMethods(desktop func(string, rpc.Handler)) {
	desktop("core.hosts.install", m.handleInstall)
	desktop("core.hosts.uninstall", m.handleUninstall)
	desktop("core.hosts.linkCode", m.handleLinkCode)
	desktop("core.hosts.deployAll", m.handleDeployAll)
}

func (m *Module) handleInstall(ctx context.Context, req *rpc.Request) (any, error) {
	var params installRequest
	if err := req.Bind(&params); err != nil {
		return nil, err
	}
	params.Origin = "desktop"
	params.Vultr = nil
	m.rememberPassword(credentialKey(params.Node, params.IP, params.User), params.Password)
	job, why := m.runHostInstall(m.ctx, params, nil)
	if job == nil {
		return map[string]any{"started": false, "message": why}, nil
	}
	return map[string]any{"started": true, "job": job.ID}, nil
}

func (m *Module) handleUninstall(ctx context.Context, req *rpc.Request) (any, error) {
	var params struct {
		Node     string `json:"node"`
		IP       string `json:"ip"`
		User     string `json:"user"`
		Password string `json:"password"`
	}
	if err := req.Bind(&params); err != nil {
		return nil, err
	}
	job, why := m.runHostUninstall(m.ctx, params.Node, params.IP, params.User, params.Password, "desktop")
	if job == nil {
		return map[string]any{"started": false, "message": why}, nil
	}
	return map[string]any{"started": true, "job": job.ID}, nil
}

func (m *Module) handleLinkCode(ctx context.Context, req *rpc.Request) (any, error) {
	var params struct {
		Code string `json:"code"`
	}
	if err := req.Bind(&params); err != nil {
		return nil, err
	}
	code := strings.TrimSpace(params.Code)
	if !sixDigits.MatchString(code) {
		return nil, rpc.Fail("bad_code")
	}
	env := m.environment(ctx)
	if signer, _ := m.linkSigner(env); signer == "" {
		return map[string]any{"started": false}, nil
	}
	go m.submitHostLinkCode(m.ctx, code)
	return map[string]any{"started": true}, nil
}

func sshFailureSummary(exitCode int, outputTail string) string {
	reason := ""
	lines := regexp.MustCompile(`[\r\n]`).Split(outputTail, -1)
	for i := len(lines) - 1; i >= 0; i-- {
		line := strings.TrimSpace(lines[i])
		if line == "" {
			continue
		}
		reason = line
		break
	}
	if utf16Len(reason) > 160 {
		reason = leftUTF16(reason, 157) + "..."
	}
	if reason == "" {
		return fmt.Sprintf("exit %d", exitCode)
	}
	return fmt.Sprintf("exit %d — %s", exitCode, reason)
}

var ipv4Pattern = regexp.MustCompile(`^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$`)

func nonRoutableAddressNote(host string) string {
	match := ipv4Pattern.FindStringSubmatch(strings.TrimSpace(host))
	if match == nil {
		return ""
	}
	var octet [4]int
	for i := 0; i < 4; i++ {
		octet[i], _ = strconv.Atoi(match[i+1])
		if octet[i] > 255 {
			return ""
		}
	}
	switch {
	case octet[0] == 10:
		return "the private range 10.0.0.0/8 (RFC 1918)"
	case octet[0] == 172 && octet[1] >= 16 && octet[1] <= 31:
		return "the private range 172.16.0.0/12 (RFC 1918)"
	case octet[0] == 192 && octet[1] == 168:
		return "the private range 192.168.0.0/16 (RFC 1918)"
	case octet[0] == 100 && octet[1] >= 64 && octet[1] <= 127:
		return "100.64.0.0/10, the carrier-grade NAT / shared address range (RFC 6598) that VPN meshes such as Tailscale also hand out"
	case octet[0] == 169 && octet[1] == 254:
		return "the link-local range 169.254.0.0/16"
	case octet[0] == 127:
		return "the loopback range 127.0.0.0/8"
	}
	return ""
}

func sshConnectionFailureHint(exitCode int, outputTail, host string) string {
	if exitCode != 255 {
		return ""
	}
	tail := strings.ToLower(outputTail)
	dropped := strings.Contains(tail, "connection timed out") || strings.Contains(tail, "operation timed out") || strings.Contains(tail, "no route to host")
	if dropped {
		if note := nonRoutableAddressNote(host); note != "" {
			return fmt.Sprintf("%s is in %s, so it is not reachable from the public internet — the packets are dropped in transit rather than by any firewall. Unless this machine is on that same private network or VPN, use the host's public address here (or connect to the network that owns the range first).", strings.TrimSpace(host), note)
		}
	}
	switch {
	case strings.Contains(tail, "connection timed out") || strings.Contains(tail, "operation timed out"):
		return "The connection to port 22 timed out — packets are being dropped, not rejected. This almost always means a firewall or cloud security group between here and the host is blocking SSH: check the provider's firewall/security-group rules for this instance (and any local network firewall) allow inbound TCP 22 from your IP, then confirm the address is correct and the host has finished booting."
	case strings.Contains(tail, "connection refused"):
		return "The host actively refused the connection on port 22 — SSH is not listening yet (a freshly booted instance can take a minute or two) or a firewall rule is rejecting the port outright. Wait a moment and retry; if it persists, check the provider's firewall/security-group settings for this instance."
	case strings.Contains(tail, "no route to host"):
		return "There is no network route to this host — check that the address is correct and that a firewall or security group is not dropping the traffic."
	case strings.Contains(tail, "host key verification failed"):
		return "The host's SSH key does not match the one ForkMesh already trusts for it — this usually means the instance was rebuilt/reinstalled at the same address. Remove the stale entry from this app's managed known_hosts file if you intended that, then retry."
	case strings.Contains(tail, "permission denied"):
		return "The host rejected the credentials — double-check the SSH user, password, and key for this saved host."
	}
	return ""
}

func sshFailureNeedsPassword(exitCode int, outputTail string) bool {
	if exitCode != 255 {
		return false
	}
	tail := strings.ToLower(outputTail)
	return strings.Contains(tail, "permission denied") || strings.Contains(tail, "authentication failed") ||
		strings.Contains(tail, "no supported authentication methods")
}
