package insights

import (
	"regexp"
	"sort"
	"strings"

	"forkmesh.com/desktop-core/internal/qtjson"
)

const (
	AvailabilityNative  = "native"
	AvailabilityPackage = "package"
	AvailabilityPlanned = "planned"

	BaselineNone         = "none"
	BaselinePartialRead  = "partial-read"
	BaselineAnchoredEdit = "anchored-edit"
	BaselineMatchedFiles = "matched-files"

	DisabledFamiliesSetting = "agents/disabledToolFamilies"
)

type Family struct {
	ID               string
	DisplayName      string
	Category         string
	Summary          string
	Availability     string
	AvailabilityNote string
	Baseline         string
	ExpectedSavings  string
	Hidden           bool
}

var families = []Family{
	{"read", "Read", "Files", "Bounded reads of files, ranges, notebooks, images, and URLs", AvailabilityNative, "Claude Read, NotebookRead, WebFetch; Codex image view", BaselinePartialRead, "50–95% input", false},
	{"write", "Write", "Files", "Create or overwrite a whole file", AvailabilityNative, "Claude Write", BaselineNone, "0–15%", false},
	{"edit", "Edit", "Files", "Anchored patches instead of whole-file rewrites", AvailabilityNative, "Claude Edit, MultiEdit, NotebookEdit; Codex file changes", BaselineAnchoredEdit, "20–61% output", false},
	{"ast_edit", "Structural edit", "Files", "Previewed structural rewrites via tree-sitter rules", AvailabilityPackage, "ast-grep MCP package", BaselineAnchoredEdit, "50–95% output on bulk edits", false},
	{"grep", "Grep", "Search", "Regex over files, returning only matching lines", AvailabilityNative, "Claude Grep", BaselineMatchedFiles, "70–99% input", false},
	{"glob", "Glob", "Search", "Path lookup by glob pattern", AvailabilityNative, "Claude Glob", BaselineNone, "60–95% input", false},
	{"ast_grep", "Structural search", "Search", "Structural code queries over tree-sitter grammars", AvailabilityPackage, "ast-grep MCP package", BaselineMatchedFiles, "60–98% input", false},
	{"bash", "Shell", "Runtime", "Workspace shell with bounded output and background jobs", AvailabilityNative, "Claude Bash, BashOutput, KillShell; Codex command execution", BaselineNone, "10–70%", false},
	{"eval", "Eval kernel", "Runtime", "Persistent Python and JavaScript cells", AvailabilityPackage, "Kernel MCP package", BaselineNone, "20–80%", false},
	{"lsp", "Language service", "Intelligence", "Diagnostics, navigation, symbols, and renames", AvailabilityPackage, "Language-server MCP package", BaselineNone, "50–95% input", false},
	{"debug", "Debugger", "Intelligence", "Breakpoints, stepping, stacks, and variables over DAP", AvailabilityPackage, "Debug-adapter MCP package", BaselineNone, "0–60%", false},
	{"security_scan", "Security scan", "Intelligence", "Structured security review findings", AvailabilityPackage, "Codex Security plugin", BaselineNone, "0–40%", false},
	{"task", "Subagents", "Coordination", "Fan out typed subagents, optionally isolated", AvailabilityNative, "Claude Task; Codex collaboration and sub-agent activity", BaselineNone, "−150% to +30% total", false},
	{"hub", "Agent hub", "Coordination", "Message, wait on, or cancel live agents and jobs", AvailabilityNative, "Claude SendMessage, ListAgents, TaskOutput, TaskStop, Monitor, ScheduleWakeup, Cron; Codex sleep", BaselineNone, "10–60% parent context", false},
	{"todo", "Todo", "Coordination", "Ordered task list with phase tracking", AvailabilityNative, "Claude TodoWrite, plan mode; Codex plan", BaselineNone, "0–20%", false},
	{"ask", "Ask", "Coordination", "Structured follow-up questions", AvailabilityNative, "Claude AskUserQuestion", BaselineNone, "0–40%", false},
	{"web_search", "Web search", "Web", "One query across providers with citations", AvailabilityNative, "Claude WebSearch; Codex web search", BaselineNone, "40–85% input", false},
	{"github", "GitHub", "Web", "Repositories, pulls, issues, code search, Actions", AvailabilityPackage, "GitHub plugin or gh MCP server", BaselineNone, "40–90% input", false},
	{"browser", "Browser", "Web", "Isolated browser tabs and structured page state", AvailabilityPackage, "Browser automation package", BaselineNone, "0–60%", false},
	{"computer", "Desktop control", "Web", "Windows, screenshots, native input, accessibility tree", AvailabilityPackage, "Desktop-control package", BaselineNone, "−100% to +20%", false},
	{"generate_image", "Generate image", "Media", "Generate or edit raster images", AvailabilityNative, "Codex image generation", BaselineNone, "−100% to 0%", false},
	{"tts", "Speech", "Media", "Text-to-speech output", AvailabilityPackage, "Voice package", BaselineNone, "−50% to 0%", false},
	{"checkpoint", "Checkpoint", "Context", "Mark conversation state for a later collapse", AvailabilityPlanned, "Host-side session checkpoints", BaselineNone, "20–60% on later turns", false},
	{"rewind", "Rewind", "Context", "Prune exploratory context, keep a concise report", AvailabilityPlanned, "Host-side session rewind", BaselineNone, "30–80% after exploration", false},
	{"context_notes", "Context notes", "Context", "Pin durable notes that survive compaction", AvailabilityPlanned, "Host-side pinned notes", BaselineNone, "5–25%", false},
	{"new_context", "New context", "Context", "Start a fresh context with a carried summary", AvailabilityPlanned, "Host-side compaction handoff", BaselineNone, "20–60%", false},
	{"retain", "Retain", "Memory", "Queue durable facts into the memory bank", AvailabilityPackage, "Memory MCP package", BaselineNone, "5–30% across sessions", false},
	{"recall", "Recall", "Memory", "Search the memory bank for raw memories", AvailabilityPackage, "Memory MCP package", BaselineNone, "20–70% input", false},
	{"reflect", "Reflect", "Memory", "Synthesize an answer over stored memories", AvailabilityPackage, "Memory MCP package", BaselineNone, "20–60% input", false},
	{"memory_edit", "Memory edit", "Memory", "Update, forget, or invalidate memories", AvailabilityPackage, "Memory MCP package", BaselineNone, "0–20%", false},
	{"learn", "Learn", "Skills", "Capture a reusable lesson as a skill", AvailabilityPlanned, "Skill capture from transcripts", BaselineNone, "10–40% across repeated tasks", false},
	{"manage_skill", "Skills", "Skills", "Invoke, create, update, or delete managed skills", AvailabilityNative, "Claude Skill; ForkMesh installed skills", BaselineNone, "5–30% across repeated tasks", false},
	{"yield", "Yield", "Control", "Hand control back while background work continues", AvailabilityNative, "Implicit in background job delivery", BaselineNone, "10–50% parent context", true},
	{"goal", "Goal", "Control", "Persistent objective with budget and completion", AvailabilityPlanned, "Host-side goal mode", BaselineNone, "5–25% on long autonomous work", true},
	{"think", "Think", "Control", "Explicit reasoning step", AvailabilityNative, "Streamed reasoning deltas", BaselineNone, "0–15%", true},
	{"mcp", "MCP tools", "Extension", "Extension tools that match no family above", AvailabilityNative, "Any MCP server; Claude ToolSearch", BaselineNone, "Depends on package", false},
	{"other", "Other", "Extension", "Runtime tools ForkMesh does not classify yet", AvailabilityNative, "", BaselineNone, "", false},
}

