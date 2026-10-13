package hosts

import (
	"context"
	"encoding/json"
	"strings"
	"time"

	"forkmesh.com/desktop-core/internal/phonefamily"
	"forkmesh.com/desktop-core/internal/rpc"
)

const linkCodeSettleDelay = 5 * time.Second

func (m *Module) busyForPhones(hc hostsContext) bool {
	return m.installBusy() || hc.Busy
}

func (m *Module) hostPrepareNative(ctx context.Context, req *rpc.Request, params map[string]any, action, host string) (any, error) {
	if action == "install" {
		if host != "" && !validHostName(host) {
			return nil, rpc.Fail("bad_host")
		}
		if protectedFleetName(host) {
			return nil, rpc.Fail("protected_node")
		}
	} else if m.hostDetail(m.desktopContext(ctx, false), host) == nil {
		return nil, rpc.Fail("unknown_host")
	}
	token := m.hostOK.Issue(peerKey(req), action, host, m.nowMs())
	result := map[string]any{
		"token":       token,
		"action":      action,
		"host":        host,
		"summary":     hostActionSummary(action, host),
		"expiresInMs": float64(phonefamily.ConfirmationLifetimeMs),
	}
	if action == "destroy" {
		result["typeToConfirm"] = host
	}
	return result, nil
}

func typedNameMatches(typed, host string) bool {
	name := strings.TrimSpace(host)
	return name != "" && strings.TrimSpace(typed) == name
}

func (m *Module) hostStarted(req *rpc.Request, key, action, host, message string) map[string]any {
	m.mu.Lock()
	m.running[key] = peerID(req)
	m.mu.Unlock()
	named := host
	if named == "" {
		named = "(new)"
	}
	m.logUI("Host %s of \"%s\" requested by %s.", action, named, deviceLabel(req.Peer))
	time.AfterFunc(broadcastAfterAnswer, func() { m.broadcastPhones("hosts.changed", nil) })
	return map[string]any{"started": true, "action": action, "host": host, "message": message}
}

func (m *Module) hostRunNative(ctx context.Context, req *rpc.Request, params map[string]any, action, host string) (any, error) {
	key := action + ":" + lowerHost(host)
	m.mu.Lock()
	_, running := m.running[key]
	m.mu.Unlock()
	if running {
		return nil, rpc.Fail("already_running")
	}
	hc := m.desktopContext(ctx, true)
	env := m.environment(ctx)
	switch action {
	case "update":
		if m.busyForPhones(hc) {
			return nil, rpc.Fail("busy")
		}
		saved, index := m.savedHostByName(host)
		if index < 0 {
			return nil, rpc.Fail("unknown_host")
		}
		node := jsonString(saved["name"])
		ip := jsonString(saved["ip"])
		user := jsonString(saved["user"])
		result := m.hostStarted(req, key, action, host, "Installing the latest ForkMesh.")
		m.setStatus("install", "Loaded \""+node+"\". Click Install ForkMesh to run the installer.")
		m.runHostInstall(m.ctx, installRequest{Node: node, IP: ip, User: user, Password: m.password(credentialKey(node, ip, user)),
			UploadChecked: env.UploadChecked, Origin: deviceLabel(req.Peer)}, func(ok bool, _ string) {
			message := "ForkMesh was updated and restarted."
			if !ok {
				message = m.status("install")
				if message == "" {
					message = "The update failed."
				}
			}
			m.finishHostAction("update", host, ok, message)
		})
		return result, nil
	case "install":
		if m.busyForPhones(hc) {
			return nil, rpc.Fail("busy")
		}
		plan := strings.TrimSpace(jsonString(params["plan"]))
		region := strings.ToLower(strings.TrimSpace(jsonString(params["region"])))
		who := deviceLabel(req.Peer)
		started, node, why := m.createVultrMirror(m.ctx, host, env.FormVultrKey, plan, region, who)
		if !started {
			m.logUI("A new host requested by %s did not start: %s", who, strings.TrimSpace(why))
			if strings.TrimSpace(why) == "" {
				return nil, rpc.Fail("not_started")
			}
			return nil, rpc.Fail(strings.TrimSpace(why))
		}
		m.mu.Lock()
		m.running["install:"+lowerHost(node)] = peerID(req)
		m.mu.Unlock()
		planText, regionText := plan, region
		if planText == "" {
			planText = "default plan"
		}
		if regionText == "" {
			regionText = "default region"
		}
		m.logUI("New Vultr host \"%s\" (%s, %s) requested by %s.", node, planText, regionText, who)
		time.AfterFunc(broadcastAfterAnswer, func() { m.broadcastPhones("hosts.changed", nil) })
		return map[string]any{"started": true, "action": action, "host": node, "message": "Creating the server. This takes a few minutes."}, nil
	case "destroy":
		row := m.hostDetail(hc, host)
		if row == nil || !rowHasAction(row, "destroy") {
			return nil, rpc.Fail("not_a_vultr_host")
		}
		result := m.hostStarted(req, key, action, host, "Destroying the server.")
		m.destroyVultrHost(m.ctx, host, env.FormVultrKey, deviceLabel(req.Peer), func(ok bool, message string) {
			m.finishHostAction("destroy", host, ok, message)
		})
		return result, nil
	}
	return nil, rpc.Fail("unsupported_action")
}

