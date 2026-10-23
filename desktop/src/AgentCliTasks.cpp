#include "AgentCliTasks.h"

#include <algorithm>
#include <functional>

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
           subtype == QLatin1String("task_progress") ||
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
        const bool known = m_tasks.contains(taskId);
        Task &task = m_tasks[taskId];
        task.id = taskId;
        if (!known)
            task.order = m_nextOrder++;
        task.description =
            event.value(QStringLiteral("description")).toString().simplified();
        task.backgrounded = event.value(QStringLiteral("is_backgrounded")).toBool();
        task.taskType = event.value(QStringLiteral("task_type")).toString();
        task.subagentType = event.value(QStringLiteral("subagent_type")).toString();
        task.toolUseId = event.value(QStringLiteral("tool_use_id")).toString();
        task.parentTaskId = event.value(QStringLiteral("parent_task_id")).toString();
        task.depth = qMax(0, event.value(QStringLiteral("spawn_depth")).toInt() - 1);
        return known ? Change::None : Change::Opened;
    }
    const auto it = m_tasks.find(taskId);
    if (it == m_tasks.end())
        return Change::None;
    if (subtype == QLatin1String("task_progress")) {
        const QString summary =
            event.value(QStringLiteral("summary")).toString().simplified();
        const QString description =
            event.value(QStringLiteral("description")).toString().simplified();
        if (!summary.isEmpty())
            it->summary = summary;
        else if (!description.isEmpty() && description != it->description)
            it->summary = description;
        const QString tool = event.value(QStringLiteral("last_tool_name")).toString();
        if (!tool.isEmpty())
            it->lastTool = tool;
        const QJsonObject usage = event.value(QStringLiteral("usage")).toObject();
        if (!usage.isEmpty()) {
            it->tokens = qint64(usage.value(QStringLiteral("total_tokens")).toDouble());
            it->toolUses = usage.value(QStringLiteral("tool_uses")).toInt();
        }
        return Change::None;
    }
    bool closed = subtype == QLatin1String("task_notification");
    if (subtype == QLatin1String("task_updated")) {
        const QJsonObject patch = event.value(QStringLiteral("patch")).toObject();
        if (patch.value(QStringLiteral("is_backgrounded")).toBool())
            it->backgrounded = true;
        const QString description =
            patch.value(QStringLiteral("description")).toString().simplified();
        if (!description.isEmpty())
            it->description = description;
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

QList<Task> Ledger::rows() const
{
    QList<Task> roots;
    QHash<QString, QList<Task>> children;
    for (const Task &task : m_tasks) {
        if (!task.parentTaskId.isEmpty() && m_tasks.contains(task.parentTaskId))
            children[task.parentTaskId].append(task);
        else
            roots.append(task);
    }
    const auto byOrder = [](const Task &a, const Task &b) { return a.order < b.order; };
    std::sort(roots.begin(), roots.end(), byOrder);
    QList<Task> out;
    std::function<void(const Task &, int)> visit = [&](const Task &task, int depth) {
        Task row = task;
        row.depth = qMax(depth, 0);
        out.append(row);
        QList<Task> kids = children.value(task.id);
        std::sort(kids.begin(), kids.end(), byOrder);
        for (const Task &kid : std::as_const(kids))
            visit(kid, depth + 1);
    };
    for (const Task &root : std::as_const(roots))
        visit(root, root.depth);
    return out;
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
