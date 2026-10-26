#pragma once

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QPair>
#include <QString>

namespace forkmesh::agenttasks {

struct Task {
    QString id;
    QString subject;
    QString activeForm;
    QString status;

    bool completed() const { return status == QLatin1String("completed"); }
    bool inProgress() const { return status == QLatin1String("in_progress"); }
};

struct TaskGroup {
    QString key;
    QString title;
    QList<Task> tasks;
    int nextId = 1;

    bool subagent() const { return !key.isEmpty(); }
};

struct Counts {
    int total = 0;
    int completed = 0;
    int inProgress = 0;
    int open() const { return total - completed - inProgress; }
};

QString normalizedStatus(const QString &status);
QString createdTaskId(const QString &resultText);
QString summaryText(const Counts &counts);

class Tracker;
QString progressText(const Tracker &tracker, int maxChars = 48);

class Tracker
{
public:
    bool ingest(const QJsonObject &event);
    void clear();

    const QList<Task> &tasks() const { return m_main.tasks; }
    QList<TaskGroup> groups() const;
    const TaskGroup *subagentGroup(const QString &key) const;
    bool isEmpty() const;
    Counts counts() const;
    int completedCount() const { return counts().completed; }
    bool allCompleted() const;
    const Task *current() const;

private:
    TaskGroup *groupFor(const QString &key);
    bool applyToolUse(TaskGroup &group, const QString &toolUseId, const QString &name,
                      const QJsonObject &input);
    bool applyToolResult(const QString &toolUseId, const QString &text, bool isError);
    static bool replaceWithTodos(TaskGroup &group, const QJsonArray &todos,
                                 const QString &textKey);
    static Task *find(TaskGroup &group, const QString &id);

    TaskGroup m_main;
    QList<TaskGroup> m_subagents;
    QHash<QString, QString> m_subagentTitles;
    QHash<QString, QPair<QString, QString>> m_pendingCreates;
};

} // namespace forkmesh::agenttasks
