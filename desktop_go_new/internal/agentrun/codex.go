package agentrun

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"sync"

	"forkmesh.com/desktop-core/internal/qtjson"
)

const RuntimeCodex = "codex"

const fullAccessSandbox = "danger-full-access"

var attachedImageLine = regexp.MustCompile(`(?i)^[\t\n\v\f\r ]*Attached image:[\t\n\v\f\r ]*(.+?)[\t\n\v\f\r ]*$`)

type pendingServerRequest struct {
	RPCID           qtjson.Value
	Method          string
	Params          qtjson.Value
	QuestionIDs     []string
	QuestionsByText map[string]string
	Choices         map[string]qtjson.Value
}

type codexState struct {
	r *run

	nextRequestID          int64
	nextQuestionToken      int64
	pendingCalls           map[int64]string
	pendingSteer           map[int64]string
	pendingServer          map[string]pendingServerRequest
	pendingPermissions     map[string]qtjson.Value
	toolUses               map[string]bool
	toolResults            map[string]bool
	reasoningDeltas        map[string]bool
	noticeItems            map[string]bool
	resultTurns            map[string]bool
	syntheticTools         map[string]qtjson.Value
	cwd                    string
	initialPrompt          string
	resumeThreadID         string
	resumeFallbackPrompt   string
	model                  string
	effectiveModel         string
	mode                   string
	externalCallGate       bool
	effort                 string
	hostSessionID          string
	appVersion             string
	threadID               string
	turnID                 string
	activeTurnMode         string
	lastAgentText          string
	queuedUserTexts        []string
	models                 qtjson.Value
	lastTokenUsage         qtjson.Value
	rateLimits             qtjson.Value
	rateLimitsByLimitID    qtjson.Value
	threadReady            bool
	initialTurnSent        bool
	turnActive             bool
	systemInitEmitted      bool
	nextNormalizedSequence int64

	lineStart int64
	lineEnd   int64
	lineIndex int64
	fresh     bool

	writeHook   func([]byte) bool
	runningHook func() bool

	persistMu    sync.Mutex
	persistValue string
	persistKick  chan struct{}
}

func metaString(meta map[string]any, key string) string {
	value, _ := meta[key].(string)
	return value
}

func newCodexState(r *run, meta map[string]any) *codexState {
	c := &codexState{r: r, nextRequestID: 1, nextQuestionToken: 1, pendingCalls: map[int64]string{},
		pendingSteer: map[int64]string{}, pendingServer: map[string]pendingServerRequest{},
		pendingPermissions: map[string]qtjson.Value{}, toolUses: map[string]bool{}, toolResults: map[string]bool{},
		reasoningDeltas: map[string]bool{}, noticeItems: map[string]bool{}, resultTurns: map[string]bool{},
		syntheticTools: map[string]qtjson.Value{}, models: qtjson.NewArray(), lastTokenUsage: qtjson.NewObject(),
		rateLimits: qtjson.NewObject(), rateLimitsByLimitID: qtjson.NewObject(), lineIndex: -1}
	c.cwd = metaString(meta, "cwd")
	c.initialPrompt = metaString(meta, "initialPrompt")
	c.resumeThreadID = trimmed(metaString(meta, "resumeThreadId"))
	c.resumeFallbackPrompt = trimmed(metaString(meta, "resumeFallbackPrompt"))
	if c.resumeFallbackPrompt == "" {
		c.resumeFallbackPrompt = c.initialPrompt
	}
	c.model = trimmed(metaString(meta, "model"))
	c.mode = metaString(meta, "mode")
	c.effort = trimmed(metaString(meta, "effort"))
	c.externalCallGate, _ = meta["externalCallGate"].(bool)
	c.hostSessionID = metaString(meta, "sessionId")
	c.appVersion = metaString(meta, "appVersion")
	if saved := metaString(meta, "codexState"); saved != "" {
		if snapshot, ok := qtjson.ParseObject([]byte(saved)); ok {
			c.restore(snapshot)
		}
	} else {
		c.fresh = true
	}
	return c
}

func (c *codexState) start() {
	if !c.fresh {
		return
	}
	c.fresh = false
	version := c.appVersion
	if version == "" {
		version = "0"
	}
	c.sendRequest("initialize", qtjson.ObjectOf(
		"clientInfo", qtjson.ObjectOf("name", "forkmesh", "title", "ForkMesh", "version", version),
		"capabilities", qtjson.ObjectOf("experimentalApi", true, "requestAttestation", false)))
	c.persist()
}

func compactJSON(value qtjson.Value) string {
	switch value.Kind() {
	case qtjson.Object, qtjson.Array:
		return string(value.Compact())
	case qtjson.String:
		return value.Str()
	case qtjson.Bool:
		if value.BoolValue() {
			return "true"
		}
		return "false"
	case qtjson.Integer, qtjson.Double:
		return strconv.FormatFloat(value.Float(), 'g', 6, 64)
	case qtjson.Null:
		return "null"
	}
	return ""
}

func normalizedMode(mode string) string { return strings.ToLower(trimmed(mode)) }

func approvalPolicyForMode(string) string { return "never" }

func setDefined(obj *qtjson.Value, key string, value qtjson.Value) {
	if value.IsUndefined() {
		obj.Remove(key)
		return
	}
	obj.Set(key, value)
}

func objectOfDefined(pairs ...any) qtjson.Value {
	out := qtjson.NewObject()
	for i := 0; i+1 < len(pairs); i += 2 {
		key, _ := pairs[i].(string)
		setDefined(&out, key, qtjson.From(pairs[i+1]))
	}
	return out
}

func contextFields(params qtjson.Value) qtjson.Value {
	fields := qtjson.NewObject()
	for _, pair := range [][2]string{{"threadId", "thread_id"}, {"turnId", "turn_id"}, {"itemId", "item_id"}} {
		if params.Has(pair[0]) {
			fields.Set(pair[0], params.Get(pair[0]))
			fields.Set(pair[1], params.Get(pair[0]))
		}
	}
	return fields
}

