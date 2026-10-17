#include "ClaudeTranscriptTools.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocale>

#include <iostream>

using namespace forkmesh::transcripttools;

static int failures = 0;

static void check(bool condition, const char *what)
{
    if (condition)
        return;
    std::cerr << "FAIL: " << what << "\n";
    ++failures;
}

static QJsonObject obj(std::initializer_list<QPair<QString, QJsonValue>> pairs)
{
    QJsonObject o;
    for (const auto &pair : pairs)
        o.insert(pair.first, pair.second);
    return o;
}

static void testNames()
{
    check(isSubagentTool(QStringLiteral("Agent")) && isSubagentTool(QStringLiteral("Task")),
          "Agent and its old name Task are subagent tools");
    check(displayName(QStringLiteral("Agent"),
                      obj({{QStringLiteral("subagent_type"), QStringLiteral("Explore")}})) ==
              QLatin1String("Explore"),
          "an Agent call is named for its subagent");
    check(displayName(QStringLiteral("Agent"), {}) == QLatin1String("Agent"),
          "an Agent call without a type says Agent");
    check(mcpServer(QStringLiteral("mcp__forkmesh__create_issue")) == QLatin1String("forkmesh") &&
              mcpToolName(QStringLiteral("mcp__forkmesh__create_issue")) ==
                  QLatin1String("create_issue"),
          "MCP names split into server and tool");
    check(displayName(QStringLiteral("mcp__forkmesh__create_issue"), {}) ==
              QStringLiteral("forkmesh · create issue"),
          "MCP tools read as server · tool");
    check(displayName(QStringLiteral("mcp__claude_ai_Gmail__search"), {}) ==
              QStringLiteral("claude.ai Gmail · search"),
          "claude.ai connectors read naturally");
    check(!isMcpTool(QStringLiteral("mcp__broken")), "a name without a tool part is not MCP");
    check(isShowWindowTool(QStringLiteral("mcp__forkmesh__show_window")),
          "the forkmesh show_window tool is recognised");
    check(!isWindowTool(QStringLiteral("mcp__other__show_window")),
          "another server's show_window is not ours");
    check(displayName(QStringLiteral("ExitPlanMode"), {}) == QLatin1String("Plan"),
          "ExitPlanMode reads as Plan");
}

static void testSubtitles()
{
    check(subtitle(QStringLiteral("Glob"), obj({{QStringLiteral("pattern"), QStringLiteral("**/*.cpp")},
                                                {QStringLiteral("path"), QStringLiteral("src")}})) ==
              QStringLiteral("**/*.cpp in src"),
          "Glob shows its pattern and path");
    check(subtitle(QStringLiteral("NotebookEdit"),
                   obj({{QStringLiteral("notebook_path"), QStringLiteral("/a/b.ipynb")}})) ==
              QLatin1String("/a/b.ipynb"),
          "NotebookEdit shows notebook_path");
    check(subtitle(QStringLiteral("Bash"), obj({{QStringLiteral("description"), QStringLiteral("Build")},
                                                {QStringLiteral("run_in_background"), true}})) ==
              QStringLiteral("Build · in background"),
          "background commands say so");
    check(subtitle(QStringLiteral("WebFetch"), obj({{QStringLiteral("url"), QStringLiteral("https://x.dev")}})) ==
              QLatin1String("https://x.dev"),
          "WebFetch shows its URL");
    check(subtitle(QStringLiteral("Skill"), obj({{QStringLiteral("skill"), QStringLiteral("review")}})) ==
              QLatin1String("/review"),
          "Skill shows the slash name");
    check(subtitle(QStringLiteral("ScheduleWakeup"),
                   obj({{QStringLiteral("delaySeconds"), 1200},
                        {QStringLiteral("reason"), QStringLiteral("watch CI")}})) ==
              QStringLiteral("in 20 min · watch CI"),
          "ScheduleWakeup shows when and why");
    check(subtitle(QStringLiteral("Workflow"),
                   obj({{QStringLiteral("script"),
                         QStringLiteral("export const meta = { name: 'review-changes' }")}})) ==
              QLatin1String("review-changes"),
          "Workflow shows the script's meta name");
    check(subtitle(QStringLiteral("mcp__forkmesh__show_window"),
                   obj({{QStringLiteral("title"), QStringLiteral("Release plan")}})) ==
              QLatin1String("Release plan"),
          "show_window shows its title");
    check(subtitle(QStringLiteral("mcp__x__y"), obj({{QStringLiteral("query"), QStringLiteral("a\nb")}})) ==
              QLatin1String("a"),
          "unknown tools fall back to a short first line");
    check(bodyHidden(QStringLiteral("ToolSearch")) && bodyHidden(QStringLiteral("Workflow")),
          "tools whose input is noise hide their body");
    check(bodyMarkdown(QStringLiteral("ExitPlanMode"),
                       obj({{QStringLiteral("plan"), QStringLiteral("## Plan")}})) ==
              QLatin1String("## Plan"),
          "the plan renders as markdown");
    check(bodyMarkdown(QStringLiteral("mcp__forkmesh__show_window"),
                       obj({{QStringLiteral("body"), QStringLiteral("<b>x</b>")},
                            {QStringLiteral("format"), QStringLiteral("html")}}))
              .isEmpty(),
          "HTML window bodies are not echoed into the transcript");
}

