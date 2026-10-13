package hosts

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"time"

	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/rpc"
)

const (
	provisionSetting        = "hosts/vultrProvision/v1"
	provisionStageCount     = 6
	provisionPollInterval   = 10 * time.Second
	provisionMaxPolls       = 60
	provisionSSHProbes      = 20
	provisionSSHInterval    = 60 * time.Second
	provisionMaxInstalls    = 1
	publicationFastPolls    = 90
	publicationSlowPolls    = 15
	publicationFastInterval = 10 * time.Second
	publicationSlowInterval = 60 * time.Second
	healthTimeout           = 5 * time.Second
	seedPushCap             = 15 * time.Minute
	diagnoseTimeout         = 45 * time.Second
	provisionLogDebounce    = 250 * time.Millisecond
	provisionLogCap         = 1 << 20
	provisionResumeDelay    = time.Second
	provisionAttachWait     = 2 * time.Second
	sharedHostKeyComment    = "forkmesh-shared-host-key"
)

const diagnoseRemoteTemplate = `sh -lc 'systemctl is-active forkmesh-mirror-node >/dev/null 2>&1 && echo "service: running" || echo "service: not running"; test -d /var/lib/forkmesh/repositories && find /var/lib/forkmesh/repositories -maxdepth 1 -name "*.git" -printf "clone: %f\\n" 2>/dev/null | head -n 4; journalctl -u forkmesh-mirror-node -n 200 --no-pager 2>/dev/null | grep -E "warnings|failed" | tail -n 2; %1'`

const diagnoseRestartCommand = `if ls -d /var/lib/forkmesh/repositories/.clone-* >/dev/null 2>&1; then echo "restart: skipped, a clone is still running"; elif systemctl restart forkmesh-mirror-node >/dev/null 2>&1; then echo "restart: requested"; else echo "restart: refused"; fi`

type provisioner struct {
	Active                bool
	resumeRequested       bool
	resumeChain           bool
	state                 string
	stage                 int
	detail                string
	message               string
	node                  string
	dnsHostname           string
	instanceID            string
	ip                    string
	identityFile          string
	installAgentClis      bool
	installUseLocalBinary bool
	pollCount             int
	installAttempts       int
	sshWaitCount          int
	hostMetadata          map[string]any
	stageDetails          [provisionStageCount]string
	publicationGap        string
	tunnelToken           string
	requestedPlan         string
	requestedRegion       string
	attemptLog            []string
	formAgentClis         bool
	origin                string
	run                   int64
	job                   *Job
	seeding               bool
	logScheduled          bool
	logMu                 sync.Mutex
	deleteInProgress      bool
	deleteTarget          string
}

type provRun struct {
	m      *Module
	gen    int64
	ctx    context.Context
	apiKey string
	origin string
}

func (m *Module) provLocked() *provisioner {
	if m.prov == nil {
		m.prov = &provisioner{hostMetadata: map[string]any{}}
	}
	return m.prov
}

func (m *Module) withProv(fn func(p *provisioner)) {
	m.mu.Lock()
	defer m.mu.Unlock()
	fn(m.provLocked())
}

func (m *Module) provisionActive() bool {
	m.mu.Lock()
	defer m.mu.Unlock()
	return m.prov != nil && m.prov.Active
}

func boundedStage(stage int) int {
	if stage < 1 {
		return 1
	}
	if stage > provisionStageCount {
		return provisionStageCount
	}
	return stage
}

func copyObject(source map[string]any) map[string]any {
	out := make(map[string]any, len(source))
	for key, value := range source {
		out[key] = value
	}
	return out
}

func (m *Module) provisionSnapshot() map[string]any {
	m.mu.Lock()
	p := m.provLocked()
	details := make([]string, provisionStageCount)
	copy(details, p.stageDetails[:])
	status := provisionStatus(provisionState{
		Active:       p.Active,
		Node:         p.node,
		Stage:        p.stage,
		State:        p.state,
		Detail:       p.detail,
		Message:      p.message,
		Gap:          p.publicationGap,
		StageDetails: details,
		Resumable:    p.resumeRequested && p.node != "" && !p.Active,
		Attempts:     p.installAttempts,
	})
	snapshot := map[string]any{
		"active":                p.Active,
		"resumeRequested":       p.resumeRequested,
		"state":                 p.state,
		"stage":                 p.stage,
		"detail":                p.detail,
		"message":               p.message,
		"node":                  p.node,
		"dnsHostname":           p.dnsHostname,
		"instanceId":            p.instanceID,
		"ip":                    p.ip,
		"identityFile":          p.identityFile,
		"installAgentClis":      p.installAgentClis,
		"installUseLocalBinary": p.installUseLocalBinary,
		"pollCount":             p.pollCount,
		"installAttempts":       p.installAttempts,
		"hostMetadata":          copyObject(p.hostMetadata),
		"publicationGap":        p.publicationGap,
		"stageDetails":          details,
		"status":                status,
	}
	m.mu.Unlock()
	return snapshot
}

func (m *Module) broadcastProvision() {
	m.broadcastUI("core.hosts.provision", m.provisionSnapshot())
}

func (m *Module) provisionResumable() (bool, string, bool) {
	m.mu.Lock()
	defer m.mu.Unlock()
	p := m.provLocked()
	return p.resumeRequested, p.node, p.Active
}

func (p *provisioner) checkpoint(nowMs int64) map[string]any {
	return map[string]any{
		"version":               1,
		"state":                 p.state,
		"stage":                 p.stage,
		"detail":                p.detail,
		"message":               p.message,
		"node":                  p.node,
		"dnsHostname":           p.dnsHostname,
		"instanceId":            p.instanceID,
		"ip":                    p.ip,
		"identityFile":          p.identityFile,
		"installAgentClis":      p.installAgentClis,
		"installUseLocalBinary": p.installUseLocalBinary,
		"pollCount":             p.pollCount,
		"installAttempts":       p.installAttempts,
		"hostMetadata":          copyObject(p.hostMetadata),
		"updatedAt":             nowMs,
	}
}

func (m *Module) persistProvision(state string, message *string) {
	now := m.nowMs()
	value := ""
	m.withProv(func(p *provisioner) {
		if state != "" {
			p.state = state
		}
		if message != nil {
			p.message = *message
		}
		value = compactJSON(p.checkpoint(now))
	})
	m.writeSettings(daemon.SettingWrite{Key: provisionSetting, Type: "string", Value: value})
	m.saveProvisionLog()
	m.broadcastProvision()
}