func mergeObject(target *qtjson.Value, source qtjson.Value) {
	for _, member := range source.Members() {
		target.Set(member.Key, member.Value)
	}
}

var nullableMetadata = map[string]bool{"credits": true, "individualLimit": true, "limitId": true, "limitName": true,
	"planType": true, "rateLimitReachedType": true, "spendControlReached": true}

func mergeRateLimitSnapshot(current, update qtjson.Value) qtjson.Value {
	if !current.IsObject() {
		current = qtjson.NewObject()
	}
	current = current.Clone()
	for _, member := range update.Members() {
		if member.Value.IsNull() && nullableMetadata[member.Key] && current.Has(member.Key) {
			continue
		}
		if (member.Key == "primary" || member.Key == "secondary") && member.Value.IsObject() &&
			current.Get(member.Key).IsObject() {
			window := current.Get(member.Key).Clone()
			mergeObject(&window, member.Value)
			current.Set(member.Key, window)
			continue
		}
		current.Set(member.Key, member.Value)
	}
	return current
}

func messageText(value qtjson.Value) string {
	switch value.Kind() {
	case qtjson.String:
		return value.Str()
	case qtjson.Array:
		parts := []string{}
		for _, part := range value.Items() {
			if text := messageText(part); text != "" {
				parts = append(parts, text)
			}
		}
		return strings.Join(parts, "\n")
	case qtjson.Object:
		for _, key := range []string{"message", "text", "error", "reason"} {
			if value.Has(key) {
				if text := messageText(value.Get(key)); text != "" {
					return text
				}
			}
		}
		return compactJSON(value)
	}
	return compactJSON(value)
}

func decisionLabel(decision qtjson.Value) string {
	if !decision.IsString() {
		return "Allow with changes"
	}
	switch decision.Str() {
	case "accept":
		return "Allow once"
	case "acceptForSession":
		return "Always allow"
	case "decline":
		return "Deny"
	case "cancel":
		return "Cancel"
	}
	return decision.Str()
}

var toolItemTypes = map[string]bool{"commandExecution": true, "fileChange": true, "mcpToolCall": true,
	"dynamicToolCall": true, "collabAgentToolCall": true, "webSearch": true, "imageView": true,
	"imageGeneration": true, "plan": true, "subAgentActivity": true, "sleep": true}

func toolName(item qtjson.Value) string {
	switch item.Get("type").Str() {
	case "commandExecution":
		return "Bash"
	case "fileChange":
		return "FileChange"
	case "mcpToolCall":
		server, tool := item.Get("server").Str(), item.Get("tool").Str()
		if server == "" {
			return tool
		}
		return server + "::" + tool
	case "dynamicToolCall":
		return item.Get("tool").StrOr("DynamicTool")
	case "collabAgentToolCall", "subAgentActivity":
		return "Task"
	case "webSearch":
		return "WebSearch"
	case "imageView":
		return "Read"
	case "imageGeneration":
		return "ImageGeneration"
	case "plan":
		return "TodoWrite"
	case "sleep":
		return "Sleep"
	}
	return "CodexTool"
}

func toolInput(item qtjson.Value) qtjson.Value {
	g := item.Get
	switch item.Get("type").Str() {
	case "commandExecution":
		return objectOfDefined("command", g("command"), "cwd", g("cwd"))
	case "fileChange":
		return objectOfDefined("changes", g("changes"))
	case "mcpToolCall":
		return objectOfDefined("server", g("server"), "tool", g("tool"), "arguments", g("arguments"))
	case "dynamicToolCall":
		return objectOfDefined("namespace", g("namespace"), "tool", g("tool"), "arguments", g("arguments"))
	case "collabAgentToolCall":
		return objectOfDefined("tool", g("tool"), "prompt", g("prompt"), "model", g("model"),
			"receiverThreadIds", g("receiverThreadIds"))
	case "webSearch":
		return objectOfDefined("query", g("query"), "action", g("action"))
	case "imageView":
		return objectOfDefined("file_path", g("path"))
	case "imageGeneration":
		return objectOfDefined("prompt", g("revisedPrompt"))
	case "plan":
		if g("todos").IsArray() {
			return objectOfDefined("todos", g("todos"))
		}
		return qtjson.ObjectOf("todos", qtjson.NewArray(objectOfDefined("content", g("text"), "status", "in_progress")))
	case "subAgentActivity":
		return objectOfDefined("description", g("kind"), "agent_thread_id", g("agentThreadId"), "agent_path", g("agentPath"))
	case "sleep":
		return objectOfDefined("duration_ms", g("durationMs"))
	}
	return item
}

func toolOutput(item qtjson.Value) string {
	g := item.Get
	switch item.Get("type").Str() {
	case "commandExecution":
		output := g("aggregatedOutput").Str()
		if g("exitCode").IsNumber() {
			output += "\nExit code: " + strconv.Itoa(g("exitCode").Int())
		}
		return trimmed(output)
	case "fileChange":
		return compactJSON(g("changes"))
	case "mcpToolCall":
		if e := g("error"); e.IsNull() || e.IsUndefined() {
			return compactJSON(g("result"))
		}
		return compactJSON(g("error"))
	case "dynamicToolCall":
		return compactJSON(g("contentItems"))
	case "collabAgentToolCall":
		return compactJSON(g("agentsStates"))
	case "webSearch":
		return compactJSON(g("action"))
	case "imageView":
		return g("path").Str()
	case "imageGeneration":
		if path := g("savedPath").Str(); path != "" {
			return path
		}
		return g("result").Str()
	case "plan":
		return g("text").Str()
	case "subAgentActivity":
		return compactJSON(objectOfDefined("kind", g("kind"), "agentThreadId", g("agentThreadId"), "agentPath", g("agentPath")))
	case "sleep":
		return "Waited " + strconv.Itoa(g("durationMs").Int()) + " ms"
	}
	return compactJSON(item)
}

func toolFailed(item qtjson.Value) bool {
	if success := item.Get("success"); success.IsBool() && !success.BoolValue() {
		return true
	}
	if code := item.Get("exitCode"); code.IsNumber() && code.Int() != 0 {
		return true
	}
	status := strings.ToLower(item.Get("status").Str())
	return strings.Contains(status, "fail") || strings.Contains(status, "error") ||
		strings.Contains(status, "declin") || strings.Contains(status, "cancel")
}

