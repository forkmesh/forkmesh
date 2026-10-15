package agentmodel

import (
	"bytes"
	"io"
	"os"
	"regexp"
	"strings"

	"forkmesh.com/desktop-core/internal/agentrun"
	"forkmesh.com/desktop-core/internal/qtjson"
)

const maxOutsideTranscriptBytes = 2 * 1024 * 1024

var (
	injectedPrefixes = []string{"<command-", "<local-command-", "<bash-", "<system-reminder>",
		"<user_instructions>", "<environment_context>", "Caveat: "}
	peerEnvelope = regexp.MustCompile(`<cross-session-message[^>]*>\n([\s\S]*)\n</cross-session-message>`)
)

const peerSteerLead = "Your user typed this in the ForkMesh app on their phone, on this session's own page, as their next message to you in this conversation. ForkMesh can only reach a terminal session through cross-session messaging, so it arrives that way, but it was not sent by another session."

func promptLine(text string) string {
	t := trimmed(text)
	if t == "" {
		return ""
	}
	return "\n> " + t + "\n"
}

func injectedPrompt(text string) bool {
	t := trimmed(text)
	for _, prefix := range injectedPrefixes {
		if strings.HasPrefix(t, prefix) {
			return true
		}
	}
	return false
}

func isTaskNotification(text string) bool {
	return strings.HasPrefix(trimmed(text), "<task-notification>")
}

func userContent(content qtjson.Value) string {
	if content.IsString() {
		text := content.Str()
		if isTaskNotification(text) {
			return agentrun.TaskNotification(text)
		}
		if injectedPrompt(text) {
			return ""
		}
		return promptLine(text)
	}
	texts := []string{}
	tasks := ""
	for _, block := range content.Items() {
		kind := block.Get("type").Str()
		if kind != "text" && kind != "input_text" {
			continue
		}
		text := block.Get("text").Str()
		switch {
		case isTaskNotification(text):
			tasks += agentrun.TaskNotification(text)
		case !injectedPrompt(text):
			if t := trimmed(text); t != "" {
				texts = append(texts, t)
			}
		}
	}
	return promptLine(strings.Join(texts, "\n")) + tasks
}

func peerPrompt(entry qtjson.Value) string {
	origin := entry.Get("origin")
	body := origin.Get("body").Str()
	if body == "" {
		content := entry.Get("message").Get("content")
		if match := peerEnvelope.FindStringSubmatch(content.Str()); match != nil {
			body = match[1]
		}
	}
	body = trimmed(body)
	if rest, ok := strings.CutPrefix(body, peerSteerLead); ok {
		body = trimmed(rest)
	}
	if body == "" {
		return ""
	}
	name := simplified(origin.Get("name").Str())
	if name == "" {
		return promptLine(body)
	}
	return promptLine("⟵ " + name + ": " + body)
}

func claudeEntry(entry qtjson.Value) string {
	if !entry.Get("isSidechain").BoolValue() && entry.Get("type").Str() == "user" &&
		entry.Get("origin").Get("kind").Str() == "peer" {
		return peerPrompt(entry)
	}
	if entry.Get("isSidechain").BoolValue() || entry.Get("isMeta").BoolValue() {
		return ""
	}
	kind := entry.Get("type").Str()
	message := entry.Get("message")
	switch {
	case kind == "user":
		return userContent(message.Get("content"))
	case kind == "assistant":
		return agentrun.AssistantContent(message)
	case kind == "system" && entry.Get("subtype").Str() == "compact_boundary":
		return "\n==> Conversation compacted\n"
	}
	return ""
}

