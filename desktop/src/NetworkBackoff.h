#pragma once

#include <QHash>
#include <QRandomGenerator>
#include <QString>
#include <QtGlobal>

class NetworkBackoff {
public:
    static constexpr qint64 kDefaultBaseMs = 60LL * 1000;      // one poll cycle
    static constexpr qint64 kDefaultCapMs = 10LL * 60 * 1000;  // 10 minutes

    bool ready(const QString &channel, qint64 nowMs) const
    {
        return m_channels.value(channel).nextAllowedMs <= nowMs;
    }

    qint64 msUntilReady(const QString &channel, qint64 nowMs) const
    {
        return qMax<qint64>(0, m_channels.value(channel).nextAllowedMs - nowMs);
    }

    void noteSuccess(const QString &channel)
    {
        if (!channel.isEmpty())
            m_channels.remove(channel);
    }

    int lastFailureStatus(const QString &channel) const
    {
        return m_channels.value(channel).lastStatus;
    }

    void noteFailure(const QString &channel, qint64 nowMs, qint64 baseMs,
                     qint64 capMs, qint64 retryAfterMs = 0, int status = 0)
    {
        if (channel.isEmpty())
            return;
        State &s = m_channels[channel];
        s.lastStatus = status;
        if (s.failures < 30) // guard the shift below against overflow
            ++s.failures;
        qint64 delay = baseMs;
        for (int i = 1; i < s.failures && delay < capMs; ++i)
            delay <<= 1;
        delay = qMin(delay, capMs);
        if (retryAfterMs > 0)
            delay = qMin(qMax(delay, retryAfterMs), capMs);
        const quint32 span = static_cast<quint32>(qMax<qint64>(1, delay / 8));
        s.nextAllowedMs =
            nowMs + delay + QRandomGenerator::global()->bounded(span);
    }

    void noteFailure(const QString &channel, qint64 nowMs)
    {
        noteFailure(channel, nowMs, kDefaultBaseMs, kDefaultCapMs);
    }

private:
    struct State {
        int failures = 0;
        qint64 nextAllowedMs = 0;
        int lastStatus = 0;
    };
    QHash<QString, State> m_channels;
};