func (c *codexState) nextSo() int64 {
	if c.lineIndex < 0 {
		return -1
	}
	c.lineIndex++
	so := c.lineStart + c.lineIndex
	if so > c.lineEnd {
		so = c.lineEnd
	}
	return so
}

func (c *codexState) emit(event qtjson.Value) {
	so := c.nextSo()
	if so >= 0 && so <= c.r.skipUntil {
		return
	}
	if so >= 0 {
		event.Set("_so", qtjson.NewDouble(float64(so)))
		event.Set("_sk", qtjson.NewString(c.r.key))
	}
	c.r.emitEventLocked(event, so, true, "")
}

func (c *codexState) emitTransient(event qtjson.Value) {
	c.r.emitEventLocked(event, -1, false, "")
}

func (c *codexState) emitLocalNotice(level, text string) {
	if trimmed(text) == "" {
		return
	}
	c.emit(qtjson.ObjectOf("type", "_local_notice", "level", level, "text", text))
}

func (c *codexState) running() bool {
	if c.runningHook != nil {
		return c.runningHook()
	}
	return c.r.m.procs != nil && c.r.m.procs.Running(c.r.key)
}

func (c *codexState) sendObject(message qtjson.Value) bool {
	if !c.running() {
		return false
	}
	line := append(message.Compact(), '\n')
	if c.writeHook != nil {
		return c.writeHook(line)
	}
	return c.r.m.procs.WriteStdin(c.r.key, line, false) == nil
}

func (c *codexState) sendRequest(method string, params qtjson.Value) int64 {
	id := c.nextRequestID
	c.nextRequestID++
	c.pendingCalls[id] = method
	c.sendObject(qtjson.ObjectOf("method", method, "id", id, "params", params))
	return id
}

func (c *codexState) sendNotification(method string, params qtjson.Value) {
	c.sendObject(qtjson.ObjectOf("method", method, "params", params))
}

func (c *codexState) ingest(data []byte, offset int64) {
	from := 0
	for from < len(data) {
		newline := bytes.IndexByte(data[from:], '\n')
		to := len(data)
		if newline >= 0 {
			to = from + newline + 1
		}
		frames, errs := c.r.framer.Append(data[from:to], false)
		end := offset + int64(to)
		for _, message := range errs {
			c.r.sendLocked(map[string]any{"key": c.r.key, "stderr": message})
			c.lineIndex = -1
			c.emitLocalNotice("error", message)
		}
		for _, frame := range frames {
			c.handleLine(frame, end)
		}
		from = to
	}
}

func (c *codexState) handleLine(line []byte, end int64) {
	if len(bytes.TrimSpace(line)) == 0 {
		return
	}
	c.lineStart = end - int64(len(line)) - 1
	c.lineEnd = end
	c.lineIndex = 0
	defer func() { c.lineIndex = -1 }()
	if end > c.r.skipUntil {
		c.r.sendLocked(map[string]any{"key": c.r.key, "raw": rawView(line), "so": end})
	}
	message, ok := qtjson.ParseObject(line)
	if !ok {
		return
	}
	if end <= c.r.skipUntil {
		return
	}
	c.handleMessage(message)
	c.r.delivered = max(c.r.delivered, end)
	c.persist()
}

func (c *codexState) handleMessage(message qtjson.Value) {
	method := message.Get("method").Str()
	if method != "" {
		if message.Has("id") {
			c.handleServerRequest(message.Get("id"), method, message.Get("params"))
		} else {
			c.handleNotification(method, message.Get("params"))
		}
		return
	}
	if message.Has("id") {
		c.handleResponse(message)
	}
}

func valueLongLong(v qtjson.Value) int64 {
	switch v.Kind() {
	case qtjson.Integer, qtjson.Double:
		return v.Int64()
	case qtjson.String:
		n, err := strconv.ParseInt(trimmed(v.Str()), 10, 64)
		if err == nil {
			return n
		}
	case qtjson.Bool:
		if v.BoolValue() {
			return 1
		}
	}
	return 0
}

func (c *codexState) failTurnResult(failure string) qtjson.Value {
	return qtjson.ObjectOf("type", "result", "subtype", "error", "is_error", true, "result", failure,
		"session_id", c.threadID, "thread_id", c.threadID, "num_turns", 1)
}

func (c *codexState) handleResponse(message qtjson.Value) {
	id := valueLongLong(message.Get("id"))
	method, known := c.pendingCalls[id]
	delete(c.pendingCalls, id)
	if !known || method == "" {
		return
	}
	if message.Has("error") {
		failure := "Codex " + method + " failed: " + messageText(message.Get("error"))
		switch method {
		case "account/rateLimits/read":
			return
		case "thread/resume":
			c.startFreshThreadAfterResumeFailure(failure)
			return
		}
		c.emitLocalNotice("error", failure)
		switch method {
		case "turn/start":
			c.turnActive = false
			c.turnID = ""
			c.activeTurnMode = ""
			c.emit(c.failTurnResult(failure))
			c.flushQueuedUserText()
		case "turn/steer":
			if rejected := c.pendingSteer[id]; rejected != "" {
				c.queuedUserTexts = append(c.queuedUserTexts, rejected)
			}
			delete(c.pendingSteer, id)
			if !c.turnActive {
				c.flushQueuedUserText()
			}
		case "initialize", "thread/start":
			if c.r.m.procs != nil {
				key := c.r.key
				procs := c.r.m.procs
				go func() {
					_ = procs.CloseStdin(key)
					procs.StopProcess(key, 1500)
				}()
			}
		}
		return
	}
	result := message.Get("result")
	switch method {
	case "initialize":
		c.sendNotification("initialized", qtjson.NewObject())
		c.sendRequest("account/rateLimits/read", qtjson.NewNull())
		c.sendRequest("model/list", qtjson.ObjectOf("limit", 100, "includeHidden", false))
		c.sendThreadRequest()
	case "model/list":
		for _, model := range result.Get("data").Items() {
			c.models.Append(model)
		}
		if cursor := result.Get("nextCursor").Str(); cursor != "" {
			c.sendRequest("model/list", qtjson.ObjectOf("cursor", cursor, "limit", 100, "includeHidden", false))
		} else {
			c.r.sendLocked(map[string]any{"key": c.r.key, "models": c.models.Clone().Interface()})
		}
	case "account/rateLimits/read":
		c.rateLimits = result.Get("rateLimits")
		if !c.rateLimits.IsObject() {
			c.rateLimits = qtjson.NewObject()
		}
		c.rateLimitsByLimitID = result.Get("rateLimitsByLimitId")
		if !c.rateLimitsByLimitID.IsObject() {
			c.rateLimitsByLimitID = qtjson.NewObject()
		}
		if legacy := c.rateLimits.Get("limitId").Str(); legacy != "" && !c.rateLimitsByLimitID.Has(legacy) {
			c.rateLimitsByLimitID.Set(legacy, c.rateLimits)
		}
		c.emit(qtjson.ObjectOf("type", "_codex_rate_limits", "rateLimits", c.rateLimits,
			"rateLimitsByLimitId", c.rateLimitsByLimitID))
	case "thread/start", "thread/resume":
		c.acceptThread(result)
	case "turn/start":
		if id := result.Get("turn").Get("id").Str(); id != "" {
			c.turnID = id
		}
		c.turnActive = true
		c.flushQueuedUserText()
	case "turn/steer":
		delete(c.pendingSteer, id)
	}
}

