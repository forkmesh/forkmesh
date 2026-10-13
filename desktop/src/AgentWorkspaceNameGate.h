#pragma once

#include <QJsonObject>
#include <QtGlobal>

namespace forkmesh::agents {

class WorkspaceNameGate {
public:
    static constexpr int kMaxInFlight = 2;
    static constexpr qint64 kFailureCooldownMs = 60LL * 1000;
    static constexpr qint64 kMaxCooldownMs = 60LL * 60 * 1000;

    bool tryBegin(qint64 nowMs)
    {
        if (nowMs < m_blockedUntilMs || m_inFlight >= kMaxInFlight)
            return false;
        ++m_inFlight;
        return true;
    }

    void finish(qint64 nowMs, int httpStatus, bool answered,
                const QJsonObject &payload, qint64 retryAfterHeaderMs)
    {
        m_inFlight = qMax(0, m_inFlight - 1);
        qint64 cooldown = 0;
        if (httpStatus == 429) {
            cooldown = static_cast<qint64>(
                payload.value(QStringLiteral("retryAfterMs")).toDouble());
            if (cooldown <= 0)
                cooldown = retryAfterHeaderMs;
            if (cooldown <= 0)
                cooldown = kFailureCooldownMs;
        } else if (!answered) {
            cooldown = kFailureCooldownMs;
        }
        if (cooldown > 0)
            m_blockedUntilMs = qMax(m_blockedUntilMs,
                                    nowMs + qMin(cooldown, kMaxCooldownMs));
    }

    int inFlight() const { return m_inFlight; }
    qint64 blockedUntilMs() const { return m_blockedUntilMs; }

private:
    int m_inFlight = 0;
    qint64 m_blockedUntilMs = 0;
};

}
