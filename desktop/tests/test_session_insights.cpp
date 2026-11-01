#include "SessionInsights.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>

#include <iostream>

using namespace forkmesh::sessioninsights;

static int failures = 0;

static void check(bool condition, const char *what)
{
    if (condition)
        return;
    std::cerr << "FAIL: " << what << "\n";
    ++failures;
}

static QJsonObject toolUse(const QString &id, const QString &name, const QJsonObject &input)
{
    return QJsonObject{
        {QStringLiteral("type"), QStringLiteral("assistant")},
        {QStringLiteral("message"),
         QJsonObject{{QStringLiteral("content"),
                      QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("tool_use")},
                                             {QStringLiteral("id"), id},
                                             {QStringLiteral("name"), name},
                                             {QStringLiteral("input"), input}}}}}}};
}

static QJsonObject toolResult(const QString &id, const QString &text, bool error = false)
{
    return QJsonObject{
        {QStringLiteral("type"), QStringLiteral("user")},
        {QStringLiteral("message"),
         QJsonObject{{QStringLiteral("content"),
                      QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("tool_result")},
                                             {QStringLiteral("tool_use_id"), id},
                                             {QStringLiteral("content"), text},
                                             {QStringLiteral("is_error"), error}}}}}}};
}

static void testOutputs()
{
    Collector c;
    c.ingest(toolUse(QStringLiteral("w1"), QStringLiteral("Write"),
                     {{QStringLiteral("file_path"), QStringLiteral("/repo/new.txt")},
                      {QStringLiteral("content"), QStringLiteral("a\nb\n")}}));
    c.ingest(toolResult(QStringLiteral("w1"), QStringLiteral("ok")));
    c.ingest(toolUse(QStringLiteral("e1"), QStringLiteral("Edit"),
                     {{QStringLiteral("file_path"), QStringLiteral("/repo/main.cpp")},
                      {QStringLiteral("old_string"), QStringLiteral("x")},
                      {QStringLiteral("new_string"), QStringLiteral("y\nz")}}));
    c.ingest(toolResult(QStringLiteral("e1"), QStringLiteral("ok")));
    c.ingest(toolUse(QStringLiteral("e2"), QStringLiteral("Edit"),
                     {{QStringLiteral("file_path"), QStringLiteral("/repo/bad.cpp")},
                      {QStringLiteral("old_string"), QStringLiteral("q")},
                      {QStringLiteral("new_string"), QStringLiteral("r")}}));
    c.ingest(toolResult(QStringLiteral("e2"), QStringLiteral("not found"), true));
    const QList<Entry> outputs = c.outputs();
    check(outputs.size() == 2, "failed edit is dropped from outputs");
    check(outputs.value(0).label == QLatin1String("main.cpp"), "newest output first");
    check(outputs.value(0).added == 2 && outputs.value(0).removed == 1, "edit line stats");
    check(outputs.value(1).created, "write of a new file is marked created");
    check(outputs.value(1).added == 2, "write counts lines");
    check(c.toolCalls() == 3 && c.toolErrors() == 1, "tool call and error totals");
}

static void testSources()
{
    Collector c;
    c.ingest(toolUse(QStringLiteral("r1"), QStringLiteral("Read"),
                     {{QStringLiteral("file_path"), QStringLiteral("/repo/a.md")}}));
    c.ingest(toolUse(QStringLiteral("r2"), QStringLiteral("Read"),
                     {{QStringLiteral("file_path"), QStringLiteral("/repo/a.md")}}));
    c.ingest(toolUse(QStringLiteral("f1"), QStringLiteral("WebFetch"),
                     {{QStringLiteral("url"), QStringLiteral("https://www.example.com/x")}}));
    c.ingest(toolUse(QStringLiteral("s1"), QStringLiteral("WebSearch"),
                     {{QStringLiteral("query"), QStringLiteral("qt drag")}}));
    c.ingest(toolUse(QStringLiteral("m1"), QStringLiteral("mcp__forkmesh__read_file"),
                     QJsonObject()));
    const QList<Entry> sources = c.sources();
    check(sources.size() == 4, "file, page, search and connector are sources");
    bool file = false;
    bool page = false;
    for (const Entry &entry : sources) {
        if (entry.key == QLatin1String("/repo/a.md"))
            file = entry.count == 2;
        if (entry.icon == QLatin1String("link"))
            page = entry.label == QLatin1String("example.com");
    }
    check(file, "repeat reads count once with a tally");
    check(page, "web page is labelled by host");
}

