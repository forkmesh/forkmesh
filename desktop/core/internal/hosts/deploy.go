package hosts

import (
	"context"
	"encoding/json"
	"fmt"
	"strings"

	"forkmesh.com/desktop-core/internal/rpc"
)

type deploySession struct {
	Node     string `json:"node"`
	IP       string `json:"ip"`
	User     string `json:"user"`
	Channel  string `json:"channel"`
	Finished bool   `json:"finished"`
	OK       bool   `json:"ok"`
	Header   string `json:"header"`

	pass         string
	linkTail     string
	linkPrompted bool
}

type deployState struct {
	ID        string           `json:"id"`
	Sessions  []*deploySession `json:"sessions"`
	Remaining int              `json:"remaining"`
	Failed    int              `json:"failed"`
	Label     string           `json:"label"`
	Release   publishedRelease `json:"release"`
	RefsReady bool             `json:"refsReady"`
	Running   bool             `json:"running"`

	job  *Job
	done []func(ok bool, failed int)
}

func fleetDeployOptions(mode string) installOptions {
	switch mode {
	case "reinstall":
		return installOptions{Reinstall: true, RequirePublish: true}
	case "source":
		return installOptions{FromSource: true}
	}
	return installOptions{RequirePublish: true}
}

func (m *Module) deployRemaining() int {
	m.mu.Lock()
	defer m.mu.Unlock()
	if m.deploy == nil {
		return 0
	}
	return m.deploy.Remaining
}

func (m *Module) deploySnapshot() map[string]any {
	m.mu.Lock()
	defer m.mu.Unlock()
	if m.deploy == nil {
		return map[string]any{"running": false, "sessions": []any{}}
	}
	raw, _ := json.Marshal(m.deploy)
	out := map[string]any{}
	_ = json.Unmarshal(raw, &out)
	return out
}

func (m *Module) broadcastDeploy() {
	m.broadcastUI("core.hosts.deploy", m.deploySnapshot())
}

type deployRequest struct {
	Mode      string            `json:"mode"`
	Release   publishedRelease  `json:"release"`
	Passwords map[string]string `json:"passwords"`
	Origin    string            `json:"-"`
}

func (m *Module) runHostDeployAll(ctx context.Context, req deployRequest, onFinished func(ok bool, failed int)) (int, string) {
	if m.jobRunning("install", "uninstall") || m.deployRemaining() > 0 {
		m.setStatus("install", "A host session is already running.")
		return 0, "A host session is already running."
	}
	for key, password := range req.Passwords {
		m.rememberPassword(key, password)
	}
	release := req.Release
	if !release.valid() {
		release = publishedRelease{}
	}
	options := fleetDeployOptions(req.Mode)
	options.Release = release
	activeOnly := release.valid()
	hosts := m.rawSavedHosts()
	sessions := []*deploySession{}
	skipped := 0
	for i := range hosts {
		host := objectAt(hosts, i)
		if activeOnly && !savedHostMayBeActive(host) {
			skipped++
			continue
		}
		node := strings.TrimSpace(jsonString(host["name"]))
		ip := strings.TrimSpace(jsonString(host["ip"]))
		user := strings.TrimSpace(jsonString(host["user"]))
		if node == "" || ip == "" || user == "" {
			skipped++
			continue
		}
		sessions = append(sessions, &deploySession{Node: node, IP: ip, User: user, pass: m.password(credentialKey(node, ip, user)),
			Header: fmt.Sprintf("%s — %s@%s …", node, user, ip)})
	}
	if len(sessions) == 0 {
		m.setStatus("install", "No saved host has complete node, address, and SSH username metadata.")
		return 0, "No saved host has complete node, address, and SSH username metadata."
	}
	job := m.startJob("deploy", "", req.Mode, req.Origin, "")
	refsReady := true
	if release.valid() && release.RefsReady != nil {
		refsReady = *release.RefsReady
	}
	state := &deployState{ID: job.ID, Sessions: sessions, Remaining: len(sessions), Release: release, RefsReady: refsReady,
		Running: true, Label: fmt.Sprintf("Live output — %d host(s) deploying in parallel", len(sessions)), job: job}
	if onFinished != nil {
		state.done = append(state.done, onFinished)
	}
	for _, session := range sessions {
		session.Channel = deployPrefix + job.ID + ":" + session.Node
	}
	m.mu.Lock()
	m.deploy = state
	m.mu.Unlock()
	m.broadcastDeploy()
	suffix := ""
	if skipped > 0 {
		suffix = fmt.Sprintf(" (%d skipped — incomplete metadata)", skipped)
	}
	m.setStatus("install", fmt.Sprintf("Deploying to %d host(s) in parallel…%s", len(sessions), suffix))
	if release.valid() {
		plural := "s"
		if len(sessions) == 1 {
			plural = ""
		}
		m.logUI("Release: deploying signed %s mirror-node artifact to %d installed host%s in parallel.", release.Tag, len(sessions), plural)
	}
	env := m.environment(ctx)
	for _, session := range sessions {
		m.startDeploySession(job.Context(), env, state, session, options)
	}
	return len(sessions), ""
}

