package agentrun

import (
	"sort"
	"strconv"

	"forkmesh.com/desktop-core/internal/qtjson"
)

func setKeys(set map[string]bool) qtjson.Value {
	keys := make([]string, 0, len(set))
	for key := range set {
		keys = append(keys, key)
	}
	sort.Strings(keys)
	return qtjson.NewStrings(keys)
}

func readSet(value qtjson.Value) map[string]bool {
	out := map[string]bool{}
	for _, item := range value.Items() {
		out[item.Str()] = true
	}
	return out
}

func (c *codexState) snapshot() qtjson.Value {
	calls := qtjson.NewObject()
	for id, method := range c.pendingCalls {
		calls.Set(strconv.FormatInt(id, 10), qtjson.NewString(method))
	}
	steer := qtjson.NewObject()
	for id, text := range c.pendingSteer {
		steer.Set(strconv.FormatInt(id, 10), qtjson.NewString(text))
	}
	server := qtjson.NewObject()
	for token, pending := range c.pendingServer {
		byText := qtjson.NewObject()
		for text, id := range pending.QuestionsByText {
			byText.Set(text, qtjson.NewString(id))
		}
		choices := qtjson.NewObject()
		for label, decision := range pending.Choices {
			choices.Set(label, decision)
		}
		server.Set(token, qtjson.ObjectOf("rpcId", pending.RPCID, "method", pending.Method, "params", pending.Params,
			"questionIds", qtjson.NewStrings(pending.QuestionIDs), "questionsByText", byText, "choices", choices))
	}
	permissions := qtjson.NewObject()
	for token, event := range c.pendingPermissions {
		permissions.Set(token, event)
	}
	synthetic := qtjson.NewObject()
	for id, item := range c.syntheticTools {
		synthetic.Set(id, item)
	}
	return qtjson.ObjectOf(
		"nextRequestId", c.nextRequestID, "nextQuestionToken", c.nextQuestionToken, "pendingCalls", calls,
		"pendingSteer", steer, "pendingServer", server, "pendingPermissions", permissions,
		"toolUses", setKeys(c.toolUses), "toolResults", setKeys(c.toolResults), "reasoningDeltas", setKeys(c.reasoningDeltas),
		"noticeItems", setKeys(c.noticeItems), "resultTurns", setKeys(c.resultTurns), "syntheticTools", synthetic,
		"cwd", c.cwd, "initialPrompt", c.initialPrompt, "resumeThreadId", c.resumeThreadID,
		"resumeFallbackPrompt", c.resumeFallbackPrompt, "model", c.model, "effectiveModel", c.effectiveModel,
		"mode", c.mode, "externalCallGate", c.externalCallGate, "effort", c.effort, "hostSessionId", c.hostSessionID,
		"threadId", c.threadID, "turnId", c.turnID, "activeTurnMode", c.activeTurnMode, "lastAgentText", c.lastAgentText,
		"queuedUserTexts", qtjson.NewStrings(c.queuedUserTexts), "lastTokenUsage", c.lastTokenUsage,
		"rateLimits", c.rateLimits, "rateLimitsByLimitId", c.rateLimitsByLimitID, "threadReady", c.threadReady,
		"initialTurnSent", c.initialTurnSent, "turnActive", c.turnActive, "systemInitEmitted", c.systemInitEmitted,
		"nextNormalizedSequence", c.nextNormalizedSequence)
}

