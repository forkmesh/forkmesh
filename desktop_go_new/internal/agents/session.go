package agents

import (
	"encoding/json"
	"errors"
	"strings"
	"unicode"
)

const (
	StatusQueued  = "queued"
	StatusRunning = "running"
	StatusWaiting = "waiting"
	StatusSuccess = "success"
	StatusFailed  = "failed"
	StatusStopped = "stopped"
	StatusCleared = "cleared"

	SchemaCurrent = 2
	SchemaMinimum = 1

	RuntimeCodexAppServer   = "codex-app-server"
	RuntimeClaudeStreamJSON = "claude-stream-json"
	RuntimeManagedRunner    = "forkmesh-managed-runner"
)

func IsTerminal(status string) bool {
	return status == StatusSuccess || status == StatusFailed || status == StatusStopped
}

func RuntimeForLegacyProvider(provider string) string {
	switch strings.ToLower(trimmed(provider)) {
	case "codex", "openai-codex":
		return RuntimeCodexAppServer
	case "claude-code":
		return RuntimeClaudeStreamJSON
	}
	return RuntimeManagedRunner
}

type Session struct {
	SchemaVersion          int
	ID                     int
	Priority               int
	Owner                  string
	Name                   string
	IssueNumber            int
	IssueTitle             string
	Prompt                 string
	PendingSteerMessage    string
	InFlightSteerMessage   string
	Provider               string
	RuntimeID              string
	RuntimeProtocolVersion int
	HandoffFromProvider    string
	Model                  string
	Mode                   string
	CreatePr               bool
	AssociationOnly        bool
	DirectCheckout         bool
	Yolo                   bool
	Vibe                   bool
	Genie                  bool
	Strength               string
	OrgTask                bool
	OrgTaskID              string
	OrgTaskAttached        bool
	StartedByBot           string
	FinishedByBot          string
	TeammateID             string
	TeammateIDs            []string
	TeammateName           string
	TeammateAvatar         string
	ChatConversation       string
	ChatThreadRootID       string
	OriginInstanceID       string
	OriginInstanceURL      string
	SkillsUsed             []string
	TaskGrade              int
	OrgAgentJob            json.RawMessage
	WebSessionID           string
	PrNumber               int
	Status                 string
	Held                   bool
	HeldBy                 string
	BranchName             string
	WorktreeName           string
	BaseRef                string
	BaseBranch             string
	MergeCandidateHead     string
	Merged                 bool
	MergeAttested          bool
	MergedAtMs             int64
	DeletedAtMs            int64
	CreatedAtMs            int64
	StartedAtMs            int64
	FinishedAtMs           int64
	PromptTokens           int
	CompletionTokens       int
	TotalTokens            int
	ContextTokens          int
	ContextWindow          int
	MaxOutputTokens        int
	EstimatedCredits       int
	CostUsd                float64
	SpendBeforeUsd         float64
	SpendAfterUsd          float64
	NumTurns               int
	DurationMs             int64
	LastError              string
	RunningMs              int64
	RunningSinceMs         int64

	Extra map[string]json.RawMessage
}

var errNotObject = errors.New("agents: session record is not a JSON object")

var knownKeys = map[string]bool{}

func init() {
	for _, key := range []string{
		"schemaVersion", "id", "priority", "owner", "name", "issueNumber", "issueTitle", "prompt",
		"pendingSteerMessage", "inFlightSteerMessage", "provider", "runtimeId", "runtimeProtocolVersion",
		"handoffFromProvider", "model", "mode", "createPr", "associationOnly", "directCheckout", "held",
		"heldBy", "yolo", "vibe", "genie", "strength", "orgTask", "orgTaskId", "orgTaskAttached",
		"startedByBot", "finishedByBot", "teammateId", "teammateIds", "teammateName", "teammateAvatar",
		"chatConversation", "chatThreadRootId", "originInstanceId", "originInstanceUrl", "skillsUsed",
		"taskGrade", "orgAgentJob", "webSessionId", "prNumber", "status", "branchName", "worktreeName",
		"baseRef", "baseBranch", "mergeCandidateHead", "merged", "mergeAttested", "mergedAtMs",
		"createdAtMs", "startedAtMs", "finishedAtMs", "promptTokens", "completionTokens", "totalTokens",
		"contextTokens", "contextWindow", "maxOutputTokens", "estimatedCredits", "costUsd",
		"spendBeforeUsd", "spendAfterUsd", "numTurns", "durationMs", "runningMs", "runningSinceMs",
		"lastError",
	} {
		knownKeys[key] = true
	}
}

