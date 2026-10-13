package hosts

import (
	"context"
	"fmt"
	"net/http"
	"net/url"
	"sort"
	"strconv"
	"strings"

	"forkmesh.com/desktop-core/internal/rpc"
)

func (m *Module) nodeDeleteState() (bool, string) {
	m.mu.Lock()
	defer m.mu.Unlock()
	p := m.provLocked()
	return p.deleteInProgress, p.deleteTarget
}

func (m *Module) setNodeDeleteStatus(text string) {
	m.setStatus("nodes", text)
	m.logUI("%s", text)
}

func (m *Module) flash(text string, isError bool) {
	m.broadcastUI("core.hosts.flash", map[string]any{"text": text, "error": isError})
}

func (m *Module) deleteNode(ctx context.Context, node, nodeID, who string) (bool, string) {
	target := strings.ToLower(strings.TrimSpace(node))
	if protectedMeshNode(target) {
		return false, ""
	}
	busy, current := m.nodeDeleteState()
	if busy {
		message := fmt.Sprintf("Deletion of \"%s\" is already in progress.", current)
		return false, message
	}
	env := m.environment(ctx)
	if !env.Admin && !m.desktopContext(ctx, false).Admin {
		message := "Only a platform admin can delete a node from the mesh."
		return false, message
	}
	if env.SessionToken == "" && !env.CanSign {
		message := "Sign in to your admin account from Settings first: deleting a node needs either an account session or this desktop's account key."
		return false, message
	}
	claimed := false
	m.withProv(func(p *provisioner) {
		if p.deleteInProgress {
			return
		}
		claimed = true
		p.deleteInProgress = true
		p.deleteTarget = target
	})
	if !claimed {
		_, current = m.nodeDeleteState()
		return false, fmt.Sprintf("Deletion of \"%s\" is already in progress.", current)
	}
	fromDesktop := who == "" || who == "desktop"
	job := m.startJob("deleteNode", target, "", who, "")
	if fromDesktop {
		m.setNodeDeleteStatus(fmt.Sprintf("1/3 — Destroying the server for \"%s\"…", target))
	} else {
		m.setNodeDeleteStatus(fmt.Sprintf("1/3: destroying the server for \"%s\" (from %s)", target, who))
	}
	go func() {
		runCtx := job.Context()
		if outcome := m.destroyVultrServerForNode(runCtx, env, target); outcome != "" {
			m.setNodeDeleteStatus(outcome)
		}
		if fromDesktop {
			m.setNodeDeleteStatus(fmt.Sprintf("2/3 — Removing DNS for \"%s\"…", target))
		} else {
			m.setNodeDeleteStatus(fmt.Sprintf("2/3: removing DNS for \"%s\"", target))
		}
		if outcome := m.removeVultrMirrorDNS(runCtx, env, target); outcome != "" {
			m.setNodeDeleteStatus(outcome)
		}
		if fromDesktop {
			m.setNodeDeleteStatus(fmt.Sprintf("3/3 — Removing \"%s\" from the mesh…", target))
		} else {
			m.setNodeDeleteStatus(fmt.Sprintf("3/3: removing \"%s\" from the mesh", target))
		}
		m.sendMeshNodeDeleteRequest(runCtx, job, env, target, nodeID, !fromDesktop)
	}()
	return true, ""
}

