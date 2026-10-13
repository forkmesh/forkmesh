#pragma once

#include <QHash>
#include <QString>
#include <QtGlobal>

class DiscussionInboxBackoff {
public:
    static constexpr qint64 CooldownMs = 10LL * 60 * 1000;

    bool shouldBackOff(const QString &key, qint64 nowMs) const
    {
        return m_unsupportedUntilMs.value(key, 0) > nowMs;
    }

    void markUnsupported(const QString &key, qint64 nowMs)
    {
        if (key.isEmpty())
            return;
        m_unsupportedUntilMs.insert(key, nowMs + CooldownMs);
    }

    void clear(const QString &key)
    {
        m_unsupportedUntilMs.remove(key);
    }

private:
    QHash<QString, qint64> m_unsupportedUntilMs;
};