func NewSession() Session {
	return Session{SchemaVersion: SchemaCurrent, Priority: 50, RuntimeProtocolVersion: 1,
		Status: StatusQueued, OrgAgentJob: json.RawMessage("{}")}
}

func (s Session) RepoKey() string { return sanitizeRepoKey(s.Owner, s.Name) }

func (s Session) EffectiveRuntimeID() string {
	if id := trimmed(s.RuntimeID); id != "" {
		return id
	}
	return RuntimeForLegacyProvider(s.Provider)
}

func (s Session) SchemaSupported() bool {
	return s.SchemaVersion >= SchemaMinimum && s.SchemaVersion <= SchemaCurrent
}

func (s Session) Active() bool {
	return s.Status == StatusQueued || s.Status == StatusRunning || s.Status == StatusWaiting
}

func (s Session) UpdatedAtMs() int64 {
	return max(max(s.CreatedAtMs, s.StartedAtMs), max(s.FinishedAtMs, s.MergedAtMs))
}

func sanitizeRepoKey(owner, name string) string {
	var b strings.Builder
	for _, r := range owner + "-" + name {
		switch {
		case r > 0xFFFF:
			b.WriteString("__")
		case unicode.IsLetter(r) || unicode.IsNumber(r) || r == '-' || r == '.':
			b.WriteRune(r)
		default:
			b.WriteByte('_')
		}
	}
	return b.String()
}

func cleanList(values []any, limit int, foldCase bool) []string {
	out := []string{}
	for _, entry := range values {
		value := left(trimmed(qtString(entry, "")), limit)
		if value != "" && !containsString(out, value, foldCase) {
			out = append(out, value)
		}
		if len(out) >= 64 {
			break
		}
	}
	return out
}

func containsString(list []string, value string, foldCase bool) bool {
	for _, existing := range list {
		if existing == value || (foldCase && strings.EqualFold(existing, value)) {
			return true
		}
	}
	return false
}

func anyStrings(values []string) []any {
	out := make([]any, len(values))
	for i, v := range values {
		out[i] = v
	}
	return out
}

