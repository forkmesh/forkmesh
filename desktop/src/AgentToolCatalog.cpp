#include "AgentToolCatalog.h"

#include <QHash>
#include <QRegularExpression>

namespace {

using namespace AgentToolAvailability;
using namespace AgentToolBaseline;

QVector<AgentToolFamily> buildFamilies()
{
    // Order is the display order inside each category. Expected savings are
    // the planning estimates per applicable operation; the board replaces
    // them with measured numbers as soon as a family has been observed.
    return {
        // Files
        {QStringLiteral("read"), QStringLiteral("Read"), QStringLiteral("Files"),
         QStringLiteral("Bounded reads of files, ranges, notebooks, images, "
                        "and URLs"),
         Native, QStringLiteral("Claude Read, NotebookRead, WebFetch; Codex "
                                "image view"),
         PartialRead, QStringLiteral("50–95% input")},
        {QStringLiteral("write"), QStringLiteral("Write"),
         QStringLiteral("Files"),
         QStringLiteral("Create or overwrite a whole file"), Native,
         QStringLiteral("Claude Write"), None, QStringLiteral("0–15%")},
        {QStringLiteral("edit"), QStringLiteral("Edit"), QStringLiteral("Files"),
         QStringLiteral("Anchored patches instead of whole-file rewrites"),
         Native,
         QStringLiteral("Claude Edit, MultiEdit, NotebookEdit; Codex file "
                        "changes"),
         AnchoredEdit, QStringLiteral("20–61% output")},
        {QStringLiteral("ast_edit"), QStringLiteral("Structural edit"),
         QStringLiteral("Files"),
         QStringLiteral("Previewed structural rewrites via tree-sitter rules"),
         Package, QStringLiteral("ast-grep MCP package"), AnchoredEdit,
         QStringLiteral("50–95% output on bulk edits")},

        // Search
        {QStringLiteral("grep"), QStringLiteral("Grep"), QStringLiteral("Search"),
         QStringLiteral("Regex over files, returning only matching lines"),
         Native, QStringLiteral("Claude Grep"), MatchedFiles,
         QStringLiteral("70–99% input")},
        {QStringLiteral("glob"), QStringLiteral("Glob"), QStringLiteral("Search"),
         QStringLiteral("Path lookup by glob pattern"), Native,
         QStringLiteral("Claude Glob"), None, QStringLiteral("60–95% input")},
        {QStringLiteral("ast_grep"), QStringLiteral("Structural search"),
         QStringLiteral("Search"),
         QStringLiteral("Structural code queries over tree-sitter grammars"),
         Package, QStringLiteral("ast-grep MCP package"), MatchedFiles,
         QStringLiteral("60–98% input")},

        // Runtime
        {QStringLiteral("bash"), QStringLiteral("Shell"), QStringLiteral("Runtime"),
         QStringLiteral("Workspace shell with bounded output and background "
                        "jobs"),
         Native, QStringLiteral("Claude Bash, BashOutput, KillShell; Codex "
                                "command execution"),
         None, QStringLiteral("10–70%")},
        {QStringLiteral("eval"), QStringLiteral("Eval kernel"),
         QStringLiteral("Runtime"),
         QStringLiteral("Persistent Python and JavaScript cells"), Package,
         QStringLiteral("Kernel MCP package"), None,
         QStringLiteral("20–80%")},

        // Intelligence
        {QStringLiteral("lsp"), QStringLiteral("Language service"),
         QStringLiteral("Intelligence"),
         QStringLiteral("Diagnostics, navigation, symbols, and renames"),
         Package, QStringLiteral("Language-server MCP package"), None,
         QStringLiteral("50–95% input")},
        {QStringLiteral("debug"), QStringLiteral("Debugger"),
         QStringLiteral("Intelligence"),
         QStringLiteral("Breakpoints, stepping, stacks, and variables over "
                        "DAP"),
         Package, QStringLiteral("Debug-adapter MCP package"), None,
         QStringLiteral("0–60%")},
        {QStringLiteral("security_scan"), QStringLiteral("Security scan"),
         QStringLiteral("Intelligence"),
         QStringLiteral("Structured security review findings"), Package,
         QStringLiteral("Codex Security plugin"), None,
         QStringLiteral("0–40%")},

        // Coordination
        {QStringLiteral("task"), QStringLiteral("Subagents"),
         QStringLiteral("Coordination"),
         QStringLiteral("Fan out typed subagents, optionally isolated"),
         Native, QStringLiteral("Claude Task; Codex collaboration and "
                                "sub-agent activity"),
         None, QStringLiteral("−150% to +30% total")},
        {QStringLiteral("hub"), QStringLiteral("Agent hub"),
         QStringLiteral("Coordination"),
         QStringLiteral("Message, wait on, or cancel live agents and jobs"),
         Native, QStringLiteral("Claude SendMessage, ListAgents, TaskOutput, "
                                "TaskStop, Monitor, ScheduleWakeup, Cron; "
                                "Codex sleep"),
         None, QStringLiteral("10–60% parent context")},
        {QStringLiteral("todo"), QStringLiteral("Todo"),
         QStringLiteral("Coordination"),
         QStringLiteral("Ordered task list with phase tracking"), Native,
         QStringLiteral("Claude TodoWrite, plan mode; Codex plan"), None,
         QStringLiteral("0–20%")},
        {QStringLiteral("ask"), QStringLiteral("Ask"),
         QStringLiteral("Coordination"),
         QStringLiteral("Structured follow-up questions"), Native,
         QStringLiteral("Claude AskUserQuestion"), None,
         QStringLiteral("0–40%")},

        // Web and forge
        {QStringLiteral("web_search"), QStringLiteral("Web search"),
         QStringLiteral("Web"),
         QStringLiteral("One query across providers with citations"), Native,
         QStringLiteral("Claude WebSearch; Codex web search"), None,
         QStringLiteral("40–85% input")},
        {QStringLiteral("github"), QStringLiteral("GitHub"), QStringLiteral("Web"),
         QStringLiteral("Repositories, pulls, issues, code search, Actions"),
         Package, QStringLiteral("GitHub plugin or gh MCP server"), None,
         QStringLiteral("40–90% input")},
        {QStringLiteral("browser"), QStringLiteral("Browser"),
         QStringLiteral("Web"),
         QStringLiteral("Isolated browser tabs and structured page state"),
         Package, QStringLiteral("Browser automation package"), None,
         QStringLiteral("0–60%")},
        {QStringLiteral("computer"), QStringLiteral("Desktop control"),
         QStringLiteral("Web"),
         QStringLiteral("Windows, screenshots, native input, accessibility "
                        "tree"),
         Package, QStringLiteral("Desktop-control package"), None,
         QStringLiteral("−100% to +20%")},

        // Media
        {QStringLiteral("generate_image"), QStringLiteral("Generate image"),
         QStringLiteral("Media"),
         QStringLiteral("Generate or edit raster images"), Native,
         QStringLiteral("Codex image generation"), None,
         QStringLiteral("−100% to 0%")},
        {QStringLiteral("tts"), QStringLiteral("Speech"), QStringLiteral("Media"),
         QStringLiteral("Text-to-speech output"), Package,
         QStringLiteral("Voice package"), None, QStringLiteral("−50% to 0%")},

        // Context
        {QStringLiteral("checkpoint"), QStringLiteral("Checkpoint"),
         QStringLiteral("Context"),
         QStringLiteral("Mark conversation state for a later collapse"),
         Planned, QStringLiteral("Host-side session checkpoints"), None,
         QStringLiteral("20–60% on later turns")},
        {QStringLiteral("rewind"), QStringLiteral("Rewind"),
         QStringLiteral("Context"),
         QStringLiteral("Prune exploratory context, keep a concise report"),
         Planned, QStringLiteral("Host-side session rewind"), None,
         QStringLiteral("30–80% after exploration")},
        {QStringLiteral("context_notes"), QStringLiteral("Context notes"),
         QStringLiteral("Context"),
         QStringLiteral("Pin durable notes that survive compaction"), Planned,
         QStringLiteral("Host-side pinned notes"), None,
         QStringLiteral("5–25%")},
        {QStringLiteral("new_context"), QStringLiteral("New context"),
         QStringLiteral("Context"),
         QStringLiteral("Start a fresh context with a carried summary"),
         Planned, QStringLiteral("Host-side compaction handoff"), None,
         QStringLiteral("20–60%")},

        // Memory
        {QStringLiteral("retain"), QStringLiteral("Retain"),
         QStringLiteral("Memory"),
         QStringLiteral("Queue durable facts into the memory bank"), Package,
         QStringLiteral("Memory MCP package"), None,
         QStringLiteral("5–30% across sessions")},
        {QStringLiteral("recall"), QStringLiteral("Recall"),
         QStringLiteral("Memory"),
         QStringLiteral("Search the memory bank for raw memories"), Package,
         QStringLiteral("Memory MCP package"), None,
         QStringLiteral("20–70% input")},
        {QStringLiteral("reflect"), QStringLiteral("Reflect"),
         QStringLiteral("Memory"),
         QStringLiteral("Synthesize an answer over stored memories"), Package,
         QStringLiteral("Memory MCP package"), None,
         QStringLiteral("20–60% input")},
        {QStringLiteral("memory_edit"), QStringLiteral("Memory edit"),
         QStringLiteral("Memory"),
         QStringLiteral("Update, forget, or invalidate memories"), Package,
         QStringLiteral("Memory MCP package"), None,
         QStringLiteral("0–20%")},

        // Skills
        {QStringLiteral("learn"), QStringLiteral("Learn"),
         QStringLiteral("Skills"),
         QStringLiteral("Capture a reusable lesson as a skill"), Planned,
         QStringLiteral("Skill capture from transcripts"), None,
         QStringLiteral("10–40% across repeated tasks")},
        {QStringLiteral("manage_skill"), QStringLiteral("Skills"),
         QStringLiteral("Skills"),
         QStringLiteral("Invoke, create, update, or delete managed skills"),
         Native, QStringLiteral("Claude Skill; ForkMesh installed skills"),
         None, QStringLiteral("5–30% across repeated tasks")},

        // Hidden control tools
        {QStringLiteral("yield"), QStringLiteral("Yield"),
         QStringLiteral("Control"),
         QStringLiteral("Hand control back while background work continues"),
         Native, QStringLiteral("Implicit in background job delivery"), None,
         QStringLiteral("10–50% parent context"), true},
        {QStringLiteral("goal"), QStringLiteral("Goal"),
         QStringLiteral("Control"),
         QStringLiteral("Persistent objective with budget and completion"),
         Planned, QStringLiteral("Host-side goal mode"), None,
         QStringLiteral("5–25% on long autonomous work"), true},
        {QStringLiteral("think"), QStringLiteral("Think"),
         QStringLiteral("Control"),
         QStringLiteral("Explicit reasoning step"), Native,
         QStringLiteral("Streamed reasoning deltas"), None,
         QStringLiteral("0–15%"), true},

        // ForkMesh buckets
        {QStringLiteral("mcp"), QStringLiteral("MCP tools"),
         QStringLiteral("Extension"),
         QStringLiteral("Extension tools that match no family above"),
         Native, QStringLiteral("Any MCP server; Claude ToolSearch"), None,
         QStringLiteral("Depends on package")},
        {QStringLiteral("other"), QStringLiteral("Other"),
         QStringLiteral("Extension"),
         QStringLiteral("Runtime tools ForkMesh does not classify yet"),
         Native, QString(), None, QString()},
    };
}

const QHash<QString, int> &familyIndex()
{
    static const QHash<QString, int> index = [] {
        QHash<QString, int> built;
        const QVector<AgentToolFamily> &families = agentToolFamilies();
        for (int i = 0; i < families.size(); ++i)
            built.insert(families.at(i).id, i);
        return built;
    }();
    return index;
}

QString classifyExtensionTool(const QString &server, const QString &tool)
{
    const QString probe = (server + QLatin1Char(' ') + tool).toLower();
    auto has = [&probe](std::initializer_list<const char *> needles) {
        for (const char *needle : needles)
            if (probe.contains(QLatin1String(needle)))
                return true;
        return false;
    };
    if (has({"github", "gh_"}))
        return QStringLiteral("github");
    if (has({"playwright", "puppeteer", "browser", "chrome", "chromium"}))
        return QStringLiteral("browser");
    if (has({"computer", "desktop", "screen", "xdotool"}))
        return QStringLiteral("computer");
    if (has({"ast_edit", "ast-edit", "astedit"}))
        return QStringLiteral("ast_edit");
    if (has({"ast_grep", "ast-grep", "astgrep", "sgrep"}))
        return QStringLiteral("ast_grep");
    if (has({"lsp", "language_server", "language-server", "diagnostics",
             "goto_definition", "find_references"}))
        return QStringLiteral("lsp");
    if (has({"debug", "dap_", "breakpoint"}))
        return QStringLiteral("debug");
    if (has({"security", "semgrep", "codeql", "scan"}))
        return QStringLiteral("security_scan");
    if (has({"jupyter", "kernel", "eval", "execute_code", "python_exec",
             "repl"}))
        return QStringLiteral("eval");
    if (has({"tts", "speech", "speak", "voice"}))
        return QStringLiteral("tts");
    if (has({"generate_image", "image_gen", "imagegen", "dalle"}))
        return QStringLiteral("generate_image");
    if (has({"memory_edit", "forget_memory", "delete_memory"}))
        return QStringLiteral("memory_edit");
    if (has({"reflect"}))
        return QStringLiteral("reflect");
    if (has({"recall", "search_memory", "memory_search"}))
        return QStringLiteral("recall");
    if (has({"retain", "remember", "store_memory", "add_memory"}))
        return QStringLiteral("retain");
    if (has({"web_search", "websearch", "search_web", "brave", "tavily",
             "exa_"}))
        return QStringLiteral("web_search");
    if (has({"fetch", "read_url"}))
        return QStringLiteral("read");
    if (has({"checkpoint"}))
        return QStringLiteral("checkpoint");
    if (has({"rewind"}))
        return QStringLiteral("rewind");
    if (has({"skill"}))
        return QStringLiteral("manage_skill");
    return QStringLiteral("mcp");
}

} // namespace