func (m *Module) destroyVultrServerForNode(ctx context.Context, env hostEnv, node string) string {
	apiKey, _ := m.vultrAPIKey(env.FormVultrKey)
	if apiKey == "" {
		return fmt.Sprintf("No Vultr API key is available (store one as VULTR_API_KEY), so no server was destroyed for \"%s\" — continuing with the mesh removal.", node)
	}
	recorded, address := "", ""
	hosts := m.rawSavedHosts()
	for i := range hosts {
		host := objectAt(hosts, i)
		if !strings.EqualFold(strings.TrimSpace(jsonString(host["name"])), node) {
			continue
		}
		recorded = savedHostVultrInstanceID(host)
		address = strings.TrimSpace(jsonString(host["ip"]))
		break
	}
	if recorded != "" {
		return m.sendNodeVultrDestroy(ctx, apiKey, recorded, node)
	}
	result, failure := m.vultrCall(ctx, apiKey, http.MethodGet, "/v2/instances?per_page=500", nil)
	if failure != "" {
		return fmt.Sprintf("Could not reach Vultr to destroy \"%s\" (%s) — continuing with the mesh removal.", node, failure)
	}
	instances := jsonObjects(result["instances"])
	instanceID := vultrInstanceIDForAddress(instances, node)
	if instanceID == "" && address != "" {
		instanceID = vultrInstanceIDForAddress(instances, address)
	}
	if instanceID == "" {
		return fmt.Sprintf("No single Vultr instance matches \"%s\", so no server was destroyed — continuing with the mesh removal.", node)
	}
	return m.sendNodeVultrDestroy(ctx, apiKey, instanceID, node)
}

func (m *Module) sendNodeVultrDestroy(ctx context.Context, apiKey, instanceID, node string) string {
	if invalid := validateVultrDestroyRequest(apiKey, instanceID); invalid != "" {
		return invalid
	}
	m.setNodeDeleteStatus(fmt.Sprintf("1/3 — Destroying the Vultr server behind \"%s\"…", node))
	if _, failure := m.vultrCall(ctx, apiKey, http.MethodDelete, "/v2/instances/"+instanceID, nil); failure != "" {
		return fmt.Sprintf("Vultr refused to destroy %s (%s) — continuing with the mesh removal.", instanceID, failure)
	}
	return fmt.Sprintf("Destroyed Vultr instance %s (\"%s\").", instanceID, node)
}

func (m *Module) removeVultrMirrorDNS(ctx context.Context, env hostEnv, node string) string {
	skip := func(reason string) string { return "Left the DNS record alone: " + reason }
	token := cloudflareAPITokenFromVariables(m.actionVariables())
	if token == "" {
		return skip("no CLOUDFLARE_API_TOKEN device variable")
	}
	zone := m.cloudflareZone(env)
	hostname := vultrMirrorDNSHostname(node, zone)
	if hostname == "" {
		if zone == "" {
			return skip("no Cloudflare zone is configured")
		}
		return skip(fmt.Sprintf("\"%s\" and \"%s\" do not form a valid hostname", node, zone))
	}
	zones, failure := m.cloudflareCall(ctx, token, http.MethodGet, "/zones?name="+percentEncode(zone), nil)
	if failure != "" {
		return skip(failure)
	}
	zoneID := cloudflareZoneID(jsonObjects(zones["result"]), zone)
	if zoneID == "" {
		return skip(fmt.Sprintf("this API token does not see exactly one \"%s\" zone", zone))
	}
	existing, failure := m.cloudflareCall(ctx, token, http.MethodGet, "/zones/"+zoneID+"/dns_records?type=A&name="+percentEncode(hostname), nil)
	if failure != "" {
		return skip(failure)
	}
	recordID := cloudflareDNSRecordID(jsonObjects(existing["result"]), hostname, "A")
	if recordID == "" {
		return fmt.Sprintf("No %s DNS record to remove.", hostname)
	}
	if _, failure := m.cloudflareCall(ctx, token, http.MethodDelete, "/zones/"+zoneID+"/dns_records/"+recordID, nil); failure != "" {
		return skip(failure)
	}
	return fmt.Sprintf("Removed the %s DNS record.", hostname)
}

func (m *Module) failNodeDelete(job *Job, node, message string) {
	m.withProv(func(p *provisioner) {
		p.deleteInProgress = false
		p.deleteTarget = ""
	})
	m.setNodeDeleteStatus(message)
	m.flash(message, true)
	m.broadcastUI("core.hosts.nodeDeleted", map[string]any{"node": node, "ok": false, "message": message})
	m.finishJob(job, false, message, nil)
}

