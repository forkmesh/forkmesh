#pragma once

#include <QString>
#include <QtGlobal>

#include <deque>

namespace forkmesh::localcontrol {

struct LogTail {
    qint64 bytes = 0;
    qint64 chars = 0;
};

QString readLogGrowth(const QString &path, LogTail &tail, bool *rewritten = nullptr);

class SendBudget
{
public:
    void configure(qint64 windowMs, int maximum);
    qint64 waitMs(qint64 nowMs);
    void spend(qint64 nowMs);
    void reset();
    int limit() const { return m_limit; }
    qint64 windowMs() const { return m_windowMs; }

private:
    qint64 m_windowMs = 10000;
    int m_limit = 180;
    std::deque<qint64> m_sent;
};

inline constexpr int kDefaultRelayControlWindowMs = 10000;
inline constexpr int kDefaultRelayControlMax = 200;

int followIntervalMs(bool relayed);

} // namespace forkmesh::localcontrol
