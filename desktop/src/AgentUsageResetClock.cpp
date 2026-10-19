#include "AgentUsageResetClock.h"

#include <QLocale>
#include <QStringList>
#include <QTimeZone>

namespace forkmesh::usageclock {

QString resetClockLabel(qint64 resetAtMs, const QDateTime &now)
{
    if (resetAtMs <= 0 || resetAtMs <= now.toMSecsSinceEpoch())
        return QString();
    const QDateTime reset = QDateTime::fromMSecsSinceEpoch(resetAtMs, now.timeZone());
    const QLocale english(QLocale::English, QLocale::UnitedStates);
    const QString clock = english.toString(reset.time(), QStringLiteral("h:mm AP"));
    const qint64 days = now.date().daysTo(reset.date());
    if (days == 0)
        return QStringLiteral("Resets %1").arg(clock);
    if (days < 7)
        return QStringLiteral("Resets %1 %2")
            .arg(english.toString(reset.date(), QStringLiteral("ddd")), clock);
    return QStringLiteral("Resets %1, %2")
        .arg(english.toString(reset.date(), QStringLiteral("MMM d")), clock);
}

QString noteWithResetClock(const QString &note, qint64 resetAtMs,
                           const QDateTime &now)
{
    const QString clock = resetClockLabel(resetAtMs, now);
    if (clock.isEmpty())
        return note;
    const QString separator = QString::fromUtf8(" \xC2\xB7 ");
    QStringList parts = note.isEmpty() ? QStringList() : note.split(separator);
    const QString relative = QStringLiteral("resets in ");
    for (QString &part : parts) {
        if (part.startsWith(relative)) {
            part = QStringLiteral("%1 (in %2)").arg(clock, part.mid(relative.size()));
            return parts.join(separator);
        }
    }
    parts.append(clock);
    return parts.join(separator);
}

}
