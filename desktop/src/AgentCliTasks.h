#pragma once

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

namespace forkmesh::agentclitasks {

enum class Change { None, Opened, Closed };

struct Task {
    QString id;
    QString description;
    bool backgrounded = false;
    QString taskType;
    QString subagentType;
    QString summary;
    QString lastTool;
    QString toolUseId;
    QString parentTaskId;
    int depth = 0;
    qint64 tokens = 0;
    int toolUses = 0;
    qint64 order = 0;
};

class Ledger
{
public:
    Change apply(const QJsonObject &event);
    int openBackground() const;
    QStringList backgroundDescriptions() const;
    QList<Task> rows() const;
    bool isEmpty() const { return m_tasks.isEmpty(); }
    void clear() { m_tasks.clear(); }

private:
    QHash<QString, Task> m_tasks;
    qint64 m_nextOrder = 0;
};

bool isTaskEvent(const QJsonObject &event);
QString waitingNotice(const Ledger &ledger);

} // namespace forkmesh::agentclitasks