static void testResults()
{
    int images = 0;
    const QJsonArray blocks{
        obj({{QStringLiteral("type"), QStringLiteral("text")}, {QStringLiteral("text"), QStringLiteral("hi")}}),
        obj({{QStringLiteral("type"), QStringLiteral("image")}}),
        obj({{QStringLiteral("type"), QStringLiteral("tool_reference")},
             {QStringLiteral("tool_name"), QStringLiteral("Monitor")}})};
    const QString text = toolResultText(blocks, &images);
    check(images == 1, "image blocks are counted");
    check(text.contains(QLatin1String("hi")) && text.contains(QLatin1String("[image]")) &&
              text.contains(QLatin1String("Loaded Monitor")),
          "text, images and loaded tools all show");

    check(isServerToolResultBlock(QStringLiteral("web_search_tool_result")) &&
              !isServerToolResultBlock(QStringLiteral("tool_result")),
          "server tool results are told apart from tool results");
    bool error = false;
    const QJsonObject search = obj(
        {{QStringLiteral("type"), QStringLiteral("web_search_tool_result")},
         {QStringLiteral("content"),
          QJsonArray{obj({{QStringLiteral("title"), QStringLiteral("Qt")},
                          {QStringLiteral("url"), QStringLiteral("https://qt.io")}})}}});
    check(serverToolResultText(search, &error) == QStringLiteral("Qt — https://qt.io") && !error,
          "web search results list title and URL");
    const QJsonObject failed = obj(
        {{QStringLiteral("type"), QStringLiteral("web_search_tool_result")},
         {QStringLiteral("content"),
          obj({{QStringLiteral("error_code"), QStringLiteral("max_uses_exceeded")}})}});
    serverToolResultText(failed, &error);
    check(error, "a web search error is an error");
}

static void testEvents()
{
    check(parentToolUseId(obj({{QStringLiteral("parent_tool_use_id"), QStringLiteral("toolu_1")}})) ==
              QLatin1String("toolu_1"),
          "subagent events carry their parent");
    check(parentToolUseId(obj({{QStringLiteral("parent_tool_use_id"), QJsonValue()}})).isEmpty(),
          "a null parent is the main agent");
    const QJsonObject compact = obj(
        {{QStringLiteral("type"), QStringLiteral("system")},
         {QStringLiteral("subtype"), QStringLiteral("compact_boundary")},
         {QStringLiteral("compact_metadata"),
          obj({{QStringLiteral("trigger"), QStringLiteral("auto")},
               {QStringLiteral("pre_tokens"), 1000}})}});
    check(systemNotice(compact).startsWith(QLatin1String("Conversation compacted (auto")),
          "compaction is announced");
    check(systemNotice(obj({{QStringLiteral("type"), QStringLiteral("system")},
                            {QStringLiteral("subtype"), QStringLiteral("hook_response")}}))
              .isEmpty(),
          "hook chatter stays out of the transcript");
    check(systemNotice(obj({{QStringLiteral("type"), QStringLiteral("system")},
                            {QStringLiteral("subtype"), QStringLiteral("api_retry")},
                            {QStringLiteral("attempt"), 2},
                            {QStringLiteral("max_retries"), 10},
                            {QStringLiteral("error_status"), 529}}))
              .contains(QLatin1String("attempt 2 of 10")),
          "API retries say which attempt");
    const QJsonObject result = obj(
        {{QStringLiteral("type"), QStringLiteral("result")},
         {QStringLiteral("usage"), obj({{QStringLiteral("input_tokens"), 10},
                                        {QStringLiteral("cache_read_input_tokens"), 90},
                                        {QStringLiteral("output_tokens"), 5}})}});
    check(resultTokenSummary(result).contains(QLatin1String("100 in")) &&
              resultTokenSummary(result).contains(QLatin1String("5 out")),
          "Claude results report input and output tokens");
}

