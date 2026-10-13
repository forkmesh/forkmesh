package hosts

import (
	"net/netip"
	"regexp"
	"strconv"
	"strings"
)

var fleetMutatingActions = []string{"add", "forget", "deployAll", "uninstall", "agentClis", "actions", "linkCode", "resume",
	"fleetTarget", "reconcile", "mirrorPush", "deleteNode"}

const maxDesiredMirrors = 12

var (
	fleetNodePattern = regexp.MustCompile(`^[A-Za-z0-9][A-Za-z0-9._-]{0,62}$`)
	fleetDNSPattern  = regexp.MustCompile(`^([A-Za-z0-9]([A-Za-z0-9-]{0,61}[A-Za-z0-9])?)(\.[A-Za-z0-9]([A-Za-z0-9-]{0,61}[A-Za-z0-9])?)*$`)
	fleetUserPattern = regexp.MustCompile(`^[a-z_][a-z0-9_-]{0,31}$`)
	fleetRepoPattern = regexp.MustCompile(`^[A-Za-z0-9][A-Za-z0-9._-]{0,63}/[A-Za-z0-9._][A-Za-z0-9._-]{0,99}$`)
)

func isFleetMutatingAction(action string) bool {
	for _, candidate := range fleetMutatingActions {
		if candidate == action {
			return true
		}
	}
	return false
}

func validFleetAddress(address string) bool {
	text := strings.TrimSpace(address)
	if text == "" || len(text) > 253 {
		return false
	}
	if _, err := netip.ParseAddr(text); err == nil {
		return true
	}
	return fleetDNSPattern.MatchString(text)
}

func protectedMeshNode(node string) bool {
	key := strings.ToLower(strings.TrimSpace(node))
	return key == "" || key == "forkmesh" || key == "forkmesh-mainnode"
}

func fleetSubject(action string, params map[string]any) (string, string) {
	host := strings.TrimSpace(jsonString(params["host"]))
	if !isFleetMutatingAction(action) {
		return "", "unsupported_action"
	}
	switch action {
	case "add":
		address := strings.TrimSpace(jsonString(params["address"]))
		user := strings.TrimSpace(jsonString(params["user"]))
		if !fleetNodePattern.MatchString(host) {
			return "", "bad_host"
		}
		if !validFleetAddress(address) {
			return "", "bad_address"
		}
		if !fleetUserPattern.MatchString(user) {
			return "", "bad_user"
		}
		return strings.ToLower(host) + "|" + strings.ToLower(address) + "|" + user, ""
	case "forget", "uninstall", "agentClis", "deleteNode":
		if !fleetNodePattern.MatchString(host) {
			return "", "bad_host"
		}
		if action == "deleteNode" && protectedMeshNode(host) {
			return "", "protected_node"
		}
		return strings.ToLower(host), ""
	case "actions":
		if !fleetNodePattern.MatchString(host) {
			return "", "bad_host"
		}
		enabled, ok := params["enabled"].(bool)
		if !ok {
			return "", "bad_params"
		}
		if enabled {
			return strings.ToLower(host) + "|on", ""
		}
		return strings.ToLower(host) + "|off", ""
	case "deployAll":
		mode := jsonString(params["mode"])
		if mode != "install" && mode != "reinstall" && mode != "source" {
			return "", "bad_mode"
		}
		return mode, ""
	case "linkCode":
		code := strings.TrimSpace(jsonString(params["code"]))
		if !sixDigits.MatchString(code) {
			return "", "bad_code"
		}
		return code, ""
	case "fleetTarget":
		enabled, ok := params["enabled"].(bool)
		desired := -1
		if number, isNumber := params["desired"].(float64); isNumber && number == float64(int(number)) {
			desired = int(number)
		}
		if !ok || desired < 1 || desired > maxDesiredMirrors {
			return "", "bad_params"
		}
		if enabled {
			return "on:" + strconv.Itoa(desired), ""
		}
		return "off:" + strconv.Itoa(desired), ""
	case "mirrorPush":
		repo := strings.TrimSpace(jsonString(params["repo"]))
		if !fleetRepoPattern.MatchString(repo) || strings.Contains(repo, "..") {
			return "", "bad_repo"
		}
		return strings.ToLower(repo), ""
	}
	return action, ""
}

func subjectHost(subject string) string {
	if index := strings.IndexByte(subject, '|'); index >= 0 {
		return subject[:index]
	}
	return subject
}

func subjectField(subject string, field int) string {
	parts := strings.Split(subject, "|")
	if field < len(parts) {
		return parts[field]
	}
	return ""
}

func fleetTypedConfirmation(action, subject string) string {
	switch {
	case action == "uninstall":
		return subject
	case action == "deleteNode":
		return "DELETE " + subject
	case action == "deployAll" && subject == "reinstall":
		return "REINSTALL ALL"
	}
	return ""
}

func fleetTypedConfirmationMatches(typed, required string) bool {
	return required == "" || strings.TrimSpace(typed) == required
}

func fleetActionSummary(action, subject string) string {
	host := subjectHost(subject)
	switch action {
	case "add":
		return "Save \"" + host + "\" (" + subjectField(subject, 2) + "@" + subjectField(subject, 1) + ") as a host on the desktop. It is reached with the shared ForkMesh SSH key; if it asks for a password, the desktop prompts for it there."
	case "forget":
		return "Remove \"" + host + "\" from the desktop's saved hosts. The server itself keeps running and is not destroyed."
	case "deployAll":
		switch subject {
		case "reinstall":
			return "REMOVE ForkMesh and ALL of its data (identity key, mirrors, chat) from every saved host, then install it fresh. This cannot be undone."
		case "source":
			return "On every saved host: pull the latest ForkMesh source, rebuild it and restart the node, all in parallel."
		}
		return "Install the latest published ForkMesh on every active saved host, in parallel."
	case "uninstall":
		return "Uninstall ForkMesh from \"" + host + "\": its service, identity key and repositories are removed. The server keeps running."
	case "agentClis":
		return "Install the Claude Code and Codex command-line tools on \"" + host + "\". No sign-in is copied; sign in on the host afterwards."
	case "actions":
		if strings.HasSuffix(subject, "|on") {
			return "Turn Actions on for \"" + host + "\": the mirror starts running workflow jobs. Its variables are left as they are."
		}
		return "Turn Actions off for \"" + host + "\": the mirror stops taking workflow jobs."
	case "linkCode":
		return "Link the node that shows code " + subject + " to your account."
	case "resume":
		return "Resume the paused Vultr deployment where it stopped. No second server is created."
	case "fleetTarget":
		count := ""
		if index := strings.IndexByte(subject, ':'); index >= 0 {
			count = subject[index+1:]
		}
		if strings.HasPrefix(subject, "on:") {
			return "Keep " + count + " healthy mirror(s) running. The desktop creates or destroys Vultr servers to match, which changes your Vultr bill."
		}
		return "Stop managing the mirror fleet size. Existing servers are left alone."
	case "reconcile":
		return "Check the fleet's health now and create or destroy servers to meet the target."
	case "mirrorPush":
		return "Push " + subject + " to every SSH mirror remote and managed host. Branches are never rewound or deleted."
	case "deleteNode":
		return "Permanently DELETE the node \"" + host + "\" everywhere: its Vultr server is destroyed, its DNS record removed, its account, mirrors and history erased from the relay, and the saved host forgotten. This cannot be undone."
	}
	return ""
}
