package hosts

import (
	"context"
	"encoding/json"
	"io"
	"os"
	"path/filepath"
	"strings"
	"time"

	"forkmesh.com/desktop-core/internal/phonefamily"
	"forkmesh.com/desktop-core/internal/rpc"
)

func peerID(req *rpc.Request) string {
	if req.Peer == nil {
		return ""
	}
	return req.Peer.ID()
}

func peerKey(req *rpc.Request) string {
	if req.Peer == nil {
		return ""
	}
	return req.Peer.Device().Key
}

func (m *Module) handleHosts(ctx context.Context, req *rpc.Request) (any, error) {
	params := req.Object()
	host := strings.TrimSpace(jsonString(params["host"]))
	switch req.Method {
	case "hosts.list":
		m.refreshProbesWithoutDesktop()
		return m.hostsPayload(m.desktopContext(ctx, true)), nil
	case "hosts.detail":
		detail := m.hostDetail(m.desktopContext(ctx, true), host)
		if detail == nil {
			return nil, rpc.Fail("unknown_host")
		}
		return detail, nil
	case "hosts.options":
		return m.hostOptions(ctx)
	case "hosts.prepare":
		return m.hostPrepare(ctx, req, params, host)
	case "hosts.run":
		return m.hostRun(ctx, req, params, host)
	case "hosts.logs.start":
		return m.logsStart(req, host)
	case "hosts.logs.ack", "hosts.logs.stop":
		return m.logsAckStop(req, params)
	}
	return nil, rpc.ErrDelegate
}

func (m *Module) hostOptions(ctx context.Context) (any, error) {
	hc := m.desktopContext(ctx, true)
	apiKey, _ := m.vultrAPIKey(m.environment(ctx).FormVultrKey)
	if apiKey == "" {
		return map[string]any{"vultr": false}, nil
	}
	known := []string{}
	for _, host := range m.savedHosts() {
		known = append(known, strings.ToLower(jsonString(host["name"])))
	}
	known = append(known, hc.LinkedNodes...)
	suggested := nextMirrorNodeName(known)
	result, failure := m.vultrCall(ctx, apiKey, "GET", "/v2/plans?per_page=500", nil)
	if failure != "" {
		return nil, rpc.Fail("vultr_unreachable")
	}
	choices := vultrPlanChoices(jsonObjects(result["plans"]))
	choices["vultr"] = true
	choices["suggestedName"] = suggested
	return choices, nil
}

func (m *Module) hostPrepare(ctx context.Context, req *rpc.Request, params map[string]any, host string) (any, error) {
	action := jsonString(params["action"])
	if !isHostMutatingAction(action) {
		return nil, rpc.Fail("unsupported_action")
	}
	if !isServiceAction(action) {
		return m.hostPrepareNative(ctx, req, params, action, host)
	}
	if m.hostDetail(m.desktopContext(ctx, false), host) == nil {
		return nil, rpc.Fail("unknown_host")
	}
	token := m.hostOK.Issue(peerKey(req), action, host, m.nowMs())
	return map[string]any{
		"token":       token,
		"action":      action,
		"host":        host,
		"summary":     hostActionSummary(action, host),
		"expiresInMs": float64(phonefamily.ConfirmationLifetimeMs),
	}, nil
}

func (m *Module) hostRun(ctx context.Context, req *rpc.Request, params map[string]any, host string) (any, error) {
	action := jsonString(params["action"])
	token := jsonString(params["token"])
	verdict := m.hostOK.Consume(token, peerKey(req), action, host, m.nowMs())
	if verdict != phonefamily.Ok {
		m.logUI("Refused a host %s of \"%s\" from %s: %s.", action, host, deviceLabel(req.Peer), phonefamily.ConfirmationError(verdict))
		return nil, rpc.Fail(phonefamily.ConfirmationError(verdict))
	}
	if action == "destroy" && !typedNameMatches(jsonString(params["confirmName"]), host) {
		m.logUI("Refused to destroy \"%s\" from %s: the typed name did not match.", host, deviceLabel(req.Peer))
		return nil, rpc.Fail("name_mismatch")
	}
	if !isServiceAction(action) {
		return m.hostRunNative(ctx, req, params, action, host)
	}
	return m.runServiceAction(ctx, req, action, host)
}

func (m *Module) runServiceAction(ctx context.Context, req *rpc.Request, action, host string) (any, error) {
	key := action + ":" + lowerHost(host)
	m.mu.Lock()
	_, busy := m.running[key]
	m.mu.Unlock()
	if busy {
		return nil, rpc.Fail("already_running")
	}
	row := m.hostDetail(m.desktopContext(ctx, false), host)
	if row == nil || !rowHasAction(row, action) {
		return nil, rpc.Fail("not_a_mirror_node")
	}
	m.mu.Lock()
	if _, busy := m.running[key]; busy {
		m.mu.Unlock()
		return nil, rpc.Fail("already_running")
	}
	m.running[key] = peerID(req)
	m.mu.Unlock()
	m.logUI("Host %s of \"%s\" requested by %s.", action, host, deviceLabel(req.Peer))
	op := m.beginOperation("service", action, host, deviceLabel(req.Peer))
	go func() {
		ok, output := m.runMirrorNodeSSH(m.ctx, host, serviceCommand(action), serviceTimeout, "", "")
		if output == "" {
			if ok {
				output = "Done."
			} else {
				output = "The host could not be reached."
			}
		}
		m.endOperation(op, ok, output)
		m.finishHostAction(action, host, ok, output)
	}()
	time.AfterFunc(broadcastAfterAnswer, func() {
		m.broadcastPhones("hosts.changed", nil)
		m.broadcastUI("core.hosts.changed", map[string]any{})
	})
	return map[string]any{"started": true, "action": action, "host": host, "message": "Asking the host's service manager."}, nil
}