static void testTokens()
{
    OutputTokenCounter counter;
    const auto message = [](const QString &id, int out) {
        return obj({{QStringLiteral("id"), id},
                    {QStringLiteral("usage"), obj({{QStringLiteral("output_tokens"), out}})}});
    };
    check(counter.add(message(QStringLiteral("m1"), 10)) == 10, "the first block counts");
    check(counter.add(message(QStringLiteral("m1"), 10)) == 0,
          "a repeated usage on the same message is not counted twice");
    check(counter.add(message(QStringLiteral("m1"), 25)) == 15, "growth on a message counts");
    check(counter.add(message(QStringLiteral("m2"), 4)) == 4, "a new message counts");
    check(counter.add(message(QString(), 3)) == 3, "messages without an id count as they come");
}

static void testWindowResults()
{
    const WindowResult answered = parseWindowResult(QStringLiteral(
        "{\"window_id\":\"w0123456789abcdef\",\"open\":true,\"answered\":true,"
        "\"button\":\"Approve\",\"input\":\"ok\"}"));
    check(answered.valid && answered.answered, "a window reply parses");
    check(windowResultSummary(answered) == QStringLiteral("The user chose “Approve”: ok"),
          "the summary names the choice");
    check(!parseWindowResult(QStringLiteral("Error: no channel")).valid,
          "an error is not a window reply");
    check(displayName(QStringLiteral("mcp__forkmesh__open_browser_window"), {}) ==
              QLatin1String("Browser"),
          "the browser tool reads as Browser");
    check(subtitle(QStringLiteral("mcp__forkmesh__open_browser_window"),
                   obj({{QStringLiteral("url"), QStringLiteral("http://localhost:3000")}})) ==
              QLatin1String("http://localhost:3000"),
          "the browser card shows its URL");
    const WindowResult page = parseWindowResult(QStringLiteral(
        "{\"window_id\":\"w0123456789abcdef\",\"kind\":\"browser\",\"open\":true,"
        "\"url\":\"http://localhost:3000/\",\"page_title\":\"Dev\"}"));
    check(page.valid && page.browser &&
              windowResultSummary(page) == QStringLiteral("Browser open · Dev"),
          "a browser reply names the page");
    const WindowResult outside = parseWindowResult(QStringLiteral(
        "{\"kind\":\"browser\",\"external\":true,\"open\":false}"));
    check(outside.valid && windowResultSummary(outside) ==
                               QStringLiteral("Opened in the user's browser"),
          "a page opened outside the app says so");
    const WindowResult read = parseWindowResult(QStringLiteral(
        "{\"window_id\":\"w0123456789abcdef\",\"kind\":\"page\",\"open\":true,"
        "\"title\":\"Login\",\"total_chars\":1234,\"has_screenshot\":true,"
        "\"console\":[{\"level\":\"error\"},{\"level\":\"info\"}]}\n"
        "Sign in\nUsername\n[image]"));
    check(read.valid && read.page, "a page read parses from its first line");
    check(windowResultSummary(read) ==
              QStringLiteral("Read %1 characters of Login · 1 console error · screenshot")
                  .arg(QLocale().toString(qint64(1234))),
          "a page read summarizes size, errors and screenshot");
    check(displayName(QStringLiteral("mcp__forkmesh__read_browser_page"), {}) ==
              QLatin1String("Read page"),
          "read_browser_page reads as Read page");
}

