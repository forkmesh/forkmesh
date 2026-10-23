#include "AgentCliTasks.h"

#include <iostream>

using namespace forkmesh::agentclitasks;

static int failures = 0;

static void check(bool condition, const char *what)
{
    if (condition)
        return;
    std::cerr << "FAIL: " << what << "\n";
    ++failures;
}

static QJsonObject started(const QString &id, const QString &description,
                           bool backgrounded)
{
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("system")},
                       {QStringLiteral("subtype"), QStringLiteral("task_started")},
                       {QStringLiteral("task_id"), id},
                       {QStringLiteral("task_type"), QStringLiteral("local_agent")},
                       {QStringLiteral("description"), description},
                       {QStringLiteral("is_backgrounded"), backgrounded}};
}

static QJsonObject notification(const QString &id, const QString &status)
{
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("system")},
                       {QStringLiteral("subtype"), QStringLiteral("task_notification")},
                       {QStringLiteral("task_id"), id},
                       {QStringLiteral("status"), status}};
}

static QJsonObject updated(const QString &id, const QJsonObject &patch)
{
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("system")},
                       {QStringLiteral("subtype"), QStringLiteral("task_updated")},
                       {QStringLiteral("task_id"), id},
                       {QStringLiteral("patch"), patch}};
}

static void checkBackgroundTasksHoldTheRun()
{
    Ledger ledger;
    check(ledger.apply(started(QStringLiteral("a1"), QStringLiteral("Find CRM hooks"),
                               true)) == Change::Opened,
          "a started task opens");
    check(ledger.apply(started(QStringLiteral("a2"), QStringLiteral("Map hackathon"),
                               true)) == Change::Opened,
          "a second started task opens");
    check(ledger.openBackground() == 2, "two background tasks are open");
    check(ledger.backgroundDescriptions() ==
              QStringList({QStringLiteral("Find CRM hooks"), QStringLiteral("Map hackathon")}),
          "the open tasks are named");
    const QString notice = waitingNotice(ledger);
    check(notice.contains(QStringLiteral("2 background tasks")) &&
              notice.contains(QStringLiteral("Map hackathon")) &&
              notice.contains(QStringLiteral("they report back")),
          "the waiting notice names both tasks");

    check(ledger.apply(updated(QStringLiteral("a1"),
                               QJsonObject{{QStringLiteral("status"),
                                            QStringLiteral("completed")}})) ==
              Change::Closed,
          "a completed task_updated closes the task");
    check(ledger.apply(notification(QStringLiteral("a1"), QStringLiteral("completed"))) ==
              Change::None,
          "the notification after the update closes nothing twice");
    check(ledger.openBackground() == 1 &&
              waitingNotice(ledger).contains(QStringLiteral("1 background task ")) &&
              waitingNotice(ledger).contains(QStringLiteral("it reports back")),
          "one task is still open");
    check(ledger.apply(notification(QStringLiteral("a2"), QStringLiteral("failed"))) ==
              Change::Closed,
          "a failed notification closes the task");
    check(ledger.openBackground() == 0 && ledger.isEmpty() &&
              waitingNotice(ledger).isEmpty(),
          "no task is left open");
}

static void checkForegroundTasksDoNotHold()
{
    Ledger ledger;
    ledger.apply(started(QStringLiteral("b1"), QStringLiteral("Run tests"), false));
    check(ledger.openBackground() == 0 && !ledger.isEmpty(),
          "a foreground task does not hold the run");
    ledger.apply(updated(QStringLiteral("b1"),
                         QJsonObject{{QStringLiteral("is_backgrounded"), true}}));
    check(ledger.openBackground() == 1,
          "a foreground task moved to the background holds the run");
    check(ledger.apply(updated(QStringLiteral("b1"),
                               QJsonObject{{QStringLiteral("status"),
                                            QStringLiteral("running")}})) ==
              Change::None &&
              ledger.openBackground() == 1,
          "a running status update keeps the task open");
    check(ledger.apply(updated(QStringLiteral("b1"),
                               QJsonObject{{QStringLiteral("status"),
                                            QStringLiteral("killed")}})) ==
              Change::Closed &&
              ledger.isEmpty(),
          "a killed task closes");
}