func (c *codexState) startFreshThreadAfterResumeFailure(failure string) {
	c.resumeThreadID = ""
	c.threadID = ""
	c.turnID = ""
	c.activeTurnMode = ""
	c.threadReady = false
	c.turnActive = false
	c.initialTurnSent = false
	c.initialPrompt = c.resumeFallbackPrompt
	c.emitLocalNotice("warning", failure+". The saved Codex thread is unavailable, so ForkMesh started a new thread "+
		"with the saved task and latest instruction.")
	c.sendThreadRequest()
}

func (c *codexState) approvalPolicy() string {
	if c.externalCallGate {
		return "untrusted"
	}
	return approvalPolicyForMode(c.mode)
}

func (c *codexState) sendThreadRequest() {
	params := qtjson.ObjectOf("cwd", c.cwd, "approvalPolicy", c.approvalPolicy(), "sandbox", fullAccessSandbox)
	if c.model != "" {
		params.Set("model", qtjson.NewString(c.model))
	}
	if c.resumeThreadID == "" {
		c.sendRequest("thread/start", params)
		return
	}
	params.Set("threadId", qtjson.NewString(c.resumeThreadID))
	c.sendRequest("thread/resume", params)
}

func (c *codexState) acceptThread(result qtjson.Value) {
	if id := result.Get("thread").Get("id").Str(); id != "" {
		c.threadID = id
	}
	if c.threadID == "" {
		return
	}
	if effective := result.Get("model").Str(); effective != "" {
		c.effectiveModel = effective
	} else {
		c.effectiveModel = c.model
	}
	c.threadReady = true
	c.emitSystemInit()
	if !c.initialTurnSent && trimmed(c.initialPrompt) != "" {
		c.initialTurnSent = true
		c.beginTurn(c.initialPrompt)
	} else {
		c.flushQueuedUserText()
	}
}

func userInputs(text string) qtjson.Value {
	textLines := []string{}
	imagePaths := []string{}
	for _, line := range strings.Split(text, "\n") {
		match := attachedImageLine.FindStringSubmatch(line)
		if match == nil {
			textLines = append(textLines, line)
			continue
		}
		path := trimmed(match[1])
		if len16(path) >= 2 && ((strings.HasPrefix(path, "\"") && strings.HasSuffix(path, "\"")) ||
			(strings.HasPrefix(path, "'") && strings.HasSuffix(path, "'"))) {
			path = path[1 : len(path)-1]
		}
		if path != "" {
			imagePaths = append(imagePaths, path)
		}
	}
	inputs := qtjson.NewArray()
	if remaining := trimmed(strings.Join(textLines, "\n")); remaining != "" {
		inputs.Append(qtjson.ObjectOf("type", "text", "text", remaining, "text_elements", qtjson.NewArray()))
	}
	for _, path := range imagePaths {
		inputs.Append(qtjson.ObjectOf("type", "localImage", "path", path))
	}
	return inputs
}

func (c *codexState) beginTurn(text string) {
	inputs := userInputs(text)
	if inputs.Len() == 0 || c.threadID == "" {
		return
	}
	params := qtjson.ObjectOf("threadId", c.threadID, "input", inputs, "approvalPolicy", c.approvalPolicy(),
		"sandboxPolicy", qtjson.ObjectOf("type", "dangerFullAccess"))
	if c.model != "" {
		params.Set("model", qtjson.NewString(c.model))
	}
	if c.effort != "" {
		params.Set("effort", qtjson.NewString(c.effort))
	}
	collaborationModel := c.effectiveModel
	if collaborationModel == "" {
		collaborationModel = c.model
	}
	if collaborationModel != "" {
		mode := "default"
		if strings.Contains(normalizedMode(c.mode), "plan") {
			mode = "plan"
		}
		effort := qtjson.NewNull()
		if c.effort != "" {
			effort = qtjson.NewString(c.effort)
		}
		params.Set("collaborationMode", qtjson.ObjectOf("mode", mode, "settings", qtjson.ObjectOf(
			"model", collaborationModel, "reasoning_effort", effort, "developer_instructions", qtjson.NewNull())))
	} else {
		params.Set("collaborationMode", qtjson.NewNull())
	}
	c.turnActive = true
	c.turnID = ""
	c.activeTurnMode = c.mode
	c.lastAgentText = ""
	c.sendRequest("turn/start", params)
}

