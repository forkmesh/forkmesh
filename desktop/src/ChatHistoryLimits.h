#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include <utility>

// Bounds for the in-RAM chat history a node keeps per channel.
// A long-running mainnode can run for months; without a cap every chat frame it
// ever saw stayed in ServerNode::m_channelHistory forever — including file
// messages, whose base64 payload alone can be tens of MB of QString chars.
// History only exists so a newcomer gets recent context replayed (and that
// replay goes out as a single WebSocket frame, so an unbounded backlog could
// not even be delivered), which recent-N with a byte budget preserves.
namespace ChatHistoryLimits {

constexpr int kMaxEntriesPerChannel = 200;
constexpr qsizetype kMaxCharsPerChannel = 2 * 1024 * 1024;

inline qsizetype entryCost(const QJsonObject &entry)
{
    return entry.value(QLatin1String("file")).toString().size() +
           entry.value(QLatin1String("text")).toString().size() + 64;
}

inline QStringList appendBounded(QList<QJsonObject> &history, QJsonObject message,
                                 qsizetype *runningTotal = nullptr)
{
    if (entryCost(message) > kMaxCharsPerChannel)
        message.remove(QLatin1String("file"));
    history.append(message);
    qsizetype totalChars = 0;
    if (runningTotal) {
        totalChars = *runningTotal + entryCost(message);
    } else {
        for (const QJsonObject &entry : std::as_const(history))
            totalChars += entryCost(entry);
    }
    QStringList evicted;
    while (history.size() > 1 && (history.size() > kMaxEntriesPerChannel ||
                                  totalChars > kMaxCharsPerChannel)) {
        totalChars -= entryCost(history.first());
        const QString id = history.first().value(QLatin1String("id")).toString();
        if (!id.isEmpty())
            evicted.append(id);
        history.removeFirst();
    }
    if (runningTotal)
        *runningTotal = totalChars;
    return evicted;
}

} // namespace ChatHistoryLimits