func (m *Module) finishHostAction(action, host string, ok bool, message string) {
	key := action + ":" + lowerHost(host)
	m.mu.Lock()
	_, tracked := m.running[key]
	delete(m.running, key)
	m.mu.Unlock()
	if !tracked {
		return
	}
	verdict := "failed"
	if ok {
		verdict = "finished"
	}
	m.logUI("Host %s of \"%s\" from the phone %s: %s", action, host, verdict, leftUTF16(message, 300))
	event := map[string]any{"action": action, "host": host, "ok": ok, "message": leftUTF16(message, 2000)}
	m.broadcastPhones("hosts.action", event)
	m.broadcastPhones("hosts.changed", nil)
	m.broadcastUI("core.hosts.action", event)
}

func (m *Module) handleFleet(ctx context.Context, req *rpc.Request) (any, error) {
	params := req.Object()
	switch req.Method {
	case "fleet.summary":
		return m.fleetSummary(m.desktopContext(ctx, true)), nil
	case "fleet.provision":
		return m.fleetProvision(m.desktopContext(ctx, true)), nil
	case "fleet.repos":
		hc := m.desktopContext(ctx, true)
		if !hc.valid {
			return nil, rpc.ErrDelegate
		}
		repos := hc.Repos
		if repos == nil {
			repos = []json.RawMessage{}
		}
		if len(repos) > maxFleetRepos {
			repos = repos[:maxFleetRepos]
		}
		return map[string]any{"repos": repos}, nil
	case "fleet.disk":
		return m.fleetDisk(ctx, params)
	case "fleet.prepare":
		return m.fleetPrepare(ctx, req, params)
	case "fleet.run":
		return m.fleetRun(ctx, req, params)
	}
	return nil, rpc.Fail("unsupported")
}

func (m *Module) fleetSummary(hc hostsContext) map[string]any {
	hosts := m.savedHosts()
	gateways := m.mirror().gatewayRecords()
	pushing, pending := m.mirror().counts()
	provision := m.provisionSnapshot()["status"]
	running := m.fleetRunningList(hc)
	inProgress, target := m.nodeDeleteState()
	if !inProgress && hc.NodeDelete.InProgress {
		inProgress, target = true, hc.NodeDelete.Target
	}
	lastResult := m.status("install")
	if lastResult == "" {
		lastResult = hc.InstallText
	}
	return map[string]any{
		"costs": costSummary(hosts),
		"fleet": map[string]any{
			"available": false, "enabled": false, "desired": 0, "status": "",
			"nextCheckInMs": -1, "reconciling": false,
		},
		"provision":    provision,
		"gateways":     gatewayRows(gateways, m.nowMs(), gatewayRetiredAfterMs, gatewayMaxRows),
		"mirrorClient": map[string]any{"status": plainText(hc.MirrorClient.Status, 400), "health": plainText(hc.MirrorClient.Health, 400)},
		"mirrorPush":   map[string]any{"pushing": hc.MirrorPush.Pushing + pushing, "pending": hc.MirrorPush.Pending + pending},
		"canCreate":    hc.CanCreate || vultrAPIKeyFromVariables(m.actionVariables()) != "",
		"busy":         m.busyForPhones(hc),
		"deploying":    m.deployRemaining() + hc.Deploying,
		"admin":        m.fleetAdmin(context.Background()),
		"nodeDelete":   map[string]any{"inProgress": inProgress, "target": target},
		"lastResult":   plainText(lastResult, 600),
		"running":      running,
	}
}

func (m *Module) provisionLogPath(hc hostsContext) string {
	if hc.ProvisionLog != "" {
		return hc.ProvisionLog
	}
	if m.core == nil || m.core.Layout.Data == "" {
		return ""
	}
	return filepath.Join(m.core.Layout.Data, "deployments", "vultr-mirror-latest.log")
}

func fileTail(path string, maxBytes int64) []byte {
	if path == "" {
		return nil
	}
	file, err := os.Open(path)
	if err != nil {
		return nil
	}
	defer file.Close()
	if info, err := file.Stat(); err == nil && info.Size() > maxBytes {
		_, _ = file.Seek(info.Size()-maxBytes, io.SeekStart)
	}
	data, _ := io.ReadAll(io.LimitReader(file, maxBytes))
	return data
}

func (m *Module) fleetProvision(hc hostsContext) map[string]any {
	status, _ := m.provisionSnapshot()["status"].(map[string]any)
	if status == nil {
		status = map[string]any{}
	}
	status["log"] = tailLines(fileTail(m.provisionLogPath(hc), provisionLogTailBytes), provisionLogLines, 400)
	return status
}

func (m *Module) fleetDisk(ctx context.Context, params map[string]any) (any, error) {
	host := strings.TrimSpace(jsonString(params["host"]))
	path := strings.TrimSpace(jsonString(params["path"]))
	if path == "" {
		path = "/"
	}
	if m.hostDetail(m.desktopContext(ctx, false), host) == nil {
		return nil, rpc.Fail("unknown_host")
	}
	command := diskUsageCommand(path)
	if command == "" {
		return nil, rpc.Fail("bad_path")
	}
	done := m.track()
	defer done()
	ok, output := m.runMirrorNodeSSH(ctx, host, command, diskUsageTimeout, "", "")
	rows := parseDiskUsage(output, path, 60)
	if !ok && len(rows) == 0 {
		return nil, rpc.Fail("host_unreachable")
	}
	return map[string]any{"host": host, "path": path, "entries": rows}, nil
}