func (m *Module) provisionLogFile() string {
	if path := m.cachedEnv().ProvisionLog; path != "" {
		return path
	}
	if m.core == nil || m.core.Layout.Data == "" {
		return ""
	}
	return filepath.Join(m.core.Layout.Data, "deployments", "vultr-mirror-latest.log")
}

func (m *Module) saveProvisionLog() {
	path := m.provisionLogFile()
	if path == "" {
		return
	}
	text := rightRunes(ansiPattern.ReplaceAllString(m.transcriptText(installChannel), ""), provisionLogCap)
	var p *provisioner
	m.withProv(func(current *provisioner) { p = current })
	p.logMu.Lock()
	defer p.logMu.Unlock()
	dir := filepath.Dir(path)
	if err := os.MkdirAll(dir, 0o700); err != nil {
		return
	}
	file, err := os.CreateTemp(dir, ".vultr-mirror-log-*")
	if err != nil {
		return
	}
	temp := file.Name()
	_, writeErr := file.WriteString(text)
	closeErr := file.Close()
	if writeErr != nil || closeErr != nil || os.Rename(temp, path) != nil {
		_ = os.Remove(temp)
	}
}

func (m *Module) installLogChanged() {
	schedule := false
	m.mu.Lock()
	if m.prov != nil && m.prov.Active && !m.prov.logScheduled {
		m.prov.logScheduled = true
		schedule = true
	}
	m.mu.Unlock()
	if !schedule {
		return
	}
	base := m.ctx
	if base == nil {
		base = context.Background()
	}
	go func() {
		m.sleep(base, provisionLogDebounce)
		m.withProv(func(p *provisioner) { p.logScheduled = false })
		m.saveProvisionLog()
	}()
}

func (m *Module) pingProvisionStage(node string, stage int, ok bool, detail string) {
	bounded := boundedStage(stage)
	if node == "" {
		node = "new mirror"
	}
	outcome := "failed"
	if ok {
		outcome = "succeeded"
	}
	body := fmt.Sprintf("%s · step %d of %d %s.", node, bounded, provisionStageCount, outcome)
	if summary := simplified(detail); summary != "" {
		body += " " + summary
	}
	m.broadcastUI("core.hosts.notify", map[string]any{
		"title": "Vultr mirror deployment · " + provisionStageNames[bounded-1] + " " + outcome,
		"body":  body,
		"error": !ok,
	})
}

func (r *provRun) alive() bool {
	alive := false
	r.m.withProv(func(p *provisioner) { alive = p.Active && p.run == r.gen })
	return alive && r.ctx.Err() == nil
}

func (r *provRun) log(text string) { r.m.appendOutput(installChannel, text) }

func (r *provRun) event(text string) { r.m.appendOutput(installChannel, r.m.timestamped(text)) }

func (r *provRun) setStage(stage int, detail string) bool {
	alive := false
	pingStage := 0
	pingDetail, node, status := "", "", ""
	r.m.withProv(func(p *provisioner) {
		alive = p.Active && p.run == r.gen
		if !alive {
			return
		}
		bounded := boundedStage(stage)
		previousStage, previousDetail := p.stage, p.detail
		if p.resumeChain {
			if bounded > p.stage {
				p.stage = bounded
			}
		} else {
			p.stage = bounded
		}
		if detail != "" {
			p.detail = detail
			p.stageDetails[boundedStage(p.stage)-1] = detail
		}
		if previousStage > 0 && p.stage > previousStage {
			pingStage, pingDetail = previousStage, previousDetail
		}
		node, status = p.node, p.detail
	})
	if !alive {
		return false
	}
	if pingStage > 0 {
		r.m.pingProvisionStage(node, pingStage, true, pingDetail)
	}
	if status != "" {
		r.m.setStatus("vultr", status)
	}
	r.m.persistProvision("active", nil)
	return true
}

func (r *provRun) finish(ok bool, message string) {
	alive := false
	node, stage := "", 0
	var job *Job
	r.m.withProv(func(p *provisioner) {
		alive = p.Active && p.run == r.gen
		if !alive {
			return
		}
		p.Active = false
		if !ok && message != "" {
			p.stageDetails[boundedStage(p.stage)-1] = message
		}
		node, stage, job = p.node, p.stage, p.job
	})
	if !alive {
		return
	}
	m := r.m
	if !ok {
		m.setStatus("vultr", "✘ "+message)
	}
	if message != "" {
		mark := "✘ "
		if ok {
			mark = "✔ "
		}
		m.appendOutput(installChannel, "\n"+m.timestamped(mark+message+"\n"))
	}
	m.pingProvisionStage(node, stage, ok, message)
	m.withProv(func(p *provisioner) {
		p.requestedPlan = ""
		p.requestedRegion = ""
	})
	m.finishHostAction("install", node, ok, message)
	if ok {
		m.setStatus("vultr", "")
	}
	m.saveProvisionLog()
	m.withProv(func(p *provisioner) {
		p.Active = false
		p.resumeRequested = !ok
		p.resumeChain = false
	})
	state := "failed"
	if ok {
		state = "succeeded"
	}
	m.persistProvision(state, &message)
	m.withProv(func(p *provisioner) {
		p.tunnelToken = ""
		if p.job == job {
			p.job = nil
		}
	})
	if job != nil {
		m.finishJob(job, ok, message, nil)
	}
}

func fileReadable(path string) bool {
	if strings.TrimSpace(path) == "" {
		return false
	}
	file, err := os.Open(path)
	if err != nil {
		return false
	}
	_ = file.Close()
	return true
}

func (m *Module) createVultrMirror(ctx context.Context, node, formKey, plan, region, origin string) (bool, string, string) {
	agentClis := false
	m.withProv(func(p *provisioner) { agentClis = p.formAgentClis })
	return m.createProvision(ctx, node, formKey, plan, region, agentClis, origin)
}