func (c *codexState) flushQueuedUserText() {
	for len(c.queuedUserTexts) > 0 && c.threadReady && (!c.turnActive || c.turnID != "") {
		if c.turnActive && normalizedMode(c.activeTurnMode) != normalizedMode(c.mode) {
			return
		}
		text := c.queuedUserTexts[0]
		c.queuedUserTexts = c.queuedUserTexts[1:]
		c.sendUserText(text)
		if c.turnActive && c.turnID == "" {
			break
		}
	}
}

func (c *codexState) sendUserText(text string) bool {
	if trimmed(text) == "" {
		return true
	}
	if !c.running() {
		return false
	}
	if !c.threadReady || (c.turnActive && c.turnID == "") {
		c.queuedUserTexts = append(c.queuedUserTexts, text)
		return true
	}
	if !c.turnActive {
		c.beginTurn(text)
		return true
	}
	if normalizedMode(c.activeTurnMode) != normalizedMode(c.mode) {
		c.queuedUserTexts = append(c.queuedUserTexts, text)
		mode := trimmed(c.mode)
		if mode == "" {
			mode = "Ask"
		}
		c.emitLocalNotice("info", "Permission mode changed to "+mode+". This message will start after the current "+
			"turn completes so Codex can apply it.")
		return true
	}
	params := qtjson.ObjectOf("threadId", c.threadID, "input", userInputs(text), "expectedTurnId", c.turnID)
	id := c.sendRequest("turn/steer", params)
	c.pendingSteer[id] = text
	return true
}

func (c *codexState) handleNotification(method string, params qtjson.Value) {
	switch method {
	case "thread/started":
		if id := params.Get("thread").Get("id").Str(); id != "" && c.threadID == "" {
			c.threadID = id
		}
		return
	case "turn/started":
		c.turnActive = true
		if id := params.Get("turn").Get("id").Str(); id != "" {
			c.turnID = id
		}
		c.flushQueuedUserText()
		return
	case "turn/completed":
		replay := len(c.queuedUserTexts) > 0
		c.finishSyntheticTools(params.Get("turn").Get("id").Str())
		if replay {
			c.emitLocalNotice("warning", "Starting the queued message as a new turn.")
		} else {
			c.emitTurnResult(params)
		}
		c.turnActive = false
		c.turnID = ""
		c.activeTurnMode = ""
		c.flushQueuedUserText()
		return
	case "item/started":
		item := params.Get("item")
		c.emitSpecialItemNotice(item)
		if toolItemTypes[item.Get("type").Str()] {
			c.emitToolUse(item)
		}
		return
	case "item/completed":
		c.emitItemCompleted(params.Get("item"))
		return
	case "item/agentMessage/delta":
		normalized := qtjson.ObjectOf("type", "_codex_agent_delta", "delta", params.Get("delta"), "text", params.Get("delta"))
		normalized = objectRemoveUndefined(normalized, params.Get("delta"), "delta", "text")
		mergeObject(&normalized, contextFields(params))
		c.emit(normalized)
		return
	case "item/reasoning/summaryTextDelta", "item/reasoning/textDelta":
		c.emitReasoningDelta(params)
		return
	case "thread/tokenUsage/updated":
		c.emitUsage(params)
		return
	case "account/rateLimits/updated":
		update := params.Get("rateLimits")
		if !update.IsObject() {
			update = qtjson.NewObject()
		}
		limitID := update.Get("limitId").Str()
		if c.rateLimits.Len() == 0 || limitID == "" || c.rateLimits.Get("limitId").Str() == limitID {
			c.rateLimits = mergeRateLimitSnapshot(c.rateLimits, update)
		}
		if limitID != "" {
			c.rateLimitsByLimitID.Set(limitID, mergeRateLimitSnapshot(c.rateLimitsByLimitID.Get(limitID), update))
		}
		shown := c.rateLimits
		if shown.Len() == 0 {
			shown = update
		}
		c.emit(qtjson.ObjectOf("type", "_codex_rate_limits", "rateLimits", shown, "rateLimitsByLimitId", c.rateLimitsByLimitID))
		return
	case "turn/plan/updated":
		turnID := params.Get("turnId").Str()
		itemID := "plan-" + turnID
		todos := qtjson.NewArray()
		lines := []string{}
		for _, value := range params.Get("plan").Items() {
			text := value.Get("step").Str()
			status := value.Get("status").Str()
			if status == "inProgress" {
				status = "in_progress"
			}
			todos.Append(qtjson.ObjectOf("content", text, "status", status))
			lines = append(lines, text)
		}
		item := qtjson.ObjectOf("type", "plan", "id", itemID, "text", strings.Join(lines, "\n"), "todos", todos, "_turnId", turnID)
		c.syntheticTools[itemID] = item
		c.emitToolUse(item)
		delta := params.Clone()
		if !delta.IsObject() {
			delta = qtjson.NewObject()
		}
		delta.Set("itemId", qtjson.NewString(itemID))
		setDefined(&delta, "delta", params.Get("plan"))
		c.emitToolDelta(method, delta)
		return
	case "error", "warning", "configWarning":
		level := "warning"
		if method == "error" {
			level = "error"
		}
		text := messageText(params)
		c.emitLocalNotice(level, text)
		if method == "error" && !params.Get("willRetry").BoolValue() {
			turnID := params.Get("turnId").Str()
			if turnID != "" {
				c.resultTurns[turnID] = true
			}
			event := c.failTurnResult(text)
			event.Set("turn_id", qtjson.NewString(turnID))
			c.emit(event)
			c.turnActive = false
			c.turnID = ""
			c.activeTurnMode = ""
		}
		return
	}
	if strings.HasPrefix(method, "item/") || method == "turn/diff/updated" {
		c.emitToolDelta(method, params)
	}
}

func objectRemoveUndefined(obj qtjson.Value, value qtjson.Value, keys ...string) qtjson.Value {
	if value.IsUndefined() {
		for _, key := range keys {
			obj.Remove(key)
		}
	}
	return obj
}

