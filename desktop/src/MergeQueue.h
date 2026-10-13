#pragma once

#include <QList>
#include <QString>
#include <QStringList>

namespace MergeQueueState {
inline const QString Queued = QStringLiteral("queued");
inline const QString Updating = QStringLiteral("updating");
inline const QString Merging = QStringLiteral("merging");
inline const QString Blocked = QStringLiteral("blocked");
inline const QString Failed = QStringLiteral("failed");
} // namespace MergeQueueState

struct MergeQueueEntry {
    int number = 0;         // pull request number
    qint64 queuedAtMs = 0;  // when it joined the queue (queue order tiebreak)
    QString state = MergeQueueState::Queued;
    QString detail;         // why it is in that state, shown in the queue list
    qint64 stateAtMs = 0;   // when `state` was last set

    bool isValid() const { return number > 0; }
};

class MergeQueue
{
public:
    MergeQueue() = default;

    static MergeQueue fromRows(const QStringList &rows);
    QStringList toRows() const;

    const QList<MergeQueueEntry> &entries() const { return m_entries; }
    int size() const { return m_entries.size(); }
    bool isEmpty() const { return m_entries.isEmpty(); }
    int indexOf(int number) const;
    bool contains(int number) const { return indexOf(number) >= 0; }
    QList<int> numbers() const;

    bool enqueue(int number, qint64 nowMs);
    bool remove(int number);
    bool move(int number, int delta);
    void clear() { m_entries.clear(); }

    bool setState(int number, const QString &state, const QString &detail,
                  qint64 nowMs);
    MergeQueueEntry front() const;
    MergeQueueEntry at(int number) const;

    QString summary() const;

    static QString sanitizeDetail(const QString &detail);

private:
    QList<MergeQueueEntry> m_entries;
};
