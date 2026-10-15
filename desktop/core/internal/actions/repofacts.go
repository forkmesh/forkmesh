package actions

import (
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"unicode"

	"forkmesh.com/desktop-core/internal/qsettings"
)

const (
	repositoriesArray          = "repositories/items"
	mirrorRootSetting          = "repositories/mirrorRoot"
	machineNodeNameSetting     = "node/machineName"
	actionNodeLabelsSetting    = "actions/nodeLabels"
	mirrorNodeSetting          = "actions/mirrorNode"
	mirrorEnabledSetting       = "actions/mirrorEnabled"
	mirrorGenerationSetting    = "actions/mirrorConfigGeneration"
	mirrorSummaryPathSetting   = "actions/mirrorSummaryPath"
	mirrorStatePathSetting     = "actions/mirrorStatePath"
	autoDeleteCancelledSetting = "actions/autoDeleteCancelledRuns"
	accountNodeNameSetting     = "account/nodeName"
)

var opaqueIDPattern = regexp.MustCompile(`^[0-9a-f]{64}$`)

type RepoEntry struct {
	Owner             string   `json:"owner"`
	Name              string   `json:"name"`
	Description       string   `json:"description"`
	MirrorPath        string   `json:"mirrorPath"`
	LocalPath         string   `json:"localPath"`
	WritablePath      string   `json:"writablePath"`
	Preview           bool     `json:"preview"`
	Relay             bool     `json:"relay"`
	Enabled           bool     `json:"enabled"`
	AutoApprove       bool     `json:"autoApprove"`
	ExternallyManaged bool     `json:"externallyManaged"`
	ExternalSource    string   `json:"externalSource"`
	ExternalRef       string   `json:"externalRef"`
	DisabledWorkflows []string `json:"disabledWorkflows"`
	WorkflowNodes     []string `json:"workflowNodes"`
	Pinned            bool     `json:"pinned"`
	Private           bool     `json:"private"`
	Published         bool     `json:"published"`
	LastSyncMs        int64    `json:"lastSyncMs"`
}

type RepoSnapshot struct {
	Repos            []RepoEntry `json:"repos"`
	NodeLabels       []string    `json:"nodeLabels"`
	Machine          string      `json:"machine"`
	Slice            string      `json:"slice"`
	MirrorGeneration string      `json:"mirrorGeneration"`
	MirrorSuspended  bool        `json:"mirrorSuspended"`
	Native           []int       `json:"native"`
}

func (s RepoSnapshot) lookup(owner, name string) (RepoEntry, bool) {
	for _, repo := range s.Repos {
		if !repo.Preview && repo.Relay && repo.Owner == owner && repo.Name == name {
			return repo, true
		}
	}
	return RepoEntry{}, false
}

func (r RepoEntry) facts(nodeLabels []string) RepoFacts {
	return RepoFacts{
		Repo:              r.Owner + "/" + r.Name,
		Owner:             r.Owner,
		Name:              r.Name,
		MirrorPath:        r.MirrorPath,
		LocalPath:         r.LocalPath,
		Enabled:           r.Enabled,
		AutoApprove:       r.AutoApprove,
		DisabledWorkflows: nonNilStrings(r.DisabledWorkflows),
		WorkflowNodes:     nonNilStrings(r.WorkflowNodes),
		NodeLabels:        nonNilStrings(nodeLabels),
	}
}

func nonNilStrings(values []string) []string {
	if values == nil {
		return []string{}
	}
	return values
}

func repoSegment(value, fallback string) string {
	var out []rune
	lastWasDash := false
	for _, r := range strings.ToLower(qtTrim(value)) {
		if unicode.IsLetter(r) || unicode.IsNumber(r) || r == '_' || r == '-' {
			out = append(out, r)
			lastWasDash = false
		} else if !lastWasDash {
			out = append(out, '-')
			lastWasDash = true
		}
	}
	text := strings.Trim(string(out), "-")
	if text == "" {
		text = fallback
	}
	return qtLeft(text, 48)
}

func mirrorRoot(settings *qsettings.Settings, dataRoot string) string {
	if configured := qtTrim(settings.String(mirrorRootSetting, "")); configured != "" {
		return configured
	}
	return dataRoot + "/mirrors"
}