func SessionFromObject(obj map[string]json.RawMessage) Session {
	value := func(key string) any { return decodeValue(obj[key]) }
	s := NewSession()
	stored := SchemaMinimum
	if raw, ok := obj["schemaVersion"]; ok {
		stored = qtInt(decodeValue(raw), 0)
	}
	if stored >= SchemaMinimum && stored <= SchemaCurrent {
		s.SchemaVersion = SchemaCurrent
	} else {
		s.SchemaVersion = stored
	}
	s.ID = qtInt(value("id"), 0)
	s.Priority = clamp(1, qtInt(value("priority"), 50), 99)
	s.Owner = qtString(value("owner"), "")
	s.Name = qtString(value("name"), "")
	s.IssueNumber = qtInt(value("issueNumber"), 0)
	s.IssueTitle = qtString(value("issueTitle"), "")
	s.Prompt = qtString(value("prompt"), "")
	s.PendingSteerMessage = qtString(value("pendingSteerMessage"), "")
	s.InFlightSteerMessage = qtString(value("inFlightSteerMessage"), "")
	s.Provider = qtString(value("provider"), "")
	s.RuntimeID = trimmed(qtString(value("runtimeId"), ""))
	if s.RuntimeID == "" {
		s.RuntimeID = RuntimeForLegacyProvider(s.Provider)
	}
	s.RuntimeProtocolVersion = max(1, qtInt(value("runtimeProtocolVersion"), 1))
	s.HandoffFromProvider = qtString(value("handoffFromProvider"), "")
	s.Model = qtString(value("model"), "")
	s.Mode = qtString(value("mode"), "")
	s.CreatePr = qtBool(value("createPr"))
	s.AssociationOnly = qtBool(value("associationOnly"))
	s.DirectCheckout = qtBool(value("directCheckout"))
	s.Held = qtBool(value("held"))
	if s.Held {
		s.HeldBy = left(qtString(value("heldBy"), ""), 200)
	}
	s.Yolo = qtBool(value("yolo"))
	s.Vibe = qtBool(value("vibe"))
	s.Genie = qtBool(value("genie"))
	s.Strength = qtString(value("strength"), "")
	s.OrgTask = qtBool(value("orgTask"))
	s.OrgTaskID = qtString(value("orgTaskId"), "")
	s.OrgTaskAttached = qtBool(value("orgTaskAttached"))
	s.StartedByBot = qtString(value("startedByBot"), "")
	s.FinishedByBot = qtString(value("finishedByBot"), "")
	s.TeammateID = qtString(value("teammateId"), "")
	s.TeammateIDs = cleanList(qtArray(value("teammateIds")), 80, false)
	s.TeammateName = qtString(value("teammateName"), "")
	s.TeammateAvatar = qtString(value("teammateAvatar"), "")
	s.ChatConversation = left(trimmed(qtString(value("chatConversation"), "")), 160)
	s.ChatThreadRootID = left(trimmed(qtString(value("chatThreadRootId"), "")), 96)
	s.OriginInstanceID = left(trimmed(qtString(value("originInstanceId"), "")), 96)
	s.OriginInstanceURL = left(trimmed(qtString(value("originInstanceUrl"), "")), 2048)
	s.SkillsUsed = cleanList(qtArray(value("skillsUsed")), 160, true)
	s.TaskGrade = clamp(0, qtInt(value("taskGrade"), 0), 5)
	job, isObject := value("orgAgentJob").(map[string]any)
	if isObject {
		s.OrgAgentJob = append(json.RawMessage(nil), obj["orgAgentJob"]...)
	}
	s.WebSessionID = trimmed(qtString(value("webSessionId"), ""))
	if s.WebSessionID == "" && isObject {
		s.WebSessionID = trimmed(qtString(job["sessionId"], ""))
	}
	s.PrNumber = qtInt(value("prNumber"), 0)
	s.Status = qtString(value("status"), StatusQueued)
	s.BranchName = qtString(value("branchName"), "")
	s.WorktreeName = qtString(value("worktreeName"), "")
	s.BaseRef = qtString(value("baseRef"), "")
	s.BaseBranch = qtString(value("baseBranch"), "")
	s.MergeCandidateHead = qtString(value("mergeCandidateHead"), "")
	s.Merged = qtBool(value("merged"))
	s.MergeAttested = qtBool(value("mergeAttested"))
	s.MergedAtMs = qtLongLong(value("mergedAtMs"))
	s.CreatedAtMs = qtLongLong(value("createdAtMs"))
	s.StartedAtMs = qtLongLong(value("startedAtMs"))
	s.FinishedAtMs = qtLongLong(value("finishedAtMs"))
	s.PromptTokens = qtInt(value("promptTokens"), 0)
	s.CompletionTokens = qtInt(value("completionTokens"), 0)
	s.TotalTokens = qtInt(value("totalTokens"), 0)
	s.ContextTokens = qtInt(value("contextTokens"), 0)
	s.ContextWindow = qtInt(value("contextWindow"), 0)
	s.MaxOutputTokens = qtInt(value("maxOutputTokens"), 0)
	s.EstimatedCredits = qtInt(value("estimatedCredits"), 0)
	s.CostUsd = qtDouble(value("costUsd"))
	s.SpendBeforeUsd = qtDouble(value("spendBeforeUsd"))
	s.SpendAfterUsd = qtDouble(value("spendAfterUsd"))
	s.NumTurns = qtInt(value("numTurns"), 0)
	s.DurationMs = qtLongLong(value("durationMs"))
	s.RunningMs = qtLongLong(value("runningMs"))
	s.RunningSinceMs = qtLongLong(value("runningSinceMs"))
	s.LastError = qtString(value("lastError"), "")
	for key, raw := range obj {
		if knownKeys[key] {
			continue
		}
		if s.Extra == nil {
			s.Extra = map[string]json.RawMessage{}
		}
		s.Extra[key] = append(json.RawMessage(nil), raw...)
	}
	return s
}

func (s *Session) UnmarshalJSON(data []byte) error {
	obj, ok := parseObject(data)
	if !ok {
		return errNotObject
	}
	*s = SessionFromObject(obj)
	return nil
}