const QVector<AgentToolFamily> &agentToolFamilies()
{
    static const QVector<AgentToolFamily> families = buildFamilies();
    return families;
}

const AgentToolFamily *agentToolFamily(const QString &id)
{
    const int index = familyIndex().value(id, -1);
    return index < 0 ? nullptr : &agentToolFamilies().at(index);
}

QStringList agentToolFamilyIds()
{
    QStringList ids;
    for (const AgentToolFamily &family : agentToolFamilies())
        ids.append(family.id);
    return ids;
}

QStringList agentToolCategories()
{
    QStringList categories;
    for (const AgentToolFamily &family : agentToolFamilies())
        if (!categories.contains(family.category))
            categories.append(family.category);
    return categories;
}

QString agentToolFamilyFor(const QString &runtimeToolName,
                           const QJsonObject &input)
{
    const QString raw = runtimeToolName.trimmed();
    if (raw.isEmpty())
        return QStringLiteral("other");

    // Extension tools: Claude names them mcp__<server>__<tool>, Codex emits
    // <server>::<tool>. Both carry the server and tool ids for heuristics.
    if (raw.startsWith(QLatin1String("mcp__"))) {
        const QStringList parts = raw.mid(5).split(QStringLiteral("__"));
        return classifyExtensionTool(parts.value(0),
                                     parts.mid(1).join(QLatin1Char('_')));
    }
    if (raw.contains(QLatin1String("::"))) {
        const int split = raw.indexOf(QLatin1String("::"));
        return classifyExtensionTool(raw.left(split), raw.mid(split + 2));
    }

    static const QHash<QString, QString> exact = {
        {QStringLiteral("read"), QStringLiteral("read")},
        {QStringLiteral("notebookread"), QStringLiteral("read")},
        {QStringLiteral("webfetch"), QStringLiteral("read")},
        {QStringLiteral("write"), QStringLiteral("write")},
        {QStringLiteral("edit"), QStringLiteral("edit")},
        {QStringLiteral("multiedit"), QStringLiteral("edit")},
        {QStringLiteral("notebookedit"), QStringLiteral("edit")},
        {QStringLiteral("filechange"), QStringLiteral("edit")},
        {QStringLiteral("apply_patch"), QStringLiteral("edit")},
        {QStringLiteral("grep"), QStringLiteral("grep")},
        {QStringLiteral("search"), QStringLiteral("grep")},
        {QStringLiteral("glob"), QStringLiteral("glob")},
        {QStringLiteral("find"), QStringLiteral("glob")},
        {QStringLiteral("ls"), QStringLiteral("glob")},
        {QStringLiteral("bash"), QStringLiteral("bash")},
        {QStringLiteral("shell"), QStringLiteral("bash")},
        {QStringLiteral("bashoutput"), QStringLiteral("bash")},
        {QStringLiteral("killshell"), QStringLiteral("bash")},
        {QStringLiteral("killbash"), QStringLiteral("bash")},
        {QStringLiteral("codextool"), QStringLiteral("bash")},
        {QStringLiteral("task"), QStringLiteral("task")},
        {QStringLiteral("agent"), QStringLiteral("task")},
        {QStringLiteral("workflow"), QStringLiteral("task")},
        {QStringLiteral("sendmessage"), QStringLiteral("hub")},
        {QStringLiteral("listagents"), QStringLiteral("hub")},
        {QStringLiteral("taskoutput"), QStringLiteral("hub")},
        {QStringLiteral("taskstop"), QStringLiteral("hub")},
        {QStringLiteral("monitor"), QStringLiteral("hub")},
        {QStringLiteral("sleep"), QStringLiteral("hub")},
        {QStringLiteral("schedulewakeup"), QStringLiteral("hub")},
        {QStringLiteral("croncreate"), QStringLiteral("hub")},
        {QStringLiteral("crondelete"), QStringLiteral("hub")},
        {QStringLiteral("cronlist"), QStringLiteral("hub")},
        // Loads deferred tool schemas, mostly MCP; classified but never
        // disallowed, since switching it off would strand every deferred tool.
        {QStringLiteral("toolsearch"), QStringLiteral("mcp")},
        {QStringLiteral("todowrite"), QStringLiteral("todo")},
        {QStringLiteral("todoread"), QStringLiteral("todo")},
        {QStringLiteral("exitplanmode"), QStringLiteral("todo")},
        {QStringLiteral("enterplanmode"), QStringLiteral("todo")},
        {QStringLiteral("askuserquestion"), QStringLiteral("ask")},
        {QStringLiteral("websearch"), QStringLiteral("web_search")},
        {QStringLiteral("imagegeneration"), QStringLiteral("generate_image")},
        {QStringLiteral("skill"), QStringLiteral("manage_skill")},
        {QStringLiteral("computer"), QStringLiteral("computer")},
        {QStringLiteral("computer_use"), QStringLiteral("computer")},
        {QStringLiteral("lsp"), QStringLiteral("lsp")},
        {QStringLiteral("debug"), QStringLiteral("debug")},
        {QStringLiteral("eval"), QStringLiteral("eval")},
        {QStringLiteral("ast_grep"), QStringLiteral("ast_grep")},
        {QStringLiteral("ast_edit"), QStringLiteral("ast_edit")},
        {QStringLiteral("security_scan"), QStringLiteral("security_scan")},
        {QStringLiteral("hub"), QStringLiteral("hub")},
        {QStringLiteral("todo"), QStringLiteral("todo")},
        {QStringLiteral("ask"), QStringLiteral("ask")},
        {QStringLiteral("browser"), QStringLiteral("browser")},
        {QStringLiteral("web_search"), QStringLiteral("web_search")},
        {QStringLiteral("github"), QStringLiteral("github")},
        {QStringLiteral("generate_image"), QStringLiteral("generate_image")},
        {QStringLiteral("tts"), QStringLiteral("tts")},
        {QStringLiteral("checkpoint"), QStringLiteral("checkpoint")},
        {QStringLiteral("rewind"), QStringLiteral("rewind")},
        {QStringLiteral("context_notes"), QStringLiteral("context_notes")},
        {QStringLiteral("new_context"), QStringLiteral("new_context")},
        {QStringLiteral("retain"), QStringLiteral("retain")},
        {QStringLiteral("recall"), QStringLiteral("recall")},
        {QStringLiteral("reflect"), QStringLiteral("reflect")},
        {QStringLiteral("memory_edit"), QStringLiteral("memory_edit")},
        {QStringLiteral("learn"), QStringLiteral("learn")},
        {QStringLiteral("manage_skill"), QStringLiteral("manage_skill")},
        {QStringLiteral("yield"), QStringLiteral("yield")},
        {QStringLiteral("goal"), QStringLiteral("goal")},
        {QStringLiteral("think"), QStringLiteral("think")},
    };
    const QString key = raw.toLower();
    if (const QString family = exact.value(key); !family.isEmpty())
        return family;

    // Codex dynamic tools carry a namespace in the input rather than the name.
    const QString ns = input.value(QStringLiteral("namespace")).toString();
    if (!ns.isEmpty())
        return classifyExtensionTool(ns, raw);
    if (key.startsWith(QLatin1String("dynamictool")))
        return QStringLiteral("mcp");
    return QStringLiteral("other");
}
