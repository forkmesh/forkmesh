package hosts

import (
	"crypto/rand"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"math"
	"regexp"
	"strconv"
	"strings"

	"forkmesh.com/desktop-core/internal/identity"
)

const (
	vultrSSHProbeMarker = "forkmesh-ssh-ready"
	emptyRefsSha256     = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
	vultrAPIKeyHint     = "Enter your Vultr API key (Account → API in the Vultr panel). It is used from memory only and never saved to disk."
)

var (
	vultrAPIKeyPattern     = regexp.MustCompile(`^[A-Za-z0-9]{20,128}$`)
	vultrNodePattern       = regexp.MustCompile(`^[a-z][a-z0-9-]{0,62}$`)
	vultrInstanceIDPattern = regexp.MustCompile(`^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$`)
	looseIPv4Pattern       = regexp.MustCompile(`^(?:\d{1,3}\.){3}\d{1,3}$`)
	firstNumberPattern     = regexp.MustCompile(`\b(\d+)\b`)
	cloudflareIDPattern    = regexp.MustCompile(`^[A-Za-z0-9_-]{1,128}$`)
)

func validateVultrMirrorRequest(apiKey, node string) string {
	if !vultrAPIKeyPattern.MatchString(strings.TrimSpace(apiKey)) {
		return vultrAPIKeyHint
	}
	if !vultrNodePattern.MatchString(strings.TrimSpace(node)) {
		return "The node name must start with a letter and contain only lowercase letters, digits, or hyphens (63 characters maximum)."
	}
	return ""
}

func validateVultrDestroyRequest(apiKey, instanceID string) string {
	if !vultrAPIKeyPattern.MatchString(strings.TrimSpace(apiKey)) {
		return vultrAPIKeyHint
	}
	if !vultrInstanceIDPattern.MatchString(strings.TrimSpace(instanceID)) {
		return "No Vultr instance is recorded for this host, so there is nothing safe to destroy. Delete it from the Vultr panel instead."
	}
	return ""
}

func jsonObject(value any) map[string]any {
	if object, ok := value.(map[string]any); ok {
		return object
	}
	return map[string]any{}
}

func jsonArray(value any) []any {
	if list, ok := value.([]any); ok {
		return list
	}
	return nil
}

func jsonObjects(value any) []map[string]any {
	list := jsonArray(value)
	out := make([]map[string]any, 0, len(list))
	for _, item := range list {
		out = append(out, jsonObject(item))
	}
	return out
}

func qtJSONInt(value any) int {
	number, ok := value.(float64)
	if !ok || math.IsNaN(number) || math.IsInf(number, 0) || number != math.Trunc(number) ||
		number < math.MinInt32 || number > math.MaxInt32 {
		return 0
	}
	return int(number)
}

func qtNumber(value float64) string { return strconv.FormatFloat(value, 'g', -1, 64) }

func simplified(text string) string { return strings.Join(strings.Fields(text), " ") }

func vultrPlanEligible(plan map[string]any) bool {
	return cheapestVultrPlan([]map[string]any{plan}) != nil
}

func chooseVultrPlan(plans []map[string]any, requestedPlan, requestedRegion string) (map[string]any, string, string) {
	var plan map[string]any
	wantedPlan := strings.TrimSpace(requestedPlan)
	if wantedPlan != "" {
		for _, candidate := range plans {
			if jsonString(candidate["id"]) == wantedPlan && vultrPlanEligible(candidate) {
				plan = candidate
				break
			}
		}
		if plan == nil {
			return nil, "", fmt.Sprintf("Vultr plan \"%s\" is not one a mirror can run on.", wantedPlan)
		}
	} else {
		plan = cheapestVultrPlan(plans)
	}
	if plan == nil {
		return nil, "", "No deployable US plan is available on this Vultr account."
	}
	regions := vultrPlanRegions(plan)
	wantedRegion := strings.ToLower(strings.TrimSpace(requestedRegion))
	chosen := ""
	if wantedRegion != "" {
		found := false
		for _, region := range regions {
			if region == wantedRegion {
				found = true
				break
			}
		}
		if !found {
			return nil, "", fmt.Sprintf("Plan \"%s\" does not deploy in region \"%s\".", jsonString(plan["id"]), wantedRegion)
		}
		chosen = wantedRegion
	} else if len(regions) > 0 {
		chosen = regions[0]
	}
	if chosen == "" {
		return nil, "", "The plan has no supported US region."
	}
	return plan, chosen, ""
}

func latestVultrDebianOS(list []map[string]any) map[string]any {
	var best map[string]any
	bestVersion := -1
	for _, os := range list {
		if strings.ToLower(jsonString(os["family"])) != "debian" || strings.ToLower(jsonString(os["arch"])) != "x64" ||
			qtJSONInt(os["id"]) <= 0 {
			continue
		}
		version := 0
		if match := firstNumberPattern.FindStringSubmatch(jsonString(os["name"])); match != nil {
			version, _ = strconv.Atoi(match[1])
		}
		if version > bestVersion || (version == bestVersion && qtJSONInt(os["id"]) > qtJSONInt(best["id"])) {
			best = os
			bestVersion = version
		}
	}
	return best
}

