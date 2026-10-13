package agentrun

import (
	"regexp"
	"strconv"
	"strings"

	"forkmesh.com/desktop-core/internal/qtjson"
)

const taskResultLimit = 1500

var (
	taskBlockPattern  = regexp.MustCompile(`<task-notification>([\s\S]*?)(?:</task-notification>|\n?\z)`)
	taskStatusPattern = regexp.MustCompile(`^[a-z_]{1,20}$`)
)

func replyLine(text string) string {
	t := trimmed(text)
	if t == "" {
		return ""
	}
	return "\n" + t + "\n"
}

func isTaskNotification(text string) bool {
	return strings.HasPrefix(trimmed(text), "<task-notification>")
}

func unescapeEntities(text string) string {
	text = strings.ReplaceAll(text, "&lt;", "<")
	text = strings.ReplaceAll(text, "&gt;", ">")
	text = strings.ReplaceAll(text, "&quot;", "\"")
	text = strings.ReplaceAll(text, "&#39;", "'")
	text = strings.ReplaceAll(text, "&apos;", "'")
	return strings.ReplaceAll(text, "&amp;", "&")
}

func tagText(xml, tag string) string {
	open := "<" + tag + ">"
	closing := "</" + tag + ">"
	start := strings.Index(xml, open)
	if start < 0 {
		return ""
	}
	from := start + len(open)
	end := strings.Index(xml[from:], closing)
	if end < 0 {
		return unescapeEntities(xml[from:])
	}
	return unescapeEntities(xml[from : from+end])
}

func indexU16(u []uint16, c uint16) int {
	for i, v := range u {
		if v == c {
			return i
		}
	}
	return -1
}

func lastIndexU16(u []uint16, c uint16, from int) int {
	if from >= len(u) {
		from = len(u) - 1
	}
	for i := from; i >= 0; i-- {
		if u[i] == c {
			return i
		}
	}
	return -1
}

func chopSpaces(u []uint16) []uint16 {
	for len(u) > 0 && isSpace16(u[len(u)-1]) {
		u = u[:len(u)-1]
	}
	return u
}

func clippedTaskResult(raw string) string {
	text := toU16(strings.ReplaceAll(raw, "\r\n", "\n"))
	for newline := indexU16(text, '\n'); newline >= 0 && trimmed(fromU16(text[:newline])) == ""; newline = indexU16(text, '\n') {
		text = text[newline+1:]
	}
	text = chopSpaces(text)
	if len(text) <= taskResultLimit {
		return fromU16(text)
	}
	cut := lastIndexU16(text, '\n', taskResultLimit)
	if cut < taskResultLimit*2/3 {
		cut = taskResultLimit
		if c := text[cut-1]; c >= 0xD800 && c <= 0xDBFF {
			cut--
		}
	}
	head := chopSpaces(append([]uint16(nil), text[:cut]...))
	headText := fromU16(head)
	fences := 0
	for _, line := range strings.Split(headText, "\n") {
		if strings.HasPrefix(trimmed(line), "```") {
			fences++
		}
	}
	if fences%2 == 1 {
		headText += "\n```"
	}
	return headText + "\n\n…more (" + strconv.Itoa(len(text)-cut) + " characters not shown)"
}

func taskStatus(xml string) string {
	status := strings.ToLower(trimmed(tagText(xml, "status")))
	if status == "" || status == "cost" || !taskStatusPattern.MatchString(status) {
		return "update"
	}
	return status
}

func taskBlock(xml string) string {
	summary := simplified(tagText(xml, "summary"))
	if summary == "" {
		summary = "Background task"
	}
	body := tagText(xml, "result")
	if trimmed(body) == "" {
		body = tagText(xml, "event")
	}
	out := "\n==> Task " + taskStatus(xml) + ": " + summary + "\n"
	if trimmed(body) == "" {
		return out
	}
	for _, line := range strings.Split(clippedTaskResult(body), "\n") {
		if trimmed(line) == "" {
			out += "\n"
		} else {
			out += "    " + line + "\n"
		}
	}
	return out
}

func TaskNotification(text string) string {
	out := ""
	for _, match := range taskBlockPattern.FindAllStringSubmatch(text, -1) {
		out += taskBlock(match[1])
	}
	return out
}

func contentText(content qtjson.Value) string {
	if content.IsString() {
		return content.Str()
	}
	texts := []string{}
	for _, value := range content.Items() {
		kind := value.Get("type").Str()
		if kind == "text" || kind == "input_text" {
			texts = append(texts, value.Get("text").Str())
		}
	}
	return strings.Join(texts, "\n")
}

func shellCommand(command qtjson.Value) string {
	if command.IsString() {
		return command.Str()
	}
	parts := []string{}
	for _, part := range command.Items() {
		parts = append(parts, part.Str())
	}
	if len(parts) == 3 && strings.HasPrefix(parts[1], "-") && strings.Contains(parts[1], "c") {
		return parts[2]
	}
	return strings.Join(parts, " ")
}

func ToolLine(name string, input qtjson.Value) string {
	if !input.IsObject() {
		input = qtjson.NewObject()
	}
	arg := input.Get("file_path").Str()
	if arg == "" {
		arg = shellCommand(input.Get("command"))
	}
	if arg == "" {
		arg = input.Get("path").Str()
	}
	if arg == "" {
		arg = input.Get("pattern").Str()
	}
	if arg == "" && input.Len() > 0 {
		arg = string(input.Compact())
	}
	if len16(arg) > 200 {
		arg = left16(arg, 200) + "…"
	}
	if arg == "" {
		return "\n[" + name + "]\n"
	}
	return "\n[" + name + " " + arg + "]\n"
}

func AssistantContent(message qtjson.Value) string {
	log := ""
	for _, block := range message.Get("content").Items() {
		switch block.Get("type").Str() {
		case "text", "output_text":
			log += replyLine(block.Get("text").Str())
		case "tool_use":
			log += ToolLine(block.Get("name").Str(), block.Get("input"))
		}
	}
	return log
}

func UserEvent(event qtjson.Value) string {
	if event.Get("isSidechain").BoolValue() {
		return ""
	}
	if parent := event.Get("parent_tool_use_id"); parent.IsString() && parent.Str() != "" {
		return ""
	}
	text := contentText(event.Get("message").Get("content"))
	if isTaskNotification(text) {
		return TaskNotification(text)
	}
	return ""
}

func LogText(event qtjson.Value) string {
	switch event.Get("type").Str() {
	case "_local_user":
		if t := trimmed(event.Get("text").Str()); t != "" {
			return "\n> " + t + "\n"
		}
	case "_codex_agent_complete":
		if t := trimmed(event.Get("text").Str()); t != "" {
			return "\n" + t + "\n"
		}
	case "assistant":
		return AssistantContent(event.Get("message"))
	case "user":
		return UserEvent(event)
	}
	return ""
}