func (c *codexState) handleServerRequest(id qtjson.Value, method string, params qtjson.Value) {
	commandApproval := method == "item/commandExecution/requestApproval"
	fileApproval := method == "item/fileChange/requestApproval"
	userInput := method == "item/tool/requestUserInput"
	elicitation := method == "mcpServer/elicitation/request"
	permissions := method == "item/permissions/requestApproval"
	if !commandApproval && !fileApproval && !userInput && !elicitation && !permissions {
		c.sendObject(qtjson.ObjectOf("id", id, "error", qtjson.ObjectOf("code", -32601,
			"message", "Unsupported Codex server request")))
		return
	}
	approval := commandApproval || fileApproval || permissions
	kind := "request"
	if approval {
		kind = "approval"
	}
	token := "codex-" + kind + "-" + strconv.FormatInt(c.nextQuestionToken, 10)
	c.nextQuestionToken++
	pending := pendingServerRequest{RPCID: id, Method: method, Params: params, QuestionsByText: map[string]string{},
		Choices: map[string]qtjson.Value{}}
	questions := qtjson.NewArray()
	switch {
	case commandApproval || fileApproval:
		decisions := params.Get("availableDecisions")
		if decisions.Len() == 0 || !decisions.IsArray() {
			decisions = qtjson.NewArray(qtjson.NewString("accept"), qtjson.NewString("acceptForSession"), qtjson.NewString("decline"))
		}
		options := qtjson.NewArray()
		for _, decision := range decisions.Items() {
			label := decisionLabel(decision)
			if label == "" {
				continue
			}
			description := compactJSON(decision)
			if decision.IsString() {
				description = decision.Str()
			}
			options.Append(qtjson.ObjectOf("label", label, "description", description))
			pending.Choices[strings.ToLower(label)] = decision
			if decision.IsString() {
				pending.Choices[strings.ToLower(decision.Str())] = decision
			}
		}
		question := params.Get("reason").Str()
		if question == "" {
			if commandApproval {
				question = "Allow Codex to run this command?\n" + params.Get("command").Str()
			} else {
				question = "Allow Codex to apply these file changes?"
			}
		}
		header := "File approval"
		if commandApproval {
			header = "Command approval"
		}
		questions.Append(qtjson.ObjectOf("question", question, "header", header, "options", options, "multiSelect", false))
	case userInput:
		for _, source := range params.Get("questions").Items() {
			questionID := source.Get("id").Str()
			pending.QuestionIDs = append(pending.QuestionIDs, questionID)
			pending.QuestionsByText[source.Get("question").Str()] = questionID
			question := objectOfDefined("question", source.Get("question"), "header", source.Get("header"),
				"options", source.Get("options"), "multiSelect", false)
			if source.Get("isOther").BoolValue() {
				question.Set("isOther", qtjson.NewBool(true))
			}
			if source.Get("isSecret").BoolValue() {
				question.Set("isSecret", qtjson.NewBool(true))
			}
			questions.Append(question)
		}
	default:
		prompt := "Allow the requested permissions?"
		header := "Permission request"
		if elicitation {
			prompt = params.Get("message").Str()
			header = "MCP request"
		}
		questions.Append(qtjson.ObjectOf("question", prompt, "header", header, "options", qtjson.NewArray(
			qtjson.ObjectOf("label", "Allow", "description", "Approve this request"),
			qtjson.ObjectOf("label", "Deny", "description", "Reject this request")), "multiSelect", false))
	}
	c.pendingServer[token] = pending
	if approval && !c.externalCallGate && c.approvalPolicy() == "never" {
		answer := "Allow for session"
		if commandApproval || fileApproval {
			if _, ok := pending.Choices["acceptforsession"]; ok {
				answer = "acceptForSession"
			} else {
				answer = "accept"
			}
		}
		c.respond(token, answer)
		return
	}
	if commandApproval && c.hostSessionID != "" {
		command := params.Get("command").Str()
		fingerprint := []byte(command)
		if command == "" {
			fingerprint = params.Compact()
		}
		digest := sha256.Sum256(fingerprint)
		description := "Allow this process to execute?"
		if command != "" {
			description = "Run command:\n" + command
		}
		if reason := params.Get("reason").Str(); reason != "" {
			description += "\n\nReason: " + reason
		}
		description = left16(description, 1024)
		event := qtjson.ObjectOf("schemaVersion", 1, "type", "permission.requested", "sequence", c.nextNormalizedSequence,
			"requestId", token, "sessionId", c.hostSessionID, "data", qtjson.ObjectOf(
				"capability", "process.execute", "resource", "command:sha256:"+hex.EncodeToString(digest[:]),
				"description", description, "command", left16(command, 8192)))
		c.nextNormalizedSequence++
		c.pendingPermissions[token] = event
		c.emitTransient(event)
		return
	}
	block := qtjson.ObjectOf("type", "tool_use", "id", token, "name", "AskUserQuestion",
		"input", qtjson.ObjectOf("questions", questions))
	c.emit(qtjson.ObjectOf("type", "assistant", "session_id", c.threadID, "thread_id", c.threadID,
		"message", qtjson.ObjectOf("role", "assistant", "content", qtjson.NewArray(block))))
}