func (m *Module) fleetAdmin(ctx context.Context) bool {
	hc := m.desktopContext(ctx, false)
	return hc.Admin || m.cachedEnv().Admin
}

func (m *Module) fleetPrepare(ctx context.Context, req *rpc.Request, params map[string]any) (any, error) {
	action := jsonString(params["action"])
	subject, why := fleetSubject(action, params)
	if subject == "" {
		return nil, rpc.Fail(why)
	}
	host := subjectHost(subject)
	hc := m.desktopContext(ctx, true)
	switch action {
	case "forget", "uninstall", "agentClis", "actions":
		if m.hostDetail(hc, host) == nil {
			return nil, rpc.Fail("unknown_host")
		}
	case "add":
		if m.hostDetail(hc, host) != nil {
			return nil, rpc.Fail("host_exists")
		}
		if protectedFleetName(host) {
			return nil, rpc.Fail("protected_node")
		}
	case "deleteNode":
		if !m.fleetAdmin(ctx) {
			return nil, rpc.Fail("not_admin")
		}
	}
	token := m.fleetOK.Issue(peerKey(req), action, subject, m.nowMs())
	result := map[string]any{
		"token":       token,
		"action":      action,
		"subject":     subject,
		"summary":     fleetActionSummary(action, subject),
		"expiresInMs": float64(phonefamily.ConfirmationLifetimeMs),
	}
	if typed := fleetTypedConfirmation(action, subject); typed != "" {
		result["typeToConfirm"] = typed
	}
	return result, nil
}

func (m *Module) fleetRun(ctx context.Context, req *rpc.Request, params map[string]any) (any, error) {
	action := jsonString(params["action"])
	subject, why := fleetSubject(action, params)
	verdict := m.fleetOK.Consume(jsonString(params["token"]), peerKey(req), action, subject, m.nowMs())
	if subject == "" || verdict != phonefamily.Ok {
		if subject != "" {
			why = phonefamily.ConfirmationError(verdict)
		}
		m.logUI("Refused a fleet %s from %s: %s.", action, deviceLabel(req.Peer), why)
		return nil, rpc.Fail(why)
	}
	if !fleetTypedConfirmationMatches(jsonString(params["confirmText"]), fleetTypedConfirmation(action, subject)) {
		m.logUI("Refused a fleet %s of \"%s\" from %s: the typed confirmation did not match.", action, subject, deviceLabel(req.Peer))
		return nil, rpc.Fail("name_mismatch")
	}
	return m.runFleetAction(ctx, req, action, subject, params)
}

func (m *Module) fleetStarted(req *rpc.Request, action, subject, message string, tracked bool) map[string]any {
	if tracked {
		m.mu.Lock()
		m.fleetRunning[action+":"+subject] = peerID(req)
		m.mu.Unlock()
	}
	m.logUI("Fleet %s (%s) requested by %s.", action, subject, deviceLabel(req.Peer))
	time.AfterFunc(broadcastAfterAnswer, func() {
		m.broadcastPhones("fleet.changed", nil)
		m.broadcastPhones("hosts.changed", nil)
	})
	return map[string]any{"started": true, "action": action, "subject": subject, "message": message}
}