func (m *Module) createProvision(ctx context.Context, node, formKey, plan, region string, installAgentClis bool, origin string) (bool, string, string) {
	resume, active := false, false
	m.withProv(func(p *provisioner) {
		resume, active = p.resumeRequested, p.Active
	})
	if resume {
		started, message := m.resumeWith(ctx, formKey, origin)
		settled := ""
		m.withProv(func(p *provisioner) { settled = p.node })
		return started, settled, message
	}
	refuse := func(text string) (bool, string, string) {
		m.setStatus("vultr", text)
		return false, "", text
	}
	if active {
		return refuse("A Vultr mirror is already being created.")
	}
	if m.installBusyLocal() {
		return refuse("Wait for the running install to finish first.")
	}
	apiKey, storedKey := m.vultrAPIKey(formKey)
	env := m.environment(ctx)
	node = strings.TrimSpace(node)
	hosts := m.rawSavedHosts()
	used := map[string]bool{}
	known := []string{}
	for i := range hosts {
		name := strings.ToLower(jsonString(objectAt(hosts, i)["name"]))
		used[name] = true
		known = append(known, name)
	}
	if node == "" {
		known = append(known, env.LinkedNodes...)
		node = nextMirrorNodeName(known)
		if node == "" {
			return refuse("Could not pick a free mirror name — enter one.")
		}
	} else if used[strings.ToLower(node)] {
		return refuse(fmt.Sprintf("A saved host named \"%s\" already exists — choose a different node name.", node))
	}
	if invalid := validateVultrMirrorRequest(apiKey, node); invalid != "" {
		return refuse(invalid)
	}
	token := cloudflareAPITokenFromVariables(m.actionVariables())
	hostname := vultrMirrorDNSHostname(node, m.cloudflareZone(env))
	if token == "" || strings.ContainsAny(token, "\n\r") || hostname == "" {
		return refuse("A Cloudflare API token and valid zone are required so the new mirror can create its Tunnel and serve live. Save CLOUDFLARE_API_TOKEN plus CLOUDFLARE_ZONE in Settings first.")
	}
	stores, storeError := m.rememberCloudflareToken(ctx, token)
	if len(stores) > 0 {
		m.appendOutput(installChannel, "Cloudflare credential kept in "+strings.Join(stores, " and ")+".\n")
	}
	if storeError != "" {
		m.appendOutput(installChannel, storeError+"\n")
	}
	useLocalBinary := localBinaryRunsOnVultrMirror(env.KernelType, env.CPUArch) && fileReadable(env.AppBinary)
	resumed := false
	var gen int64
	m.withProv(func(p *provisioner) {
		resumed = p.resumeChain
		if !resumed {
			p.stageDetails = [provisionStageCount]string{}
		}
		p.Active = true
		p.node = node
		p.pollCount = 0
		p.installAttempts = 0
		p.sshWaitCount = 0
		p.installUseLocalBinary = useLocalBinary
		p.installAgentClis = installAgentClis
		p.attemptLog = nil
		p.dnsHostname = hostname
		p.tunnelToken = token
		if !resumed {
			p.hostMetadata = map[string]any{
				"provider":     "Vultr",
				"displayName":  node,
				"dnsHostname":  hostname,
				"publicOrigin": "https://" + hostname,
			}
		}
		p.requestedPlan = plan
		p.requestedRegion = region
		p.origin = origin
		p.run++
		gen = p.run
	})
	r := m.beginRun(gen, node, apiKey, origin)
	if r == nil {
		return false, "", ""
	}
	if !resumed {
		m.clearOutput(installChannel)
		m.saveProvisionLog()
	}
	r.event(fmt.Sprintf("Creating Vultr mirror \"%s\" — cheapest supported US plan, latest Debian, managed SSH key…\n", node))
	if storedKey {
		r.log("Using the VULTR_API_KEY stored as a device variable.\n")
	}
	if useLocalBinary {
		r.log("This app's own binary will be uploaded over SSH, so the new mirror needs no published release to install.\n")
	}
	if installAgentClis {
		if env.AgentAccess == "" {
			r.log("Claude Code and Codex will be installed, but this device has no provider login to copy — the mirror will need its own sign-in.\n")
		} else {
			r.log(fmt.Sprintf("Claude Code and Codex will be installed and signed in with this device's access (%s).\n", env.AgentAccess))
		}
	}
	first := "Preparing credentials and shared SSH key…"
	if resumed {
		first = "Resuming credentials and shared SSH key…"
	}
	r.setStage(1, first)
	go r.create(node)
	return true, node, ""
}

func (m *Module) beginRun(gen int64, node, apiKey, origin string) *provRun {
	job := m.startJob("provision", node, "", origin, installChannel)
	current := false
	m.withProv(func(p *provisioner) {
		current = p.Active && p.run == gen
		if current {
			p.job = job
		}
	})
	if !current {
		m.finishJob(job, false, "", nil)
		return nil
	}
	m.broadcastProvision()
	return &provRun{m: m, gen: gen, ctx: job.Context(), apiKey: apiKey, origin: origin}
}

func (m *Module) ensureSharedHostKeypair(ctx context.Context) (string, string, string) {
	dir := ensureSSHDir(m.sshDir())
	if dir == "" {
		return "", "", "ForkMesh could not create its managed SSH key directory."
	}
	keyPath := sharedHostKeyPath(dir)
	readPublicKey := func() string {
		file, err := os.Open(keyPath + ".pub")
		if err != nil {
			return ""
		}
		defer file.Close()
		data, _ := io.ReadAll(io.LimitReader(file, 64*1024))
		return strings.TrimSpace(strings.ToValidUTF8(string(data), "�"))
	}
	if isFile(keyPath) {
		if publicKey := readPublicKey(); publicKey != "" {
			return keyPath, publicKey, ""
		}
	}
	m.appendOutput(installChannel, "Generating the shared ForkMesh SSH key for this fleet…\n")
	result := runProcess(ctx, procSpec{Program: "ssh-keygen", Args: []string{"-q", "-t", "ed25519", "-N", "", "-C", sharedHostKeyComment, "-f", keyPath}})
	if !result.Started {
		return "", "", "Could not start ssh-keygen. Install OpenSSH and try again."
	}
	if !result.OK() || !isFile(keyPath) {
		return "", "", fmt.Sprintf("ssh-keygen could not create the managed key (exit %d).", result.ExitCode)
	}
	_ = os.Chmod(keyPath, 0o600)
	publicKey := readPublicKey()
	if publicKey == "" {
		return "", "", "The managed key was created but its public half could not be read."
	}
	return keyPath, publicKey, ""
}