func (s Session) Object() map[string]any {
	obj := map[string]any{}
	for key, raw := range s.Extra {
		obj[key] = raw
	}
	obj["schemaVersion"] = s.SchemaVersion
	obj["id"] = s.ID
	obj["priority"] = clamp(1, s.Priority, 99)
	obj["owner"] = s.Owner
	obj["name"] = s.Name
	obj["issueNumber"] = s.IssueNumber
	obj["issueTitle"] = s.IssueTitle
	obj["prompt"] = s.Prompt
	obj["pendingSteerMessage"] = s.PendingSteerMessage
	obj["inFlightSteerMessage"] = s.InFlightSteerMessage
	obj["provider"] = s.Provider
	obj["runtimeId"] = s.EffectiveRuntimeID()
	obj["runtimeProtocolVersion"] = s.RuntimeProtocolVersion
	obj["handoffFromProvider"] = s.HandoffFromProvider
	obj["model"] = s.Model
	obj["mode"] = s.Mode
	obj["createPr"] = s.CreatePr
	obj["associationOnly"] = s.AssociationOnly
	if s.DirectCheckout {
		obj["directCheckout"] = true
	}
	if s.Held {
		obj["held"] = true
		if s.HeldBy != "" {
			obj["heldBy"] = left(s.HeldBy, 200)
		}
	}
	obj["yolo"] = s.Yolo
	if s.Vibe {
		obj["vibe"] = true
	}
	obj["genie"] = s.Genie
	obj["strength"] = s.Strength
	obj["orgTask"] = s.OrgTask
	obj["orgTaskId"] = s.OrgTaskID
	if s.OrgTaskAttached {
		obj["orgTaskAttached"] = true
	}
	obj["startedByBot"] = s.StartedByBot
	obj["finishedByBot"] = s.FinishedByBot
	obj["teammateId"] = s.TeammateID
	obj["teammateIds"] = cleanList(anyStrings(s.TeammateIDs), 80, false)
	obj["teammateName"] = s.TeammateName
	obj["teammateAvatar"] = s.TeammateAvatar
	obj["chatConversation"] = left(trimmed(s.ChatConversation), 160)
	obj["chatThreadRootId"] = left(trimmed(s.ChatThreadRootID), 96)
	obj["originInstanceId"] = left(trimmed(s.OriginInstanceID), 96)
	obj["originInstanceUrl"] = left(trimmed(s.OriginInstanceURL), 2048)
	obj["skillsUsed"] = cleanList(anyStrings(s.SkillsUsed), 160, true)
	obj["taskGrade"] = clamp(0, s.TaskGrade, 5)
	if len(s.OrgAgentJob) == 0 {
		obj["orgAgentJob"] = json.RawMessage("{}")
	} else {
		obj["orgAgentJob"] = s.OrgAgentJob
	}
	obj["webSessionId"] = s.WebSessionID
	obj["prNumber"] = s.PrNumber
	obj["status"] = s.Status
	obj["branchName"] = s.BranchName
	obj["worktreeName"] = s.WorktreeName
	obj["baseRef"] = s.BaseRef
	obj["baseBranch"] = s.BaseBranch
	obj["mergeCandidateHead"] = s.MergeCandidateHead
	obj["merged"] = s.Merged
	obj["mergeAttested"] = s.MergeAttested
	obj["mergedAtMs"] = s.MergedAtMs
	obj["createdAtMs"] = s.CreatedAtMs
	obj["startedAtMs"] = s.StartedAtMs
	obj["finishedAtMs"] = s.FinishedAtMs
	obj["promptTokens"] = s.PromptTokens
	obj["completionTokens"] = s.CompletionTokens
	obj["totalTokens"] = s.TotalTokens
	obj["contextTokens"] = s.ContextTokens
	obj["contextWindow"] = s.ContextWindow
	obj["maxOutputTokens"] = s.MaxOutputTokens
	obj["estimatedCredits"] = s.EstimatedCredits
	obj["costUsd"] = s.CostUsd
	obj["spendBeforeUsd"] = s.SpendBeforeUsd
	obj["spendAfterUsd"] = s.SpendAfterUsd
	obj["numTurns"] = s.NumTurns
	obj["durationMs"] = s.DurationMs
	obj["runningMs"] = s.RunningMs
	obj["runningSinceMs"] = s.RunningSinceMs
	obj["lastError"] = s.LastError
	return obj
}

func (s Session) MarshalJSON() ([]byte, error) {
	return json.Marshal(s.Object())
}