func notStarted(text string) error {
	why := plainText(text, 400)
	if why == "" {
		return rpc.Fail("not_started")
	}
	return rpc.Fail(why)
}

func (m *Module) runFleetAction(ctx context.Context, req *rpc.Request, action, subject string, params map[string]any) (any, error) {
	key := action + ":" + subject
	host := subjectHost(subject)
	m.mu.Lock()
	_, running := m.fleetRunning[key]
	m.mu.Unlock()
	if running {
		return nil, rpc.Fail("already_running")
	}
	hc := m.desktopContext(ctx, true)
	env := m.environment(ctx)
	who := deviceLabel(req.Peer)
	switch action {
	case "add":
		name := strings.TrimSpace(jsonString(params["host"]))
		address := strings.TrimSpace(jsonString(params["address"]))
		user := strings.TrimSpace(jsonString(params["user"]))
		m.rememberHost(name, address, user, "", "added", "", nil)
		m.probeAll()
		return m.fleetStarted(req, action, subject, "Saved \""+name+"\". The desktop is checking it now.", false), nil
	case "forget":
		saved, _ := m.savedHostByName(host)
		name := host
		if value, ok := saved["name"].(string); ok {
			name = value
		}
		m.forgetHostNamed(name)
		return m.fleetStarted(req, action, subject, "Removed \""+host+"\" from the saved hosts.", false), nil
	case "deployAll":
		if m.busyForPhones(hc) {
			return nil, rpc.Fail("busy")
		}
		count, _ := m.runHostDeployAll(m.ctx, deployRequest{Mode: subject, Origin: who}, nil)
		if count <= 0 {
			return nil, notStarted(m.status("install"))
		}
		return m.fleetStarted(req, action, subject, "Deploying to "+itoa(count)+" host(s) in parallel.", false), nil
	case "uninstall":
		if m.busyForPhones(hc) {
			return nil, rpc.Fail("busy")
		}
		saved, index := m.savedHostByName(host)
		if index < 0 {
			return nil, rpc.Fail("unknown_host")
		}
		node, ip, user := jsonString(saved["name"]), jsonString(saved["ip"]), jsonString(saved["user"])
		m.setStatus("install", "Loaded \""+node+"\". Click Install ForkMesh to run the installer.")
		job, _ := m.runHostUninstall(m.ctx, node, ip, user, m.password(credentialKey(node, ip, user)), who)
		if job == nil {
			return nil, notStarted(m.status("install"))
		}
		return m.fleetStarted(req, action, subject, "Uninstalling ForkMesh from \""+host+"\".", false), nil
	case "agentClis":
		if m.jobRunning("agentClis") {
			return nil, rpc.Fail("busy")
		}
		saved, _ := m.savedHostByName(host)
		name := strings.TrimSpace(jsonString(saved["name"]))
		ip := strings.TrimSpace(jsonString(saved["ip"]))
		user := strings.TrimSpace(jsonString(saved["user"]))
		if name == "" || ip == "" || user == "" {
			return nil, rpc.Fail("unknown_host")
		}
		result := m.fleetStarted(req, action, subject, "Installing Claude Code and Codex on \""+name+"\".", true)
		identity := savedHostIdentityFile(m.savedHosts(), m.sshDir(), name, ip, user)
		m.runAgentCliInstall(m.ctx, name, ip, user, m.password(credentialKey(name, ip, user)), identity, false, who, func(ok bool, message string) {
			m.setStatus("install", message)
			m.finishFleetAction(action, subject, ok, message)
		})
		return result, nil
	case "actions":
		if m.jobRunning("actions") {
			return nil, rpc.Fail("busy")
		}
		saved, _ := m.savedHostByName(host)
		request := mirrorActionsRequest{RequestID: newStreamID(), Host: strings.TrimSpace(jsonString(saved["ip"])),
			SSHUser: strings.TrimSpace(jsonString(saved["user"])), NodeName: strings.TrimSpace(jsonString(saved["name"])),
			ActionsEnabled: strings.HasSuffix(subject, "|on")}
		if request.Host == "" || request.SSHUser == "" {
			return nil, rpc.Fail("unknown_host")
		}
		m.runHostActionsConfiguration(m.ctx, request, m.password(credentialKey(request.NodeName, request.Host, request.SSHUser)), who, nil)
		message := "Turning Actions off for \"" + host + "\"."
		if request.ActionsEnabled {
			message = "Turning Actions on for \"" + host + "\"."
		}
		return m.fleetStarted(req, action, subject, message, false), nil
	case "linkCode":
		if signer, _ := m.linkSigner(env); signer == "" {
			return nil, rpc.Fail("cannot_sign")
		}
		go m.submitHostLinkCode(m.ctx, subject)
		result := m.fleetStarted(req, action, subject, "Sent the link code. The result shows on the Hosts page.", true)
		go func() {
			if !m.sleep(m.ctx, linkCodeSettleDelay) {
				return
			}
			outcome := plainText(m.status("install"), 400)
			m.finishFleetAction(action, subject, !strings.Contains(outcome, "Could not"), outcome)
		}()
		return result, nil
	case "resume":
		resumeRequested, node, active := m.provisionResumable()
		if active {
			return nil, rpc.Fail("already_running")
		}
		if !resumeRequested || node == "" {
			return nil, rpc.Fail("nothing_to_resume")
		}
		started, why := m.resumeProvision(m.ctx, env.FormVultrKey)
		if !started {
			return nil, notStarted(why)
		}
		_, node, _ = m.provisionResumable()
		return m.fleetStarted(req, action, subject, "Resumed the deployment of \""+node+"\".", false), nil
	case "fleetTarget", "reconcile":
		return nil, rpc.Fail("hosts_page_unavailable")
	case "mirrorPush":
		return m.mirrorPushFromPhone(ctx, req, action, subject)
	case "deleteNode":
		if !m.fleetAdmin(ctx) {
			return nil, rpc.Fail("not_admin")
		}
		if busy, _ := m.nodeDeleteState(); busy {
			return nil, rpc.Fail("busy")
		}
		if env.SessionToken == "" && !env.CanSign {
			return nil, rpc.Fail("cannot_sign")
		}
		result := m.fleetStarted(req, action, subject, "Deleting \""+host+"\": server, DNS, then the mesh.", false)
		m.deleteNode(m.ctx, host, "", who)
		return result, nil
	}
	return nil, rpc.Fail("unsupported_action")
}

