package agents

import (
	"encoding/json"
	"math"
	"strconv"
	"strings"

	"forkmesh.com/desktop-core/internal/qtjson"
)

func qtValueLongLong(v qtjson.Value) int64 {
	switch v.Kind() {
	case qtjson.Bool:
		if v.BoolValue() {
			return 1
		}
		return 0
	case qtjson.Integer:
		return v.Int64()
	case qtjson.Double:
		d := v.Float()
		if math.IsNaN(d) || math.IsInf(d, 0) || d >= math.MaxInt64 || d <= math.MinInt64 {
			return 0
		}
		return int64(math.Round(d))
	case qtjson.String:
		i, err := strconv.ParseInt(strings.TrimSpace(v.Str()), 10, 64)
		if err != nil {
			return 0
		}
		return i
	}
	return 0
}

func qtCleanList(values []qtjson.Value, limit int, foldCase bool) []string {
	out := []string{}
	for _, entry := range values {
		value := left(trimmed(entry.Str()), limit)
		if value != "" && !containsString(out, value, foldCase) {
			out = append(out, value)
		}
		if len(out) >= 64 {
			break
		}
	}
	return out
}

func SessionFromQt(obj qtjson.Value) Session {
	s := NewSession()
	stored := SchemaMinimum
	if obj.Has("schemaVersion") {
		stored = obj.Get("schemaVersion").IntOr(0)
	}
	if stored >= SchemaMinimum && stored <= SchemaCurrent {
		s.SchemaVersion = SchemaCurrent
	} else {
		s.SchemaVersion = stored
	}
	str := func(key string) string { return obj.Get(key).Str() }
	s.ID = obj.Get("id").IntOr(0)
	s.Priority = clamp(1, obj.Get("priority").IntOr(50), 99)
	s.Owner = str("owner")
	s.Name = str("name")
	s.IssueNumber = obj.Get("issueNumber").IntOr(0)
	s.IssueTitle = str("issueTitle")
	s.Prompt = str("prompt")
	s.PendingSteerMessage = str("pendingSteerMessage")
	s.InFlightSteerMessage = str("inFlightSteerMessage")
	s.Provider = str("provider")
	s.RuntimeID = trimmed(str("runtimeId"))
	if s.RuntimeID == "" {
		s.RuntimeID = RuntimeForLegacyProvider(s.Provider)
	}
	s.RuntimeProtocolVersion = max(1, obj.Get("runtimeProtocolVersion").IntOr(1))
	s.HandoffFromProvider = str("handoffFromProvider")
	s.Model = str("model")
	s.Mode = str("mode")
	s.CreatePr = obj.Get("createPr").BoolValue()
	s.AssociationOnly = obj.Get("associationOnly").BoolValue()
	s.DirectCheckout = obj.Get("directCheckout").BoolValue()
	s.Held = obj.Get("held").BoolValue()
	if s.Held {
		s.HeldBy = left(str("heldBy"), 200)
	}
	s.Yolo = obj.Get("yolo").BoolValue()
	s.Vibe = obj.Get("vibe").BoolValue()
	s.Genie = obj.Get("genie").BoolValue()
	s.Strength = str("strength")
	s.OrgTask = obj.Get("orgTask").BoolValue()
	s.OrgTaskID = str("orgTaskId")
	s.OrgTaskAttached = obj.Get("orgTaskAttached").BoolValue()
	s.StartedByBot = str("startedByBot")
	s.FinishedByBot = str("finishedByBot")
	s.TeammateID = str("teammateId")
	s.TeammateIDs = qtCleanList(obj.Get("teammateIds").Items(), 80, false)
	s.TeammateName = str("teammateName")
	s.TeammateAvatar = str("teammateAvatar")
	s.ChatConversation = left(trimmed(str("chatConversation")), 160)
	s.ChatThreadRootID = left(trimmed(str("chatThreadRootId")), 96)
	s.OriginInstanceID = left(trimmed(str("originInstanceId")), 96)
	s.OriginInstanceURL = left(trimmed(str("originInstanceUrl")), 2048)
	s.SkillsUsed = qtCleanList(obj.Get("skillsUsed").Items(), 160, true)
	s.TaskGrade = clamp(0, obj.Get("taskGrade").IntOr(0), 5)
	job := obj.Get("orgAgentJob")
	if job.IsObject() {
		s.OrgAgentJob = json.RawMessage(job.Compact())
	}
	s.WebSessionID = trimmed(str("webSessionId"))
	if s.WebSessionID == "" && job.IsObject() {
		s.WebSessionID = trimmed(job.Get("sessionId").Str())
	}
	s.PrNumber = obj.Get("prNumber").IntOr(0)
	s.Status = obj.Get("status").StrOr(StatusQueued)
	s.BranchName = str("branchName")
	s.WorktreeName = str("worktreeName")
	s.BaseRef = str("baseRef")
	s.BaseBranch = str("baseBranch")
	s.MergeCandidateHead = str("mergeCandidateHead")
	s.Merged = obj.Get("merged").BoolValue()
	s.MergeAttested = obj.Get("mergeAttested").BoolValue()
	s.MergedAtMs = qtValueLongLong(obj.Get("mergedAtMs"))
	s.CreatedAtMs = qtValueLongLong(obj.Get("createdAtMs"))
	s.StartedAtMs = qtValueLongLong(obj.Get("startedAtMs"))
	s.FinishedAtMs = qtValueLongLong(obj.Get("finishedAtMs"))
	s.PromptTokens = obj.Get("promptTokens").IntOr(0)
	s.CompletionTokens = obj.Get("completionTokens").IntOr(0)
	s.TotalTokens = obj.Get("totalTokens").IntOr(0)
	s.ContextTokens = obj.Get("contextTokens").IntOr(0)
	s.ContextWindow = obj.Get("contextWindow").IntOr(0)
	s.MaxOutputTokens = obj.Get("maxOutputTokens").IntOr(0)
	s.EstimatedCredits = obj.Get("estimatedCredits").IntOr(0)
	s.CostUsd = obj.Get("costUsd").Float()
	s.SpendBeforeUsd = obj.Get("spendBeforeUsd").Float()
	s.SpendAfterUsd = obj.Get("spendAfterUsd").Float()
	s.NumTurns = obj.Get("numTurns").IntOr(0)
	s.DurationMs = qtValueLongLong(obj.Get("durationMs"))
	s.RunningMs = qtValueLongLong(obj.Get("runningMs"))
	s.RunningSinceMs = qtValueLongLong(obj.Get("runningSinceMs"))
	s.LastError = str("lastError")
	return s
}