func (m *Module) startDeploySession(ctx context.Context, env hostEnv, state *deployState, session *deploySession, options installOptions) {
	channel := session.Channel
	remoteCmd, uploadBytes, err := m.buildHostInstallCommand(env, session.IP, session.User, session.Node, options)
	if err != nil {
		m.appendOutput(channel, "\n✘ "+err.Error()+"\n")
		m.finishDeploySession(state, session, false)
		return
	}
	command, err := buildHostSSHCommand(m.sshDir(), session.IP, session.User, session.pass, remoteCmd,
		savedHostIdentityFile(m.savedHosts(), m.sshDir(), session.Node, session.IP, session.User))
	if err != nil {
		m.appendOutput(channel, "\n✘ "+err.Error()+"\n")
		m.finishDeploySession(state, session, false)
		return
	}
	needSudo := session.User != "root"
	m.appendOutput(channel, "$ ssh "+session.User+"@"+session.IP+" …\nConnecting and running the installer…\n\n")
	if options.RequirePublish {
		version := env.Version
		if options.Release.valid() {
			version = options.Release.Version
		}
		m.appendOutput(channel, "Installing the published ForkMesh v"+version+"; the host will verify its release checksum, reported version and exact source commit.\n")
	} else if options.UploadBinary && !options.FromSource {
		m.appendOutput(channel, fmt.Sprintf("Uploading this app's release binary (%.1f MB) over the SSH session…\n", float64(len(uploadBytes))/(1024.0*1024.0)))
	}
	var stdin []byte
	if needSudo {
		stdin = append(stdin, []byte(session.pass+"\n")...)
	}
	if options.UploadBinary && !options.FromSource {
		stdin = append(stdin, []byte(hostUploadMarker+"\n")...)
		stdin = append(stdin, uploadBytes...)
	}
	go func() {
		carry := &utf8Carry{}
		result := runProcess(ctx, procSpec{Program: command.Program, Args: command.Args, Env: command.Env, Stdin: stdin,
			OnOutput: func(chunk []byte) {
				text := carry.decode(chunk)
				if text == "" {
					return
				}
				m.appendOutput(channel, text)
				if session.linkPrompted {
					return
				}
				session.linkTail = rightRunes(session.linkTail+text, installLinkTailChars)
				if match := linkCodePattern.FindStringSubmatch(session.linkTail); match != nil {
					session.linkPrompted = true
					m.handleInstallLinkCode(match[1], channel, false)
				}
			}})
		if !result.Started {
			m.appendOutput(channel, "\n[error] Could not start SSH. Install OpenSSH (and sshpass only when using password login).\n")
			m.finishDeploySession(state, session, false)
			return
		}
		ok := result.OK()
		if ok {
			m.appendOutput(channel, "\n✔ Finished.\n")
		} else {
			m.appendOutput(channel, fmt.Sprintf("\n✘ Failed (exit %d).\n", result.ExitCode))
		}
		m.finishDeploySession(state, session, ok)
	}()
}

func (m *Module) finishDeploySession(state *deployState, session *deploySession, ok bool) {
	m.mu.Lock()
	if session.Finished {
		m.mu.Unlock()
		return
	}
	session.Finished = true
	session.OK = ok
	if !ok {
		state.Failed++
	}
	glyph := "✘"
	if ok {
		glyph = "✔"
	}
	session.Header = fmt.Sprintf("%s %s — %s@%s", glyph, session.Node, session.User, session.IP)
	if state.Remaining > 0 {
		state.Remaining--
	}
	remaining, failed, total := state.Remaining, state.Failed, len(state.Sessions)
	release, refsReady := state.Release, state.RefsReady
	m.mu.Unlock()
	status := "install failed"
	if ok || release.Tag != "" {
		status = "installed"
	}
	m.rememberHost(session.Node, session.IP, session.User, session.pass, status, "", nil)
	if remaining == 0 {
		if release.Tag != "" {
			plural := "s"
			if failed == 1 {
				plural = ""
			}
			switch {
			case failed == 0 && refsReady:
				m.clearReleaseRollout(release)
				m.logUI("Release: %s deployed successfully to every installed mirror host.", release.Tag)
			case failed > 0:
				m.logUI("Release: %s deployment failed on %d host%s; the rollout remains pending and will resume after restart or a successful release rerun.", release.Tag, failed, plural)
			default:
				m.logUI("Release: %s installed on every host, but at least one managed repository ref push failed; the rollout remains pending for a safe retry.", release.Tag)
			}
		}
		switch {
		case failed == 0 && release.Tag != "" && !refsReady:
			m.setStatus("install", "The binaries are installed, but repository ref sync is still pending a safe retry.")
		case failed == 0:
			m.setStatus("install", fmt.Sprintf("✔ Finished deploying to all %d host(s).", total))
		default:
			m.setStatus("install", fmt.Sprintf("Finished deploying to %d host(s) — %d failed (see panes above).", total, failed))
		}
		m.mu.Lock()
		state.Running = false
		callbacks := state.done
		state.done = nil
		m.mu.Unlock()
		m.broadcastDeploy()
		m.finishJob(state.job, failed == 0, m.status("install"), map[string]any{"failed": failed, "total": total})
		for _, callback := range callbacks {
			callback(failed == 0, failed)
		}
		return
	}
	suffix := ""
	if failed > 0 {
		suffix = fmt.Sprintf(", %d failed", failed)
	}
	m.setStatus("install", fmt.Sprintf("Deploying… %d/%d done%s", total-remaining, total, suffix))
	m.broadcastDeploy()
}

func (m *Module) clearReleaseRollout(release publishedRelease) {
	if !m.uiAttached() {
		return
	}
	ctx, cancel := context.WithTimeout(m.ctx, environmentTimeout)
	_, _ = m.callUI(ctx, "ui.hosts.clearReleaseRollout", map[string]any{"owner": release.Owner, "name": release.Name, "tag": release.Tag})
	cancel()
}

func (m *Module) handleDeployAll(ctx context.Context, req *rpc.Request) (any, error) {
	var params deployRequest
	if err := req.Bind(&params); err != nil {
		return nil, err
	}
	params.Origin = "desktop"
	count, why := m.runHostDeployAll(m.ctx, params, nil)
	if count == 0 {
		return map[string]any{"started": false, "message": why}, nil
	}
	return map[string]any{"started": true, "sessions": count}, nil
}