func localBinaryRunsOnVultrMirror(kernelType, cpuArch string) bool {
	os := strings.ToLower(strings.TrimSpace(kernelType))
	arch := strings.ToLower(strings.TrimSpace(cpuArch))
	if arch == "amd64" || arch == "x64" {
		arch = "x86_64"
	}
	return os == "linux" && arch == "x86_64"
}

func vultrInstanceCreatePayload(node, planID, regionID string, osID int, sshKeyID string) map[string]any {
	return map[string]any{
		"region":           regionID,
		"plan":             planID,
		"os_id":            osID,
		"label":            strings.TrimSpace(node),
		"hostname":         strings.TrimSpace(node),
		"sshkey_id":        []any{sshKeyID},
		"backups":          "disabled",
		"enable_ipv6":      false,
		"activation_email": false,
		"tags":             []any{"forkmesh-mirror"},
	}
}

func vultrInstanceReadyIP(instance map[string]any) string {
	if jsonString(instance["status"]) != "active" || jsonString(instance["power_status"]) != "running" {
		return ""
	}
	ip := strings.TrimSpace(jsonString(instance["main_ip"]))
	if ip == "" || ip == "0.0.0.0" || !looseIPv4Pattern.MatchString(ip) {
		return ""
	}
	return ip
}

func vultrInstanceIsIPv6Only(instance map[string]any) bool {
	v6 := strings.TrimSpace(jsonString(instance["v6_main_ip"]))
	if v6 == "" || !strings.Contains(v6, ":") {
		return false
	}
	ip := strings.TrimSpace(jsonString(instance["main_ip"]))
	return ip == "" || ip == "0.0.0.0"
}

func vultrSSHProbeRemoteCommand() string { return "echo " + vultrSSHProbeMarker }

func vultrSSHProbeReady(exitCode int, outputTail string) bool {
	return exitCode == 0 && strings.Contains(outputTail, vultrSSHProbeMarker)
}

func vultrInstanceIDForAddress(instances []map[string]any, address string) string {
	wanted := strings.ToLower(strings.TrimSpace(address))
	if wanted == "" {
		return ""
	}
	match := ""
	for _, instance := range instances {
		id := strings.TrimSpace(jsonString(instance["id"]))
		if id == "" {
			continue
		}
		hit := false
		for _, field := range []string{"main_ip", "v6_main_ip", "label", "hostname"} {
			candidate := strings.ToLower(strings.TrimSpace(jsonString(instance[field])))
			if candidate == "" || candidate == "0.0.0.0" {
				continue
			}
			if candidate == wanted {
				hit = true
				break
			}
		}
		if !hit {
			continue
		}
		if match != "" && match != id {
			return ""
		}
		match = id
	}
	return match
}

func mirrorRepositoryProofServesContent(refsSha256 string) bool {
	digest := strings.ToLower(strings.TrimSpace(refsSha256))
	return len(digest) == 64 && digest != emptyRefsSha256
}

func mirrorSeedSSHCommand(identityFile string) string {
	return "ssh -i '" + strings.ReplaceAll(identityFile, "'", `'\''`) + "' -o StrictHostKeyChecking=accept-new -o ConnectTimeout=10"
}

func mirrorSeedPushArguments(bareMirrorPath, address string) []string {
	mirror := strings.TrimSpace(bareMirrorPath)
	host := strings.TrimSpace(address)
	if mirror == "" || host == "" {
		return nil
	}
	if strings.Contains(host, ":") && !strings.HasPrefix(host, "[") {
		host = "[" + host + "]"
	}
	return []string{
		"--git-dir=" + mirror,
		"push",
		"--prune",
		"--receive-pack=runuser -u forkmesh-node -- git-receive-pack",
		"ssh://root@" + host + "/var/lib/forkmesh/repositories/forkmesh-forkmesh.git",
		"+refs/heads/*:refs/heads/*",
		"+refs/tags/*:refs/tags/*",
	}
}

func vultrMirrorDNSRecordPayload(hostname, ip string) map[string]any {
	name := strings.ToLower(strings.TrimSpace(hostname))
	address := strings.TrimSpace(ip)
	if !dnsZonePattern.MatchString(name) || len(name) > 253 {
		return nil
	}
	octets := strings.Split(address, ".")
	if len(octets) != 4 || address == "0.0.0.0" {
		return nil
	}
	for _, octet := range octets {
		value, err := strconv.Atoi(octet)
		if err != nil || octet == "" || len(octet) > 3 || value < 0 || value > 255 {
			return nil
		}
	}
	return map[string]any{
		"type":    "A",
		"name":    name,
		"content": address,
		"proxied": false,
		"ttl":     1,
		"comment": "ForkMesh mirror node",
	}
}

