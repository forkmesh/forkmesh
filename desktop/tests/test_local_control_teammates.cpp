#include "LocalControlInsights.h"
#include "LocalControlTeammates.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>

#include <iostream>

namespace mates = forkmesh::localcontrol::teammates;
namespace insights = forkmesh::localcontrol::insights;
using forkmesh::teammates::Config;

static int failures = 0;

static void check(bool condition, const char *what)
{
    if (condition)
        return;
    std::cerr << "FAIL: " << what << "\n";
    ++failures;
}

static Config sample()
{
    Config config;
    config.id = QStringLiteral("qa");
    config.name = QStringLiteral("Quinn");
    config.specialty = QStringLiteral("Testing");
    config.skills = {QStringLiteral("pytest")};
    config.permissions = {QStringLiteral("repo.write")};
    return config;
}

static void testStatus()
{
    Config config = sample();
    check(mates::statusFor(config, QStringLiteral("running"), false) == QStringLiteral("Working"),
          "running session is working");
    check(mates::statusFor(config, QString(), false) == QStringLiteral("Idle"), "idle");
    config.schedule.active = true;
    config.schedule.mode = forkmesh::teammates::kScheduleDaily;
    check(mates::statusFor(config, QString(), false) == QStringLiteral("Scheduled"), "scheduled");
    config.enabled = false;
    check(mates::statusFor(config, QStringLiteral("running"), false) == QStringLiteral("Paused"),
          "paused wins over a session");
    check(mates::statusFor(config, QString(), true) == QStringLiteral("Working"),
          "native workflow in flight is working");
    check(mates::statusIsWorking(QStringLiteral("Waiting")), "waiting counts as working");
}

static void testRows()
{
    const Config config = sample();
    const QDateTime now = QDateTime::currentDateTime();
    const QJsonObject row = mates::teammateRow(config, QStringLiteral("Idle"), now);
    check(row.value(QStringLiteral("name")).toString() == QStringLiteral("Quinn"), "row name");
    check(row.value(QStringLiteral("schedule")).toObject().contains(QStringLiteral("human")),
          "schedule has a human line");
    check(!row.contains(QStringLiteral("instructions")), "list rows stay small");
    const QJsonObject detail = mates::teammateDetail(config, QStringLiteral("Idle"),
                                                     QString(10000, QLatin1Char('x')), now);
    check(detail.value(QStringLiteral("latestOutput")).toString().size() == 6000,
          "latest output bounded");
    check(detail.value(QStringLiteral("permissions")).toArray().size() == 1,
          "permissions listed read-only");
}

static void testEdit()
{
    Config config = sample();
    QString error;
    check(mates::applyEdit(&config,
                           {{QStringLiteral("name"), QStringLiteral(" Quinn Two ")},
                            {QStringLiteral("model"), QStringLiteral("claude-opus-4")},
                            {QStringLiteral("taskAgents"), QJsonArray{}},
                            {QStringLiteral("schedule"),
                             QJsonObject{{QStringLiteral("mode"), QStringLiteral("weekly")},
                                         {QStringLiteral("time"), QStringLiteral("07:30")},
                                         {QStringLiteral("days"), QJsonArray{5, 1, 5}},
                                         {QStringLiteral("active"), true}}},
                            {QStringLiteral("skills"),
                             QJsonArray{QStringLiteral("a"), QStringLiteral("a"), QStringLiteral("b")}}},
                           &error),
          "a valid edit applies");
    check(config.name == QStringLiteral("Quinn Two"), "name trimmed");
    check(config.schedule.days == QList<int>({1, 5}), "days sorted and unique");
    check(config.schedule.time == QTime(7, 30), "time parsed");
    check(config.skills == QStringList({QStringLiteral("a"), QStringLiteral("b")}), "skills unique");
    const Config before = config;
    check(!mates::applyEdit(&config, {{QStringLiteral("permissions"), QJsonArray{}}}, &error) &&
              error == QStringLiteral("field_not_editable:permissions"),
          "permissions stay on the desktop");
    check(!mates::applyEdit(&config, {{QStringLiteral("logMonitors"), QJsonArray{}}}, &error),
          "log monitors stay on the desktop");
    check(!mates::applyEdit(&config,
                            {{QStringLiteral("name"), QStringLiteral("ok")},
                             {QStringLiteral("avatar"), QStringLiteral("rm -rf")}},
                            &error) &&
              config.name == before.name,
          "a bad field leaves everything unchanged");
    check(!mates::applyEdit(&config,
                            {{QStringLiteral("schedule"),
                              QJsonObject{{QStringLiteral("days"), QJsonArray{8}}}}},
                            &error),
          "day out of range");
    check(!mates::applyEdit(&config, {{QStringLiteral("relayChannel"), QStringLiteral("general")}},
                            &error),
          "relay channel needs a #");
    check(mates::applyEdit(&config, {{QStringLiteral("relayChannel"), QStringLiteral("#team")}},
                           &error),
          "#team accepted");
    check(!mates::editableFields().contains(QStringLiteral("model")) &&
              !mates::editableFields().contains(QStringLiteral("taskAgents")),
          "agent, model, mode, backups and task routes are not special agent settings");
    check(!mates::applyEdit(&config, {}, &error) && error == QStringLiteral("no_changes"),
          "empty edit refused");
}

