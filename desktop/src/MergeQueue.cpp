#include "MergeQueue.h"

namespace {
constexpr int kMaxDetailChars = 200;

bool knownState(const QString &state)
{
    return state == MergeQueueState::Queued || state == MergeQueueState::Updating ||
           state == MergeQueueState::Merging || state == MergeQueueState::Blocked ||
           state == MergeQueueState::Failed;
}
} // namespace

MergeQueue MergeQueue::fromRows(const QStringList &rows)
{
    MergeQueue queue;
    for (const QString &row : rows) {
        const QStringList parts = row.split(QLatin1Char('\t'));
        if (parts.isEmpty())
            continue;
        bool ok = false;
        const int number = parts.at(0).trimmed().toInt(&ok);
        if (!ok || number <= 0 || queue.contains(number))
            continue;
        MergeQueueEntry entry;
        entry.number = number;
        if (parts.size() > 1)
            entry.queuedAtMs = parts.at(1).trimmed().toLongLong();
        if (parts.size() > 2) {
            const QString state = parts.at(2).trimmed();
            entry.state = knownState(state) && state != MergeQueueState::Updating &&
                                  state != MergeQueueState::Merging
                              ? state
                              : MergeQueueState::Queued;
        }
        if (parts.size() > 3)
            entry.detail = sanitizeDetail(parts.mid(3).join(QLatin1Char(' ')));
        entry.stateAtMs = entry.queuedAtMs;
        queue.m_entries.append(entry);
    }
    return queue;
}

QStringList MergeQueue::toRows() const
{
    QStringList rows;
    rows.reserve(m_entries.size());
    for (const MergeQueueEntry &entry : m_entries) {
        rows << QString::number(entry.number) + QLatin1Char('\t') +
                    QString::number(entry.queuedAtMs) + QLatin1Char('\t') +
                    entry.state + QLatin1Char('\t') + sanitizeDetail(entry.detail);
    }
    return rows;
}

int MergeQueue::indexOf(int number) const
{
    for (int i = 0; i < m_entries.size(); ++i)
        if (m_entries.at(i).number == number)
            return i;
    return -1;
}

QList<int> MergeQueue::numbers() const
{
    QList<int> out;
    out.reserve(m_entries.size());
    for (const MergeQueueEntry &entry : m_entries)
        out << entry.number;
    return out;
}

bool MergeQueue::enqueue(int number, qint64 nowMs)
{
    if (number <= 0 || contains(number))
        return false;
    MergeQueueEntry entry;
    entry.number = number;
    entry.queuedAtMs = nowMs;
    entry.stateAtMs = nowMs;
    m_entries.append(entry);
    return true;
}

bool MergeQueue::remove(int number)
{
    const int index = indexOf(number);
    if (index < 0)
        return false;
    m_entries.removeAt(index);
    return true;
}

bool MergeQueue::move(int number, int delta)
{
    const int index = indexOf(number);
    if (index < 0 || delta == 0)
        return false;
    int target = index + delta;
    if (target < 0)
        target = 0;
    if (target > m_entries.size() - 1)
        target = m_entries.size() - 1;
    if (target == index)
        return false;
    m_entries.move(index, target);
    return true;
}

bool MergeQueue::setState(int number, const QString &state, const QString &detail,
                          qint64 nowMs)
{
    const int index = indexOf(number);
    if (index < 0)
        return false;
    m_entries[index].state = knownState(state) ? state : MergeQueueState::Queued;
    m_entries[index].detail = sanitizeDetail(detail);
    m_entries[index].stateAtMs = nowMs;
    return true;
}

MergeQueueEntry MergeQueue::front() const
{
    return m_entries.isEmpty() ? MergeQueueEntry() : m_entries.first();
}

MergeQueueEntry MergeQueue::at(int number) const
{
    const int index = indexOf(number);
    return index < 0 ? MergeQueueEntry() : m_entries.at(index);
}

QString MergeQueue::summary() const
{
    if (m_entries.isEmpty())
        return QStringLiteral("Empty");
    int blocked = 0;
    int running = 0;
    for (const MergeQueueEntry &entry : m_entries) {
        if (entry.state == MergeQueueState::Blocked ||
            entry.state == MergeQueueState::Failed)
            ++blocked;
        else if (entry.state == MergeQueueState::Updating ||
                 entry.state == MergeQueueState::Merging)
            ++running;
    }
    QString text = QStringLiteral("%1 queued").arg(m_entries.size());
    if (running > 0)
        text += QString::fromUtf8(" \xC2\xB7 %1 in progress").arg(running);
    if (blocked > 0)
        text += QString::fromUtf8(" \xC2\xB7 %1 blocked").arg(blocked);
    return text;
}

QString MergeQueue::sanitizeDetail(const QString &detail)
{
    QString flat = detail.simplified();
    flat.replace(QLatin1Char('\t'), QLatin1Char(' '));
    if (flat.size() > kMaxDetailChars)
        flat = flat.left(kMaxDetailChars - 1) + QString::fromUtf8("\xE2\x80\xA6");
    return flat;
}