static QStringList iconKeys(const QString &command)
{
    QStringList keys;
    for (const CommandIcon &icon : commandIcons(command))
        keys << icon.key;
    return keys;
}

static void testCommandIcons()
{
    check(commandPrograms(QStringLiteral("git status --short && grep -rn foo src | head -20")) ==
              QStringList({QStringLiteral("git"), QStringLiteral("grep"), QStringLiteral("head")}),
          "a pipeline names each program in order");
    check(commandPrograms(QStringLiteral("cd /tmp/x && FOO=1 sudo -u me env A=b timeout 30s ./build/run-tests 2>&1 | tail -5")) ==
              QStringList({QStringLiteral("cd"), QStringLiteral("run-tests"), QStringLiteral("tail")}),
          "assignments, wrappers and redirects are skipped");
    check(commandPrograms(QStringLiteral("python3 - <<'EOF'\nimport os\nprint(os.getcwd())\nEOF\nls -la")) ==
              QStringList({QStringLiteral("python3"), QStringLiteral("ls")}),
          "a heredoc body is not read as commands");
    check(commandPrograms(QStringLiteral("echo \"a | b; c\" > out.txt; cat < in.txt")) ==
              QStringList({QStringLiteral("echo"), QStringLiteral("cat")}),
          "quoted separators and redirect targets are not programs");
    check(commandPrograms(QStringLiteral("for f in a b; do wc -l \"$f\"; done")) ==
              QStringList({QStringLiteral("wc")}),
          "loop headers are not programs");
    check(commandPrograms(QStringLiteral("bash -lc 'make -j4 && ctest'")) ==
              QStringList({QStringLiteral("make"), QStringLiteral("ctest")}),
          "a bash -c script is read for its own programs");
    check(commandPrograms(QStringLiteral("x=$(git rev-parse HEAD); echo $x")) ==
              QStringList({QStringLiteral("git"), QStringLiteral("echo")}),
          "command substitution is read");
    check(commandPrograms(QStringLiteral("GIT_SSH_COMMAND=\"ssh -o A=/x/known_hosts\" git push; echo $(( n+1 ))")) ==
              QStringList({QStringLiteral("git"), QStringLiteral("echo")}),
          "quoted assignments and arithmetic are not programs");
    check(commandPrograms(QStringLiteral("git log $(git merge-base main HEAD)..main -- \"$(basename x)\" `date`")) ==
              QStringList({QStringLiteral("git"), QStringLiteral("basename"), QStringLiteral("date")}),
          "arguments after a command substitution stay arguments");
    check(commandPrograms(QStringLiteral("case \"$n\" in a|b) ;; *) continue;; esac; ls")) ==
              QStringList({QStringLiteral("continue"), QStringLiteral("ls")}),
          "case patterns are not programs");
    check(commandPrograms(QStringLiteral("python3 -m pytest app/tests -q")) ==
              QStringList({QStringLiteral("python3"), QStringLiteral("pytest")}),
          "python -m names its module");
    check(commandPrograms(QStringLiteral("systemd-run --user --scope -p MemoryMax=4G ninja -C build")) ==
              QStringList({QStringLiteral("systemd-run"), QStringLiteral("ninja")}),
          "systemd-run names the program it runs");
    check(commandPrograms(QStringLiteral("ls | xargs -I {} rm {}")) ==
              QStringList({QStringLiteral("ls"), QStringLiteral("rm")}),
          "xargs names the program it runs");
    check(commandPrograms(QStringLiteral("true; [ -f x ] || exit 1")).isEmpty() ||
              iconKeys(QStringLiteral("true; [ -f x ] || exit 1")).isEmpty(),
          "shell plumbing carries no icon");
    check(iconKeys(QStringLiteral("grep a b | rg c | sed s/a/b/ | awk 1")) ==
              QStringList({QStringLiteral("search"), QStringLiteral("replace")}),
          "programs sharing an icon collapse to one");
    const QVector<CommandIcon> search = commandIcons(QStringLiteral("grep a b | rg c"));
    check(search.size() == 1 &&
              search.first().programs == QStringList({QStringLiteral("grep"), QStringLiteral("rg")}),
          "a shared icon lists every program it stands for");
    check(commandIconKey(QStringLiteral("forkmesh-window-tests")) == QLatin1String("test") &&
              commandIconKey(QStringLiteral("build.py")) == QLatin1String("python") &&
              commandIconKey(QStringLiteral("some-tool")) == QLatin1String("terminal"),
          "unknown programs fall back by name");
    check(commandIconKey(QStringLiteral("git")) == QLatin1String("git") &&
              commandIconKey(QStringLiteral("curl")) == QLatin1String("web") &&
              commandIconKey(QStringLiteral("rm")) == QLatin1String("delete") &&
              commandIconKey(QStringLiteral("journalctl")) == QLatin1String("logs"),
          "common programs have their own icons");
    check(commandIconLabel(QStringLiteral("search")) == QLatin1String("Search text") &&
              commandIconLabel(QStringLiteral("delete")) == QLatin1String("Delete files") &&
              commandIconLabel(QStringLiteral("no-such-key")) == QLatin1String("Shell"),
          "an icon key names what it stands for");
    bool everyKeyLabelled = true;
    for (const char *program :
         {"grep", "cat", "head", "tail", "sed", "cut", "wc", "sort", "ls", "find", "cd",
          "mkdir", "cp", "mv", "ln", "rm", "chmod", "touch", "diff", "echo", "git", "gh",
          "python3", "pytest", "node", "npm", "cmake", "ninja", "make", "g++", "gcc",
          "dart", "swift", "xcodebuild", "go", "rustc", "cargo", "gradle",
          "java", "kotlin", "ruby", "docker", "sqlite3", "psql", "curl", "ping", "ssh",
          "openssl", "systemctl", "journalctl", "ps", "df", "kill", "sleep", "date", "tar",
          "apt", "jq", "ffmpeg", "adb", "claude", "ollama", "aws", "xdg-open"}) {
        const QString key = commandIconKey(QString::fromLatin1(program));
        if (key == QLatin1String("terminal")
            || commandIconLabel(key) == QLatin1String("Shell"))
            everyKeyLabelled = false;
    }
    check(everyKeyLabelled, "every command icon has its own hover name");
}