var familyIndex = func() map[string]int {
	index := map[string]int{}
	for i, family := range families {
		index[family.ID] = i
	}
	return index
}()

func Families() []Family { return families }

func FamilyByID(id string) *Family {
	if i, ok := familyIndex[id]; ok {
		return &families[i]
	}
	return nil
}

func classifyExtensionTool(server, tool string) string {
	probe := strings.ToLower(server + " " + tool)
	has := func(needles ...string) bool {
		for _, needle := range needles {
			if strings.Contains(probe, needle) {
				return true
			}
		}
		return false
	}
	switch {
	case has("github", "gh_"):
		return "github"
	case has("playwright", "puppeteer", "browser", "chrome", "chromium"):
		return "browser"
	case has("computer", "desktop", "screen", "xdotool"):
		return "computer"
	case has("ast_edit", "ast-edit", "astedit"):
		return "ast_edit"
	case has("ast_grep", "ast-grep", "astgrep", "sgrep"):
		return "ast_grep"
	case has("lsp", "language_server", "language-server", "diagnostics", "goto_definition", "find_references"):
		return "lsp"
	case has("debug", "dap_", "breakpoint"):
		return "debug"
	case has("security", "semgrep", "codeql", "scan"):
		return "security_scan"
	case has("jupyter", "kernel", "eval", "execute_code", "python_exec", "repl"):
		return "eval"
	case has("tts", "speech", "speak", "voice"):
		return "tts"
	case has("generate_image", "image_gen", "imagegen", "dalle"):
		return "generate_image"
	case has("memory_edit", "forget_memory", "delete_memory"):
		return "memory_edit"
	case has("reflect"):
		return "reflect"
	case has("recall", "search_memory", "memory_search"):
		return "recall"
	case has("retain", "remember", "store_memory", "add_memory"):
		return "retain"
	case has("web_search", "websearch", "search_web", "brave", "tavily", "exa_"):
		return "web_search"
	case has("fetch", "read_url"):
		return "read"
	case has("checkpoint"):
		return "checkpoint"
	case has("rewind"):
		return "rewind"
	case has("skill"):
		return "manage_skill"
	}
	return "mcp"
}

