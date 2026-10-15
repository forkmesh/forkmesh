#include "LocalControlUpdate.h"

#include <QJsonArray>
#include <QJsonObject>

#include <iostream>

using namespace forkmesh::localcontrol::update;

static int failures = 0;

static void check(bool condition, const char *what)
{
    if (condition)
        return;
    std::cerr << "FAIL: " << what << "\n";
    ++failures;
}

static void testNames()
{
    check(familyPrefix() == QLatin1String("update."), "prefix");
    check(actionName() == QLatin1String("updateRestart"), "action");
    check(subjectName() == QLatin1String("desktop"), "subject");
    const QString none = actionSummary(0, QStringLiteral("v1.2.3"));
    check(none.contains(QLatin1String("v1.2.3")), "summary names the version");
    check(!none.contains(QLatin1String("agent")), "no agents, no agent note");
    check(actionSummary(1, QString()).contains(QLatin1String("1 agent is running")), "one agent");
    check(actionSummary(3, QString()).contains(QLatin1String("3 agents are running")), "agents");
    check(computerRestartActionName() == QLatin1String("computerRestart"), "computer action");
    check(computerRestartSubjectName() == QLatin1String("computer"), "computer subject");
    check(computerRestartSummary(1).contains(QLatin1String("entire computer")), "computer summary");
    check(computerRestartSummary(3).contains(QLatin1String("3 agents are running")),
          "computer summary agents");
}

static void testLinesSplitAndCarry()
{
    ProgressFeed feed;
    feed.append(QStringLiteral("$ git pull\nAlready up"));
    check(feed.seq() == 1, "partial line waits");
    feed.append(QStringLiteral(" to date.\r\n\n[1/3] Building CXX\r[2/3] Building CXX\n"));
    check(feed.seq() == 4, "cr and lf end lines, blanks dropped");
    const QJsonArray lines = feed.backlog();
    check(lines.size() == 4, "backlog holds every line");
    check(lines.at(1).toString() == QLatin1String("Already up to date."), "carry joins");
    check(lines.at(3).toString() == QLatin1String("[2/3] Building CXX"), "last line");
}

static void testCleanLine()
{
    check(cleanLine(QStringLiteral("\x1B[1;32mok\x1B[0m  ")) == QLatin1String("ok"),
          "ansi colours and trailing space stripped");
    const QString longLine(kMaxLineChars * 2, QLatin1Char('x'));
    check(cleanLine(longLine).size() == kMaxLineChars, "long lines are capped");
}

static void testBatches()
{
    ProgressFeed feed;
    for (int i = 1; i <= 5; ++i)
        feed.append(QStringLiteral("line %1\n").arg(i));
    QJsonObject batch = feed.batchSince(2);
    check(batch.value(QStringLiteral("first")).toInt() == 3, "batch starts after the sent seq");
    check(batch.value(QStringLiteral("seq")).toInt() == 5, "batch seq");
    check(batch.value(QStringLiteral("skipped")).toInt() == 0, "nothing skipped");
    check(batch.value(QStringLiteral("lines")).toArray().size() == 3, "three new lines");
    batch = feed.batchSince(5);
    check(batch.value(QStringLiteral("lines")).toArray().isEmpty(), "nothing new");

    ProgressFeed flood;
    for (int i = 1; i <= kMaxBacklogLines + 50; ++i)
        flood.append(QStringLiteral("compile %1\n").arg(i));
    check(flood.backlog().size() == kMaxBacklogLines, "backlog capped");
    batch = flood.batchSince(0);
    const QJsonArray lines = batch.value(QStringLiteral("lines")).toArray();
    check(lines.size() == kMaxBatchLines, "batch capped by line count");
    check(lines.last().toString() == QStringLiteral("compile %1").arg(kMaxBacklogLines + 50),
          "batch keeps the newest lines");
    check(batch.value(QStringLiteral("skipped")).toInt() ==
              kMaxBacklogLines + 50 - kMaxBatchLines,
          "skipped counts the dropped older lines");

    ProgressFeed wide;
    const QString big(300, QLatin1Char('y'));
    for (int i = 0; i < 100; ++i)
        wide.append(big + QLatin1Char('\n'));
    int chars = 0;
    for (const QJsonValue &line : wide.batchSince(0).value(QStringLiteral("lines")).toArray())
        chars += int(line.toString().size());
    check(chars <= kMaxBatchChars, "batch capped by characters");
}

static void testStatusAndReset()
{
    ProgressFeed feed;
    const int firstRun = feed.run();
    feed.append(QStringLiteral("a\n"));
    feed.noteStatus(QStringLiteral("Pulling"), false);
    check(feed.status() == QLatin1String("Pulling") && feed.error().isEmpty(), "status");
    feed.noteStatus(QStringLiteral("Update failed: build"), true);
    check(feed.error() == QLatin1String("Update failed: build"), "error kept");
    check(!feed.relaunching(), "not relaunching");
    feed.reset();
    check(feed.run() == firstRun + 1, "reset starts a new run");
    check(feed.backlog().isEmpty() && feed.error().isEmpty(), "reset clears");
    check(feed.seq() == 1, "seq stays monotonic across runs");
    feed.noteStatus(QStringLiteral("Relaunching..."), false);
    check(feed.relaunching(), "relaunching");

    RunState run;
    run.running = true;
    run.kind = QStringLiteral("update");
    run.percent = 140;
    run.phase = QStringLiteral("compiling");
    const QJsonObject state = stateObject(feed, run);
    check(state.value(QStringLiteral("percent")).toInt() == 100, "percent clamped");
    check(state.value(QStringLiteral("running")).toBool(), "running");
    check(state.value(QStringLiteral("relaunching")).toBool(), "relaunching in state");
    check(state.value(QStringLiteral("run")).toInt() == feed.run(), "run in state");
}

int main()
{
    testNames();
    testLinesSplitAndCarry();
    testCleanLine();
    testBatches();
    testStatusAndReset();
    if (failures) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "local control update tests passed\n";
    return 0;
}
