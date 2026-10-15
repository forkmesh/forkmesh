#include "AgentCliTasks.h"

namespace forkmesh::agentclitasks {

namespace {

bool terminalStatus(const QString &status)
{
    return !status.isEmpty() && status != QLatin1String("running") &&
           status != QLatin1String("pending");
}

} // namespace

bool isTaskEvent(const QJsonObject &event)
{
    if (event.value(QStringLiteral("type")).toString() != QLatin1String("system"))
        return false;
    const QString subtype = event.value(QStringLiteral("subtype")).toString();
    return subtype == QLatin1String("task_started") ||
           subtype == QLatin1String("task_updated") ||
           subtype == QLatin1String("task_notification");
}

Change Ledger::apply(const QJsonObject &event)
{
    if (!isTaskEvent(event))
        return Change::None;
    const QString taskId = event.value(QStringLiteral("task_id")).toString();
    if (taskId.isEmpty())
        return Change::None;
    const QString subtype = event.value(QStringLiteral("subtype")).toString();
    if (subtype == QLatin1String("task_started")) {
        Task &task = m_tasks[taskId];
        task.description =
            event.value(QStringLiteral("description")).toString().simplified();
        task.backgrounded = event.value(QStringLiteral("is_backgrounded")).toBool();
        return Change::Opened;
    }
    const auto it = m_tasks.find(taskId);
    if (it == m_tasks.end())
        return Change::None;
    bool closed = subtype == QLatin1String("task_notification");
    if (subtype == QLatin1String("task_updated")) {
        const QJsonObject patch = event.value(QStringLiteral("patch")).toObject();
        if (patch.value(QStringLiteral("is_backgrounded")).toBool())
            it->backgrounded = true;
        closed = terminalStatus(patch.value(QStringLiteral("status")).toString());
    }
    if (!closed)
        return Change::None;
    m_tasks.erase(it);
    return Change::Closed;
}

int Ledger::openBackground() const
{
    int open = 0;
    for (const Task &task : m_tasks)
        if (task.backgrounded)
            ++open;
    return open;
}

QStringList Ledger::backgroundDescriptions() const
{
    QStringList descriptions;
    for (const Task &task : m_tasks)
        if (task.backgrounded)
            descriptions << (task.description.isEmpty()
                                 ? QStringLiteral("unnamed task")
                                 : task.description);
    descriptions.sort();
    return descriptions;
}

QString waitingNotice(const Ledger &ledger)
{
    const int open = ledger.openBackground();
    if (open == 0)
        return QString();
    return QStringLiteral("The turn is over but %1 background task%2 the agent "
                          "started %3 still running (%4). This session stays "
                          "Running, and its done ping waits, until %5 back.")
        .arg(open)
        .arg(open == 1 ? QString() : QStringLiteral("s"),
             open == 1 ? QStringLiteral("is") : QStringLiteral("are"),
             ledger.backgroundDescriptions().join(QStringLiteral(", ")),
             open == 1 ? QStringLiteral("it reports") : QStringLiteral("they report"));
}

} // namespace forkmesh::agentclitasks