func accountNameFromInput(value, fallback string) string {
	var b strings.Builder
	for _, r := range strings.ToLower(strings.TrimSpace(value)) {
		if r >= 128 {
			continue
		}
		if (r >= 'a' && r <= 'z') || (r >= '0' && r <= '9') || r == '-' {
			b.WriteRune(r)
		}
	}
	out := b.String()
	for out != "" && !(out[0] >= 'a' && out[0] <= 'z') {
		out = out[1:]
	}
	if len(out) > 63 {
		out = out[:63]
	}
	out = strings.TrimRight(out, "-")
	if out == "" {
		return fallback
	}
	return out
}

func settingsAccountOwner(settings *qsettings.Settings) string {
	if stored := settings.String(accountNodeNameSetting, ""); stored != "" {
		return stored
	}
	for _, key := range []string{"profile/handle", "profile/displayName"} {
		if name := strings.TrimSpace(settings.String(key, "")); name != "" {
			return accountNameFromInput(strings.ToLower(name), "owner")
		}
	}
	return "owner"
}

func settingsMachineName(settings *qsettings.Settings) string {
	if saved := accountNameFromInput(settings.String(machineNodeNameSetting, ""), ""); saved != "" {
		return saved
	}
	return settingsAccountOwner(settings)
}

func hasGitCheckout(dir string) bool {
	if qtTrim(dir) == "" {
		return false
	}
	_, err := os.Stat(filepath.Join(dir, ".git"))
	return err == nil
}

func SettingsSnapshot(settings *qsettings.Settings, dataRoot string) RepoSnapshot {
	count := int(settings.Int(repositoriesArray+"/size", 0))
	account := settingsAccountOwner(settings)
	root := mirrorRoot(settings, dataRoot)
	var repos []RepoEntry
	for i := 1; i <= count; i++ {
		prefix := repositoriesArray + "/" + strconv.Itoa(i) + "/"
		get := func(key string) string { return settings.String(prefix+key, "") }
		repo := RepoEntry{
			Owner:             get("owner"),
			Name:              get("name"),
			Description:       get("description"),
			LocalPath:         get("localPath"),
			MirrorPath:        get("mirrorPath"),
			Relay:             true,
			AutoApprove:       settings.Bool(prefix+"actionsAutoApprove", true),
			ExternallyManaged: settings.Bool(prefix+"externallyManagedActions", false),
			ExternalSource:    qtTrim(get("externalActionsSource")),
			ExternalRef:       qtTrim(get("externalActionsRef")),
			DisabledWorkflows: settings.Strings(prefix + "disabledWorkflows"),
			WorkflowNodes:     settings.Strings(prefix + "workflowNodes"),
			Private:           settings.Bool(prefix+"isPrivate", false),
			Published:         settings.Bool(prefix+"publishToNetwork", false),
			LastSyncMs:        settings.Int(prefix+"lastSyncMs", 0),
		}
		repo.Enabled = settings.Bool(prefix+"actionsEnabled", repo.Owner == account)
		replica := qtTrim(get("privateReplicaId"))
		archive := qtTrim(get("publicArchiveId"))
		if !opaqueIDPattern.MatchString(replica) {
			replica = ""
		}
		if !opaqueIDPattern.MatchString(archive) {
			archive = ""
		}
		if !repo.Private && qtTrim(repo.MirrorPath) == "" && qtTrim(repo.Owner) != "" && qtTrim(repo.Name) != "" {
			repo.MirrorPath = root + "/" + repoSegment(repo.Owner, "owner") + "-" + repoSegment(repo.Name, "repository") + ".git"
		}
		repo.Pinned = replica != "" || archive != ""
		source := qtTrim(repo.LocalPath)
		if source == "" {
			source = qtTrim(get("cloneUrl"))
		}
		if repo.Name == "" || (source == "" && replica == "" && archive == "") {
			continue
		}
		repos = append(repos, repo)
	}
	for index := range repos {
		repos[index].WritablePath = writablePathFor(repos, index)
	}
	labels := NodeLabels(settingsMachineName(settings), settings.String(mirrorNodeSetting, ""), settings.String(actionNodeLabelsSetting, ""))
	return RepoSnapshot{Repos: repos, NodeLabels: labels, Machine: settingsMachineName(settings)}
}

func writablePathFor(repos []RepoEntry, index int) string {
	repo := repos[index]
	if hasGitCheckout(repo.LocalPath) {
		return repo.LocalPath
	}
	for i, other := range repos {
		if i == index || other.Preview {
			continue
		}
		if other.Owner == repo.Owner && other.Name == repo.Name && hasGitCheckout(other.LocalPath) {
			return other.LocalPath
		}
	}
	return repo.LocalPath
}
