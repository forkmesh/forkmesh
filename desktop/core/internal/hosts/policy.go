package hosts

import (
	"crypto/rand"
	"encoding/hex"
	"fmt"
	"regexp"
	"strings"
	"unicode/utf16"
)

var hostMutatingActions = []string{"update", "restart", "stop", "start", "install", "destroy"}

func isHostMutatingAction(action string) bool {
	for _, candidate := range hostMutatingActions {
		if candidate == action {
			return true
		}
	}
	return false
}

func isServiceAction(action string) bool {
	return action == "restart" || action == "stop" || action == "start"
}

func hostActionSummary(action, host string) string {
	switch action {
	case "update":
		return fmt.Sprintf("Reinstall the latest ForkMesh on \"%s\" and restart its service. Mirrors it serves are briefly unavailable.", host)
	case "restart":
		return fmt.Sprintf("Restart the mirror service on \"%s\". A clone in progress is left alone.", host)
	case "stop":
		return fmt.Sprintf("Stop the mirror service on \"%s\". It stops serving until you start it again.", host)
	case "start":
		return fmt.Sprintf("Start the mirror service on \"%s\".", host)
	case "install":
		named := ""
		if host != "" {
			named = fmt.Sprintf(" named \"%s\"", host)
		}
		return fmt.Sprintf("Create a new Vultr server%s and install a ForkMesh mirror on it. Vultr bills your account for it every month until it is destroyed.", named)
	case "destroy":
		return fmt.Sprintf("Permanently DELETE the Vultr server behind \"%s\". Everything on it is gone, billing stops, it leaves the Hosts list and the git remotes that pushed to it are removed. This cannot be undone.", host)
	}
	return ""
}

var hostNamePattern = regexp.MustCompile(`^[A-Za-z0-9][A-Za-z0-9._-]{0,62}$`)

func validHostName(name string) bool { return hostNamePattern.MatchString(name) }

func protectedFleetName(name string) bool {
	return strings.HasPrefix(strings.ToLower(strings.TrimSpace(name)), "fmnext-")
}

func newStreamID() string {
	var raw [16]byte
	_, _ = rand.Read(raw[:])
	return hex.EncodeToString(raw[:])
}

func healthFromProbeLabel(label string) string {
	text := strings.TrimSpace(label)
	switch {
	case text == "":
		return "unknown"
	case strings.Contains(text, "Provisioning"):
		return "provisioning"
	case strings.Contains(text, "Offline"):
		return "offline"
	case strings.Contains(text, "Attention") || strings.Contains(text, "missing"):
		return "attention"
	case strings.Contains(text, "Online"):
		return "online"
	case strings.Contains(strings.ToLower(text), "checking"):
		return "unknown"
	}
	return "attention"
}

func hostRow(saved map[string]any, probe, version string, rosterOnline bool) map[string]any {
	name := strings.TrimSpace(jsonString(saved["name"]))
	provider := jsonString(saved["provider"])
	vultr := strings.EqualFold(provider, "Vultr")
	status := jsonString(saved["status"])
	health := healthFromProbeLabel(probe)
	if health == "unknown" && rosterOnline {
		health = "online"
	}
	if status == "installing" {
		health = "provisioning"
	}
	actions := []string{"update"}
	if vultr || jsonString(saved["user"]) == "root" {
		actions = append(actions, "restart", "stop", "start")
	}
	if vultr {
		actions = append(actions, "destroy")
	}
	return map[string]any{
		"name":            name,
		"address":         jsonString(saved["ip"]),
		"user":            jsonString(saved["user"]),
		"status":          status,
		"statusChangedAt": jsonDouble(saved["statusChangedAt"]),
		"provider":        provider,
		"region":          jsonString(saved["region"]),
		"plan":            jsonString(saved["planType"]),
		"monthlyCost":     jsonDouble(saved["monthlyCost"]),
		"ramMb":           jsonDouble(saved["ramMb"]),
		"diskGb":          jsonDouble(saved["diskGb"]),
		"dnsHostname":     jsonString(saved["dnsHostname"]),
		"health":          health,
		"healthLabel":     strings.TrimSpace(probe),
		"version":         version,
		"online":          rosterOnline,
		"actions":         actions,
	}
}

func rowHasAction(row map[string]any, action string) bool {
	actions, _ := row["actions"].([]string)
	for _, candidate := range actions {
		if candidate == action {
			return true
		}
	}
	return false
}

func serviceCommand(action string) string {
	switch action {
	case "restart":
		return `sh -lc 'if ls -d /var/lib/forkmesh/repositories/.clone-* >/dev/null 2>&1; then echo "restart: skipped, a clone is still running"; elif systemctl restart forkmesh-mirror-node; then echo "restart: done"; else echo "restart: refused"; exit 1; fi'`
	case "stop":
		return `sh -lc 'systemctl stop forkmesh-mirror-node && echo "stop: done"'`
	case "start":
		return `sh -lc 'systemctl start forkmesh-mirror-node && echo "start: done"'`
	}
	return ""
}

func jsonString(value any) string {
	text, _ := value.(string)
	return text
}

func jsonDouble(value any) float64 {
	switch number := value.(type) {
	case float64:
		return number
	case int:
		return float64(number)
	case int64:
		return float64(number)
	}
	return 0
}

func utf16Len(text string) int { return len(utf16.Encode([]rune(text))) }

func leftUTF16(text string, limit int) string {
	if limit <= 0 {
		return ""
	}
	if len(text) <= limit {
		return text
	}
	units := 0
	for index, r := range text {
		width := 1
		if r >= 0x10000 {
			width = 2
		}
		if units+width > limit {
			return text[:index]
		}
		units += width
	}
	return text
}