func (m *Module) resolveVultrSSHKeyID(ctx context.Context, apiKey, publicKey string) (string, string) {
	parts := spaceFields(publicKey)
	blob := publicKey
	if len(parts) >= 2 {
		blob = parts[0] + " " + parts[1]
	}
	result, failure := m.vultrCall(ctx, apiKey, http.MethodGet, "/v2/ssh-keys?per_page=500", nil)
	if failure != "" {
		return "", failure
	}
	for _, key := range jsonObjects(result["ssh_keys"]) {
		id := jsonString(key["id"])
		if id != "" && strings.HasPrefix(strings.TrimSpace(jsonString(key["ssh_key"])), blob) {
			return id, ""
		}
	}
	created, failure := m.vultrCall(ctx, apiKey, http.MethodPost, "/v2/ssh-keys", map[string]any{"name": sharedHostKeyComment, "ssh_key": publicKey})
	if failure != "" {
		return "", failure
	}
	id := jsonString(jsonObject(created["ssh_key"])["id"])
	if id == "" {
		return "", "Vultr did not return an SSH key id."
	}
	return id, ""
}

func (m *Module) findVultrProvisionInstance(ctx context.Context, apiKey, node string) (string, string) {
	result, failure := m.vultrCall(ctx, apiKey, http.MethodGet, "/v2/instances?per_page=500", nil)
	if failure != "" {
		return "", failure
	}
	for _, instance := range jsonObjects(result["instances"]) {
		if strings.EqualFold(jsonString(instance["label"]), node) || strings.EqualFold(jsonString(instance["hostname"]), node) {
			return jsonString(instance["id"]), ""
		}
	}
	return "", ""
}

func (r *provRun) create(node string) {
	m := r.m
	keyPath, publicKey, keyError := m.ensureSharedHostKeypair(r.ctx)
	if keyError != "" {
		r.finish(false, keyError)
		return
	}
	if !r.alive() {
		return
	}
	m.withProv(func(p *provisioner) { p.identityFile = keyPath })
	m.persistProvision("", nil)
	r.event("Shared ForkMesh SSH key: " + keyPath + "\n")
	m.setStatus("vultr", "Registering the SSH key with Vultr…")
	sshKeyID, sshError := m.resolveVultrSSHKeyID(r.ctx, r.apiKey, publicKey)
	if sshError != "" {
		r.finish(false, sshError)
		return
	}
	if !r.setStage(2, "Choosing a supported plan and Debian image…") {
		return
	}
	plans, planError := m.vultrCall(r.ctx, r.apiKey, http.MethodGet, "/v2/plans?per_page=500", nil)
	if planError != "" {
		r.finish(false, planError)
		return
	}
	requestedPlan, requestedRegion := "", ""
	m.withProv(func(p *provisioner) {
		requestedPlan, requestedRegion = p.requestedPlan, p.requestedRegion
		p.requestedPlan, p.requestedRegion = "", ""
	})
	plan, region, choiceError := chooseVultrPlan(jsonObjects(plans["plans"]), requestedPlan, requestedRegion)
	if plan == nil || region == "" {
		if requestedPlan == "" && requestedRegion == "" {
			choiceError = "No deployable US plan is available on this Vultr account (New Jersey, Atlanta, or another supported US region)."
		}
		r.finish(false, choiceError)
		return
	}
	m.withProv(func(p *provisioner) {
		if p.hostMetadata == nil {
			p.hostMetadata = map[string]any{}
		}
		for field, source := range map[string]string{"planType": "id", "monthlyCost": "monthly_cost", "ramMb": "ram", "diskGb": "disk"} {
			if value, ok := plan[source]; ok && value != nil {
				p.hostMetadata[field] = value
			} else {
				delete(p.hostMetadata, field)
			}
		}
		p.hostMetadata["region"] = region
	})
	r.event(fmt.Sprintf("Cheapest supported plan: %s ($%s/month, %d MB RAM, %d GB disk) in region %s\n",
		jsonString(plan["id"]), qtNumber(jsonDouble(plan["monthly_cost"])), qtJSONInt(plan["ram"]), qtJSONInt(plan["disk"]), region))
	osList, osError := m.vultrCall(r.ctx, r.apiKey, http.MethodGet, "/v2/os?per_page=500", nil)
	if osError != "" {
		r.finish(false, osError)
		return
	}
	debian := latestVultrDebianOS(jsonObjects(osList["os"]))
	if debian == nil {
		r.finish(false, "Vultr offers no Debian x64 image right now.")
		return
	}
	r.event("Operating system: " + jsonString(debian["name"]) + "\n")
	if !r.setStage(3, "Checking for an existing server before creation…") {
		return
	}
	payload := vultrInstanceCreatePayload(node, jsonString(plan["id"]), region, qtJSONInt(debian["id"]), sshKeyID)
	existingID, findError := m.findVultrProvisionInstance(r.ctx, r.apiKey, node)
	if findError != "" {
		r.finish(false, findError)
		return
	}
	instanceID := existingID
	reused := existingID != ""
	if !reused {
		if !r.setStage(3, "Creating the Vultr server…") {
			return
		}
		created, createError := m.vultrCall(r.ctx, r.apiKey, http.MethodPost, "/v2/instances", payload)
		if createError != "" {
			r.finish(false, createError)
			return
		}
		instanceID = jsonString(jsonObject(created["instance"])["id"])
	}
	if !r.alive() {
		return
	}
	if instanceID == "" {
		r.finish(false, "Vultr did not return an instance id.")
		return
	}
	m.withProv(func(p *provisioner) {
		p.instanceID = instanceID
		p.identityFile = keyPath
		if p.hostMetadata == nil {
			p.hostMetadata = map[string]any{}
		}
		p.hostMetadata["instanceId"] = instanceID
	})
	if reused {
		r.event(fmt.Sprintf("Found existing instance %s for %s; resuming instead of creating a duplicate.\n", instanceID, node))
	} else {
		r.event(fmt.Sprintf("Instance %s created — waiting for it to boot…\n", instanceID))
	}
	m.withProv(func(p *provisioner) { p.resumeChain = false })
	if !r.setStage(4, "Waiting for the server to boot and accept SSH…") {
		return
	}
	r.poll(instanceID, node, keyPath)
}