func cloudflareZoneID(zones []map[string]any, zoneName string) string {
	zone := strings.ToLower(strings.TrimSpace(zoneName))
	for strings.HasSuffix(zone, ".") {
		zone = zone[:len(zone)-1]
	}
	if zone == "" {
		return ""
	}
	found := ""
	for _, candidate := range zones {
		if strings.ToLower(strings.TrimSpace(jsonString(candidate["name"]))) != zone {
			continue
		}
		id := strings.TrimSpace(jsonString(candidate["id"]))
		if !cloudflareIDPattern.MatchString(id) {
			return ""
		}
		if found != "" && found != id {
			return ""
		}
		found = id
	}
	return found
}

func cloudflareDNSRecordID(records []map[string]any, hostname, recordType string) string {
	name := strings.ToLower(strings.TrimSpace(hostname))
	kind := strings.ToUpper(strings.TrimSpace(recordType))
	if name == "" || kind == "" {
		return ""
	}
	found := ""
	for _, record := range records {
		if strings.ToLower(strings.TrimSpace(jsonString(record["name"]))) != name ||
			strings.ToUpper(strings.TrimSpace(jsonString(record["type"]))) != kind {
			continue
		}
		id := strings.TrimSpace(jsonString(record["id"]))
		if !cloudflareIDPattern.MatchString(id) {
			return ""
		}
		if found != "" && found != id {
			return ""
		}
		found = id
	}
	return found
}

func flagshipHealthMessage(node, nonce string, issuedAt int64, proof map[string]any) []byte {
	return []byte("forkmesh-https-health-repository-v1\n" + node + "\n" + nonce + "\n" + strconv.FormatInt(issuedAt, 10) +
		"\nforkmesh\nforkmesh\n1\n" + jsonString(proof["integrity"]) + "\n" + jsonString(proof["refsSha256"]) + "\n" +
		jsonString(proof["operationsSha256"]))
}

func verifiedFlagshipMirrorHealth(payload map[string]any, expectedNode, nonce string, issuedAt int64) bool {
	proof := jsonObject(payload["repositoryProof"])
	challenge := jsonObject(payload["challenge"])
	node := strings.ToLower(strings.TrimSpace(jsonString(payload["node"])))
	signed := flagshipHealthMessage(node, nonce, issuedAt, proof)
	digest := sha256.Sum256(signed)
	available, _ := proof["available"].(bool)
	challengeIssued := jsonDouble(challenge["issuedAt"])
	return node == strings.ToLower(strings.TrimSpace(expectedNode)) &&
		jsonString(proof["owner"]) == "forkmesh" &&
		jsonString(proof["repository"]) == "forkmesh" &&
		available &&
		jsonString(proof["integrity"]) == "ok" &&
		mirrorRepositoryProofServesContent(jsonString(proof["refsSha256"])) &&
		jsonString(challenge["messageType"]) == "forkmesh-https-health-repository-v1" &&
		jsonString(challenge["nonce"]) == nonce &&
		int64(challengeIssued) == issuedAt &&
		jsonString(challenge["messageSha256"]) == hex.EncodeToString(digest[:]) &&
		identity.Verify(jsonString(payload["publicKey"]), jsonString(challenge["signature"]), signed)
}

func conciseElapsedTime(elapsedMs int64) string {
	if elapsedMs < 0 {
		elapsedMs = 0
	}
	seconds := (elapsedMs + 999) / 1000
	if seconds < 60 {
		return fmt.Sprintf("%ds", seconds)
	}
	minutes := seconds / 60
	if minutes < 60 {
		return fmt.Sprintf("%dm %ds", minutes, seconds%60)
	}
	return fmt.Sprintf("%dh %dm %ds", minutes/60, minutes%60, seconds%60)
}

func percentEncode(text string) string {
	var out strings.Builder
	for i := 0; i < len(text); i++ {
		c := text[i]
		if (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~' {
			out.WriteByte(c)
			continue
		}
		fmt.Fprintf(&out, "%%%02X", c)
	}
	return out.String()
}

func newUUID() string {
	var raw [16]byte
	_, _ = rand.Read(raw[:])
	raw[6] = (raw[6] & 0x0f) | 0x40
	raw[8] = (raw[8] & 0x3f) | 0x80
	text := hex.EncodeToString(raw[:])
	return text[0:8] + "-" + text[8:12] + "-" + text[12:16] + "-" + text[16:20] + "-" + text[20:32]
}

func spaceFields(text string) []string {
	out := []string{}
	for _, part := range strings.Split(text, " ") {
		if part != "" {
			out = append(out, part)
		}
	}
	return out
}