func (m *Module) sendMeshNodeDeleteRequest(ctx context.Context, job *Job, env hostEnv, node, nodeID string, fromPhone bool) {
	endpoint, err := url.Parse(env.NodeDeleteURL)
	if err != nil {
		endpoint = &url.URL{}
	}
	endpoint.RawQuery = ""
	endpoint.ForceQuery = false
	bearer := env.SessionToken
	if bearer == "" {
		actor := strings.ToLower(strings.TrimSpace(env.AccountOwner))
		ts := strconv.FormatInt(m.nowMs(), 10)
		signature := ""
		if m.core != nil {
			if id, err := m.core.RequireIdentity(); err == nil {
				signature = id.Sign([]byte("forkmesh-world-node-delete-v1\n" + actor + "\n" + node + "\n" + ts))
			}
		}
		if actor == "" || signature == "" {
			m.failNodeDelete(job, node, fmt.Sprintf("Could not sign the deletion of \"%s\" with this desktop's account key.", node))
			return
		}
		endpoint.RawQuery = "node=" + percentEncode(actor) + "&ts=" + ts + "&sig=" + percentEncode(signature)
	}
	body := map[string]any{"nodeName": node, "nodeId": nodeID, "confirmation": "DELETE " + node}
	m.setNodeDeleteStatus(fmt.Sprintf("3/3 — Removing \"%s\" from the mesh…", node))
	if fromPhone {
		m.broadcastPhones("fleet.changed", nil)
	}
	_, response, failure := m.postJSON(ctx, endpoint.String(), bearer, body)
	if failure != "" || !jsonBool(response["ok"]) {
		detail := strings.TrimSpace(jsonString(response["error"]))
		if detail == "" {
			detail = failure
		}
		m.failNodeDelete(job, node, fmt.Sprintf("Could not delete \"%s\": %s", node, detail))
		return
	}
	deletedName := strings.ToLower(strings.TrimSpace(jsonString(response["nodeDeleted"])))
	names := map[string]bool{}
	for _, value := range jsonArray(response["identifiers"]) {
		if identifier := strings.ToLower(strings.TrimSpace(jsonString(value))); identifier != "" {
			names[identifier] = true
		}
	}
	if deletedName != "" {
		names[deletedName] = true
	}
	if deletedName == "" || !names[node] {
		m.failNodeDelete(job, node, fmt.Sprintf("The relay did not confirm deletion of \"%s\"; it remains visible.", node))
		return
	}
	deleted := make([]string, 0, len(names))
	for name := range names {
		deleted = append(deleted, name)
	}
	sort.Strings(deleted)
	for _, name := range deleted {
		m.unwireNode(ctx, name)
	}
	m.forgetHostNamed(node)
	for _, name := range deleted {
		m.recordLedgerEntry(deletedMeshNodesSetting, name)
		m.recordLedgerEntry(decommissionedNodesSetting, name)
	}
	alreadyAbsent := jsonBool(response["alreadyAbsent"])
	message := fmt.Sprintf("3/3 complete — Relay confirmed deletion of \"%s\"; it has been removed from this list.", node)
	if alreadyAbsent {
		message = fmt.Sprintf("Relay confirmed \"%s\" was already absent; removed its stale local entry.", node)
	}
	m.withProv(func(p *provisioner) {
		p.deleteInProgress = false
		p.deleteTarget = ""
	})
	m.setNodeDeleteStatus(message)
	m.broadcastUI("core.hosts.nodeDeleted", map[string]any{"node": node, "ok": true, "message": message, "deletedNames": deleted, "alreadyAbsent": alreadyAbsent})
	m.finishJob(job, true, message, nil)
}

func (m *Module) handleDeleteNode(ctx context.Context, req *rpc.Request) (any, error) {
	var params struct {
		Node   string `json:"node"`
		NodeID string `json:"nodeId"`
		Origin string `json:"origin"`
	}
	if err := req.Bind(&params); err != nil {
		return nil, err
	}
	if busy, _ := m.nodeDeleteState(); busy {
		return nil, rpc.Fail("busy")
	}
	origin := params.Origin
	if origin == "" {
		origin = "desktop"
	}
	started, message := m.deleteNode(m.ctx, params.Node, params.NodeID, origin)
	return map[string]any{"started": started, "message": message}, nil
}