func (r *provRun) poll(instanceID, node, identityFile string) {
	m := r.m
	for {
		if !r.alive() {
			return
		}
		result, failure := m.vultrCall(r.ctx, r.apiKey, http.MethodGet, "/v2/instances/"+instanceID, nil)
		if !r.alive() {
			return
		}
		if failure != "" {
			r.event("Server status check failed: " + failure + "; retrying.\n")
			if !r.setStage(4, "Vultr status check was interrupted; retrying automatically…") || !m.sleep(r.ctx, provisionPollInterval) {
				return
			}
			continue
		}
		instance := jsonObject(result["instance"])
		ip := vultrInstanceReadyIP(instance)
		if ip == "" {
			if vultrInstanceIsIPv6Only(instance) {
				r.finish(false, fmt.Sprintf("Vultr gave this instance an IPv6 address only, which the mesh cannot reach. Destroy instance %s in the Vultr panel and retry — ForkMesh only deploys IPv4 plans.", instanceID))
				return
			}
			count := 0
			m.withProv(func(p *provisioner) {
				p.pollCount++
				count = p.pollCount
			})
			if count >= provisionMaxPolls {
				r.finish(false, "The instance did not become ready in time. Check it in the Vultr panel, then Add host + Install ForkMesh manually once it is up.")
				return
			}
			if !r.setStage(4, fmt.Sprintf("Waiting for the server to boot (status: %s, check %d/%d)…", jsonString(instance["status"]), count+1, provisionMaxPolls)) ||
				!m.sleep(r.ctx, provisionPollInterval) {
				return
			}
			continue
		}
		r.event("Instance is up at " + ip + ".\n")
		metadata := map[string]any{}
		m.withProv(func(p *provisioner) {
			p.instanceID = instanceID
			p.ip = ip
			p.identityFile = identityFile
			metadata = copyObject(p.hostMetadata)
		})
		m.rememberHost(node, ip, "root", "", "vultr booting", identityFile, metadata)
		m.withProv(func(p *provisioner) { p.sshWaitCount = 0 })
		r.waitForSSH(node, ip, identityFile)
		return
	}
}

func (r *provRun) waitForSSH(node, ip, identityFile string) {
	m := r.m
	for {
		if !r.alive() {
			return
		}
		if note := nonRoutableAddressNote(ip); note != "" {
			r.finish(false, fmt.Sprintf("%s is in %s, so SSH from this machine can never reach it. Give the instance a public address (or run the install from the network that owns that range); it is saved under Hosts — fix the address there and click Update.", ip, note))
			return
		}
		command, err := buildHostSSHCommand(m.sshDir(), ip, "root", "", vultrSSHProbeRemoteCommand(), identityFile)
		if err != nil {
			r.finish(false, err.Error())
			return
		}
		knock := 0
		m.withProv(func(p *provisioner) {
			p.sshWaitCount++
			knock = p.sshWaitCount
		})
		m.setStatus("vultr", fmt.Sprintf("Waiting for SSH on %s (knock %d of %d)…", ip, knock, provisionSSHProbes))
		started := time.Now()
		output := &cappedBuffer{limit: execMaxOutput}
		result := runProcess(r.ctx, procSpec{Program: command.Program, Args: command.Args, Env: command.Env,
			OnOutput: func(chunk []byte) { _, _ = output.Write(chunk) }})
		if !r.alive() {
			return
		}
		if !result.Started {
			r.finish(false, fmt.Sprintf("\"%s\" could not be started on this machine, so the new instance cannot be reached. Install it and click Update on the host saved under Hosts.", command.Program))
			return
		}
		exitCode := result.ExitCode
		if result.Crashed || result.TimedOut {
			exitCode = 255
		}
		raw := output.Bytes()
		if len(raw) > 2048 {
			raw = raw[len(raw)-2048:]
		}
		tail := redactProcessOutput(strings.ToValidUTF8(string(raw), "�"))
		if vultrSSHProbeReady(exitCode, tail) {
			noun := "knocks"
			if knock == 1 {
				noun = "knock"
			}
			r.event(fmt.Sprintf("SSH answered on %s after %d %s.\n", ip, knock, noun))
			r.startInstall(node, ip, identityFile)
			return
		}
		if knock >= 3 && sshFailureNeedsPassword(exitCode, tail) {
			r.log("SSH probe: " + sshFailureSummary(exitCode, tail) + "\n")
			r.finish(false, "The new instance refused the managed SSH key. It is saved under Hosts — fix its credentials there and click Update to finish the install.")
			return
		}
		if knock >= provisionSSHProbes {
			r.log("SSH probe: " + sshFailureSummary(exitCode, tail) + "\n")
			r.finish(false, fmt.Sprintf("SSH on %s never answered (%d attempts over %d minutes). The instance is saved under Hosts — click Update there once it is reachable.", ip, provisionSSHProbes, provisionSSHProbes))
			return
		}
		wait := provisionSSHInterval - time.Since(started)
		if wait < 0 {
			wait = 0
		}
		if wait > provisionSSHInterval {
			wait = provisionSSHInterval
		}
		if !m.sleep(r.ctx, wait) {
			return
		}
	}
}

func (r *provRun) appendAttemptHistory() {
	entries := []string{}
	r.m.withProv(func(p *provisioner) { entries = append(entries, p.attemptLog...) })
	if len(entries) == 0 {
		return
	}
	block := "\nInstall attempts this run:\n"
	for _, entry := range entries {
		block += "  • " + entry + "\n"
	}
	r.log(block)
}