func codexEntry(entry qtjson.Value) string {
	wrapped := entry.Has("payload")
	item := entry
	if wrapped {
		item = entry.Get("payload")
		if !item.IsObject() {
			item = qtjson.NewObject()
		}
	}
	outer := entry.Get("type").Str()
	kind := item.Get("type").Str()
	if wrapped && outer == "event_msg" {
		if kind == "user_message" {
			if injectedPrompt(item.Get("message").Str()) {
				return ""
			}
			return promptLine(item.Get("message").Str())
		}
		return ""
	}
	if wrapped && outer != "response_item" {
		return ""
	}
	switch kind {
	case "message":
		role := item.Get("role").Str()
		if role == "assistant" {
			return agentrun.AssistantContent(item)
		}
		if role == "user" && !wrapped {
			return userContent(item.Get("content"))
		}
		return ""
	case "function_call":
		input, ok := qtjson.ParseObject([]byte(item.Get("arguments").Str()))
		if !ok {
			input = qtjson.NewObject()
		}
		return agentrun.ToolLine(item.Get("name").Str(), input)
	case "local_shell_call":
		command := item.Get("action").Get("command")
		input := qtjson.NewObject()
		if !command.IsUndefined() {
			input.Set("command", command)
		}
		return agentrun.ToolLine("shell", input)
	case "custom_tool_call":
		input := item.Get("input").Str()
		name := item.Get("name").Str()
		first := trimmed(strings.SplitN(input, "\n", 2)[0])
		for _, line := range strings.Split(input, "\n") {
			if strings.HasPrefix(line, "*** ") && !strings.HasPrefix(line, "*** Begin") {
				first = trimmed(line[4:])
				break
			}
		}
		if first == "" {
			return agentrun.ToolLine(name, qtjson.NewObject())
		}
		return agentrun.ToolLine(name, qtjson.ObjectOf("path", first))
	}
	return ""
}

func outsideEntry(kind string, entry qtjson.Value) string {
	switch kind {
	case "claude":
		return claudeEntry(entry)
	case "codex":
		return codexEntry(entry)
	}
	return ""
}

func readsTranscript(kind string) bool {
	return kind == "claude" || kind == "codex"
}

var machineLabels = map[string]string{"claude": "Claude Code", "codex": "Codex", "gemini": "Gemini CLI",
	"opencode": "opencode", "aider": "Aider", "goose": "Goose", "amp": "Amp", "crush": "Crush",
	"cursor-agent": "Cursor Agent", "copilot": "Copilot CLI"}

func outsideLabel(kind string) string {
	if label, ok := machineLabels[kind]; ok {
		return label
	}
	return kind
}

func readJSONLines(path string, from int64) ([]qtjson.Value, int64) {
	file, err := os.Open(path)
	if err != nil {
		return nil, from
	}
	defer file.Close()
	info, err := file.Stat()
	if err != nil {
		return nil, from
	}
	if from < 0 || from > info.Size() {
		from = 0
	}
	if _, err := file.Seek(from, io.SeekStart); err != nil {
		return nil, from
	}
	buf, _ := io.ReadAll(file)
	out := []qtjson.Value{}
	consumed := 0
	start := 0
	for {
		nl := bytes.IndexByte(buf[start:], '\n')
		if nl < 0 {
			break
		}
		line := buf[start : start+nl]
		consumed = start + nl + 1
		start = consumed
		if len(bytes.TrimSpace(line)) == 0 {
			continue
		}
		if object, ok := qtjson.ParseObject(line); ok && object.Len() > 0 {
			out = append(out, object)
		}
	}
	return out, from + int64(consumed)
}

func tailStartOffset(path string, maxBytes int64) int64 {
	file, err := os.Open(path)
	if err != nil {
		return 0
	}
	defer file.Close()
	info, err := file.Stat()
	if err != nil {
		return 0
	}
	size := info.Size()
	if size <= maxBytes {
		return 0
	}
	off := size - maxBytes
	if _, err := file.Seek(off, io.SeekStart); err != nil {
		return off
	}
	probe := make([]byte, min(maxBytes, 1<<20))
	n, _ := io.ReadFull(file, probe)
	if nl := bytes.IndexByte(probe[:n], '\n'); nl >= 0 {
		off += int64(nl) + 1
	}
	return off
}

func fileSize(path string) int64 {
	info, err := os.Stat(path)
	if err != nil {
		return 0
	}
	return info.Size()
}