static void testSubjects()
{
    QString error;
    const QJsonObject changes{{QStringLiteral("focus"), QStringLiteral("docs")}};
    const QString a = mates::subjectFor(QStringLiteral("edit"),
                                        {{QStringLiteral("id"), QStringLiteral("qa")},
                                         {QStringLiteral("changes"), changes}});
    const QString b = mates::subjectFor(
        QStringLiteral("edit"),
        {{QStringLiteral("id"), QStringLiteral("qa")},
         {QStringLiteral("changes"), QJsonObject{{QStringLiteral("focus"), QStringLiteral("code")}}}});
    check(a.startsWith(QStringLiteral("qa|")) && a != b, "an edit token is bound to its changes");
    check(mates::subjectFor(QStringLiteral("edit"),
                            {{QStringLiteral("id"), QStringLiteral("../x")},
                             {QStringLiteral("changes"), changes}})
              .isEmpty(),
          "bad id refused");
    check(mates::subjectFor(QStringLiteral("edit"),
                            {{QStringLiteral("id"), QStringLiteral("qa")},
                             {QStringLiteral("changes"),
                              QJsonObject{{QStringLiteral("permissions"), QJsonArray{}}}}},
                            &error)
                  .isEmpty() &&
              error.startsWith(QStringLiteral("field_not_editable")),
          "prepare refuses what run would refuse");
    check(mates::subjectFor(QStringLiteral("create"), {{QStringLiteral("changes"), changes}}).isEmpty(),
          "create needs a name");
    check(mates::subjectFor(QStringLiteral("grade"),
                            {{QStringLiteral("session"), 4}, {QStringLiteral("grade"), 5}}) ==
              QStringLiteral("4:5"),
          "grade subject");
    check(mates::subjectFor(QStringLiteral("grade"),
                            {{QStringLiteral("session"), 4}, {QStringLiteral("grade"), 6}})
              .isEmpty(),
          "grade bounded");
    const QString p1 = mates::subjectFor(QStringLiteral("prompt"),
                                         {{QStringLiteral("id"), QStringLiteral("qa")},
                                          {QStringLiteral("text"), QStringLiteral("hi")}});
    const QString p2 = mates::subjectFor(QStringLiteral("prompt"),
                                         {{QStringLiteral("id"), QStringLiteral("qa")},
                                          {QStringLiteral("text"), QStringLiteral("bye")}});
    check(!p1.isEmpty() && p1 != p2, "a prompt token is bound to its text");
    check(mates::subjectFor(QStringLiteral("prompt"),
                            {{QStringLiteral("id"), QStringLiteral("qa")},
                             {QStringLiteral("text"), QString(9000, QLatin1Char('x'))}})
              .isEmpty(),
          "prompt bounded");
    check(mates::subjectFor(QStringLiteral("continue"), {{QStringLiteral("id"), QStringLiteral("qa")}})
              .startsWith(QStringLiteral("qa|")),
          "continue may carry no text");
    check(mates::subjectFor(QStringLiteral("briefing"), {}) == QStringLiteral("team"),
          "team briefing");
    check(mates::subjectFor(QStringLiteral("delete"), {{QStringLiteral("id"), QStringLiteral("qa")}},
                            &error)
                  .isEmpty() &&
              error == QStringLiteral("unsupported_action"),
          "delete stays on the desktop");
    for (const QString &action : mates::mutatingActions())
        check(!mates::actionSummary(action, QStringLiteral("qa|x"), QStringLiteral("Quinn")).isEmpty(),
              "every teammate action has a summary");
}