static void testFileEdits()
{
    const auto kinds = [](const QVector<DiffLine> &lines) {
        QString out;
        for (const DiffLine &line : lines)
            out += line.kind;
        return out;
    };
    check(kinds(lineDiff(QStringLiteral("a\nb\nc\nd"), QStringLiteral("a\nB\nc\nd\ne"))) ==
              QLatin1String(" -+  +"),
          "a line diff keeps unchanged middle lines as context between two hunks");
    check(kinds(lineDiff(QStringLiteral("x"), QStringLiteral("x"))) == QLatin1String(" "),
          "identical text diffs to context only");

    const EditStats edit = editStats(
        QStringLiteral("Edit"),
        obj({{QStringLiteral("file_path"), QStringLiteral("/r/docs/README.md")},
             {QStringLiteral("old_string"), QStringLiteral("## Head\n\n- old one\n- keep")},
             {QStringLiteral("new_string"),
              QStringLiteral("## Head\n\n- new one\n- added\n- keep")}}));
    check(edit.known && edit.added == 2 && edit.removed == 1,
          "an Edit counts only its changed lines, not the context it quoted");

    const EditStats multi = editStats(
        QStringLiteral("MultiEdit"),
        obj({{QStringLiteral("edits"),
              QJsonArray{obj({{QStringLiteral("old_string"), QStringLiteral("a")},
                              {QStringLiteral("new_string"), QStringLiteral("b")}}),
                         obj({{QStringLiteral("old_string"), QStringLiteral("c\nd")},
                              {QStringLiteral("new_string"), QString()}})}}}));
    check(multi.known && multi.added == 1 && multi.removed == 3,
          "a MultiEdit sums the changed lines of every edit");

    const EditStats write = editStats(
        QStringLiteral("Write"),
        obj({{QStringLiteral("content"), QStringLiteral("one\ntwo\nthree\n")}}));
    check(write.known && write.added == 3 && write.removed == 0,
          "a Write counts the lines it wrote");

    const EditStats change = editStats(
        QStringLiteral("FileChange"),
        obj({{QStringLiteral("changes"),
              QJsonArray{
                  obj({{QStringLiteral("path"), QStringLiteral("/r/a.h")},
                       {QStringLiteral("diff"),
                        QStringLiteral("@@ -8,2 +8,4 @@\n \n+#include <functional>\n+\n class "
                                       "QLabel;\n@@ -27,2 +29,1 @@\n-old one\n-old two\n+new\n")},
                       {QStringLiteral("kind"), obj({{QStringLiteral("type"),
                                                      QStringLiteral("update")}})}}),
                  obj({{QStringLiteral("path"), QStringLiteral("/r/b.txt")},
                       {QStringLiteral("diff"), QStringLiteral("x\ny\n")},
                       {QStringLiteral("kind"), obj({{QStringLiteral("type"),
                                                      QStringLiteral("add")}})}})}}}));
    check(change.known && change.added == 5 && change.removed == 2,
          "a Codex FileChange counts its hunk lines and an added file's lines");
    check(!editStats(QStringLiteral("Edit"), obj({{QStringLiteral("_summaryOnly"), true}})).known,
          "a summary-only Edit has no counts to show");
    check(isFileEditTool(QStringLiteral("Edit")) && isFileEditTool(QStringLiteral("Write")) &&
              !isFileEditTool(QStringLiteral("Read")),
          "Edit, MultiEdit and Write are the file edit tools");
}