func (s Session) ToQt() qtjson.Value {
	obj := qtjson.NewObject()
	set := func(key string, value qtjson.Value) { obj.Set(key, value) }
	str := func(key, value string) { obj.Set(key, qtjson.NewString(value)) }
	num := func(key string, value int64) { obj.Set(key, qtjson.NewInt(value)) }
	flag := func(key string, value bool) { obj.Set(key, qtjson.NewBool(value)) }
	num("schemaVersion", int64(s.SchemaVersion))
	num("id", int64(s.ID))
	num("priority", int64(clamp(1, s.Priority, 99)))
	str("owner", s.Owner)
	str("name", s.Name)
	num("issueNumber", int64(s.IssueNumber))
	str("issueTitle", s.IssueTitle)
	str("prompt", s.Prompt)
	str("pendingSteerMessage", s.PendingSteerMessage)
	str("inFlightSteerMessage", s.InFlightSteerMessage)
	str("provider", s.Provider)
	str("runtimeId", s.EffectiveRuntimeID())
	num("runtimeProtocolVersion", int64(s.RuntimeProtocolVersion))
	str("handoffFromProvider", s.HandoffFromProvider)
	str("model", s.Model)
	str("mode", s.Mode)
	flag("createPr", s.CreatePr)
	flag("associationOnly", s.AssociationOnly)
	if s.DirectCheckout {
		flag("directCheckout", true)
	}
	if s.Held {
		flag("held", true)
		if s.HeldBy != "" {
			str("heldBy", left(s.HeldBy, 200))
		}
	}
	flag("yolo", s.Yolo)
	if s.Vibe {
		flag("vibe", true)
	}
	flag("genie", s.Genie)
	str("strength", s.Strength)
	flag("orgTask", s.OrgTask)
	str("orgTaskId", s.OrgTaskID)
	if s.OrgTaskAttached {
		flag("orgTaskAttached", true)
	}
	str("startedByBot", s.StartedByBot)
	str("finishedByBot", s.FinishedByBot)
	str("teammateId", s.TeammateID)
	set("teammateIds", qtjson.NewStrings(cleanList(anyStrings(s.TeammateIDs), 80, false)))
	str("teammateName", s.TeammateName)
	str("teammateAvatar", s.TeammateAvatar)
	str("chatConversation", left(trimmed(s.ChatConversation), 160))
	str("chatThreadRootId", left(trimmed(s.ChatThreadRootID), 96))
	str("originInstanceId", left(trimmed(s.OriginInstanceID), 96))
	str("originInstanceUrl", left(trimmed(s.OriginInstanceURL), 2048))
	set("skillsUsed", qtjson.NewStrings(cleanList(anyStrings(s.SkillsUsed), 160, true)))
	num("taskGrade", int64(clamp(0, s.TaskGrade, 5)))
	job := qtjson.NewObject()
	if len(s.OrgAgentJob) > 0 {
		if parsed, err := qtjson.ParseKeepNumbers(s.OrgAgentJob); err == nil && parsed.IsObject() {
			job = parsed
		}
	}
	set("orgAgentJob", job)
	str("webSessionId", s.WebSessionID)
	num("prNumber", int64(s.PrNumber))
	str("status", s.Status)
	str("branchName", s.BranchName)
	str("worktreeName", s.WorktreeName)
	str("baseRef", s.BaseRef)
	str("baseBranch", s.BaseBranch)
	str("mergeCandidateHead", s.MergeCandidateHead)
	flag("merged", s.Merged)
	flag("mergeAttested", s.MergeAttested)
	num("mergedAtMs", s.MergedAtMs)
	num("createdAtMs", s.CreatedAtMs)
	num("startedAtMs", s.StartedAtMs)
	num("finishedAtMs", s.FinishedAtMs)
	num("promptTokens", int64(s.PromptTokens))
	num("completionTokens", int64(s.CompletionTokens))
	num("totalTokens", int64(s.TotalTokens))
	num("contextTokens", int64(s.ContextTokens))
	num("contextWindow", int64(s.ContextWindow))
	num("maxOutputTokens", int64(s.MaxOutputTokens))
	num("estimatedCredits", int64(s.EstimatedCredits))
	set("costUsd", qtjson.NewDouble(s.CostUsd))
	set("spendBeforeUsd", qtjson.NewDouble(s.SpendBeforeUsd))
	set("spendAfterUsd", qtjson.NewDouble(s.SpendAfterUsd))
	num("numTurns", int64(s.NumTurns))
	num("durationMs", s.DurationMs)
	num("runningMs", s.RunningMs)
	num("runningSinceMs", s.RunningSinceMs)
	str("lastError", s.LastError)
	return obj
}