func (c *codexState) restore(s qtjson.Value) {
	c.nextRequestID = max(1, s.Get("nextRequestId").Int64())
	c.nextQuestionToken = max(1, s.Get("nextQuestionToken").Int64())
	for _, member := range s.Get("pendingCalls").Members() {
		if id, err := strconv.ParseInt(member.Key, 10, 64); err == nil {
			c.pendingCalls[id] = member.Value.Str()
		}
	}
	for _, member := range s.Get("pendingSteer").Members() {
		if id, err := strconv.ParseInt(member.Key, 10, 64); err == nil {
			c.pendingSteer[id] = member.Value.Str()
		}
	}
	for _, member := range s.Get("pendingServer").Members() {
		v := member.Value
		pending := pendingServerRequest{RPCID: v.Get("rpcId"), Method: v.Get("method").Str(), Params: v.Get("params"),
			QuestionsByText: map[string]string{}, Choices: map[string]qtjson.Value{}}
		for _, id := range v.Get("questionIds").Items() {
			pending.QuestionIDs = append(pending.QuestionIDs, id.Str())
		}
		for _, entry := range v.Get("questionsByText").Members() {
			pending.QuestionsByText[entry.Key] = entry.Value.Str()
		}
		for _, entry := range v.Get("choices").Members() {
			pending.Choices[entry.Key] = entry.Value
		}
		c.pendingServer[member.Key] = pending
	}
	for _, member := range s.Get("pendingPermissions").Members() {
		c.pendingPermissions[member.Key] = member.Value
	}
	c.toolUses = readSet(s.Get("toolUses"))
	c.toolResults = readSet(s.Get("toolResults"))
	c.reasoningDeltas = readSet(s.Get("reasoningDeltas"))
	c.noticeItems = readSet(s.Get("noticeItems"))
	c.resultTurns = readSet(s.Get("resultTurns"))
	for _, member := range s.Get("syntheticTools").Members() {
		c.syntheticTools[member.Key] = member.Value
	}
	str := func(key string) string { return s.Get(key).Str() }
	c.cwd, c.initialPrompt, c.resumeThreadID = str("cwd"), str("initialPrompt"), str("resumeThreadId")
	c.resumeFallbackPrompt, c.model, c.effectiveModel = str("resumeFallbackPrompt"), str("model"), str("effectiveModel")
	c.mode, c.effort, c.hostSessionID = str("mode"), str("effort"), str("hostSessionId")
	c.externalCallGate = s.Get("externalCallGate").BoolValue()
	c.threadID, c.turnID, c.activeTurnMode, c.lastAgentText = str("threadId"), str("turnId"), str("activeTurnMode"), str("lastAgentText")
	c.queuedUserTexts = nil
	for _, text := range s.Get("queuedUserTexts").Items() {
		c.queuedUserTexts = append(c.queuedUserTexts, text.Str())
	}
	if v := s.Get("lastTokenUsage"); v.IsObject() {
		c.lastTokenUsage = v
	}
	if v := s.Get("rateLimits"); v.IsObject() {
		c.rateLimits = v
	}
	if v := s.Get("rateLimitsByLimitId"); v.IsObject() {
		c.rateLimitsByLimitID = v
	}
	c.threadReady = s.Get("threadReady").BoolValue()
	c.initialTurnSent = s.Get("initialTurnSent").BoolValue()
	c.turnActive = s.Get("turnActive").BoolValue()
	c.systemInitEmitted = s.Get("systemInitEmitted").BoolValue()
	c.nextNormalizedSequence = s.Get("nextNormalizedSequence").Int64()
}

func (c *codexState) persist() {
	if c.r.m.procs == nil {
		return
	}
	value := string(c.snapshot().Compact())
	c.persistMu.Lock()
	if value == c.persistValue {
		c.persistMu.Unlock()
		return
	}
	c.persistValue = value
	if c.persistKick == nil {
		c.persistKick = make(chan struct{}, 1)
		go c.persistLoop()
	}
	c.persistMu.Unlock()
	select {
	case c.persistKick <- struct{}{}:
	default:
	}
}

func (c *codexState) persistLoop() {
	written := ""
	for range c.persistKick {
		c.persistMu.Lock()
		value := c.persistValue
		c.persistMu.Unlock()
		if value == written {
			continue
		}
		if c.r.m.procs.MergeMeta(c.r.key, map[string]any{"codexState": value}) {
			written = value
		}
	}
}