static void testBackground()
{
    Collector c;
    c.ingest(toolUse(QStringLiteral("t1"), QStringLiteral("Task"),
                     {{QStringLiteral("description"), QStringLiteral("Explore code")}}));
    c.ingest(toolUse(QStringLiteral("b1"), QStringLiteral("Bash"),
                     {{QStringLiteral("command"), QStringLiteral("make")},
                      {QStringLiteral("run_in_background"), true}}));
    check(c.runningBackground() == 2, "subagent and background shell run");
    c.ingest(toolResult(QStringLiteral("t1"), QStringLiteral("report")));
    c.ingest(toolResult(QStringLiteral("b1"),
                        QStringLiteral("Command running in background with ID: abc123")));
    check(c.runningBackground() == 1, "foreground subagent finishes on its result");
    c.ingest(toolUse(QStringLiteral("k1"), QStringLiteral("KillShell"),
                     {{QStringLiteral("shell_id"), QStringLiteral("abc123")}}));
    check(c.runningBackground() == 0, "killed shell stops");
    const QList<Background> items = c.background();
    check(items.size() == 2, "both background items listed");
}

static void testProgressAndClear()
{
    Collector c;
    c.ingest(toolUse(
        QStringLiteral("todo"), QStringLiteral("TodoWrite"),
        {{QStringLiteral("todos"),
          QJsonArray{QJsonObject{{QStringLiteral("content"), QStringLiteral("One")},
                                 {QStringLiteral("status"), QStringLiteral("completed")}},
                     QJsonObject{{QStringLiteral("content"), QStringLiteral("Two")},
                                 {QStringLiteral("status"), QStringLiteral("in_progress")}}}}}));
    check(c.tasks().counts().total == 2, "todos feed progress");
    check(c.tools().value(0).calls == 1, "tool counts");
    c.clear();
    check(c.tasks().isEmpty() && c.outputs().isEmpty() && c.toolCalls() == 0, "clear resets");
}

static void testCardOrder()
{
    const QStringList defaults = defaultCardOrder();
    check(normalizedCardOrder({}) == defaults, "empty order falls back to default");
    const QStringList saved{QStringLiteral("tools"), QStringLiteral("bogus"),
                            QStringLiteral("progress"), QStringLiteral("tools")};
    const QStringList order = normalizedCardOrder(saved);
    check(order.size() == defaults.size(), "order holds every card once");
    check(order.value(0) == QLatin1String("tools"), "saved order is kept");
    check(order.indexOf(QStringLiteral("subsessions")) == order.indexOf(QStringLiteral("progress")) + 1,
          "missing cards slot in after their default neighbour");
    check(order.indexOf(QStringLiteral("outputs")) == order.indexOf(QStringLiteral("subsessions")) + 1,
          "a run of missing cards keeps its default order");
    const QStringList older = normalizedCardOrder(
        {QStringLiteral("tools"), QStringLiteral("progress"), QStringLiteral("outputs"),
         QStringLiteral("sources"), QStringLiteral("background"), QStringLiteral("changes"),
         QStringLiteral("context"), QStringLiteral("environment")});
    check(older.indexOf(QStringLiteral("subsessions")) == older.indexOf(QStringLiteral("progress")) + 1,
          "a saved order from before Sub-sessions gets it under Progress");
    const QStringList moved = movedCard(defaults, QStringLiteral("tools"), 0);
    check(moved.value(0) == QLatin1String("tools") && moved.size() == defaults.size(),
          "moved card lands at index");
    check(movedCard(defaults, QStringLiteral("nope"), 0) == defaults, "unknown card ignored");
}

static QJsonObject reply(qint64 input, qint64 output, const QString &text)
{
    return QJsonObject{
        {QStringLiteral("type"), QStringLiteral("assistant")},
        {QStringLiteral("message"),
         QJsonObject{{QStringLiteral("content"),
                      QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                             {QStringLiteral("text"), text}}}},
                     {QStringLiteral("usage"),
                      QJsonObject{{QStringLiteral("input_tokens"), 4},
                                  {QStringLiteral("cache_creation_input_tokens"), 6},
                                  {QStringLiteral("cache_read_input_tokens"), input - 10},
                                  {QStringLiteral("output_tokens"), output}}}}}};
}