func (c *codexState) respond(token, answer string) bool {
	pending, ok := c.pendingServer[token]
	if !ok || !c.running() {
		return false
	}
	result := qtjson.NewObject()
	key := strings.ToLower(trimmed(answer))
	switch pending.Method {
	case "item/commandExecution/requestApproval", "item/fileChange/requestApproval":
		decision, found := pending.Choices[key]
		if !found {
			switch {
			case key == "accept" || key == "allow" || key == "yes" || strings.Contains(key, "once"):
				decision = qtjson.NewString("accept")
			case strings.Contains(key, "session") || strings.Contains(key, "always"):
				decision = qtjson.NewString("acceptForSession")
			case strings.Contains(key, "cancel"):
				decision = qtjson.NewString("cancel")
			default:
				decision = qtjson.NewString("decline")
			}
		}
		result.Set("decision", decision)
	case "item/tool/requestUserInput":
		answers := qtjson.NewObject()
		supplied, parsed := qtjson.ParseObject([]byte(trimmed(answer)))
		if !parsed {
			supplied = qtjson.NewObject()
		}
		displayed := map[string]string{}
		if supplied.Len() == 0 && len(pending.QuestionIDs) > 1 {
			for _, line := range strings.Split(answer, "\n") {
				if line == "" {
					continue
				}
				separator := strings.Index(line, "→")
				width := len("→")
				if separator < 0 {
					separator = strings.Index(line, " -> ")
					width = 4
				}
				if separator < 0 {
					continue
				}
				question := trimmed(line[:separator])
				value := trimmed(line[separator+width:])
				if id := pending.QuestionsByText[question]; id != "" && value != "" {
					displayed[id] = value
				}
			}
		}
		for _, id := range pending.QuestionIDs {
			value := supplied.Get(id)
			if value.IsUndefined() {
				if text, ok := displayed[id]; ok {
					value = qtjson.NewString(text)
				}
			}
			if value.IsUndefined() && len(pending.QuestionIDs) == 1 {
				value = qtjson.NewString(answer)
			}
			values := qtjson.NewArray()
			switch {
			case value.IsArray():
				values = value
			case value.IsObject() && value.Get("answers").IsArray():
				values = value.Get("answers")
			case !value.IsUndefined() && !value.IsNull():
				values.Append(qtjson.NewString(value.StrOr(compactJSON(value))))
			}
			answers.Set(id, qtjson.ObjectOf("answers", values))
		}
		result.Set("answers", answers)
	case "mcpServer/elicitation/request":
		action := "decline"
		if strings.HasPrefix(key, "accept") {
			action = "accept"
		} else if strings.HasPrefix(key, "cancel") {
			action = "cancel"
		}
		result = qtjson.ObjectOf("action", action, "content", nil, "_meta", nil)
	case "item/permissions/requestApproval":
		accepted := strings.HasPrefix(key, "allow") || key == "accept"
		if accepted {
			setDefined(&result, "permissions", pending.Params.Get("permissions"))
		} else {
			result.Set("permissions", qtjson.NewObject())
		}
		scope := "turn"
		if accepted && strings.Contains(strings.ToLower(answer), "session") {
			scope = "session"
		}
		result.Set("scope", qtjson.NewString(scope))
	}
	if !c.sendObject(qtjson.ObjectOf("id", pending.RPCID, "result", result)) {
		return false
	}
	delete(c.pendingServer, token)
	delete(c.pendingPermissions, token)
	c.persist()
	return true
}

func (c *codexState) emitSystemInit() {
	if c.systemInitEmitted || c.threadID == "" {
		return
	}
	c.systemInitEmitted = true
	c.emit(qtjson.ObjectOf("type", "system", "subtype", "init", "session_id", c.threadID, "cwd", c.cwd,
		"model", c.effectiveModel, "permissionMode", c.mode, "provider", "codex"))
}

func (c *codexState) emitReasoningDelta(params qtjson.Value) {
	if itemID := params.Get("itemId").Str(); itemID != "" {
		c.reasoningDeltas[itemID] = true
	}
	delta := qtjson.ObjectOf("type", "thinking_delta")
	setDefined(&delta, "thinking", params.Get("delta"))
	normalized := qtjson.ObjectOf("type", "stream_event", "event", qtjson.ObjectOf("type", "content_block_delta", "delta", delta))
	mergeObject(&normalized, contextFields(params))
	c.emit(normalized)
}

func (c *codexState) emitToolDelta(method string, params qtjson.Value) {
	delta := params.Get("delta")
	for _, key := range []string{"message", "diff", "changes", "plan"} {
		if !delta.IsUndefined() {
			break
		}
		delta = params.Get(key)
	}
	if delta.IsArray() || delta.IsObject() {
		delta = qtjson.NewString(compactJSON(delta))
	}
	normalized := qtjson.ObjectOf("type", "_codex_tool_delta", "method", method)
	setDefined(&normalized, "delta", delta)
	mergeObject(&normalized, contextFields(params))
	c.emit(normalized)
}

func (c *codexState) emitItemCompleted(item qtjson.Value) {
	c.emitSpecialItemNotice(item)
	kind := item.Get("type").Str()
	switch {
	case kind == "agentMessage":
		c.lastAgentText = item.Get("text").Str()
		event := qtjson.ObjectOf("type", "_codex_agent_complete", "text", c.lastAgentText)
		setDefined(&event, "item_id", item.Get("id"))
		c.emit(event)
	case kind == "reasoning":
		parts := []string{}
		for _, part := range item.Get("summary").Items() {
			parts = append(parts, part.Str())
		}
		for _, part := range item.Get("content").Items() {
			parts = append(parts, part.Str())
		}
		itemID := item.Get("id").Str()
		text := strings.Join(parts, "\n")
		if text != "" && !c.reasoningDeltas[itemID] {
			c.emitReasoningDelta(qtjson.ObjectOf("itemId", itemID, "delta", text))
		}
		c.emit(qtjson.ObjectOf("type", "_codex_reasoning_complete", "item_id", itemID, "text", text))
	case toolItemTypes[kind]:
		c.emitToolUse(item)
		c.emitToolResult(item)
	}
}

func (c *codexState) emitSpecialItemNotice(item qtjson.Value) {
	kind := item.Get("type").Str()
	if kind != "enteredReviewMode" && kind != "exitedReviewMode" && kind != "contextCompaction" {
		return
	}
	id := item.Get("id").Str()
	if id != "" && c.noticeItems[id] {
		return
	}
	if id != "" {
		c.noticeItems[id] = true
	}
	text := "Codex compacted the conversation context."
	switch kind {
	case "enteredReviewMode":
		text = "Codex entered review mode."
	case "exitedReviewMode":
		text = "Codex exited review mode."
	}
	if review := trimmed(item.Get("review").Str()); review != "" {
		text += " " + review
	}
	c.emitLocalNotice("info", text)
}

func (c *codexState) emitToolUse(item qtjson.Value) {
	id := item.Get("id").Str()
	if id == "" || c.toolUses[id] {
		return
	}
	c.toolUses[id] = true
	block := qtjson.ObjectOf("type", "tool_use", "id", id, "name", toolName(item), "input", toolInput(item))
	c.emit(qtjson.ObjectOf("type", "assistant", "session_id", c.threadID,
		"message", qtjson.ObjectOf("role", "assistant", "content", qtjson.NewArray(block))))
}