static void testAgentReports()
{
    const QString framed = QStringLiteral(
        "[Subagent hand-back] The text below is the final report of a subagent. The report follows:\n"
        "  The short answer is no. Nothing in the push path refuses a push.\n"
        "  \n"
        "  **What the delete job locks**\n"
        "  \n"
        "  - The executor takes a lock.\n"
        "agentId: a123 (use SendMessage to continue)\n"
        "<usage>total_tokens: 10</usage>");
    const QString report = agentReport(framed);
    check(report.startsWith(QLatin1String("The short answer is no.")),
          "the hand-back frame and its indent are taken off a report");
    check(report.contains(QLatin1String("\n**What the delete job locks**\n")),
          "the report keeps its own lines");
    check(!report.contains(QLatin1String("agentId")) && !report.contains(QLatin1String("usage")),
          "the agent id and usage trailers are dropped");
    check(reportSummary(report) ==
              QLatin1String("The short answer is no. Nothing in the push path refuses a push."),
          "the summary is the report's lead paragraph");
    check(agentReport(QStringLiteral("[harness: output matched a pattern.]\n\nIt works."))
                  == QLatin1String("It works."),
          "a harness note before the report is dropped");

    check(reportSummary(QStringLiteral("## Findings\n\n| a | b |\n|---|---|\n\nAll three pass.\n"))
                  == QLatin1String("All three pass."),
          "headings and tables are skipped to reach prose");
    check(reportSummary(QStringLiteral("- one\n- two\n")) == QLatin1String("- one\n- two"),
          "a report that is only a list keeps its first list");
    const QString longLead = QStringLiteral("First sentence here. ") + QString(500, QLatin1Char('x'));
    check(reportSummary(longLead) == QLatin1String("First sentence here."),
          "a long lead paragraph is cut at a sentence end");
    const QString words = QStringLiteral("word ").repeated(200);
    const QString cut = reportSummary(words);
    check(cut.size() <= 420 && cut.endsWith(QStringLiteral("…")),
          "a lead with no sentence end is cut at a word");
    check(plainSummary(QStringLiteral("Uses **bold** and `code`. Then more text follows here."), 30)
                  == QLatin1String("Uses bold and code."),
          "a plain summary drops markdown marks and keeps whole sentences");

    check(isAgentLaunchResult(QStringLiteral("Async agent launched successfully.\nagentId: x")),
          "a background launch is recognised");
    check(!isAgentLaunchResult(QStringLiteral("The agent found it.")),
          "a report is not a launch");

    const auto notes = taskNotifications(QStringLiteral(
        "<task-notification>\n<task-id>a1</task-id>\n<tool-use-id>toolu_9</tool-use-id>\n"
        "<status>completed</status>\n<summary>Agent \"Map pages\" finished</summary>\n"
        "<result>It is done &amp; merged.</result>\n</task-notification>\n"
        "<task-notification><status>completed</status><summary>Monitor</summary>"
        "<event>line 1</event></task-notification>"));
    check(notes.size() == 2, "every notification block is read");
    check(notes.value(0).toolUseId == QLatin1String("toolu_9") &&
              notes.value(0).status == QLatin1String("completed") &&
              notes.value(0).summary == QLatin1String("Agent \"Map pages\" finished") &&
              notes.value(0).result == QLatin1String("It is done & merged."),
          "a notification gives its tool call, status, summary and unescaped result");
    check(notes.value(1).event == QLatin1String("line 1") && notes.value(1).result.isEmpty(),
          "a monitor event keeps its event text");

    const QJsonObject stream = obj(
        {{QStringLiteral("type"), QStringLiteral("system")},
         {QStringLiteral("subtype"), QStringLiteral("task_notification")},
         {QStringLiteral("status"), QStringLiteral("completed")},
         {QStringLiteral("summary"),
          QStringLiteral("Short answer up front: there is no page. The rest follows.\n\n## 1. Pages\n") +
              QString(800, QLatin1Char('y'))}});
    check(systemNotice(stream) ==
              QLatin1String("Background task completed: Short answer up front: there is no page. "
                            "The rest follows."),
          "a background agent's whole report is cut to its lead in a notice");
}