static void testContext()
{
    Collector c;
    c.ingest(QJsonObject{{QStringLiteral("type"), QStringLiteral("system")},
                         {QStringLiteral("subtype"), QStringLiteral("init")},
                         {QStringLiteral("model"), QStringLiteral("claude-opus[1m]")},
                         {QStringLiteral("tools"), QJsonArray{QStringLiteral("Read"),
                                                              QStringLiteral("Bash")}},
                         {QStringLiteral("mcp_servers"), QJsonArray{}},
                         {QStringLiteral("skills"), QJsonArray{QStringLiteral("pdf")}}});
    c.ingest(QJsonObject{{QStringLiteral("type"), QStringLiteral("_local_user")},
                         {QStringLiteral("text"), QString(400, QLatin1Char('p'))}});
    c.ingest(reply(20100, 50, QString(400, QLatin1Char('r'))));
    c.ingest(toolUse(QStringLiteral("r1"), QStringLiteral("Read"),
                     {{QStringLiteral("file_path"), QStringLiteral("/repo/a")}}));
    c.ingest(toolResult(QStringLiteral("r1"), QString(4000, QLatin1Char('x'))));
    c.ingest(reply(22000, 100, QStringLiteral("done")));
    const ContextUse &use = c.context();
    check(use.baseline == 20000, "baseline is the first turn minus the prompt");
    check(use.latest == 22100, "latest is the last turn's input and output");
    check(use.tools == 2 && use.mcpServers == 0 && use.skills == 1 && use.agents == -1,
          "init counts tools, MCP servers and skills");
    check(use.messageChars == 804 && use.toolChars > 4000, "conversation chars by kind");
    check(contextWindowFor(use.latest, 200000, use.model) == 1000000, "[1m] model has 1M");
    check(contextWindowFor(250000, 200000, QString()) == 1000000, "overflow means 1M");
    check(contextWindowFor(1000, 200000, QString()) == 200000, "configured window kept");

    const ContextSplit split = splitContext(use, use.latest, 200000, 3000);
    check(split.memory == 3000 && split.system == 17000, "memory comes out of the baseline");
    check(split.messages + split.toolUse == 2100 && split.toolUse > split.messages,
          "conversation split by length");
    check(split.free == 177900, "free space");

    ContextUse bare;
    const ContextSplit plain = splitContext(bare, 5000, 10000, 1000);
    check(plain.memory == 1000 && plain.system == 0 && plain.messages == 4000,
          "no usage events: all but memory is messages");

    const quint64 before = c.context().revision;
    c.ingest(QJsonObject{{QStringLiteral("type"), QStringLiteral("system")},
                         {QStringLiteral("subtype"), QStringLiteral("compact_boundary")}});
    check(c.context().compactions == 1 && c.context().messageChars == 0 &&
              c.context().toolChars == 0 && c.context().revision > before,
          "compaction resets the conversation");
    c.clear();
    check(c.context().latest == 0 && c.context().baseline == -1, "clear resets context");
}

static void write(const QString &path, const QByteArray &text)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (file.open(QIODevice::WriteOnly))
        file.write(text);
}

static void testMemoryFiles()
{
    QTemporaryDir dir;
    const QString home = dir.filePath(QStringLiteral("home"));
    const QString repo = home + QStringLiteral("/code/repo");
    const QString work = repo + QStringLiteral("/sub");
    QDir().mkpath(work);
    write(home + QStringLiteral("/.claude/CLAUDE.md"), QByteArray(40, 'u'));
    write(repo + QStringLiteral("/CLAUDE.md"), "@AGENTS.md\n");
    write(repo + QStringLiteral("/AGENTS.md"), QByteArray(400, 'a'));
    write(work + QStringLiteral("/CLAUDE.local.md"), QByteArray(8, 'l'));
    write(home + QStringLiteral("/.claude/projects/") + memoryProjectSlug(QDir(repo).canonicalPath()) +
              QStringLiteral("/memory/MEMORY.md"),
          "one\ntwo\n");
    const QList<MemoryFile> files = memoryFiles(work, QDir(home).canonicalPath());
    QStringList labels;
    for (const MemoryFile &file : files)
        labels << file.label;
    check(labels == QStringList{QStringLiteral("~/.claude/CLAUDE.md"),
                                QStringLiteral("~/code/repo/CLAUDE.md"),
                                QStringLiteral("~/code/repo/AGENTS.md"),
                                QStringLiteral("CLAUDE.local.md"),
                                QStringLiteral("MEMORY.md (auto memory)")},
          "memory files: user, ancestors, imports, local, auto memory");
    check(files.size() == 5 && files.at(2).tokens == 100, "tokens estimated from size");
    check(memoryProjectSlug(QStringLiteral("/home/f/src/.worktrees/x")) ==
              QStringLiteral("-home-f-src--worktrees-x"),
          "memory slug matches Claude Code's project folders");

    const QString codex = dir.filePath(QStringLiteral("codex"));
    write(codex + QStringLiteral("/AGENTS.md"), QByteArray(4, 'c'));
    const QList<MemoryFile> agents = memoryFiles(codex, dir.filePath(QStringLiteral("none")));
    check(agents.size() == 1 && agents.first().label == QStringLiteral("AGENTS.md"),
          "without CLAUDE.md the AGENTS.md chain is used");
    check(memoryFiles(QString(), home).isEmpty(), "no folder, no files");
}

int main()
{
    testOutputs();
    testSources();
    testBackground();
    testProgressAndClear();
    testCardOrder();
    testContext();
    testMemoryFiles();
    if (failures == 0)
        std::cout << "session insights tests passed\n";
    return failures == 0 ? 0 : 1;
}
