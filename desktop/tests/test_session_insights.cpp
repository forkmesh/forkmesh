#include "SessionInsights.h"

#include <QJsonArray>
#include <QJsonObject>

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
    check(order.indexOf(QStringLiteral("outputs")) == order.indexOf(QStringLiteral("progress")) + 1,
          "missing cards slot in after their default neighbour");
    const QStringList moved = movedCard(defaults, QStringLiteral("tools"), 0);
    check(moved.value(0) == QLatin1String("tools") && moved.size() == defaults.size(),
          "moved card lands at index");
    check(movedCard(defaults, QStringLiteral("nope"), 0) == defaults, "unknown card ignored");
}

int main()
{
    testOutputs();
    testSources();
    testBackground();
    testProgressAndClear();
    testCardOrder();
    if (failures == 0)
        std::cout << "session insights tests passed\n";
    return failures == 0 ? 0 : 1;
}