func (r *provRun) startInstall(node, ip, identityFile string) {
	m := r.m
	if !r.alive() {
		return
	}
	if !r.setStage(5, "SSH is ready — installing ForkMesh…") {
		return
	}
	attempts := 0
	useLocalBinary, agentClis := false, false
	dnsHostname, tunnelToken := "", ""
	m.withProv(func(p *provisioner) {
		p.installAttempts++
		attempts = p.installAttempts
		p.node = node
		p.ip = ip
		p.identityFile = identityFile
		useLocalBinary, agentClis = p.installUseLocalBinary, p.installAgentClis
		dnsHostname, tunnelToken = p.dnsHostname, p.tunnelToken
	})
	label := fmt.Sprintf("Attempt %d of %d started at %s", attempts, provisionMaxInstalls, isoTimestamp(m.now()))
	began := m.now()
	m.setStatus("vultr", fmt.Sprintf("Installing the Go mirror server (attempt %d of %d) — no Qt or GTK packages…", attempts, provisionMaxInstalls))
	isFinal := attempts >= provisionMaxInstalls
	installed := make(chan bool, 1)
	m.runHostInstall(r.ctx, installRequest{Node: node, IP: ip, User: "root", ForceUpload: useLocalBinary, AttemptBanner: label,
		SuppressFailureStatus: !isFinal, Origin: r.origin, Vultr: &vultrInstallTarget{DNSHostname: dnsHostname, TunnelToken: tunnelToken}},
		func(ok bool, _ string) { installed <- ok })
	ok := false
	select {
	case ok = <-installed:
	case <-r.ctx.Done():
		return
	}
	if !r.alive() {
		return
	}
	verdict := "installed"
	if !ok {
		verdict = m.lastFailure()
		if verdict == "" {
			verdict = "failed"
		}
	}
	entry := fmt.Sprintf("%s — finished at %s after %s — %s", label, isoTimestamp(m.now()), conciseElapsedTime(m.now().Sub(began).Milliseconds()), verdict)
	m.withProv(func(p *provisioner) { p.attemptLog = append(p.attemptLog, entry) })
	if ok {
		r.appendAttemptHistory()
		address := ip
		if dnsHostname != "" {
			address = dnsHostname + ", " + ip
		}
		done := fmt.Sprintf("Vultr mirror \"%s\" (%s) is installed, linked, healthy, and serving repository traffic.", node, address)
		if !agentClis {
			r.seedThenVerify(node, done)
			return
		}
		copyLogins := m.environment(r.ctx).AgentCredentials
		if !r.setStage(5, "ForkMesh installed; setting up Claude Code and Codex…") {
			return
		}
		type agentOutcome struct {
			ok      bool
			message string
		}
		agents := make(chan agentOutcome, 1)
		m.runAgentCliInstall(r.ctx, node, ip, "root", "", identityFile, copyLogins, r.origin, func(agentOK bool, message string) {
			agents <- agentOutcome{agentOK, message}
		})
		var outcome agentOutcome
		select {
		case outcome = <-agents:
		case <-r.ctx.Done():
			return
		}
		if !r.alive() {
			return
		}
		if outcome.ok {
			r.seedThenVerify(node, done+" "+outcome.message)
		} else {
			r.seedThenVerify(node, done+" The agent CLIs were not set up: "+outcome.message)
		}
		return
	}
	if note := nonRoutableAddressNote(ip); note != "" {
		r.appendAttemptHistory()
		r.finish(false, fmt.Sprintf("%s is in %s, so SSH from this machine can never reach it. Give the instance a public address (or run the install from the network that owns that range); it is saved under Hosts — fix the address there and click Update.", ip, note))
		return
	}
	r.appendAttemptHistory()
	r.finish(false, "Install did not succeed. The instance is saved under Hosts — the transcript above shows why; click Update there to retry.")
}

func (r *provRun) seedThenVerify(node, successMessage string) {
	if !r.alive() {
		return
	}
	r.seedPush()
	if !r.alive() {
		return
	}
	r.waitForPublication(node, successMessage)
}

func lastLine(text string) string {
	lines := strings.Split(text, "\n")
	return strings.TrimSpace(lines[len(lines)-1])
}

func (r *provRun) seedPush() bool {
	m := r.m
	env := m.environment(r.ctx)
	ip, identityFile := "", ""
	m.withProv(func(p *provisioner) { ip, identityFile = p.ip, p.identityFile })
	bareMirror := env.MirrorRoot + "/forkmesh-forkmesh.git"
	args := mirrorSeedPushArguments(bareMirror, ip)
	_, statErr := os.Stat(bareMirror)
	if len(args) == 0 || strings.TrimSpace(identityFile) == "" || statErr != nil {
		r.log("Skipping the repository seed push (no local bare mirror, address, or SSH identity available).\n")
		return false
	}
	busy := false
	m.withProv(func(p *provisioner) {
		busy = p.seeding
		p.seeding = true
	})
	if busy {
		r.log("A repository seed push is already running; not starting another.\n")
		return false
	}
	defer m.withProv(func(p *provisioner) { p.seeding = false })
	r.log(fmt.Sprintf("Seeding forkmesh/forkmesh on %s from this node's local mirror (a no-op when the node already cloned it)…\n", ip))
	output := &cappedBuffer{limit: execMaxOutput}
	environment := append(environmentWithout("GIT_SSH_COMMAND"), "GIT_SSH_COMMAND="+mirrorSeedSSHCommand(identityFile))
	result := runProcess(r.ctx, procSpec{Program: "git", Args: args, Env: environment, Timeout: seedPushCap,
		OnOutput: func(chunk []byte) { _, _ = output.Write(chunk) }})
	ok := result.OK()
	if ok {
		r.log("✔ Repository seed push completed.\n")
		return true
	}
	text := strings.TrimSpace(strings.ToValidUTF8(string(output.Bytes()), "�"))
	suffix := ""
	if text != "" {
		suffix = ": " + lastLine(text)
	}
	r.log(fmt.Sprintf("The repository seed push did not complete%s — the node will rely on the public mesh for its first sync.\n", suffix))
	return false
}

func (r *provRun) diagnose(node string, restart bool) string {
	command := "true"
	if restart {
		command = diagnoseRestartCommand
	}
	ip, identityFile := "", ""
	r.m.withProv(func(p *provisioner) { ip, identityFile = p.ip, p.identityFile })
	ok, output := r.m.runMirrorNodeSSH(r.ctx, node, qtArg(diagnoseRemoteTemplate, command), diagnoseTimeout, ip, identityFile)
	if !ok {
		return ""
	}
	return output
}

func (r *provRun) fetchHealth(dnsHostname, nonce string, issuedAt int64) map[string]any {
	ctx, cancel := context.WithTimeout(r.ctx, healthTimeout)
	defer cancel()
	target := "https://" + strings.ToLower(strings.TrimSpace(dnsHostname)) + "/health?nonce=" + percentEncode(nonce) +
		"&issuedAt=" + strconv.FormatInt(issuedAt, 10) + "&owner=forkmesh&repo=forkmesh"
	request, err := http.NewRequestWithContext(ctx, http.MethodGet, target, nil)
	if err != nil {
		return map[string]any{}
	}
	request.Header.Set("accept", "application/json")
	response, err := r.m.httpClient().Do(request)
	if err != nil {
		return map[string]any{}
	}
	defer response.Body.Close()
	raw, _ := io.ReadAll(io.LimitReader(response.Body, apiBodyLimit))
	payload := map[string]any{}
	if json.Unmarshal(raw, &payload) != nil || payload == nil {
		return map[string]any{}
	}
	return payload
}

