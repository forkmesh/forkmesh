#pragma once

#include <QDate>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QPair>
#include <QString>
#include <QTime>

namespace forkmesh::ui {

extern const QString kRepositoryCommitsCalendarId;
extern const QString kRepositoryMergesCalendarId;

QJsonArray repositoryCalendarEvents(const QString &gitPath,
                                    const QString &repository,
                                    qint64 startAtMs,
                                    qint64 endAtMs);

// One repository's commits for one calendar month. Reads are cached and
// replayed per month so paging the Calendar only walks the months the view
// gained, and changing view or opening a day walks nothing at all.
struct RepositoryCalendarScan {
    QString gitPath;
    QString label;
    QString key;
    qint64 startAtMs = 0;
    qint64 endAtMs = 0;
};

// Whole months covering [first, last), oldest first.
QList<QPair<qint64, qint64>> repositoryCalendarMonths(const QDate &first,
                                                      const QDate &last);

// A projected event with its local calendar day and time resolved once.
// Converting a UTC stamp to local time is not cheap, and the views used to do
// it per event per rendered cell; with every commit on the calendar that was
// the bulk of a redraw.
struct CalendarOccurrence {
    QJsonObject event;
    qint64 startAtMs = 0;
    qint64 endAtMs = 0;
    QDate date;
    QTime time;
};

} // namespace forkmesh::ui