func (c *codexState) emitToolResult(item qtjson.Value) {
	id := item.Get("id").Str()
	if id == "" || c.toolResults[id] {
		return
	}
	c.toolResults[id] = true
	block := qtjson.ObjectOf("type", "tool_result", "tool_use_id", id, "content", toolOutput(item), "is_error", toolFailed(item))
	c.emit(qtjson.ObjectOf("type", "user", "session_id", c.threadID,
		"message", qtjson.ObjectOf("role", "user", "content", qtjson.NewArray(block))))
}

func (c *codexState) finishSyntheticTools(turnID string) {
	ids := make([]string, 0, len(c.syntheticTools))
	for id, item := range c.syntheticTools {
		if item.Get("_turnId").Str() == turnID {
			ids = append(ids, id)
		}
	}
	sort.Strings(ids)
	for _, id := range ids {
		c.emitToolResult(c.syntheticTools[id])
		delete(c.syntheticTools, id)
	}
}

func (c *codexState) emitTurnResult(params qtjson.Value) {
	turn := params.Get("turn")
	turnID := turn.Get("id").Str()
	if turnID != "" && c.resultTurns[turnID] {
		return
	}
	if turnID != "" {
		c.resultTurns[turnID] = true
	}
	status := turn.Get("status").Str()
	clean := status == "completed"
	resultText := c.lastAgentText
	if resultText == "" {
		items := turn.Get("items").Items()
		for i := len(items) - 1; i >= 0; i-- {
			if items[i].Get("type").Str() == "agentMessage" {
				resultText = items[i].Get("text").Str()
				break
			}
		}
	}
	if !clean && resultText == "" {
		resultText = messageText(turn.Get("error"))
	}
	if !clean && resultText == "" {
		if status == "interrupted" {
			resultText = "Codex turn was interrupted."
		} else {
			resultText = "Codex turn did not complete successfully."
		}
	}
	subtype := "error"
	if clean {
		subtype = "success"
	} else if status == "interrupted" {
		subtype = "interrupted"
	}
	normalized := qtjson.ObjectOf("type", "result", "subtype", subtype, "is_error", !clean, "result", resultText,
		"session_id", c.threadID, "thread_id", c.threadID)
	setDefined(&normalized, "turn_id", turn.Get("id"))
	if turn.Get("durationMs").IsNumber() {
		normalized.Set("duration_ms", turn.Get("durationMs"))
	}
	normalized.Set("turn_status", qtjson.NewString(status))
	normalized.Set("num_turns", qtjson.NewInt(1))
	if turn.Has("startedAt") {
		setDefined(&normalized, "started_at", turn.Get("startedAt"))
	}
	if turn.Has("completedAt") {
		setDefined(&normalized, "completed_at", turn.Get("completedAt"))
	}
	if c.lastTokenUsage.Len() > 0 {
		normalized.Set("tokenUsage", c.lastTokenUsage)
		total := c.lastTokenUsage.Get("total")
		setDefined(&normalized, "input_tokens", total.Get("inputTokens"))
		setDefined(&normalized, "output_tokens", total.Get("outputTokens"))
		setDefined(&normalized, "total_tokens", total.Get("totalTokens"))
		setDefined(&normalized, "context_window", c.lastTokenUsage.Get("modelContextWindow"))
	}
	c.emit(normalized)
}

func (c *codexState) emitUsage(params qtjson.Value) {
	normalized := params.Clone()
	if !normalized.IsObject() {
		normalized = qtjson.NewObject()
	}
	normalized.Set("type", qtjson.NewString("_codex_usage"))
	if tokenUsage := params.Get("tokenUsage"); tokenUsage.IsObject() && tokenUsage.Len() > 0 {
		c.lastTokenUsage = tokenUsage
		total := tokenUsage.Get("total")
		setDefined(&normalized, "input_tokens", total.Get("inputTokens"))
		setDefined(&normalized, "output_tokens", total.Get("outputTokens"))
		setDefined(&normalized, "total_tokens", total.Get("totalTokens"))
		setDefined(&normalized, "context_window", tokenUsage.Get("modelContextWindow"))
	}
	c.emit(normalized)
}

func (c *codexState) interrupt() {
	if c.running() && c.turnActive && c.threadID != "" && c.turnID != "" {
		c.sendRequest("turn/interrupt", qtjson.ObjectOf("threadId", c.threadID, "turnId", c.turnID))
	}
}

func (c *codexState) setTurnOptions(model, mode, effort string) {
	if model = trimmed(model); model != "" {
		c.model = model
		c.effectiveModel = model
	}
	c.mode = mode
	c.effort = trimmed(effort)
}

func (c *codexState) exited() {
	frames, errs := c.r.framer.Append(nil, true)
	for _, message := range errs {
		c.r.sendLocked(map[string]any{"key": c.r.key, "stderr": message})
		c.lineIndex = -1
		c.emitLocalNotice("error", message)
	}
	for _, frame := range frames {
		c.handleLine(frame, c.r.next)
	}
	c.persist()
}

func (c *codexState) describe(state map[string]any) {
	state["threadId"] = c.threadID
	state["turnId"] = c.turnID
	state["turnActive"] = c.turnActive
	state["threadReady"] = c.threadReady
	state["mode"] = c.mode
	state["model"] = c.effectiveModel
	pending := []string{}
	for token := range c.pendingServer {
		pending = append(pending, token)
	}
	sort.Strings(pending)
	state["pendingRequests"] = pending
}

func (c *codexState) resendPermissions(send func(payload map[string]any)) {
	tokens := make([]string, 0, len(c.pendingPermissions))
	for token := range c.pendingPermissions {
		tokens = append(tokens, token)
	}
	sort.Strings(tokens)
	for _, token := range tokens {
		send(map[string]any{"key": c.r.key, "line": string(c.pendingPermissions[token].Compact()), "transient": true})
	}
}