func (r *provRun) waitForPublication(node, successMessage string) {
	m := r.m
	const maxPolls = publicationFastPolls + publicationSlowPolls
	m.withProv(func(p *provisioner) {
		p.node = node
		p.resumeChain = false
		p.publicationGap = ""
	})
	for attempt := 0; ; attempt++ {
		if !r.alive() {
			return
		}
		gap, dnsHostname := "", ""
		m.withProv(func(p *provisioner) { gap, dnsHostname = p.publicationGap, p.dnsHostname })
		missing := ""
		if gap != "" {
			missing = " Still missing: " + gap + "."
		}
		if !r.setStage(6, fmt.Sprintf("ForkMesh is running on %s — verifying healthy public traffic (%d/%d)…%s", node, attempt+1, maxPolls, missing)) {
			return
		}
		issuedAt := m.nowMs()
		nonce := newUUID()
		payload := r.fetchHealth(dnsHostname, nonce, issuedAt)
		if !r.alive() {
			return
		}
		proof := jsonObject(payload["repositoryProof"])
		published := verifiedFlagshipMirrorHealth(payload, node, nonce, issuedAt)
		if !published {
			emptyRepository := len(proof) > 0 && !mirrorRepositoryProofServesContent(jsonString(proof["refsSha256"]))
			next := "the signed repository health proof is invalid"
			switch {
			case len(payload) == 0:
				next = "its direct HTTPS endpoint is not answering"
			case len(proof) == 0:
				next = "the forkmesh/forkmesh repository proof is missing"
			case emptyRepository:
				next = "its repository is still empty — no refs to serve yet"
			}
			if next != gap {
				r.log(fmt.Sprintf("%s is not a healthy public mirror yet: %s.\n", node, next))
			}
			gap = next
			m.withProv(func(p *provisioner) { p.publicationGap = next })
		}
		if published {
			r.log(fmt.Sprintf("\n✔ Verified %s directly: its HTTPS endpoint returned a valid signed forkmesh/forkmesh repository proof. No relay Worker was used.\n", node))
			ip, identityFile := "", ""
			metadata := map[string]any{}
			m.withProv(func(p *provisioner) {
				ip, identityFile = p.ip, p.identityFile
				metadata = copyObject(p.hostMetadata)
			})
			m.rememberHost(node, ip, "root", "", "installed", identityFile, metadata)
			r.finish(true, successMessage)
			return
		}
		if attempt+1 >= maxPolls {
			r.log(fmt.Sprintf("%s never became a healthy public mirror; asking the node why…\n", node))
			report := r.diagnose(node, false)
			if report != "" {
				r.log(report + "\n")
			}
			gapText, reportText := "", ""
			if gap != "" {
				gapText = " Direct verification was still missing: " + gap + "."
			}
			if report != "" {
				reportText = " It last reported: " + lastLine(report)
			}
			r.finish(false, fmt.Sprintf("%s is installed and running but never returned a valid signed repository health proof, so the deployment was ended after thirty minutes.%s%s The server is saved under Hosts — click Retry deployment once the cause is cleared.", node, gapText, reportText))
			return
		}
		if attempt+1 == publicationFastPolls {
			r.log(fmt.Sprintf("%s has not served a valid repository proof after fifteen minutes; re-seeding its repository and restarting its mirror node…\n", node))
			go func() {
				r.seedPush()
				if report := r.diagnose(node, true); report != "" {
					r.log(report + "\n")
				}
			}()
		}
		interval := publicationSlowInterval
		if attempt+1 < publicationFastPolls {
			interval = publicationFastInterval
		}
		if !m.sleep(r.ctx, interval) {
			return
		}
	}
}

func (m *Module) resumeProvision(ctx context.Context, formKey string) (bool, string) {
	return m.resumeWith(ctx, formKey, "")
}

func (m *Module) resumeWith(ctx context.Context, formKey, origin string) (bool, string) {
	active, requested := false, false
	node, ip, identityFile, instanceID, dnsHostname := "", "", "", "", ""
	stage := 0
	agentClis := false
	m.withProv(func(p *provisioner) {
		active, requested = p.Active, p.resumeRequested
		node, ip, identityFile, instanceID, dnsHostname = p.node, p.ip, p.identityFile, p.instanceID, p.dnsHostname
		stage, agentClis = p.stage, p.installAgentClis
	})
	if active {
		return true, ""
	}
	if !requested || node == "" {
		return false, ""
	}
	apiKey, _ := m.vultrAPIKey(formKey)
	if invalid := validateVultrMirrorRequest(apiKey, node); invalid != "" {
		m.withProv(func(p *provisioner) { p.Active = false })
		message := "Deployment is paused. Enter the saved Vultr API key, then click Resume deployment. " + invalid
		m.setStatus("vultr", message)
		m.broadcastProvision()
		return false, message
	}
	token := cloudflareAPITokenFromVariables(m.actionVariables())
	resumeLine := fmt.Sprintf("\n↻ Desktop restarted — resuming Vultr deployment at stage %d.\n", stage)
	seed := stage >= 6
	install := !seed && stage >= 5 && ip != "" && identityFile != ""
	poll := !seed && !install && instanceID != "" && identityFile != ""
	if !seed && !install && !poll {
		m.withProv(func(p *provisioner) {
			p.tunnelToken = token
			p.resumeRequested = false
			p.resumeChain = true
			p.Active = false
		})
		m.appendOutput(installChannel, resumeLine)
		started, _, message := m.createProvision(ctx, node, formKey, "", "", agentClis, origin)
		return started, message
	}
	var gen int64
	claimed := false
	m.withProv(func(p *provisioner) {
		if p.Active {
			return
		}
		claimed = true
		p.tunnelToken = token
		p.Active = true
		p.origin = origin
		p.run++
		gen = p.run
	})
	if !claimed {
		return true, ""
	}
	r := m.beginRun(gen, node, apiKey, origin)
	if r == nil {
		return false, ""
	}
	r.log(resumeLine)
	switch {
	case seed:
		address := dnsHostname
		if address == "" {
			address = ip
		}
		go r.seedThenVerify(node, fmt.Sprintf("Vultr mirror \"%s\" (%s) is installed, linked, and serving traffic.", node, address))
	case install:
		m.withProv(func(p *provisioner) { p.installAttempts = 0 })
		m.persistProvision("", nil)
		go func() {
			if !m.sleep(r.ctx, provisionResumeDelay) {
				return
			}
			r.startInstall(node, ip, identityFile)
		}()
	default:
		r.setStage(4, "Resuming server boot checks…")
		go r.poll(instanceID, node, identityFile)
	}
	return true, ""
}

