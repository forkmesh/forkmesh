#pragma once

#include <QLatin1String>
#include <QRegularExpression>
#include <QString>

namespace ChatVisitorPresence {

constexpr qint64 kVisitorIdleMs = 600000; // 10 minutes

inline bool isTransientVisitor(const QString &accountKind, const QString &name)
{
    if (accountKind.trimmed().compare(QLatin1String("guest"),
                                      Qt::CaseInsensitive) == 0)
        return true;
    static const QRegularExpression visitorName(
        QStringLiteral("^\\s*(guest\\b|world\\s+guest\\b|world\\s+visitor\\b)"),
        QRegularExpression::CaseInsensitiveOption);
    return visitorName.match(name).hasMatch();
}

inline bool visitorIsIdle(qint64 lastSeenMs, qint64 nowMs)
{
    return lastSeenMs > 0 && nowMs - lastSeenMs > kVisitorIdleMs;
}

} // namespace ChatVisitorPresence