static void testRawLogLineStart()
{
    const QJsonObject init = obj({{QStringLiteral("type"), QStringLiteral("system")},
                                  {QStringLiteral("subtype"), QStringLiteral("init")}});
    const QJsonObject toolUse =
        obj({{QStringLiteral("type"), QStringLiteral("assistant")},
             {QStringLiteral("uuid"), QStringLiteral("u-assistant-1")},
             {QStringLiteral("message"),
              obj({{QStringLiteral("content"),
                    QJsonArray{obj({{QStringLiteral("type"), QStringLiteral("tool_use")},
                                    {QStringLiteral("id"), QStringLiteral("toolu_01")}})}}})}});
    const auto line = [](const QJsonObject &o) {
        return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) +
               QStringLiteral("\n\n");
    };
    const QString stored = line(init) + line(toolUse);
    check(rawLogLineStart(stored, init) == 0, "the first event is found at the top");
    check(rawLogLineStart(stored, toolUse) == line(init).size(),
          "a stored event is found at its own line");

    const QString live =
        QStringLiteral("[stderr] warming up\n"
                       "{\"type\":\"system\",\"subtype\":\"init\"}\n\n"
                       "{\"uuid\": \"u-assistant-1\", \"type\": \"assistant\"}\n\n");
    check(rawLogLineStart(live, toolUse) == live.indexOf(QStringLiteral("{\"uuid\"")),
          "a live line written in another key order is found by its uuid");

    const QJsonObject result =
        obj({{QStringLiteral("type"), QStringLiteral("user")},
             {QStringLiteral("message"),
              obj({{QStringLiteral("content"),
                    QJsonArray{obj({{QStringLiteral("tool_use_id"),
                                     QStringLiteral("toolu_01")}})}}})}});
    const QString codex = QStringLiteral(
        "{\"method\":\"item/started\",\"params\":{\"item\":{\"id\":\"toolu_01\"}}}\n");
    check(rawLogLineStart(codex, result) == 0, "an event without a uuid is found by its ids");
    check(rawLogLineStart(stored, obj({{QStringLiteral("type"), QStringLiteral("result")}})) ==
              -1,
          "an event the log does not hold is not found");
    check(rawLogLineStart(QString(), init) == -1, "an empty log finds nothing");
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    testNames();
    testSubtitles();
    testResults();
    testEvents();
    testTokens();
    testWindowResults();
    testCommandIcons();
    testFileEdits();
    testAgentReports();
    testRawLogLineStart();
    if (failures) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "claude transcript tools tests passed\n";
    return 0;
}