static void testStats()
{
    Config one = sample();
    Config two;
    two.id = QStringLiteral("docs");
    two.name = QStringLiteral("Dee");
    QList<mates::TaskRecord> tasks;
    tasks.append({1, QStringLiteral("qa"), {}, QStringLiteral("a"), QStringLiteral("success"), 4, 100, 1.0, {}});
    tasks.append({2, QStringLiteral("qa"), {}, QStringLiteral("b"), QStringLiteral("failed"), 2, 300, 0.5, {}});
    tasks.append({3, QString(), {QStringLiteral("qa"), QStringLiteral("docs")}, QStringLiteral("team"),
                  QStringLiteral("running"), 0, 200, 2.0, {}});
    const QJsonObject stats = mates::teamStats({one, two}, tasks, 2);
    const QJsonObject qa = stats.value(QStringLiteral("teammates")).toArray().at(0).toObject();
    check(qa.value(QStringLiteral("runs")).toInt() == 3, "team runs count for each member");
    check(qFuzzyCompare(qa.value(QStringLiteral("averageGrade")).toDouble(), 3.0), "average grade");
    check(qFuzzyCompare(qa.value(QStringLiteral("costUsd")).toDouble(), 2.5), "team cost split");
    const QJsonArray recent = stats.value(QStringLiteral("tasks")).toArray();
    check(recent.size() == 2 && recent.at(0).toObject().value(QStringLiteral("session")).toInt() == 2,
          "recent tasks newest first and bounded");
    check(!recent.at(1).toObject().value(QStringLiteral("gradable")).toBool(),
          "a running task is not gradable");
    check(mates::roomMessage(QStringLiteral("m"), QStringLiteral("s"), QString(5000, QLatin1Char('x')),
                             1, false, {})
                  .value(QStringLiteral("text"))
                  .toString()
                  .size() == 4000,
          "room message bounded");
}