static void checkUnrelatedEvents()
{
    Ledger ledger;
    check(ledger.apply(QJsonObject{{QStringLiteral("type"), QStringLiteral("result")},
                                   {QStringLiteral("task_id"), QStringLiteral("x")}}) ==
              Change::None,
          "a result is not a task event");
    check(ledger.apply(QJsonObject{{QStringLiteral("type"), QStringLiteral("system")},
                                   {QStringLiteral("subtype"), QStringLiteral("init")}}) ==
              Change::None,
          "init is not a task event");
    check(ledger.apply(notification(QStringLiteral("never-started"),
                                    QStringLiteral("completed"))) == Change::None,
          "a notification for an unknown task changes nothing");
    check(ledger.apply(started(QString(), QStringLiteral("no id"), true)) == Change::None &&
              ledger.isEmpty(),
          "a task without an id is ignored");
    ledger.apply(started(QStringLiteral("c1"), QString(), true));
    check(ledger.backgroundDescriptions() == QStringList{QStringLiteral("unnamed task")},
          "a task without a description is still named");
    ledger.clear();
    check(ledger.isEmpty(), "clear forgets every task");
}

static void checkSubSessionRows()
{
    Ledger ledger;
    QJsonObject parent = started(QStringLiteral("p1"), QStringLiteral("Review auth"), true);
    parent.insert(QStringLiteral("subagent_type"), QStringLiteral("general-purpose"));
    parent.insert(QStringLiteral("spawn_depth"), 1);
    ledger.apply(parent);
    ledger.apply(started(QStringLiteral("s1"), QStringLiteral("Run lint"), false));
    QJsonObject child = started(QStringLiteral("c1"), QStringLiteral("Read login.cpp"), true);
    child.insert(QStringLiteral("parent_task_id"), QStringLiteral("p1"));
    child.insert(QStringLiteral("spawn_depth"), 2);
    ledger.apply(child);
    check(ledger.apply(parent) == Change::None, "a repeated start does not reopen a task");
    check(ledger.apply(QJsonObject{{QStringLiteral("type"), QStringLiteral("system")},
                                   {QStringLiteral("subtype"), QStringLiteral("task_progress")},
                                   {QStringLiteral("task_id"), QStringLiteral("p1")},
                                   {QStringLiteral("description"), QStringLiteral("Review auth")},
                                   {QStringLiteral("summary"), QStringLiteral("Reading the token code")},
                                   {QStringLiteral("last_tool_name"), QStringLiteral("Read")},
                                   {QStringLiteral("usage"),
                                    QJsonObject{{QStringLiteral("total_tokens"), 15100},
                                                {QStringLiteral("tool_uses"), 3},
                                                {QStringLiteral("duration_ms"), 900}}}}) ==
              Change::None,
          "progress changes nothing about what holds the run");
    const QList<Task> rows = ledger.rows();
    check(rows.size() == 3, "every open task is a row");
    check(rows.value(0).id == QLatin1String("p1") && rows.value(1).id == QLatin1String("c1") &&
              rows.value(2).id == QLatin1String("s1"),
          "a nested sub-session is listed under the one that started it");
    check(rows.value(0).depth == 0 && rows.value(1).depth == 1,
          "a nested sub-session is indented one level");
    check(rows.value(0).summary == QLatin1String("Reading the token code") &&
              rows.value(0).tokens == 15100 && rows.value(0).toolUses == 3 &&
              rows.value(0).lastTool == QLatin1String("Read") &&
              rows.value(0).subagentType == QLatin1String("general-purpose"),
          "a row keeps the latest progress of its sub-session");
    ledger.apply(updated(QStringLiteral("p1"),
                         QJsonObject{{QStringLiteral("description"), QStringLiteral("Review auth flow")}}));
    check(ledger.rows().value(0).description == QLatin1String("Review auth flow"),
          "a renamed sub-session shows its new name");
    ledger.apply(notification(QStringLiteral("p1"), QStringLiteral("completed")));
    check(ledger.rows().size() == 2 && ledger.rows().value(0).id == QLatin1String("s1"),
          "a finished sub-session leaves the list, its child stays");
}

int main()
{
    checkBackgroundTasksHoldTheRun();
    checkForegroundTasksDoNotHold();
    checkUnrelatedEvents();
    checkSubSessionRows();
    if (failures)
        return 1;
    std::cout << "agent cli tasks: all checks passed\n";
    return 0;
}