var exactFamilies = map[string]string{
	"read": "read", "notebookread": "read", "webfetch": "read",
	"write": "write",
	"edit":  "edit", "multiedit": "edit", "notebookedit": "edit", "filechange": "edit", "apply_patch": "edit",
	"grep": "grep", "search": "grep",
	"glob": "glob", "find": "glob", "ls": "glob",
	"bash": "bash", "shell": "bash", "bashoutput": "bash", "killshell": "bash", "killbash": "bash", "codextool": "bash",
	"task": "task", "agent": "task", "workflow": "task",
	"sendmessage": "hub", "listagents": "hub", "taskoutput": "hub", "taskstop": "hub", "monitor": "hub",
	"sleep": "hub", "schedulewakeup": "hub", "croncreate": "hub", "crondelete": "hub", "cronlist": "hub",
	"toolsearch": "mcp",
	"todowrite":  "todo", "todoread": "todo", "exitplanmode": "todo", "enterplanmode": "todo",
	"askuserquestion": "ask",
	"websearch":       "web_search",
	"imagegeneration": "generate_image",
	"skill":           "manage_skill",
	"computer":        "computer", "computer_use": "computer",
	"lsp": "lsp", "debug": "debug", "eval": "eval", "ast_grep": "ast_grep", "ast_edit": "ast_edit",
	"security_scan": "security_scan", "hub": "hub", "todo": "todo", "ask": "ask", "browser": "browser",
	"web_search": "web_search", "github": "github", "generate_image": "generate_image", "tts": "tts",
	"checkpoint": "checkpoint", "rewind": "rewind", "context_notes": "context_notes", "new_context": "new_context",
	"retain": "retain", "recall": "recall", "reflect": "reflect", "memory_edit": "memory_edit", "learn": "learn",
	"manage_skill": "manage_skill", "yield": "yield", "goal": "goal", "think": "think",
}

func FamilyFor(runtimeToolName string, input qtjson.Value) string {
	raw := trimmed(runtimeToolName)
	if raw == "" {
		return "other"
	}
	if strings.HasPrefix(raw, "mcp__") {
		parts := strings.Split(raw[5:], "__")
		server := parts[0]
		return classifyExtensionTool(server, strings.Join(parts[1:], "_"))
	}
	if split := strings.Index(raw, "::"); split >= 0 {
		return classifyExtensionTool(raw[:split], raw[split+2:])
	}
	key := strings.ToLower(raw)
	if family, ok := exactFamilies[key]; ok && family != "" {
		return family
	}
	if ns := input.Get("namespace").Str(); ns != "" {
		return classifyExtensionTool(ns, raw)
	}
	if strings.HasPrefix(key, "dynamictool") {
		return "mcp"
	}
	return "other"
}

var claudeToolNames = map[string][]string{
	"read":         {"Read", "NotebookRead", "WebFetch"},
	"write":        {"Write"},
	"edit":         {"Edit", "MultiEdit", "NotebookEdit"},
	"grep":         {"Grep"},
	"glob":         {"Glob"},
	"bash":         {"Bash", "BashOutput", "KillShell", "KillBash"},
	"task":         {"Task", "Agent"},
	"hub":          {"SendMessage", "ListAgents", "TaskOutput", "TaskStop", "Monitor", "ScheduleWakeup", "CronCreate", "CronDelete", "CronList"},
	"todo":         {"TodoWrite", "TodoRead"},
	"ask":          {"AskUserQuestion"},
	"web_search":   {"WebSearch"},
	"manage_skill": {"Skill"},
	"computer":     {"Computer"},
}

var codexOverrides = map[string][]string{
	"web_search": {`web_search="disabled"`, "tools.web_search=false"},
	"read":       {"tools.view_image=false"},
}

func NormalizeDisabledFamilies(raw []string) []string {
	out := []string{}
	seen := map[string]bool{}
	for _, value := range raw {
		id := strings.ToLower(trimmed(value))
		if id == "" || FamilyByID(id) == nil || seen[id] {
			continue
		}
		seen[id] = true
		out = append(out, id)
	}
	sort.Strings(out)
	return out
}

var safeClaudeName = regexp.MustCompile(`^[A-Za-z][A-Za-z0-9_]*$`)

func ClaudeDisallowedTools(disabled []string) []string {
	tools := []string{}
	seen := map[string]bool{}
	for _, family := range NormalizeDisabledFamilies(disabled) {
		for _, name := range claudeToolNames[family] {
			if safeClaudeName.MatchString(name) && !seen[name] {
				seen[name] = true
				tools = append(tools, name)
			}
		}
	}
	return tools
}

func EnforcementNote(family string) string {
	parts := []string{}
	if claude := claudeToolNames[family]; len(claude) > 0 {
		parts = append(parts, "Claude Code launches with --disallowedTools "+strings.Join(claude, ", "))
	}
	if codex := codexOverrides[family]; len(codex) > 0 {
		parts = append(parts, "Codex launches with -c "+strings.Join(codex, " -c "))
	}
	if family == "bash" {
		parts = append(parts, "Codex shell approvals are denied by the permission broker")
	}
	if len(parts) == 0 {
		return "No runtime switch exists for this family yet; calls made while it is off are counted so the gap stays visible."
	}
	return strings.Join(parts, "; ") + ". Takes effect on the next session launch or resume."
}