func (m *Module) endProvision() {
	has := false
	node := ""
	var job *Job
	m.withProv(func(p *provisioner) {
		has = p.Active || p.state == "active" || p.state == "failed"
		if !has {
			return
		}
		node, job = p.node, p.job
		p.Active = false
		p.run++
		p.job = nil
		p.resumeRequested = false
		p.resumeChain = false
		p.state = ""
		p.stage = 0
		p.detail = ""
		p.message = ""
		p.node = ""
		p.instanceID = ""
		p.ip = ""
		p.identityFile = ""
		p.dnsHostname = ""
		p.hostMetadata = map[string]any{}
		p.pollCount = 0
		p.installAttempts = 0
		p.tunnelToken = ""
		p.publicationGap = ""
		p.stageDetails = [provisionStageCount]string{}
	})
	if !has {
		return
	}
	if job != nil {
		m.finishJob(job, false, "Deployment ended by request.", nil)
	}
	m.writeSettings(daemon.SettingWrite{Key: provisionSetting, Remove: true})
	m.setStatus("vultr", "")
	suffix := ""
	if node != "" {
		suffix = " (" + node + ")"
	}
	m.appendOutput(installChannel, "\n⚠ Deployment ended by request"+suffix+".\n")
	m.saveProvisionLog()
	m.broadcastProvision()
}

func (m *Module) startProvision(ctx context.Context) {
	var saved map[string]any
	if json.Unmarshal([]byte(m.settingString(provisionSetting)), &saved) != nil || saved == nil {
		return
	}
	if qtJSONInt(saved["version"]) != 1 {
		return
	}
	state := ""
	restored := ""
	m.withProv(func(p *provisioner) {
		p.state = jsonString(saved["state"])
		p.stage = qtJSONInt(saved["stage"])
		p.detail = jsonString(saved["detail"])
		p.message = jsonString(saved["message"])
		p.node = jsonString(saved["node"])
		p.dnsHostname = jsonString(saved["dnsHostname"])
		p.instanceID = jsonString(saved["instanceId"])
		p.ip = jsonString(saved["ip"])
		p.identityFile = jsonString(saved["identityFile"])
		p.installAgentClis = jsonBool(saved["installAgentClis"])
		p.installUseLocalBinary = jsonBool(saved["installUseLocalBinary"])
		p.pollCount = qtJSONInt(saved["pollCount"])
		p.installAttempts = qtJSONInt(saved["installAttempts"])
		p.hostMetadata = copyObject(jsonObject(saved["hostMetadata"]))
		p.formAgentClis = p.installAgentClis
		p.stageDetails = [provisionStageCount]string{}
		if p.detail != "" {
			p.stageDetails[boundedStage(p.stage)-1] = p.detail
		}
		p.resumeRequested = p.state == "active" || p.state == "failed"
		state = p.state
		restored = p.message
		if restored == "" {
			restored = p.detail
		}
	})
	if m.uiAttached() {
		m.environment(ctx)
	}
	if m.transcriptText(installChannel) == "" {
		if logged := fileHead(m.provisionLogFile(), provisionLogCap); len(logged) > 0 {
			m.appendOutput(installChannel, strings.ToValidUTF8(string(logged), "�"))
		}
	}
	m.setStatus("vultr", restored)
	m.broadcastProvision()
	if state != "active" {
		return
	}
	go func() {
		for !m.uiAttached() {
			if !m.sleep(ctx, provisionAttachWait) {
				return
			}
		}
		m.resumeWith(ctx, "", "core")
	}()
}

func fileHead(path string, limit int64) []byte {
	if path == "" {
		return nil
	}
	file, err := os.Open(path)
	if err != nil {
		return nil
	}
	defer file.Close()
	data, _ := io.ReadAll(io.LimitReader(file, limit))
	return data
}

func (m *Module) registerProvisionMethods(desktop func(string, rpc.Handler)) {
	desktop("core.hosts.provision.create", m.handleProvisionCreate)
	desktop("core.hosts.provision.resume", m.handleProvisionResume)
	desktop("core.hosts.provision.end", m.handleProvisionEnd)
	desktop("core.hosts.provision.state", m.handleProvisionState)
	desktop("core.hosts.destroy", m.handleDestroy)
	desktop("core.hosts.deleteNode", m.handleDeleteNode)
}

func (m *Module) handleProvisionCreate(ctx context.Context, req *rpc.Request) (any, error) {
	var params struct {
		Node             string `json:"node"`
		APIKey           string `json:"apiKey"`
		InstallAgentClis bool   `json:"installAgentClis"`
		Plan             string `json:"plan"`
		Region           string `json:"region"`
	}
	if err := req.Bind(&params); err != nil {
		return nil, err
	}
	m.withProv(func(p *provisioner) { p.formAgentClis = params.InstallAgentClis })
	started, node, message := m.createProvision(m.ctx, params.Node, params.APIKey, params.Plan, params.Region, params.InstallAgentClis, "desktop")
	return map[string]any{"started": started, "node": node, "message": message}, nil
}

func (m *Module) handleProvisionResume(ctx context.Context, req *rpc.Request) (any, error) {
	var params struct {
		APIKey string `json:"apiKey"`
	}
	if err := req.Bind(&params); err != nil {
		return nil, err
	}
	started, message := m.resumeWith(m.ctx, params.APIKey, "desktop")
	_, node, _ := m.provisionResumable()
	return map[string]any{"started": started, "node": node, "message": message}, nil
}

func (m *Module) handleProvisionEnd(ctx context.Context, req *rpc.Request) (any, error) {
	m.endProvision()
	return map[string]any{}, nil
}

func (m *Module) handleProvisionState(ctx context.Context, req *rpc.Request) (any, error) {
	return m.provisionSnapshot(), nil
}
