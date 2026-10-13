#include "RepositoryCalendar.h"

#include "MainWindowInternal.h"

namespace forkmesh::ui {

const QString kRepositoryCommitsCalendarId =
    QStringLiteral("forkmesh:commits");
const QString kRepositoryMergesCalendarId =
    QStringLiteral("forkmesh:merges");

QList<QPair<qint64, qint64>> repositoryCalendarMonths(const QDate &first,
                                                      const QDate &last)
{
    QList<QPair<qint64, qint64>> months;
    if (!first.isValid() || !last.isValid() || last <= first)
        return months;
    QDate cursor(first.year(), first.month(), 1);
    // A month view spans three months at most and the agenda four, so the
    // bound only guards against a nonsense range.
    while (cursor < last && months.size() < 24) {
        const QDate next = cursor.addMonths(1);
        months.append({QDateTime(cursor, QTime(0, 0)).toMSecsSinceEpoch(),
                       QDateTime(next, QTime(0, 0)).toMSecsSinceEpoch()});
        cursor = next;
    }
    return months;
}

// Project every unique commit reachable from a branch or tag into the Calendar
// page's event shape. Operational refs and stashes are deliberately not user
// activity. The visible time range keeps the Git read bounded without silently
// dropping a busy day's later commits.
QJsonArray repositoryCalendarEvents(const QString &gitPath,
                                    const QString &repository,
                                    qint64 startAtMs,
                                    qint64 endAtMs)
{
    QJsonArray events;
    if (gitPath.trimmed().isEmpty() || !QDir(gitPath).exists() ||
        startAtMs < 0 || endAtMs <= startAtMs)
        return events;

    const qint64 startSecs = startAtMs / 1000;
    const qint64 endSecs = (endAtMs - 1) / 1000;
    QByteArray output;
    if (!runGitCapture(
            gitPath,
            {QStringLiteral("log"), QStringLiteral("--branches"),
             QStringLiteral("--tags"), QStringLiteral("--remotes"),
             QStringLiteral("HEAD"),
             QStringLiteral("--since=@%1").arg(startSecs),
             QStringLiteral("--until=@%1").arg(endSecs),
             QStringLiteral("--format=%H%x1f%P%x1f%ct%x1f%an%x1f%s%x1e"),
             QStringLiteral("--")},
            &output, nullptr))
        return events;

    QString label = repository.trimmed();
    if (label.isEmpty()) {
        label = QFileInfo(gitPath).fileName();
        if (label.endsWith(QLatin1String(".git")))
            label.chop(4);
    }
    static const QRegularExpression commitHash(
        QStringLiteral("^(?:[0-9a-f]{40}|[0-9a-f]{64})$"));
    for (QByteArray record : output.split('\x1e')) {
        record = record.trimmed();
        if (record.isEmpty())
            continue;
        const QList<QByteArray> fields = record.split('\x1f');
        if (fields.size() != 5)
            continue;
        const QString hash = QString::fromLatin1(fields.at(0)).trimmed().toLower();
        bool timestampOk = false;
        const qint64 committedAtMs =
            QString::fromLatin1(fields.at(2)).trimmed().toLongLong(&timestampOk) *
            1000;
        if (!commitHash.match(hash).hasMatch() || !timestampOk ||
            committedAtMs < startAtMs || committedAtMs >= endAtMs)
            continue;
        const QString parents = QString::fromLatin1(fields.at(1)).trimmed();
        const bool merge =
            parents.split(QLatin1Char(' '), Qt::SkipEmptyParts).size() > 1;
        const QString author = QString::fromUtf8(fields.at(3)).trimmed();
        QString subject = QString::fromUtf8(fields.at(4)).trimmed();
        if (subject.isEmpty())
            subject = QStringLiteral("Commit %1").arg(hash.left(12));
        const QString kind = merge ? QStringLiteral("merge")
                                   : QStringLiteral("commit");

        events.append(QJsonObject{
            {QStringLiteral("id"),
             QStringLiteral("git:%1:%2").arg(label, hash)},
            {QStringLiteral("source"), QStringLiteral("repository")},
            {QStringLiteral("kind"), kind},
            {QStringLiteral("readOnly"), true},
            {QStringLiteral("calendar"),
             merge ? kRepositoryMergesCalendarId
                   : kRepositoryCommitsCalendarId},
            {QStringLiteral("scope"), QStringLiteral("repository")},
            {QStringLiteral("repository"), label},
            {QStringLiteral("hash"), hash},
            {QStringLiteral("parents"), parents},
            {QStringLiteral("owner"), author},
            {QStringLiteral("startAt"), double(committedAtMs)},
            {QStringLiteral("endAt"), double(committedAtMs + 60 * 1000)},
            {QStringLiteral("allDay"), false},
            {QStringLiteral("timezone"),
             QString::fromUtf8(QTimeZone::systemTimeZoneId())},
            {QStringLiteral("title"), subject},
            {QStringLiteral("description"),
             QStringLiteral("%1 %2\n%3 · %4")
                 .arg(merge ? QStringLiteral("Merge")
                            : QStringLiteral("Commit"),
                      hash, author, label)},
            {QStringLiteral("location"), label},
            {QStringLiteral("reminders"), QJsonArray{}},
            {QStringLiteral("recurrence"), QStringLiteral("none")},
            {QStringLiteral("recurrenceUntil"), 0},
            {QStringLiteral("attendees"), QJsonArray{}},
        });
    }
    return events;
}

} // namespace forkmesh::ui