func (m *Module) finishFleetAction(action, subject string, ok bool, message string) {
	key := action + ":" + subject
	m.mu.Lock()
	_, tracked := m.fleetRunning[key]
	delete(m.fleetRunning, key)
	m.mu.Unlock()
	if !tracked {
		return
	}
	verdict := "failed"
	if ok {
		verdict = "finished"
	}
	m.logUI("Fleet %s (%s) from the phone %s: %s", action, subject, verdict, leftUTF16(message, 300))
	m.broadcastPhones("fleet.action", map[string]any{"action": action, "subject": subject, "ok": ok, "message": plainText(message, 2000)})
	m.broadcastPhones("fleet.changed", nil)
}

func (m *Module) handleFleetFinished(ctx context.Context, req *rpc.Request) (any, error) {
	var params struct {
		Action  string `json:"action"`
		Subject string `json:"subject"`
		OK      bool   `json:"ok"`
		Message string `json:"message"`
	}
	if err := req.Bind(&params); err != nil {
		return nil, err
	}
	m.finishFleetAction(params.Action, params.Subject, params.OK, params.Message)
	return map[string]any{}, nil
}

func (m *Module) fleetRunningList(hc hostsContext) []string {
	m.mu.Lock()
	out := make([]string, 0, len(m.fleetRunning)+len(hc.FleetRunning))
	for key := range m.fleetRunning {
		out = append(out, key)
	}
	m.mu.Unlock()
	out = append(out, hc.FleetRunning...)
	return out
}

func itoa(n int) string {
	b, _ := json.Marshal(n)
	return string(b)
}

func (m *Module) provisioningNode(hc hostsContext) string {
	if resume, node, active := m.provisionResumable(); active {
		_ = resume
		return node
	}
	return hc.Provisioning
}
