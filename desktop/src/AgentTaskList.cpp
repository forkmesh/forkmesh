#include "AgentTaskList.h"

#include "ClaudeTranscriptTools.h"

#include <QJsonDocument>
#include <QJsonValue>
#include <QRegularExpression>
#include <QStringList>

#include <algorithm>

namespace forkmesh::agenttasks {

QString normalizedStatus(const QString &status)
{
    const QString value = status.trimmed();
    if (value == QLatin1String("inProgress") || value == QLatin1String("in-progress") ||
        value == QLatin1String("active"))
        return QStringLiteral("in_progress");
    if (value == QLatin1String("done") || value == QLatin1String("complete"))
        return QStringLiteral("completed");
    return value.isEmpty() ? QStringLiteral("pending") : value;
}

QString createdTaskId(const QString &resultText)
{
    static const QRegularExpression pattern(QStringLiteral("Task\\s+#?(\\w+)\\s+created"),
                                            QRegularExpression::CaseInsensitiveOption);
    return pattern.match(resultText).captured(1);
}

QString summaryText(const Counts &counts)
{
    QStringList parts;
    if (counts.completed > 0)
        parts << QStringLiteral("%1 done").arg(counts.completed);
    if (counts.inProgress > 0)
        parts << QStringLiteral("%1 in progress").arg(counts.inProgress);
    if (counts.open() > 0)
        parts << QStringLiteral("%1 open").arg(counts.open());
    const QString noun = counts.total == 1 ? QStringLiteral("task") : QStringLiteral("tasks");
    if (parts.isEmpty())
        return QStringLiteral("%1 %2").arg(counts.total).arg(noun);
    return QStringLiteral("%1 %2 (%3)")
        .arg(counts.total)
        .arg(noun, parts.join(QStringLiteral(", ")));
}

bool Tracker::ingest(const QJsonObject &event)
{
    const QString type = event.value(QStringLiteral("type")).toString();
    if (type == QLatin1String("_codex_tool_delta")) {
        if (event.value(QStringLiteral("method")).toString() !=
            QLatin1String("turn/plan/updated"))
            return false;
        const QJsonValue delta = event.value(QStringLiteral("delta"));
        const QJsonArray plan =
            delta.isArray() ? delta.toArray()
                            : QJsonDocument::fromJson(delta.toString().toUtf8()).array();
        return replaceWithTodos(m_main, plan, QStringLiteral("step"));
    }
    if (type != QLatin1String("assistant") && type != QLatin1String("user"))
        return false;
    const QString parent = forkmesh::transcripttools::parentToolUseId(event);
    const QJsonArray content = event.value(QStringLiteral("message"))
                                   .toObject()
                                   .value(QStringLiteral("content"))
                                   .toArray();
    bool changed = false;
    for (const QJsonValue &value : content) {
        const QJsonObject block = value.toObject();
        const QString blockType = block.value(QStringLiteral("type")).toString();
        if (type == QLatin1String("assistant") && blockType == QLatin1String("tool_use")) {
            const QString id = block.value(QStringLiteral("id")).toString();
            const QString name = block.value(QStringLiteral("name")).toString();
            const QJsonObject input = block.value(QStringLiteral("input")).toObject();
            if (forkmesh::transcripttools::isSubagentTool(name)) {
                QString title = input.value(QStringLiteral("description")).toString().simplified();
                if (title.isEmpty())
                    title = input.value(QStringLiteral("subagent_type")).toString().simplified();
                m_subagentTitles.insert(id, title.isEmpty() ? QStringLiteral("Subagent") : title);
            }
            TaskGroup *group = parent.isEmpty() ? &m_main : groupFor(parent);
            changed |= applyToolUse(*group, id, name, input);
        } else if (type == QLatin1String("user") && blockType == QLatin1String("tool_result")) {
            const QString id = block.value(QStringLiteral("tool_use_id")).toString();
            changed |= applyToolResult(
                id,
                forkmesh::transcripttools::toolResultText(block.value(QStringLiteral("content"))),
                block.value(QStringLiteral("is_error")).toBool());
            if (parent.isEmpty()) {
                const qsizetype before = m_subagents.size();
                m_subagents.removeIf([&id](const TaskGroup &g) { return g.key == id; });
                m_subagentTitles.remove(id);
                changed |= m_subagents.size() != before;
            }
        }
    }
    m_subagents.removeIf([](const TaskGroup &g) { return g.tasks.isEmpty(); });
    return changed;
}

void Tracker::clear()
{
    m_main = TaskGroup();
    m_subagents.clear();
    m_pendingCreates.clear();
}

QList<TaskGroup> Tracker::groups() const
{
    QList<TaskGroup> all;
    if (!m_main.tasks.isEmpty())
        all.append(m_main);
    for (const TaskGroup &group : m_subagents)
        if (!group.tasks.isEmpty())
            all.append(group);
    return all;
}

const TaskGroup *Tracker::subagentGroup(const QString &key) const
{
    for (const TaskGroup &group : m_subagents)
        if (group.key == key)
            return &group;
    return nullptr;
}

bool Tracker::isEmpty() const
{
    return groups().isEmpty();
}

Counts Tracker::counts() const
{
    Counts counts;
    for (const TaskGroup &group : groups())
        for (const Task &task : group.tasks) {
            ++counts.total;
            counts.completed += task.completed() ? 1 : 0;
            counts.inProgress += task.inProgress() ? 1 : 0;
        }
    return counts;
}

bool Tracker::allCompleted() const
{
    const Counts all = counts();
    return all.total > 0 && all.completed == all.total;
}

const Task *Tracker::current() const
{
    for (const Task &task : m_main.tasks)
        if (task.inProgress())
            return &task;
    for (const TaskGroup &group : m_subagents)
        for (const Task &task : group.tasks)
            if (task.inProgress())
                return &task;
    return nullptr;
}

TaskGroup *Tracker::groupFor(const QString &key)
{
    for (TaskGroup &group : m_subagents)
        if (group.key == key)
            return &group;
    TaskGroup group;
    group.key = key;
    group.title = m_subagentTitles.value(key, QStringLiteral("Subagent"));
    m_subagents.append(group);
    return &m_subagents.last();
}

bool Tracker::applyToolUse(TaskGroup &group, const QString &toolUseId, const QString &name,
                           const QJsonObject &input)
{
    if (name == QLatin1String("TodoWrite"))
        return replaceWithTodos(group, input.value(QStringLiteral("todos")).toArray(),
                                QStringLiteral("content"));
    if (name == QLatin1String("TaskCreate")) {
        const QString subject = input.value(QStringLiteral("subject")).toString().trimmed();
        if (subject.isEmpty())
            return false;
        Task task;
        task.id = QStringLiteral("pending:%1").arg(toolUseId);
        task.subject = subject;
        task.activeForm = input.value(QStringLiteral("activeForm")).toString().trimmed();
        task.status = QStringLiteral("pending");
        group.tasks.append(task);
        m_pendingCreates.insert(toolUseId, {group.key, task.id});
        return true;
    }
    if (name == QLatin1String("TaskUpdate")) {
        const QString id = input.value(QStringLiteral("taskId")).toString().trimmed();
        Task *task = find(group, id);
        if (!task)
            return false;
        const QString status =
            normalizedStatus(input.value(QStringLiteral("status")).toString());
        if (status == QLatin1String("deleted")) {
            group.tasks.removeIf([&id](const Task &t) { return t.id == id; });
            return true;
        }
        if (input.contains(QStringLiteral("status")))
            task->status = status;
        const QString subject = input.value(QStringLiteral("subject")).toString().trimmed();
        if (!subject.isEmpty())
            task->subject = subject;
        const QString activeForm =
            input.value(QStringLiteral("activeForm")).toString().trimmed();
        if (!activeForm.isEmpty())
            task->activeForm = activeForm;
        return true;
    }
    return false;
}

bool Tracker::applyToolResult(const QString &toolUseId, const QString &text, bool isError)
{
    const auto pending = m_pendingCreates.constFind(toolUseId);
    if (pending == m_pendingCreates.constEnd())
        return false;
    const QString key = pending.value().first;
    const QString provisional = pending.value().second;
    m_pendingCreates.erase(pending);
    TaskGroup *group = key.isEmpty() ? &m_main : nullptr;
    for (TaskGroup &candidate : m_subagents)
        if (!key.isEmpty() && candidate.key == key)
            group = &candidate;
    if (!group)
        return false;
    if (isError) {
        group->tasks.removeIf([&provisional](const Task &t) { return t.id == provisional; });
        return true;
    }
    Task *task = find(*group, provisional);
    if (!task)
        return false;
    const QString id = createdTaskId(text);
    task->id = id.isEmpty() ? QString::number(group->nextId) : id;
    bool ok = false;
    const int numeric = task->id.toInt(&ok);
    group->nextId = ok ? qMax(group->nextId, numeric + 1) : group->nextId + 1;
    return true;
}

bool Tracker::replaceWithTodos(TaskGroup &group, const QJsonArray &todos,
                               const QString &textKey)
{
    QList<Task> next;
    for (const QJsonValue &value : todos) {
        const QJsonObject item = value.toObject();
        Task task;
        task.subject = item.value(textKey).toString().trimmed();
        if (task.subject.isEmpty())
            task.subject = item.value(QStringLiteral("content")).toString().trimmed();
        if (task.subject.isEmpty())
            continue;
        task.id = QString::number(next.size() + 1);
        task.activeForm = item.value(QStringLiteral("activeForm")).toString().trimmed();
        task.status = normalizedStatus(item.value(QStringLiteral("status")).toString());
        next.append(task);
    }
    const bool same =
        next.size() == group.tasks.size() &&
        std::equal(next.cbegin(), next.cend(), group.tasks.cbegin(),
                   [](const Task &a, const Task &b) {
                       return a.subject == b.subject && a.status == b.status &&
                              a.activeForm == b.activeForm;
                   });
    group.tasks = next;
    return !same;
}

Task *Tracker::find(TaskGroup &group, const QString &id)
{
    for (Task &task : group.tasks)
        if (task.id == id)
            return &task;
    return nullptr;
}

} // namespace forkmesh::agenttasks