static void testInsights()
{
    AgentToolFamilyStats stats;
    stats.family = QStringLiteral("read");
    stats.calls = 10;
    stats.latencyMsTotal = 100;
    stats.latencySamples = 4;
    const QJsonObject row = insights::familyRow(stats, nullptr, true, QStringLiteral("note"));
    check(row.value(QStringLiteral("averageLatencyMs")).toDouble() == 25.0, "average latency");
    check(row.value(QStringLiteral("disabled")).toBool(), "disabled flag");

    AgentGuardrailRepoStats guard;
    guard.repository = QStringLiteral("f/r");
    guard.tools.insert(QStringLiteral("Bash"), 3);
    guard.tools.insert(QStringLiteral("Edit"), 5);
    guard.tools.insert(QStringLiteral("Read"), 1);
    guard.recentPaths = {QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")};
    const QJsonObject guardRow = insights::guardrailRow(guard, 2, 2);
    check(guardRow.value(QStringLiteral("tools")).toArray().size() == 2 &&
              guardRow.value(QStringLiteral("tools")).toArray().at(0).toObject()
                      .value(QStringLiteral("tool")).toString() == QStringLiteral("Edit"),
          "top blocked tools first and bounded");
    check(guardRow.value(QStringLiteral("recentPaths")).toArray().size() == 2, "paths bounded");

    QList<insights::SessionUsage> sessions;
    sessions.append({1, QStringLiteral("a"), QStringLiteral("codex"), QStringLiteral("m1"), {}, 100, 1.0, 3, 0, {}});
    sessions.append({2, QStringLiteral("b"), QStringLiteral("codex"), QStringLiteral("m1"), {}, 50, 3.0, 1, 0, {}});
    sessions.append({3, QStringLiteral("c"), QStringLiteral("claude-code"), QStringLiteral("m2"), {}, 10, 0.5, 1, 0, {}});
    const QJsonObject usage = insights::usageSummary(sessions, 2);
    check(qFuzzyCompare(usage.value(QStringLiteral("costUsd")).toDouble(), 4.5), "usage total");
    check(usage.value(QStringLiteral("models")).toArray().at(0).toObject()
                  .value(QStringLiteral("sessions")).toInt() == 2,
          "usage grouped by model, priciest first");
    check(usage.value(QStringLiteral("top")).toArray().size() == 2 &&
              usage.value(QStringLiteral("top")).toArray().at(0).toObject()
                      .value(QStringLiteral("id")).toInt() == 2,
          "top sessions by cost");

    QList<insights::PeerTraffic> traffic;
    traffic.append({7, QStringLiteral("127.0.0.1"), 80, 5, 5});
    traffic.append({7, QStringLiteral("140.82.112.3"), 443, 100, 200});
    traffic.append({7, QStringLiteral("140.82.112.3"), 443, 1, 1});
    traffic.append({8, QStringLiteral("::1"), 22, 1, 1});
    const QJsonArray net = insights::networkRows(traffic, {{7, QStringLiteral("agent seven")}},
                                                 {{QStringLiteral("140.82.112.3"), QStringLiteral("github.com")}}, 5);
    check(net.size() == 1, "loopback-only agents drop out");
    const QJsonObject host = net.at(0).toObject().value(QStringLiteral("hosts")).toArray().at(0).toObject();
    check(host.value(QStringLiteral("name")).toString() == QStringLiteral("github.com") &&
              host.value(QStringLiteral("received")).toDouble() == 201.0,
          "peers merged and named");

    const insights::ShortcutMeta meta = insights::parseShortcutMeta(
        QStringLiteral("fix.md"),
        QStringLiteral("# name: Fix it\n# agent: Claude\n# model: opus\nDo the thing\n"));
    check(meta.name == QStringLiteral("Fix it") && meta.agent == QStringLiteral("claude") &&
              meta.model == QStringLiteral("opus"),
          "shortcut header parsed");
    check(insights::shortcutPrompt(QStringLiteral("# name: X\nbody\n")) == QStringLiteral("body"),
          "prompt drops the header");
    check(insights::shortcutKind(QStringLiteral("run.sh"), false) == QStringLiteral("script"),
          "script kind");
    check(insights::shortcutKind(QStringLiteral("x.skill"), false) == QStringLiteral("skill"),
          "skill kind");
    check(!insights::validShortcutFile(QStringLiteral("../x.md")), "no traversal");
    check(!insights::validShortcutFile(QStringLiteral("a/b.md")), "one component");
    check(insights::subjectFor(QStringLiteral("toggleFamily"),
                               {{QStringLiteral("family"), QStringLiteral("shell")},
                                {QStringLiteral("enabled"), false}}) == QStringLiteral("shell|off"),
          "toggle subject binds the state");
    check(insights::subjectFor(QStringLiteral("runShortcut"),
                               {{QStringLiteral("repo"), QStringLiteral("f/r")},
                                {QStringLiteral("scope"), QStringLiteral("shortcut")},
                                {QStringLiteral("file"), QStringLiteral("fix.md")}}) ==
              QStringLiteral("f/r|shortcut|fix.md"),
          "shortcut subject");
    check(insights::subjectFor(QStringLiteral("runShortcut"),
                               {{QStringLiteral("repo"), QStringLiteral("f/r")},
                                {QStringLiteral("scope"), QStringLiteral("home")},
                                {QStringLiteral("file"), QStringLiteral("fix.md")}})
              .isEmpty(),
          "unknown scope refused");
    check(insights::actionSummary(QStringLiteral("runShortcut"),
                                  QStringLiteral("f/r|shortcut|ship.sh"), QString(), true)
              .startsWith(QStringLiteral("Run the \"ship.sh\" script")),
          "script summary says it runs the script");
    check(insights::actionSummary(QStringLiteral("runShortcut"),
                                  QStringLiteral("f/r|shortcut|fix.md"), QString())
              .startsWith(QStringLiteral("Start an agent")),
          "prompt summary starts an agent");
    for (const QString &action : {QStringLiteral("queueShortcut"), QStringLiteral("unqueueShortcut")}) {
        check(insights::mutatingActions().contains(action), "queue actions need a confirmation");
        check(insights::subjectFor(action, {{QStringLiteral("repo"), QStringLiteral("f/r")},
                                            {QStringLiteral("scope"), QStringLiteral("shortcut")},
                                            {QStringLiteral("file"), QStringLiteral("ship.sh")}}) ==
                  QStringLiteral("f/r|shortcut|ship.sh"),
              "queue subject names the script");
    }
    check(insights::actionSummary(QStringLiteral("queueShortcut"),
                                  QStringLiteral("f/r|shortcut|ship.sh"), QString(), true)
              .startsWith(QStringLiteral("Queue the \"ship.sh\" script")),
          "queue summary says it queues the script");
    check(insights::actionSummary(QStringLiteral("unqueueShortcut"),
                                  QStringLiteral("f/r|shortcut|ship.sh"), QString(), true)
              .startsWith(QStringLiteral("Take the \"ship.sh\" script out")),
          "unqueue summary says it takes the script out");
    check(insights::appendShortcutOutput(QStringLiteral("a\n"), QStringLiteral("b\n"), 100) ==
              QStringLiteral("a\nb\n"),
          "output appends");
    check(insights::appendShortcutOutput(QStringLiteral("first line\nsecond\n"),
                                         QStringLiteral("third\n"), 12) ==
              QStringLiteral("third\n"),
          "output keeps whole lines of the tail");
    insights::ShortcutRun run;
    run.id = 3;
    run.repo = QStringLiteral("f/r");
    run.file = QStringLiteral("ship.sh");
    run.output = QStringLiteral("log");
    run.running = true;
    const QJsonObject runRow = insights::shortcutRunRow(run, false);
    check(runRow.value(QStringLiteral("id")).toInt() == 3 &&
              runRow.value(QStringLiteral("running")).toBool() &&
              !runRow.contains(QStringLiteral("output")) &&
              runRow.value(QStringLiteral("expectedMs")).toDouble() == -1,
          "run row without output");
    run.expectedMs = 90000;
    check(insights::shortcutRunRow(run, false).value(QStringLiteral("expectedMs")).toDouble() ==
              90000,
          "run row carries the last run's duration as its estimate");
    run.startedAtMs = QDateTime::currentMSecsSinceEpoch() - 5000;
    const double elapsed =
        insights::shortcutRunRow(run, false).value(QStringLiteral("elapsedMs")).toDouble();
    run.running = false;
    check(elapsed >= 5000 && elapsed < 60000 &&
              insights::shortcutRunRow(run, false).value(QStringLiteral("elapsedMs")).toDouble() == -1,
          "run row says how long a running run has gone, and -1 once it stops");
    check(insights::shortcutRunRow(run, true).value(QStringLiteral("output")).toString() ==
              QStringLiteral("log"),
          "run row with output");
    check(insights::lastOutputLine(QStringLiteral("one\ntwo\n\n  \n")) == QStringLiteral("two"),
          "last output line skips blank lines");
    check(insights::lastOutputLine(QStringLiteral("10%\r55%\r\x1b[32mdone\x1b[0m\n")) ==
              QStringLiteral("done"),
          "last output line keeps the last carriage-return frame without colour codes");
    check(insights::lastOutputLine(QStringLiteral("abcdef"), 3) == QStringLiteral("abc"),
          "last output line is capped");
    check(insights::lastOutputLine(QString()).isEmpty(), "no output, no line");
}

static void testShortcutSteps()
{
    const QString script = QStringLiteral(
        "#!/usr/bin/env bash\n"
        "# name: Deploy new version\n"
        "# step: Prepare\n"
        "set -euo pipefail\n"
        "\n"
        "tag=\"$(date +%Y%m%d)\"\n"
        "\n"
        "# step: Reset build numbers to 1\n"
        "python3 -u tools/forkmesh-release-host.py --reset-builds --tag \"$tag\"\n"
        "\n"
        "#step:   Deploy   Android build  \n"
        "bash .forkmesh/shortcuts/deploy-build-android.sh --first-build\n"
        "bash .forkmesh/shortcuts/deploy-build-linux.sh\n"
        "\n"
        "  \n"
        "# step: Deploy Linux build\r\n"
        "  bash .forkmesh/shortcuts/deploy-build-linux.sh\r\n"
        "\n"
        "# just a comment\n"
        "# step: not a step, not the first line\n"
        "echo done\n"
        "\n"
        "# step: Bad link\n"
        "bash .forkmesh/shortcuts/../escape.sh\n"
        "bashx .forkmesh/shortcuts/other.sh\n"
        "bash .forkmesh/shortcuts/joined.sh;echo\n");
    const QList<insights::ShortcutStep> steps = insights::parseShortcutSteps(script);
    check(steps.size() == 5, "named blocks are steps, unnamed blocks are not");
    if (steps.size() == 5) {
        check(steps.at(0).name == QStringLiteral("Prepare"), "a step right after the header");
        check(steps.at(1).name == QStringLiteral("Reset build numbers to 1") &&
                  steps.at(1).shortcut.isEmpty(),
              "plain step has no link");
        check(steps.at(2).name == QStringLiteral("Deploy Android build") &&
                  steps.at(2).shortcut == QStringLiteral("deploy-build-android.sh"),
              "first bash line links the step, whitespace collapsed");
        check(steps.at(3).name == QStringLiteral("Deploy Linux build") &&
                  steps.at(3).shortcut == QStringLiteral("deploy-build-linux.sh"),
              "CRLF lines and indented bash lines");
        check(steps.at(4).name == QStringLiteral("Bad link") && steps.at(4).shortcut.isEmpty(),
              "only exact bash .forkmesh/shortcuts/<file> lines link");
    }
    const QJsonArray rows = insights::shortcutStepRows(steps);
    check(rows.size() == 5 && !rows.at(1).toObject().contains(QStringLiteral("shortcut")) &&
              rows.at(2).toObject().value(QStringLiteral("shortcut")).toString() ==
                  QStringLiteral("deploy-build-android.sh"),
          "step rows carry shortcut only when linked");

    const QList<insights::ShortcutStep> plain = insights::parseShortcutSteps(
        QStringLiteral("#!/bin/bash\n# name: X\nset -e\n# step: Build\nmake\n"));
    check(plain.isEmpty(), "a step line inside a block is not a step");

    QString many;
    for (int i = 0; i < 40; ++i)
        many += QStringLiteral("# step: Step %1 %2\necho %1\n\n").arg(i).arg(QString(100, QLatin1Char('x')));
    const QList<insights::ShortcutStep> capped = insights::parseShortcutSteps(many);
    check(capped.size() == insights::kMaxShortcutSteps, "at most 32 steps");
    check(!capped.isEmpty() && capped.first().name.size() == insights::kMaxShortcutStepName,
          "step names cut at 80 characters");
    check(insights::parseShortcutSteps(QStringLiteral("# step:   \necho\n")).isEmpty(),
          "an empty step name is not a step");

    check(insights::shortcutStepMarker(QStringLiteral("::step::Deploy Android build"), steps) == 2,
          "a marker names its step");
    check(insights::shortcutStepMarker(QStringLiteral("  ::step::  deploy   android BUILD \r"),
                                       steps) == 2,
          "markers match case-insensitively, whitespace collapsed");
    check(insights::shortcutStepMarker(QStringLiteral("\x1b[1m::step::4\x1b[0m"), steps) == 3,
          "a marker can give the step's number");
    check(insights::shortcutStepMarker(QStringLiteral("::step::9"), steps) == -1 &&
              insights::shortcutStepMarker(QStringLiteral("::step::Nope"), steps) == -1 &&
              insights::shortcutStepMarker(QStringLiteral("echo ::step::Prepare"), steps) == -1,
          "unknown steps and text before the marker are ignored");
    insights::ShortcutRun tracked;
    tracked.steps = steps;
    check(!insights::trackShortcutStep(tracked, QStringLiteral("output\n::step::Pre")) &&
              tracked.step == -1,
          "a marker split across chunks waits for its line end");
    check(insights::trackShortcutStep(tracked, QStringLiteral("pare\nmore\n")) && tracked.step == 0,
          "the joined marker moves the run to its step");
    check(insights::trackShortcutStep(tracked,
                                      QStringLiteral("::step::2\nx\n::step::Bad link\n")) &&
              tracked.step == 4,
          "the last marker in a chunk wins");
    check(!insights::trackShortcutStep(tracked, QStringLiteral("::step::Bad link\n")),
          "repeating the current step is no change");
    check(insights::shortcutRunRow(tracked, false).value(QStringLiteral("step")).toInt() == 4 &&
              insights::shortcutRunRow(tracked, false).value(QStringLiteral("stepCount")).toInt() ==
                  5,
          "run rows carry the current step");
    check(!insights::shortcutRunRow(insights::ShortcutRun{}, false)
               .contains(QStringLiteral("step")),
          "runs without steps have no step key");

    const insights::ShortcutMeta meta =
        insights::parseShortcutMeta(QStringLiteral("deploy.sh"), script);
    check(meta.description.isEmpty() ||
              !meta.description.contains(QStringLiteral("step"), Qt::CaseInsensitive),
          "step lines never become the description");
    check(insights::parseShortcutMeta(QStringLiteral("d.sh"),
                                      QStringLiteral("# step: Build\n# Builds it\n"))
                  .description == QStringLiteral("Builds it"),
          "the first non-step comment is still the description");
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    testStatus();
    testRows();
    testEdit();
    testSubjects();
    testStats();
    testInsights();
    testShortcutSteps();
    if (failures == 0)
        std::cout << "local control teammates tests passed\n";
    return failures == 0 ? 0 : 1;
}
