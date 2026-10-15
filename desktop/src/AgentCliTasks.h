#pragma once

#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace forkmesh::agentclitasks {

enum class Change { None, Opened, Closed };

struct Task {
    QString description;
    bool backgrounded = false;
};

class Ledger
{
public:
    Change apply(const QJsonObject &event);
    int openBackground() const;
    QStringList backgroundDescriptions() const;
    bool isEmpty() const { return m_tasks.isEmpty(); }
    void clear() { m_tasks.clear(); }

private:
    QHash<QString, Task> m_tasks;
};

bool isTaskEvent(const QJsonObject &event);
QString waitingNotice(const Ledger &ledger);

} // namespace forkmesh::agentclitasks
