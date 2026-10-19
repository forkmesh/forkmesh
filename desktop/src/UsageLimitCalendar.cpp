#include "UsageLimitCalendar.h"
#include "AppPaths.h"

#include <QDateTime>
#include <QDir>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimeZone>

namespace {

QString safeKey(QString value)
{
    value = value.toLower();
    for (QChar &c : value) {
        if (!c.isLetterOrNumber())
            c = QLatin1Char('-');
    }
    return value;
}

QString escapeIcalendar(QString value)
{
    value.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    value.replace(QLatin1Char(';'), QStringLiteral("\\;"));
    value.replace(QLatin1Char(','), QStringLiteral("\\,"));
    value.replace(QStringLiteral("\r\n"), QStringLiteral("\\n"));
    value.replace(QLatin1Char('\n'), QStringLiteral("\\n"));
    value.replace(QLatin1Char('\r'), QStringLiteral("\\n"));
    return value;
}

QString utcIcalendarTime(qint64 ms)
{
    const QDateTime time =
        QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::utc());
    return time.isValid()
               ? time.toString(QStringLiteral("yyyyMMdd'T'HHmmss'Z'"))
               : QString();
}

QString reminderDirectory()
{
    if (forkmesh::paths::dataRoot().isEmpty())
        return QDir(QDir::tempPath()).filePath(QStringLiteral("usage-reminders"));
    return forkmesh::paths::dataPath(QStringLiteral("usage-reminders"));
}

} // namespace

namespace UsageLimitCalendar {

QString eventText(const QString &providerKey, const QString &windowKey,
                  const QString &providerName, const QString &windowName,
                  qint64 resetMs, qint64 createdMs)
{
    if (resetMs <= 0)
        return QString();
    const QString startsAt = utcIcalendarTime(resetMs);
    if (startsAt.isEmpty())
        return QString();
    if (createdMs <= 0)
        createdMs = QDateTime::currentMSecsSinceEpoch();
    const QString createdAt = utcIcalendarTime(createdMs);
    if (createdAt.isEmpty())
        return QString();

    const QString provider = safeKey(providerKey);
    const QString window = safeKey(windowKey);
    const QString summary = QStringLiteral("ForkMesh: %1 usage is ready")
                                .arg(providerName);
    const QString description =
        QStringLiteral("Your %1 %2 usage window has reset. You can resume "
                       "agent work.")
            .arg(providerName, windowName);
    const QString uid = QStringLiteral("forkmesh-usage-%1-%2-%3@local")
                            .arg(provider, window, QString::number(resetMs));
    const QString endsAt = utcIcalendarTime(resetMs + 5 * 60 * 1000);

    return QStringLiteral(
               "BEGIN:VCALENDAR\r\n"
               "VERSION:2.0\r\n"
               "PRODID:-//ForkMesh//Agent usage reminder//EN\r\n"
               "CALSCALE:GREGORIAN\r\n"
               "METHOD:PUBLISH\r\n"
               "BEGIN:VEVENT\r\n"
               "UID:%1\r\n"
               "DTSTAMP:%2\r\n"
               "DTSTART:%3\r\n"
               "DTEND:%4\r\n"
               "SUMMARY:%5\r\n"
               "DESCRIPTION:%6\r\n"
               "TRANSP:TRANSPARENT\r\n"
               "BEGIN:VALARM\r\n"
               "ACTION:DISPLAY\r\n"
               "TRIGGER:PT0M\r\n"
               "DESCRIPTION:%5\r\n"
               "END:VALARM\r\n"
               "END:VEVENT\r\n"
               "END:VCALENDAR\r\n")
        .arg(uid, createdAt, startsAt, endsAt, escapeIcalendar(summary),
             escapeIcalendar(description));
}

QString writeEvent(const QString &providerKey, const QString &windowKey,
                   const QString &providerName, const QString &windowName,
                   qint64 resetMs)
{
    const QString content = eventText(providerKey, windowKey, providerName,
                                      windowName, resetMs);
    if (content.isEmpty())
        return QString();
    const QString directory = reminderDirectory();
    if (!QDir().mkpath(directory))
        return QString();
    const QString path = QDir(directory).filePath(
        QStringLiteral("%1-%2.ics").arg(safeKey(providerKey),
                                         safeKey(windowKey)));
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(content.toUtf8()) != content.toUtf8().size() ||
        !file.commit()) {
        return QString();
    }
    return path;
}

} // namespace UsageLimitCalendar
